# Telemy Architecture (v0.0.4)

This document describes the all-native C++ OBS plugin architecture introduced in v0.0.4, replacing the v0.0.3 hybrid Rust bridge + C++ shim + IPC pipe system.

## System Overview

v0.0.4 is a single-DLL OBS plugin (`aegis-obs-plugin.dll`) that runs entirely inside the OBS process. There is no standalone binary, no IPC layer, and no Rust dependency.

```
OBS Process
├── aegis-obs-plugin.dll
│   ├── PluginEntry          — module lifecycle, tick callback, action dispatch
│   ├── MetricsCollector     — OBS C API + Win32 + NVML polling (500ms)
│   ├── ConfigVault          — JSON config + DPAPI encrypted vault
│   ├── HttpsClient          — WinHTTP RAII wrapper
│   ├── RelayClient          — relay lifecycle, heartbeat, emergency shutdown
│   └── DockHost             — CEF browser panel, JS injection
└── data/obs-plugins/aegis-obs-plugin/
    ├── aegis-dock.html
    ├── aegis-dock-app.js     — bundled React dock UI
    ├── aegis-dock-bridge.js
    ├── aegis-dock-bridge-host.js
    └── aegis-dock-browser-host-bootstrap.js
```

## Components

### PluginEntry (`src/obs_plugin_entry.cpp`)

OBS module lifecycle hooks:
- `obs_module_load()` — initializes all components, registers 500ms tick callback, creates dock host
- `obs_module_unload()` — tears down relay (emergency shutdown), cleans up resources
- **Tick callback** — drives MetricsCollector polling, pushes JSON snapshots to dock via CEF
- **Action dispatch** — routes dock UI actions (`switch_scene`, `relay_start`, `relay_stop`, `save_scene_prefs`, `load_scene_prefs`) to native handlers

### MetricsCollector (`src/metrics_collector.cpp`)

Polls three data sources every 500ms:

| Source | API | Metrics |
|--------|-----|---------|
| OBS | `obs_enum_outputs`, `obs_get_active_fps`, `obs_output_get_total_bytes`, etc. | Per-output bitrate, FPS, drop %, encoder, resolution, active state. Global dropped/missed/skipped frames, active FPS, disk space. |
| Win32 | `GetSystemTimes`, `GlobalMemoryStatusEx` | System CPU %, memory usage |
| NVML | `nvmlDeviceGetUtilizationRates`, `nvmlDeviceGetTemperature` | GPU utilization %, temperature (graceful degradation if no NVIDIA GPU) |

Network throughput uses delta-based byte calculation (not session averages). Encoding lag derived from `obs_get_average_frame_render_time`.

Output: a JSON telemetry snapshot containing `health`, OBS stats, system stats, GPU stats, network stats, and per-output data array.

### ConfigVault (`src/config_vault.cpp`)

Two files at `%APPDATA%/Telemy/`:

- **`config.json`** — non-sensitive settings (relay API host, preferences). Read/write via `QJsonDocument`.
- **`vault.json`** — secrets (JWT tokens, API keys). Encrypted with Windows DPAPI (`CryptProtectData`/`CryptUnprotectData`), stored as base64 blobs.

### HttpsClient (`src/https_client.cpp`)

Windows-only WinHTTP wrapper:
- RAII session/connection/request handles
- Synchronous calls on worker threads (no blocking OBS main thread)
- Bearer token auth from ConfigVault
- TLS via Windows certificate store (no bundled CA certs)

### RelayClient (`src/relay_client.cpp`)

Manages the AWS relay instance lifecycle:
- **Start** — POST to control plane, receives session ID + connection credentials (public IP, SRT port, pair token, WebSocket URL)
- **Heartbeat** — 30s interval keep-alive; server-side 5min TTL
- **Stop** — explicit teardown request
- **Emergency shutdown** — on `obs_module_unload`, ensures relay is torn down even on OBS crash/force-quit

### DockHost (`src/obs_browser_dock_host_scaffold.cpp`)

CEF browser dock panel:
- Reads all HTML/JS assets at startup, inlines into a `data:text/html` URL for CEF
- Injects `aegis-dock-bridge.js` chain into the page
- Pushes telemetry snapshots via `ExecuteJavaScript()` on each tick
- **Deferred show pattern** — 1.5s QTimer before deciding to float, respects OBS DockState layout serialization
- **Theme injection** — Qt palette CSS overrides injected into dock HTML before CEF loads

## Data Flow

### Telemetry (Downstream)

```
OBS C API ──┐
Win32 APIs ─┤──> MetricsCollector (500ms) ──> JSON snapshot ──> CEF ExecuteJavaScript()
NVML ───────┘                                       │                    │
                                                     │                    ▼
                                                     │              Dock UI (React)
                                                     ▼
                                              RelayClient ──> HTTPS ──> AWS Go Control Plane
```

### UI Actions (Upstream)

```
Dock UI ──> sendDockAction() ──> document.title transport ──> CEF titleChanged
    ──> PluginEntry action dispatch ──> native handler (OBS API / RelayClient / ConfigVault)
    ──> receiveDockActionResultJson() ──> Dock UI callback
```

The `document.title` transport encodes JSON actions as percent-encoded strings prefixed with `__AEGIS_DOCK_ACTION__:`. CEF intercepts `titleChanged`, decodes, and routes to the plugin.

### Scene Switch Lifecycle

```
switch_scene action ──> OBS API (obs_frontend_set_current_scene)
    ──> OBS scene transition ──> completion callback
    ──> receiveSceneSwitchCompletedJson() ──> Dock UI
```

### Scene Preferences Persistence

- **Save**: Dock → `save_scene_prefs` action → PluginEntry → writes `dock_scene_prefs.json` via `obs_module_config_path()`
- **Load**: Dock → `load_scene_prefs` action → PluginEntry → reads from disk → `receiveDockActionResultJson()`
- Survives OBS restarts. `bfree()` required after `obs_module_config_path()` (OBS allocator).

## Bridge Contract (`window.aegisDockNative`)

### Methods (Dock → Native)
- `getState()` — returns current DockState (nested: `header`, `live`, `scenes`, `connections`, `bitrate`, `relay`, `failover`, `settings`, `events`, `pipe`, `meta`)
- `sendDockAction(action)` — sends action object
- `sendDockActionJson(json)` — JSON string variant

### Callbacks (Native → Dock)
- `receiveSceneSnapshotJson(json)` — OBS scene inventory
- `receiveDockActionResultJson(json)` — action queued/rejected/completed/failed
- `receiveSceneSwitchCompletedJson(json)` — authoritative scene switch result

### Action Types
- `switch_scene` — native OBS scene switch
- `request_status` — request fresh telemetry snapshot
- `relay_start` / `relay_stop` — relay lifecycle
- `save_scene_prefs` / `load_scene_prefs` — native disk persistence

## Relay Stack (AWS)

EC2 relay instances run [OpenIRL srtla-receiver](https://github.com/OpenIRL/srtla-receiver) via Docker Compose:

| Port | Protocol | Purpose |
|------|----------|---------|
| 5000 | UDP | SRTLA bonded ingest (encoder connects here) |
| 4000 | UDP | SRT player output (OBS pulls from here) |
| 4001 | UDP | SRT direct ingest (non-bonded fallback) |
| 3000 | TCP | Management UI (restricted to control plane IP) |
| 8090 | TCP | Backend API (restricted to control plane IP) |

Provisioned via Go control plane (`aegis-control-plane/`):
- `internal/relay/aws.go` — EC2 RunInstances with user-data bootstrap
- `scripts/relay-user-data.sh` — Docker + srtla-receiver install (~2-3 min boot)
- Security group `aegis-relay-sg` — public UDP ports + restricted TCP management

## Build System

CMake 3.20+, C++17:

```bash
cmake -B build \
  -DAEGIS_BUILD_OBS_PLUGIN=ON \
  -DAEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=ON \
  -DAEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON \
  -DOBS_INCLUDE_DIRS="<libobs>;<frontend-api>" \
  -DOBS_LIBRARIES="obs;obs-frontend-api" \
  -DOBS_LIBRARY_DIRS="<import-libs>" \
  -DOBS_BROWSER_PANEL_DIR="<browser-panel-headers>" \
  -DCMAKE_PREFIX_PATH="<qt6>"

cmake --build build --config Release
```

Dependencies: OBS Studio SDK headers, Qt6 (Core + Widgets), Windows SDK (WinHTTP, DPAPI, NVML optional).

Dock JS assets are staged automatically by CMake post-build from the repo root.

## Deployment

1. Copy `aegis-obs-plugin.dll` to `<OBS>/obs-plugins/64bit/`
2. Copy dock assets to `<OBS>/data/obs-plugins/aegis-obs-plugin/`
3. Restart OBS

OBS reads dock assets from staged location. C++ inlines all JS/HTML at startup into a `data:text/html` URL — files are not loaded by CEF from disk at runtime.

## Regression Guards

### Bridge-Data Synchronization

Any `useEffect` syncing bridge-derived data must guard on `useBridge` to prevent SIM fallback contamination:

```javascript
useEffect(() => {
  if (!useBridge) return;
  setCachedData(bridgeData);
}, [bridgeData, useBridge]);
```

### OBS Allocator

Always call `bfree()` after `obs_module_config_path()` — OBS uses its own allocator.

## Platform Constraints

- **Windows-only**: Win32 DPAPI, WinHTTP, `GetSystemTimes`/`GlobalMemoryStatusEx`
- **NVIDIA GPU optional**: NVML loaded dynamically, graceful degradation
- **OBS Studio required**: Direct C API integration, CEF dock hosting
- **Qt6 required**: Bundled with OBS, used for dock widget + JSON parsing
