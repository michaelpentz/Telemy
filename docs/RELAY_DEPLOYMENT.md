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
| 5000 | UDP | SRTLA bonded ingest (IRL Pro srtla://) | 0.0.0.0/0 |
| 4001 | UDP | SRT publisher (SLS ingest) | 0.0.0.0/0 |
| 4000 | UDP | SRT player (SLS output for OBS) | 0.0.0.0/0 |
| 22   | TCP | SSH Access (Diagnostics) | 0.0.0.0/0 (or admin IP) |
| 3000 | TCP | SLS Management UI | Control plane IP only |
| 8090 | TCP | srtla-receiver Backend API | Control plane IP only |

**Note:** `srtla_rec` acts as a raw UDP proxy on port 5000, forwarding bonded traffic to `localhost:4001` where SLS handles the SRT session.

## Security Group

Security group: `aegis-relay-sg` (`sg-0da8cf50c2fd72518`)

- **UDP 4000-5000**: Open to all (`0.0.0.0/0`) for dynamic cellular ingest.
- **TCP 22**: Open for SSH diagnostics using `aegis-relay-key.pem`.
- **TCP 3000, 8090**: Restricted to control plane IP (`52.13.2.122/32`).

## SSH Access

To diagnose a relay instance:
```bash
ssh -i aegis-relay-key.pem ec2-user@{relay_ip}
```

## Automatic Provisioning

The control plane provisions the relay and auto-creates stream IDs:
- **Publisher ID**: `live_aegis`
- **Player ID**: `play_aegis`

The API key for the backend is auto-generated on first boot and stored at:
`/opt/srtla-receiver/data/.apikey`

## Manual Install (Ad-Hoc Testing)

SSH into a running relay instance and run:

```bash
# ... (Docker/Compose install steps unchanged)

# Setup srtla-receiver
sudo mkdir -p /opt/srtla-receiver/data
cd /opt/srtla-receiver

sudo curl -sL https://raw.githubusercontent.com/OpenIRL/srtla-receiver/main/docker-compose.prod.yml \
  -o docker-compose.yml

sudo tee .env << 'EOF'
SRTLA_PORT=5000
SRT_SENDER_PORT=4001
SRT_PLAYER_PORT=4000
SLS_MGNT_PORT=3000
SLS_STATS_PORT=8090
APP_URL=http://localhost
EOF

sudo chown -R 3001:3001 /opt/srtla-receiver/data
sudo docker compose up -d
```

## IRL Pro Connection Guide (Bonded)

1. **Bonding Mode**: Use **SRTLA** (not SRT with bonding toggle).
2. **Settings**: Go to **Settings > Bonding > Own Bonding Server (SRTLA)**.
3. **URL**: `srtla://{relay_ip}:5000`
4. **SRT Settings**:
   - **Stream ID**: `live_aegis`
   - **Latency**: `2500` ms (recommended)
5. **Validation**: Confirm bonding works by checking both WiFi and Cellular icons in the SRTLA group.

*Note: Enabling the built-in "Connection Bonding Service" routes through IRL Toolkit proxies and will fail with our private relay.*

## OBS SRT Input Setup

1. Add a **Media Source** in OBS.
2. Uncheck **Local File**.
3. **Input**: `srt://{relay_ip}:4000?streamid=play_aegis`
4. **Input Format**: `mpegts`
5. The stream should appear with ~5s end-to-end latency (typical for bonded relay).

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
