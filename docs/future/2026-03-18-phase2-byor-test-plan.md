# Phase 2 BYOR Integration Test Spec — Telemy v0.0.5

**Date:** 2026-03-18  
**Status:** Draft  
**Depends on:** Phase 1 field renames (v005-plan §1.1–1.3), `0010_byor_relay_config.sql` migration

---

## 1. Control Plane Unit Tests (Go)

All tests go in `internal/relay/byor_test.go` and `internal/api/handlers_test.go` unless noted.

### 1.1 BYORProvisioner

| Test | Setup | Assert |
|------|-------|--------|
| `TestBYORProvision_ReturnsUserConfig` | `NewBYORProvisioner(&model.BYORConfig{Host: "relay.example.com", Port: 5000, StreamID: "live/abc"})` | `result.PublicIP == "relay.example.com"`, `result.SRTPort == 5000`, `result.InstanceID == "byor-<userID>"` |
| `TestBYORProvision_StoreBackedConfig` | `NewStoreBackedBYORProvisioner(mockStore)` with mock returning valid config | Same assertions as above; verify `store.GetBYORConfig` called once |
| `TestBYORProvision_MissingConfig` | Config with empty `Host` | Returns error containing `"byor relay config missing"` |
| `TestBYORProvision_NilStore_NilConfig` | `NewBYORProvisioner(nil)` with nil store | Returns error containing `"missing config"` |
| `TestBYORProvision_DefaultPort` | Config with `Port: 0` | `result.SRTPort == 5000` |
| `TestBYORDeprovision_NoOp` | Any provisioner instance | `Deprovision()` returns `nil`, no side effects |

### 1.2 POST /api/v1/user/relay-config Validation

Test via `httptest.NewServer` against `handleSetRelayConfig`.

| Test | Request Body | Expected |
|------|-------------|----------|
| `TestRelayConfig_ValidHost` | `{"relay_host": "relay.example.com", "relay_port": 5000}` | 200, response contains `relay_config.relay_host` |
| `TestRelayConfig_ValidIP` | `{"relay_host": "203.0.113.10", "relay_port": 5000}` | 200 |
| `TestRelayConfig_InvalidPort_Zero` | `{"relay_host": "relay.example.com", "relay_port": 0}` | 200, port defaults to 5000 |
| `TestRelayConfig_InvalidPort_Negative` | `{"relay_host": "relay.example.com", "relay_port": -1}` | 400, `"relay_port must be 1-65535"` |
| `TestRelayConfig_InvalidPort_TooHigh` | `{"relay_host": "relay.example.com", "relay_port": 70000}` | 400, `"relay_port must be 1-65535"` |
| `TestRelayConfig_EmptyHost_ClearsConfig` | `{"relay_host": ""}` | 200, `relay_config: null`, verify `ClearUserBYORConfig` called |
| `TestRelayConfig_HostTooLong` | `relay_host` = 254-char string | 400, `"relay_host too long"` |
| `TestRelayConfig_StreamIDTooLong` | `stream_id` = 257-char string | 400, `"stream_id too long"` |
| `TestRelayConfig_Unauthenticated` | No JWT | 401 |

### 1.3 handleRelayStart() Routing

| Test | User Setup | Expected |
|------|-----------|----------|
| `TestRelayStart_FreeTier_WithBYORConfig` | `plan_tier=free`, BYOR config saved | 200, `mode: "byor"`, response includes `relay_config` |
| `TestRelayStart_FreeTier_NoBYORConfig` | `plan_tier=free`, no BYOR config | 403, `reason: "byor_not_configured"` |
| `TestRelayStart_StarterTier_ManagedPath` | `plan_tier=starter`, active subscription | Enters managed provision flow (existing behavior) |
| `TestRelayStart_ProTier_ManagedPath` | `plan_tier=pro`, active subscription | Enters managed provision flow |
| `TestRelayStart_FreeTier_NeverHitsProvisionPipeline` | `plan_tier=free`, BYOR config present | Verify `runProvisionPipeline` is NOT called |

### 1.4 Auth Session Response

| Test | User Setup | Assert on GET /auth/session |
|------|-----------|----------------------------|
| `TestAuthSession_FreeTier_BYORMode` | `plan_tier=free`, BYOR config saved | `relay_mode == "byor"`, `byor_config` object present with host/port/stream_id |
| `TestAuthSession_FreeTier_NoBYORConfig` | `plan_tier=free`, no config | `relay_mode == "byor"`, `byor_config` absent |
| `TestAuthSession_ProTier_ManagedMode` | `plan_tier=pro` | `relay_mode == "managed"`, no `byor_config` |
| `TestAuthSession_StarterTier` | `plan_tier=starter` | `relay_mode` is empty or `"managed"` (existing behavior preserved) |

### 1.5 Store Methods

Use a test database (or pgx mock) for `internal/store/store_test.go`.

| Test | Action | Assert |
|------|--------|--------|
| `TestSetGetBYORConfig` | `SetUserBYORConfig` then `GetUserBYORConfig` | Round-trip preserves host, port, stream_id |
| `TestGetBYORConfig_NotConfigured` | Query user with NULL byor columns | Returns config with empty `RelayHost` |
| `TestClearBYORConfig` | Set config, then `ClearUserBYORConfig` | Subsequent get returns empty `RelayHost` |
| `TestSetBYORConfig_UserNotFound` | Nonexistent user ID | Returns `ErrNotFound` |
| `TestBYORPortConstraint` | Insert `byor_relay_port = 99999` directly via SQL | DB constraint violation |

---

## 2. Plugin Tests (C++)

Tests go alongside existing plugin test files. Use Google Test or the project's existing test framework.

### 2.1 ConnectDirect / DisconnectDirect

| Test | Action | Assert |
|------|--------|--------|
| `ConnectDirect_SetsSession` | Call `ConnectDirect("relay.example.com", 5000, "live_abc")` | `HasActiveSession() == true`, `IsBYORMode() == true`, `CurrentSession()->public_ip == "relay.example.com"` |
| `ConnectDirect_WithBYORConfig` | Call `ConnectDirect(BYORConfig{"relay.example.com", 5000, "custom/stream"}, "live_abc")` | `CurrentSession()->stream_token == "custom/stream"` (stream_id overrides stream_token) |
| `ConnectDirect_DefaultStreamToken` | `ConnectDirect(BYORConfig{"relay.example.com", 5000, ""}, "live_abc")` | `CurrentSession()->stream_token == "live_abc"` (falls back to stream_token) |
| `DisconnectDirect_ClearsState` | Connect then `DisconnectDirect()` | `HasActiveSession() == false`, `IsBYORMode() == false`, stats cleared |
| `ConnectDirect_DoesNotStartHeartbeat` | Connect, wait 2s | Heartbeat thread not running (`heartbeat_running_ == false`) |

### 2.2 Config Vault Persistence

| Test | Action | Assert |
|------|--------|--------|
| `PluginAuthState_PersistsBYORFields` | Serialize `PluginAuthState` with `BYORConfig{host, port, stream_id}` via `ToVaultJson()` then `FromVaultJson()` | Round-trip preserves all three fields |
| `AuthSessionSnapshot_ParsesBYORConfig` | Parse JSON with `relay_mode: "byor"` and `byor_config` object | `entitlement.relay_mode == "byor"`, byor_config fields populated |
| `AuthSessionSnapshot_NoBYORConfig` | Parse JSON without `byor_config` key | No crash, byor fields empty/default |

### 2.3 Stats Polling Graceful Degradation

| Test | Setup | Assert |
|------|-------|--------|
| `PollRelayStats_BYORMode_Unreachable` | `byor_mode_ = true`, stats endpoint returns connection refused | `CurrentStats().available == false`, no error log (silent degradation) |
| `PollRelayStats_BYORMode_Reachable` | `byor_mode_ = true`, mock stats endpoint returns valid JSON | `CurrentStats().available == true`, bitrate/rtt populated |
| `PollPerLinkStats_BYORMode_Unavailable` | `byor_mode_ = true`, per-link endpoint returns 404 | `CurrentPerLinkStats().available == false`, no error log |
| `PollRelayStats_ManagedMode_Unreachable` | `byor_mode_ = false`, stats endpoint unreachable | Error IS logged (existing behavior preserved) |

---

## 3. End-to-End Manual Test Procedure

### Prerequisites

- A running BYOR relay: use the existing `srtla-receiver` Docker stack (`docker compose up` from the srtla-receiver repo) on localhost or a cheap VPS. Note the host IP and SRT port (default 5000).
- A test PostgreSQL database with migration `0010_byor_relay_config.sql` applied.
- Control plane running locally (`go run ./cmd/aegis-server`).
- OBS 30+ with the v0.0.5 plugin build installed.
- IRL Pro (or any SRTLA-capable encoder) for the streaming step.

### Steps

| # | Action | Expected Result |
|---|--------|----------------|
| 1 | **Create free-tier test user.** Insert or register a user with `plan_tier = 'free'` in the DB. | User exists, `byor_relay_host` is NULL. |
| 2 | **POST relay config.** `curl -X POST http://localhost:8080/api/v1/user/relay-config -H "Authorization: Bearer <jwt>" -d '{"relay_host":"<VPS_IP>","relay_port":5000,"stream_id":"live/test123"}'` | 200 response with `relay_config` object. `health_check.reachable` is true if relay is up. |
| 3 | **Verify DB.** `SELECT byor_relay_host, byor_relay_port, byor_stream_id FROM users WHERE id = '<user_id>';` | Columns populated with values from step 2. |
| 4 | **GET /auth/session.** `curl http://localhost:8080/auth/session -H "Authorization: Bearer <jwt>"` | Response contains `relay_mode: "byor"` and `byor_config: {relay_host, relay_port, stream_id}`. |
| 5 | **Open OBS with plugin.** Log in with the free-tier test user. | Dock shows BYOR mode: custom relay settings pre-populated from auth session. No "Start Relay" button visible. "Connect" / "Disconnect" buttons shown instead. |
| 6 | **Click Connect.** | Plugin calls `ConnectDirect()`. Dock status shows "Connected to <VPS_IP>:5000". No `/relay/start` API call made (verify in server logs). |
| 7 | **Check stats polling.** | If relay runs SLS: stats card shows bitrate, RTT, etc. If not: dock shows "Stats unavailable for custom relay" gracefully. No error spam in OBS log. |
| 8 | **Stream from IRL Pro.** Point IRL Pro at `srt://<VPS_IP>:5000?streamid=live/test123`. | Video arrives at relay. OBS receives stream via relay. Dock telemetry updates (if SLS stats available). |
| 9 | **Verify per-link stats.** (Only if using custom srtla_rec fork with `:5080/stats`.) | Per-link section populates in dock. If endpoint unavailable, section hidden gracefully. |
| 10 | **Click Disconnect.** | `DisconnectDirect()` called. Dock returns to idle state. No `/relay/stop` API call. Stats cleared. |
| 11 | **Verify clean teardown.** Check OBS logs for errors. | No crash, no orphaned threads, no leaked HTTP connections. |

### Edge Cases to Probe Manually

- **Unreachable relay:** Set `relay_host` to a non-routable IP (e.g., `198.51.100.1`), click Connect. Plugin should timeout and show a connection error in the dock, not crash.
- **Config update while connected:** Change relay_host via POST while connected. Disconnect and reconnect — should use new config after re-fetching auth session.
- **Offline BYOR:** Disconnect from internet, open OBS. Plugin should load local config from vault and allow ConnectDirect to the relay (if on same LAN).

---

## 4. Regression Tests

Ensure the managed relay path is unbroken after BYOR changes.

### 4.1 Managed Relay Start/Stop

| Test | Setup | Assert |
|------|-------|--------|
| `TestManagedRelayStart_ProTier` | `plan_tier=pro`, active subscription | `/relay/start` returns session with `status: "provisioning"`, provision pipeline runs, session transitions to `"active"` |
| `TestManagedRelayStop_ProTier` | Active managed session | `/relay/stop` returns success, `Deprovision()` called on AWS provisioner |
| `TestManagedHeartbeat` | Active managed session | Heartbeat succeeds, session stays active |

### 4.2 Auth Session — Managed Users Unchanged

| Test | Setup | Assert |
|------|-------|--------|
| `TestAuthSession_ProTier_NoByorConfig` | `plan_tier=pro` | `relay_mode == "managed"`, `relay_access_status == "enabled"`, NO `byor_config` key in response |
| `TestAuthSession_StandardTier` | `plan_tier=standard` | Same as pro — managed mode, no BYOR leakage |

### 4.3 API Contract Stability

| Test | Assert |
|------|--------|
| `TestRelayStartResponse_Schema` | Response JSON for managed start still contains `session_id`, `status`, `public_ip`, `srt_port`, `relay_hostname`, `stream_token` — no field renames leak to the API layer |
| `TestRelayStopResponse_Schema` | Response shape unchanged |
| `TestAuthSessionResponse_BackwardsCompatible` | Existing fields (`relay_access_status`, `reason_code`, `plan_tier`, `plan_status`, `usage`) still present. `relay_mode` is additive only. |
| `TestProvisionResult_FieldRenames` | `ProvisionResult` uses `InstanceID` (not `AWSInstanceID`). Verify all references in `handlers.go`, `aws.go`, `fake.go`, `byor.go` use the new name. `grep -r "AWSInstanceID" internal/` returns zero hits. |

### 4.4 Database Constraint Regression

| Test | Assert |
|------|--------|
| `TestPlanTierConstraint_ExistingTiers` | `INSERT` with `plan_tier` = `'starter'`, `'standard'`, `'pro'` all succeed |
| `TestPlanTierConstraint_FreeTier` | `INSERT` with `plan_tier = 'free'` succeeds |
| `TestPlanTierConstraint_InvalidTier` | `INSERT` with `plan_tier = 'admin'` fails with constraint violation |

---

## 5. Test Execution Checklist

- [ ] All Go unit tests pass: `cd aegis-control-plane && go test ./...`
- [ ] Migration applies cleanly: `psql < migrations/0010_byor_relay_config.sql`
- [ ] Plugin builds with no warnings: `cmake --build . --config Release`
- [ ] Plugin unit tests pass (if Google Test harness exists)
- [ ] Manual E2E walkthrough completed (section 3)
- [ ] `grep -r "AWSInstanceID" internal/` returns zero results (Phase 1 renames done)
- [ ] No regressions in existing managed relay tests
