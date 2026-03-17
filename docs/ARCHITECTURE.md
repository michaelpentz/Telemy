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
- `obs_module_unload()` — joins relay worker threads, tears down relay (emergency shutdown), cleans up resources
- **Tick callback** — drives MetricsCollector polling at configurable `metrics_poll_interval_ms` (default 500ms, clamped 100-5000ms), pushes JSON snapshots to dock via CEF
- **Action dispatch** — routes dock UI actions (`switch_scene`, `relay_start`, `relay_stop`, `save_scene_prefs`, `load_scene_prefs`) to native handlers
- **Secret isolation** — `vault_get` and `vault_keys` actions are rejected from the dock; `load_config` returns `relay_shared_key_present` (boolean) instead of the actual key

### MetricsCollector (`src/metrics_collector.cpp`)

Polls three data sources every 500ms:

| Source | API | Metrics |
|--------|-----|---------|
| OBS | `obs_enum_outputs`, `obs_get_active_fps`, `obs_output_get_total_bytes`, etc. | Per-output bitrate, FPS, drop %, encoder, resolution, active state. Global dropped/missed/skipped frames, active FPS, disk space. |
| Win32 | `GetSystemTimes`, `GlobalMemoryStatusEx` | System CPU %, memory usage |
| NVML | `nvmlDeviceGetUtilizationRates`, `nvmlDeviceGetTemperature` | GPU utilization %, temperature (graceful degradation if no NVIDIA GPU) |

Network throughput uses delta-based byte calculation (not session averages). Encoding lag derived from `obs_get_average_frame_render_time`.

Output: a JSON telemetry snapshot containing `health`, OBS stats, system stats, GPU stats, network stats, and per-output data array.

**Security Note**: The telemetry snapshot delivered to the JS dock is stripped of all sensitive fields (`pair_token`, `ws_url`, `relay_shared_key`). The dock cannot read secrets — `vault_get` and `vault_keys` actions are rejected, and `load_config` returns only a boolean for `relay_shared_key_present`. The dock layer operates in a restricted trust zone and only receives operational metrics and non-sensitive status.

### ConfigVault (`src/config_vault.cpp`)

Two files at `%APPDATA%/Telemy/`:

- **`config.json`** — non-sensitive settings (relay API host, preferences). Read/write via `QJsonDocument`.
- **`vault.json`** — secrets (JWT tokens, API keys). Must be a **flat JSON format** (not nested). Encrypted with Windows DPAPI (`CryptProtectData`/`CryptUnprotectData`), stored as base64 blobs.

Product auth model:
- The recommended relay-access UX is account login in the plugin, not manual transport secrets.
- The plugin stores backend-issued control-plane auth in `vault.json`.
- Relay entitlement is enforced by the backend on `relay/start`.
- Publish/play access on the relay uses per-user stream IDs derived from `stream_token`.

See also: `AUTH_ENTITLEMENT_MODEL.md`.

### HttpsClient (`src/https_client.cpp`)

Windows-only WinHTTP wrapper:
- RAII session/connection/request handles
- **HTTP/HTTPS support**: Uses `parse_host_port()` with scheme-driven TLS (`https://` or no prefix = TLS, `http://` = plaintext). Port number does not influence TLS decision.
- Synchronous calls on worker threads (no blocking OBS main thread)
- Bearer token auth from ConfigVault
- TLS via Windows certificate store (no bundled CA certs)

### RelayClient (`src/relay_client.cpp`)

Manages the AWS relay instance lifecycle:
- **Start** — POST to control plane, receives session ID + connection details (public IP, SRT port, instance_id). Runs on a joinable worker thread (mutex-protected, old thread joined before new one starts).
- **Heartbeat** — 30s interval keep-alive with `instance_id` binding; server-side 5min TTL
- **Stop** — explicit teardown request. Runs on a separate joinable worker thread.
- **Emergency shutdown** — on `obs_module_unload`, joins both worker threads, then tears down relay

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

### Relay Telemetry Data Flow

When a relay session is active, the plugin polls the relay's stats API for aggregate stream health:

1. **Polling** — `PollRelayStats()` in `relay_client.cpp` executes every 2 seconds via a `WinHTTP` GET request to `:8090/stats/play_{stream_token}?legacy=1`.
   This path is derived from the authenticated user's `stream_token`.
2. **Parsing** — The SLS legacy JSON format is parsed, specifically targeting the nested `publishers` object for the active stream.
3. **Snapshot Injection** — Nine relay-prefixed fields (bitrate, RTT, loss, latency, drop, etc.) are injected into the top-level telemetry snapshot by `BuildStatusSnapshotJson`.
4. **Bridge Delivery** — The JS bridge passes the snapshot to `getState().relay` in the Dock state.
5. **UI Rendering** — The Dock UI renders the data as a dedicated "Relay Ingest" card with a bitrate bar and stat pills for RTT, Loss, and Latency.

**Network Requirement**: Port 8090 on the relay instance must be accessible from the OBS machine (Security Group rule required).

### Per-Link Relay Telemetry

v0.0.4 introduces deep visibility into the individual cellular/WiFi links contributing to a bonded stream:

1. **Custom `srtla_rec` Fork** — The relay uses a custom fork (`michaelpentz/srtla`, forked from `OpenIRL/srtla`) which adds atomic per-connection byte and packet counters (`std::atomic<uint64_t>`, relaxed ordering) to the core SRTLA proxy logic.
2. **HTTP Stats Server** — The fork includes a lightweight HTTP server listening on `--stats_port` (relay uses port 5080).
3. **Stats Schema** — `GET /stats` returns a JSON object containing a `groups[]` array. Each group contains a `connections[]` list with:
   - `addr`: Remote IP of the link.
   - `bytes` / `pkts`: Total throughput counters.
   - `share_pct`: Real-time percentage of total traffic carried by this link.
   - `last_ms_ago`: Milliseconds since the last packet was received on this link (stale detection).
   - `uptime_s`: Duration the link has been active.
4. **Relay Stack Integration** — The `ghcr.io/michaelpentz/srtla-receiver:latest` Docker image (forked from `OpenIRL/srtla-receiver`) runs this modified binary, with `supervisord` passing `--stats_port=5080`.
5. **Plugin Polling** — `PollPerLinkStats()` in the C++ plugin polls the relay's port 5080 every ~2s.
6. **UI Visualization** — The Dock UI renders a dynamic `BitrateBar` for each link, showing its relative share %. Links with `last_ms_ago > 3000` are rendered with reduced opacity to indicate staleness.
7. **ASN-Based Carrier Identification** — The custom `srtla_rec` fork optionally links against `libmaxminddb` to resolve each connection's source IP to its ASN organization name (e.g., "T-Mobile USA, Inc.", "AT&T Mobility"). The stats endpoint includes an `asn_org` field per connection when a GeoLite2-ASN database is available. The Dock UI maps ASN org names to short carrier labels (T-Mobile, AT&T, Verizon, etc.) for display. When ASN data is unavailable, links are labeled "Link 1", "Link 2", etc. (no IP leak, safe for on-stream display). With ASN data, mobile carriers get cellular type labels and ISPs get ethernet type labels (covers USB-C-to-Ethernet setups).

**Data Flow Summary**: `srtla_rec` counters -> HTTP `/stats` -> C++ `PollPerLinkStats` -> JSON snapshot -> CEF JS -> Dock BitrateBar.

### Relay Provision Progress

v0.0.4 provides real-time provisioning feedback instead of a static "Provisioning relay..." message:

1. **Async Pipeline** — `handleRelayStart` in the Go control plane returns immediately with `status: "provisioning"`, then launches `runProvisionPipeline()` as a background goroutine.
2. **Step Tracking** — The pipeline updates a `provision_step` column in the sessions table as it progresses through six steps: `launching_instance` → `waiting_for_instance` → `starting_docker` → `starting_containers` → `creating_stream` → `ready`.
3. **Step Dwell** — Each step has a minimum 3-second dwell time so clients polling at 2s intervals see each step.
4. **Plugin Polling** — After `relay_start`, the C++ plugin polls `GET /relay/active` every 2s, reads `provision_step` from the response, and emits `relay_provision_progress` events to the dock.
5. **Dock UI** — `RelayProvisionProgress` component shows the current step label, step counter (N/6), animated progress bar, and blinking dots. Transitions smoothly from "Ready 6/6" to the active relay display.

**Data Flow**: Go pipeline → DB `provision_step` → API response → C++ polling → dock event → React progress bar.

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

EC2 relay instances run a dual-process stack via Docker Compose for bonded SRT:

| Component | Port | Protocol | Purpose |
|-----------|------|----------|---------|
| `srtla_rec` | 5000 | UDP | **SRTLA proxy**: Raw UDP proxy (no libsrt). Groups bonded packets from multiple IPs (e.g. WiFi + Cellular) and forwards verbatim to `localhost:4001`. |
| `SLS` (v1.5.0) | 4001 | UDP | **SRT Live Server**: Handles actual SRT sessions. Ingests from `srtla_rec`. |
| `SLS` (v1.5.0) | 4000 | UDP | **SRT Player**: OBS pulls the bonded stream from here. |
| Management | 3000 | TCP | SLS Web UI (restricted). |
| Backend API | 8090 | TCP | srtla-receiver control API (restricted). |

### Connection Schemes
- **Encoder (IRL Pro)**: `srtla://{relay_host}:5000` with `streamid=live_{stream_token}`.
- **Player (OBS)**: `srt://{relay_host}:4000?streamid=play_{stream_token}`.

Notes:
- `relay_host` may be a relay IP or the user's relay hostname when available.
- `stream_token` is permanent per user and is the sender-facing security boundary for SLS publish/play.
- This is intentionally separate from plugin-to-backend authentication.

The stack handles packet reordering and bonding overhead, resulting in ~5s E2E latency with high reliability over unstable cellular links.

Provisioned via Go control plane (`aegis-control-plane/`):
- `internal/relay/aws.go` — EC2 RunInstances with user-data bootstrap, Elastic IP management
- `scripts/relay-user-data.sh` — Docker + srtla-receiver install (~2-3 min boot)
- Security group `aegis-relay-sg` — public UDP ports + restricted TCP management

### Elastic IP (Stable Relay Addresses)

Each user gets one AWS Elastic IP per region, allocated on first provision and reused for all subsequent provisions. This ensures mobile clients (IRL Pro) never lose connectivity due to stale DNS caches.

- **Allocation**: First provision allocates an EIP via `ec2:AllocateAddress`, stored in `users.eip_allocation_id` and `users.eip_public_ip`.
- **Association**: Each provision calls `ec2:AssociateAddress` to bind the EIP to the new instance.
- **DNS**: The relay DNS record (`<slug>.relay.telemyapp.com`) is permanent — created once, never deleted on deprovision. Instance termination auto-disassociates the EIP (AWS handles this).
- **Fallback**: If EIP allocation fails (quota, permissions), the pipeline falls back to auto-assigned IP + DNS update.
- **Cost**: Free while attached to a running instance; ~$3.60/mo per user when idle.

## Dock Source Management

- **Source of truth**: `telemy-v0.0.4/obs-plugin/dock/`
- **OBS reads from**: `obs-plugin/dock/` via `AEGIS_DOCK_BRIDGE_ROOT` env var
- **Build command**:
  ```bash
  cd obs-plugin/dock && NODE_PATH=../../../dock-preview/node_modules npx esbuild aegis-dock-entry.jsx --bundle --format=iife --jsx=automatic --outfile=aegis-dock-app.js --target=es2020 --minify
  ```

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
