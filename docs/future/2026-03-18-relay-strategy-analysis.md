# Telemy Relay Architecture Analysis

**Date:** 2026-03-18
**Version analyzed:** telemy-v0.0.5
**Author:** Claude (analysis based on full codebase read)

---

## A. Current Architecture Summary

### End-to-End Relay Provisioning Flow

The relay system provisions ephemeral AWS EC2 instances that run a Docker-based SRTLA/SRT stack for bonded IRL streaming. Here is the complete flow:

**1. Plugin initiates relay start**

The OBS plugin's `RelayClient::Start()` sends `POST /api/v1/relay/start` to the Go control plane with a JWT bearer token and idempotency key. The request body includes region preference and client context.

**2. Control plane validates entitlement**

`handleRelayStart()` in `handlers.go` performs three checks: JWT auth (user identity), entitlement lookup (subscription tier allows relay access), and idempotency dedup. If the user is not entitled, the request is rejected with a 403.

**3. Session creation and async provisioning**

`store.StartOrGetSession()` creates a session row (status: `provisioning`). The handler returns immediately with the session ID, then launches `runProvisionPipeline()` as a background goroutine.

**4. EC2 instance launch (`aws.go`)**

`AWSProvisioner.Provision()` calls `ec2:RunInstances` with:
- Pre-baked AMI (`aegis-relay-v1`) per region from `AEGIS_AWS_AMI_MAP`
- Instance type: `t4g.small` (ARM/Graviton, configurable)
- User-data script with `STREAM_TOKEN` placeholder replaced with the user's `stream_token`
- Tags: session ID, user ID, managed-by marker
- Subnet + security group configuration
- SSH key for diagnostics

The provisioner waits for the instance to reach `running` state via `InstanceRunningWaiter`, then extracts the public IP.

**5. Elastic IP association**

If the user has an existing Elastic IP (stored in `users.eip_allocation_id`), it's associated with the new instance. If this is the first provision, a new EIP is allocated and stored. This gives users a stable IP across relay cycles.

**6. Provision step tracking**

The pipeline updates `provision_step` through six stages: `launching_instance` -> `waiting_for_instance` -> `starting_docker` -> `starting_containers` -> `creating_stream` -> `ready`. Each step has a 3-second minimum dwell time so the plugin (polling every 2s) sees each step.

**7. User-data bootstrap on EC2**

The `relay-user-data.sh` script runs on boot:
- Starts Docker (pre-installed in AMI)
- Writes `docker-compose.yml` with two services: `sls-management-ui` (OpenIRL web UI) and `receiver` (custom `ghcr.io/michaelpentz/srtla-receiver:latest`)
- Writes `.env` with port mappings (SRTLA:5000, SRT player:4000, SRT sender:4001, management:3000, stats:8090, per-link stats:5080)
- Pulls and starts containers
- Waits for containers to be healthy
- Extracts the SLS admin API key
- Auto-creates stream IDs: `live_{stream_token}` and `play_{stream_token}` via `POST localhost:8090/api/stream-ids`
- Writes a `/tmp/srtla-ready` marker file

**8. Health probing**

The control plane probes `http://{public_ip}:8090/health` to confirm the relay stack is up before marking the session as `active`.

**9. DNS record creation**

If the user has a relay slug, the control plane creates/updates a Cloudflare A record at `{slug}.relay.telemyapp.com` pointing to the relay IP. DNS records are permanent (never deleted on deprovision).

**10. Plugin polls and connects**

The plugin polls `GET /api/v1/relay/active` every 2s, reads the `provision_step`, and shows a progress bar. Once `active`, the plugin starts:
- SLS stats polling: `GET http://{relay_ip}:8090/stats/play_{stream_token}?legacy=1` every 2s
- Per-link stats polling: `GET http://{relay_ip}:5080/stats` every 2s
- Heartbeat: `POST /api/v1/relay/health` every 30s

**11. Streaming connections**

- Encoder (IRL Pro): `srtla://{relay_host}:5000` with `streamid=live_{stream_token}`
- Player (OBS): `srt://{relay_host}:4000?streamid=play_{stream_token}`

**12. Teardown**

Plugin sends `POST /api/v1/relay/stop`. Control plane calls `ec2:TerminateInstances`, updates session to `stopped`. EIP auto-disassociates but stays allocated. DNS record stays.

### Docker Stack on Relay

| Component | Image | Port | Purpose |
|-----------|-------|------|---------|
| `srtla_rec` | Custom fork (`michaelpentz/srtla`) built into `michaelpentz/srtla-receiver` | UDP 5000 | SRTLA bonded UDP proxy, forwards to localhost:4001 |
| SLS (SRT Live Server) | Bundled in srtla-receiver image | UDP 4001 (ingest), UDP 4000 (player) | SRT session handling |
| SLS Management UI | `ghcr.io/openirl/sls-management-ui` | TCP 3000 | Web admin UI |
| Stats API | Part of SLS | TCP 8090 | Aggregate stream stats |
| Per-link stats | Part of custom `srtla_rec` | TCP 5080 | Individual connection stats + ASN carrier ID |

The custom `srtla_rec` fork adds per-connection byte/packet counters and an HTTP stats server with optional ASN lookup via `libmaxminddb` (IPinfo Lite database).

---

## B. Coupling Analysis

### Tightly Coupled to AWS

| Component | File | AWS Dependency | Details |
|-----------|------|----------------|---------|
| **Provisioner implementation** | `internal/relay/aws.go` | **Hard** | `ec2:RunInstances`, `ec2:TerminateInstances`, `ec2:DescribeInstances`, Elastic IP allocation/association/release. Uses `aws-sdk-go-v2`. |
| **User-data script** | `scripts/relay-user-data.sh` (embedded in `aws.go`) | **Hard** | Assumes Amazon Linux 2023, `systemctl start docker`, `/opt/srtla-receiver/` path conventions, cloud-init execution model. |
| **AMI configuration** | `config.go` | **Hard** | `AEGIS_AWS_AMI_MAP` (region->AMI mapping), `AEGIS_AWS_INSTANCE_TYPE`, `AEGIS_AWS_SUBNET_ID`, `AEGIS_AWS_SECURITY_GROUP_IDS`, `AEGIS_AWS_KEY_NAME`. |
| **EIP management** | `aws.go` + `handlers.go` | **Hard** | `AllocateElasticIP`, `AssociateElasticIP`, `ReleaseElasticIP`. EIP allocation IDs stored in `users.eip_allocation_id`. |
| **Instance ID references** | `relay_instances` table, `sessions` table | **Medium** | `aws_instance_id` column, used for heartbeat identity binding and deprovision. |
| **Retry logic** | `aws.go` | **Hard** | `retryAWS()` with AWS-specific error codes (`RequestLimitExceeded`, `Throttling`, `EC2ThrottledException`, etc.). |
| **Metrics labels** | `handlers.go` | **Soft** | `"provider": s.cfg.RelayProvider` label on metrics, but hardcoded "aws" logic paths. |
| **Type assertion in pipeline** | `handlers.go` line ~200 | **Hard** | `if awsProv, ok := s.provisioner.(*relay.AWSProvisioner); ok` — EIP logic only runs for AWS. |

### Already Abstracted / Provider-Agnostic

| Component | File | Notes |
|-----------|------|-------|
| **Provisioner interface** | `internal/relay/provisioner.go` | Clean interface: `Provision(ctx, ProvisionRequest) (ProvisionResult, error)` and `Deprovision(ctx, DeprovisionRequest) error`. This is the primary abstraction seam. |
| **FakeProvisioner** | `internal/relay/fake.go` | Test/dev provisioner that returns mock data. Proves the interface works. |
| **Config provider selection** | `config.go` | `AEGIS_RELAY_PROVIDER` env var: `"fake"` or `"aws"`. Adding `"hetzner"` or `"byor"` would require a config change + new provisioner. |
| **Session state machine** | `store/store.go`, `model/types.go` | Session lifecycle (provisioning->active->grace->stopped) is completely provider-agnostic. |
| **Plugin relay client** | `relay_client.h/.cpp` | The C++ plugin talks to the control plane API and relay stats endpoints. It has zero knowledge of AWS. It only knows: `public_ip`, `srt_port`, `stream_token`, `relay_hostname`. |
| **DNS management** | `internal/dns/cloudflare.go` | Cloudflare DNS is used but only called from the provision pipeline. A BYOR model could skip DNS entirely. |
| **Docker relay stack** | `srtla-receiver-fork/` | The Docker Compose stack is cloud-agnostic. It runs anywhere Docker runs. |
| **User-data script (core logic)** | The actual bootstrap logic (write compose file, start containers, create stream) is portable. Only the AMI/cloud-init wrapper is AWS-specific. |
| **Entitlement system** | `store.GetRelayEntitlement()`, `AUTH_ENTITLEMENT_MODEL.md` | Plan tiers and usage tracking are provider-independent. |
| **Stats polling** | `relay_client.cpp` | Plugin polls relay stats via HTTP to `{relay_ip}:8090` and `{relay_ip}:5080`. Works with any host running the Docker stack. |

### Key Insight: The Provisioner Interface Is the Right Seam

The existing `Provisioner` interface in `provisioner.go` is clean and minimal:

```go
type Provisioner interface {
    Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error)
    Deprovision(ctx context.Context, req DeprovisionRequest) error
}
```

`ProvisionRequest` contains: `SessionID`, `UserID`, `Region`, `StreamToken`.
`ProvisionResult` contains: `AWSInstanceID`, `AMIID`, `InstanceType`, `PublicIP`, `SRTPort`.

The `ProvisionResult` struct has AWS-named fields (`AWSInstanceID`) but the actual values are only used as opaque identifiers downstream. Renaming to `InstanceID` would make it provider-neutral.

The `DeprovisionRequest` has `AWSInstanceID` — same rename applies.

**However**, the EIP logic in `runProvisionPipeline()` breaks the abstraction by type-asserting `s.provisioner.(*relay.AWSProvisioner)`. This would need to be refactored into the interface or handled differently for non-AWS providers.

---

## C. Three Viable Options

### Option 1: BYOR (Bring Your Own Relay)

**Concept:** Let users point the plugin at any SRTLA/SRT endpoint they control. No managed infrastructure, no provisioning.

#### What Needs to Change

**Plugin (C++ changes):**
- Add a "custom relay" mode in the dock settings UI where users enter: relay host/IP, SRT port (default 5000), and optionally their stream ID
- When custom relay is configured, skip `POST /relay/start` entirely — go straight to the stats polling and SRT connection phase
- `RelayClient` needs a `ConnectDirect(host, port, stream_token)` path that sets `current_session_` with a synthetic session (no session_id, no heartbeat, no provisioning)
- Stats polling (`PollRelayStats`, `PollPerLinkStats`) already works with any `relay_ip` — no changes needed there
- The dock UI would show relay stats but hide provisioning progress and the start/stop button (or repurpose start/stop as connect/disconnect)

**Estimated effort:** 2-3 days of C++ plugin work + 1 day dock UI changes.

**Control plane changes:**
- Minimal. BYOR users don't hit the relay API at all
- The entitlement system could add a `byor` tier that grants plugin access without relay provisioning rights
- `GET /auth/session` could return `relay_access_status: "byor"` to tell the plugin to show custom relay settings instead of the managed relay UI

**Estimated effort:** 0.5 days.

**Could the app be free for BYOR users?**

Yes. The control plane cost for BYOR users is essentially zero (no EC2, no EIP). The only costs are:
- Auth/session endpoints: negligible (a few API calls per session)
- Database row for the user account: negligible
- The plugin binary itself: zero marginal cost

A free tier with BYOR + paid tier for managed relays is very viable. The entitlement system already supports `plan_tier` with different capabilities.

**What users would need to bring:**
- A server running the srtla-receiver Docker stack (or any SRTLA-compatible endpoint)
- They'd configure IRL Pro with `srtla://{their_server}:5000` and OBS with `srt://{their_server}:4000`
- The Telemy plugin would just be a monitoring/telemetry overlay

#### Risk Assessment
- Low technical risk — plugin already has all the polling code
- Medium adoption risk — BYOR users are technical and may not need the plugin's telemetry features
- No AWS cost risk — that's the whole point

---

### Option 2: Hetzner/Cheap VPS Managed Relays

**Concept:** Replace ephemeral AWS EC2 instances with always-on shared VPS instances on Hetzner (or similar cheap providers). Multiple users share a relay server.

#### What Needs to Change

**Control plane — new `HetznerProvisioner`:**
- Implement the `Provisioner` interface for Hetzner Cloud API (`hcloud-go` SDK)
- Instead of launching per-user instances, maintain a pool of always-on relay servers
- `Provision()` would: pick an available relay from the pool (or the least-loaded one), create the user's stream IDs on it via the SLS API, and return its IP
- `Deprovision()` would: delete the user's stream IDs from the relay, but NOT destroy the server

**Estimated effort:** 3-5 days for the provisioner + pool management.

**Control plane — shared relay pool management:**
- New concept: relay pool (a set of always-on servers with capacity tracking)
- Need a `relay_pool` table: server_id, host, ip, region, status, current_sessions, max_sessions
- Need assignment logic: which relay gets the next user? (round-robin, least-loaded, region-affinity)
- Need stream isolation: each user gets unique `live_{token}`/`play_{token}` IDs, but they share the same server
- Need capacity monitoring: when a relay hits max sessions, mark it full

**Estimated effort:** 3-4 days for pool management.

**User-data / bootstrap:**
- The Docker stack setup becomes a one-time server provisioning task, not per-user
- Could use Hetzner's user-data, Ansible, or manual setup
- The srtla-receiver stack is identical — it's already Docker-based and cloud-agnostic

**Estimated effort:** 1 day.

**Elastic IP equivalent:**
- Hetzner has floating IPs (~$4/mo) — similar concept to AWS EIP
- For shared relays, a single floating IP per relay server is sufficient (not per-user)
- DNS records would point to the shared relay, not to per-user IPs

**Plugin changes:**
- None if the API contract stays the same. The plugin doesn't know or care what's behind the relay IP
- The `instance_id` field in responses would be a Hetzner server ID instead of `i-xxx`

**Estimated effort:** 0 days (plugin is already abstracted).

**Key architectural change:**
- Current model: 1 user = 1 EC2 instance (ephemeral). Relay boots when user starts, terminates when they stop.
- New model: N users = 1 VPS (always-on). Relay is pre-provisioned, users get assigned to it.
- This changes the cost model fundamentally: from per-user-per-hour to shared-infrastructure-per-month.

**EIP refactoring:**
- The `runProvisionPipeline()` type assertion `s.provisioner.(*relay.AWSProvisioner)` for EIP logic needs to be either:
  - Moved into the provisioner interface (add `ManageStaticIP()` method), or
  - Made conditional on provider type with a clean switch, or
  - Removed entirely for shared relays (the relay has a fixed IP, no per-user EIP needed)

**Estimated effort:** 1 day.

**Total estimated effort: 8-11 days.**

---

### Option 3: Hybrid Model (BYOR Free + Managed Paid)

**Concept:** Combine Option 1 and Option 2. BYOR users get the plugin for free. Managed relay users pay for the service. The managed relays could be AWS, Hetzner, or a mix.

#### Architecture Design

**Entitlement tiers:**

| Tier | Relay Mode | Cost | Features |
|------|-----------|------|----------|
| `free` | BYOR only | $0 | Plugin + telemetry + custom relay endpoint |
| `starter` | Managed (shared Hetzner) | $X/mo | Shared relay, N hours included |
| `standard` | Managed (dedicated or shared) | $Y/mo | More hours, priority assignment |
| `pro` | Managed (dedicated AWS on-demand) | $Z/mo | Dedicated instance, EIP, full isolation |

**Control plane changes:**

1. **Entitlement model expansion** (`AUTH_ENTITLEMENT_MODEL.md`, `store.go`):
   - Add `relay_mode` field to entitlement: `"byor"`, `"shared"`, `"dedicated"`
   - `relay_access_status` values: `"enabled"` (managed), `"byor"` (self-hosted only), `"disabled"`
   - The `GET /auth/session` response already returns entitlement info — just add the mode field

2. **Multi-provider provisioner routing** (`handlers.go`):
   ```go
   switch entitlement.RelayMode {
   case "byor":
       // No provisioning needed. Return user's configured endpoint.
   case "shared":
       // Use shared pool provisioner (Hetzner/cheap VPS)
   case "dedicated":
       // Use AWS provisioner (current behavior)
   }
   ```

3. **BYOR config storage:**
   - Add `byor_relay_host`, `byor_relay_port` to users table (or a separate `user_relay_config` table)
   - Plugin sends BYOR config via a new `POST /api/v1/user/relay-config` endpoint
   - `GET /relay/active` for BYOR users returns their configured endpoint without provisioning

4. **Usage tracking adjustments:**
   - BYOR users: no usage tracking (they're not consuming managed resources)
   - Shared relay users: track session duration against included hours
   - Dedicated relay users: track session duration + instance cost

**Estimated effort:**
- BYOR path: 2-3 days (plugin + control plane)
- Shared relay provisioner: 8-11 days (from Option 2)
- Entitlement model expansion: 2 days
- Multi-provider routing: 1 day
- **Total: 13-17 days** for the full hybrid model
- **Shortcut: BYOR first (3-4 days), add managed later**

---

## D. Cost Comparison

### AWS Per-User On-Demand (Current Model)

| Cost Item | Per-User Per-Hour | Per-User Per-Month (4h/day, 20 days) | Notes |
|-----------|-------------------|--------------------------------------|-------|
| t4g.small (2 vCPU, 2GB) | $0.0168/hr | $1.34/mo | us-east-1 on-demand pricing |
| Data transfer out (est. 5 Mbps avg) | ~$0.09/hr | ~$7.20/mo | $0.09/GB * ~2.25 GB/hr |
| Elastic IP (idle) | $0.005/hr | $3.60/mo | Charged when instance is OFF |
| **Total per user** | **~$0.12/hr** | **~$12.14/mo** | |

For a user streaming 4 hours per week (16 hrs/mo): **~$5.50/mo** (compute + data transfer, EIP idle cost adds ~$3 for non-streaming days).

**Key problem:** The EIP idle cost ($3.60/mo) is incurred 24/7 regardless of usage. With 100 users, that's $360/mo in EIP costs alone, even if nobody streams.

### Hetzner Shared Always-On Relay

| Cost Item | Per Server | Users Per Server | Per-User Equivalent |
|-----------|-----------|-----------------|---------------------|
| CX22 (2 vCPU, 4GB, 20TB traffic) | €4.35/mo (~$4.75) | 10-20 concurrent | $0.24-$0.48/mo |
| CAX11 (2 ARM vCPU, 4GB, 20TB) | €3.79/mo (~$4.15) | 10-20 concurrent | $0.21-$0.42/mo |
| CPX21 (3 vCPU, 4GB, 20TB) | €7.99/mo (~$8.75) | 20-40 concurrent | $0.22-$0.44/mo |
| Floating IP | €4.00/mo (~$4.35) | Shared across all users on server | $0.22-$0.44/mo |
| **Total per user** | | | **$0.43-$0.92/mo** |

**Hetzner's included traffic is massive:** 20TB/mo. At 5 Mbps average stream, one user consumes ~2.25 GB/hr. A server handling 20 concurrent users for 4 hours each would use ~180 GB/day = ~5.4 TB/mo — well within the 20TB allowance.

**No idle cost problem:** The server runs 24/7 regardless. There's no per-user idle penalty.

### Cost Comparison Summary

| Metric | AWS (current) | Hetzner (shared) | Savings |
|--------|--------------|-------------------|---------|
| Per-user per-month (casual: 16 hrs/mo) | ~$5.50 + $3.60 EIP = **~$9.10** | **~$0.50** | **~94% cheaper** |
| Per-user per-month (heavy: 80 hrs/mo) | ~$9.60 + $3.60 EIP = **~$13.20** | **~$0.50** | **~96% cheaper** |
| 100 users, 20% concurrent | ~$912/mo | ~$26/mo (3 servers) | **~97% cheaper** |
| Scaling cost model | Linear per user | Step function (add servers) | More predictable |
| Data transfer | $0.09/GB overage risk | 20TB included | No overage risk |

### The Real AWS Cost Driver: Elastic IPs

With the current model, every user who has ever provisioned a relay has an Elastic IP allocated ($3.60/mo idle). At 100 users, even with low usage, that's $360/mo in EIP costs alone. At 1000 users: $3,600/mo. This doesn't scale.

**If you drop EIPs** (accept DNS-based addressing with possible stale caches on iOS), the per-user cost drops to ~$5.50/mo for casual users. But mobile client DNS caching is a real problem for IRL streaming.

---

## E. What's Already Abstracted

### Strong Abstractions (Ready for Multi-Provider)

1. **`Provisioner` interface** (`provisioner.go`) — the main seam. Any new provider implements `Provision()` and `Deprovision()`. Already has `FakeProvisioner` as proof.

2. **`AEGIS_RELAY_PROVIDER` config** (`config.go`) — environment variable that selects the provisioner. Currently `"fake"` or `"aws"`. Adding `"hetzner"` or `"byor"` is a config-level change.

3. **Session state machine** — completely provider-agnostic. Works with any provisioner.

4. **Plugin relay client** — zero cloud awareness. Only knows HTTP endpoints and IP addresses.

5. **Docker relay stack** — runs anywhere. The `srtla-receiver-fork` is a self-contained Docker image with no cloud dependencies.

6. **Entitlement system** — plan tiers and usage tracking are provider-independent. Adding a `byor` tier is a data change.

7. **DNS management** — Cloudflare DNS is called from the pipeline but could be made optional or swapped.

### Weak Abstractions (Need Refactoring)

1. **EIP management** — hardcoded type assertion `s.provisioner.(*relay.AWSProvisioner)` in `runProvisionPipeline()`. This needs to be either part of the interface or behind a provider-specific hook.

2. **`ProvisionResult` field names** — `AWSInstanceID` should be renamed to `InstanceID` for neutrality.

3. **`DeprovisionRequest` field names** — same: `AWSInstanceID` should be `InstanceID`.

4. **`relay_instances` table** — has `aws_instance_id` column. Should be `instance_id` or `provider_instance_id`.

5. **User-data script** — embedded as a Go string constant in `aws.go`. The script logic (write compose, start containers, create stream) is portable, but the delivery mechanism (EC2 user-data) is AWS-specific. For Hetzner, you'd use cloud-init or Ansible. For BYOR, it's the user's problem.

6. **Health probing URL** — hardcoded to `http://{ip}:8090/health` in the pipeline. This is actually relay-stack-specific (not AWS-specific), so it works for any provider running the same Docker stack. But for BYOR with a different stack, it might not apply.

### Recommended Refactoring Priority

1. **Rename AWS-specific fields** (1 hour) — `AWSInstanceID` -> `InstanceID` everywhere
2. **Extract EIP into provider interface** (2 hours) — add optional `StaticIPManager` interface
3. **Make user-data a template** (2 hours) — separate the bootstrap script from the provider, pass it as a parameter
4. **Add BYOR provisioner** (1 day) — a no-op provisioner that returns user-configured endpoints
5. **Add shared pool provisioner** (1 week) — Hetzner or generic VPS pool management

---

## Summary Recommendations

**Short term (1 week):** Implement BYOR mode. Low effort, enables a free tier, validates the abstraction layer. Users who already have Belabox or self-hosted SRTLA servers can use Telemy immediately.

**Medium term (2-3 weeks):** Add Hetzner shared relay provisioner. Dramatically reduces cost per user. Keep AWS as a premium/dedicated option.

**Long term:** Hybrid model with BYOR (free) + shared managed (cheap) + dedicated managed (premium). The entitlement system already supports this — it just needs the relay mode dimension added.

The codebase is well-positioned for this evolution. The `Provisioner` interface is the right abstraction, and the plugin is already fully provider-agnostic. The main work is on the control plane side: renaming AWS-specific fields, extracting EIP management, and building the shared relay pool concept.
