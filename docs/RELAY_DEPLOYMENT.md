# Relay Deployment Guide

## Overview

Aegis relay instances run [OpenIRL srtla-receiver](https://github.com/OpenIRL/srtla-receiver) to provide bonded SRT relay for IRL streaming. The control plane provisions ephemeral EC2 instances that auto-install srtla-receiver via user-data.

**Pipeline:** IRL Pro (bonded cellular) -> SRTLA -> EC2 relay -> SRT -> OBS

## Software Stack

| Component | Purpose |
|-----------|---------|
| Amazon Linux 2023 | Base OS (x86_64 or aarch64) |
| Docker + Compose | Container runtime |
| srtla-receiver | SRTLA bonded SRT relay (Docker containers) |

srtla-receiver provides:
- **SRTLA bonded ingest** — accepts bonded connections from IRL Pro
- **SRT player output** — single SRT stream for OBS consumption
- **SRT direct sender** — non-bonded SRT fallback
- **Management UI** — web UI for stream creation and monitoring
- **Backend API** — REST API for programmatic control

## Port Map

| Port | Protocol | Purpose | Security Group |
|------|----------|---------|----------------|
| 5000 | UDP | SRTLA bonded ingest (IRL Pro connects here) | 0.0.0.0/0 |
| 4000 | UDP | SRT player output (OBS connects here) | 0.0.0.0/0 |
| 4001 | UDP | SRT direct sender (non-bonded fallback) | 0.0.0.0/0 |
| 3000 | TCP | Management UI | Control plane IP only |
| 8090 | TCP | Backend API | Control plane IP only |
| 9000 | UDP | Legacy SRT ingest (deprecated, kept for transition) | 0.0.0.0/0 |
| 7443 | TCP | Legacy WebSocket/TLS (kept for transition) | 0.0.0.0/0 |

**Note:** Backend API remapped from default 8080 to 8090 to avoid conflict with the control plane listener.

## Security Group

Security group: `aegis-relay-sg` (`sg-0da8cf50c2fd72518`)

Management ports (TCP 3000, 8090) are restricted to the control plane IP (`52.13.2.122/32`). UDP ports are open to all sources since IRL Pro connections come from dynamic cellular IPs.

## Automatic Provisioning

The control plane passes a user-data script to each EC2 instance at launch. The script:

1. Installs Docker and Docker Compose plugin
2. Downloads `docker-compose.prod.yml` from OpenIRL
3. Configures ports via `.env` file
4. Starts srtla-receiver containers
5. Writes `/tmp/srtla-ready` marker when complete

Boot time: ~2-3 minutes from instance launch to srtla-receiver ready.

Source: `aegis-control-plane/scripts/relay-user-data.sh` (canonical reference)
Inline: `internal/relay/aws.go` (compiled into provisioner binary)

## Manual Install (Ad-Hoc Testing)

SSH into a running relay instance and run:

```bash
# Install Docker
sudo dnf update -y
sudo dnf install -y docker
sudo systemctl enable docker
sudo systemctl start docker

# Install Docker Compose
ARCH=$(uname -m)
sudo mkdir -p /usr/local/lib/docker/cli-plugins
sudo curl -SL "https://github.com/docker/compose/releases/download/v2.27.0/docker-compose-linux-${ARCH}" \
  -o /usr/local/lib/docker/cli-plugins/docker-compose
sudo chmod +x /usr/local/lib/docker/cli-plugins/docker-compose

# Setup srtla-receiver
sudo mkdir -p /opt/srtla-receiver/data
cd /opt/srtla-receiver

sudo curl -sL https://raw.githubusercontent.com/OpenIRL/srtla-receiver/main/docker-compose.prod.yml \
  -o docker-compose.yml

sudo tee .env << 'EOF'
SRTLA_PORT=5000
SRT_PORT=4001
SRT_PLAYER_PORT=4000
MANAGEMENT_PORT=3000
BACKEND_PORT=8090
URL=http://localhost
EOF

sudo chown -R 3001:3001 /opt/srtla-receiver/data
sudo docker compose up -d
```

## IRL Pro Connection Guide

1. Provision a relay via the control plane API or dock UI
2. Open the management UI at `http://{relay_ip}:3000`
3. Create a new stream (note the stream ID)
4. In IRL Pro, set the SRTLA server URL:
   ```
   srtla://{relay_ip}:5000?streamid={stream_id}
   ```
5. Start streaming in IRL Pro

## OBS SRT Input Setup

1. Add a **Media Source** in OBS
2. Uncheck "Local File"
3. Set Input to:
   ```
   srt://{relay_ip}:4000?streamid={stream_id}&mode=caller
   ```
4. The stream should appear within a few seconds

## Troubleshooting

### Check container status
```bash
cd /opt/srtla-receiver && sudo docker compose ps
```

### View container logs
```bash
sudo docker compose logs -f
```

### Check setup log
```bash
cat /var/log/srtla-setup.log
```

### Verify srtla-receiver is listening
```bash
ss -ulnp | grep 5000   # SRTLA ingest
ss -ulnp | grep 4000   # SRT player
ss -tlnp | grep 3000   # Management UI
```

### Management API: list streams
```bash
curl -s http://localhost:8090/api/streams | jq .
```

### Common issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| Containers not running | Docker not started | `sudo systemctl start docker && cd /opt/srtla-receiver && sudo docker compose up -d` |
| IRL Pro can't connect | Security group missing UDP 5000 | Verify `aegis-relay-sg` has UDP 5000 rule |
| OBS shows black screen | Wrong stream ID or stream not created | Verify stream exists in management UI |
| Management UI unreachable | TCP 3000 restricted to control plane IP | SSH tunnel: `ssh -L 3000:localhost:3000 ec2-user@{relay_ip}` |

## Future: Custom AMI (Deferred)

For production, a Packer template will pre-bake Docker + srtla-receiver into the AMI. This eliminates the ~2-3 min user-data boot delay. Not yet implemented — user-data approach is sufficient for testing.
