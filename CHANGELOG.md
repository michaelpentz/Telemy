# Changelog - Telemy v0.0.3

All notable changes to the Telemy project will be documented in this file.

## [v0.0.3]

### 2026-03-02 — Backend Hardening & Architectural Refinement

#### Added
- **Feat: Relay IPC Wiring** (`4d9a9f5`):
  - C++ plugin now forwards `relay_start`/`relay_stop` to Rust core with pending-action tracking.
  - Implemented `relay_action_result` consumption and resolution in the plugin shim.
  - Extended MsgPack parser with support for signed int, float, binary, and ext types.
- **Harden: Mutex Poison Recovery** (`a35e5e3`):
  - Introduced `MutexExt` trait with `lock_or_recover()` to handle poisoned mutexes without panicking.
  - Replaced all 36 occurrences of `lock().unwrap()` across `app`, `server`, and `ipc` modules.
- **Harden: Exporter Observability** (`a35e5e3`):
  - Integrated OpenTelemetry error handler for Grafana exporter to log `PeriodicReader` failures.
  - Added `ExporterHealth` counter to track cumulative export errors, reported every 60 cycles.

#### Fixed
- **Fix: Idempotency Key Format** (`4d9a9f5`):
  - Switched from `telemy-{ts}-{random}` to `Uuid::new_v4()` for compatibility with Go control plane.
- **Fix: Instantaneous Bitrate** (`3198766`):
  - Switched from session average to delta-based instantaneous bitrate calculation with per-output tracking.
- **Fix: Script Hardening** (`4d9a9f5`):
  - Added strict validation fallback mode and completion timeout guards to PowerShell scripts.
  - Fixed hashtable splatting bugs in `run-strict-cycle.ps1`.

#### Refactored
- **Refactor: Server Module Split** (`3198766`):
  - Split the 1810-line `server/mod.rs` into logical sub-modules: `dashboard.rs`, `settings.rs`, and `aegis.rs`.
  - Reduced `mod.rs` to 418 lines (77% reduction) for better maintainability.

### 2026-03-01 — Dock UX & Relay Bridge Implementation

#### Added
- **Feat: Encoders & Uploads Section** (`68a2647`):
  - New dedicated section in the OBS dock for monitoring individual encoder and upload health.
  - Per-output health bars with self-calibrating rolling max bitrate (0.2% decay).
  - Support for grouping outputs (e.g., Horizontal vs. Vertical) and hiding inactive streams.
- **Test: Relay IPC Coverage** (`3dc48d1`):
  - Added 3 new tests for relay action result handling (Total: 34 tests, all passing).
  - Verified `relay_start`/`relay_stop` failure states for missing configurations and session persistence during pending results.

#### Fixed
- **Fix: Relay Timeout & IPC Loop** (`579fecd`):
  - Resolved relay action timeouts by implementing a non-blocking `tokio::select!` loop in the Rust core.
  - Added background task spawning for long-running relay operations, ensuring the main IPC bridge remains responsive.
- **Fix: Dock UX & Persistence** (`ab6e2d0`):
  - **Deferred Show:** Implemented 1.5s `QTimer` delay to respect OBS layout restoration and eliminate the center-screen floating flash on launch.
  - **Synthetic Theme:** Added fallback theme replay from Qt palette cache to ensure immediate OBS theme application on dock load.
  - **Scene Prefs:** Added `load_scene_prefs`/`save_scene_prefs` native disk storage (`dock_scene_prefs.json`). Scene-to-rule links now persist across OBS restarts.

#### Documentation
- **Docs: Architecture & Validation** (`adc1bc0`):
  - Updated `ARCHITECTURE.md` with new initialization patterns and data flows.
  - Created `TRIAGE_RELAY_VALIDATION.md` to define policies for strict automated validation in environments with stale logs.
  - New `OBS_DOCK_THEME_PERSISTENCE.md` guide for maintaining stable UI colors.
  - Expanded QA checklists for regression testing of dock actions and scene rules.
  - Updated `RELEASE_NOTES.md` with current session milestones.
