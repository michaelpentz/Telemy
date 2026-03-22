# Aegis Database Schema v1 (v0.0.5)

## 1. Scope

This schema supports the Telemy v0.0.5 cloud control plane.

Covers:
- Identity and API access
- Relay session lifecycle
- Idempotent provisioning
- Usage metering (Time Bank)
- Outage reconciliation and billing auditability

Database:
- PostgreSQL 15+

---

## 2. Conventions

- Primary keys: text IDs with prefixes (`usr_`, `ses_`, `rly_`, `key_`, `use_`).
- Timestamps: `timestamptz` in UTC.
- Monetary amounts: integer cents.
- Durations: integer seconds.
- Status fields: constrained text (check constraints or enums).

---

## 3. Tables

## 3.1 `users`

Purpose:
- Account-level identity and plan state.

Columns:
- `id` text primary key
- `email` text not null unique
- `display_name` text null
- `stream_token` text not null unique
- `plan_tier` text not null default `starter`
- `plan_status` text not null default `active`
- `cycle_start_at` timestamptz not null
- `cycle_end_at` timestamptz not null
- `included_seconds` integer not null default 0
- `eip_allocation_id` text null
- `eip_public_ip` inet null
- `created_at` timestamptz not null default now()
- `updated_at` timestamptz not null default now()

Notes:
- `stream_token`: Permanent per-user stream identifier used to derive relay SLS stream IDs (`live_{stream_token}` / `play_{stream_token}`).
- `eip_allocation_id` / `eip_public_ip`: Per-user Elastic IP for stable relay addresses. Allocated on first relay provision, reused for all subsequent provisions. Single-region (us-west-2) for now.

Checks:
- `plan_tier in ('starter','standard','pro')`
- `plan_status in ('active','past_due','canceled','trial')`
- `included_seconds >= 0`

## 3.2 `auth_sessions`

Purpose:
- Session-backed control-plane auth for plugin-issued `cp_access_jwt` and refresh-token rotation.

Columns:
- `id` text primary key
- `user_id` text not null references `users(id)` on delete cascade
- `refresh_token_hash` text not null unique
- `client_platform` text not null
- `client_version` text null
- `device_name` text null
- `created_at` timestamptz not null default now()
- `last_seen_at` timestamptz not null default now()
- `expires_at` timestamptz not null
- `revoked_at` timestamptz null

Notes:
- `cp_access_jwt` is short-lived and carries `uid` plus session id (`sid`) claims.
- `refresh_token_hash` stores the server-side hash of the opaque refresh token.
- `revoked_at` immediately invalidates session-backed JWTs that present the matching `sid`.

## 3.3 `api_keys`

Purpose:
- API key metadata for bootstrap auth and key management.

## 3.4 `plugin_login_attempts`

Purpose:
- Track short-lived browser login attempts for the OBS plugin.

Columns:
- `id` text primary key
- `poll_token_hash` text not null unique
- `status` text not null
- `user_id` text null references `users(id)` on delete set null
- `completed_session_id` text null references `auth_sessions(id)` on delete set null
- `client_platform` text not null
- `client_version` text null
- `device_name` text null
- `deny_reason_code` text null
- `expires_at` timestamptz not null
- `completed_at` timestamptz null
- `created_at` timestamptz not null default now()

Checks:
- `status in ('pending','completed','denied','expired')`

Notes:
- `poll_token_hash` stores the server-side hash of the opaque plugin poll token.
- `completed_session_id` is set once a completed attempt is claimed into an `auth_session`.
- A claimed attempt is not reusable; the plugin is expected to stop polling after the first `200 completed`.

## 3.5 `relay_instances`

Purpose:
- Track actual cloud relay infrastructure lifecycle.

Columns:
- `id` text primary key
- `session_id` text null unique
- `aws_instance_id` text not null unique
- `region` text not null
- `ami_id` text not null
- `instance_type` text not null
- `public_ip` inet null
- `srt_port` integer not null default 5000
- `ws_url` text null
- `state` text not null
- `launched_at` timestamptz not null
- `terminated_at` timestamptz null
- `last_health_at` timestamptz null
- `created_at` timestamptz not null default now()

Checks:
- `state in ('provisioning','running','terminating','terminated','error')`

## 3.6 `sessions`

Purpose:
- Authoritative relay session lifecycle and usage anchors.

Columns:
- `id` text primary key
- `user_id` text not null references `users(id)` on delete cascade
- `relay_instance_id` text null references `relay_instances(id)`
- `status` text not null
- `provision_step` text null
- `region` text not null
- `idempotency_key` uuid null
- `requested_by` text not null default `dashboard`
- `started_at` timestamptz not null
- `grace_started_at` timestamptz null
- `stopped_at` timestamptz null
- `max_session_seconds` integer not null default 57600
- `grace_window_seconds` integer not null default 600
- `duration_seconds` integer not null default 0
- `reconciled_seconds` integer not null default 0
- `created_at` timestamptz not null default now()
- `updated_at` timestamptz not null default now()

Checks:
- `status in ('provisioning','active','grace','stopped')`

## 3.7 `relay_pool`

Purpose:
- Registry of always-on relay VPS nodes available for pool assignment.

Columns:
- `server_id` text primary key
- `provider` text not null
- `host` text not null
- `ip` inet not null
- `region` text not null
- `status` text not null default `active`
- `current_sessions` integer not null default 0
- `max_sessions` integer not null default 10
- `created_at` timestamptz default now()
- `last_health_check` timestamptz
- `health_status` text default `unknown`

Checks:
- `status in ('active','draining','offline')`
- `health_status in ('healthy','unhealthy','unknown')`

Notes:
- `AssignRelay` uses `FOR UPDATE SKIP LOCKED` to pick the least-loaded healthy server.
- `current_sessions` is incremented on assign, decremented on release.

## 3.8 `relay_assignments`

Purpose:
- Per-session relay pool allocations. Released when the session ends.

Columns:
- `id` bigserial primary key
- `user_id` text not null
- `session_id` text not null unique
- `connection_id` text null
- `server_id` text not null references `relay_pool(server_id)`
- `stream_token` text not null
- `assigned_at` timestamptz default now()
- `released_at` timestamptz null

Notes:
- `released_at IS NULL` means the allocation is active.
- Session queries join `relay_pool` via `relay_assignments` to resolve the relay's `host` and `ip` without an AWS API call.

## 3.9 `user_stream_slots`

Purpose:
- Named per-user stream slots for multi-stream setups. Each slot has its own `stream_token`.

Columns:
- `user_id` text not null
- `slot_number` integer not null
- `label` text null
- `stream_token` text not null unique
- Primary key: `(user_id, slot_number)`

Notes:
- `stream_token` is globally unique; SLS stream IDs are `live_<token>` / `play_<token>`.
- `label` is user-assigned (e.g., "Primary", "Backup"). Settable via `PUT /api/v1/user/stream-slots/{slotNumber}/label`.
- Exposed to the plugin via `GET /api/v1/auth/session` (`stream_slots` array) and `GET /api/v1/user/stream-slots`.

## 3.10 `idempotency_records`

Purpose:
- Store dedupe semantics for `POST /relay/start`.

Columns:
- `id` bigserial primary key
- `user_id` text not null references `users(id)` on delete cascade
- `endpoint` text not null
- `idempotency_key` uuid not null
- `request_hash` text not null
- `response_json` jsonb not null
- `session_id` text null references `sessions(id)`
- `created_at` timestamptz not null default now()
- `expires_at` timestamptz not null

---

## 4. Lifecycle and Integrity Rules

1. One active session per user:
- Enforced by partial unique index in `sessions`.

2. State transitions:
- Service layer enforces legal transitions:
  - `provisioning -> active`
  - `active -> grace`
  - `grace -> active`
  - `active|grace -> stopped`

3. Idempotency (Async Provisioning):
- `idempotency_records` stores request hash and canonical response.
- If a session is replayed while `active`, the response is reconstructed from current state to ensure the client receives final credentials.

4. Outage reconciliation:
- Recovery job compares:
  - backend-calculated session duration
  - max `session_uptime_seconds` from relay health events
- Positive gap emits:
  - `billing_adjustments` row
  - update to `sessions.reconciled_seconds`
  - update to `usage_records.reconciled_seconds` and `billable_seconds`

---

## 5. Operational Jobs

1. `idempotency_ttl_cleanup`: Deletes expired `idempotency_records`.
2. `session_usage_rollup`: Updates live `duration_seconds` for active/grace sessions.
3. `outage_reconciliation`: Applies `session_uptime_seconds` true-ups after backend recovery.
