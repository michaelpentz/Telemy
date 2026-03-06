# Changelog

All notable changes to telemy-v0.0.4 will be documented in this file.

## [0.0.4] — 2026-03-05

### Validated

- **E2E relay telemetry** — Validated live telemetry path: IRL Pro bonded stream → srtla_rec → SLS → C++ PollRelayStats → dock UI.
- **Live relay stats** — Dock displays live relay stats (bitrate 4.6 Mbps, RTT 53ms, latency 1000ms, loss/drop).
- **Validation-only session** — No code changes were made during this session.

## [0.0.4] — 2026-03-04

### Fixed

- **Auto scene switch toggle** — Fixed issue where bridge was not reading C++ snapshot settings and toggle was routing to the wrong setting key.

### Added — C++ Plugin (`obs-plugin/src/`)

- **Telemetry Snapshot** — Added `relay_public_ip` and `relay_srt_port` to status snapshot.

### Added

- **sync-dock.sh** — script for syncing dock sources between repo and root-level runtime copies.

### Changed

- **Encoders & Uploads section** — now wired to real bridge data (removed simulated outputs).
- **Encoders section visibility** — always visible with empty state when no outputs detected.
- **DEFAULT_SETTINGS array** — cloned before snap.settings overlay to prevent mutation.
- **Dock Source Management** — Dock source files are now tracked in `obs-plugin/dock/`.
- **Relay API Key Extraction** — Improved extraction logic using `apikey` file with a `grep` fallback.

### Removed

- **Dead Components** — Removed `CopyRow`, `RelayUrlsInline`, `RelayUrlCard`, and `ConnectionInfoCard`.

## [0.0.4] — 2026-03-03

### Architecture

- **All-native C++ OBS plugin** — replaced the v0.0.3 Rust bridge + C++ shim + IPC named-pipe architecture with a single `aegis-obs-plugin.dll`. No standalone process, no IPC, no Rust dependency.
- **Bridge JS simplified** — reducer/projection eliminated. C++ produces the full dock state JSON directly; bridge is a thin pass-through.

### Added — C++ Plugin (`obs-plugin/src/`)

- **MetricsCollector** (`metrics_collector.cpp`) — polls OBS C API (`obs_enum_outputs`, `obs_get_active_fps`, general stats, studio mode), Win32 system CPU/memory (`GetSystemTimes`/`GlobalMemoryStatusEx`), and NVIDIA NVML GPU metrics every 500ms. Produces a JSON telemetry snapshot with delta-based network throughput and encoding lag.
- **ConfigVault** (`config_vault.cpp`) — JSON config at `%APPDATA%/Telemy/config.json` with DPAPI-encrypted vault (`CryptProtectData`/`CryptUnprotectData`) at `vault.json`. Round-trip safe via `QJsonDocument`.
- **HttpsClient** (`https_client.cpp`) — WinHTTP RAII wrapper for outbound HTTPS calls. Supports Bearer auth, sync calls on worker threads. Zero external deps beyond Windows SDK.
- **RelayClient** (`relay_client.cpp`) — relay lifecycle management (start/stop/heartbeat). 30s heartbeat interval, 5min server-side TTL. Communicates with AWS Go control plane via HTTPS.
- **DockHost** (`obs_browser_dock_host_scaffold.cpp`) — creates a CEF browser dock panel in OBS, injects the JS bridge, pushes telemetry snapshots into the dock via `ExecuteJavaScript()`. Deferred show pattern (1.5s QTimer) respects OBS DockState layout serialization.
- **PluginEntry** (`obs_plugin_entry.cpp`) — OBS module lifecycle (`obs_module_load`/`obs_module_unload`), 500ms tick callback, action dispatch from dock UI back to native code, scene prefs persistence via `dock_scene_prefs.json`.

### Added — Go Control Plane (`aegis-control-plane/`)

- **srtla-receiver relay integration** — EC2 relay instances now auto-install Docker + [OpenIRL srtla-receiver](https://github.com/OpenIRL/srtla-receiver) via user-data bootstrap script.
- **User-data bootstrap** (`scripts/relay-user-data.sh`) — installs Docker + Docker Compose on AL2023, downloads and runs srtla-receiver containers. ~2-3 min boot time.
- **AWS provisioner** (`internal/relay/aws.go`) — user-data script embedded as const, base64-encoded for `RunInstances`. SRT port changed from 9000 to 5000 (SRTLA bonded ingest).
- **Security group** (`aegis-relay-sg`) — UDP 5000 (SRTLA ingest), UDP 4000 (SRT player), UDP 4001 (SRT direct), TCP 3000 (management UI, restricted), TCP 8090 (backend API, restricted).

### Added — Dock UI

- **Encoders & Uploads section** — per-output health bars with rolling-max bitrate self-calibration, encoder grouping, hidden output toggle.
- **Scene linking persistence** — scene-to-rule links survive OBS restarts via native disk storage (`dock_scene_prefs.json`).
- **Synthetic theme replay** — dock picks up OBS color scheme on load even before telemetry pipe is established.

### Changed

- SRT relay port: 9000 → 5000 (SRTLA bonded ingest via srtla-receiver)
- Bridge JS: reducer/projection removed, now a thin pass-through
- Config format: TOML (v0.0.3) → JSON (v0.0.4, Qt-native parsing)
- HTTP client: libcurl (via Rust) → WinHTTP (native, zero external deps)

### Removed

- Rust bridge binary (`obs-telemetry-bridge.exe`)
- IPC named pipes (`aegis_cmd_v1`, `aegis_evt_v1`)
- MsgPack codec in C++ (`ipc_client.cpp`)
- obs-websocket connection (`obws` crate)
- HTTP/WebSocket server (port 7070)
- Web dashboard HTML and settings page
- System tray icon (`tray-item` crate)
- Autostart registry mechanism

### Infrastructure

- Terminated 2 stale bare EC2 relay instances (no SRT software)
- Moved `aegis-control-plane/` and `docs/` from `telemy-v0.0.3/` to `telemy-v0.0.4/`
- v0.0.3 archived with tag on GitHub + GitLab
