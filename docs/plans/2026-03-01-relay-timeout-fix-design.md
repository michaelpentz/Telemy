# Relay Timeout Fix + Hardening — Design

Date: 2026-03-01
Status: Approved

## Problem

`relay_start` and `relay_stop` dock actions complete with `relay_action_result_not_observed` (timeout) instead of `completed`/`failed`.

### Root Cause

The IPC session loop in `ipc/mod.rs` (`handle_session_io`) is a sequential polling loop. Relay HTTP calls (`client.relay_start().await`, `client.relay_active().await`, `client.relay_stop().await`) are awaited inline inside match arms. These calls can take up to 15 seconds (the `DEFAULT_TIMEOUT_SECS` on `ControlPlaneClient`).

While an HTTP call is in-flight, the loop is suspended:
- No `ping` frames are read from the C++ plugin
- `last_ping_at` goes stale
- On the next iteration after the HTTP call returns, the heartbeat check fires (`HEARTBEAT_TIMEOUT = 3500ms`)
- The session is terminated with a protocol error
- If the pipe is already broken by the time `write_frame(evt_writer, &result).await?` executes, the `?` propagates the error and the `relay_action_result` is never written

The C++ plugin's 15-second pending relay action timer expires with `relay_action_result_not_observed`.

### Break Points (file:line)

1. `ipc/mod.rs:1144` — `client.relay_start(&idempotency_key, &request).await` blocks loop
2. `ipc/mod.rs:1252` — `client.relay_active().await` blocks loop (relay_stop first call)
3. `ipc/mod.rs:1283` — `client.relay_stop(&stop_req).await` blocks loop (relay_stop second call)
4. `ipc/mod.rs:827` — heartbeat timeout fires after stale `last_ping_at`
5. `ipc/mod.rs:1166` — `write_frame(evt_writer, &result).await?` fails on broken pipe

## Solution: tokio::select! Loop Refactor

### Loop Architecture

Replace the sequential polling loop with a `tokio::select!`-based event loop:

```rust
let (relay_result_tx, mut relay_result_rx) = mpsc::channel::<RelayTaskResult>(4);
let mut tick = tokio::time::interval(Duration::from_millis(250));

loop {
    tokio::select! {
        // 1. Frame from C++ plugin (cmd pipe)
        frame_result = read_frame(&mut cmd_reader) => {
            match frame_result {
                Ok(frame) => { /* dispatch on message_type */ }
                Err(e) if e.kind() == InvalidData => { /* protocol error handling */ }
                Err(e) => return Err(e),
            }
        }

        // 2. Relay API result (from spawned background task)
        Some(result) = relay_result_rx.recv() => {
            write relay_action_result to evt_writer
            update aegis_session_snapshot if success
            push status_snapshot
            clear relay_in_flight
        }

        // 3. Core command (switch_scene from failover engine)
        Ok(cmd) = core_cmd_rx.recv() => {
            write switch_scene to evt_writer
            track pending switch
        }

        // 4. Tick: heartbeat check + periodic status push + pending switch expiry
        _ = tick.tick() => {
            if heartbeat expired -> return Ok(())
            if status push interval elapsed -> push snapshot
            expire timed-out pending switches
        }
    }
}
```

### Relay Spawn Pattern

When `relay_start_request` or `relay_stop_request` arrives:

1. Parse payload, validate `request_id`
2. Build `ControlPlaneClient` (synchronous, no HTTP)
3. Check `relay_in_flight` — reject if already in-flight (write error result immediately)
4. Set `relay_in_flight = Some(request_id.clone())`
5. Clone: `relay_result_tx`, `aegis_session_snapshot`, `client`, request data
6. `tokio::spawn(async move { ... })` — runs HTTP calls, sends result through channel

```rust
struct RelayTaskResult {
    request_id: String,
    action_type: String,  // "relay_start" or "relay_stop"
    ok: bool,
    error: Option<String>,
    detail: Option<String>,
    session: Option<RelaySession>,  // populated on successful start
}
```

When `relay_result_rx.recv()` fires:
1. Write `relay_action_result` envelope to `evt_writer`
2. If `result.ok && result.session.is_some()`, update `aegis_session_snapshot`
3. If `result.ok && action_type == "relay_stop"`, clear `aegis_session_snapshot`
4. Push `status_snapshot` with updated relay state
5. Set `relay_in_flight = None`

### Edge Cases

- **Session drops during in-flight relay task**: Spawned task completes, `relay_result_tx.send()` fails (receiver dropped), result discarded. C++ times out — correct behavior.
- **Duplicate relay request while one is in-flight**: Immediately rejected with `relay_action_result { ok: false, error: "relay_action_already_in_flight" }`.
- **Client build failure**: Handled synchronously before spawn (no HTTP needed). Error result written immediately.

## Hardening Bundle

### #13 — SeqCst Ordering (tray/mod.rs)

Change `Ordering::SeqCst` to `Ordering::Release` (store, line 47) and `Ordering::Acquire` (load, line 52) on the shutdown `AtomicBool`. These are the correct orderings for a simple flag — SeqCst is unnecessarily strong.

### #11 — Deduplicate Helpers

Extract into new `src/aegis_client.rs`:

```rust
pub fn generate_idempotency_key() -> String { Uuid::new_v4().to_string() }

pub fn build_aegis_client(
    config: &Config,
    vault: &Vault,
) -> Result<ControlPlaneClient, Box<dyn std::error::Error>> { ... }

pub fn build_aegis_client_from_local_config() -> Result<ControlPlaneClient, String> { ... }
```

Remove duplicate definitions from `app/mod.rs`, `server/mod.rs`, `ipc/mod.rs`. All three import from the shared module.

### #9 — Async Mutex Swap (server/mod.rs)

Replace `std::sync::Mutex` with `tokio::sync::Mutex` for three fields in `ServerState`:
- `vault: Arc<Mutex<Vault>>`
- `grafana_configured: Arc<Mutex<bool>>`
- `aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>`

Change all `.lock().unwrap_or_else(|p| p.into_inner())` to `.lock().await` (~22 call sites). `tokio::sync::Mutex` does not poison, so no unwrap handling needed.

## Files Changed

| File | Change | Effort |
|------|--------|--------|
| `src/ipc/mod.rs` | Refactor `handle_session_io` to `tokio::select!`, spawn relay calls | Medium |
| `src/aegis_client.rs` | New shared module for client builder + idempotency key | Small |
| `src/app/mod.rs` | Import from `aegis_client` instead of local definitions | Small |
| `src/server/mod.rs` | Import from `aegis_client` + swap to `tokio::sync::Mutex` | Small |
| `src/tray/mod.rs` | SeqCst → Release/Acquire (2 lines) | Trivial |
| `src/main.rs` or `src/lib.rs` | Add `mod aegis_client;` | Trivial |

## Testing

- `cargo test` — all 31 existing tests must pass
- `cargo clippy` — no new warnings
- The IPC test harness in `ipc/mod.rs` uses shorter timeouts (`#[cfg(test)]`) and should exercise the new select loop
- Full validation requires OBS running (separate step — Priority #4 on handoff list)
