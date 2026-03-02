# Relay Timeout Fix + Hardening — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix relay timeout bug (relay HTTP calls blocking IPC heartbeat) and apply 3 hardening items (#13 SeqCst, #11 dedup, #9 async Mutex).

**Architecture:** Refactor the sequential polling loop in `handle_session_io` to a `tokio::select!`-based event loop. Relay HTTP calls are spawned as background tasks, results return through an `mpsc` channel. The main loop never blocks on HTTP, so pings keep flowing and heartbeat never fires falsely.

**Tech Stack:** Rust, tokio (select!, spawn, mpsc, Mutex), MessagePack IPC

**Design doc:** `docs/plans/2026-03-01-relay-timeout-fix-design.md`

---

### Task 1: Extract Shared `aegis_client` Module

Three files duplicate `generate_idempotency_key()` and `build_aegis_client(config, vault)`. Extract into one shared module.

**Files:**
- Create: `src/aegis_client.rs`
- Modify: `src/main.rs`
- Modify: `src/app/mod.rs`
- Modify: `src/server/mod.rs`
- Modify: `src/ipc/mod.rs`

**Step 1: Create `src/aegis_client.rs`**

```rust
use crate::aegis::ControlPlaneClient;
use crate::config::Config;
use crate::security::Vault;
use uuid::Uuid;

/// Generate a random UUID v4 idempotency key for relay API calls.
pub fn generate_idempotency_key() -> String {
    Uuid::new_v4().to_string()
}

/// Build a `ControlPlaneClient` from pre-loaded config and vault references.
/// Used by `app/mod.rs` (CLI commands) and `server/mod.rs` (HTTP routes).
pub fn build_aegis_client(
    config: &Config,
    vault: &Vault,
) -> Result<ControlPlaneClient, Box<dyn std::error::Error>> {
    let base_url = config
        .aegis
        .base_url
        .as_deref()
        .ok_or("missing aegis.base_url in config")?
        .trim();
    let jwt_key = config
        .aegis
        .access_jwt_key
        .as_deref()
        .ok_or("missing aegis.access_jwt_key in config")?
        .trim();
    if base_url.is_empty() {
        return Err("missing aegis.base_url in config".into());
    }
    if jwt_key.is_empty() {
        return Err("missing aegis.access_jwt_key in config".into());
    }
    let access_jwt = vault.retrieve(jwt_key)?;
    Ok(ControlPlaneClient::new(base_url, access_jwt.trim())?)
}

/// Build a `ControlPlaneClient` by loading config and vault from disk.
/// Used by `ipc/mod.rs` where no pre-loaded config/vault is available.
pub fn build_aegis_client_from_local_config() -> Result<ControlPlaneClient, String> {
    let config = Config::load().map_err(|err| format!("config load failed: {err}"))?;
    let base_url = config
        .aegis
        .base_url
        .as_deref()
        .ok_or_else(|| "missing aegis.base_url in config".to_string())?
        .trim()
        .to_string();
    let jwt_key = config
        .aegis
        .access_jwt_key
        .as_deref()
        .ok_or_else(|| "missing aegis.access_jwt_key in config".to_string())?
        .trim()
        .to_string();
    if base_url.is_empty() {
        return Err("missing aegis.base_url in config".to_string());
    }
    if jwt_key.is_empty() {
        return Err("missing aegis.access_jwt_key in config".to_string());
    }
    let vault = Vault::new(config.vault.path.as_deref())
        .map_err(|err| format!("vault init failed: {err}"))?;
    let access_jwt = vault
        .retrieve(jwt_key.as_str())
        .map_err(|err| format!("vault retrieve failed for '{}': {err}", jwt_key))?;
    ControlPlaneClient::new(base_url, access_jwt.trim())
        .map_err(|err| format!("aegis client init failed: {err}"))
}
```

**Step 2: Register the module in `src/main.rs`**

Add `mod aegis_client;` after the existing `mod aegis;` line (line 1). The file should read:

```rust
mod aegis;
mod aegis_client;
mod app;
// ... rest unchanged
```

**Step 3: Update `src/app/mod.rs`**

- Add import: `use crate::aegis_client::{build_aegis_client, generate_idempotency_key};`
- Delete the local `build_aegis_client` function (lines 377-402)
- Delete the local `generate_idempotency_key` function (lines 412-414)
- All call sites already use `build_aegis_client(...)` and `generate_idempotency_key()` — no call-site changes needed

**Step 4: Update `src/server/mod.rs`**

- Add import: `use crate::aegis_client::{build_aegis_client, generate_idempotency_key};`
- Delete the local `build_aegis_client_from_config` function (lines 1869-1893)
- Delete the local `generate_idempotency_key` function (lines 1895-1897)
- Update call sites: replace `build_aegis_client_from_config(` with `build_aegis_client(` (same signature, just a name change)

**Step 5: Update `src/ipc/mod.rs`**

- Add import: `use crate::aegis_client::{build_aegis_client_from_local_config, generate_idempotency_key};`
- Delete the local `generate_idempotency_key` function (lines 666-668)
- Delete the local `build_aegis_client_from_local_config` function (lines 670-701)
- No call-site changes needed — same function names

**Step 6: Run tests**

Run: `cargo test --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: All 31 tests pass. Zero compilation errors.

Run: `cargo clippy --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: No new warnings (unused imports from removed functions are gone).

**Step 7: Commit**

```bash
git add src/aegis_client.rs src/main.rs src/app/mod.rs src/server/mod.rs src/ipc/mod.rs
git commit -m "refactor: extract shared aegis_client module (dedup #11)"
```

---

### Task 2: Fix Atomic Ordering in `tray/mod.rs`

`SeqCst` is unnecessarily strong for a simple boolean shutdown flag. `Release` (store) and `Acquire` (load) are the correct orderings.

**Files:**
- Modify: `src/tray/mod.rs:47,52`

**Step 1: Change orderings**

In `src/tray/mod.rs`:

Line 47 — change:
```rust
        quit_flag.store(true, Ordering::SeqCst);
```
to:
```rust
        quit_flag.store(true, Ordering::Release);
```

Line 52 — change:
```rust
        if shutdown_flag.load(Ordering::SeqCst) {
```
to:
```rust
        if shutdown_flag.load(Ordering::Acquire) {
```

**Step 2: Run tests**

Run: `cargo test --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: All 31 tests pass.

**Step 3: Commit**

```bash
git add src/tray/mod.rs
git commit -m "fix: SeqCst -> Release/Acquire for shutdown flag (#13)"
```

---

### Task 3: Swap `std::sync::Mutex` to `tokio::sync::Mutex`

Three fields in `ServerState` use `std::sync::Mutex`. These are held across `.await` points in async handler code. `tokio::sync::Mutex` is correct for async contexts and does not poison.

**Files:**
- Modify: `src/server/mod.rs` (imports, struct, ~5 lock sites)
- Modify: `src/app/mod.rs` (snapshot creation site)
- Modify: `src/ipc/mod.rs` (parameter type, ~4 lock sites, test helper)

**Step 1: Update `src/server/mod.rs` imports and struct**

Change the import (line 20-25) from:
```rust
use std::{
    collections::HashMap,
    net::SocketAddr,
    sync::{Arc, Mutex},
    time::Duration,
};
```
to:
```rust
use std::{
    collections::HashMap,
    net::SocketAddr,
    sync::Arc,
    time::Duration,
};
use tokio::sync::Mutex;
```

The `ServerState` struct fields (lines 36-38) keep the same type names — `Arc<Mutex<T>>` — but `Mutex` now resolves to `tokio::sync::Mutex`.

**Step 2: Update server lock sites**

Find all `.lock().unwrap_or_else(|p| p.into_inner())` in `server/mod.rs` (5 occurrences at lines 1016, 1057, 1515, 1606, 1704) and replace with `.lock().await`.

Example — line 1016:
```rust
// Before:
let mut vault = state.vault.lock().unwrap_or_else(|p| p.into_inner());
// After:
let mut vault = state.vault.lock().await;
```

Apply the same change to all 5 sites. `tokio::sync::Mutex` does not poison, so no unwrap handling is needed.

Also search for any `grafana_configured.lock()` and `aegis_session_snapshot.lock()` patterns and apply the same change (`.lock().await`, remove `.unwrap_or_else(...)`).

**Step 3: Update `src/app/mod.rs` snapshot creation**

Find where `aegis_session_snapshot` is created (likely `Arc::new(std::sync::Mutex::new(None))` or `Arc::new(Mutex::new(None))`) and change to use `tokio::sync::Mutex`:

```rust
// Before:
let aegis_session_snapshot = Arc::new(std::sync::Mutex::new(None));
// After:
let aegis_session_snapshot = Arc::new(tokio::sync::Mutex::new(None));
```

Also update the type annotation if there is one:
```rust
// Before:
let aegis_session_snapshot: Arc<std::sync::Mutex<Option<RelaySession>>> = ...
// After:
let aegis_session_snapshot: Arc<tokio::sync::Mutex<Option<RelaySession>>> = ...
```

If `app/mod.rs` also passes `vault` or `grafana_configured` to the server as `Arc<std::sync::Mutex<...>>`, update those too.

**Step 4: Update `src/ipc/mod.rs` parameter type**

The `handle_session_io` function signature (line 706) takes:
```rust
aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
```
where `Mutex` is `std::sync::Mutex` from the import on line 11.

Since `ipc/mod.rs` also needs `std::sync::Mutex` for `IpcDebugStatusHandle` (line 22), we can't just swap the import. Instead, keep `std::sync::Mutex` for `IpcDebugStatusHandle` and use the full path for the session snapshot:

Change the function signature parameter from:
```rust
    aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
```
to:
```rust
    aegis_session_snapshot: Arc<tokio::sync::Mutex<Option<RelaySession>>>,
```

**Step 5: Update IPC lock sites**

Find all `aegis_session_snapshot.lock().unwrap_or_else(|p| p.into_inner())` in `ipc/mod.rs` and replace with `aegis_session_snapshot.lock().await`.

Key locations:
- Status push interval block (~line 812): `.lock().unwrap_or_else(|p| p.into_inner()).clone()` → `.lock().await.clone()`
- relay_start success (~line 1148): `*aegis_session_snapshot.lock().unwrap_or_else(...)` → `*aegis_session_snapshot.lock().await`
- relay_start status push (~line 1170): same pattern
- relay_stop clear (~line 1296): same pattern
- relay_stop no-active-session clear (~line 1347): same pattern

**Step 6: Update test helper**

In the `#[cfg(test)] mod tests` block, the `spawn_test_session` helper creates:
```rust
let snapshot = Arc::new(Mutex::new(None));
```

This `Mutex` is `std::sync::Mutex` from the module import. Change to:
```rust
let snapshot = Arc::new(tokio::sync::Mutex::new(None));
```

**Step 7: Run tests**

Run: `cargo test --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: All 31 tests pass.

Run: `cargo clippy --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: No new warnings.

**Step 8: Commit**

```bash
git add src/server/mod.rs src/app/mod.rs src/ipc/mod.rs
git commit -m "refactor: swap std::sync::Mutex to tokio::sync::Mutex (#9)"
```

---

### Task 4: Refactor IPC Loop to `tokio::select!` + Spawn Relay Calls

This is the core fix. Replace the sequential polling loop with a `tokio::select!`-based event loop. Relay HTTP calls are spawned as background tasks so they never block heartbeat processing.

**Files:**
- Modify: `src/ipc/mod.rs`

**Step 1: Add `RelayTaskResult` struct and new imports**

Add near the top of `ipc/mod.rs` (after the existing struct definitions, around line 60):

```rust
/// Result of a spawned relay HTTP task (relay_start or relay_stop).
/// Sent back to the main select! loop via mpsc channel.
struct RelayTaskResult {
    request_id: String,
    action_type: String,
    ok: bool,
    error: Option<String>,
    detail: Option<String>,
    /// Populated on successful relay_start; used to update aegis_session_snapshot.
    session: Option<RelaySession>,
}
```

Add to the imports at the top (if not already present):
```rust
use tokio::sync::mpsc;
```

**Step 2: Add tick interval constant**

Add after the existing `STATUS_PUSH_INTERVAL` constants (~line 33):

```rust
#[cfg(not(test))]
const TICK_INTERVAL: Duration = Duration::from_millis(250);
#[cfg(test)]
const TICK_INTERVAL: Duration = Duration::from_millis(25);
```

The `READ_POLL_TIMEOUT` constant is no longer needed after the refactor. Remove it (lines 24-28):
```rust
// DELETE these lines:
#[cfg(not(test))]
const READ_POLL_TIMEOUT: Duration = Duration::from_millis(250);
#[cfg(test)]
const READ_POLL_TIMEOUT: Duration = Duration::from_millis(25);
```

**Step 3: Run tests (expect compilation failure)**

Run: `cargo test --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: Compilation succeeds (READ_POLL_TIMEOUT was only used in the loop body we're about to replace; if it's still referenced, keep it until step 5). All 31 tests pass.

If `READ_POLL_TIMEOUT` is still referenced, keep it for now and remove in step 5.

**Step 4: Refactor `handle_session_io` loop body**

This is the main change. Replace the entire `loop { ... }` body inside `handle_session_io` (starting at ~line 721) with the `tokio::select!`-based loop below.

Keep the local variable declarations before the loop (`protocol_errors`, `pending_switches`, `session_overrides`, `handshake_complete`, `last_ping_at`, `last_status_push_at`).

Add new variables after the existing ones:

```rust
    let (relay_result_tx, mut relay_result_rx) = mpsc::channel::<RelayTaskResult>(4);
    let mut relay_in_flight: Option<String> = None;
    let mut core_cmd_closed = false;
    let mut tick = tokio::time::interval(TICK_INTERVAL);
```

Replace the `loop { ... }` with:

```rust
    loop {
        tokio::select! {
            // ── Branch 1: Frame from C++ plugin ──────────────────────
            frame_result = read_frame(cmd_reader) => {
                let incoming: Envelope<serde_json::Value> = match frame_result {
                    Ok(frame) => frame,
                    Err(err) if err.kind() == io::ErrorKind::InvalidData => {
                        let msg = err.to_string();
                        let code = if msg.contains("frame too large") {
                            ProtocolErrorCode::FrameTooLarge
                        } else {
                            ProtocolErrorCode::DecodeFailed
                        };
                        let protocol_error = make_protocol_error(code, msg, None);
                        let _ = write_frame(evt_writer, &protocol_error).await;
                        update_debug_status(&debug_status, |s| {
                            s.last_notice = Some("IPC decode/frame protocol error".to_string());
                        });
                        if protocol_errors.record_and_should_reset() {
                            tracing::warn!("ipc session reset after repeated protocol errors");
                            return Ok(());
                        }
                        continue;
                    }
                    Err(err) => return Err(err),
                };

                // ── Protocol version check ──
                if incoming.v != IPC_PROTOCOL_VERSION {
                    // ... KEEP EXISTING version mismatch handling unchanged ...
                    continue;
                }

                // ── Message dispatch ──
                // All existing match arms stay the same EXCEPT relay_start_request
                // and relay_stop_request which are modified below.
                match incoming.message_type.as_str() {
                    "hello" => {
                        // ... KEEP EXISTING hello handling unchanged ...
                    }
                    "ping" => {
                        // ... KEEP EXISTING ping handling unchanged ...
                        // IMPORTANT: update last_ping_at here (existing code does this)
                        last_ping_at = Instant::now();
                        // ... write pong, etc ...
                    }
                    "request_status" => {
                        // ... KEEP EXISTING request_status handling unchanged ...
                        // NOTE: aegis_session_snapshot.lock() calls become .lock().await
                    }
                    "set_mode_request" => {
                        // ... KEEP EXISTING set_mode_request handling unchanged ...
                    }
                    "set_setting_request" => {
                        // ... KEEP EXISTING set_setting_request handling unchanged ...
                    }

                    // ── MODIFIED: relay_start_request ──
                    "relay_start_request" => {
                        let req: RelayStartRequestPayload = match decode_payload(&incoming) {
                            Ok(v) => v,
                            Err(err) => {
                                emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                                if protocol_errors.record_and_should_reset() {
                                    tracing::warn!("ipc session reset after repeated protocol errors");
                                    return Ok(());
                                }
                                continue;
                            }
                        };
                        if req.request_id.trim().is_empty() {
                            let protocol_error = make_protocol_error(
                                ProtocolErrorCode::InvalidPayload,
                                "Missing request_id for relay_start_request",
                                Some(incoming.id.clone()),
                            );
                            write_frame(evt_writer, &protocol_error).await?;
                            continue;
                        }

                        // In-flight guard: reject if another relay call is running
                        if let Some(ref existing_id) = relay_in_flight {
                            let result = make_envelope(
                                "relay_action_result",
                                Priority::High,
                                RelayActionResultPayload {
                                    request_id: req.request_id.clone(),
                                    action_type: "relay_start".to_string(),
                                    ok: false,
                                    error: Some("relay_action_already_in_flight".to_string()),
                                    detail: Some(format!("existing request: {existing_id}")),
                                },
                            );
                            tracing::info!(
                                request_id = %req.request_id,
                                existing = %existing_id,
                                "relay_start rejected: already in flight"
                            );
                            write_frame(evt_writer, &result).await?;
                            continue;
                        }

                        // Build client synchronously (no HTTP, just config+vault)
                        let client = match build_aegis_client_from_local_config() {
                            Ok(c) => c,
                            Err(err) => {
                                let result = make_envelope(
                                    "relay_action_result",
                                    Priority::High,
                                    RelayActionResultPayload {
                                        request_id: req.request_id.clone(),
                                        action_type: "relay_start".to_string(),
                                        ok: false,
                                        error: Some("relay_start_failed".to_string()),
                                        detail: Some(err),
                                    },
                                );
                                tracing::info!(
                                    request_id = %req.request_id,
                                    action_type = "relay_start",
                                    ok = false,
                                    error = "relay_start_failed",
                                    "ipc relay_action_result emitted"
                                );
                                write_frame(evt_writer, &result).await?;
                                continue;
                            }
                        };

                        // Mark in-flight and spawn background task
                        relay_in_flight = Some(req.request_id.clone());
                        let tx = relay_result_tx.clone();
                        let request_id = req.request_id.clone();
                        tokio::spawn(async move {
                            let request = RelayStartRequest {
                                region_preference: Some("auto".to_string()),
                                client_context: Some(RelayStartClientContext {
                                    obs_connected: None,
                                    mode: Some("studio".to_string()),
                                    requested_by: Some("dock_ui".to_string()),
                                }),
                            };
                            let idempotency_key = generate_idempotency_key();
                            let result = match client.relay_start(&idempotency_key, &request).await {
                                Ok(session) => RelayTaskResult {
                                    request_id,
                                    action_type: "relay_start".to_string(),
                                    ok: true,
                                    error: None,
                                    detail: Some(format!("relay_start_ok:{}", session.status)),
                                    session: Some(session),
                                },
                                Err(err) => RelayTaskResult {
                                    request_id,
                                    action_type: "relay_start".to_string(),
                                    ok: false,
                                    error: Some("relay_start_failed".to_string()),
                                    detail: Some(err.to_string()),
                                    session: None,
                                },
                            };
                            let _ = tx.send(result).await;
                        });
                    }

                    // ── MODIFIED: relay_stop_request ──
                    "relay_stop_request" => {
                        let req: RelayStopRequestPayload = match decode_payload(&incoming) {
                            Ok(v) => v,
                            Err(err) => {
                                emit_protocol_error_for_payload(evt_writer, &incoming, err).await?;
                                if protocol_errors.record_and_should_reset() {
                                    tracing::warn!("ipc session reset after repeated protocol errors");
                                    return Ok(());
                                }
                                continue;
                            }
                        };
                        if req.request_id.trim().is_empty() {
                            let protocol_error = make_protocol_error(
                                ProtocolErrorCode::InvalidPayload,
                                "Missing request_id for relay_stop_request",
                                Some(incoming.id.clone()),
                            );
                            write_frame(evt_writer, &protocol_error).await?;
                            continue;
                        }

                        // In-flight guard
                        if let Some(ref existing_id) = relay_in_flight {
                            let result = make_envelope(
                                "relay_action_result",
                                Priority::High,
                                RelayActionResultPayload {
                                    request_id: req.request_id.clone(),
                                    action_type: "relay_stop".to_string(),
                                    ok: false,
                                    error: Some("relay_action_already_in_flight".to_string()),
                                    detail: Some(format!("existing request: {existing_id}")),
                                },
                            );
                            tracing::info!(
                                request_id = %req.request_id,
                                existing = %existing_id,
                                "relay_stop rejected: already in flight"
                            );
                            write_frame(evt_writer, &result).await?;
                            continue;
                        }

                        // Build client synchronously
                        let client = match build_aegis_client_from_local_config() {
                            Ok(c) => c,
                            Err(err) => {
                                let result = make_envelope(
                                    "relay_action_result",
                                    Priority::High,
                                    RelayActionResultPayload {
                                        request_id: req.request_id.clone(),
                                        action_type: "relay_stop".to_string(),
                                        ok: false,
                                        error: Some("relay_stop_failed".to_string()),
                                        detail: Some(err),
                                    },
                                );
                                tracing::info!(
                                    request_id = %req.request_id,
                                    action_type = "relay_stop",
                                    ok = false,
                                    error = "relay_stop_failed",
                                    "ipc relay_action_result emitted"
                                );
                                write_frame(evt_writer, &result).await?;
                                continue;
                            }
                        };

                        // Mark in-flight and spawn background task
                        relay_in_flight = Some(req.request_id.clone());
                        let tx = relay_result_tx.clone();
                        let request_id = req.request_id.clone();
                        tokio::spawn(async move {
                            let result = match client.relay_active().await {
                                Ok(Some(session)) => {
                                    let stop_req = RelayStopRequest {
                                        session_id: session.session_id,
                                        reason: "dock_ui".to_string(),
                                    };
                                    match client.relay_stop(&stop_req).await {
                                        Ok(_) => RelayTaskResult {
                                            request_id,
                                            action_type: "relay_stop".to_string(),
                                            ok: true,
                                            error: None,
                                            detail: Some("relay_stop_ok".to_string()),
                                            session: None,
                                        },
                                        Err(err) => RelayTaskResult {
                                            request_id,
                                            action_type: "relay_stop".to_string(),
                                            ok: false,
                                            error: Some("relay_stop_failed".to_string()),
                                            detail: Some(err.to_string()),
                                            session: None,
                                        },
                                    }
                                }
                                Ok(None) => RelayTaskResult {
                                    request_id,
                                    action_type: "relay_stop".to_string(),
                                    ok: true,
                                    error: None,
                                    detail: Some("relay_stop_no_active_session".to_string()),
                                    session: None,
                                },
                                Err(err) => RelayTaskResult {
                                    request_id,
                                    action_type: "relay_stop".to_string(),
                                    ok: false,
                                    error: Some("relay_stop_failed".to_string()),
                                    detail: Some(format!("relay_active_lookup_failed: {err}")),
                                    session: None,
                                },
                            };
                            let _ = tx.send(result).await;
                        });
                    }

                    "scene_switch_result" => {
                        // ... KEEP EXISTING scene_switch_result handling unchanged ...
                    }
                    unknown => {
                        // ... KEEP EXISTING unknown message_type handling unchanged ...
                    }
                }
            }

            // ── Branch 2: Relay result from spawned background task ──
            Some(result) = relay_result_rx.recv() => {
                let action_result = make_envelope(
                    "relay_action_result",
                    Priority::High,
                    RelayActionResultPayload {
                        request_id: result.request_id.clone(),
                        action_type: result.action_type.clone(),
                        ok: result.ok,
                        error: result.error.clone(),
                        detail: result.detail.clone(),
                    },
                );
                tracing::info!(
                    request_id = %result.request_id,
                    action_type = %result.action_type,
                    ok = result.ok,
                    "ipc relay_action_result emitted"
                );
                write_frame(evt_writer, &action_result).await?;

                // Update session snapshot
                if result.ok {
                    if let Some(session) = result.session {
                        *aegis_session_snapshot.lock().await = Some(session);
                    } else if result.action_type == "relay_stop" {
                        *aegis_session_snapshot.lock().await = None;
                    }
                }

                // Push status snapshot with updated relay state
                let frame = rx.borrow().clone();
                let relay = aegis_session_snapshot.lock().await.clone();
                let payload = build_status_snapshot_with_overrides(
                    &frame,
                    relay.as_ref(),
                    &session_overrides,
                );
                let snapshot = make_envelope("status_snapshot", Priority::High, payload);
                write_frame(evt_writer, &snapshot).await?;
                last_status_push_at = Instant::now();

                // Clear in-flight flag
                relay_in_flight = None;
            }

            // ── Branch 3: Core command (switch_scene from failover engine) ──
            result = core_cmd_rx.recv(), if !core_cmd_closed => {
                match result {
                    Ok(cmd) => {
                        if !handshake_complete {
                            tracing::debug!("dropping core ipc command before handshake");
                            continue;
                        }
                        match cmd {
                            CoreIpcCommand::SwitchScene {
                                scene_name,
                                reason,
                                deadline_ms,
                            } => {
                                // ... KEEP EXISTING switch_scene handling unchanged ...
                                // (build envelope, write_frame, insert pending_switches,
                                //  update debug_status)
                                let request_id = Uuid::new_v4().to_string();
                                let request_ts = now_unix_ms();
                                let evt = make_envelope(
                                    "switch_scene",
                                    Priority::Critical,
                                    SwitchScenePayload {
                                        request_id: request_id.clone(),
                                        scene_name: scene_name.clone(),
                                        reason,
                                        deadline_ms,
                                    },
                                );
                                write_frame(evt_writer, &evt).await?;
                                pending_switches.insert(
                                    request_id,
                                    PendingSwitchScene {
                                        scene_name,
                                        deadline_at: Instant::now()
                                            + Duration::from_millis(deadline_ms),
                                    },
                                );
                                let payload = evt.payload.clone();
                                update_debug_status(&debug_status, |s| {
                                    s.pending_switch_count = pending_switches.len() as u32;
                                    s.last_switch_request = Some(IpcSwitchRequestDebug {
                                        request_id: payload.request_id,
                                        scene_name: payload.scene_name,
                                        reason: payload.reason,
                                        deadline_ms: payload.deadline_ms,
                                        ts_unix_ms: request_ts,
                                    });
                                });
                            }
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(n)) => {
                        tracing::warn!(dropped = n, "core ipc command channel lagged");
                    }
                    Err(broadcast::error::RecvError::Closed) => {
                        tracing::debug!("core ipc command channel closed");
                        core_cmd_closed = true;
                    }
                }
            }

            // ── Branch 4: Tick — heartbeat, status push, pending switch expiry ──
            _ = tick.tick() => {
                // Heartbeat check
                if handshake_complete && last_ping_at.elapsed() >= HEARTBEAT_TIMEOUT {
                    let protocol_error = make_protocol_error(
                        ProtocolErrorCode::Timeout,
                        "Heartbeat timeout (missing ping)",
                        None,
                    );
                    let _ = write_frame(evt_writer, &protocol_error).await;
                    tracing::warn!("ipc session closed after heartbeat timeout");
                    update_debug_status(&debug_status, |s| {
                        s.last_notice =
                            Some("Heartbeat timeout (missing ping)".to_string());
                    });
                    return Ok(());
                }

                // Periodic status push
                if handshake_complete
                    && last_status_push_at.elapsed() >= STATUS_PUSH_INTERVAL
                {
                    let frame = rx.borrow().clone();
                    let relay = aegis_session_snapshot.lock().await.clone();
                    let payload = build_status_snapshot_with_overrides(
                        &frame,
                        relay.as_ref(),
                        &session_overrides,
                    );
                    let snapshot =
                        make_envelope("status_snapshot", Priority::Normal, payload);
                    write_frame(evt_writer, &snapshot).await?;
                    last_status_push_at = Instant::now();
                }

                // Pending switch expiry
                if !pending_switches.is_empty() {
                    let now = Instant::now();
                    let expired_ids: Vec<String> = pending_switches
                        .iter()
                        .filter_map(|(id, pending)| {
                            (now >= pending.deadline_at).then_some(id.clone())
                        })
                        .collect();
                    for id in expired_ids {
                        if let Some(expired) = pending_switches.remove(&id) {
                            tracing::warn!(
                                request_id = %id,
                                scene_name = %expired.scene_name,
                                "ipc switch_scene request timed out"
                            );
                            let notice = make_envelope(
                                "user_notice",
                                Priority::High,
                                UserNoticePayload {
                                    level: UserNoticeLevel::Warn,
                                    message: format!(
                                        "Scene switch to '{}' timed out (request {})",
                                        expired.scene_name, id
                                    ),
                                },
                            );
                            let _ = write_frame(evt_writer, &notice).await;
                            update_debug_status(&debug_status, |s| {
                                s.pending_switch_count =
                                    pending_switches.len() as u32;
                                s.last_switch_result = Some(IpcSwitchResultDebug {
                                    request_id: id.clone(),
                                    status: "timeout".to_string(),
                                    error: None,
                                    ts_unix_ms: now_unix_ms(),
                                });
                                s.last_notice = Some(format!(
                                    "Scene switch '{}' timed out ({})",
                                    expired.scene_name, id
                                ));
                            });
                        }
                    }
                }
            }
        }
    }
```

**Key differences from the old loop:**

| Aspect | Old (sequential) | New (select!) |
|--------|-------------------|---------------|
| Frame read | `tokio::time::timeout(250ms, read_frame(...))` | Direct `read_frame(...)` in select branch |
| Core commands | `core_cmd_rx.try_recv()` drain loop | `core_cmd_rx.recv()` as select branch |
| Heartbeat/status/expiry | Checked every loop iteration | Checked in `tick.tick()` branch (250ms) |
| Relay HTTP calls | Awaited inline (BLOCKS loop) | Spawned as background tasks |
| Relay results | Written immediately after HTTP await | Received via `relay_result_rx` channel |

**Step 5: Run tests**

Run: `cargo test --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: All 31 tests pass. The existing IPC tests exercise hello/ack, heartbeat timeout, ping/pong, status snapshots, protocol errors, switch_scene — all of which should work identically in the new select! loop.

Run: `cargo clippy --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: No new warnings. If `READ_POLL_TIMEOUT` was kept, clippy may warn about dead code — remove it now.

**Step 6: Commit**

```bash
git add src/ipc/mod.rs
git commit -m "fix: refactor IPC loop to tokio::select!, spawn relay HTTP calls

Relay HTTP calls (up to 15s) no longer block the IPC session loop.
Heartbeat processing continues while relay calls are in flight.
Adds in-flight guard to reject duplicate relay requests.

Fixes relay_action_result_not_observed timeout bug."
```

---

### Task 5: Final Verification

**Files:** None (verification only)

**Step 1: Full test suite**

Run: `cargo test --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: `test result: ok. 31 passed; 0 failed;`

**Step 2: Clippy**

Run: `cargo clippy --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml -- -D warnings`
Expected: No warnings, no errors.

**Step 3: Release build**

Run: `cargo build --release --manifest-path telemy-v0.0.3/obs-telemetry-bridge/Cargo.toml`
Expected: Compiles successfully.

**Step 4: Commit any clippy/build fixes**

If step 2 or 3 surfaced issues, fix them and commit:
```bash
git commit -m "fix: address clippy warnings from relay refactor"
```

**Step 5: Tag the work**

```bash
git log --oneline -5
```

Expected output shows 3-4 commits:
1. `refactor: extract shared aegis_client module (dedup #11)`
2. `fix: SeqCst -> Release/Acquire for shutdown flag (#13)`
3. `refactor: swap std::sync::Mutex to tokio::sync::Mutex (#9)`
4. `fix: refactor IPC loop to tokio::select!, spawn relay HTTP calls`

---

## Implementation Notes

### What existing tests verify after the refactor

| Test | Verifies |
|------|----------|
| `session_sends_hello_ack_and_periodic_status_snapshot` | Select loop processes hello frame, emits hello_ack and periodic status_snapshot via tick branch |
| `session_emits_timeout_protocol_error_when_heartbeat_missing` | Tick branch correctly fires heartbeat timeout when no ping received |
| `session_replies_to_ping_with_matching_pong` | Frame branch dispatches ping, writes pong, resets `last_ping_at` |
| `session_returns_status_snapshot_on_request_status` | Frame branch handles request_status, reads watch channel, emits snapshot |
| `malformed_payload_emits_invalid_payload_protocol_error` | Frame branch error handling for decode failures |
| `unknown_message_type_emits_unknown_type_protocol_error` | Frame branch default arm for unknown messages |
| `repeated_protocol_errors_trigger_controlled_session_reset` | Protocol error tracker works across select iterations |
| `core_switch_scene_command_emits_event_and_ack_clears_timeout` | Core cmd branch processes SwitchScene, frame branch processes scene_switch_result |
| `core_switch_scene_command_timeout_emits_user_notice` | Tick branch expires pending switches |
| `repeated_identical_set_mode_request_is_noop` | Frame branch dedup logic for set_mode |
| `repeated_identical_set_setting_request_is_noop` | Frame branch dedup logic for set_setting |

### What requires manual testing with OBS (Priority #4 on handoff list)

- Send relay_start from dock UI → verify relay_action_result arrives (not timeout)
- Send relay_stop from dock UI → verify relay_action_result arrives (not timeout)
- Heartbeat stays alive during 15s relay HTTP call
- Rapid relay_start + relay_start → second one rejected with `relay_action_already_in_flight`

### Gotchas for the implementer

1. **`last_ping_at` reset**: Must happen inside the `"ping"` match arm in the frame branch. If you accidentally put it at the top of the frame branch (before the match), ANY frame would reset the heartbeat — that's wrong. Only `ping` frames reset it.

2. **`broadcast::Receiver` in select**: Use the `if !core_cmd_closed` precondition guard. Without it, a closed channel causes `recv()` to return `Err(Closed)` immediately on every select iteration, creating a busy-loop.

3. **`tokio::sync::Mutex` in ipc**: The module still uses `std::sync::Mutex` for `IpcDebugStatusHandle`. Don't accidentally change that — `update_debug_status` is called from synchronous closures. Use the fully-qualified `tokio::sync::Mutex` for the `aegis_session_snapshot` parameter type.

4. **`read_frame` EOF**: When the pipe closes, `read_frame` returns `Err` with EOF. The `Err(err) => return Err(err)` branch handles this. The session ends, the spawned relay task (if any) will fail to send its result (receiver dropped), and the result is silently discarded. This is correct behavior.

5. **Test timing**: The `TICK_INTERVAL` in tests is 25ms (matching old `READ_POLL_TIMEOUT`). If tests become flaky, increase `HEARTBEAT_TIMEOUT` test value from 350ms to 500ms. But the existing values should work since select! is more responsive than polling.
