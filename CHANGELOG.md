 Changelog

All notable changes to telemy-v0.0.5 will be documented in this file.


## [0.0.5-dev] - 2026-03-20

### Fixed - E2E Gaps

- **Snapshot push on connect/disconnect** — `connection_connect` and `connection_disconnect` (BYOR path) now emit a fresh telemetry snapshot immediately after completion, so the dock reflects updated state without waiting for the next 500ms tick.
- **relay_host_masked field** — `MaskRelayHost()` added; relay connections snapshot now includes a `relay_host_masked` field (e.g. `"my-relay.e***.com"`) for safe display in the dock without exposing the full host.
- **not_found error for unknown connection IDs** — `connection_connect` and `connection_disconnect` now return a `not_found` error when the given `connection_id` doesn't exist. Previously they silently succeeded, masking misconfigured dock state.

### Added - MetricsCollector Background Thread

- **Start()/Stop()/PollLoop()** — MetricsCollector now runs polling on its own background thread instead of the OBS render thread. `Start()` launches the thread; `Stop()` signals and joins it. `Poll()` is no longer called directly from the OBS tick callback — the tick only reads the cached snapshot via `Latest()`. Eliminates any render-thread stalls from metric collection.

### Added - Dock UI

- **Separate font and density axes** — Font size and UI density are now independent settings. Font scale controls text size; density controls spacing/padding. Previously linked.
- **C1 sanitizer** — Added C++ sanitizer for C1 control characters (U+0080–U+009F) that break JSON parsing. Prevents OBS output names containing these characters from corrupting the telemetry snapshot.
- **Density-aware dock spacing** — Dock layout spacing now scales with the density setting.
- **Per-link freshness indicators** — Each relay link row shows a freshness indicator based on `last_ms_ago`. Links not updated within 3s fade to indicate staleness.
- **Dynamic OBS font scaling** — Dock font size adjusts dynamically based on the font scale setting rather than requiring a reload.
- **C++ char sanitizer** — Shared C++ string sanitizer for output/encoder names strips control characters before JSON serialization.

## [0.0.5-dev] - 2026-03-18

### Added - Phase 1: Provider Abstraction

- **InstanceID rename** - Renamed AWSInstanceID -> InstanceID across provisioner interface, handlers, store, model, types. Migration 010 renames DB column. Removes AWS coupling from shared types.
- **EIP behind AWS provider** - Moved Elastic IP allocation/association/DNS logic fully behind the AWSProvisioner boundary. unProvisionPipeline() no longer type-asserts the provisioner - EIP orchestration is an internal AWS concern.

### Added - Phase 2: BYOR (Bring Your Own Relay)

- **BYORProvisioner** (internal/relay/byor.go) - Implements Provisioner interface for user-owned relays. Reads stored relay config (host, port, stream key) from DB instead of launching cloud infrastructure.
- **Migration 011** - Adds yor_host, yor_port, yor_stream_key columns to users table. Updates CHECK constraint on relay_sessions.provider to include 'byor'.
- **Relay config endpoint** - POST /api/v1/user/relay-config allows free-tier users to save their BYOR relay connection details.
- **Per-user routing** - handleRelayStart() routes based on plan_tier: free -> BYORProvisioner, managed tiers -> AWSProvisioner. Returns yor_config_required error for unconfigured free users.
- **relay_mode in auth session** - GET /api/v1/auth/session returns relay_mode field (yor/managed/none) so the dock UI can show appropriate controls.
- **Plugin ConnectDirect** - C++ plugin ConnectDirect/DisconnectDirect path for BYOR relays, bypassing managed provisioning lifecycle.
- **Dock BYOR UI** - BYOR settings panel in telemy-dock.jsx with toggle, host/port/stream inputs, connect/disconnect button. Bridge JS wired for relay_connect_direct/relay_disconnect_direct actions.
- **BYOR test plan** - Integration test plan covering Go unit tests, C++ plugin tests, E2E manual procedure, and regression tests.
- **Graceful stats degradation** - Plugin handles missing/unreachable stats endpoints on non-SLS relays without erroring.
## [0.0.4] - 2026-03-16

### Added - Plugin Auth Flow

- **Browser-handoff login** - Plugin login via browser handoff: `POST /api/v1/auth/plugin/login/start` initiates login and returns a browser URL + attempt ID; `POST /api/v1/auth/plugin/login/poll` polls for completion; `POST /api/v1/auth/plugin/login/complete` binds the attempt to a user and issues auth material (called by web tier, not plugin).
- **Session management endpoints** - `GET /api/v1/auth/session` (validate session + entitlement), `POST /api/v1/auth/refresh` (rotate access token), `POST /api/v1/auth/logout` (invalidate session).
- **Token regeneration** - `POST /api/v1/user/regenerate-token` allows authenticated users to regenerate their `stream_token`, invalidating previous `live_` / `play_` stream IDs.
- **DPAPI vault auth persistence** - `cp_access_jwt` and `cp_refresh_token` stored in DPAPI-encrypted `vault.json`. Tokens are never exposed to dock JS.
- **Temporary operator-assisted completion page** - `telemyapp.com/login/plugin?attempt=...` allows operator approval of plugin login attempts against existing user accounts. Placeholder for future self-service account flow.
- **DB migration `0008_auth_sessions.sql`** - `auth_sessions` table for server-side session tracking (session ID, user ID, refresh token hash, expiry, revocation).
- **DB migration `0009_plugin_login_attempts.sql`** - `plugin_login_attempts` table for browser-handoff flow (attempt ID, short code, status, expiry, bound user ID).

### Added - Dock Auth Support

- **Login action** - Dock sends `auth_login` action to trigger browser-handoff flow in C++.
- **Browser launch** - Native `ShellExecuteW` opens the auth URL in the user's default browser.
- **Login polling** - C++ polls backend on worker thread; dock receives status updates via `receiveDockActionResultJson`.
- **Session refresh** - Automatic token refresh on C++ side; dock notified of auth state changes.
- **Logout action** - `auth_logout` clears vault credentials and calls backend logout endpoint.
- **Entitlement-aware relay gating** - Relay start/stop controls enabled/disabled based on `entitled` flag in telemetry snapshot.

### Changed

- **`POST /api/v1/relay/start`** - Now enforces entitlement server-side. Validates `cp_access_jwt`, checks user entitlement tier, rejects unentitled users before provisioning.


## [0.0.4] - 2026-03-05

### Validated

- **E2E relay telemetry** - Validated live telemetry path: IRL Pro bonded stream -> srtla_rec -> SLS -> C++ PollRelayStats -> dock UI.
- **Live relay stats** - Dock displays live relay stats (bitrate 4.6 Mbps, RTT 53ms, latency 1000ms, loss/drop).
- **Validation-only session** - No code changes were made during this session.

## [0.0.4] - 2026-03-04

### Fixed

- **Auto scene switch toggle** - Fixed issue where bridge was not reading C++ snapshot settings and toggle was routing to the wrong setting key.

### Added - C++ Plugin (`obs-plugin/src/`)

- **Telemetry Snapshot** - Added `relay_public_ip` and `relay_srt_port` to status snapshot.

### Added

- **sync-dock.sh** - script for syncing dock sources between repo and root-level runtime copies.

### Changed

- **Encoders & Uploads section** - now wired to real bridge data (removed simulated outputs).
- **Encoders section visibility** - always visible with empty state when no outputs detected.
- **DEFAULT_SETTINGS array** - cloned before snap.settings overlay to prevent mutation.
- **Dock Source Management** - Dock source files are now tracked in `obs-plugin/dock/`.
- **Relay API Key Extraction** - Improved extraction logic using `apikey` file with a `grep` fallback.

### Removed

- **Dead Components** - Removed `CopyRow`, `RelayUrlsInline`, `RelayUrlCard`, and `ConnectionInfoCard`.

## [0.0.4] - 2026-03-03

### Architecture

- **All-native C++ OBS plugin** - replaced the v0.0.3 Rust bridge + C++ shim + IPC named-pipe architecture with a single `telemy-obs-plugin.dll`. No standalone process, no IPC, no Rust dependency.
- **Bridge JS simplified** - reducer/projection eliminated. C++ produces the full dock state JSON directly; bridge is a thin pass-through.

### Added - C++ Plugin (`obs-plugin/src/`)

- **MetricsCollector** (`metrics_collector.cpp`) - polls OBS C API (`obs_enum_outputs`, `obs_get_active_fps`, general stats, studio mode), Win32 system CPU/memory (`GetSystemTimes`/`GlobalMemoryStatusEx`), and NVIDIA NVML GPU metrics every 500ms. Produces a JSON telemetry snapshot with delta-based network throughput and encoding lag.
- **ConfigVault** (`config_vault.cpp`) - JSON config at `%APPDATA%/Telemy/config.json` with DPAPI-encrypted vault (`CryptProtectData`/`CryptUnprotectData`) at `vault.json`. Round-trip safe via `QJsonDocument`.
- **HttpsClient** (`https_client.cpp`) - WinHTTP RAII wrapper for outbound HTTPS calls. Supports Bearer auth, sync calls on worker threads. Zero external deps beyond Windows SDK.
- **RelayClient** (`relay_client.cpp`) - relay lifecycle management (start/stop/heartbeat). 30s heartbeat interval, 5min server-side TTL. Communicates with AWS Go control plane via HTTPS.
- **DockHost** (`obs_browser_dock_host_scaffold.cpp`) - creates a CEF browser dock panel in OBS, injects the JS bridge, pushes telemetry snapshots into the dock via `ExecuteJavaScript()`. Deferred show pattern (1.5s QTimer) respects OBS DockState layout serialization.
- **PluginEntry** (`obs_plugin_entry.cpp`) - OBS module lifecycle (`obs_module_load`/`obs_module_unload`), 500ms tick callback, action dispatch from dock UI back to native code, scene prefs persistence via `dock_scene_prefs.json`.

### Added - Go Control Plane (`control-plane/`)

- **srtla-receiver relay integration** - EC2 relay instances now auto-install Docker + [OpenIRL srtla-receiver](https://github.com/OpenIRL/srtla-receiver) via user-data bootstrap script.
- **User-data bootstrap** (`scripts/relay-user-data.sh`) - installs Docker + Docker Compose on AL2023, downloads and runs srtla-receiver containers. ~2-3 min boot time.
- **AWS provisioner** (`internal/relay/aws.go`) - user-data script embedded as const, base64-encoded for `RunInstances`. SRT port changed from 9000 to 5000 (SRTLA bonded ingest).
- **Security group** (`telemy-relay-sg`) - UDP 5000 (SRTLA ingest), UDP 4000 (SRT player), UDP 4001 (SRT direct), TCP 3000 (management UI, restricted), TCP 8090 (backend API, restricted).

### Added - Dock UI

- **Encoders & Uploads section** - per-output health bars with rolling-max bitrate self-calibration, encoder grouping, hidden output toggle.
- **Scene linking persistence** - scene-to-rule links survive OBS restarts via native disk storage (`dock_scene_prefs.json`).
- **Synthetic theme replay** - dock picks up OBS color scheme on load even before telemetry pipe is established.

### Changed

- SRT relay port: 9000 -> 5000 (SRTLA bonded ingest via srtla-receiver)
- Bridge JS: reducer/projection removed, now a thin pass-through
- Config format: TOML (v0.0.3) -> JSON (v0.0.4, Qt-native parsing)
- HTTP client: libcurl (via Rust) -> WinHTTP (native, zero external deps)

### Removed

- Rust bridge binary (`obs-telemetry-bridge.exe`)
- IPC named pipes (`telemy_cmd_v1`, `telemy_evt_v1`)
- MsgPack codec in C++ (`ipc_client.cpp`)
- obs-websocket connection (`obws` crate)
- HTTP/WebSocket server (port 7070)
- Web dashboard HTML and settings page
- System tray icon (`tray-item` crate)
- Autostart registry mechanism

### Infrastructure

- Terminated 2 stale bare EC2 relay instances (no SRT software)
- Moved `control-plane/` and `docs/` from `telemy-v0.0.3/` to `telemy-v0.0.4/`
- v0.0.3 archived with tag on GitHub + GitLab
