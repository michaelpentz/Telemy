# Aegis Database Schema v1 (v0.0.4)

## 1. Scope

This schema supports the Telemy v0.0.4 cloud control plane.

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
- `eip_allocation_id` / `eip_public_ip`: Per-user Elastic IP for stable relay addresses. Allocated on first relay provision, reused for all subsequent provisions. Single-region (us-west-2) for now.

Checks:
- `plan_tier in ('starter','standard','pro')`
- `plan_status in ('active','past_due','canceled','trial')`
- `included_seconds >= 0`

## 3.2 `api_keys`

Purpose:
- API key metadata for bootstrap auth and key management.

## 3.3 `relay_instances`

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

## 3.4 `sessions`

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

## 3.5 `idempotency_records`

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
