# Control Plane API Migration: EC2 → Advin VPS

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move the aegis-api/aegis-jobs control plane from AWS EC2 (<redacted-ec2-ip>, ~$20/mo) to the Advin VPS (kc1.relay.telemyapp.com, $8/mo) where the personal relay already runs.

**Architecture:** PostgreSQL and both Go binaries run in Docker Compose on the Advin VPS. Cloudflare continues to terminate TLS and proxy api.telemyapp.com — only the origin A record changes. AWS is still used for relay EC2 provisioning; the Advin box just runs the API and database, not the relay instances themselves.

**Tech Stack:** Go binaries (aegis-api, aegis-jobs), PostgreSQL 16 (Docker), Docker Compose, UFW, Cloudflare DNS.

---

## Environment Quick Reference

| Item | Current (EC2) | Target (Advin) |
|------|--------------|----------------|
| Host | `<redacted-ec2-ip>` | `kc1.relay.telemyapp.com` |
| SSH | `ssh -i ~/.ssh/id_server_new ec2-user@<redacted-ec2-ip>` | `ssh <user>@kc1.relay.telemyapp.com` |
| API port | 8080 (systemd, HTTP) | 8080 (Docker, HTTP) |
| DB | PostgreSQL on host | PostgreSQL 16 (Docker, port 5432) |
| Binaries | `/opt/aegis/bin/` | Docker containers |
| Env file | `/opt/aegis/.env` (exact path TBD) | `/opt/aegis/.env` |
| Logs | `/var/log/aegis/api.log` | `docker compose logs` |

## Required Environment Variables

Collect these from the EC2 server before starting (see Phase 0).

```
AEGIS_DATABASE_URL          # postgres://... connection string
AEGIS_JWT_SECRET            # JWT signing secret
AEGIS_PLUGIN_LOGIN_COMPLETE_KEY
AEGIS_RELAY_SHARED_KEY
CLOUDFLARE_DNS_TOKEN        # for relay DNS record management
CLOUDFLARE_ZONE_ID
AEGIS_AWS_AMI_MAP           # relay EC2 AMI IDs by region
AEGIS_AWS_KEY_NAME          # EC2 key pair name
AEGIS_AWS_SECURITY_GROUP_IDS
AEGIS_AWS_SUBNET_ID
AEGIS_SUPPORTED_REGIONS     # defaults: us-east-1,eu-west-1
AEGIS_LISTEN_ADDR           # defaults to :8080
```

---

## Phase 0: Preparation (do on EC2 before touching Advin)

### Task 0.1: Collect env vars from EC2

**Step 1: SSH to EC2 and dump the env file**

```bash
ssh -i ~/.ssh/id_server_new ec2-user@<redacted-ec2-ip>
# Find where env vars live:
sudo systemctl cat aegis-api | grep -E 'Env|EnvironmentFile'
sudo cat /opt/aegis/.env   # or wherever it is
```

**Step 2: Copy to a local safe file**

```bash
# On local machine — save to a temp file, NOT committed to git
mkdir -p /tmp/aegis-migration
# Paste contents into /tmp/aegis-migration/ec2.env
```

Expected: All required vars from the list above are present.

### Task 0.2: Note the current database name and user

```bash
# Still on EC2
sudo -u postgres psql -c "\l"
sudo -u postgres psql -c "\du"
```

Expected: See database name (likely `aegis` or similar) and user (likely `aegis`).

### Task 0.3: Cross-compile the binaries locally

```bash
cd E:/Code/telemyapp/telemy-v0.0.5/aegis-control-plane

GOOS=linux GOARCH=amd64 go build -o /tmp/aegis-migration/aegis-api ./cmd/api/
GOOS=linux GOARCH=amd64 go build -o /tmp/aegis-migration/aegis-jobs ./cmd/jobs/

ls -lh /tmp/aegis-migration/aegis-api /tmp/aegis-migration/aegis-jobs
```

Expected: Both binaries present, ~10-30MB each.

---

## Phase 1: PostgreSQL on Advin (Docker)

Run PostgreSQL as a Docker container. The relay stack already uses Docker Compose in `/opt/srtla-receiver/`. The control plane gets its own directory.

### Task 1.1: Create the control plane directory on Advin

```bash
ssh <user>@kc1.relay.telemyapp.com

sudo mkdir -p /opt/aegis/{bin,data,logs}
sudo chown $USER:$USER /opt/aegis
```

### Task 1.2: Write the Docker Compose file

Create `/opt/aegis/docker-compose.yml`:

```yaml
services:
  postgres:
    image: postgres:16-alpine
    container_name: aegis-postgres
    restart: unless-stopped
    environment:
      POSTGRES_DB: aegis
      POSTGRES_USER: aegis
      POSTGRES_PASSWORD_FILE: /run/secrets/db_password
    secrets:
      - db_password
    volumes:
      - pgdata:/var/lib/postgresql/data
    ports:
      - "127.0.0.1:5432:5432"   # localhost only — not exposed externally
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U aegis"]
      interval: 10s
      timeout: 5s
      retries: 5

  aegis-api:
    image: ubuntu:24.04
    container_name: aegis-api
    restart: unless-stopped
    depends_on:
      postgres:
        condition: service_healthy
    env_file: .env
    volumes:
      - ./bin/aegis-api:/usr/local/bin/aegis-api:ro
      - ./logs:/var/log/aegis
    ports:
      - "8080:8080"
    command: ["/usr/local/bin/aegis-api"]

  aegis-jobs:
    image: ubuntu:24.04
    container_name: aegis-jobs
    restart: unless-stopped
    depends_on:
      postgres:
        condition: service_healthy
    env_file: .env
    volumes:
      - ./bin/aegis-jobs:/usr/local/bin/aegis-jobs:ro
      - ./logs:/var/log/aegis
    command: ["/usr/local/bin/aegis-jobs"]

secrets:
  db_password:
    file: ./secrets/db_password.txt

volumes:
  pgdata:
```

> **Note on ubuntu:24.04 image:** The Go binaries are statically linked (`CGO_ENABLED=0` — verify this). If they dynamically link glibc, `ubuntu:24.04` provides compatible glibc. Alternatively use `scratch` or `debian:bookworm-slim`.

**Step 1: Verify CGO status of the build**

```bash
# On local machine
file /tmp/aegis-migration/aegis-api
# Expected: "ELF 64-bit LSB executable, x86-64, ... statically linked"
# OR: "... dynamically linked (uses shared libs)"
```

If statically linked → use `FROM scratch` or `FROM alpine` in Compose. If dynamically linked → `ubuntu:24.04` is correct.

### Task 1.3: Create secrets and env file on Advin

```bash
# On Advin
mkdir -p /opt/aegis/secrets
chmod 700 /opt/aegis/secrets

# Set DB password (generate a strong one)
echo "CHANGEME_STRONG_PASSWORD" > /opt/aegis/secrets/db_password.txt
chmod 600 /opt/aegis/secrets/db_password.txt
```

Create `/opt/aegis/.env` from your collected env vars. **Replace the DATABASE_URL** to point at the Docker network:

```bash
# .env content (replace values from ec2.env):
AEGIS_DATABASE_URL=postgres://aegis:CHANGEME_STRONG_PASSWORD@postgres:5432/aegis?sslmode=disable
AEGIS_JWT_SECRET=<from ec2.env>
AEGIS_PLUGIN_LOGIN_COMPLETE_KEY=<from ec2.env>
AEGIS_RELAY_SHARED_KEY=<from ec2.env>
CLOUDFLARE_DNS_TOKEN=<from ec2.env>
CLOUDFLARE_ZONE_ID=<from ec2.env>
AEGIS_AWS_AMI_MAP=<from ec2.env>
AEGIS_AWS_KEY_NAME=<from ec2.env>
AEGIS_AWS_SECURITY_GROUP_IDS=<from ec2.env>
AEGIS_AWS_SUBNET_ID=<from ec2.env>
AEGIS_SUPPORTED_REGIONS=us-east-1,eu-west-1
AEGIS_LISTEN_ADDR=:8080
```

```bash
chmod 600 /opt/aegis/.env
```

### Task 1.4: Start PostgreSQL only

```bash
cd /opt/aegis
docker compose up -d postgres
docker compose logs postgres
```

Expected: `database system is ready to accept connections`

---

## Phase 2: Database Migration

### Task 2.1: Dump from EC2

```bash
# On EC2
sudo -u postgres pg_dump -Fc aegis > /tmp/aegis-$(date +%Y%m%d).dump

# Verify dump size (should be non-zero)
ls -lh /tmp/aegis-*.dump
```

### Task 2.2: SCP dump to Advin

```bash
# On local machine (adjust paths):
scp -i ~/.ssh/id_server_new \
  ec2-user@<redacted-ec2-ip>:/tmp/aegis-$(date +%Y%m%d).dump \
  /tmp/aegis-migration/aegis.dump

scp /tmp/aegis-migration/aegis.dump \
  <user>@kc1.relay.telemyapp.com:/tmp/aegis.dump
```

### Task 2.3: Restore into Docker PostgreSQL

```bash
# On Advin
docker exec -i aegis-postgres \
  pg_restore \
    -U aegis \
    -d aegis \
    --no-owner \
    --role=aegis \
    -v \
  < /tmp/aegis.dump
```

Expected: Tables and data restored. Ignore "role does not exist" warnings for owner roles that differ.

### Task 2.4: Verify row counts match EC2

```bash
# On Advin
docker exec -it aegis-postgres psql -U aegis -d aegis \
  -c "SELECT schemaname, tablename, n_live_tup FROM pg_stat_user_tables ORDER BY n_live_tup DESC;"
```

```bash
# On EC2 (for comparison)
sudo -u postgres psql -d aegis \
  -c "SELECT schemaname, tablename, n_live_tup FROM pg_stat_user_tables ORDER BY n_live_tup DESC;"
```

Expected: Row counts match (or are within a few rows if there was recent activity).

### Task 2.5: Verify all migrations are present

```bash
# On Advin
docker exec -it aegis-postgres psql -U aegis -d aegis \
  -c "SELECT * FROM schema_migrations ORDER BY version;"
# (Adjust table name if the migration tracker uses a different name)
```

Expected: All migrations 0001–0013 plus 004/005 present. No gaps that would break the running binary.

---

## Phase 3: Deploy Control Plane on Advin

### Task 3.1: Copy binaries to Advin

```bash
# On local machine
scp /tmp/aegis-migration/aegis-api <user>@kc1.relay.telemyapp.com:/opt/aegis/bin/
scp /tmp/aegis-migration/aegis-jobs <user>@kc1.relay.telemyapp.com:/opt/aegis/bin/
```

```bash
# On Advin — verify and set executable
chmod +x /opt/aegis/bin/aegis-api /opt/aegis/bin/aegis-jobs
ls -lh /opt/aegis/bin/
```

### Task 3.2: Start aegis-api (Advin, not yet live traffic)

```bash
cd /opt/aegis
docker compose up -d aegis-api aegis-jobs
docker compose logs --tail=50 aegis-api
docker compose logs --tail=50 aegis-jobs
```

Expected: `API server starting on plain HTTP (:8080)` — no crash, no DB connection errors.

### Task 3.3: Smoke-test the health endpoint from Advin localhost

```bash
# On Advin (internal, bypassing Cloudflare)
curl -s http://localhost:8080/health
```

Expected: `{"status":"ok"}` or equivalent 200 response. If endpoint path differs, check API_SPEC_v1.md.

---

## Phase 4: UFW Rules on Advin

Cloudflare proxies api.telemyapp.com (orange cloud). Only Cloudflare's edge IPs need to reach port 8080. Restrict to Cloudflare IP ranges for defense-in-depth.

### Task 4.1: Add Cloudflare IPv4 ranges to UFW

Cloudflare publishes its IP ranges at https://www.cloudflare.com/ips-v4 (verify current list before running):

```bash
# On Advin — add each Cloudflare IPv4 range
for ip in \
  173.245.48.0/20 \
  103.21.244.0/22 \
  103.22.200.0/22 \
  103.31.4.0/22 \
  141.101.64.0/18 \
  108.162.192.0/18 \
  190.93.240.0/20 \
  188.114.96.0/20 \
  197.234.240.0/22 \
  198.41.128.0/17 \
  162.158.0.0/15 \
  104.16.0.0/13 \
  104.24.0.0/14 \
  172.64.0.0/13 \
  131.0.72.0/22; do
  sudo ufw allow from $ip to any port 8080 proto tcp
done
```

### Task 4.2: Verify UFW status

```bash
sudo ufw status numbered | grep 8080
```

Expected: Rules allowing Cloudflare ranges to port 8080 TCP.

### Task 4.3: Confirm port 8080 is NOT open to 0.0.0.0 (world)

```bash
sudo ufw status verbose | grep "8080"
```

Expected: No `ALLOW IN` from `Anywhere` for port 8080. Only the Cloudflare ranges.

---

## Phase 5: Pre-Cutover Parallel Verification

Test the Advin API directly by IP before touching DNS. This confirms the new deployment works end-to-end without any risk.

### Task 5.1: Test API via direct IP from local machine

```bash
# From local machine — bypass Cloudflare entirely
curl -s -H "Host: api.telemyapp.com" http://kc1.relay.telemyapp.com:8080/health
```

> **Note:** If UFW blocks your IP (non-Cloudflare), temporarily allow your IP: `sudo ufw allow from <your-ip> to any port 8080 proto tcp`, test, then remove.

Expected: 200 health response.

### Task 5.2: Test an auth endpoint

```bash
# Example — adjust to a real endpoint from API_SPEC_v1.md
curl -s -X POST \
  -H "Host: api.telemyapp.com" \
  -H "Content-Type: application/json" \
  http://kc1.relay.telemyapp.com:8080/v1/health \
  -w "\nHTTP %{http_code}\n"
```

Expected: Valid response, not a 500 or connection refused.

### Task 5.3: Check logs for errors

```bash
# On Advin
docker compose -f /opt/aegis/docker-compose.yml logs --tail=100 aegis-api
```

Expected: No panics, no DB errors, no auth errors from the test requests.

---

## Phase 6: DNS Cutover

Cloudflare's orange-cloud proxy means the DNS change takes effect within seconds — TTL is managed by Cloudflare, not your origin TTL.

### Task 6.1: Update the A record in Cloudflare

Via Cloudflare dashboard or API:

```
api.telemyapp.com  A  kc1.relay.telemyapp.com  Proxied (orange cloud ON)
```

Change from `<redacted-ec2-ip>` → `kc1.relay.telemyapp.com`. Keep **Proxied** enabled.

The existing Cloudflare Origin Rule (port rewrite → 8080) and SSL Flexible config apply unchanged.

### Task 6.2: Verify DNS propagation (Cloudflare edge, not origin)

```bash
# Cloudflare proxied — the returned IP will be a Cloudflare edge IP, not kc1.relay.telemyapp.com
# Check the record was saved:
curl -s "https://dns.google/resolve?name=api.telemyapp.com&type=A" | python3 -m json.tool
```

Expected: Cloudflare edge IP in answer (e.g., 104.x.x.x range), not <redacted-ec2-ip>.

---

## Phase 7: Post-Cutover Verification

### Task 7.1: Hit health endpoint through Cloudflare

```bash
curl -sv https://api.telemyapp.com/health
```

Expected:
- TLS cert valid (Cloudflare Universal SSL)
- HTTP 200
- Response body matches what you got in Phase 5

### Task 7.2: Check API logs on Advin for real traffic

```bash
# On Advin — watch for Cloudflare-sourced requests (CF-Connecting-IP headers)
docker compose -f /opt/aegis/docker-compose.yml logs -f aegis-api
```

Expected: Incoming requests appear with Cloudflare edge IPs as source.

### Task 7.3: Test OBS plugin connect (live device)

In OBS with the Telemy plugin loaded:
1. Open the Telemy dock
2. Trigger any action that hits the API (relay provision, auth check, etc.)
3. Check dock for success response

Expected: No connection errors in the dock.

### Task 7.4: Test Cloudflare Pages Functions (plugin login)

Run through a real plugin login flow end-to-end.

Expected: Auth flow completes, JWT issued, no 502/504 from Cloudflare.

### Task 7.5: Monitor for 30 minutes

```bash
# On Advin — watch API logs
docker compose -f /opt/aegis/docker-compose.yml logs -f

# Watch for any 5xx responses or panics
```

Expected: Clean logs, no errors.

---

## Rollback Plan

DNS rollback via Cloudflare takes effect in seconds (orange cloud, no caching delay).

### Rollback Step 1: Revert A record in Cloudflare

```
api.telemyapp.com  A  <redacted-ec2-ip>  Proxied (orange cloud ON)
```

Do NOT terminate the EC2 instance until you've been stable on Advin for at least 24 hours.

### Rollback Step 2: Verify traffic returns to EC2

```bash
curl -sv https://api.telemyapp.com/health
# Then check EC2 logs:
ssh -i ~/.ssh/id_server_new ec2-user@<redacted-ec2-ip>
tail -f /var/log/aegis/api.log
```

### Rollback trigger criteria

Roll back immediately if any of these occur after DNS cutover:
- Health endpoint returns non-200
- OBS plugin fails to connect/auth
- Database errors in logs
- Any relay provisioning failures

---

## Phase 8: EC2 Teardown

**Wait at least 24 hours after a clean cutover before terminating.**

### Task 8.1: Stop and disable EC2 services

```bash
# On EC2 — stop services (don't terminate yet)
ssh -i ~/.ssh/id_server_new ec2-user@<redacted-ec2-ip>
sudo systemctl stop aegis-api aegis-jobs
sudo systemctl disable aegis-api aegis-jobs
```

Expected: Services stop. Verify Advin is still handling all traffic.

### Task 8.2: Final backup snapshot

Before terminating, create an AMI snapshot for safety:

```bash
# Via AWS console or CLI
aws ec2 create-image \
  --instance-id <EC2_INSTANCE_ID> \
  --name "aegis-api-final-backup-$(date +%Y%m%d)" \
  --no-reboot
```

Expected: AMI created successfully.

### Task 8.3: Terminate EC2 instance

Via AWS console: EC2 → Instances → Select `<redacted-ec2-ip>` → Instance State → Terminate.

> **Check before terminating:**
> - No EIP attached to this instance that needs to be kept (the relay EIP `44.237.197.131` is on a relay instance, not the API server — confirm)
> - Security group isn't shared with anything else
> - Any EIPs attached to the API EC2 — release them to stop the ~$3.60/mo EIP charge

### Task 8.4: Release EIP if attached to API EC2

```bash
# List EIPs
aws ec2 describe-addresses --query 'Addresses[*].{IP:PublicIp,AllocationId:AllocationId,InstanceId:InstanceId}'

# Release any EIP that was on <redacted-ec2-ip> (confirm it's NOT 44.237.197.131)
aws ec2 release-address --allocation-id <alloc-id>
```

### Task 8.5: Verify monthly cost reduction

- EC2 t3.small: ~$20/mo → **eliminated**
- Any attached EIP (unattached after termination): ~$3.60/mo → **eliminated**
- Advin VPS: $8/mo (no change, already running)
- Net savings: ~$15-20/mo

---

## Ongoing Ops Notes

**Deploy new binary to Advin:**
```bash
# Cross-compile locally
GOOS=linux GOARCH=amd64 go build -o /tmp/aegis-api ./cmd/api/

# Copy and restart
scp /tmp/aegis-api <user>@kc1.relay.telemyapp.com:/opt/aegis/bin/aegis-api
ssh <user>@kc1.relay.telemyapp.com "cd /opt/aegis && docker compose restart aegis-api"
```

**View logs:**
```bash
ssh <user>@kc1.relay.telemyapp.com "docker compose -f /opt/aegis/docker-compose.yml logs --tail=100 -f"
```

**Database backup (add to cron):**
```bash
docker exec aegis-postgres \
  pg_dump -U aegis aegis | gzip > /opt/aegis/backups/aegis-$(date +%Y%m%d).sql.gz
```

**PostgreSQL access:**
```bash
docker exec -it aegis-postgres psql -U aegis -d aegis
```
