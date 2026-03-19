# BYOR (Bring Your Own Relay) Provisioner — Implementation Spec

**Date:** 2026-03-18
**Version:** v0.0.5 Phase 2
**Author:** Generated from codebase analysis
**Status:** Ready for implementation
**Prerequisite:** Phase 1 field renames (see v005-plan.md §1.1–1.3)

---

## 1. BYORProvisioner (Go Control Plane)

### 1.1 Struct & Interface Implementation

File: `internal/relay/byor.go` (new file)

```go
package relay

import (
    "context"
    "errors"

    "github.com/telemyapp/aegis-control-plane/internal/store"
)

// ErrBYORNoManagedProvision is returned when a BYOR user attempts to use
// the managed provision pipeline. BYOR users bypass runProvisionPipeline
// entirely; this error exists as a safety net.
var ErrBYORNoManagedProvision = errors.New("byor: managed provisioning not available for BYOR users")

// BYORProvisioner implements the Provisioner interface for users who bring
// their own SRTLA relay endpoint. It performs no infrastructure provisioning.
type BYORProvisioner struct {
    store *store.Store
}

func NewBYORProvisioner(s *store.Store) *BYORProvisioner {
    return &BYORProvisioner{store: s}
}

// Provision returns the user's pre-configured BYOR relay endpoint.
// It reads byor_relay_host, byor_relay_port, and byor_stream_id from the
// users table. If the user has not configured a BYOR endpoint, it returns
// ErrBYORNotConfigured.
//
// This method exists for interface completeness. In the normal BYOR flow,
// the plugin never calls /relay/start — it connects directly using the
// endpoint from GET /auth/session. This method is only reached if the
// routing logic in handleRelayStart() has a bug.
func (p *BYORProvisioner) Provision(ctx context.Context, req ProvisionRequest) (ProvisionResult, error) {
    cfg, err := p.store.GetUserBYORConfig(ctx, req.UserID)
    if err != nil {
        return ProvisionResult{}, err
    }
    if cfg.RelayHost == "" {
        return ProvisionResult{}, ErrBYORNotConfigured
    }
    return ProvisionResult{
        InstanceID:   "byor-" + req.UserID,  // synthetic; no real instance
        PublicIP:     cfg.RelayHost,
        SRTPort:      cfg.RelayPort,
    }, nil
}

// Deprovision is a no-op. BYOR relays are user-managed infrastructure;
// Telemy has nothing to tear down.
func (p *BYORProvisioner) Deprovision(_ context.Context, _ DeprovisionRequest) error {
    return nil
}

var ErrBYORNotConfigured = errors.New("byor: relay endpoint not configured")
```

### 1.2 Key Design Decisions

**Why the provisioner reads from the database rather than from the ProvisionRequest:**
The `ProvisionRequest` struct carries `SessionID`, `UserID`, `Region`, and `StreamToken` — none of which include the BYOR endpoint. Rather than modifying the shared `ProvisionRequest` struct (which would leak BYOR concepts into the AWS and Hetzner paths), the `BYORProvisioner` reads the user's saved config via `store.GetUserBYORConfig()`.

**Why Provision() exists at all if BYOR users bypass the pipeline:**
Defense in depth. If routing logic changes or a BYOR user somehow reaches the provision pipeline, the provisioner returns their configured endpoint rather than crashing. It also allows future use cases like "validate BYOR endpoint on session start."

### 1.3 ProvisionResult Changes (Phase 1 prerequisite)

Per the v005-plan Phase 1.1, the following renames MUST be completed first:

| Before (v0.0.4) | After (v0.0.5) |
|---|---|
| `ProvisionResult.AWSInstanceID` | `ProvisionResult.InstanceID` |
| `DeprovisionRequest.AWSInstanceID` | `DeprovisionRequest.InstanceID` |
| `model.Session.RelayAWSInstanceID` | `model.Session.RelayInstanceID` (the string field, not the FK pointer) |
| `ActivateProvisionedSessionInput.AWSInstanceID` | `ActivateProvisionedSessionInput.InstanceID` |

The BYOR provisioner depends on these neutral field names.

---

## 2. Database Changes

### 2.1 Migration: `0010_byor_relay_config.sql`

The codebase uses PostgreSQL via `pgx/v5` (confirmed in `store.go` imports and `0001_init.sql` which uses `timestamptz`, `inet`, `bigserial`, partial indexes). All migrations must be PostgreSQL dialect.

```sql
-- 0010_byor_relay_config.sql
-- Add BYOR relay configuration columns to users table.
-- These are nullable: NULL means the user has not configured a BYOR endpoint.

ALTER TABLE users ADD COLUMN byor_relay_host TEXT;
ALTER TABLE users ADD COLUMN byor_relay_port INTEGER DEFAULT 5000;
ALTER TABLE users ADD COLUMN byor_stream_id TEXT;

-- Validate port range when set
ALTER TABLE users ADD CONSTRAINT users_byor_port_range
    CHECK (byor_relay_port IS NULL OR (byor_relay_port >= 1 AND byor_relay_port <= 65535));

-- Update plan_tier CHECK to allow 'free' tier for BYOR users.
-- The existing constraint (from 0001_init.sql line 12) is:
--   CHECK (plan_tier IN ('starter', 'standard', 'pro'))
-- We must drop and re-add it because PostgreSQL does not support ALTER CHECK.

ALTER TABLE users DROP CONSTRAINT users_plan_tier_check;
ALTER TABLE users ADD CONSTRAINT users_plan_tier_check
    CHECK (plan_tier IN ('free', 'starter', 'standard', 'pro'));

COMMENT ON COLUMN users.byor_relay_host IS 'User-provided SRTLA relay hostname or IP. NULL = no BYOR config.';
COMMENT ON COLUMN users.byor_relay_port IS 'User-provided SRTLA relay port. Defaults to 5000 (standard SRTLA).';
COMMENT ON COLUMN users.byor_stream_id IS 'User-provided SLS stream ID (e.g. "live/my_stream"). NULL = use default live_{stream_token}.';
```

**Important notes:**

1. The existing CHECK constraint on `plan_tier` in `0001_init.sql` only allows `'starter'`, `'standard'`, `'pro'`. PostgreSQL names this constraint `users_plan_tier_check` by convention. The migration drops and re-creates it to add `'free'`. Verify the actual constraint name with: `SELECT conname FROM pg_constraint WHERE conrelid = 'users'::regclass AND contype = 'c';`

2. The `byor_relay_host` column stores either a hostname (e.g., `my-relay.example.com`) or an IPv4/IPv6 address. We intentionally use `TEXT` rather than `inet` because it may be a hostname, not just an IP.

3. The `byor_stream_id` column is optional. If NULL, the plugin should fall back to the user's standard `live_{stream_token}` stream ID. This accommodates users whose BYOR relay is already configured with Telemy-style stream tokens (e.g., self-hosted srtla-receiver users).

### 2.2 Store Methods

Add to `internal/store/store.go`:

```go
// BYORConfig holds a user's Bring Your Own Relay endpoint configuration.
type BYORConfig struct {
    RelayHost string
    RelayPort int
    StreamID  string // empty = use default live_{stream_token}
}

// GetUserBYORConfig reads the BYOR relay columns from the users table.
// Returns a zero-value BYORConfig (empty RelayHost) if not configured.
func (s *Store) GetUserBYORConfig(ctx context.Context, userID string) (*BYORConfig, error) {
    const q = `
SELECT coalesce(byor_relay_host, ''),
       coalesce(byor_relay_port, 5000),
       coalesce(byor_stream_id, '')
FROM users WHERE id = $1`
    var cfg BYORConfig
    if err := s.db.QueryRow(ctx, q, userID).Scan(
        &cfg.RelayHost, &cfg.RelayPort, &cfg.StreamID,
    ); err != nil {
        if errors.Is(err, pgx.ErrNoRows) {
            return nil, ErrNotFound
        }
        return nil, err
    }
    return &cfg, nil
}

// SetUserBYORConfig upserts the BYOR relay columns on the users table.
func (s *Store) SetUserBYORConfig(ctx context.Context, userID string, cfg BYORConfig) error {
    const q = `
UPDATE users
SET byor_relay_host = $2,
    byor_relay_port = $3,
    byor_stream_id  = $4,
    updated_at      = now()
WHERE id = $1`
    tag, err := s.db.Exec(ctx, q, userID, nilIfEmpty(cfg.RelayHost), cfg.RelayPort, nilIfEmpty(cfg.StreamID))
    if err != nil {
        return err
    }
    if tag.RowsAffected() == 0 {
        return ErrNotFound
    }
    return nil
}

// ClearUserBYORConfig removes the BYOR relay config (sets columns to NULL).
func (s *Store) ClearUserBYORConfig(ctx context.Context, userID string) error {
    const q = `
UPDATE users
SET byor_relay_host = NULL,
    byor_relay_port = NULL,
    byor_stream_id  = NULL,
    updated_at      = now()
WHERE id = $1`
    _, err := s.db.Exec(ctx, q, userID)
    return err
}

func nilIfEmpty(s string) *string {
    if s == "" {
        return nil
    }
    return &s
}
```

---

## 3. New API Endpoint: POST /api/v1/user/relay-config

### 3.1 Request/Response

File: `internal/api/handlers.go` (add handler + register route)

**Request:**
```json
POST /api/v1/user/relay-config
Authorization: Bearer <cp_access_jwt>

{
    "relay_host": "my-relay.example.com",
    "relay_port": 5000,
    "stream_id": "live/my_stream"
}
```

All fields are optional on update (PATCH semantics on POST). An empty `relay_host` clears the BYOR config entirely.

**Response (200 OK):**
```json
{
    "relay_config": {
        "relay_host": "my-relay.example.com",
        "relay_port": 5000,
        "stream_id": "live/my_stream",
        "health_check": {
            "reachable": true,
            "latency_ms": 42,
            "checked_at": "2026-03-18T14:00:00Z"
        }
    }
}
```

**Response (health check failed, still saved):**
```json
{
    "relay_config": {
        "relay_host": "my-relay.example.com",
        "relay_port": 5000,
        "stream_id": "live/my_stream",
        "health_check": {
            "reachable": false,
            "error": "connection refused",
            "checked_at": "2026-03-18T14:00:00Z"
        }
    },
    "warning": "Relay endpoint is not reachable. Config saved — you can update it later."
}
```

### 3.2 Handler Implementation

```go
type relayConfigRequest struct {
    RelayHost string `json:"relay_host"`
    RelayPort int    `json:"relay_port"`
    StreamID  string `json:"stream_id"`
}

func (s *Server) handleSetRelayConfig(w http.ResponseWriter, r *http.Request) {
    userID, ok := auth.UserIDFromContext(r.Context())
    if !ok {
        writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
        return
    }

    var req relayConfigRequest
    if code, msg := decodeJSON(r, &req); code != 0 {
        writeAPIError(w, code, "invalid_request", msg)
        return
    }

    // Clear config if relay_host is empty
    if req.RelayHost == "" {
        if err := s.store.ClearUserBYORConfig(r.Context(), userID); err != nil {
            writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to clear relay config")
            return
        }
        writeJSON(w, http.StatusOK, map[string]any{"relay_config": nil})
        return
    }

    // Validate inputs
    if len(req.RelayHost) > 253 {
        writeAPIError(w, http.StatusBadRequest, "invalid_request", "relay_host too long (max 253 chars)")
        return
    }
    if req.RelayPort == 0 {
        req.RelayPort = 5000
    }
    if req.RelayPort < 1 || req.RelayPort > 65535 {
        writeAPIError(w, http.StatusBadRequest, "invalid_request", "relay_port must be 1-65535")
        return
    }
    if len(req.StreamID) > 256 {
        writeAPIError(w, http.StatusBadRequest, "invalid_request", "stream_id too long (max 256 chars)")
        return
    }

    // Optional health check (non-blocking, best-effort)
    var healthResult map[string]any
    healthResult = probeBYOREndpoint(req.RelayHost, req.RelayPort)

    // Save regardless of health check result
    cfg := store.BYORConfig{
        RelayHost: req.RelayHost,
        RelayPort: req.RelayPort,
        StreamID:  req.StreamID,
    }
    if err := s.store.SetUserBYORConfig(r.Context(), userID, cfg); err != nil {
        writeAPIError(w, http.StatusInternalServerError, "internal_error", "failed to save relay config")
        return
    }

    resp := map[string]any{
        "relay_config": map[string]any{
            "relay_host":   cfg.RelayHost,
            "relay_port":   cfg.RelayPort,
            "stream_id":    cfg.StreamID,
            "health_check": healthResult,
        },
    }
    if !healthResult["reachable"].(bool) {
        resp["warning"] = "Relay endpoint is not reachable. Config saved — you can update it later."
    }
    writeJSON(w, http.StatusOK, resp)
}
```

### 3.3 Health Check Implementation

```go
// probeBYOREndpoint performs a best-effort reachability check on the user's
// BYOR relay. It tries two things:
//   1. TCP connect to relay_host:8090 (SLS stats API) with 3s timeout
//   2. If TCP succeeds, GET http://relay_host:8090/health with 3s timeout
//
// Returns a map suitable for JSON response. The health check is advisory;
// the config is saved regardless of the result.
func probeBYOREndpoint(host string, port int) map[string]any {
    result := map[string]any{
        "reachable":  false,
        "checked_at": time.Now().UTC().Format(time.RFC3339),
    }

    // Try SLS stats API on :8090 (the standard port for the SLS backend)
    statsAddr := fmt.Sprintf("%s:8090", host)
    start := time.Now()
    conn, err := net.DialTimeout("tcp", statsAddr, 3*time.Second)
    if err != nil {
        result["error"] = fmt.Sprintf("TCP connect to %s failed: %v", statsAddr, err)
        return result
    }
    conn.Close()
    result["latency_ms"] = time.Since(start).Milliseconds()
    result["reachable"] = true

    // Optional: try HTTP health endpoint
    client := &http.Client{Timeout: 3 * time.Second}
    resp, err := client.Get(fmt.Sprintf("http://%s:8090/health", host))
    if err == nil {
        resp.Body.Close()
        result["sls_available"] = resp.StatusCode == http.StatusOK
    }

    return result
}
```

### 3.4 Route Registration

In the Server's route setup (wherever `mux.HandleFunc` calls are registered):

```go
mux.HandleFunc("POST /api/v1/user/relay-config", s.authMiddleware(s.handleSetRelayConfig))
mux.HandleFunc("GET /api/v1/user/relay-config",  s.authMiddleware(s.handleGetRelayConfig))
```

The GET handler reads and returns the current BYOR config (simple passthrough of `store.GetUserBYORConfig`).

---

## 4. Control Plane Routing Changes

### 4.1 handleRelayStart() Routing Logic

The current `handleRelayStart()` flow (handlers.go lines 67–154):

```
1. Extract userID from JWT
2. Check entitlement → if !Allowed, return 403
3. Parse request, resolve region
4. StartOrGetSession()
5. If new session → spawn goroutine → runProvisionPipeline()
```

**For BYOR, this flow changes.** A BYOR user should NOT hit `/relay/start` at all in the normal case. The plugin connects directly to the user's relay using config from `GET /auth/session`. However, we need a safety mechanism for edge cases.

**Modified handleRelayStart():**

```go
func (s *Server) handleRelayStart(w http.ResponseWriter, r *http.Request) {
    userID, ok := auth.UserIDFromContext(r.Context())
    if !ok {
        writeAPIError(w, http.StatusUnauthorized, "unauthorized", "missing user identity")
        return
    }

    entitlement, err := s.store.GetRelayEntitlement(r.Context(), userID)
    if err != nil { /* ... existing error handling ... */ }

    // --- NEW: BYOR routing gate ---
    if entitlement.PlanTier == "free" {
        // Free-tier users use BYOR. They should not provision managed relays.
        // Return their BYOR config instead of starting a managed session.
        byorCfg, err := s.store.GetUserBYORConfig(r.Context(), userID)
        if err != nil || byorCfg.RelayHost == "" {
            writeAPIErrorWithReason(w, http.StatusForbidden, "byor_not_configured",
                "Configure your relay endpoint at POST /api/v1/user/relay-config first", "byor_not_configured")
            return
        }
        writeJSON(w, http.StatusOK, map[string]any{
            "mode": "byor",
            "relay_config": map[string]any{
                "relay_host": byorCfg.RelayHost,
                "relay_port": byorCfg.RelayPort,
                "stream_id":  byorCfg.StreamID,
            },
        })
        return
    }
    // --- END BYOR gate ---

    if !entitlement.Allowed {
        // ... existing 403 logic for non-free tiers without active subscriptions ...
    }

    // ... rest of existing managed relay flow unchanged ...
}
```

### 4.2 Entitlement System Changes

**Current behavior** (store.go `GetRelayEntitlement()`, lines 494–525):

```go
// Current: "starter" tier is DENIED relay access
if usage.PlanTier == "starter" || usage.PlanTier == "" {
    out.ReasonCode = "subscription_required"
    return out, nil
}
```

**Modified behavior:**

```go
func (s *Store) GetRelayEntitlement(ctx context.Context, userID string) (*model.RelayEntitlement, error) {
    usage, err := s.GetUsageCurrent(ctx, userID)
    if err != nil {
        return nil, err
    }

    out := &model.RelayEntitlement{
        PlanTier:         usage.PlanTier,
        PlanStatus:       usage.PlanStatus,
        IncludedSeconds:  usage.IncludedSeconds,
        ConsumedSeconds:  usage.ConsumedSeconds,
        RemainingSeconds: usage.RemainingSeconds,
        OverageSeconds:   usage.OverageSeconds,
    }

    // Free tier: BYOR access granted, managed access denied
    if usage.PlanTier == "free" {
        out.Allowed = false              // cannot use managed relays
        out.RelayMode = "byor"           // signal to plugin
        out.ReasonCode = "byor_only"
        return out, nil
    }

    // Plan status check (existing)
    switch usage.PlanStatus {
    case "active", "trial":
    default:
        out.ReasonCode = "subscription_inactive"
        return out, nil
    }

    // Starter still denied managed access (existing behavior)
    if usage.PlanTier == "starter" || usage.PlanTier == "" {
        out.ReasonCode = "subscription_required"
        return out, nil
    }

    out.Allowed = true
    out.RelayMode = "managed"
    return out, nil
}
```

### 4.3 Model Changes

Add `RelayMode` to the entitlement model:

```go
// In model/types.go
type RelayEntitlement struct {
    PlanTier         string
    PlanStatus       string
    RelayMode        string  // NEW: "byor", "managed", ""
    IncludedSeconds  int
    ConsumedSeconds  int
    RemainingSeconds int
    OverageSeconds   int
    Allowed          bool
    ReasonCode       string
}
```

### 4.4 Auth Session Response Changes

In `buildAuthSessionResponse()` (auth_handlers.go ~line 265), add BYOR config and relay_mode to the entitlement block:

```go
entitlementMap := map[string]any{
    "relay_access_status": relayAccessStatus(entitlement),
    "reason_code":         relayAccessReason(entitlement),
    "plan_tier":           entitlement.PlanTier,
    "plan_status":         entitlement.PlanStatus,
    "relay_mode":          entitlement.RelayMode,   // NEW
}

// If BYOR, include the user's relay config in the session response
if entitlement.RelayMode == "byor" {
    byorCfg, err := s.store.GetUserBYORConfig(ctx, userID)
    if err == nil && byorCfg.RelayHost != "" {
        entitlementMap["byor_config"] = map[string]any{
            "relay_host": byorCfg.RelayHost,
            "relay_port": byorCfg.RelayPort,
            "stream_id":  byorCfg.StreamID,
        }
    }
}
```

This means the plugin gets the BYOR endpoint from `GET /auth/session` without a separate API call. The `relay_access_status` value for BYOR users will be `"disabled"` (since `Allowed` is false), but the plugin checks `relay_mode` to distinguish BYOR from truly disabled accounts.

**Plugin-side branching:**
```
relay_mode == "byor"     → show custom relay settings, hide Start/Stop
relay_mode == "managed"  → show managed relay UI (existing behavior)
relay_mode == ""         → show upgrade prompt
```

---

## 5. Plugin Changes (C++ Side)

### 5.1 RelayClient: ConnectDirect() Path

File: `obs-plugin/src/relay_client.h` + `relay_client.cpp`

Add a new method for direct BYOR connection without going through the provision pipeline:

```cpp
// New struct for BYOR connection params
struct BYORConfig {
    std::string relay_host;
    int         relay_port = 5000;
    std::string stream_id;    // empty = use live_{stream_token}
};

// In RelayClient class:
class RelayClient {
public:
    // ... existing methods ...

    // BYOR: Connect directly to a user-provided relay endpoint.
    // Sets current_session_ to a synthetic session (no server-side session ID),
    // starts stats polling, but does NOT start heartbeat loop.
    void ConnectDirect(const BYORConfig& config, const std::string& stream_token);

    // BYOR: Disconnect from a BYOR relay. Stops stats polling.
    // No server-side cleanup needed.
    void DisconnectDirect();

    // True if currently in BYOR mode (vs managed mode)
    bool IsBYORMode() const;

private:
    // ... existing members ...
    std::atomic<bool> byor_mode_{false};
};
```

**ConnectDirect() implementation sketch:**

```cpp
void RelayClient::ConnectDirect(const BYORConfig& config, const std::string& stream_token) {
    std::lock_guard<std::mutex> lock(session_mutex_);

    // Build a synthetic RelaySession (no provision pipeline, no server session ID)
    RelaySession session;
    session.session_id = "byor-local-" + GenerateUuidV4();  // local tracking only
    session.status = "active";
    session.public_ip = config.relay_host;
    session.srt_port = config.relay_port;
    session.stream_token = stream_token;
    session.relay_hostname = config.relay_host;

    // Override stream_token with custom stream_id if provided
    if (!config.stream_id.empty()) {
        session.stream_token = config.stream_id;
    }

    current_session_ = session;
    byor_mode_ = true;

    // Start stats polling immediately — no waiting for provision pipeline
    // PollRelayStats and PollPerLinkStats already work with any host
}
```

### 5.2 Stats Polling Compatibility

**What works as-is with BYOR relays:**

| Feature | Endpoint | Works with any SLS relay? | Works with non-SLS relay? |
|---------|----------|--------------------------|--------------------------|
| Aggregate SRT stats | `GET http://{host}:8090/stats/play_{token}?legacy=1` | Yes | No |
| Per-link bonded stats | `GET http://{host}:5080/stats` | Only with custom srtla_rec fork | No |
| Relay health | `GET http://{host}:8090/health` | Yes | No |

**Graceful degradation strategy:**

In `PollRelayStats()` and `PollPerLinkStats()`, add BYOR-aware error handling:

```cpp
void RelayClient::PollRelayStats(const std::string& relay_ip) {
    // ... existing HTTP request to :8090/stats ...

    if (/* HTTP error or non-200 */) {
        if (byor_mode_) {
            // BYOR relay may not run SLS. Silently mark stats unavailable.
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.available = false;
            return;  // Don't log as error — expected for non-SLS relays
        }
        // Managed relay: this IS an error, log it
        // ... existing error handling ...
    }
}
```

For per-link stats (`PollPerLinkStats`), the same pattern: if `:5080/stats` returns an error in BYOR mode, set `per_link_.available = false` and hide the per-link section in the dock UI.

### 5.3 Dock UI Changes

File: `obs-plugin/src/dock-widget.cpp` (or equivalent dock UI file)

**Settings panel additions:**

1. **Relay Source toggle:** Radio buttons or dropdown — "Managed" (default) vs "Custom (BYOR)"
2. **When "Custom" is selected, show:**
   - Host field: text input, placeholder "my-relay.example.com"
   - Port field: numeric input, default 5000
   - Stream ID field: text input, placeholder "live/my_stream" (optional)
   - "Test Connection" button → calls `probeBYOREndpoint` logic client-side (TCP connect to :8090)
   - "Save" button → `POST /api/v1/user/relay-config`
3. **When "Custom" is active, hide:**
   - "Start Relay" / "Stop Relay" buttons (no provision pipeline)
   - Provisioning progress bar and step indicators
   - Region selector (BYOR relay has a fixed location)
4. **When "Custom" is active, show:**
   - "Connect" / "Disconnect" buttons (calls `ConnectDirect` / `DisconnectDirect`)
   - Relay stats card (if SLS stats are available)
   - Per-link stats section (if custom srtla_rec fork is detected)
   - "Stats unavailable for custom relay" message (if stats polling fails 3+ times)

**Auto-detection from auth session:**

When the plugin receives `GET /auth/session` response with `relay_mode: "byor"`:
- Automatically switch dock to BYOR mode
- Pre-populate host/port/stream_id from `byor_config` in the response
- If `byor_config` is absent, show the settings panel with empty fields and a prompt to configure

### 5.4 Heartbeat Behavior

BYOR mode does NOT send heartbeats to the control plane. The heartbeat loop (`StartHeartbeatLoop`) is not started in BYOR mode. Rationale:

- No server-side session to heartbeat against
- No usage metering for free-tier BYOR users
- Reduces control plane load from free users

If telemetry sync is added later (optional account), a lightweight telemetry ping (not a relay heartbeat) can be added separately.

### 5.5 Config Persistence

The plugin stores BYOR settings in two places:

1. **Server-side** (if authenticated): via `POST /api/v1/user/relay-config` → stored in `users` table. Syncs across machines.
2. **Local** (always): in the plugin's `config.json` (existing OBS plugin settings file). This enables fully offline BYOR usage without an account.

On startup, the plugin loads local config first, then overwrites with server-side config if authenticated. This ensures BYOR works even if the control plane is unreachable.

---

## 6. File Change List

### Control Plane (Go)

| File | Change | Est. Lines |
|------|--------|-----------|
| `internal/relay/byor.go` | **NEW** — BYORProvisioner struct, Provision(), Deprovision() | ~60 |
| `internal/relay/provisioner.go` | Rename `AWSInstanceID` → `InstanceID` in ProvisionResult and DeprovisionRequest (Phase 1) | ~5 |
| `internal/relay/aws.go` | Update to use `InstanceID` field name; no logic changes | ~15 |
| `internal/relay/fake.go` | Update to use `InstanceID` field name | ~3 |
| `internal/store/store.go` | Add `GetUserBYORConfig()`, `SetUserBYORConfig()`, `ClearUserBYORConfig()`, `BYORConfig` struct; modify `GetRelayEntitlement()` for free tier; rename `aws_instance_id` refs in SQL queries (~8 queries) | ~120 |
| `internal/model/types.go` | Add `RelayMode` to `RelayEntitlement`; rename `RelayAWSInstanceID` → `RelayInstanceID` on `Session` | ~8 |
| `internal/api/handlers.go` | Add `handleSetRelayConfig()`, `handleGetRelayConfig()`, `probeBYOREndpoint()`; modify `handleRelayStart()` for BYOR routing gate; rename `AWSInstanceID` refs; update `toSessionResponse()`; update `runProvisionPipeline()` for neutral field names | ~150 |
| `internal/api/auth_handlers.go` | Modify `buildAuthSessionResponse()` to include `relay_mode` and `byor_config` | ~25 |
| `internal/api/server.go` | Register new routes (`POST /api/v1/user/relay-config`, `GET /api/v1/user/relay-config`) | ~5 |
| `internal/config/config.go` | Extend `RelayProvider` validation to accept `"byor"` alongside `"fake"` and `"aws"` | ~5 |
| `migrations/0010_byor_relay_config.sql` | **NEW** — ALTER TABLE users, drop/recreate CHECK constraint | ~15 |
| **Control plane subtotal** | | **~411** |

### OBS Plugin (C++)

| File | Change | Est. Lines |
|------|--------|-----------|
| `obs-plugin/src/relay_client.h` | Add `BYORConfig` struct, `ConnectDirect()`, `DisconnectDirect()`, `IsBYORMode()`, `byor_mode_` atomic | ~30 |
| `obs-plugin/src/relay_client.cpp` | Implement `ConnectDirect()`, `DisconnectDirect()`; add BYOR-aware graceful degradation in `PollRelayStats()` and `PollPerLinkStats()` | ~100 |
| `obs-plugin/src/dock-widget.cpp` | BYOR settings panel (host/port/stream_id fields, relay source toggle, connect/disconnect buttons); auto-detection from `relay_mode`; stats unavailable messaging | ~200 |
| `obs-plugin/src/dock-widget.h` | UI state for BYOR mode, new widget declarations | ~20 |
| `obs-plugin/src/auth_manager.cpp` | Parse `relay_mode` and `byor_config` from `GET /auth/session` response; store in `PluginAuthState` or `AuthSessionSnapshot` | ~30 |
| `obs-plugin/src/auth_manager.h` | Add `byor_config` to `AuthSessionSnapshot`, `relay_mode` to `RelayEntitlement` | ~10 |
| **Plugin subtotal** | | **~390** |

### Total Estimated Changes

| Component | New Files | Modified Files | Est. Lines |
|-----------|-----------|---------------|-----------|
| Control plane | 2 | 8 | ~411 |
| OBS plugin | 0 | 6 | ~390 |
| **Total** | **2** | **14** | **~801** |

---

## Appendix A: Sequence Diagram — BYOR User Flow

```
Plugin                      Control Plane                 User's Relay
  |                              |                             |
  |--- GET /auth/session ------->|                             |
  |<-- relay_mode:"byor" --------|                             |
  |    byor_config:{host,port}   |                             |
  |                              |                             |
  | [User clicks "Connect"]      |                             |
  |--- ConnectDirect() --------->|                             |
  |    (local only, no API call) |                             |
  |                              |                             |
  |--- PollRelayStats() ---------|----------TCP:8090---------->|
  |<-- stats or unavailable -----|<----- JSON stats -----------|
  |                              |                             |
  |--- PollPerLinkStats() -------|----------TCP:5080---------->|
  |<-- per_link or unavailable --|<----- JSON per-link --------|
  |                              |                             |
  | [User clicks "Disconnect"]   |                             |
  |--- DisconnectDirect() ------>|                             |
  |    (local only)              |                             |
```

## Appendix B: Sequence Diagram — BYOR Config Save

```
Plugin                      Control Plane                    DB
  |                              |                            |
  | [User enters relay settings] |                            |
  |--- POST /user/relay-config ->|                            |
  |    {host, port, stream_id}   |                            |
  |                              |--- probeBYOREndpoint() --->|
  |                              |    (TCP connect :8090)     |
  |                              |                            |
  |                              |--- UPDATE users SET ------>|
  |                              |    byor_relay_host, etc.   |
  |                              |<-- ok --------------------|
  |<-- 200 {relay_config, -------|                            |
  |         health_check}        |                            |
```

## Appendix C: Migration Safety Checklist

Before applying `0010_byor_relay_config.sql` in production:

- [ ] Verify existing CHECK constraint name: `SELECT conname FROM pg_constraint WHERE conrelid = 'users'::regclass AND conname LIKE '%plan_tier%';`
- [ ] Run migration against a clone of the production database first
- [ ] Confirm no rows have `plan_tier` values outside the new constraint set
- [ ] Verify `byor_relay_port` default (5000) aligns with standard SRTLA port
- [ ] After migration, verify: `SELECT byor_relay_host, byor_relay_port FROM users LIMIT 5;` — all NULL for existing users
- [ ] Test inserting a user with `plan_tier = 'free'` succeeds
- [ ] Test inserting a user with an invalid `plan_tier` (e.g., 'admin') fails

## Appendix D: Open Questions for Implementation

1. **Should `POST /api/v1/user/relay-config` require authentication?** Yes — it writes to the users table. Unauthenticated BYOR (offline mode) uses local config.json only.

2. **Should the health check probe the SRTLA port (UDP 5000) or only the stats API (TCP 8090)?** TCP 8090 only. UDP probing is unreliable from a control plane context, and the stats API being reachable is a stronger signal that the relay stack is running.

3. **Should BYOR users consume a session record in the sessions table?** No. BYOR connections are local-only. No server-side session, no usage tracking, no heartbeat. This keeps the sessions table clean for managed relays only.

4. **Should the control plane support `AEGIS_RELAY_PROVIDER=byor` as a global config?** No. BYOR is a per-user attribute (plan_tier = free), not a server-wide provider setting. The global provider config (`fake` / `aws` / future `hetzner`) controls managed relay infrastructure. A free-tier user gets BYOR regardless of the global provider setting.
