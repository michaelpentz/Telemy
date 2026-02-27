# v0.0.3 Handoff Archive (Backend + Client/App)

This file is the durable, append-only handoff log. It includes superseded snapshots for audit/history, but the current state is summarized here first.

## Current Snapshot (2026-02-23, US/Pacific)

### Project status

- Backend/cloud is materially ahead and live-validated in AWS mode.
- Rust app now has live-validated Aegis control-plane client plumbing and temporary local dashboard controls for validation.
- Browser dashboard remains transitional; v0.0.3 target UX is still hybrid OBS plugin + Rust core via local IPC named pipes.

### Current implementation priority

1. Hybrid plugin path first (OBS plugin + Rust core IPC).
2. Rust IPC v1 foundations (`hello`, `ping`, `request_status`, `status_snapshot`, `user_notice`) and local server stub.
3. Plugin-facing status snapshot terminology aligned with `docs/STATE_MACHINE_v1.md`.
4. Browser dashboard changes kept minimal/transitional.

### Latest validated runtime notes

- Aegis control-plane host (after reboot/hardening): `http://52.13.2.122:8080` (EC2 public IP `52.13.2.122` as of 2026-02-22 US/Pacific).
- Timeout mitigation for `POST /relay/start` was deployed and revalidated.
- Access hardening and recovery paths (SSM + SSH) were validated.
- Local client path validation:
  - fresh control-plane JWT minted for `uid=usr_ec2_validation`
  - stored in Telemy vault key `aegis_cp_access_jwt`
  - `aegis-relay-active` returned `null` during validation (auth/path valid, no active relay at test time)

### Known small follow-up (not blocking current client/plugin work)

- API `relay.public_ip` may still include `/32` suffix; client currently normalizes this.

## How To Use This File

- Read the latest addenda first (most recent dates).
- Treat older sections before the addenda as historical snapshots that may be superseded.
- Canonical specs remain in `docs/` (`API_SPEC_v1.md`, `DB_SCHEMA_v1.md`, `STATE_MACHINE_v1.md`, `IPC_PROTOCOL_v1.md`).

## Latest Addenda (Read First)

- `Dock Runtime Regression Recovery (Scenes/Toggle/Theme/Title) Addendum (2026-02-26, US/Pacific)`
- `Dock Action Status-Lifecycle + Mode/Setting Mapping Sync Addendum (2026-02-27, US/Pacific)`
- `OBS/CEF JS Sink Validation + Asset Staging Follow-Up Addendum (2026-02-24, US/Pacific)`
- `OBS/CEF Dock Host Runtime Bring-Up + Shutdown Stabilization Addendum (2026-02-24, US/Pacific)`
- `Scaffold-Mode Real OBS Validation + Qt/WebEngine Runtime Blocker Addendum (2026-02-24, US/Pacific)`
- `Dock Bridge + Shim Callback/Scene-Switch Integration Addendum (2026-02-23, US/Pacific)`
- `Aegis Dock UX Reference Prototype Addendum (2026-02-23, US/Pacific)`
- `IPC Foundations + C++ Shim Harness Validation Addendum (2026-02-23, US/Pacific)`
- `Client/Aegis Integration + Temporary Dashboard Clarification Addendum (2026-02-23, US/Pacific)`
- `Post-Timeout-Mitigation + Access Hardening Addendum (2026-02-22, US/Pacific)`
- `Post-Audit Execution Addendum (2026-02-22, US/Pacific)`
- `Deep Audit Addendum (2026-02-22, US/Pacific)`

## Historical Baseline Snapshot (Superseded by Later Addenda)

The sections below are retained for audit/history and may describe temporary or dev states that were later replaced.

## Completed

1. Specs and architecture baseline
- `docs/API_SPEC_v1.md`
- `docs/IPC_PROTOCOL_v1.md`
- `docs/STATE_MACHINE_v1.md`
- `docs/DB_SCHEMA_v1.md`
- `docs/archive/ARCHITECTURE-v0.0.3-FIRST-PASS.md`

2. Control-plane implementation
- API handlers and routing in `aegis-control-plane/internal/api`
- Store/auth/config/model layers in `aegis-control-plane/internal/*`
- Migrations in `aegis-control-plane/migrations`

3. Relay lifecycle and providers
- `POST /api/v1/relay/start`, `GET /relay/active`, `POST /relay/stop`, `GET /relay/manifest`, `POST /relay/health`
- Providers: `fake` and `aws` (EC2 run/terminate)
- Manifest now sourced from persisted DB table (`relay_manifests`)

4. Operational jobs and reliability hardening
- In-process jobs: idempotency TTL cleanup, usage rollup, outage reconciliation
- Structured latency metrics logs for provision/deprovision and jobs
- AWS transient retry/backoff policy for run/terminate operations

5. Tests
- API path tests (start/stop/manifest behavior)
- Relay AWS error classification and retry-related tests
- Store transaction and operational SQL tests (`pgxmock`)

## Validation Snapshot

- `go test ./...` in `aegis-control-plane`: passing
- `gofmt` on touched Go files: applied

## Deployment Status (EC2 Manual Bring-Up)

- Date: 2026-02-22 (US/Pacific)
- EC2 instance `i-07a5f4a42ef4c39f9` recovered and reachable after stop/start (public IP changed during restart cycle).
- AWS backup access paths configured and validated:
  - SSH key-based access (persistent `authorized_keys`)
  - SSM Session Manager (`PingStatus=Online`)
- PostgreSQL 16 installed on the EC2 instance (local dev/test DB for control-plane bring-up)
  - local DB created: `aegis`
  - migrations applied: `0001_init.sql`, `0002_relay_manifest.sql`
- `aegis-control-plane` deployed to EC2 in service mode (prebuilt Linux binaries uploaded)
  - API binary: `/opt/aegis/bin/aegis-api`
  - Jobs binary: `/opt/aegis/bin/aegis-jobs`
  - env file: `/etc/aegis-control-plane.env`
  - services: `aegis-api.service`, `aegis-jobs.service`
- Runtime verification completed:
  - `GET /healthz` returns `{"status":"ok"}`
  - `GET /metrics` responds (Prometheus exposition)
  - jobs worker executes scheduled loops successfully (idempotency cleanup, usage rollup, outage reconciliation)

## Historical Runtime Mode (Superseded / Temporary Dev Snapshot)

- `AEGIS_RELAY_PROVIDER=fake`
- Local PostgreSQL on same EC2 host
- Dev credentials/secrets currently in env file (`/etc/aegis-control-plane.env`)
- PostgreSQL auth currently uses `postgres` user/password for bootstrap/testing

## Historical Important Env Vars

Required:
- `AEGIS_DATABASE_URL`
- `AEGIS_JWT_SECRET`
- `AEGIS_RELAY_SHARED_KEY`

Provider-specific:
- `AEGIS_RELAY_PROVIDER=fake|aws`
- `AEGIS_AWS_AMI_MAP=us-east-1=ami-...,eu-west-1=ami-...`
- `AEGIS_AWS_INSTANCE_TYPE`, `AEGIS_AWS_SUBNET_ID`, `AEGIS_AWS_SECURITY_GROUP_IDS`, `AEGIS_AWS_KEY_NAME`

## Historical Next Recommended Tasks (Superseded)

1. Replace dev secrets in `/etc/aegis-control-plane.env` (`AEGIS_JWT_SECRET`, `AEGIS_RELAY_SHARED_KEY`).
2. Create a dedicated PostgreSQL role/database for Aegis and update `AEGIS_DATABASE_URL`.
3. Move from `AEGIS_RELAY_PROVIDER=fake` to `aws` with AMI/subnet/security-group/key envs (`AEGIS_AWS_*`).
4. Lock down EC2 security group ingress to required ports/source IPs only.
5. Remove any temporary IAM inline policies used during setup (keep only least-privilege backup access).

## Deep Audit Addendum (2026-02-22, US/Pacific)

Scope audited:
- `aegis-control-plane` (Go control plane / EC2 prototype path)
- `obs-telemetry-bridge` (Rust local OBS telemetry app)
- `docs/*` specs and operational docs
- local packaged artifact note: `tmp/aegis-control-plane-v003-ec2.tar.gz` (contents inspection only)

### Validation Performed (Local)

- Go:
  - `go test ./...` in `aegis-control-plane`: passing
  - `go build ./...` in `aegis-control-plane`: passing
  - `go test -cover ./...`: not usable on this machine due local Go coverage toolchain issue (`covdata` missing)
- Rust:
  - `cargo test` in `obs-telemetry-bridge`: passing
  - `cargo build --release` in `obs-telemetry-bridge`: passing
  - `cargo clippy --all-targets --all-features -- -D warnings`: passing

### Key Audit Findings (Prioritized)

1. High: `POST /api/v1/relay/start` failure paths can leave stuck `provisioning` sessions and orphan relay instances
- Session is committed before provisioning; provisioning/activation failures return `500` without compensating cleanup.
- This can block user retries (existing `provisioning` session returned) and leak EC2 instances on partial failure.

2. High: `POST /api/v1/relay/health` maps backend/store failures to `400 invalid_request`
- Operational failures (DB/runtime) are surfaced as client payload errors, which obscures outages and misleads callers.

3. High: EC2 deployment snapshot is still explicitly temporary/dev
- Current documented runtime mode is `AEGIS_RELAY_PROVIDER=fake`.
- Dev secrets are stored in `/etc/aegis-control-plane.env`.
- PostgreSQL bootstrap creds are documented as `postgres` user/password.

4. High: `fake` provider bring-up can still produce empty relay manifest (`/relay/manifest` => `503`)
- Manifest seeding currently depends on `AEGIS_AWS_AMI_MAP`; the README fake-mode quickstart does not include it.

5. Medium: Spec/code drift in control-plane API security contract
- Spec requires HTTPS + client version/platform headers + rate limits.
- Current implementation validates JWT only and does not enforce the documented headers/rate limits.

6. Medium: `cmd/jobs` has no metrics HTTP listener, but ops docs describe scraping a jobs endpoint
- Jobs process currently runs worker loops only; no `/metrics` server is implemented in `cmd/jobs`.

7. Medium: Rust local server token is accepted via query string for all routes (including POST routes)
- Tokenized URLs are printed to console and query-token fallback is accepted broadly.
- This is convenient for local use but weak for mutation endpoints (settings/output names/dashboard import).

8. Medium: Rust config loader silently ignores malformed `config.toml`
- Parse failures fall back to defaults without error/logging, making config corruption harder to diagnose.

9. Medium: Rust Windows autostart registry value uses unquoted exe path
- Can break startup on paths containing spaces.

10. Medium: Non-Windows vault fallback is plaintext (base64 only, not encrypted)
- Added for portability/server scenarios, but secrets are not protected at rest on non-Windows hosts.

11. Low: Plugin work in v0.0.3 appears spec-only in this workspace snapshot
- `docs/IPC_PROTOCOL_v1.md` defines plugin/core IPC, but no separate OBS plugin implementation directory is present.
- Current v0.0.3 code on this PC is Rust local app + Go backend + docs.

12. Low: Packaged EC2 source artifact appears to differ from current workspace snapshot
- Tarball contents include `internal/usage/` not present in current `aegis-control-plane` workspace tree.
- Treat packaged artifact and local tree as separate audit subjects until reconciled.

### Notes / Clarifications

- The above findings are a code-and-doc audit of the local `telemy-v0.0.3` workspace plus archived EC2 handoff notes.
- No live EC2 verification was performed during this audit pass.
- The "prototype OBS plugin" is not implemented in this workspace snapshot; plugin behavior is specified in docs (`docs/IPC_PROTOCOL_v1.md`) only.

### Recommended Follow-Up (Audit-Driven)

1. Add compensating cleanup for `relay/start` failures (session fail/stop transition + deprovision on activation failure) and tests.
2. Decide fake-mode manifest behavior and document/implement manifest seeding independent of AWS AMI env if needed.
3. Replace dev secrets and bootstrap DB credentials on EC2 before any broader testing.
4. Reconcile docs/spec vs implementation (HTTPS/headers/rate limits/jobs metrics endpoint) or mark spec items as future work.
5. Tighten Rust local auth handling (restrict query-token fallback to GET UI routes; prefer header-only for POST/WS).

## Post-Audit Execution Addendum (2026-02-22, US/Pacific)

This addendum supersedes the earlier "Current Runtime Mode (Temporary / Dev)" and EC2 secret/bootstrap-credential notes above.

### Completed Since Deep Audit

Code / docs follow-up:
- Completed audit item 1: added `relay/start` compensating cleanup (best-effort stop on provisioning failure; deprovision+stop on post-provision failures) with handler tests.
- Completed audit item 2: fake-mode relay manifest seeding now works without `AEGIS_AWS_AMI_MAP` (placeholder `ami-fake-<region>` entries) and docs updated.
- Completed audit item 4 (docs reconciliation): `docs/API_SPEC_v1.md` and `docs/OPERATIONS_METRICS.md` now explicitly mark current implementation gaps vs target contract.
- Completed audit item 5 (Rust local auth tightening): query-token fallback restricted; POST routes and `/output-names` GET require bearer header. WebSocket query-token fallback remains for browser compatibility.
- Additional audit fix completed: `POST /api/v1/relay/health` now classifies backend/store failures as `500` and only state-level relay-health rejection as `400`.

EC2 hardening / operations:
- Replaced dev secrets in `/etc/aegis-control-plane.env` (`AEGIS_JWT_SECRET`, `AEGIS_RELAY_SHARED_KEY`).
- Replaced PostgreSQL bootstrap app access with dedicated DB role `aegis` + new password.
- `AEGIS_DATABASE_URL` now uses the `aegis` role.
- Verified `aegis-api` and `aegis-jobs` restarted healthy after credential rotation.

AWS relay cutover prep + execution:
- Created relay SG in `us-west-2`: `sg-0da8cf50c2fd72518` (`aegis-relay-sg`)
  - ingress `udp/9000` (SRT)
  - ingress `tcp/7443` (relay WS/TLS)
- Switched EC2 control-plane runtime from `AEGIS_RELAY_PROVIDER=fake` to `AEGIS_RELAY_PROVIDER=aws`.
- Configured `AEGIS_AWS_AMI_MAP=us-west-2=ami-075b5421f670d735c`.
- Configured `AEGIS_AWS_INSTANCE_TYPE=t3.small` (AMI is `x86_64`, so `t4g.small`/arm64 is incompatible).
- Configured AWS subnet/SG/key envs:
  - `AEGIS_AWS_SUBNET_ID=subnet-0eb3c4369ad5dfc1a`
  - `AEGIS_AWS_SECURITY_GROUP_IDS=sg-0da8cf50c2fd72518`
  - `AEGIS_AWS_KEY_NAME=aegis-relay-key`
- Granted missing AWS permissions:
  - operator user `telemy-cli`: SG create/manage (for cutover prep)
  - host role `TelemyEc2SsmRole`: EC2 launch/terminate/describe for relay provisioning

### Validation Outcome (Live EC2, us-west-2)

- AWS relay provisioning path is functional:
  - relay instance launched successfully (`i-053fa5dd3778334d0`)
  - session activated in DB
- AWS relay deprovision path is functional:
  - `POST /api/v1/relay/stop` returned `200`
  - DB relay state -> `terminated`
  - EC2 instance entered `shutting-down`

### New Issue Found During Validation

High:
- `POST /api/v1/relay/start` can exceed current API HTTP timeouts during EC2 provisioning.
- Client observed `curl: (52) Empty reply from server`, but provisioning completed and session became `active`.
- Root cause is likely timeout mismatch (`cmd/api` `WriteTimeout` and chi middleware timeout too short for EC2 launch/wait).

Mitigation landed in workspace code (deployment pending):
- Increased chi middleware timeout to `3m`.
- Increased API server `WriteTimeout` to `3m` (`ReadTimeout` to `30s`).

### Current EC2 Runtime Snapshot (After Cutover)

- Region: `us-west-2`
- Control-plane instance: `i-07a5f4a42ef4c39f9` (`Aegis`)
- `AEGIS_RELAY_PROVIDER=aws`
- Secrets: rotated (no longer dev defaults)
- DB auth: dedicated `aegis` role (no longer `postgres:postgres` for app runtime)

### Recommended Next Steps (Current)

1. Deploy the timeout mitigation build to EC2 (`aegis-api`) and re-run live `relay/start` validation to confirm clean client response.
2. Clean up stale `provisioning` sessions created during pre-IAM/timeout validation attempts (if not kept for debugging).
3. Perform a short repeated start/stop soak test (3-5 cycles) in AWS mode.
4. Continue client/plugin v0.0.3 overhaul before broad beta testing (backend is ahead of client maturity).

## Post-Timeout-Mitigation + Access Hardening Addendum (2026-02-22, US/Pacific)

This addendum supersedes the prior "Recommended Next Steps (Current)" items 1-2 in the immediately preceding section.

### Completed Since Prior Addendum

Timeout mitigation deployment + validation:
- Deployed timeout-mitigation `aegis-api` build to EC2 host (`/opt/aegis/bin/aegis-api`) after API timeout patch landed in workspace code.
- Confirmed `aegis-api` service restarted healthy and `GET /healthz` returns `{"status":"ok"}`.
- Re-ran live AWS-mode validation with existing user (`uid=usr_ec2_validation`):
  - `POST /api/v1/relay/start` returned `201` with active session response (no empty reply)
  - `POST /api/v1/relay/stop` returned `200`
- Timeout symptom is resolved:
  - previous behavior: `curl: (52) Empty reply from server` during EC2 provisioning
  - current behavior: client receives normal JSON response (success or error)

Validation clarification:
- A validation attempt using a non-existent JWT `uid` (`ops-validation`) returned a proper JSON `500` (`failed to start relay session`).
- This was a test-user/data issue (user missing from `users` table), not an AWS relay provisioning regression.

DB cleanup:
- Cleaned stale `sessions.status='provisioning'` rows created during earlier failed validations where `relay_instance_id` was null.
- Updated 2 stale rows to `stopped`; remaining `provisioning` sessions after cleanup: `0` (at cleanup time).

Access hardening / operator recovery improvements:
- `telemy-cli` IAM permissions expanded and validated for incident recovery:
  - `ssm:CancelCommand` (used to cancel stuck RunCommand)
  - `ssm:DescribeInstanceInformation`
  - `ec2:RebootInstances`, `ec2:StopInstances`, `ec2:StartInstances` (dry-run validated)
  - `ec2-instance-connect:SendSSHPublicKey`
  - IAM self-read (`iam:ListUserPolicies`, `iam:GetUserPolicy`, `iam:ListAttachedUserPolicies`)
- Local operator machine prepared with Session Manager Plugin and helper scripts (`C:\Users\mpent\Documents\telemy-tools\*`).
- Host-side access hardening applied after reboot:
  - `amazon-ssm-agent` enabled with service restart policy (`Restart=always`)
  - `sshd` enabled
  - key-only SSH config drop-in
  - persistent `ec2-user` `authorized_keys` entries added for local operator keys
- Verified direct SSH login to host as `ec2-user` using local key `id_server_new`.

### Current EC2 Runtime Snapshot (After Timeout Fix + Access Hardening)

- Region: `us-west-2`
- Control-plane instance: `i-07a5f4a42ef4c39f9` (`Aegis`)
- Current public IP after reboot: `52.13.2.122` (previous `52.36.68.54` is stale)
- `AEGIS_RELAY_PROVIDER=aws`
- Secrets: rotated
- DB auth: dedicated `aegis` role
- Access paths (validated):
  - SSM Session Manager (with improved recovery permissions)
  - Direct SSH (`ec2-user`, key-based)
  - EC2 Instance Connect temp-key path (operator-side tooling prepared)

### Remaining / Newly Observed Items

1. Low/Medium: API response currently returns relay public IP with CIDR suffix (`/32`)
- Example observed response field during successful validation: `"public_ip":"18.236.185.240/32"`.
- Likely DB `inet` serialization formatting leaking into API response contract.
- Not blocking relay lifecycle validation, but client-facing cleanup is recommended.

2. Operational follow-up: SSM agent stability should be re-observed after hardening
- Host experienced repeated `SSM PingStatus=ConnectionLost` during prior deploy attempts.
- Reboot + agent hardening stabilized current session, but short monitoring/soak is recommended.

### Recommended Next Steps (Updated)

1. Perform short AWS-mode relay start/stop soak test (3-5 cycles) now that timeout mitigation is deployed and access recovery is hardened.
2. Fix API `relay.public_ip` serialization to return bare IP (no `/32`) and add regression coverage if contract requires plain IP string.
3. Continue v0.0.3 client/plugin/app overhaul (primary project focus): backend/cloud is materially ahead of client maturity.
4. Continue docs condense/archive cleanup pass and refresh handoff docs at next stopping point.

## Client/Aegis Integration + Temporary Dashboard Clarification Addendum (2026-02-23, US/Pacific)

This addendum captures post-backend-handoff client-side progress in `obs-telemetry-bridge` and clarifies current UI work vs the documented v0.0.3 target architecture.

### Completed Since Prior Addendum (Client/App Side)

Rust client/Aegis control-plane plumbing (new in `obs-telemetry-bridge`):
- Added typed Aegis control-plane client module for:
  - `GET /api/v1/relay/active`
  - `POST /api/v1/relay/start`
  - `POST /api/v1/relay/stop`
- Enforces/spec-aligns common client headers (`Authorization`, `X-Aegis-Client-Version`, `X-Aegis-Client-Platform`) and `Idempotency-Key` for relay start.
- Added client-side normalization workaround for current backend `relay.public_ip` `/32` formatting leak (strips CIDR suffix when parsing response).
- Added unit tests for request headers, `relay/start` request body, `relay/active` `204` handling, and `public_ip` normalization.

Rust app config + startup integration:
- Added minimal `[aegis]` config section to Rust app config schema:
  - `enabled`
  - `base_url`
  - `access_jwt_key`
- Added env overrides:
  - `TELEMY_AEGIS_ENABLED`
  - `TELEMY_AEGIS_BASE_URL`
  - `TELEMY_AEGIS_ACCESS_JWT_KEY`
- Added config validation when Aegis is enabled (base URL + vault key required).
- Added startup reconnect-first probe (`relay/active`) that caches the latest Aegis relay session snapshot in memory.

Operator local setup + live validation (client path):
- Minted fresh `cp_access_jwt` for existing validation user `uid=usr_ec2_validation` on Aegis EC2 host (using current `AEGIS_JWT_SECRET`) and stored locally in Telemy vault key `aegis_cp_access_jwt`.
- Created local `obs-telemetry-bridge/config.toml` from `config.example.toml` and added `[aegis]` config pointing to current Aegis host (`http://52.13.2.122:8080`) with `access_jwt_key = "aegis_cp_access_jwt"`.
- Live Rust client validation succeeded:
  - `aegis-relay-active` returns `null` (auth/config/backend path works; no active relay for that user at test time).

Temporary local dashboard control-surface upgrades (transitional only):
- Added local Aegis status endpoint (`GET /aegis/status`) using cached snapshot with optional backend refresh (`?refresh=1`).
- Added local Aegis relay action endpoints:
  - `POST /aegis/start`
  - `POST /aegis/stop`
- Added temporary dashboard controls/badge:
  - Aegis status badge + refresh
  - Manual `Aegis Start` / `Aegis Stop` buttons (calls local endpoints which call backend via Rust client)
- Added a summary-first dock layout pass:
  - always-visible summary (connection/system/main stream+encoder)
  - expandable diagnostics section
  - expandable outputs section
- Note: current telemetry model still does not expose VRAM fields; UI currently shows `VRAM: n/a` placeholder.

### Important Clarification (Architecture Target vs Current UI)

Current state:
- The browser-based local dashboard/OBS dock page is still being used as a temporary validation and control surface.
- It remains structurally rooted in the v0.0.2-era local dashboard code path (even with incremental layout and Aegis control additions).

v0.0.3 target (still intended / not yet implemented):
- Hybrid OBS plugin + Rust core architecture (not browser-page-as-final-UX).
- OBS plugin provides dock UI + OBS-native integration/hooks.
- Rust core owns telemetry collection, Aegis/cloud control, state machine, and automation.
- Plugin <-> core communication uses local IPC (named pipes, MessagePack) per `docs/IPC_PROTOCOL_v1.md`.

Reason for current approach:
- Client/Aegis plumbing and live backend integration were validated incrementally using the existing local dashboard path to avoid blocking on plugin IPC implementation before cloud-path validation.
- This was a deliberate transition step, not a final UI/architecture decision.

### Current Client/App Snapshot (After This Addendum)

- Backend/cloud remains materially ahead and live-validated in AWS mode.
- Rust app now has live-validated Aegis control-plane client plumbing and manual relay control path.
- Browser dashboard has temporary Aegis status/control additions and a more compact summary+details layout.
- Full hybrid plugin path (IPC server/runtime, plugin shim integration, plugin-facing status model, auto-scene coordination) remains the primary unfinished v0.0.3 client/plugin milestone.

### Recommended Next Steps (Client-Focused, Updated)

1. Re-center implementation on documented hybrid plugin target:
- Add Rust-side IPC v1 message/envelope types aligned to `docs/IPC_PROTOCOL_v1.md`.
- Implement initial local IPC server stub (`hello`, `ping`, `request_status`, `status_snapshot`) in Rust core.

2. Define/emit a v0.0.3 plugin-facing status snapshot model:
- Use v0.0.3 terminology (`STUDIO`, `IRL_CONNECTING`, `IRL_ACTIVE`, `IRL_GRACE`, etc.) from `docs/STATE_MACHINE_v1.md`.
- Keep browser dashboard as transitional fallback/debug surface only.

3. Continue client-side telemetry/schema improvements needed by dock UX:
- Add VRAM telemetry fields to Rust `SystemFrame` + collectors if feasible.
- Improve per-destination output naming/status mapping for compact dock tiles.

4. Optional backend cleanup (still small, not blocking current client plumbing):
- Fix API `relay.public_ip` serialization to return bare IP (remove `/32`) and keep regression coverage.

## IPC Foundations + C++ Shim Harness Validation Addendum (2026-02-23, US/Pacific)

This addendum captures the plugin/core IPC milestone reached after the temporary dashboard/Aegis integration work.

### Completed Since Prior Addendum (Client/Plugin Path)

Rust core IPC implementation (`obs-telemetry-bridge`):
- Implemented named-pipe IPC server foundation with MessagePack envelopes and length-prefix framing.
- Implemented/handled IPC v1 messages:
  - plugin -> core: `hello`, `ping`, `request_status`, `scene_switch_result`, `obs_shutdown_notice`
  - core -> plugin: `hello_ack`, `pong`, `status_snapshot`, `protocol_error`, `user_notice`, `switch_scene`
- Added heartbeat timeout handling, repeated-protocol-error reset behavior, and pending `switch_scene` request tracking.
- Added extensive duplex-based unit tests covering handshake, heartbeat, status snapshots, invalid payloads, unknown types, reset threshold, and `switch_scene` ack/timeout paths.

Rust app/server integration (transitional debug path only):
- Added local debug endpoint `POST /ipc/switch-scene` (token-protected) to queue core `switch_scene` commands.
- Added local debug endpoint `GET /ipc/status` and `/obs` dashboard IPC status line for connection/pending/last-result visibility.
- Added temporary `/obs` page controls to trigger `IPC Switch Scene` for validation.

C++ plugin shim scaffold (`obs-plugin-shim/`):
- Added minimal CMake project + standalone harness executable (`aegis_plugin_shim_harness.exe`).
- Added shim runtime and IPC worker thread skeleton (non-OBS harness mode + OBS plugin entry skeleton).
- Implemented initial named-pipe connect/reconnect loop and minimal MessagePack protocol support for:
  - `hello`
  - `request_status`
  - `ping`
  - `scene_switch_result(ok)` auto-ack on incoming `switch_scene`

### Validation Outcome (Local Windows, Named Pipes)

Validated with live Rust core + dashboard + C++ harness:
- C++ harness connected to Rust core named pipes.
- Rust core emitted and C++ harness received:
  - `hello_ack`
  - `status_snapshot` (periodic)
  - `pong`
  - `switch_scene` (triggered via `/obs` `IPC Switch Scene` button)
- C++ harness auto-acked `switch_scene` with `scene_switch_result(ok)`.
- Dashboard displayed successful queueing, and core-side IPC debug status path remained functional.

This is the first end-to-end validation of the hybrid plugin/core IPC path with a C++ plugin-side stand-in (beyond Rust-only test clients).

### Current Priority (Updated)

1. Move from harness to real OBS plugin lifecycle integration while keeping the validated `IpcClient` path.
2. Replace shim auto-ack behavior with real OBS scene-switch execution + success/failure reporting.
3. Keep browser dashboard changes minimal and validation-focused until OBS plugin path is sufficient.

### Known Follow-Up (Current)

- C++ shim IPC parser/encoder is intentionally minimal and only covers current bring-up messages; full protocol coverage and hardened parsing are still needed.
- Shim currently uses blocking I/O on a background thread; spec-target overlapped I/O and explicit timeout policy remain to be implemented.

## Aegis Dock UX Reference Prototype Addendum (2026-02-23, US/Pacific)

This addendum records a repo-local UI reference prototype for the intended OBS plugin dock look/flow and captures the implementation implications for the hybrid plugin/core path.

### Artifact Added / Reviewed

- Repo-root UI prototype: `aegis-dock.jsx`
- Purpose: idealized operator-facing dock look, feel, and interaction flow for Aegis in the OBS plugin context (reference only; not production plugin code)

### What The Prototype Clarifies (Useful for Plugin Work)

- Operator-first information hierarchy:
  1. live status banner (live indicator, elapsed, bonded bitrate)
  2. scene switcher
  3. connections + bitrate monitoring
  4. cloud relay (IRL mode)
  5. failover engine state
  6. quick settings
  7. event log
- Target dock style direction:
  - compact, mono, instrumentation-like visual language
  - collapsible sections for dense operational detail without losing top-level status
  - clear status coloring for healthy/degraded/disconnected/active states

### Important Implementation Clarification

- `aegis-dock.jsx` is currently a mock/simulated UI:
  - local scene selection state (no OBS scene execution)
  - local toggle state (no persisted config writes)
  - simulated bitrate fluctuation and elapsed timer
  - static relay/failover/event values
- It should be treated as a target information architecture and state-shape reference, not as the shipping UI implementation path.

### Implied Plugin/Core Data Surfaces (Draft)

The prototype usefully implies a plugin-facing dock state contract that should be mapped to Rust core IPC messages/status snapshots:

- Scene state:
  - available scenes
  - active scene
  - queued/pending scene switch request
  - last `scene_switch_result` success/failure + reason
- IPC / plugin-core health:
  - pipe connection state
  - heartbeat freshness / latency
  - protocol error count (if surfaced)
- Connection telemetry:
  - per-link label/type/status
  - signal strength indicator
  - instantaneous bitrate
  - bonded/aggregate bitrate
  - thresholds used by failover decisions
- Relay/cloud state (IRL mode):
  - relay active/inactive
  - region/instance class
  - latency
  - uptime
  - time-bank remaining/percent
- Failover engine:
  - current state machine state (`S0..S3` / mapped v0.0.3 terms)
  - current health classification
  - last failover transition
  - response-time budget/status
  - session failover counts
- Settings and events:
  - effective toggle states (auto-switch, low-quality fallback, alerts, etc.)
  - recent event log entries with severity/time

### Current Recommended Follow-Up (Non-Blocking, Plugin-Path Aligned)

1. Define a concrete plugin-facing `DockState` shape (or equivalent IPC snapshot payload) that covers the prototype sections above.
2. Map each prototype section to:
   - existing IPC messages/status fields (already available)
   - missing IPC/status fields (to add next)
3. Keep browser dashboard work minimal/transitional; use the prototype as UX reference while implementing real OBS plugin dock lifecycle/hooks.

## Dock Bridge + Shim Callback/Scene-Switch Integration Addendum (2026-02-23, US/Pacific)

This addendum captures the follow-on work after the dock UX reference + mapping docs: a bridge host/reducer for the dock UI and concrete plugin shim callback plumbing toward real OBS scene switching.

### Completed Since Prior Addendum (Dock/UI Bridge)

Repo-root dock UI (`aegis-dock.jsx`):
- Refactored prototype to support both:
  - mock/simulated mode (default fallback)
  - controlled mode via external `dockState` + `onAction`
- Exported `DEFAULT_DOCK_STATE` baseline shape.
- Added `AegisDockBridge` wrapper (polling/subscription bridge consumer) and `createWindowAegisDockBridge(...)` adapter.
- Added pending scene UI state support (`pendingSceneId`) and disabled conflicting scene clicks while pending (controlled mode).
- Added aggregate-bitrate fallback display path (`bitrate.bondedKbps`) so IPC v1 aggregate throughput renders even when per-link telemetry is not yet available.
- Added explicit placeholder text for missing per-link telemetry ("aggregate bitrate only from IPC v1").

Repo-root bridge host/reducer (`aegis-dock-bridge.js`):
- Added bridge contract implementation:
  - `getState()`
  - `sendAction(action)`
  - `subscribe(listener)`
- Added IPC envelope reducers for:
  - `hello_ack`
  - `pong`
  - `status_snapshot`
  - `switch_scene`
  - `user_notice`
  - `protocol_error`
- Added `projectDockState(cache)` projection aligned with `docs/IPC_PROTOCOL_v1.md` section 10.
- Added plugin-local feed helpers for near-term OBS integration:
  - `setObsScenes(...)`
  - `setObsSceneNames(...)`
  - `setObsActiveScene(...)`
  - `setObsActiveSceneName(...)`
  - `setConnectionTelemetry(...)`
  - `setBitrateThresholds(...)`
  - `setLiveInfo(...)`
  - `setSettings(...)`
- Added manual dock scene-switch pending behavior:
  - UI `switch_scene` actions now generate local request ids and immediately set pending scene state
  - `notifySceneSwitchCompleted(...)` convenience helper clears pending state and logs success/failure
- Added `attachAegisDockBridgeToWindow(...)` helper and richer example wiring snippet for browser/host integration.

Docs/spec alignment:
- Added `DockState` projection and action mapping section to `docs/IPC_PROTOCOL_v1.md` (section 10), including `Available` / `Derived` / `Gap` status for UI fields and recommended future IPC additions.

### Completed Since Prior Addendum (C++ Shim / OBS Plugin Path)

`obs-plugin-shim` IPC client and runtime:
- Added `IpcClient` callback hooks:
  - pipe state (`on_pipe_state`)
  - message type (`on_message_type`)
  - parsed `switch_scene` request callback (`request_id`, `scene_name`, `reason`)
- Extended minimal MessagePack parsing to extract `scene_name` + `reason` from core `switch_scene` payloads.
- Added `SetAutoAckSwitchScene(bool)` with default `true` to preserve harness behavior while enabling real plugin control flow.
- Added explicit queued `scene_switch_result` reporting path (thread-safe queue -> IPC worker drain/send):
  - supports success and failure (`ok=false`, error string)
- Added explicit queued `obs_shutdown_notice` reporting path (thread-safe queue -> IPC worker drain/send).
- Surfaced new APIs through `ShimRuntime`:
  - `SetIpcCallbacks(...)`
  - `SetAutoAckSwitchScene(...)`
  - `QueueSceneSwitchResult(...)`
  - `QueueObsShutdownNotice(...)`

`obs-plugin-shim` OBS plugin entry (`src/obs_plugin_entry.cpp`):
- Disabled auto-ack in plugin mode (`SetAutoAckSwitchScene(false)`).
- Registered IPC callbacks in `obs_module_load`.
- Added thread-safe queue for incoming `switch_scene` requests from IPC callbacks.
- Added OBS timer pump (`timer_add`, 50ms) to drain queued switch requests on OBS-side callback path instead of IPC worker callback thread.
- Added best-effort real OBS scene switch attempt in timer path:
  - `obs_get_source_by_name(scene_name)`
  - `obs_frontend_set_current_scene(scene_source)`
  - explicit `scene_switch_result(ok=true)` on best-effort success
  - explicit error results for missing scene name / scene not found
- Added best-effort graceful unload signaling:
  - queue `obs_shutdown_notice("obs_module_unload")`
  - short delay before stopping IPC worker
  - remove timer and clear pending queue on unload

### Validation / Build Status (This Pass)

- Rebuilt `obs-plugin-shim` core + harness targets multiple times during integration changes:
  - `cmake --build build --config Debug` (passing)
- Current warnings unchanged and non-blocking for this pass:
  - `WIN32_LEAN_AND_MEAN` / `NOMINMAX` macro redefinition warnings in `ipc_client.cpp`
- OBS plugin module target (`AEGIS_BUILD_OBS_PLUGIN=ON`) was not built/validated in this pass (OBS SDK include/lib path dependency).

### Current State (After This Addendum)

- Dock UI/bridge side has enough infrastructure to render live-ish state from:
  - current IPC v1 snapshots/events
  - plugin-local OBS scene and stream state
  - plugin-local connection telemetry (when available)
- C++ shim no longer depends solely on `switch_scene` auto-ack behavior:
  - it can callback, queue work to OBS-side processing, execute a scene switch attempt, and explicitly report result.
- Remaining plugin-path work is now concentrated in OBS integration details (scene inventory/current scene feed, build validation against OBS SDK, and runtime verification), rather than protocol scaffolding.

### Current Recommended Next Steps (Plugin/Bridge Integration)

1. Wire OBS scene inventory + active scene callbacks into the dock bridge:
   - feed `setObsSceneNames(...)`
   - feed `setObsActiveSceneName(...)`
2. Validate OBS plugin module build path with local OBS SDK (`AEGIS_BUILD_OBS_PLUGIN=ON`) and fix any API/header/link mismatches.
3. Add active-scene verification after `obs_frontend_set_current_scene(...)` before returning `scene_switch_result(ok=true)` (or return failure if verification misses).
4. Feed plugin-local stream status/elapsed and (when available) network link telemetry into bridge helpers:
   - `setLiveInfo(...)`
   - `setConnectionTelemetry(...)`
   - `setBitrateThresholds(...)`
5. Keep browser dashboard changes minimal; use the dock bridge + plugin path as the primary forward implementation track.

## OBS 32.0.4 Plugin Build + Real Callback-Mode Scene-Switch Validation Addendum (2026-02-24, US/Pacific)

This addendum captures the next plugin/core validation pass after the prior IPC + shim integration work: real OBS callback-mode `switch_scene_result` behavior was exercised end-to-end against the Rust IPC server/debug dashboard, including negative-path cases.

### Completed Since Prior Addendum (Plugin/Core Validation + Debug Path)

Rust core / debug surface (`obs-telemetry-bridge`):
- Confirmed Rust IPC v1 tests still pass (`cargo test`) after plugin/debug iteration changes.
- Added a debug-only `/ipc/switch-scene` request option `allow_empty=true` (surfaced in the `/obs` page as an `empty (debug)` checkbox) to intentionally emit an empty `scene_name` for negative-path plugin validation.
- Kept production IPC semantics intact while enabling explicit `missing_scene_name` validation from the local debug dashboard.

C++ OBS plugin shim (`obs-plugin-shim`):
- Rebuilt both harness and OBS plugin targets successfully from local CMake build dirs:
  - `cmake --build build --config Debug`
  - `cmake --build build-obs-local --config Debug`
- Confirmed callback-mode scene switching remains active (`auto-ack` disabled in plugin mode).
- Added/kept active-scene verification after `obs_frontend_set_current_scene(...)` and explicit failure reporting via `switch_verify_failed` if verification misses.

Repo hygiene / docs:
- Added `.gitignore` coverage for `obs-plugin-shim/build*` to reduce generated CMake project/build noise in repo status.
- Updated shim README to reflect current callback-mode + OBS validation capabilities.

### Real OBS Validation Outcome (OBS 32.0.4 + Rust Core IPC)

Environment used:
- Rust app: `obs-telemetry-bridge` (`cargo run --bin obs-telemetry-bridge`)
- OBS plugin shim DLL from local `obs-plugin-shim` build
- Local `/obs` dashboard debug controls (`IPC Switch Scene`)

Validated in real OBS (not harness):
1. Session / lifecycle basics:
- plugin loads
- named-pipe IPC session connects
- `hello_ack`, periodic `status_snapshot`, and `pong` observed
- clean plugin unload/disconnect observed
- plugin -> core `obs_shutdown_notice` observed

2. Callback-mode `switch_scene_result` positive path:
- dashboard-triggered `IPC Switch Scene` to a real scene (`BRB`)
- plugin received `switch_scene` in callback mode (`auto-ack disabled`)
- plugin executed scene switch and queued explicit `scene_switch_result(ok=true)`
- Rust core logged `ipc scene_switch_result received ... ok=true`

3. Callback-mode `switch_scene_result` negative paths:
- `scene_not_found`:
  - dashboard-triggered `IPC Switch Scene` to nonexistent scene (e.g. `DOES_NOT_EXIST_123`)
  - plugin logged `switch_scene target not found`
  - plugin queued/sent `scene_switch_result(ok=false, error="scene_not_found")`
  - Rust core logged matching `ipc scene_switch_result received ... ok=false error=Some("scene_not_found")`
- `missing_scene_name`:
  - dashboard-triggered `IPC Switch Scene` with empty scene name via debug `allow_empty=true`
  - plugin logged `switch_scene request missing scene_name`
  - plugin queued/sent `scene_switch_result(ok=false, error="missing_scene_name")`
  - Rust core logged matching `ipc scene_switch_result received ... ok=false error=Some("missing_scene_name")`

This completes the previously pending real callback-mode validation set for:
- `success`
- `scene_not_found`
- `missing_scene_name`

### Important Clarification (Doc/Checkout Drift)

The prior `Dock Bridge + Shim Callback/Scene-Switch Integration` addendum described repo-root dock bridge artifacts (notably `aegis-dock-bridge.js`) as present. In this checkout:
- `E:\Code\telemyapp\aegis-dock.jsx` exists
- `E:\Code\telemyapp\aegis-dock-wide.jsx` exists
- `aegis-dock-bridge.js` is **not** present anywhere under `E:\Code\telemyapp`

Treat dock bridge implementation status as unresolved in this repo snapshot until the file is recovered or recreated.

### Current State (After This Addendum)

- Rust core IPC and OBS plugin callback-mode scene switching are now real-OBS validated for the main positive/negative result cases.
- Browser dashboard remains a temporary debug/control surface and now includes an explicit empty-scene debug trigger for plugin validation.
- The plugin/dock UI direction remains valid from the repo-root aesthetic references:
  - `E:\Code\telemyapp\aegis-dock.jsx`
  - `E:\Code\telemyapp\aegis-dock-wide.jsx`
- Terminology/label alignment work is still needed before dock UI implementation:
  - align operator-facing labels with `docs/STATE_MACHINE_v1.md`
  - align IPC-/status-surface wording with current v0.0.3 payload naming
  - trim/rename ambiguous or legacy labels inherited from prototype/mock wording

### Recommended Next Steps (Updated)

1. Recreate or recover the missing dock bridge file(s) (`aegis-dock-bridge.js`) in this repo checkout.
2. Wire OBS scene inventory/current-scene callbacks into the bridge/UI state path (or equivalent replacement bridge).
3. Start plugin dock UI implementation with aesthetics intentionally aligned to:
   - `E:\Code\telemyapp\aegis-dock.jsx`
   - `E:\Code\telemyapp\aegis-dock-wide.jsx`
4. Do a terminology cleanup pass before/while wiring UI:
   - state labels (`STUDIO`, `IRL_CONNECTING`, `IRL_ACTIVE`, `IRL_GRACE`, etc.)
   - operator-facing health/status wording
   - scene-switch result phrasing and event log text
5. Keep browser `/obs` dashboard changes minimal and validation-focused while plugin dock work proceeds.

## Scaffold-Mode Real OBS Validation + Qt/WebEngine Runtime Blocker Addendum (2026-02-24, US/Pacific)

This addendum captures the next real-OBS validation pass after the callback-mode scene-switch validation addendum: the scaffold-mode dock-host plugin path was revalidated end-to-end with the Rust core IPC, and the opt-in Qt/WebEngine dock host path was narrowed to a runtime compatibility blocker (not just missing deployment files).

### Validated Working State (Real OBS, Scaffold Mode)

Build/runtime mode used:
- `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=ON`
- `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE=OFF` (scaffold fallback path)

Observed in real OBS + Rust core IPC:
- plugin load/unload succeeds
- named-pipe connect/disconnect succeeds
- `hello_ack`, recurring `status_snapshot`, and `pong` received
- OBS frontend callbacks observed (`SCENE_COLLECTION_CHANGED`, `SCENE_CHANGED`, `FINISHED_LOADING`)
- callback-mode `switch_scene` handling still succeeds with explicit queued/sent `scene_switch_result(ok=true)`
- plugin unload still emits queued/sent `obs_shutdown_notice`
- scaffold/log fallback bridge path is active in real OBS:
  - `dock bridge payload setObsSceneSnapshot=...`
  - `dock bridge payload receiveSceneSwitchCompletedJson=...`

Reference validation evidence:
- OBS log: `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 10-30-10.txt`

Important plugin paths at handoff time:
- Working scaffold build output:
  - `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-scaffold\Release\aegis-obs-shim.dll`
- Installed OBS plugin (currently scaffold-mode fallback and working):
  - `C:\Program Files (x86)\obs-studio\obs-plugins\64bit\aegis-obs-shim.dll`

### Qt/WebEngine Dock Host Runtime Blocker (Confirmed)

What was tried:
- Built/rebuilt the opt-in Qt/WebEngine dock host plugin against local Qt `6.8.2` and `6.8.3` (`aqtinstall`).
- Installed/deployed required Qt modules/runtime assets:
  - `qtwebengine`
  - `qtwebchannel`
  - `qtpositioning`
  - `qtserialport`
- Ran `windeployqt` and additionally placed `dxcompiler.dll` / `dxil.dll`.

Outcome:
- The Qt/WebEngine dock-host build compiles/links locally, but real OBS plugin load still fails with the generic module load failure (`aegis-obs-shim.dll not loaded`).
- This persisted after runtime file deployment, so the blocker is no longer treated as a simple "missing DLL/resource" issue.

Likely root cause (working hypothesis):
- Mixed Qt builds in-process:
  - OBS reports Qt `6.8.3` runtime, but OBS-shipped Qt binaries differ from local `aqtinstall` Qt `6.8.3` binaries (hash mismatch observed on `Qt6Core.dll` despite matching version string).
  - Loading local Qt WebEngine components against OBS-loaded Qt core likely causes ABI/build incompatibility at plugin load time.

### Current Direction (Updated)

- Continue plugin/core hybrid validation and bridge/state integration on the working scaffold path to maintain momentum.
- Re-center browser-host work on an OBS-native browser/CEF route (or another approach that avoids mixed-Qt runtime embedding), instead of pursuing local Qt WebEngine embedding as the near-term runtime path.
- Keep browser `/obs` dashboard changes minimal and validation-focused.

### Notes for Next Handoff / Context Window

- OBS plugin folder may currently contain extra Qt runtime files/resources from `windeployqt` experiments; these were not required for the working scaffold path and are currently runtime clutter (but not blocking scaffold validation).
- Rust app run path remains validated:
  - `cd E:\Code\telemyapp\telemy-v0.0.3\obs-telemetry-bridge`
  - `cargo run --bin obs-telemetry-bridge`

## OBS/CEF Dock Host Runtime Bring-Up + Shutdown Stabilization Addendum (2026-02-24, US/Pacific)

This addendum captures the next dock-host implementation and validation pass after the Qt/WebEngine runtime blocker: an OBS-native CEF dock host path was implemented using OBS `obs-browser` panel APIs, validated in real OBS, and stabilized for shutdown after multiple CEF teardown crashes.

### Completed Since Prior Addendum (OBS/CEF Host Implementation)

OBS source/deps bring-up:
- Initialized/populated the vendored OBS `obs-browser` submodule:
  - `third_party/obs-studio/plugins/obs-browser`
- Confirmed availability of panel APIs/headers needed for plugin-side CEF docking:
  - `panel/browser-panel.hpp`
  - `QCef`, `QCefWidget`
  - `obs_browser_init_panel()`
  - `obs_browser_qcef_version()`

`obs-plugin-shim` build/system:
- Added a new opt-in build flag in `obs-plugin-shim/CMakeLists.txt`:
  - `AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF`
- Added CMake detection/wiring for:
  - Qt6 `Core` / `Widgets`
  - local `obs-browser` panel headers in vendored OBS checkout
- Kept scaffold fallback and Qt/WebEngine path intact.

`obs-plugin-shim` dock host implementation (`src/obs_browser_dock_host_scaffold.cpp`):
- Added OBS/CEF-backed dock host branch using OBS `obs-browser` panel APIs:
  - `obs_browser_init_panel()`
  - `QCef::create_widget(...)`
  - `QCefWidget::executeJavaScript(...)` wired to existing shim JS executor callback ABI
- Reused existing shim page lifecycle semantics:
  - `aegis_obs_browser_dock_host_scaffold_on_page_ready()`
  - `aegis_obs_browser_dock_host_scaffold_on_page_unloaded()`
- Added deferred init retry (timer-based) to handle OBS module-load ordering:
  - `aegis-obs-shim` can load before `obs-browser`
  - host retries CEF panel init until available (bounded attempts)
- Added CEF page-ready timer probe path and existing page-ready -> `request_status` replay trigger.

Bridge/asset compatibility:
- Restored missing repo-root bridge-host/bootstrap files:
  - `E:\Code\telemyapp\aegis-dock-bridge-host.js`
  - `E:\Code\telemyapp\aegis-dock-browser-host-bootstrap.js`
- Added classic-script compatibility bridge wrapper for embedded page use:
  - `E:\Code\telemyapp\aegis-dock-bridge.global.js`
  - exposes `window.AegisDockBridge` without ESM `export` syntax
- Updated dock asset loader to prefer:
  - `aegis-dock-bridge.global.js` (fallback to `aegis-dock-bridge.js`)
- Refactored dock asset/html helper code so the OBS/CEF branch can load the same real bridge stack used by the Qt/WebEngine diagnostic path.

### Real OBS Validation Outcome (OBS 32.0.4 + Rust Core IPC)

Builds produced/used:
- Working scaffold build:
  - `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-scaffold\Release\aegis-obs-shim.dll`
- OBS/CEF host build:
  - `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef\Release\aegis-obs-shim.dll`

CEF host runtime bring-up (real OBS):
- CEF host branch compiles/links and loads in OBS (no plugin load failure).
- First CEF runtime issue was ordering, not ABI:
  - plugin `obs_module_load` ran before `obs-browser` finished loading
  - `obs_browser_init_panel()` initially unavailable
  - deferred retry later succeeded
- Real OBS logs confirmed:
  - deferred init
  - retry scheduling
  - `OBS/CEF host active`
  - CEF page-ready hook firing
  - shim page-ready callback causing queued/sent `request_status`

Reference logs:
- First successful CEF activation (fallback page path): `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 11-27-00.txt`
- Successful CEF activation + real bridge asset load + clean shutdown: `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 11-39-26.txt`

Real bridge asset loading (dev override):
- Launching OBS with `AEGIS_DOCK_BRIDGE_ROOT=E:\Code\telemyapp` enabled successful asset resolution in the CEF host:
  - `aegis-dock-bridge.global.js`
  - `aegis-dock-bridge-host.js`
  - `aegis-dock-browser-host-bootstrap.js`
- OBS log confirmed `bridge assets loaded from ...` with repo-root paths.

### Shutdown Crash Investigation + Stabilization (Real OBS)

Observed issue:
- Multiple real OBS shutdown crashes occurred after CEF host activation.
- Crash reports consistently pointed to:
  - `libcef.dll`
  - `obs-browser.dll!QCefWidgetInternal::closeBrowser`
  - `obs-browser.dll!obs_module_unload`
- This indicated teardown ordering/ownership issues during OBS exit.

Stabilization changes applied:
- Early browser host shutdown on frontend `EXIT` event (in `src/obs_plugin_entry.cpp`) instead of waiting for module unload.
- Avoided active dock removal and manual widget deletion during shutdown in the CEF host path (let OBS own dock/widget teardown).
- Explicitly called `QCefWidget::closeBrowser()` on shutdown when `obs_browser_qcef_version() >= 2` (matching OBS `BrowserDock` behavior).

Result:
- Real OBS shutdown no longer produced new crash reports in subsequent validation runs.
- `C:\Users\mpent\AppData\Roaming\obs-studio\crashes\Crash 2026-02-24 11-21-45.txt` is the latest crash captured before stabilization.
- Later successful runs (`11-27-00`, `11-39-26`) exited without new crash files.

### Current State (After This Addendum)

- Qt/WebEngine host remains runtime-blocked due to mixed-Qt incompatibility.
- OBS-native CEF host path now exists, builds, loads in real OBS, activates after deferred retry, and shuts down cleanly.
- Real bridge/host/bootstrap JS assets can load in the CEF host via `AEGIS_DOCK_BRIDGE_ROOT` dev override.
- IPC and callback-mode scene switching remain healthy while the CEF host is active.
- Some early startup scene snapshot payloads still log via fallback before JS sink registration/page-ready (expected timing behavior).

### Recommended Next Steps (Updated)

1. Add explicit post-page-ready JS sink delivery validation logs (one-time/sampled) for:
   - `receiveIpcEnvelopeJson`
   - `receiveSceneSnapshotJson`
   - `receiveSceneSwitchCompletedJson`
2. Reduce/noise-gate fallback payload logging after JS sink registration to better distinguish pre-page-ready vs sink-delivered events.
3. Decide near-term asset packaging for plugin runtime:
   - continue `AEGIS_DOCK_BRIDGE_ROOT` for dev, and/or
   - deploy JS assets into plugin module data dir
4. Replace/advance the generated diagnostic HTML shell toward the real dock UI mount path while preserving fallback behavior.
5. Keep scaffold build available as a known-good fallback during CEF host iteration.

## OBS/CEF JS Sink Validation + Asset Staging Follow-Up Addendum (2026-02-24, US/Pacific)

This addendum captures the next validation-focused pass after OBS/CEF host bring-up and shutdown stabilization: post-page-ready JS sink delivery logging was added/validated, fallback payload logs were noise-gated, and a first-pass build-time dock JS asset staging path was added.

### Completed Since Prior Addendum (Validation + Packaging Follow-Up)

`obs-plugin-shim` plugin entry (`src/obs_plugin_entry.cpp`):
- Added one-time post-page-ready JS sink delivery validation logs (per page-ready cycle) for:
  - `receiveIpcEnvelopeJson`
  - `receiveSceneSnapshotJson`
  - `receiveSceneSwitchCompletedJson`
- Added fallback payload log phase labeling + noise-gating/sampling after JS sink registration:
  - `no_js_sink`
  - `pre_page_ready`
  - `post_page_ready_sink_miss`
- Preserved replay/page-ready behavior (`ReplayDockStateToJsSinkIfAvailable()` + queued `request_status`) while making the logs easier to distinguish in real OBS output.

`obs-plugin-shim` CEF host scaffold (`src/obs_browser_dock_host_scaffold.cpp`):
- Added validation-focused timing logs for:
  - CEF bootstrap HTML mode (`real_bridge_assets` vs `validation_fallback`) and payload size
  - ready-probe timer start / fire
  - CEF init retry attempt counts
  - duplicate page-ready suppression (debug)

`obs-plugin-shim` build/system (`CMakeLists.txt`):
- Added first-pass build-time dock JS asset staging support for the OBS plugin target:
  - option `AEGIS_STAGE_DOCK_BRIDGE_ASSETS` (default `ON`)
  - cache path override `AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT`
- Post-build staging copies:
  - `aegis-dock-bridge.global.js`
  - `aegis-dock-bridge-host.js`
  - `aegis-dock-browser-host-bootstrap.js`
  into:
  - `.../Release/data/obs-plugins/aegis-obs-shim/`
- `AEGIS_DOCK_BRIDGE_ROOT` runtime override remains supported and preferred for local dev iteration.

### Validation Outcome (Real OBS, 2026-02-24 US/Pacific)

Build/package validation:
- Rebuilt CEF plugin target in:
  - `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef`
- CMake reconfigure confirmed staging source path in output (`Aegis OBS shim dock assets will be staged from: ...`)
- Staged JS assets were present under:
  - `E:\Code\telemyapp\telemy-v0.0.3\obs-plugin-shim\build-obs-cef\Release\data\obs-plugins\aegis-obs-shim\`

OBS runtime validation (fallback path + instrumentation checks):
- `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 12-08-01.txt` confirmed:
  - phase-tagged fallback payload logs (e.g. `phase=no_js_sink`)
  - CEF bootstrap mode log (`mode=validation_fallback`)
  - post-page-ready JS sink delivery validation logs for:
    - `receiveIpcEnvelopeJson`
    - `receiveSceneSnapshotJson`
    - `receiveSceneSwitchCompletedJson`

OBS runtime validation (real bridge asset path):
- Launching OBS with `AEGIS_DOCK_BRIDGE_ROOT=E:\Code\telemyapp` and the correct OBS working directory produced:
  - `C:\Users\mpent\AppData\Roaming\obs-studio\logs\2026-02-24 12-20-35.txt`
- That log confirmed:
  - `browser dock scaffold bridge assets loaded from ...`
  - `CEF bootstrap html prepared mode=real_bridge_assets`
  - `browser dock scaffold CEF page ready (timer)`
  - post-page-ready JS sink delivery validation logs for:
    - `receiveIpcEnvelopeJson`
    - `receiveSceneSnapshotJson`
- `receiveSceneSwitchCompletedJson` was subsequently re-confirmed in the same `12-20-35` real-asset-path run after a debug scene switch trigger:
  - log shows `switch_scene applying`, queued `scene_switch_result`, and post-page-ready JS sink delivery validation for `receiveSceneSwitchCompletedJson`

### Important Operator Gotcha (OBS Launch From PowerShell)

- Launching `obs64.exe` from an arbitrary working directory (for example, `E:\Code\telemyapp`) can cause OBS app startup to fail before plugin initialization with:
  - `error: Failed to load locale`
- This is an OBS application locale resolution issue (not the plugin locale warning).
- When launching OBS from PowerShell for `AEGIS_DOCK_BRIDGE_ROOT` validation, use:
  - `Start-Process -WorkingDirectory (Split-Path <obs64.exe>) ...`

### Recommended Next Steps (Updated)

1. Decide whether the staged module-data asset path should become the default runtime strategy (with `AEGIS_DOCK_BRIDGE_ROOT` retained as a dev override).
2. Keep scaffold build available as fallback while continuing CEF-host validation and dock UI iteration.
3. Consider reducing or demoting some CEF timing logs (`LOG_DEBUG`/`LOG_INFO`) once validation focus shifts from host bring-up to dock UI implementation.

## Dock Action Status-Lifecycle + Mode/Setting Mapping Sync Addendum (2026-02-27, US/Pacific)

This addendum captures a focused follow-up to keep implementation and handoff/spec docs aligned for dock action transport behavior.

### Completed Since Prior Addendum

`obs-plugin-shim/src/obs_plugin_entry.cpp`:
- Added lightweight tracking for dock-originated `request_status` actions by `request_id`.
- When an incoming IPC envelope type is `status_snapshot`, the oldest pending `request_status` action now emits terminal native action-result:
  - `actionType="request_status"`
  - `status="completed"`
  - `ok=true`
  - `detail="status_snapshot_received"`
- Preserved existing immediate queued action-result (`status="queued"`) behavior.
- Clears pending request-status action tracking during plugin unload.

Documentation sync:
- Updated root `HANDOFF_STATUS.md` native dock action coverage to reflect implemented behavior:
  - `request_status` now has terminal completion semantics.
  - `set_mode` and `set_setting` are forwarded to Rust core IPC (no longer `not_implemented` reject-only notes).
- Updated `docs/IPC_PROTOCOL_v1.md` section `10.3 Action Mapping`:
  - `set_mode` mapping marked `Available` via `set_mode_request`.
  - `set_setting` mapping marked `Available` via `set_setting_request` for recognized keys.
- Updated `docs/CURRENT_STATUS.md` with a dated implementation-follow-up note (2026-02-27).

### Current State (After This Addendum)

- Dock action transport now supports explicit queued -> completed lifecycle for `request_status`.
- `set_mode` / `set_setting` transport paths are active and validated at plugin/core message level (payload validation + forward-to-core queueing).
- Remaining validation work is runtime-focused in real OBS sessions (confirming expected action-result sequencing and UI behavior under reconnect/noise conditions).

### Recommended Next Steps (Updated)

1. Run real OBS + Rust core validation for `request_status` action lifecycle and capture log evidence (`queued` then `completed` on next `status_snapshot`).
2. Define/implement terminal completion semantics for `set_mode` / `set_setting` (currently queue acknowledgement is immediate; completion is inferred via snapshot changes).
3. Continue CEF dock UI polish and asset packaging decisions with this synchronized action contract as baseline.

## Dock Runtime Regression Recovery (Scenes/Toggle/Theme/Title) Addendum (2026-02-26, US/Pacific)

This addendum captures a focused regression-recovery pass after dock action transport changes, where visible dock runtime behavior diverged (empty scenes, toggle landing state mismatch, theme no longer applying in panel, and temporary title-channel payload text leakage).

### Completed Since Prior Addendum

Bridge/runtime compatibility and state projection:
- Restored CEF-compatible classic/global bridge runtime path for repo-root `aegis-dock-bridge.js` so host bootstrap does not fall back to noop behavior in classic script contexts.
- Updated bridge projection in the global path to derive:
  - `scenes.autoSwitchArmed` from effective setting state (`auto_scene_switch` + `manual_override`) instead of only `override_enabled`
  - `scenes.autoSwitchEnabled` and `scenes.manualOverrideEnabled`
  - default settings list from `status_snapshot.payload.settings` when plugin-local settings are absent
- Reintroduced `theme` projection from `status_snapshot.payload.theme` into dock state for runtime panel theming.

Dock action transport and UX polish:
- Kept native dock-action forwarding over temporary `document.title` channel for OBS/CEF host intake.
- Fixed dock title bar leakage by restoring previous document title immediately after action signaling (`__AEGIS_DOCK_ACTION__:<...>` no longer persists in title bar).
- Removed temporary bootstrap diagnostic event noise (`aegis:dock:action-forwarded`) after transport revalidation.

Runtime asset/path corrections:
- Synced rebuilt `aegis-dock-app.js` into OBS installed module-data path to avoid stale app bundle behavior during runtime validation.
- Revalidated runtime with `AEGIS_DOCK_BRIDGE_ROOT=E:\Code\telemyapp` and fresh OBS + Rust core restarts.

### Validation Outcome (Real OBS)

User-verified outcomes in real OBS runtime:
- scenes now populate without manual scene interaction
- auto scene switch correctly toggles and lands on `ARMED` / `MANUAL`
- OBS theme changes now apply dynamically to dock panel content
- dock title no longer shows persistent encoded `__AEGIS_DOCK_ACTION__:%7B...` payload text

Observed evidence patterns in latest logs remained consistent with healthy host/runtime path:
- module load + bridge assets load + CEF page-ready
- `receiveSceneSnapshotJson` delivery validation
- queued `set_setting_request` action path with plugin/core IPC active

### Current State (After This Addendum)

- Dock runtime regressions from the interim transport refactor are recovered.
- Core action transport path remains active while visible dock behavior matches expected UX for scenes, settings toggle state, and dynamic theme.
- Remaining work should prioritize packaging/cleanup and reducing temporary runtime bring-up instrumentation where no longer needed.
