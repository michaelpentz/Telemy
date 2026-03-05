package relay

import (
	"crypto/rand"
	"context"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"log"
	"strings"
	"time"

	"github.com/aws/aws-sdk-go-v2/aws"
	awscfg "github.com/aws/aws-sdk-go-v2/config"
	"github.com/aws/aws-sdk-go-v2/service/ec2"
	ec2types "github.com/aws/aws-sdk-go-v2/service/ec2/types"
	"github.com/aws/smithy-go"
	"github.com/telemyapp/aegis-control-plane/internal/metrics"
)

// relayUserDataScript is the EC2 user-data bootstrap for OpenIRL srtla-receiver.
// Canonical copy lives at scripts/relay-user-data.sh.
const relayUserDataScript = `#!/bin/bash
set -euo pipefail
exec > /var/log/srtla-setup.log 2>&1
echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup starting"

dnf update -y
dnf install -y docker jq
systemctl enable docker
systemctl start docker

DOCKER_COMPOSE_VERSION="v2.27.0"
ARCH=$(uname -m)
mkdir -p /usr/local/lib/docker/cli-plugins
curl -SL "https://github.com/docker/compose/releases/download/${DOCKER_COMPOSE_VERSION}/docker-compose-linux-${ARCH}" \
  -o /usr/local/lib/docker/cli-plugins/docker-compose
chmod +x /usr/local/lib/docker/cli-plugins/docker-compose

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') Docker installed, setting up srtla-receiver"

mkdir -p /opt/srtla-receiver/data
cd /opt/srtla-receiver

curl -sL https://raw.githubusercontent.com/OpenIRL/srtla-receiver/main/docker-compose.prod.yml \
  -o docker-compose.yml

cat > .env << 'ENVEOF'
SRTLA_PORT=5000
SRT_SENDER_PORT=4001
SRT_PLAYER_PORT=4000
SLS_MGNT_PORT=3000
SLS_STATS_PORT=8090
APP_URL=http://localhost
ENVEOF

chown -R 3001:3001 /opt/srtla-receiver/data

docker compose up -d

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver containers started"

# Wait for containers to become ready
deadline=$((SECONDS + 120))
while true; do
  all_ready=true
  has_containers=false
  while IFS= read -r cid; do
    [ -z "${cid}" ] && continue
    has_containers=true
    health=$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "${cid}")
    status=$(docker inspect --format '{{.State.Status}}' "${cid}")
    if [ "${health}" = "none" ]; then
      [ "${status}" != "running" ] && all_ready=false && break
    elif [ "${health}" != "healthy" ]; then
      all_ready=false && break
    fi
  done < <(docker compose ps -q)
  [ "${has_containers}" = true ] && [ "${all_ready}" = true ] && break
  if [ ${SECONDS} -ge ${deadline} ]; then
    echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') timeout waiting for containers"
    docker compose ps
    exit 1
  fi
  sleep 3
done

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') containers ready, extracting API key"

# Extract API key (try .apikey file first, then container logs as fallback)
APIKEY=""
for attempt in $(seq 1 30); do
  # Method 1: .apikey file written by srtla-receiver container
  if [ -f /opt/srtla-receiver/data/.apikey ]; then
    APIKEY=$(cat /opt/srtla-receiver/data/.apikey)
    [ -n "${APIKEY}" ] && break
  fi
  # Method 2: grep from container logs (|| true prevents pipefail exit)
  APIKEY=$(docker compose logs receiver 2>/dev/null \
    | grep "\[CSLSDatabase\] Generated default admin API key:" \
    | sed 's/.*Generated default admin API key: \([A-Za-z0-9]*\).*/\1/' \
    | tail -1 || true)
  [ -n "${APIKEY}" ] && break
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') waiting for API key (attempt ${attempt})"
  sleep 3
done

if [ -z "${APIKEY}" ]; then
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') WARNING: API key not found, stream auto-creation skipped"
else
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') API key found, creating default stream"
  STREAM_RESP=$(curl -s -X POST http://localhost:8090/api/stream-ids \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer ${APIKEY}" \
    -d '{"publisher":"live_aegis","player":"play_aegis","description":"aegis-relay"}')
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') stream create: ${STREAM_RESP}"

  STREAMS=$(curl -s http://localhost:8090/api/stream-ids \
    -H "Authorization: Bearer ${APIKEY}")
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') streams: ${STREAMS}"

  # Write stream keys to well-known files for health-check polling
  echo "${STREAMS}" > /tmp/srtla-streams.json
  echo "${APIKEY}" > /tmp/srtla-apikey
fi

touch /tmp/srtla-ready
echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup complete"
`

type AWSProvisioner struct {
	amiByRegion   map[string]string
	instanceType  string
	subnetID      string
	securityGroup []string
	keyName       string
}

type AWSProvisionerOptions struct {
	AMIByRegion   map[string]string
	InstanceType  string
	SubnetID      string
	SecurityGroup []string
	KeyName       string
}

func NewAWSProvisioner(opts AWSProvisionerOptions) (*AWSProvisioner, error) {
	if len(opts.AMIByRegion) == 0 {
		return nil, fmt.Errorf("AMIByRegion is required")
	}
	instanceType := strings.TrimSpace(opts.InstanceType)
	if instanceType == "" {
		instanceType = "t4g.small"
	}
	return &AWSProvisioner{
		amiByRegion:   opts.AMIByRegion,
		instanceType:  instanceType,
		subnetID:      strings.TrimSpace(opts.SubnetID),
		securityGroup: opts.SecurityGroup,
		keyName:       strings.TrimSpace(opts.KeyName),
	}, nil
}

func (p *AWSProvisioner) Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error) {
	amiID, ok := p.amiByRegion[req.Region]
	if !ok || strings.TrimSpace(amiID) == "" {
		return ProvisionResult{}, fmt.Errorf("no AMI configured for region %s", req.Region)
	}

	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(req.Region))
	if err != nil {
		return ProvisionResult{}, fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)

	userData := base64.StdEncoding.EncodeToString([]byte(relayUserDataScript))
	runInput := &ec2.RunInstancesInput{
		ImageId:      aws.String(amiID),
		InstanceType: ec2types.InstanceType(p.instanceType),
		MinCount:     aws.Int32(1),
		MaxCount:     aws.Int32(1),
		UserData:     aws.String(userData),
		TagSpecifications: []ec2types.TagSpecification{
			{
				ResourceType: ec2types.ResourceTypeInstance,
				Tags: []ec2types.Tag{
					{Key: aws.String("Name"), Value: aws.String("aegis-relay-" + req.SessionID)},
					{Key: aws.String("ManagedBy"), Value: aws.String("aegis-control-plane")},
					{Key: aws.String("AegisSessionID"), Value: aws.String(req.SessionID)},
					{Key: aws.String("AegisUserID"), Value: aws.String(req.UserID)},
				},
			},
		},
	}
	if p.keyName != "" {
		runInput.KeyName = aws.String(p.keyName)
	}

	if p.subnetID != "" {
		eni := ec2types.InstanceNetworkInterfaceSpecification{
			DeviceIndex:              aws.Int32(0),
			AssociatePublicIpAddress: aws.Bool(true),
			SubnetId:                 aws.String(p.subnetID),
		}
		if len(p.securityGroup) > 0 {
			eni.Groups = p.securityGroup
		}
		runInput.NetworkInterfaces = []ec2types.InstanceNetworkInterfaceSpecification{eni}
	} else if len(p.securityGroup) > 0 {
		runInput.SecurityGroupIds = p.securityGroup
	}

	var runOut *ec2.RunInstancesOutput
	runStart := time.Now()
	err = retryAWS(ctx, "run_instances", req.Region, func(callCtx context.Context) error {
		var runErr error
		runOut, runErr = client.RunInstances(callCtx, runInput)
		return runErr
	})
	log.Printf("metric=aws_run_instances_latency_ms region=%s session_id=%s value=%d", req.Region, req.SessionID, time.Since(runStart).Milliseconds())
	runDurMS := float64(time.Since(runStart).Milliseconds())
	if err != nil {
		labels := map[string]string{"op": "run_instances", "region": req.Region, "status": "error"}
		metrics.Default().IncCounter("aegis_aws_operations_total", labels)
		metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", runDurMS, labels)
		return ProvisionResult{}, fmt.Errorf("run instances: %w", err)
	}
	labels := map[string]string{"op": "run_instances", "region": req.Region, "status": "ok"}
	metrics.Default().IncCounter("aegis_aws_operations_total", labels)
	metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", runDurMS, labels)
	if len(runOut.Instances) == 0 || runOut.Instances[0].InstanceId == nil {
		return ProvisionResult{}, fmt.Errorf("run instances: no instance returned")
	}
	instanceID := aws.ToString(runOut.Instances[0].InstanceId)

	waitCtx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	waiter := ec2.NewInstanceRunningWaiter(client)
	if err := waiter.Wait(waitCtx, &ec2.DescribeInstancesInput{InstanceIds: []string{instanceID}}, 2*time.Minute); err != nil {
		return ProvisionResult{}, fmt.Errorf("wait running: %w", err)
	}

	descOut, err := client.DescribeInstances(ctx, &ec2.DescribeInstancesInput{InstanceIds: []string{instanceID}})
	if err != nil {
		return ProvisionResult{}, fmt.Errorf("describe instances: %w", err)
	}

	publicIP := extractPublicIP(descOut)
	if publicIP == "" {
		return ProvisionResult{}, fmt.Errorf("instance %s has no public ip", instanceID)
	}

	return ProvisionResult{
		AWSInstanceID: instanceID,
		AMIID:         amiID,
		InstanceType:  p.instanceType,
		PublicIP:      publicIP,
		SRTPort:       5000,
		WSURL:         fmt.Sprintf("wss://%s:7443/telemetry", publicIP),
	}, nil
}

func (p *AWSProvisioner) Deprovision(ctx context.Context, req DeprovisionRequest) error {
	if strings.TrimSpace(req.AWSInstanceID) == "" {
		return nil
	}
	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(req.Region))
	if err != nil {
		return fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)
	termStart := time.Now()
	err = retryAWS(ctx, "terminate_instances", req.Region, func(callCtx context.Context) error {
		_, termErr := client.TerminateInstances(callCtx, &ec2.TerminateInstancesInput{
			InstanceIds: []string{req.AWSInstanceID},
		})
		return termErr
	})
	log.Printf("metric=aws_terminate_instances_latency_ms region=%s session_id=%s instance_id=%s value=%d", req.Region, req.SessionID, req.AWSInstanceID, time.Since(termStart).Milliseconds())
	termDurMS := float64(time.Since(termStart).Milliseconds())
	if err != nil {
		if shouldIgnoreTerminateError(err) {
			labels := map[string]string{"op": "terminate_instances", "region": req.Region, "status": "ignored"}
			metrics.Default().IncCounter("aegis_aws_operations_total", labels)
			metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", termDurMS, labels)
			return nil
		}
		labels := map[string]string{"op": "terminate_instances", "region": req.Region, "status": "error"}
		metrics.Default().IncCounter("aegis_aws_operations_total", labels)
		metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", termDurMS, labels)
		return fmt.Errorf("terminate instance: %w", err)
	}
	labels := map[string]string{"op": "terminate_instances", "region": req.Region, "status": "ok"}
	metrics.Default().IncCounter("aegis_aws_operations_total", labels)
	metrics.Default().ObserveHistogram("aegis_aws_operation_latency_ms", termDurMS, labels)
	return nil
}

func shouldIgnoreTerminateError(err error) bool {
	var apiErr smithy.APIError
	if !errors.As(err, &apiErr) {
		return false
	}
	code := apiErr.ErrorCode()
	return code == "InvalidInstanceID.NotFound" || code == "IncorrectInstanceState"
}

func retryAWS(ctx context.Context, opName, region string, fn func(context.Context) error) error {
	const (
		maxAttempts = 4
		baseDelay   = 250 * time.Millisecond
		maxDelay    = 2 * time.Second
	)
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		err := fn(ctx)
		if err == nil {
			return nil
		}
		lastErr = err
		if !isTransientAWSError(err) {
			return err
		}
		if attempt == maxAttempts {
			metrics.Default().IncCounter("aegis_aws_retry_exhausted_total", map[string]string{
				"op":     opName,
				"region": region,
			})
			return err
		}
		reason := awsErrorCode(err)
		metrics.Default().IncCounter("aegis_aws_retries_total", map[string]string{
			"op":     opName,
			"region": region,
			"reason": reason,
		})
		delay := baseDelay * time.Duration(1<<(attempt-1))
		if delay > maxDelay {
			delay = maxDelay
		}
		delay = withJitter(delay)
		log.Printf("event=aws_retry op=%s region=%s attempt=%d delay_ms=%d err=%q", opName, region, attempt, delay.Milliseconds(), err.Error())
		timer := time.NewTimer(delay)
		select {
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		case <-timer.C:
		}
	}
	return lastErr
}

func withJitter(delay time.Duration) time.Duration {
	if delay <= 0 {
		return 0
	}
	floor := delay / 10
	span := delay - floor
	if span <= 0 {
		return floor
	}
	var raw [8]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return floor + (span / 2)
	}
	max := uint64(span)
	if max == 0 {
		return floor + (span / 2)
	}
	n := binary.LittleEndian.Uint64(raw[:]) % max
	// Jittered delay in [10% of base, 100% of base).
	return floor + time.Duration(n)
}

func isTransientAWSError(err error) bool {
	var apiErr smithy.APIError
	if !errors.As(err, &apiErr) {
		return false
	}
	switch apiErr.ErrorCode() {
	case "RequestLimitExceeded",
		"Throttling",
		"ThrottlingException",
		"RequestThrottled",
		"ServiceUnavailable",
		"InternalError",
		"RequestTimeout",
		"EC2ThrottledException",
		"InsufficientInstanceCapacity":
		return true
	default:
		return false
	}
}

func awsErrorCode(err error) string {
	var apiErr smithy.APIError
	if !errors.As(err, &apiErr) {
		return "non_api_error"
	}
	code := strings.TrimSpace(apiErr.ErrorCode())
	if code == "" {
		return "unknown"
	}
	return code
}

func extractPublicIP(out *ec2.DescribeInstancesOutput) string {
	for _, res := range out.Reservations {
		for _, inst := range res.Instances {
			if inst.PublicIpAddress != nil && strings.TrimSpace(*inst.PublicIpAddress) != "" {
				return *inst.PublicIpAddress
			}
		}
	}
	return ""
}
