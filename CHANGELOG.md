# Changelog - Telemy v0.0.3

All notable changes to the Telemy project will be documented in this file.

## [v0.0.3] - 2026-03-01

### Added
- **Feat: Encoders & Uploads Section** (`68a2647`):
  - New dedicated section in the OBS dock for monitoring individual encoder and upload health.
  - Per-output health bars with self-calibrating rolling max bitrate (0.2% decay).
  - Support for grouping outputs (e.g., Horizontal vs. Vertical) and hiding inactive streams.
- **Test: Relay IPC Coverage** (`3dc48d1`):
  - Added 3 new tests for relay action result handling (Total: 34 tests, all passing).
  - Verified `relay_start`/`relay_stop` failure states for missing configurations and session persistence during pending results.

### Fixed
- **Fix: Relay Timeout & IPC Loop** (`579fecd`):
  - Resolved relay action timeouts by implementing a non-blocking `tokio::select!` loop in the Rust core.
  - Added background task spawning for long-running relay operations, ensuring the main IPC bridge remains responsive.
- **Fix: Dock UX & Persistence** (`ab6e2d0`):
  - **Deferred Show:** Implemented 1.5s `QTimer` delay to respect OBS layout restoration and eliminate the center-screen floating flash on launch.
  - **Synthetic Theme:** Added fallback theme replay from Qt palette cache to ensure immediate OBS theme application on dock load.
  - **Scene Prefs:** Added `load_scene_prefs`/`save_scene_prefs` native disk storage (`dock_scene_prefs.json`). Scene-to-rule links now persist across OBS restarts.

### Documentation
- **Docs: Architecture & Validation** (`adc1bc0`):
  - Updated `ARCHITECTURE.md` with new initialization patterns and data flows.
  - Created `TRIAGE_RELAY_VALIDATION.md` to define policies for strict automated validation in environments with stale logs.
  - New `OBS_DOCK_THEME_PERSISTENCE.md` guide for maintaining stable UI colors.
  - Expanded QA checklists for regression testing of dock actions and scene rules.
  - Updated `RELEASE_NOTES.md` with current session milestones.
