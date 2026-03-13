#!/bin/bash
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
    image: ghcr.io/openirl/sls-management-ui:latest
    container_name: sls-management-ui
    restart: unless-stopped
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

# Dump final container state for debugging
docker compose ps --format json > /tmp/srtla-containers.json 2>/dev/null || true
docker compose logs --tail=50 > /tmp/srtla-container-logs.txt 2>/dev/null || true

# Signal ready (marker file for health check polling)
touch /tmp/srtla-ready

echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ') srtla-receiver setup complete"
