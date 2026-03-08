# Aegis State Machine Spec v1 (v0.0.4 Native)

## 1. Scope

This spec defines the authoritative runtime state machine for the Telemy v0.0.4 native C++ plugin.

Responsibilities:
- Determine desired OBS scene target from local and cloud signals.
- Enforce deterministic transitions and conflict resolution.
- Support crash recovery and reconnect semantics within the OBS process.

---

## 2. State Model

## 2.1 Top-Level Modes

1. `STUDIO`
- Local-only monitoring active.
- CloudLink inactive or disconnected.

2. `IRL_CONNECTING`
- Cloud relay start requested, awaiting async provisioning and telemetry.

3. `IRL_ACTIVE`
- Cloud relay telemetry connected and healthy.

4. `IRL_GRACE`
- Relay disconnected or ingest lost; grace timer active.

5. `DEGRADED`
- Critical dependency unavailable or OBS control path unstable, limited automation.

6. `FATAL`
- Non-recoverable runtime failure; requires operator action.

## 2.2 Scene Intents

Scene intent is orthogonal to mode and may be:
- `LIVE`
- `BRB`
- `OFFLINE`
- `HOLD` (no change)

Final scene command is emitted only when intent differs from current scene and guards pass.

---

## 3. Inputs

## 3.1 Local Inputs (Native Polling)

- `obs_bitrate_kbps`
- `obs_rtt_ms`
- `obs_connected` (OBS module state)
- `chat_command` (authorized)

## 3.2 Cloud Inputs (IRL path)

- `relay_session_status` (`provisioning|active|grace|stopped`)
- `relay_provision_step` (`launching_instance|waiting_for_instance|starting_docker|starting_containers|creating_stream|ready`)
- `relay_ingest_active` (bool)
- `relay_telemetry_connected` (bool)
- `grace_remaining_seconds`

## 3.3 System Inputs

- `startup`
- `obs_shutdown_notice`
- `config_reload`

---

## 4. Guard Conditions

Global guards:
1. Scene switch blocked if manual override is enabled (except explicit admin commands).
2. Chat mutating commands require RBAC + cooldown pass.

IRL guards:
1. Enter `IRL_ACTIVE` only if relay telemetry auth succeeded.
2. Transition to `IRL_GRACE` only if prior mode was `IRL_ACTIVE`.
3. Exit `IRL_GRACE` to `IRL_ACTIVE` on telemetry reconnect before timer expiry.

---

## 5. Transition Table (Top-Level Modes)

1. `STUDIO -> IRL_CONNECTING`
- Trigger: user/admin activates IRL (`ui` or `chat`) and CloudLink start accepted.

2. `IRL_CONNECTING -> IRL_ACTIVE`
- Trigger: relay session active and telemetry connected.

3. `IRL_CONNECTING -> STUDIO`
- Trigger: start request denied, timeout, or explicit cancel.

4. `IRL_ACTIVE -> IRL_GRACE`
- Trigger: relay telemetry disconnect OR ingest becomes inactive.

5. `IRL_GRACE -> IRL_ACTIVE`
- Trigger: telemetry and ingest recover before grace expiry.

6. `IRL_GRACE -> STUDIO`
- Trigger: grace expires OR explicit IRL off command.

7. `IRL_ACTIVE -> STUDIO`
- Trigger: explicit IRL off command and stop acknowledged.

8. `ANY -> DEGRADED`
- Trigger: OBS control unavailable or protocol error storm.

9. `DEGRADED -> prior stable mode`
- Trigger: dependency health restored and validation probe succeeds.

10. `ANY -> FATAL`
- Trigger: unrecoverable config/auth/runtime invariant failure.

---

## 6. Scene Decision Rules

Rule order (highest priority first):

1. `Manual admin/mod explicit switch`:
- Authorized explicit command (`!live`, `!brb`, `!offline`, `!switch <scene>`) wins immediately, unless safety guard blocks.

2. `Local safety failover`:
- If local signal indicates hard failure (`obs_bitrate_kbps == 0` sustained over threshold), intent -> `BRB` or configured failover scene.
- Local hard-failure rule has precedence over cloud healthy signal.

3. `IRL cloud signal`:
- In IRL modes, relay signal may request failover/recover intent.

4. `Hysteresis recovery`:
- Return from failover to `LIVE` only after recovery stability window.

5. `HOLD`:
- If no rule triggers, keep current scene.

Determinism:
- Only one intent chosen per evaluation tick.
- Ties resolved by rule order above.

---

## 7. Timing Parameters (v1 defaults)

- Local polling interval: 500ms
- Zero-bitrate failure threshold: 1 poll hit (configurable)
- Recovery stability window: 3 consecutive healthy polls
- IRL grace window: 600s
- Mode transition debounce (non-critical): 250ms
- Relay provision step dwell: 3s (minimum)

All thresholds are configuration-backed with sensible bounds.

---

## 8. Reconnect-First Startup Logic

On startup:
1. Initialize local subsystems.
2. If cloud enabled, call `GET /relay/active`.
3. If active session exists:
- Restore IRL context.
- Attempt relay telemetry reconnect.
- Enter `IRL_ACTIVE` on success, else `IRL_GRACE` if within window.
4. If no active session:
- Enter `STUDIO`.

No implicit provisioning on startup.

---

## 9. Failure Handling Policies

1. Native Control failure:
- Enter `DEGRADED`, suppress auto scene-switch commands requiring plugin path.

2. Backend unreachable:
- Stay in `STUDIO` for local operation.
- New IRL activation denied with user-facing notice.

3. Relay active during backend outage:
- Continue IRL operation while ingest and telemetry remain active.
- Rely on C1 relay teardown conditions for safety.

---

## 10. Formal Invariants

1. No duplicate active relay sessions per user.
2. No scene-switch emit when native OBS control is unstable.
3. Pair token is never used for control-plane auth.
4. State transitions must match table in section 5.
5. Every emitted scene-switch command must have traceable reason and request id.

---

## 11. Test Matrix (Minimum)

## 11.1 Transition Tests

1. `STUDIO -> IRL_CONNECTING -> IRL_ACTIVE` happy path.
2. `IRL_CONNECTING -> STUDIO` on backend deny/timeout.
3. `IRL_ACTIVE -> IRL_GRACE -> IRL_ACTIVE` recover within grace.
4. `IRL_ACTIVE -> IRL_GRACE -> STUDIO` grace expiry.

## 11.2 Precedence Tests

1. Local zero bitrate and cloud healthy => local failover wins.
2. Authorized manual switch overrides automated `LIVE`.
3. Override mode blocks auto-switch rules.

## 11.3 Safety Tests

1. OBS control unstable => no scene-switch emit, `DEGRADED` entered.
2. Startup with existing active session => reconnect path, no new provisioning.

## 11.4 Timing Tests

1. Worst-case local detection-to-command within configured bounds.
2. Recovery hysteresis prevents rapid scene flapping.

---

## 12. Observability Requirements

Emit structured events for:
- Mode transitions (`from`, `to`, `trigger`)
- Scene intent decisions (`rule`, `intent`, `reason`)
- Guard rejections (`guard_name`)
- Reconnect attempts and outcomes
- Grace timer start/stop/expiry

Key metrics:
- `state_transition_total{from,to}`
- `scene_intent_total{intent,rule}`
- `grace_entries_total`
- `grace_recoveries_total`
- `degraded_duration_seconds`

---

## 13. Acceptance Criteria

State machine v1 is accepted when:
1. All invariants in section 10 are enforced by tests.
2. Transition and precedence matrix in section 11 passes.
3. Structured logs can reconstruct every scene switch decision.
4. Reconnect-first startup behaves deterministically across restarts.

