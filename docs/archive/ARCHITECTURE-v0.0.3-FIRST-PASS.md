# Telemy v0.0.3 Architecture Mapping (First Pass)

Status note:
- This is an initial mapping document.
- Authoritative behavior contracts now live in `docs/API_SPEC_v1.md`, `docs/DB_SCHEMA_v1.md`, `docs/STATE_MACHINE_v1.md`, and `docs/IPC_PROTOCOL_v1.md`.
- Where this file conflicts with implementation status, use `HANDOFF_STATUS.md` and code as source of truth.

## 1. Scope

This document maps the Aegis architecture into the `telemy-v0.0.3` implementation workspace.

Implementation target:
- `E:\Code\telemyapp\telemy-v0.0.3`

Current Rust crate:
- `telemy-v0.0.3/obs-telemetry-bridge`

---

## 2. Repository Layout Plan for v0.0.3

Planned top-level layout:

1. `obs-telemetry-bridge/` (existing)
- Rust local engine (core runtime, state machine, dashboard, ChatBridge, CloudLink).

2. `obs-aegis-plugin/` (new, planned)
- C++ OBS plugin shim.
- Child-process supervision and Named Pipe client.
- Browser dock registration and OBS scene-switch bridge.

3. `aegis-control-plane/` (new, planned)
- Go API/backend.
- Relay lifecycle, auth, usage, billing primitives.

4. `aegis-relay/` (new, planned)
- Relay runtime packaging (Go telemetry service + SRT binaries).
- AMI build scripts and health/self-destruct logic.

5. `docs/` (new, planned)
- API contracts, state machine spec, IPC protocol, migration notes.

Note:
- Phase 1 build is local-only; only `obs-telemetry-bridge` and `obs-aegis-plugin` are required for first ship.
- Update: `aegis-control-plane/` is now implemented in this workspace.

---

## 3. Rust Module Ownership (`obs-telemetry-bridge/src`)

Current modules:
- `app`, `config`, `exporters`, `metrics`, `model`, `security`, `server`, `startup`, `tray`

Target ownership map:

1. `app/`
- Runtime orchestration, task lifecycle, shutdown coordination.

2. `startup/`
- Boot sequencing, dependency checks, mode initialization.

3. `config/`
- Config model + validation + migration hooks for v0.0.3 settings.

4. `metrics/`
- OBS signal ingestion and normalized health indicators.

5. `model/`
- Domain types: mode, session, health status, switch intent.

6. `server/`
- Local dashboard API and status endpoints.

7. `security/`
- DPAPI storage and token handling for local secrets.

8. `tray/`
- Desktop tray controls, status indication, quick actions.

9. `exporters/`
- Telemetry export path(s), diagnostics, structured metrics output.

Planned new Rust modules:

1. `state/` (new)
- Authoritative failover state machine logic and transition guards.

2. `chatbridge/` (new)
- Twitch/Kick command parsing, RBAC, cooldowns, audit events.

3. `cloudlink/` (new, dormant in Phase 1)
- Go backend API client, session lifecycle calls, relay telemetry client.

4. `ipc/` (new)
- Named Pipe message schema, priority queues, client/server adapters.

5. `obs_control/` (new)
- Scene switch command dispatch abstraction to plugin/OBS boundary.

---

## 4. Contract-to-Module Mapping (v0.0.3)

1. C1 Relay outage survival:
- Implement in `aegis-control-plane` watchdog + `aegis-relay` health loop.

2. C2 Credential separation:
- `security/` + `cloudlink/` + backend auth middleware.

3. C3 SLA wording and measurements:
- `metrics/` + `state/` + local diagnostics endpoint in `server/`.

4. C4 Idempotent lifecycle:
- `cloudlink/` request behavior + backend idempotency store/table.

5. H1 reconnect-first:
- `startup/` + `cloudlink/` active-session check before any start request.

6. H2 ChatBridge controls:
- `chatbridge/` parser, RBAC, cooldown, and audit sink.

7. H3 Relay runtime in Go:
- `aegis-relay` service runtime selection and deployment templates.

---

## 5. Milestone Tickets (Execution Backlog)

Ticket IDs use `AEGIS-v0.0.3-###`.

### Phase 1: Local Ship (Studio Mode + ChatBridge)

1. `AEGIS-v0.0.3-001`:
- Add `state` module and define state transitions for local failover.

2. `AEGIS-v0.0.3-002`:
- Add `ipc` module with MessagePack schema v1 and priority lanes.

3. `AEGIS-v0.0.3-003`:
- Build `chatbridge` module with RBAC and cooldown controls.

4. `AEGIS-v0.0.3-004`:
- Integrate local dashboard controls for mode/status/chat command visibility.

5. `AEGIS-v0.0.3-005`:
- Implement plugin shim async pipe policy (overlapped I/O, 500ms timeout).

6. `AEGIS-v0.0.3-006`:
- Add crash recovery policy: one restart attempt + user-visible failure state.

7. `AEGIS-v0.0.3-007`:
- Add Phase 1 test suite for state transitions and IPC timeout behavior.

### Phase 2: Cloud Skeleton

1. `AEGIS-v0.0.3-101`:
- Stand up `aegis-control-plane` service skeleton and auth middleware.

2. `AEGIS-v0.0.3-102`:
- Implement `/relay/start`, `/relay/stop`, `/relay/active` with idempotency.

3. `AEGIS-v0.0.3-103`:
- Create PostgreSQL schema v1 for users/sessions/relay_instances/usage.

4. `AEGIS-v0.0.3-104`:
- Implement relay provisioner and region/AMI manifest endpoint.

5. `AEGIS-v0.0.3-105`:
- Implement watchdog conditions for idle/orphan cleanup (C1 logic).

### Phase 3: IRL Integration

1. `AEGIS-v0.0.3-201`:
- Activate `cloudlink` in Rust core and connect relay telemetry channel.

2. `AEGIS-v0.0.3-202`:
- Implement dual-path signal merge (local + cloud) with conflict rules.

3. `AEGIS-v0.0.3-203`:
- Add reconnect-first startup path with active-session resume.

4. `AEGIS-v0.0.3-204`:
- Add pairing flow UX for `pair_token` and relay endpoint details.

### Phase 4: Billing and Launch Hardening

1. `AEGIS-v0.0.3-301`:
- Implement usage aggregation and Time Bank enforcement.

2. `AEGIS-v0.0.3-302`:
- Implement outage reconciliation using relay-reported uptime.

3. `AEGIS-v0.0.3-303`:
- Add region selection override and final operational telemetry.

4. `AEGIS-v0.0.3-304`:
- Final packaging, installer updates, and release validation checklist.

---

## 6. Risk Controls Required in v0.0.3

1. Named Pipe deadlock prevention:
- Async/overlapped pipe I/O only in plugin.
- No blocking operations on OBS main thread.

2. ARM64 relay sizing risk:
- Benchmark t4g.small under sustained ingest.
- Upgrade backend default instance type if jitter/throttling appears.

3. Time Bank reconciliation:
- Relay health payload includes `session_uptime_seconds`.
- Backend reconciliation job applies gap corrections after outage.

---

## 7. Immediate Next Deliverables

1. `docs/API_SPEC_v1.md` (backend endpoints + auth/idempotency contract).
2. `docs/IPC_PROTOCOL_v1.md` (MessagePack schema + timeout rules).
3. `docs/STATE_MACHINE_v1.md` (formal transitions and guard conditions).
4. `docs/DB_SCHEMA_v1.md` (tables, indexes, retention, reconciliation fields).

---

## 8. Current Implementation Snapshot

Implemented in `aegis-control-plane`:

1. API surface:
- `/api/v1/relay/start`, `/relay/active`, `/relay/stop`, `/relay/manifest`, `/usage/current`, `/relay/health`

2. Lifecycle behavior:
- idempotent start semantics
- provision -> activate session flow
- stop deprovision + terminal state sync

3. Relay providers:
- `fake` provider for local dev
- `aws` provider for EC2 `RunInstances` and `TerminateInstances`

4. Testing:
- API stop-path tests
- relay deprovision error-classification tests
- store transaction tests with `pgxmock`
