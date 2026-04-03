# Changelog

All notable changes to the Telemy OBS Plugin will be documented in this file.

## [0.0.5-dev] - 2026-03-20

### Fixed

- **Snapshot push on connect/disconnect** — `connection_connect` and `connection_disconnect` now emit a fresh telemetry snapshot immediately after completion, so the dock reflects updated state without waiting for the next 500ms tick.
- **relay_host_masked field** — `MaskRelayHost()` added; relay connections snapshot now includes a `relay_host_masked` field for safe display in the dock without exposing the full host.
- **not_found error for unknown connection IDs** — `connection_connect` and `connection_disconnect` now return a `not_found` error when the given `connection_id` doesn't exist.

### Added

- **MetricsCollector background thread** — Polling now runs on its own background thread instead of the OBS render thread. Eliminates any render-thread stalls from metric collection.
- **Separate font and density axes** — Font size and UI density are now independent settings.
- **C1 sanitizer** — C++ sanitizer for C1 control characters that break JSON parsing.
- **Density-aware dock spacing** — Layout spacing scales with density setting.
- **Per-link freshness indicators** — Each relay link row shows a freshness indicator based on `last_ms_ago`.
- **Dynamic OBS font scaling** — Dock font size adjusts dynamically based on the font scale setting.
- **Provider abstraction** — InstanceID decoupled from cloud provider. Provider encapsulation for IP orchestration.
- **BYOR (Bring Your Own Relay)** — Users can connect their own relay via ConnectDirect/DisconnectDirect, bypassing managed provisioning.
- **Graceful stats degradation** — Plugin handles missing/unreachable stats endpoints without erroring.

## [0.0.4] - 2026-03-16

### Added

- **Browser-handoff login** — Plugin login via browser handoff with polling for completion.
- **Session management** — Session validation, token refresh, and logout from the plugin.
- **Token regeneration** — Users can regenerate their `stream_token`.
- **DPAPI vault auth persistence** — JWT tokens stored in DPAPI-encrypted vault. Never exposed to dock JS.
- **Dock auth support** — Login/logout actions, browser launch, login polling, session refresh, entitlement-aware relay gating.

### Changed

- Relay start now enforces entitlement server-side.

## [0.0.4] - 2026-03-05

### Validated

- **E2E relay telemetry** — Validated live telemetry path: IRL Pro bonded stream -> srtla_rec -> SLS -> C++ PollRelayStats -> dock UI.
- **Live relay stats** — Dock displays live relay stats (bitrate, RTT, latency, loss/drop).

## [0.0.4] - 2026-03-04

### Fixed

- **Auto scene switch toggle** — Fixed bridge not reading C++ snapshot settings.

### Added

- **Telemetry Snapshot** — Added `relay_public_ip` and `relay_srt_port` to status snapshot.

### Changed

- Encoders & Uploads section wired to real bridge data (removed simulated outputs).
- Dock source files tracked in `obs-plugin/dock/`.

## [0.0.4] - 2026-03-03

### Architecture

- **All-native C++ OBS plugin** — Replaced the v0.0.3 Rust bridge + C++ shim + IPC architecture with a single `telemy-obs-plugin.dll`. No standalone process, no IPC, no Rust dependency.

### Added

- **MetricsCollector** — Polls OBS C API, Win32 system stats, NVIDIA NVML GPU metrics every 500ms.
- **ConfigVault** — JSON config with DPAPI-encrypted vault for sensitive fields.
- **HttpsClient** — WinHTTP RAII wrapper for outbound HTTPS. Zero external deps.
- **RelayClient** — Relay lifecycle management (start/stop/heartbeat).
- **DockHost** — CEF browser dock panel with JS bridge and telemetry injection.
- **PluginEntry** — OBS module lifecycle, tick callback, action dispatch, scene prefs persistence.
- **Encoders & Uploads section** — Per-output health bars with rolling-max bitrate self-calibration.
- **Scene linking persistence** — Scene-to-rule links survive OBS restarts.
- **Synthetic theme replay** — Dock picks up OBS color scheme on load.

### Changed

- SRT relay port: 9000 -> 5000 (SRTLA bonded ingest)
- Bridge JS: reducer/projection removed, now a thin pass-through
- Config format: TOML -> JSON (Qt-native parsing)
- HTTP client: libcurl (via Rust) -> WinHTTP (native)

### Removed

- Rust bridge binary, IPC named pipes, MsgPack codec, obs-websocket, HTTP/WebSocket server, web dashboard, system tray icon, autostart registry
