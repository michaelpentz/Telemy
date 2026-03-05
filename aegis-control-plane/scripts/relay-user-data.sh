#!/bin/bash
# OpenIRL srtla-receiver auto-setup for Aegis relay instances
# Passed as EC2 user-data by the control plane provisioner
#
# Target: Amazon Linux 2023 (AL2023) on t3.small (x86_64) or t4g.small (aarch64)
# Installs Docker + Docker Compose, then runs srtla-receiver containers
#
# Port map:
#   UDP 5000  SRTLA bonded ingest (IRL Pro connects here)
#   UDP 4000  SRT player output (OBS connects here)
#   UDP 4001  SRT direct sender (non-bonded fallback)
#   TCP 3000  Management UI
#   TCP 8090  Backend API (remapped from default 8080 to avoid control plane conflict)

set -euo pipefail
exec > /var/log/srtla-setup.log 2>&1
echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup starting"

# Install Docker
dnf update -y
dnf install -y docker
systemctl enable docker
systemctl start docker

# Install Docker Compose plugin
DOCKER_COMPOSE_VERSION="v2.27.0"
ARCH=$(uname -m)
mkdir -p /usr/local/lib/docker/cli-plugins
curl -SL "https://github.com/docker/compose/releases/download/${DOCKER_COMPOSE_VERSION}/docker-compose-linux-${ARCH}" \
  -o /usr/local/lib/docker/cli-plugins/docker-compose
curl -SL "https://github.com/docker/compose/releases/download/${DOCKER_COMPOSE_VERSION}/docker-compose-linux-${ARCH}.sha256" \
  -o /tmp/docker-compose.sha256
echo "$(cat /tmp/docker-compose.sha256)  /usr/local/lib/docker/cli-plugins/docker-compose" | sha256sum -c -
chmod +x /usr/local/lib/docker/cli-plugins/docker-compose

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') Docker installed, setting up srtla-receiver"

# Setup srtla-receiver
mkdir -p /opt/srtla-receiver/data
cd /opt/srtla-receiver

# Fetch docker-compose from OpenIRL
SRTLA_RECEIVER_COMMIT_SHA="e2fe790dbb4c286e6506cf996f1de32bbb3764d2"
curl -sL "https://raw.githubusercontent.com/OpenIRL/srtla-receiver/${SRTLA_RECEIVER_COMMIT_SHA}/docker-compose.prod.yml" \
  -o docker-compose.yml

# Write .env (non-interactive, using defaults aligned with Aegis)
cat > .env << 'ENVEOF'
SRTLA_PORT=5000
SRT_SENDER_PORT=4001
SRT_PLAYER_PORT=4000
SLS_MGNT_PORT=3000
SLS_STATS_PORT=8090
APP_URL=http://localhost
ENVEOF

# Set ownership for SLS data dir (srtla-receiver expects uid 3001)
chown -R 3001:3001 /opt/srtla-receiver/data

# Start containers
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
    -d '{"publisher":"live_aegis","player":"play_aegis","description":"aegis-relay"}')
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') stream create response: ${STREAM_RESP}"

  # Fetch the created stream to get publish/play keys
  STREAMS=$(curl -s http://localhost:8090/api/stream-ids \
    -H "Authorization: Bearer ${APIKEY}")
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') streams: ${STREAMS}"

  # Write stream info for control plane health checks
  echo "${STREAMS}" > /tmp/srtla-streams.json
  echo "${APIKEY}" > /tmp/srtla-apikey
fi

# Signal ready (marker file for health check polling)
touch /tmp/srtla-ready

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup complete"
