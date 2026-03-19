package relay

import (
	"context"
	"crypto/rand"
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
# Aegis relay bootstrap for pre-baked AMI (aegis-relay-v1)
# AMI includes: Docker, Docker Compose, pre-pulled container images
# This script only writes config, starts containers, and creates the stream.
#
# Port map:
#   UDP 5000  SRTLA bonded ingest (IRL Pro connects here)
#   UDP 4000  SRT player output (OBS connects here)
#   UDP 4001  SRT direct sender (non-bonded fallback)
#   TCP 3000  Management UI
#   TCP 5080  Per-link stats API (srtla_rec HTTP)
#   TCP 8090  Backend API (remapped from default 8080 to avoid control plane conflict)

set -euo pipefail
exec > /var/log/srtla-setup.log 2>&1
echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup starting (pre-baked AMI)"

# Forward cloud-init output for relay debugging
ln -sf /var/log/cloud-init-output.log /opt/srtla-receiver/cloud-init.log 2>/dev/null || true

# Docker + Compose already installed in AMI; just ensure Docker is running
systemctl start docker

cd /opt/srtla-receiver

# Write docker-compose with custom srtla-receiver image (per-link stats)
cat > docker-compose.yml << 'COMPOSEEOF'
services:
  sls-management-ui:
    image: ghcr.io/openirl/sls-management-ui@sha256:2cd2c4ea05bd75144b3b30f735e62665dbb8c1352245e5b8f994790582cff007
    container_name: sls-management-ui
    restart: unless-stopped
    security_opt:
      - no-new-privileges:true
    environment:
      REACT_APP_BASE_URL: "${APP_URL}"
      REACT_APP_SRT_PLAYER_PORT: "${SRT_PLAYER_PORT:-4000}"
      REACT_APP_SRT_SENDER_PORT: "${SRT_SENDER_PORT:-4001}"
      REACT_APP_SLS_STATS_PORT: "${SLS_STATS_PORT:-8080}"
      REACT_APP_SRTLA_PORT: "${SRTLA_PORT:-5000}"
    ports:
      - "${SLS_MGNT_PORT}:3000"
    logging:
      driver: "json-file"
      options:
        max-size: "10m"
        max-file: "3"

  receiver:
    image: ghcr.io/michaelpentz/srtla-receiver:latest
    container_name: srtla-receiver
    restart: unless-stopped
    security_opt:
      - no-new-privileges:true
    ports:
      - "${SLS_STATS_PORT}:8080/tcp"
      - "${SRTLA_PORT}:5000/udp"
      - "${SRT_SENDER_PORT}:4001/udp"
      - "${SRT_PLAYER_PORT}:4000/udp"
      - "5080:5080/tcp"
    volumes:
      - ./data:/var/lib/sls
      - ./data/ipinfo_lite.mmdb:/usr/share/GeoIP/ipinfo_lite.mmdb:ro
    logging:
      driver: "json-file"
      options:
        max-size: "10m"
        max-file: "3"
COMPOSEEOF

# Write .env (non-interactive, using defaults aligned with Aegis)
cat > .env << 'ENVEOF'
SRTLA_PORT=5000
SRT_SENDER_PORT=4001
SRT_PLAYER_PORT=4000
SLS_MGNT_PORT=3000
SLS_STATS_PORT=8090
APP_URL=http://localhost
# Per-link stats exposed on port 5080 (hardcoded in compose, not variable)
ENVEOF

# Pull latest images (AMI has pre-pulled base, but :latest may have been updated)
docker compose pull
docker compose up -d

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver containers started"

# Wait until containers are healthy/running before signaling ready
deadline=$((SECONDS + 120))
while true; do
  all_ready=true
  has_containers=false
  while IFS= read -r cid; do
    if [ -z "${cid}" ]; then
      continue
    fi

    has_containers=true
    health=$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "${cid}")
    status=$(docker inspect --format '{{.State.Status}}' "${cid}")

    if [ "${health}" = "none" ]; then
      if [ "${status}" != "running" ]; then
        all_ready=false
        break
      fi
    elif [ "${health}" != "healthy" ]; then
      all_ready=false
      break
    fi
  done < <(docker compose ps -q)

  if [ "${has_containers}" = true ] && [ "${all_ready}" = true ]; then
    break
  fi

  if [ ${SECONDS} -ge ${deadline} ]; then
    echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') timeout waiting for srtla-receiver containers to become ready"
    docker compose ps
    exit 1
  fi

  sleep 3
done

# Extract API key (try .apikey file first, then container logs as fallback)
APIKEY=""
for attempt in $(seq 1 30); do
  # Method 1: .apikey file written by srtla-receiver container
  if [ -f /opt/srtla-receiver/data/.apikey ]; then
    APIKEY=$(cat /opt/srtla-receiver/data/.apikey)
    chmod 0600 /opt/srtla-receiver/data/.apikey 2>/dev/null || true
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
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') WARNING: .apikey not found, stream auto-creation skipped"
else
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') API key found, creating default stream"
  # Create a default publisher stream via the backend API
  STREAM_RESP=$(curl -s -X POST http://localhost:8090/api/stream-ids \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer ${APIKEY}" \
    -d '{"publisher":"live_STREAM_TOKEN","player":"play_STREAM_TOKEN","description":"aegis-relay"}')
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') stream create response: [redacted]"

  # Fetch the created stream to get publish/play keys
  STREAMS=$(curl -s http://localhost:8090/api/stream-ids \
    -H "Authorization: Bearer ${APIKEY}")
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') streams: [redacted]"

  # Write stream info under the service data directory (not world-readable /tmp)
  echo "${STREAMS}" > /opt/srtla-receiver/data/srtla-streams.json
  echo "${APIKEY}" > /opt/srtla-receiver/data/srtla-apikey
  chmod 0600 /opt/srtla-receiver/data/srtla-apikey /opt/srtla-receiver/data/srtla-streams.json
fi

# Dump final container state for debugging
docker compose ps --format json > /tmp/srtla-containers.json 2>/dev/null || true
docker compose logs --tail=50 > /tmp/srtla-container-logs.txt 2>/dev/null || true

# Signal ready (marker file for health check polling)
touch /tmp/srtla-ready

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup complete"
`

type AWSProvisioner struct {
	amiByRegion   map[string]string
	instanceType  string
	subnetID      string
	securityGroup []string
	keyName       string
	eipStore      EIPStore // optional; nil disables EIP management
}

type AWSProvisionerOptions struct {
	AMIByRegion   map[string]string
	InstanceType  string
	SubnetID      string
	SecurityGroup []string
	KeyName       string
	EIPStore      EIPStore // optional; nil disables EIP management
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
		eipStore:      opts.EIPStore,
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

	userData := strings.ReplaceAll(relayUserDataScript, "STREAM_TOKEN", req.StreamToken)
	userData = base64.StdEncoding.EncodeToString([]byte(userData))
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

	// --- Elastic IP management (stable addressing across provision cycles) ---
	// If an EIPStore is configured, try to allocate/associate an EIP for the user.
	// On failure, fall back to the auto-assigned public IP — the relay still works,
	// it just won't have a stable address for DNS.
	if p.eipStore != nil {
		eipAllocID, eipIP, eipErr := p.eipStore.GetUserEIP(ctx, req.UserID)
		if eipErr != nil {
			log.Printf("aws_provision: get_user_eip_failed session_id=%s err=%v", req.SessionID, eipErr)
			// Treat as "no EIP yet" — fall through to allocation.
			eipAllocID = ""
			eipIP = ""
		}
		if eipAllocID == "" {
			// First provision for this user — allocate a new EIP.
			allocID, allocIP, allocErr := p.AllocateElasticIP(ctx, req.Region)
			if allocErr != nil {
				log.Printf("aws_provision: allocate_eip_failed session_id=%s err=%v (falling back to auto-assigned IP)", req.SessionID, allocErr)
			} else {
				eipAllocID = allocID
				eipIP = allocIP
				if setErr := p.eipStore.SetUserEIP(ctx, req.UserID, eipAllocID, eipIP); setErr != nil {
					log.Printf("aws_provision: set_user_eip_failed session_id=%s err=%v — releasing orphaned EIP %s", req.SessionID, setErr, eipAllocID)
					if relErr := p.ReleaseElasticIP(ctx, req.Region, eipAllocID); relErr != nil {
						log.Printf("aws_provision: release_orphaned_eip_failed session_id=%s alloc=%s err=%v", req.SessionID, eipAllocID, relErr)
					}
					eipAllocID = ""
					eipIP = ""
				}
			}
		}
		if eipAllocID != "" {
			if assocErr := p.AssociateElasticIP(ctx, req.Region, eipAllocID, instanceID); assocErr != nil {
				log.Printf("aws_provision: associate_eip_failed session_id=%s alloc=%s err=%v (falling back to auto-assigned IP)", req.SessionID, eipAllocID, assocErr)
			} else {
				log.Printf("aws_provision: eip_associated session_id=%s alloc=%s ip=%s", req.SessionID, eipAllocID, eipIP)
				publicIP = eipIP
			}
		}
	}

	return ProvisionResult{
		InstanceID:   instanceID,
		AMIID:        amiID,
		InstanceType: p.instanceType,
		PublicIP:     publicIP,
		SRTPort:      5000,
	}, nil
}

func (p *AWSProvisioner) Deprovision(ctx context.Context, req DeprovisionRequest) error {
	if strings.TrimSpace(req.InstanceID) == "" {
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
			InstanceIds: []string{req.InstanceID},
		})
		return termErr
	})
	log.Printf("metric=aws_terminate_instances_latency_ms region=%s session_id=%s instance_id=%s value=%d", req.Region, req.SessionID, req.InstanceID, time.Since(termStart).Milliseconds())
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

// AllocateElasticIP allocates a new VPC Elastic IP and tags it for management.
func (p *AWSProvisioner) AllocateElasticIP(ctx context.Context, region string) (allocationID string, publicIP string, err error) {
	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(region))
	if err != nil {
		return "", "", fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)

	out, err := client.AllocateAddress(ctx, &ec2.AllocateAddressInput{
		Domain: ec2types.DomainTypeVpc,
		TagSpecifications: []ec2types.TagSpecification{
			{
				ResourceType: ec2types.ResourceTypeElasticIp,
				Tags: []ec2types.Tag{
					{Key: aws.String("Name"), Value: aws.String("aegis-relay-eip")},
					{Key: aws.String("ManagedBy"), Value: aws.String("aegis-control-plane")},
				},
			},
		},
	})
	if err != nil {
		return "", "", fmt.Errorf("allocate address: %w", err)
	}
	allocID := aws.ToString(out.AllocationId)
	ip := aws.ToString(out.PublicIp)
	log.Printf("eip: allocated %s (%s) in %s", allocID, ip, region)
	return allocID, ip, nil
}

// AssociateElasticIP associates an allocated EIP with an EC2 instance.
func (p *AWSProvisioner) AssociateElasticIP(ctx context.Context, region, allocationID, instanceID string) error {
	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(region))
	if err != nil {
		return fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)

	_, err = client.AssociateAddress(ctx, &ec2.AssociateAddressInput{
		AllocationId: aws.String(allocationID),
		InstanceId:   aws.String(instanceID),
	})
	if err != nil {
		return fmt.Errorf("associate address: %w", err)
	}
	log.Printf("eip: associated %s with %s in %s", allocationID, instanceID, region)
	return nil
}

// ReleaseElasticIP releases an allocated EIP back to the AWS pool.
func (p *AWSProvisioner) ReleaseElasticIP(ctx context.Context, region, allocationID string) error {
	cfg, err := awscfg.LoadDefaultConfig(ctx, awscfg.WithRegion(region))
	if err != nil {
		return fmt.Errorf("aws config: %w", err)
	}
	client := ec2.NewFromConfig(cfg)

	_, err = client.ReleaseAddress(ctx, &ec2.ReleaseAddressInput{
		AllocationId: aws.String(allocationID),
	})
	if err != nil {
		return fmt.Errorf("release address: %w", err)
	}
	log.Printf("eip: released %s in %s", allocationID, region)
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
