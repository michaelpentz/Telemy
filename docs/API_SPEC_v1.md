# Aegis Control Plane API Spec v1 (v0.0.4)

## 1. Scope

This spec defines the cloud API for the Telemy v0.0.4 control plane. This is the authoritative contract for the native C++ plugin.

Focus:
- Authentication and authorization
- Relay lifecycle (`start`, `stop`, `active`)
- Idempotency guarantees
- Session state model
- Usage and reconciliation data contracts

Base path:
- `/api/v1`

Transport:
- **HTTPS only (TLS 1.2+)**. 
- Clients must not infer TLS intent from the port number (e.g., custom ports like 8443 must still use TLS).
- The Go API server assumes TLS termination happens upstream (reverse proxy / load balancer).

---

## 2. Authentication Model

Recommended product model:
- End users authenticate through a browser-based account login flow in the plugin.
- The backend determines whether the user is entitled to the paid relay feature.
- The plugin stores only backend-issued auth material locally and uses that for control-plane calls.
- Sender apps authenticate to the relay's SLS layer with per-user stream IDs derived from `stream_token`, not with control-plane credentials.

See also: `AUTH_ENTITLEMENT_MODEL.md`.

Credentials:
1. `cp_access_jwt`:
- Used by the C++ plugin for control-plane API calls.
- Sent as `Authorization: Bearer <jwt>`.
- Stored in DPAPI-encrypted vault on the client. Never exposed to the dock JS layer.
- Short-lived bearer JWT. Current backend issuance path is session-backed with `sid` validation when present.

2. `refresh_token`:
- Opaque token used to rotate `cp_access_jwt`.
- Sent only to `POST /api/v1/auth/refresh`.
- Stored in DPAPI-encrypted vault on the client. Never exposed to the dock JS layer.

Hard rule:
- Only `cp_access_jwt` is valid for control-plane endpoints.
- Relay activation entitlement is enforced server-side even if the plugin UI already hides or disables controls.

---

## 3. Common Headers

Required on authenticated endpoints:
- `Authorization: Bearer <cp_access_jwt>`
- `X-Aegis-Client-Version: 0.0.4`
- `X-Aegis-Client-Platform: windows`

Required for relay start:
- `Idempotency-Key: <uuid-v4>`

Optional tracing:
- `X-Request-ID: <uuid-v4>`

---

## 4. Resource Model

## 4.1 Relay Session

```json
{
  "session_id": "ses_01JABCDEF...",
  "status": "provisioning|active|grace|stopped",
  "provision_step": "launching_instance|waiting_for_instance|starting_docker|starting_containers|creating_stream|ready",
  "region": "us-east-1",
  "relay": {
    "instance_id": "i-0abc123...",
    "public_ip": "203.0.113.10",
    "srt_port": 5000
  },
  "timers": {
    "grace_window_seconds": 600,
    "max_session_seconds": 57600
  },
  "usage": {
    "started_at": "2026-02-21T20:00:00Z",
    "ended_at": null,
    "duration_seconds": 0
  }
}
```

---

## 5. Endpoints

## 5.1 GET `/api/v1/auth/session`

Return the current signed-in user, relay entitlement, usage snapshot, and active relay state.

Response `200`:
```json
{
  "user": {
    "id": "usr_01J...",
    "email": "user@example.com",
    "display_name": "telemy-user"
  },
  "entitlement": {
    "relay_access_status": "enabled|disabled",
    "reason_code": "ok|subscription_required|subscription_inactive|user_not_found",
    "plan_tier": "starter|standard|pro",
    "plan_status": "trial|active|past_due|canceled"
  },
  "usage": {
    "included_seconds": 14400,
    "consumed_seconds": 3900,
    "remaining_seconds": 10500,
    "overage_seconds": 0
  },
  "active_relay": {
    "session_id": "ses_01J...",
    "status": "active"
  }
}
```

Error responses:
- `401` invalid/expired auth session
- `500` internal error

## 5.2 POST `/api/v1/auth/plugin/login/start`

Create a short-lived plugin login attempt and return the browser URL plus poll token.

Current rollout note:
- Today the returned `authorize_url` lands on the temporary operator-assisted Pages flow at `telemyapp.com/login/plugin?attempt=...`.
- That page completes the attempt against an existing Telemy `user_id` through `POST /api/v1/auth/plugin/login/complete`.

Request body:
```json
{
  "client": {
    "platform": "windows",
    "plugin_version": "0.0.4",
    "device_name": "OBS Desktop"
  }
}
```

Response `201`:
```json
{
  "login_attempt_id": "pla_01J...",
  "authorize_url": "https://telemyapp.com/login/plugin?attempt=pla_01J...",
  "poll_token": "<opaque>",
  "expires_at": "2026-03-16T22:10:00Z",
  "poll_interval_seconds": 3
}
```

Error responses:
- `400` invalid payload
- `500` internal error

## 5.3 POST `/api/v1/auth/plugin/login/poll`

Poll the status of a plugin login attempt.

Request body:
```json
{
  "login_attempt_id": "pla_01J...",
  "poll_token": "<opaque>"
}
```

Response `202` pending:
```json
{
  "status": "pending"
}
```

Response `200` completed:
```json
{
  "status": "completed",
  "auth": {
    "cp_access_jwt": "<jwt>",
    "refresh_token": "<opaque>"
  },
  "user": {
    "id": "usr_01J...",
    "email": "user@example.com",
    "display_name": "telemy-user"
  },
  "entitlement": {
    "relay_access_status": "enabled|disabled",
    "reason_code": "ok|subscription_required|subscription_inactive|user_not_found",
    "plan_tier": "starter|standard|pro",
    "plan_status": "trial|active|past_due|canceled"
  },
  "usage": {
    "included_seconds": 14400,
    "consumed_seconds": 3900,
    "remaining_seconds": 10500,
    "overage_seconds": 0
  },
  "active_relay": null
}
```

Error responses:
- `400` invalid payload
- `403` login denied
- `404` login attempt not found
- `409` login attempt already claimed
- `410` login attempt expired
- `500` internal error

## 5.4 POST `/api/v1/auth/refresh`

Rotate refresh token and issue a new `cp_access_jwt`.

Request body:
```json
{
  "refresh_token": "<opaque>"
}
```

Response `200`:
```json
{
  "auth": {
    "cp_access_jwt": "<jwt>",
    "refresh_token": "<new_opaque>"
  },
  "user": {
    "id": "usr_01J...",
    "email": "user@example.com",
    "display_name": "telemy-user"
  },
  "entitlement": {
    "relay_access_status": "enabled|disabled",
    "reason_code": "ok|subscription_required|subscription_inactive|user_not_found",
    "plan_tier": "starter|standard|pro",
    "plan_status": "trial|active|past_due|canceled"
  },
  "usage": {
    "included_seconds": 14400,
    "consumed_seconds": 3900,
    "remaining_seconds": 10500,
    "overage_seconds": 0
  }
}
```

Error responses:
- `400` invalid payload
- `401` invalid refresh token
- `410` auth session expired
- `500` internal error

## 5.5 POST `/api/v1/auth/logout`

Revoke the current auth session.

Response:
- `204 No Content`

Error responses:
- `401` invalid/expired auth session
- `500` internal error

## 5.6 POST `/api/v1/relay/start`

Start or return an active relay session for the authenticated user.

Idempotency:
- `Idempotency-Key` is required.
- **Async Refresh**: If a session transitions from `provisioning` to `active` after the initial request, subsequent replays with the same key will return the current `active` state (reconstructing the session response) rather than a stale `provisioning` cached response.

Request body:
```json
{
  "region_preference": "auto|us-east-1|eu-west-1",
  "client_context": {
    "obs_connected": true,
    "mode": "studio|irl",
    "requested_by": "dashboard"
  }
}
```

Success responses:
- `200 OK` (existing session returned)
- `201 Created` (new session created)

Response body:
```json
{
  "session": {
    "session_id": "ses_01JABCDEF...",
    "status": "provisioning|active",
    "region": "us-east-1",
    "relay": {
      "instance_id": "i-0abc123...",
      "public_ip": "203.0.113.10",
      "srt_port": 5000
    },
    "timers": {
      "grace_window_seconds": 600,
      "max_session_seconds": 57600
    }
  }
}
```

Error responses:
- `400` invalid payload
- `401` invalid/missing JWT
- `403` tier/entitlement denied
- `409` illegal state transition
- `429` rate limited
- `500` internal error

Example `403 entitlement_denied`:
```json
{
  "error": {
    "code": "entitlement_denied",
    "message": "relay access is not enabled for this account",
    "reason_code": "subscription_required|subscription_inactive|user_not_found"
  }
}
```

## 5.7 GET `/api/v1/relay/active`

Return active or provisioning session for authenticated user.

Response:
- `200 OK` with session
- `204 No Content` if none exists

Example `200`:
```json
{
  "session": {
    "session_id": "ses_01JABCDEF...",
    "status": "active",
    "region": "us-east-1",
    "relay": {
      "instance_id": "i-0abc123...",
      "public_ip": "203.0.113.10",
      "srt_port": 5000
    },
    "timers": {
      "grace_remaining_seconds": 0,
      "max_session_remaining_seconds": 54000
    },
    "usage": {
      "started_at": "2026-02-21T20:00:00Z",
      "duration_seconds": 3600
    }
  }
}
```

## 5.8 POST `/api/v1/relay/stop`

Idempotently stop a relay session.

Request body:
```json
{
  "session_id": "ses_01JABCDEF...",
  "reason": "user_requested|shutdown|admin_forced|error_recovery"
}
```

Rules:
- Repeated calls with same `session_id` return success.
- If session already `stopped`, return terminal state.

Response `200`:
```json
{
  "session_id": "ses_01JABCDEF...",
  "status": "stopped",
  "stopped_at": "2026-02-21T21:15:00Z"
}
```

## 5.9 GET `/api/v1/relay/manifest`

Return launchable region and AMI metadata for relay provisioning.

Response `200`:
```json
{
  "regions": [
    {
      "region": "us-east-1",
      "ami_id": "ami-0123abcd",
      "default_instance_type": "t4g.small",
      "updated_at": "2026-02-21T18:00:00Z"
    },
    {
      "region": "eu-west-1",
      "ami_id": "ami-0456efgh",
      "default_instance_type": "t4g.small",
      "updated_at": "2026-02-21T18:00:00Z"
    }
  ]
}
```

---

## 6. Session State Machine (Backend)

States:
- `provisioning`
- `active`
- `grace`
- `stopped`

Valid transitions:
- `provisioning -> active`
- `active -> grace`
- `grace -> active`
- `active -> stopped`
- `grace -> stopped`

Invalid transitions return `409 conflict` with `invalid_transition`.

---

## 7. Idempotency Semantics

Header:
- `Idempotency-Key` must be UUIDv4 format.

Retention:
- Backend stores key mapping for 1 hour.

Behavior:
- Same user + same key + same endpoint returns original success payload.
- **Async Replay**: Replaying `/relay/start` will reconstruct the session response based on current DB state if the session has progressed since the original record was created.
- Same key with materially different body returns `409 idempotency_mismatch`.

---

## 8. Error Contract

Standard error body:
```json
{
  "error": {
    "code": "invalid_request",
    "message": "Human-readable summary",
    "request_id": "9f27b2ea-4bf2-4b87-9c5f-2ea59a4b8a38",
    "details": {}
  }
}
```

Current implementation note:
- The Go server returns `error.code` and `error.message`.
- `request_id` and structured `details` are not currently populated.

Canonical error codes:
- `invalid_request`
- `unauthorized`
- `forbidden`
- `not_found`
- `conflict`
- `invalid_transition`
- `idempotency_mismatch`
- `rate_limited`
- `session_expired`
- `internal_error`

---

## 9. Usage and Billing Endpoints (Minimal v1)

## 9.1 GET `/api/v1/usage/current`

Returns current cycle usage for Time Bank model.

Response `200`:
```json
{
  "plan_tier": "starter|standard|pro",
  "plan_status": "trial|active|past_due|canceled",
  "cycle_start": "2026-02-01T00:00:00Z",
  "cycle_end": "2026-03-01T00:00:00Z",
  "included_seconds": 54000,
  "consumed_seconds": 12600,
  "remaining_seconds": 41400,
  "overage_seconds": 0
}
```

## 9.2 POST `/api/v1/relay/health` (relay internal)

Used by relay service to report liveness and billing reconciliation data.

Auth:
- **X-Relay-Auth**: Shared relay health secret.
- **Identity Binding**: Request must include both `session_id` and `instance_id`. Backend validates that `instance_id` matches the session.

Request body:
```json
{
  "session_id": "ses_01JABCDEF...",
  "instance_id": "i-0abc123...",
  "ingest_active": true,
  "egress_active": true,
  "session_uptime_seconds": 1820,
  "observed_at": "2026-02-21T20:30:20Z"
}
```

Purpose:
- Watchdog safety checks (C1).
- Outage true-up using `session_uptime_seconds`.

---

## 10. Rate Limits (v1 Defaults)

- `POST /relay/start`: 6 per minute per user.
- `POST /relay/stop`: 20 per minute per user.
- `GET /relay/active`: 60 per minute per user.
- `GET /usage/current`: 30 per minute per user.

Responses include:
- `X-RateLimit-Limit`
- `X-RateLimit-Remaining`
- `X-RateLimit-Reset`

---

## 11. Versioning and Compatibility

- Breaking changes require `/api/v2`.
- Additive fields are allowed in v1.
- Unknown response fields must be ignored by clients.
- C++ plugin must send `X-Aegis-Client-Version` for server-side compatibility policy.

---

## 12. Per-Link Relay Telemetry (Implemented)

### Purpose

When a streamer bonds multiple cellular/WiFi connections through the relay, the dock surfaces individual link health so the streamer can identify weak links.

### Actual Architecture (v0.0.4)

The v0.0.4 implementation bypasses the control plane entirely for per-link stats, using direct HTTP polling from the C++ plugin to the relay's srtla stats server:

```
Phone (T-Mo + WiFi)
    │  SRTLA bonded UDP
    ▼
srtla_rec (relay, port 5000)
    │  per-connection atomic counters
    │  HTTP stats server (port 5080)
    │  optional ASN lookup (GeoLite2-ASN.mmdb)
    ▼
C++ Plugin (PollPerLinkStats, every ~2s)
    │  HTTP GET relay_ip:5080/stats
    │  parses per-connection JSON
    ▼
MetricsCollector (BuildStatusSnapshotJson)
    │  injects relay_links[] into telemetry snapshot
    ▼
CEF ExecuteJavaScript → Dock UI
    per-link BitrateBar with carrier labels
```

### 12.1 Stats Endpoint: `GET :5080/stats`

The custom `srtla_rec` fork (`michaelpentz/srtla`) exposes per-connection stats:

```json
{
  "groups": [{
    "id": "0xaabbccdd...",
    "total_bytes": 123456789,
    "connections": [
      {
        "addr": "192.168.1.42:49201",
        "bytes": 98765432,
        "pkts": 65432,
        "share_pct": 80.0,
        "last_ms_ago": 150,
        "uptime_s": 3600,
        "asn_org": "T-Mobile USA, Inc."
      },
      {
        "addr": "172.58.34.12:50100",
        "bytes": 24691357,
        "pkts": 16432,
        "share_pct": 20.0,
        "last_ms_ago": 200,
        "uptime_s": 3595
      }
    ]
  }]
}
```

Per-connection fields:
- `addr`: Remote IP:port of the bonded link
- `bytes` / `pkts`: Cumulative counters (atomic, relaxed ordering)
- `share_pct`: Percentage of total group traffic carried by this link
- `last_ms_ago`: Milliseconds since last packet (staleness detection)
- `uptime_s`: Duration since link registration
- `asn_org` (optional): ASN organization name from GeoLite2-ASN lookup. Omitted for private IPs or when database is unavailable.

### 12.2 ASN-Based Carrier Identification

When a GeoLite2-ASN.mmdb database is present, `srtla_rec` resolves each connection's source IP to its ISP/carrier name via `libmaxminddb` (memory-mapped, microsecond lookups). The dock UI maps these to short labels:

| ASN Organization | Dock Label |
|-----------------|------------|
| T-Mobile USA, Inc. | T-Mobile |
| AT&T Mobility | AT&T |
| Verizon Wireless | Verizon |
| (unknown/no ASN) | Link N |

Fallback when ASN unavailable: links labeled "Link 1", "Link 2", etc. (no IP leak, safe for on-stream display).

### 12.3 C++ Plugin Integration

- `PerLinkStats` struct: `addr`, `asn_org`, `bytes`, `pkts`, `share_pct`, `last_ms_ago`, `uptime_s`
- `PollPerLinkStats()` polls every ~2s via `HttpsClient::Get()` to `relay_ip:5080/stats`
- `BuildStatusSnapshotJson()` emits `relay_per_link_available`, `relay_conn_count`, and `relay_links[]` array
- Each link in the JSON snapshot includes all fields from the stats endpoint

### 12.4 Dock UI Rendering

- Per-link `BitrateBar` components in the Relay section
- Bitrate computed client-side from delta bytes / delta time
- Share percentage label on each bar
- Links with `last_ms_ago > 3000` rendered with reduced opacity (stale)
- Carrier label from `asn_org` or IP heuristic fallback

### 12.5 Network Requirements

- **TCP 5080** on relay security group must be accessible from the OBS machine
- Stats server binds to `0.0.0.0:5080` (configurable via `--stats_port`)
- No authentication on the stats endpoint (internal relay port)

---

## 13. Multi-Encode / Multi-Upload Per-Output Telemetry (Implemented in v0.0.4)

### Purpose

Streamers running multi-encode setups (e.g., horizontal 1920x1080 for Twitch/Kick/YouTube + vertical 1080x1920 for TikTok/YT Shorts) need per-encoder and per-upload health visible in the dock.

### Current State (v0.0.4)

v0.0.4 collects per-output metrics directly via OBS C API (`obs_enum_outputs`):
- `MetricsCollector` polls per-output stats every 500ms
- Per-output struct: `id`, `name`, `active`, `bitrate_kbps`, `drop_pct`, `fps`, `encoding_lag_ms`, `encoder`, `group`, `resolution`, `hidden`
- JSON snapshot includes `multistream_outputs[]` array
- Dock UI renders per-output bitrate bars with encoder groups

### 13.1 Per-Output Data Model

The C++ `MetricsCollector` enumerates OBS outputs via `obs_enum_outputs` and collects per-output stats. Each output in the `multistream_outputs[]` JSON array contains:

```json
{
  "id": "adv_stream",
  "name": "Twitch",
  "active": true,
  "bitrate_kbps": 6200,
  "drop_pct": 0.01,
  "fps": 60.0,
  "encoding_lag_ms": 2.1,
  "encoder": "nvenc",
  "group": "Horizontal",
  "resolution": "1920x1080",
  "hidden": false
}
```

Encoder name is detected via OBS C API (`obs_output_get_video_encoder` → `obs_encoder_get_name`). Resolution from encoder settings.

### 13.2 Bridge Contract: `getState().outputs`

Top-level section in the bridge projection:

```js
{
  outputs: {
    groups: [
      {
        name: "Horizontal",               // from encoder_group config
        encoder: "x264",                   // from first output in group (if available)
        resolution: "1920x1080",           // from first output in group
        totalBitrateKbps: 18500,           // sum of active outputs in group
        avgLagMs: 2.1,                     // avg encoding_lag_ms across group
        items: [
          {
            id: "adv_stream",
            name: "Twitch",
            active: true,
            bitrateKbps: 6200,
            dropPct: 0.01,
            fps: 60.0,
            lagMs: 2.1,
            status: "healthy"              // derived: healthy/degraded/offline
          },
          {
            id: "adv_stream_2",
            name: "Kick",
            active: true,
            bitrateKbps: 6100,
            dropPct: 0.02,
            fps: 60.0,
            lagMs: 2.0,
            status: "healthy"
          },
          {
            id: "adv_stream_3",
            name: "YT Horizontal",
            active: true,
            bitrateKbps: 6200,
            dropPct: 0.01,
            fps: 60.0,
            lagMs: 2.2,
            status: "healthy"
          }
        ]
      },
      {
        name: "Vertical",
        encoder: "x264",
        resolution: "1080x1920",
        totalBitrateKbps: 8400,
        avgLagMs: 3.0,
        items: [
          {
            id: "adv_stream_4",
            name: "TikTok",
            active: true,
            bitrateKbps: 4200,
            dropPct: 0.03,
            fps: 30.0,
            lagMs: 3.1,
            status: "healthy"
          },
          {
            id: "adv_stream_5",
            name: "YT Shorts",
            active: true,
            bitrateKbps: 4200,
            dropPct: 0.02,
            fps: 30.0,
            lagMs: 2.9,
            status: "healthy"
          }
        ]
      }
    ],
    hidden: [
      { id: "virtualcam_output", name: "Virtual Camera", active: false },
      { id: "adv_file_output", name: "Recording", active: false }
    ],
    totalBitrateKbps: 26900,
    activeCount: 5,
    hiddenCount: 2
  }
}
```

**Bridge logic:**
- Group outputs by `group` field from IPC payload
- Outputs with no group go into an "Ungrouped" default group
- Outputs with `hidden: true` go into the `hidden` array (dock can show/hide via toggle)
- Per-output `status` derived from `drop_pct` + `active`:
  - `active && drop_pct < 0.01` → `"healthy"`
  - `active && drop_pct < 0.05` → `"degraded"`
  - `active && drop_pct >= 0.05` → `"critical"`
  - `!active` → `"offline"`

### 13.3 Dock UI Layout

Target dock rendering for multi-encode/multi-upload:

```
Encoders & Uploads
  ┌─ Horizontal (1920×1080) ────────────────┐
  │  Pool: 18.5 Mbps   Lag: 2.1ms          │
  │                                          │
  │  ● Twitch       6.2 Mbps  60fps  0.01% │
  │  ● Kick         6.1 Mbps  60fps  0.02% │
  │  ● YT Horiz     6.2 Mbps  60fps  0.01% │
  ├─ Vertical (1080×1920) ──────────────────┤
  │  Pool: 8.4 Mbps    Lag: 3.0ms          │
  │                                          │
  │  ● TikTok       4.2 Mbps  30fps  0.03% │
  │  ● YT Shorts    4.2 Mbps  30fps  0.02% │
  └──────────────────────────────────────────┘
  Hidden (2): Recording, Virtual Camera [Show]
```

**Per-output row:** status dot (color-coded) + display name + bitrate + FPS + drop%
**Per-group header:** group name + resolution + pool bitrate + avg lag
**Hidden section:** collapsed by default, expandable, shows count + names
**Inactive outputs within active groups:** dimmed row, "--" for metrics

### 13.4 Settings UI for Output Config

The dock settings page should allow:
1. **Rename outputs** — map OBS output ID to display name
2. **Assign encoder group** — dropdown or text field per output
3. **Toggle visibility** — show/hide per output
4. **Auto-detect** — button to scan OBS outputs and populate list with current IDs

This replaces the v0.0.1 rename modal with a richer config experience.

### 13.5 Implementation Status

- Per-output data collection via OBS C API: **Implemented**
- JSON snapshot includes `multistream_outputs[]`: **Implemented**
- Dock UI renders per-output bitrate bars grouped by encoder: **Implemented**
- Output rename/group/visibility settings UI: **Planned**
- Auto-detect encoder group from resolution: **Planned**

---

## 14. Acceptance Criteria

API v1 is ready when:
1. Endpoints 5.1-5.4 are implemented with integration tests.
2. Idempotency and transition rules are verified by tests.
3. Error contract and rate limits are enforced consistently.
4. Relay health reconciliation data is persisted and queryable.
5. ~~Per-link relay telemetry (section 12) flows from relay to dock.~~ **Done** — direct HTTP polling from C++ plugin.
6. ~~Per-output multi-encode telemetry (section 13) flows to dock.~~ **Done** — C++ collects via OBS C API, renders in dock.
