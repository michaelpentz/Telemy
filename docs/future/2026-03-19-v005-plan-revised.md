# Telemy v0.0.5 Implementation Plan — Revised

**Date:** 2026-03-19
**Author:** Michael Pentz
**Status:** In Progress — Phase 1 complete, Phase 2 C++ core complete — supersedes `2026-03-18-v005-plan.md`
**Baseline:** v0.0.4 (commit `e9ce030`)
**Phase 1:** Complete (field renames, EIP extraction, templatized bootstrap, provider config)

---

## Vision

v0.0.5 introduces a **Connection List** as the primary UI model — borrowing the OBS scenes/sources pattern. Each entry is a named source→relay pairing. The user has a list of connections, each independently connected/disconnected, each with its own relay type, host, and stream ID. The + button adds a new entry.

This replaces the previous single-relay model and enables:
- **Single cam, one relay** — the simple case looks the same, just cleaner
- **Multi-cam IRL** — one backpack cam on US-East, laptop on EU-Central, different stream IDs on same or different relays
- **Multi-tenant collab** — each streamer manages their own connection entry
- **BYOR + managed hybrid** — custom relay alongside a Telemy-managed relay, both visible at a glance
- **Future: failover/backup relay** — ordered priority list, automatic failover between connections

**Auth model:**
- **BYOR = no account, no login, zero friction.** Plugin works fully offline. Connections stored in local `config.json`.
- **Managed relays require login.** Adding a managed connection prompts the user to log in or create an account. BYOR connections remain available regardless of login state.
- Logged-out default state: connection list (BYOR entries work) + "Log in to use Telemy Relays" button.
- Logged-in state: managed relay options appear alongside BYOR in the connection type picker. BYOR connections never disappear.

The `plan_tier = "free"` account concept from the 2026-03-18 plan is dropped. BYOR is not a tier — it's the offline default.

---

## Key Decisions

| Decision | Rationale |
|----------|-----------|
| BYOR requires no account | Zero friction for adoption; OBS plugin utility (telemetry, scene management) is valuable even without Telemy infra |
| Connections are the primary data model, not a single relay config | Enables multi-cam, hybrid, and future failover use cases |
| BYOR connections stored locally in `config.json` only | No server-side dependency; plugin works offline indefinitely |
| Managed connections are per-connection, not per-user | Same user can have multiple managed connections to different relays |
| Sensitive fields (host, port, stream ID) hidden behind expandable secrets area | Privacy-conscious UX; mirrors how OBS handles source settings |
| Phase 2 code rewritten for multi-connection model | The existing single-relay BYOR code is wrong-shaped; reusing it would create tech debt |
| Phase 6 (video stabilization) deferred to v0.0.6 | Infrastructure and business model first; stabilization is independent R&D |

---

## Phase 1 Status (Complete)

Phase 1 is done. The following were completed:

- `ProvisionResult.AWSInstanceID` → `InstanceID`, `DeprovisionRequest.AWSInstanceID` → `InstanceID`
- Migration `0008_rename_instance_id.sql` — `relay_instances.aws_instance_id` → `instance_id`
- EIP logic moved fully inside `AWSProvisioner` (behind `StaticIPManager` interface or internal)
- Bootstrap script extracted to `text/template` in `AWSProvisioner`
- `AEGIS_RELAY_PROVIDER=byor` accepted in `config.go` without crashing

Phase 2 code also exists (from `2026-03-18-byor-implementation-spec.md`) but needs rework for the multi-connection model. See Phase 2 below for what is reusable and what must change.

---

## Phase 2: Multi-Connection Model (Week 1–2)

**Goal:** The dock UI has a Connection List. Users can add BYOR connections (no account required) and managed connections (requires login). Each connection is independently connected/disconnected. Sensitive fields are hidden by default.

### 2.1 Connection Data Model

#### Local Storage (config.json — always used, no account required)

The plugin stores connections as a JSON array in `%APPDATA%/Telemy/config.json`:

```json
{
  "connections": [
    {
      "id": "c1a2b3c4-d5e6-f7a8-b9c0-d1e2f3a4b5c6",
      "name": "Main Cam → My VPS",
      "type": "byor",
      "relay_host": "my-relay.example.com",
      "relay_port": 5000,
      "stream_id": "live/stream123",
      "created_at": "2026-03-19T12:00:00Z"
    },
    {
      "id": "a1b2c3d4-e5f6-a7b8-c9d0-e1f2a3b4c5d6",
      "name": "Backpack → Telemy US-East",
      "type": "managed",
      "managed_region": "us-east",
      "session_id": "",
      "created_at": "2026-03-19T12:00:00Z"
    }
  ]
}
```

Sensitive fields (`relay_host`, `relay_port`, `stream_id`) are stored encrypted via DPAPI in `vault.json`, keyed by connection ID. The `config.json` stores only the non-sensitive metadata (id, name, type, region).

#### Server-Side (Go control plane — only for managed connections)

Managed connections are tracked server-side as sessions. No schema change needed for the connection list itself — managed connections create sessions in the existing `sessions` table. The connection ID in the plugin maps to the `session_id` returned by `/relay/start`.

**No new server-side table for connections.** BYOR connections are local-only. Managed connections are sessions.

#### Migration `0009_free_tier_removal.sql` (replaces `0010_byor_relay_config.sql`)

The single-user `byor_relay_*` columns from the 2026-03-18 spec are **not implemented**. If migration `0010` was already applied to any database, roll it back before applying this one.

```sql
-- 0009_free_tier_removal.sql
-- Remove the byor_relay_* columns if they were added (from draft Phase 2 spec).
-- The multi-connection model stores BYOR configs locally, not server-side.
-- Only run if columns exist; safe to run as no-op otherwise.

ALTER TABLE users DROP COLUMN IF EXISTS byor_relay_host;
ALTER TABLE users DROP COLUMN IF EXISTS byor_relay_port;
ALTER TABLE users DROP COLUMN IF EXISTS byor_stream_id;

-- Remove 'free' from plan_tier if it was added (restoring original constraint).
-- The 'free' tier concept is eliminated; BYOR users have no account.
DO $$
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_constraint
    WHERE conrelid = 'users'::regclass
      AND conname = 'users_plan_tier_check'
  ) THEN
    ALTER TABLE users DROP CONSTRAINT users_plan_tier_check;
    ALTER TABLE users ADD CONSTRAINT users_plan_tier_check
        CHECK (plan_tier IN ('starter', 'standard', 'pro'));
  END IF;
END $$;
```

### 2.2 C++ Architecture: ConnectionManager

The existing `RelayClient` singleton becomes `RelayConnection` — a per-connection object. A new `ConnectionManager` owns the list.

#### ConnectionManager (`src/connection_manager.h` + `.cpp`) — New File

```cpp
struct RelayConnectionConfig {
    std::string id;              // UUID, locally generated
    std::string name;            // User-provided display name
    std::string type;            // "byor" or "managed"
    // BYOR fields (loaded from vault on startup):
    std::string relay_host;
    int         relay_port = 5000;
    std::string stream_id;       // Empty = use live_{stream_token}
    // Managed fields:
    std::string managed_region;  // e.g. "us-east"
    std::string session_id;      // Populated after successful managed provision
    // Runtime state (not persisted):
    std::string status;          // "idle", "connecting", "connected", "error"
    std::string error_msg;
};

class ConnectionManager {
public:
    explicit ConnectionManager(
        ConfigVault* vault,
        HttpsClient* https_client,
        std::function<void()> on_state_changed
    );

    // CRUD — these update config.json + vault.json
    std::string AddConnection(const RelayConnectionConfig& config);   // returns new ID
    void        UpdateConnection(const std::string& id, const RelayConnectionConfig& config);
    void        RemoveConnection(const std::string& id);
    std::vector<RelayConnectionConfig> ListConnections() const;

    // Lifecycle — independent per connection
    void Connect(const std::string& id);      // BYOR: direct; managed: POST /relay/start
    void Disconnect(const std::string& id);   // BYOR: local stop; managed: POST /relay/stop

    // Stats access — called by MetricsCollector
    RelayStats        GetStats(const std::string& id) const;
    PerLinkStats      GetPerLinkStats(const std::string& id) const;

    // Serialize for dock JSON snapshot
    nlohmann::json ToSnapshot() const;

private:
    // One RelayClient per active connection
    std::unordered_map<std::string, std::unique_ptr<RelayClient>> active_clients_;
    std::vector<RelayConnectionConfig>                             connections_;
    mutable std::mutex                                             mutex_;
    ConfigVault*                                                   vault_;
    HttpsClient*                                                   https_client_;
    std::function<void()>                                          on_state_changed_;

    void SaveConnections();        // Persist metadata to config.json
    void LoadConnections();        // Load from config.json + decrypt secrets from vault.json
};
```

#### Changes to Existing RelayClient

`RelayClient` (`src/relay_client.h/.cpp`) is extended minimally:
- `ConnectDirect(host, port, stream_id)` — already implemented, keep as-is
- `DisconnectDirect()` — already implemented, keep as-is
- `IsBYORMode()` — already implemented, keep as-is
- `ConnectManaged(session_result)` — existing path, unchanged
- `StartStatsPolling()` / `StopStatsPolling()` — factored out if not already, so `ConnectionManager` can start polling when ready

The `RelayClient` singleton in `obs_plugin_entry.cpp` is **replaced** by `ConnectionManager`. The `g_relay_client` global becomes `g_connection_manager`.

#### Changes to MetricsCollector

`MetricsCollector::Tick()` iterates over all active connections via `g_connection_manager->ToSnapshot()` and includes a `connections` array in the JSON telemetry snapshot:

```json
{
  "health": 0.95,
  "obs": { ... },
  "system": { ... },
  "connections": [
    {
      "id": "c1a2b3c4...",
      "name": "Main Cam → My VPS",
      "type": "byor",
      "status": "connected",
      "relay_host_masked": "my-relay.e***.com",
      "relay_port": 5000,
      "stats": { "bitrate_kbps": 4200, "rtt_ms": 45, "available": true },
      "per_link": { "available": false }
    },
    {
      "id": "a1b2c3d4...",
      "name": "Backpack → Telemy US-East",
      "type": "managed",
      "status": "connected",
      "stats": { "bitrate_kbps": 6000, "rtt_ms": 31, "available": true },
      "per_link": {
        "available": true,
        "links": [
          { "carrier": "Cox", "bitrate_kbps": 3500, "rtt_ms": 28 },
          { "carrier": "T-Mobile", "bitrate_kbps": 2500, "rtt_ms": 38 }
        ]
      }
    }
  ]
}
```

Note `relay_host_masked` — the actual host is shown in the secrets area, not in the stats card.

#### Config Vault Changes

`ConfigVault` gains connection-keyed methods:

```cpp
// Store sensitive connection fields encrypted via DPAPI
void  SetConnectionSecrets(const std::string& conn_id, const std::string& host, int port, const std::string& stream_id);
bool  GetConnectionSecrets(const std::string& conn_id, std::string& out_host, int& out_port, std::string& out_stream_id);
void  DeleteConnectionSecrets(const std::string& conn_id);
```

`vault.json` structure:

```json
{
  "connections": {
    "c1a2b3c4...": "BASE64_DPAPI_ENCRYPTED_BLOB",
    "a1b2c3d4...": "BASE64_DPAPI_ENCRYPTED_BLOB"
  }
}
```

### 2.3 Dock UI: Connection List

File: `obs-plugin/dock/aegis-dock.jsx` + supporting modules.

#### New UI Structure

Replace the single relay card with a Connection List section:

```
┌─────────────────────────────────────────────┐
│  CONNECTIONS                            [+] │
├─────────────────────────────────────────────┤
│ ● Main Cam → My VPS        [BYOR] [Connect] │
│   ↳ Bitrate: 4,200 kbps · RTT: 45ms        │
│   [▼ Show details]                          │
├─────────────────────────────────────────────┤
│ ● Backpack → Telemy US-East  [MGD] [Stop ■] │
│   ↳ Bitrate: 6,000 kbps · RTT: 31ms        │
│   [▼ Show details]                          │
│     ┌── Per-link ──────────────────────┐    │
│     │ Cox      ████████░░ 3,500 kbps   │    │
│     │ T-Mobile ██████░░░░ 2,500 kbps   │    │
│     └──────────────────────────────────┘    │
├─────────────────────────────────────────────┤
│ ○ Backup → BYOR EU         [BYOR]  [Connect]│
│   [▼ Show details]                          │
└─────────────────────────────────────────────┘
```

Status dot: green = connected, yellow = connecting/provisioning, grey = idle, red = error.

#### Add Connection Flow

Clicking [+] opens a modal/panel:

```
Add Connection
  Name: [__________________________]
  Type: ○ BYOR (custom relay)   ○ Telemy Managed Relay

  [If BYOR selected:]
  Host:       [________________________________]
  Port:       [5000]
  Stream ID:  [________________________________] (optional)
              ← Sensitive — stored encrypted locally →

  [If Managed selected and NOT logged in:]
  ← Log in or sign up to use Telemy Managed Relays →
  [Log In]  [Sign Up]

  [If Managed selected and logged in:]
  Region:  [US East ▼]

  [Cancel]  [Add Connection]
```

#### Per-Connection Expanded View (Secrets Area)

Clicking [▼ Show details] expands inline:

```
  Details (click to collapse ▲)
  ┌─────────────────────────────────────────┐
  │ Host:      my-relay.example.com    [📋] │
  │ Port:      5000                         │
  │ Stream ID: live/stream123          [📋] │
  │ Status:    Connected                    │
  │ Session:   byor-local-...               │
  └─────────────────────────────────────────┘
  [Edit]  [Remove]
```

For managed connections, the expanded view shows the session ID and relay IP (after provisioning).

#### Managed Relay Inline Provisioning Progress

When a managed connection is starting, the row shows an inline progress bar instead of opening a full-screen modal:

```
│ ◌ Backpack → Telemy US-East  [MGD] [Cancel]│
│   Starting relay...  ████░░░░░░  40%       │
│   Creating stream IDs...                    │
```

The 6-step provision progress (from v0.0.4) condenses to an inline bar within the connection row.

#### Auth State in Dock

The dock header shows auth state:

```
Logged out:   [Sign In to Telemy]  ← subtle, not intrusive
Logged in:    Michael P. · Starter [Account ▼]
```

BYOR connections work and show stats regardless of this state.

#### JSX Component Structure

New components:
- `ConnectionListSection` — renders the list, + button, empty state
- `ConnectionRow` — single connection: status dot, name, type badge, action button, inline stats
- `ConnectionExpandedDetail` — secrets area, edit/remove buttons
- `AddConnectionModal` — type picker, host/port/stream_id fields, region picker, auth prompt
- `ManagedProvisionProgress` — inline progress bar for managed connection startup

Existing components that feed into this:
- `RelayStatsCard` — repurposed into per-connection stats section inside `ConnectionRow`
- `PerLinkBars` — embedded in `ConnectionRow` expanded view
- `ProvisionProgress` — the existing modal version is retired; replace with `ManagedProvisionProgress`

#### Dock-to-Native Bridge

The bridge message protocol gains new actions:

```js
// From dock to native (user actions):
window.aegisDockNative.postMessage({ type: "connection_add", config: { name, type, host, port, stream_id, region } })
window.aegisDockNative.postMessage({ type: "connection_remove", id: "c1a2b3c4..." })
window.aegisDockNative.postMessage({ type: "connection_connect", id: "c1a2b3c4..." })
window.aegisDockNative.postMessage({ type: "connection_disconnect", id: "c1a2b3c4..." })
window.aegisDockNative.postMessage({ type: "connection_update", id: "...", config: { ... } })

// From native to dock (telemetry snapshot already includes connections array)
// No new push messages needed — the existing 500ms snapshot tick carries all state
```

The plugin entry dispatch (`obs_plugin_entry.cpp`) handles these new action types, delegating to `ConnectionManager`.

### 2.4 Control Plane Changes for Multi-Connection

The managed connection flow remains: `POST /api/v1/relay/start` → provisions relay → returns session. The new addition is a `connection_id` parameter so the plugin can correlate server sessions with local connection entries.

#### New Parameter: connection_id

Add optional `connection_id` field to relay start request:

```json
POST /api/v1/relay/start
{
  "region": "us-east",
  "connection_id": "a1b2c3d4-e5f6-a7b8-c9d0-e1f2a3b4c5d6"
}
```

The control plane stores `connection_id` in the `sessions` table (new column via migration) and returns it in the session response. The plugin uses this to correlate sessions to local connection entries after restart.

#### Multiple Active Sessions Per User

The existing `handleRelayStart()` calls `StartOrGetSession()`, which may enforce single-session-per-user. For multi-connection support, this must allow multiple concurrent sessions per user — each with a different `connection_id`.

Change `StartOrGetSession()` to check for an active session matching the `connection_id` (not just the `user_id`). Two connections to the same relay type in the same region are allowed.

#### Migration `0009_multi_connection.sql`

```sql
-- 0009_multi_connection.sql
-- Add connection_id to sessions table for multi-connection plugin support.

ALTER TABLE sessions ADD COLUMN connection_id TEXT;
CREATE INDEX sessions_user_connection_idx ON sessions(user_id, connection_id)
    WHERE connection_id IS NOT NULL AND status NOT IN ('stopped', 'error');
```

#### handleRelayStop() Connection Scoping

`POST /api/v1/relay/stop` gains optional `connection_id` body parameter. If provided, only the session matching that `connection_id` is stopped. If absent, falls back to current behavior (stop any active session for the user).

#### BYORProvisioner (from existing Phase 2 spec)

The `BYORProvisioner` struct from `2026-03-18-byor-implementation-spec.md` is **not needed** in the multi-connection model. BYOR connections never touch the control plane. The provisioner exists for interface completeness per the existing spec, but `handleRelayStart()` no longer routes BYOR traffic — the plugin never calls `/relay/start` for BYOR connections. The `BYORProvisioner` can be kept as a stub or removed.

#### Auth Session Response

`GET /auth/session` no longer returns `relay_mode` or `byor_config`. BYOR is fully local. The session response returns entitlement info for managed relays (existing) and, optionally, the list of active managed sessions (to allow the plugin to re-sync connection state after restart):

```json
{
  "user_id": "...",
  "email": "...",
  "entitlement": {
    "relay_access_status": "enabled",
    "plan_tier": "pro",
    "reason_code": ""
  },
  "active_sessions": [
    {
      "session_id": "ses_...",
      "connection_id": "a1b2c3d4...",
      "status": "active",
      "public_ip": "...",
      "srt_port": 5000,
      "stream_token": "live_..."
    }
  ]
}
```

### 2.5 Phase 2 Implementation Status (2026-03-20)

The following Phase 2 C++ items are **complete**:

- **ConnectionManager** (`connection_manager.cpp/.h`) — owns all RelayClient instances, background stats polling thread, CRUD, SaveConnections/LoadConnections
- **MetricsCollector background thread** — `Start()`/`Stop()`/`PollLoop()` implemented; Poll() no longer on OBS render thread
- **E2E gap fixes** — snapshot push on connect/disconnect, `relay_host_masked` field, `not_found` errors for unknown connection IDs
- **Dock UI polish** — separate font/density axes, C1 sanitizer, density-aware spacing, per-link freshness indicators

Remaining Phase 2 work:
- Dock connection list UI (add/remove/edit connections, + button, connection rows)
- `connection_id`-scoped managed relay provisioning with inline progress
- Logged-out state ("Log in for managed relays" prompt)

### 2.5 What Existing Phase 2 Code is Reusable vs Needs Rework

| Component | Status | Notes |
|-----------|--------|-------|
| `ConnectDirect()` in `relay_client.cpp` | **Reuse** | Becomes `RelayClient::ConnectDirect()` used by `ConnectionManager`; no changes needed |
| `DisconnectDirect()` | **Reuse** | Same |
| `IsBYORMode()` + `byor_mode_` atomic | **Reuse** | Per-connection property, still valid |
| Stats polling graceful degradation (`PollRelayStats`, `PollPerLinkStats`) | **Reuse** | Logic unchanged; works per `RelayClient` instance |
| `BYORConfig` struct in `relay_client.h` | **Reuse** | Rename to `BYORConnectionConfig` or keep as-is |
| `BYORProvisioner` Go struct | **Remove or stub** | BYOR no longer hits the control plane |
| `store.GetUserBYORConfig/SetUserBYORConfig` | **Remove** | Single-user BYOR config columns are dropped |
| `POST /api/v1/user/relay-config` endpoint | **Remove** | Replaced by connection-scoped relay start |
| `handleRelayStart()` BYOR routing gate | **Remove** | BYOR never reaches the control plane |
| `model.RelayEntitlement.RelayMode` field | **Remove** | No more `relay_mode: "byor"` concept |
| Auth session `byor_config` response block | **Remove** | BYOR config is local-only |
| Dock UI: single relay source toggle | **Remove** | Replaced by connection list + add modal |
| Migration `0010_byor_relay_config.sql` | **Replace** | Drop columns; use `0009_multi_connection.sql` instead |
| `0010_byor_relay_config.sql` CHECK constraint change | **Replace** | No `free` tier; restore original constraint |

### 2.6 Phase 2 Deliverable

At the end of Phase 2:
1. User opens dock — sees empty Connection List with + button
2. Clicks + → selects BYOR, enters host/port/stream_id → connection added instantly, no account needed
3. Clicks Connect → plugin calls `ConnectDirect()` on that connection's `RelayClient`
4. Stats appear inline in the connection row (if relay runs SLS)
5. User adds a second BYOR connection → both connections show independently, can connect/disconnect each
6. User tries to add Managed connection while logged out → sees "Log in to use Telemy Relays" prompt
7. Logged-in user adds Managed connection → `POST /relay/start` with `connection_id`, provisioning progress inline

---

## Phase 3: Hetzner Shared Relay + Multi-Connection (Week 2–3)

**Goal:** Managed connections provision onto shared always-on Hetzner relays in <2 seconds. Multiple managed connections per user are supported.

### 3.1 Relay Pool Schema

Migration `0010_relay_pool.sql` (unchanged from original plan, renumbered):

```sql
CREATE TABLE relay_pool (
    server_id         TEXT PRIMARY KEY,
    provider          TEXT NOT NULL DEFAULT 'hetzner',
    host              TEXT NOT NULL,
    ip                INET NOT NULL,
    region            TEXT NOT NULL,
    status            TEXT NOT NULL DEFAULT 'active',
    current_sessions  INTEGER NOT NULL DEFAULT 0,
    max_sessions      INTEGER NOT NULL DEFAULT 10,
    created_at        TIMESTAMPTZ DEFAULT NOW(),
    last_health_check TIMESTAMPTZ,
    health_status     TEXT DEFAULT 'unknown'
);

CREATE TABLE relay_assignments (
    id            SERIAL PRIMARY KEY,
    user_id       TEXT NOT NULL,
    session_id    TEXT NOT NULL,
    connection_id TEXT,                                -- maps to plugin connection_id
    server_id     TEXT NOT NULL REFERENCES relay_pool(server_id),
    stream_token  TEXT NOT NULL,
    assigned_at   TIMESTAMPTZ DEFAULT NOW(),
    released_at   TIMESTAMPTZ,
    UNIQUE(session_id)
);
```

The `relay_instances` table is phased out for the Hetzner path. See original plan §3.1 for the full redesign rationale.

### 3.2 HetznerProvisioner

Unchanged from original plan §3.2. The `connection_id` flows through as part of the provision request metadata, stored in `relay_assignments.connection_id` so the pool assignment can be correlated back to the plugin connection.

### 3.3 Per-Connection Assignment

Each managed connection gets its own relay assignment — even if two connections from the same user land on the same relay server (same IP, different stream tokens). The assignment logic (§3.5 of the original plan) is unchanged; it operates per session/connection, not per user.

**DNS for shared relays:** Multiple users share the same relay IP. DNS record (`{slug}.relay.telemyapp.com`) points to the shared server. For multi-connection, the OBS plugin displays the relay hostname in the connection's expanded detail section. Different connections to the same relay server will show the same hostname — that's correct and expected.

### 3.4 Provision Pipeline Inline Progress

The existing 6-step provision pipeline drives the inline `ManagedProvisionProgress` bar in the connection row. The SSE or WebSocket events (or the polling the plugin already does) are scoped by `connection_id` / `session_id`. The dock matches events to connection rows by `connection_id`.

For Hetzner, provision is near-instant (§3.6 option A from original plan). The inline bar flashes through quickly.

### 3.5 Phase 3 Deliverable

At the end of Phase 3:
1. Paid user adds a Managed connection → assigned to Hetzner relay in <2 seconds
2. Second managed connection (same user) → independently assigned, potentially same or different relay server
3. Both connections show independently in the Connection List, each with their own stats
4. Stop one connection → that stream token removed from relay, server stays running; other connection unaffected
5. Cost: ~$0.50/user/month vs $9–13/user/month on AWS

---

## Phase 4: Control Plane Migration (Week 3–4)

Unchanged from original plan §4 (§4.1–4.5). Hetzner CX22 (~$5/mo), PostgreSQL migration, DNS cutover, 48h EC2 overlap.

**Additional verification for multi-connection:**

- [ ] Two simultaneous managed connections from one user both provision correctly
- [ ] `GET /auth/session` returns both active sessions in `active_sessions` array
- [ ] Stopping one connection does not affect the other

---

## Phase 5: Entitlement & Pricing (Week 4)

**The auth model change affects entitlement significantly.**

### 5.1 New Tier Definitions

| Tier | `plan_tier` | Relay Access | Monthly Price | Includes |
|------|------------|--------------|---------------|----------|
| *(no account)* | N/A | BYOR only | $0 | Plugin + telemetry + scene management + unlimited BYOR connections |
| **Starter** | `starter` | 1 Managed connection | $8–10/mo | 1 concurrent managed relay, 40 hrs/month |
| **Pro** | `pro` | Up to 3 Managed connections | $20–30/mo | 3 concurrent managed relays, 120 hrs/month, priority assignment |
| **Self-Hosted** | N/A | BYOR + guide | $0 | Same as no-account + Docker stack deployment guide |

Key change: the number of concurrent **managed connections** is the entitlement gate, not just "relay access."

### 5.2 Multi-Connection Entitlement Enforcement

`GetRelayEntitlement()` returns:

```go
type RelayEntitlement struct {
    PlanTier               string
    PlanStatus             string
    RelayAccessStatus      string  // "enabled", "disabled", "subscription_required"
    MaxConcurrentConns     int     // 0 = not allowed; 1 = Starter; 3 = Pro
    ActiveManagedConns     int     // count of currently active managed sessions
    IncludedSeconds        int
    ConsumedSeconds        int
    RemainingSeconds       int
    Allowed                bool
    ReasonCode             string
}
```

`handleRelayStart()` checks `ActiveManagedConns < MaxConcurrentConns` before provisioning. Returns `"connection_limit_reached"` if at limit.

### 5.3 No Login Required for BYOR — Client-Side Enforcement

Since BYOR connections never reach the control plane, there is no server-side enforcement needed. The dock UI enforces it: clicking + for a Managed connection while logged out shows the auth prompt. The + button for BYOR connections is always enabled.

### 5.4 Entitlement in Auth Session Response

```go
entitlementMap := map[string]any{
    "relay_access_status":   "enabled",
    "plan_tier":             "pro",
    "max_concurrent_conns":  3,
    "active_managed_conns":  1,
    "remaining_seconds":     432000,
    "included_seconds":      432000,
}
```

The dock uses `max_concurrent_conns` to decide whether to grey out the "Add Managed Connection" button in the + modal.

### 5.5 Self-Hosted Option

Unchanged from original plan §5.4. Public Docker Compose guide, BYOR mode in plugin, `ghcr.io/michaelpentz/srtla-receiver:latest`.

---

## Phase 5a: Billing Integration (Week 4–5)

Unchanged from original plan §5a. LemonSqueezy recommended for v0.0.5 solo dev speed; Stripe later if needed.

**Multi-connection billing note:** Starter tier limits to 1 concurrent managed connection; Pro limits to 3. The webhook handler updates `plan_tier` on subscription events; the `max_concurrent_conns` is derived from `plan_tier` in `GetRelayEntitlement()` — no extra column needed.

---

## Phase 6: Video Stabilization (v0.0.6 — Deferred)

Explicitly deferred. Build the multi-connection relay infrastructure and billing first. Phase 6 design unchanged from original plan §6.

---

## C++ File Change Summary

| File | Change |
|------|--------|
| `src/connection_manager.h` | **NEW** — `ConnectionManager`, `RelayConnectionConfig` struct |
| `src/connection_manager.cpp` | **NEW** — `Add/Remove/Connect/Disconnect`, CRUD, `ToSnapshot()` |
| `src/relay_client.h` | Keep `ConnectDirect`, `DisconnectDirect`, `IsBYORMode`; `BYORConfig` struct |
| `src/relay_client.cpp` | Keep `ConnectDirect` path; no other changes for Phase 2 |
| `src/config_vault.h/.cpp` | Add `SetConnectionSecrets`, `GetConnectionSecrets`, `DeleteConnectionSecrets` |
| `src/obs_plugin_entry.cpp` | Replace `g_relay_client` singleton with `g_connection_manager`; handle new bridge actions: `connection_add`, `connection_remove`, `connection_connect`, `connection_disconnect` |
| `src/metrics_collector.cpp` | Call `g_connection_manager->ToSnapshot()` for `connections` array in telemetry JSON |
| `dock/aegis-dock.jsx` | Replace relay card with `ConnectionListSection`; add `ConnectionRow`, `ConnectionExpandedDetail`, `AddConnectionModal`, `ManagedProvisionProgress` components |
| `dock/ui-components.jsx` | New shared UI: `SecretField`, `ConnectionTypeBadge`, `StatusDot` |

## Go Control Plane File Change Summary

| File | Change |
|------|--------|
| `internal/api/handlers.go` | `handleRelayStart()`: add `connection_id` param; allow multi-session per user; add `connection_id` to session response. `handleRelayStop()`: scope by `connection_id` |
| `internal/api/auth_handlers.go` | `buildAuthSessionResponse()`: return `active_sessions` array; add `max_concurrent_conns` to entitlement block; **remove** `relay_mode` and `byor_config` |
| `internal/store/store.go` | `GetRelayEntitlement()`: add `MaxConcurrentConns` and `ActiveManagedConns`; **remove** `GetUserBYORConfig/SetUserBYORConfig/ClearUserBYORConfig` |
| `internal/model/types.go` | `RelayEntitlement`: add `MaxConcurrentConns`, `ActiveManagedConns`; **remove** `RelayMode` |
| `internal/relay/byor.go` | **Remove** or keep as empty stub (BYOR no longer uses control plane) |
| `migrations/0009_multi_connection.sql` | **NEW** — `connection_id` column on `sessions` table |
| `migrations/0010_relay_pool.sql` | **NEW** — `relay_pool` and `relay_assignments` tables (Phase 3) |

---

## Migration Strategy

Pre-migration steps same as original plan (tag `v0.0.4-stable`, branch `v0.0.5-dev`).

**Critical:** Do NOT apply `0010_byor_relay_config.sql` (from 2026-03-18 draft spec). If it was applied to any environment, run `0009_free_tier_removal.sql` first.

Migration sequence for a clean database:
```
0001_init.sql           (existing)
0002 ... 0008           (existing + Phase 1 renames)
0009_multi_connection   (Phase 2: connection_id on sessions)
0010_relay_pool         (Phase 3: Hetzner pool tables)
```

---

## Open Questions

### 1. Should BYOR connections optionally sync to the server for cross-machine use?

A logged-in user with both BYOR and managed connections may want their BYOR config to follow them across machines. Options:
- **A (simple):** No sync. Each machine has its own local connections. User re-enters BYOR credentials per machine.
- **B (v0.0.6+):** Optional cloud sync: if logged in, BYOR connection metadata (not secrets) syncs to server. Secrets remain local.

**Recommendation:** Option A for v0.0.5. Reduce scope.

### 2. Should connection ordering be persisted?

OBS scenes/sources are ordered. Users may want to reorder connections (e.g., primary relay first).

**Recommendation:** Store an `order` field in the local `config.json` array. Simple array index ordering. No server-side impact.

### 3. How many concurrent BYOR connections should be allowed?

BYOR connections are local-only and free. The plugin should not artificially limit them.

**Recommendation:** No limit. If a user has 5 BYOR connections, all 5 can be simultaneously connected. Stats polling overhead is minimal (one HTTP call per connection per 500ms).

### 4. Should the old ProvisionProgress full-screen modal be kept alongside the new inline progress?

**Recommendation:** Remove the full-screen modal entirely. The inline progress in the connection row is sufficient and cleaner. The 6-step progress UI is adapted to fit the row.

### 5. What happens to the existing single-relay API on existing deployed OBS plugins (v0.0.4)?

The v0.0.4 plugin calls `POST /relay/start` without a `connection_id`. The control plane must handle this gracefully — treat missing `connection_id` as `null` (backward compatible). The `active_sessions` field in `GET /auth/session` is additive only. v0.0.4 clients will ignore it.

---

## Success Criteria

- [ ] User can add a BYOR connection without creating an account or logging in
- [ ] BYOR connection connects, shows stats, disconnects — no control plane calls
- [ ] User can have 3+ BYOR connections and connect/disconnect them independently
- [ ] Logged-in Starter user can add 1 managed connection; second managed connection is blocked with clear error
- [ ] Logged-in Pro user can add up to 3 managed connections simultaneously
- [ ] Each managed connection provisions a Hetzner relay in <2 seconds (Phase 3)
- [ ] Removing a connection removes its secrets from vault.json
- [ ] Stats in Connection List are per-connection, correctly isolated
- [ ] All existing v0.0.4 managed relay functionality still works on the v0.0.5 control plane
- [ ] `grep -r "AWSInstanceID" internal/` returns zero hits (Phase 1, already done)

---

## Cost Impact

Unchanged from original plan. At 100 paying users (Starter/Pro mix), Hetzner shared relays cost ~$26/month. BYOR users cost Telemy $0.

---

## Revision History

| Date | Revision | Notes |
|------|----------|-------|
| 2026-03-18 | v1 | Initial plan (single-relay BYOR model) |
| 2026-03-18 | v2 | Review corrections (PostgreSQL refs, effort estimates, billing integration, scoped Phase 6 to v0.0.6) |
| 2026-03-19 | v3 | **Full revision** — multi-connection model. BYOR no longer requires an account. Connection List replaces single-relay UI. `ConnectionManager` replaces `RelayClient` singleton. Multi-session-per-user entitlement. Existing Phase 2 single-BYOR-config code replaced. Phases 3–5a updated for multi-connection context. |
