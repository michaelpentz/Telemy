# Telemy Architecture (v0.0.5)

This document describes the all-native C++ OBS plugin architecture introduced in v0.0.4, replacing the v0.0.3 hybrid Rust bridge + C++ shim + IPC pipe system.

## System Overview

v0.0.5 is a single-DLL OBS plugin (`aegis-obs-plugin.dll`) that runs entirely inside the OBS process. There is no standalone binary, no IPC layer, and no Rust dependency.

```
OBS Process
â”œâ”€â”€ aegis-obs-plugin.dll
â”‚   â”œâ”€â”€ PluginEntry          â€” module lifecycle, tick callback, action dispatch
â”‚   â”œâ”€â”€ MetricsCollector     â€” OBS C API + Win32 + NVML polling (500ms)
â”‚   â”œâ”€â”€ ConfigVault          â€” JSON config + DPAPI encrypted vault
â”‚   â”œâ”€â”€ HttpsClient          â€” WinHTTP RAII wrapper
â”‚   â”œâ”€â”€ RelayClient          â€” relay lifecycle, heartbeat, emergency shutdown
â”‚   â””â”€â”€ DockHost             â€” CEF browser panel, JS injection
â””â”€â”€ data/obs-plugins/aegis-obs-plugin/
    â”œâ”€â”€ aegis-dock.html
    â”œâ”€â”€ aegis-dock-app.js     â€” bundled React dock UI
    â”œâ”€â”€ aegis-dock-bridge.js
    â”œâ”€â”€ aegis-dock-bridge-host.js
    â””â”€â”€ aegis-dock-browser-host-bootstrap.js
```

## Components

### PluginEntry (`src/obs_plugin_entry.cpp`)

OBS module lifecycle hooks:
- `obs_module_load()` â€” initializes all components, registers 500ms tick callback, creates dock host
- `obs_module_unload()` â€” joins relay worker threads, tears down relay (emergency shutdown), cleans up resources
- **Tick callback** â€” drives MetricsCollector polling at configurable `metrics_poll_interval_ms` (default 500ms, clamped 100-5000ms), pushes JSON snapshots to dock via CEF
- **Action dispatch** â€” routes dock UI actions (`switch_scene`, `relay_start`, `relay_stop`, `save_scene_prefs`, `load_scene_prefs`) to native handlers
- **Secret isolation** â€” `vault_get` and `vault_keys` actions are rejected from the dock; `load_config` returns `relay_shared_key_present` (boolean) instead of the actual key

### MetricsCollector (`src/metrics_collector.cpp`)

Polls three data sources every 500ms:

| Source | API | Metrics |
|--------|-----|---------|
| OBS | `obs_enum_outputs`, `obs_get_active_fps`, `obs_output_get_total_bytes`, etc. | Per-output bitrate, FPS, drop %, encoder, resolution, active state. Global dropped/missed/skipped frames, active FPS, disk space. |
| Win32 | `GetSystemTimes`, `GlobalMemoryStatusEx` | System CPU %, memory usage |
| NVML | `nvmlDeviceGetUtilizationRates`, `nvmlDeviceGetTemperature` | GPU utilization %, temperature (graceful degradation if no NVIDIA GPU) |

Network throughput uses delta-based byte calculation (not session averages). Encoding lag derived from `obs_get_average_frame_render_time`.

Output: a JSON telemetry snapshot containing `health`, OBS stats, system stats, GPU stats, network stats, and per-output data array.

**Security Note**: The telemetry snapshot delivered to the JS dock is stripped of all sensitive fields (`pair_token`, `ws_url`, `relay_shared_key`). The dock cannot read secrets â€” `vault_get` and `vault_keys` actions are rejected, and `load_config` returns only a boolean for `relay_shared_key_present`. The dock layer operates in a restricted trust zone and only receives operational metrics and non-sensitive status.

### ConfigVault (`src/config_vault.cpp`)

Two files at `%APPDATA%/Telemy/`:

- **`config.json`** â€” non-sensitive settings (relay API host, preferences). Read/write via `QJsonDocument`.
- **`vault.json`** â€” secrets (JWT tokens, API keys). Must be a **flat JSON format** (not nested). Encrypted with Windows DPAPI (`CryptProtectData`/`CryptUnprotectData`), stored as base64 blobs.

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
- **Start** â€” POST to control plane, receives session ID + connection details (public IP, SRT port, instance_id). Runs on a joinable worker thread (mutex-protected, old thread joined before new one starts).
- **Heartbeat** â€” 30s interval keep-alive with `instance_id` binding; server-side 5min TTL
- **Stop** â€” explicit teardown request. Runs on a separate joinable worker thread.
- **Emergency shutdown** â€” on `obs_module_unload`, joins both worker threads, then tears down relay

### DockHost (`src/obs_browser_dock_host_scaffold.cpp`)

CEF browser dock panel:
- Reads all HTML/JS assets at startup, inlines into a `data:text/html` URL for CEF
- Injects `aegis-dock-bridge.js` chain into the page
- Pushes telemetry snapshots via `ExecuteJavaScript()` on each tick
- **Deferred show pattern** â€” 1.5s QTimer before deciding to float, respects OBS DockState layout serialization
- **Theme injection** â€” Qt palette CSS overrides injected into dock HTML before CEF loads

## Data Flow

### Telemetry (Downstream)

```
OBS C API â”€â”€â”
Win32 APIs â”€â”¤â”€â”€> MetricsCollector (500ms) â”€â”€> JSON snapshot â”€â”€> CEF ExecuteJavaScript()
NVML â”€â”€â”€â”€â”€â”€â”€â”˜                                       â”‚                    â”‚
                                                     â”‚                    â–¼
                                                     â”‚              Dock UI (React)
                                                     â–¼
                                              RelayClient â”€â”€> HTTPS â”€â”€> AWS Go Control Plane
```

### Relay Telemetry Data Flow

When a relay session is active, the plugin polls the relay's stats API for aggregate stream health:

1. **Polling** â€” `PollRelayStats()` in `relay_client.cpp` executes every 2 seconds via a `WinHTTP` GET request to `:8090/stats/play_{stream_token}?legacy=1`.
   This path is derived from the authenticated user's `stream_token`.
2. **Parsing** â€” The SLS legacy JSON format is parsed, specifically targeting the nested `publishers` object for the active stream.
3. **Snapshot Injection** â€” Nine relay-prefixed fields (bitrate, RTT, loss, latency, drop, etc.) are injected into the top-level telemetry snapshot by `BuildStatusSnapshotJson`.
4. **Bridge Delivery** â€” The JS bridge passes the snapshot to `getState().relay` in the Dock state.
5. **UI Rendering** â€” The Dock UI renders the data as a dedicated "Relay Ingest" card with a bitrate bar and stat pills for RTT, Loss, and Latency.

**Network Requirement**: Port 8090 on the relay instance must be accessible from the OBS machine (Security Group rule required).

### Per-Link Relay Telemetry

v0.0.5 introduces deep visibility into the individual cellular/WiFi links contributing to a bonded stream:

1. **Custom `srtla_rec` Fork** â€” The relay uses a custom fork (`michaelpentz/srtla`, forked from `OpenIRL/srtla`) which adds atomic per-connection byte and packet counters (`std::atomic<uint64_t>`, relaxed ordering) to the core SRTLA proxy logic.
2. **HTTP Stats Server** â€” The fork includes a lightweight HTTP server listening on `--stats_port` (relay uses port 5080).
3. **Stats Schema** â€” `GET /stats` returns a JSON object containing a `groups[]` array. Each group contains a `connections[]` list with:
   - `addr`: Remote IP of the link.
   - `bytes` / `pkts`: Total throughput counters.
   - `share_pct`: Real-time percentage of total traffic carried by this link.
   - `last_ms_ago`: Milliseconds since the last packet was received on this link (stale detection).
   - `uptime_s`: Duration the link has been active.
4. **Relay Stack Integration** â€” The `ghcr.io/michaelpentz/srtla-receiver:latest` Docker image (forked from `OpenIRL/srtla-receiver`) runs this modified binary, with `supervisord` passing `--stats_port=5080`.
5. **Plugin Polling** â€” `PollPerLinkStats()` in the C++ plugin polls the relay's port 5080 every ~2s.
6. **UI Visualization** â€” The Dock UI renders a dynamic `BitrateBar` for each link, showing its relative share %. Links with `last_ms_ago > 3000` are rendered with reduced opacity to indicate staleness.
7. **ASN-Based Carrier Identification** â€” The custom `srtla_rec` fork optionally links against `libmaxminddb` to resolve each connection's source IP to its ASN organization name (e.g., "T-Mobile USA, Inc.", "AT&T Mobility"). The stats endpoint includes an `asn_org` field per connection when a GeoLite2-ASN database is available. The Dock UI maps ASN org names to short carrier labels (T-Mobile, AT&T, Verizon, etc.) for display. When ASN data is unavailable, links are labeled "Link 1", "Link 2", etc. (no IP leak, safe for on-stream display). With ASN data, mobile carriers get cellular type labels and ISPs get ethernet type labels (covers USB-C-to-Ethernet setups).

**Data Flow Summary**: `srtla_rec` counters -> HTTP `/stats` -> C++ `PollPerLinkStats` -> JSON snapshot -> CEF JS -> Dock BitrateBar.

### Relay Provision Progress

v0.0.5 provides real-time provisioning feedback instead of a static "Provisioning relay..." message:

1. **Async Pipeline** â€” `handleRelayStart` in the Go control plane returns immediately with `status: "provisioning"`, then launches `runProvisionPipeline()` as a background goroutine.
2. **Step Tracking** â€” The pipeline updates a `provision_step` column in the sessions table as it progresses through six steps: `launching_instance` â†’ `waiting_for_instance` â†’ `starting_docker` â†’ `starting_containers` â†’ `creating_stream` â†’ `ready`.
3. **Step Dwell** â€” Each step has a minimum 3-second dwell time so clients polling at 2s intervals see each step.
4. **Plugin Polling** â€” After `relay_start`, the C++ plugin polls `GET /relay/active` every 2s, reads `provision_step` from the response, and emits `relay_provision_progress` events to the dock.
5. **Dock UI** â€” `RelayProvisionProgress` component shows the current step label, step counter (N/6), animated progress bar, and blinking dots. Transitions smoothly from "Ready 6/6" to the active relay display.

**Data Flow**: Go pipeline â†’ DB `provision_step` â†’ API response â†’ C++ polling â†’ dock event â†’ React progress bar.

### UI Actions (Upstream)

```
Dock UI â”€â”€> sendDockAction() â”€â”€> document.title transport â”€â”€> CEF titleChanged
    â”€â”€> PluginEntry action dispatch â”€â”€> native handler (OBS API / RelayClient / ConfigVault)
    â”€â”€> receiveDockActionResultJson() â”€â”€> Dock UI callback
```

The `document.title` transport encodes JSON actions as percent-encoded strings prefixed with `__AEGIS_DOCK_ACTION__:`. CEF intercepts `titleChanged`, decodes, and routes to the plugin.

### Scene Switch Lifecycle

```
switch_scene action â”€â”€> OBS API (obs_frontend_set_current_scene)
    â”€â”€> OBS scene transition â”€â”€> completion callback
    â”€â”€> receiveSceneSwitchCompletedJson() â”€â”€> Dock UI
```

### Scene Preferences Persistence

- **Save**: Dock â†’ `save_scene_prefs` action â†’ PluginEntry â†’ writes `dock_scene_prefs.json` via `obs_module_config_path()`
- **Load**: Dock â†’ `load_scene_prefs` action â†’ PluginEntry â†’ reads from disk â†’ `receiveDockActionResultJson()`
- Survives OBS restarts. `bfree()` required after `obs_module_config_path()` (OBS allocator).

## Bridge Contract (`window.aegisDockNative`)

### Methods (Dock â†’ Native)
- `getState()` â€” returns current DockState (nested: `header`, `live`, `scenes`, `connections`, `bitrate`, `relay`, `failover`, `settings`, `events`, `pipe`, `meta`)
- `sendDockAction(action)` â€” sends action object
- `sendDockActionJson(json)` â€” JSON string variant

### Callbacks (Native â†’ Dock)
- `receiveSceneSnapshotJson(json)` â€” OBS scene inventory
- `receiveDockActionResultJson(json)` â€” action queued/rejected/completed/failed
- `receiveSceneSwitchCompletedJson(json)` â€” authoritative scene switch result

### Action Types
- `switch_scene` â€” native OBS scene switch
- `request_status` â€” request fresh telemetry snapshot
- `relay_start` / `relay_stop` â€” relay lifecycle
- `save_scene_prefs` / `load_scene_prefs` â€” native disk persistence

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
- `internal/relay/aws.go` â€” EC2 RunInstances with user-data bootstrap, Elastic IP management
- `scripts/relay-user-data.sh` â€” Docker + srtla-receiver install (~2-3 min boot)
- Security group `aegis-relay-sg` â€” public UDP ports + restricted TCP management

### Elastic IP (Stable Relay Addresses)

Each user gets one AWS Elastic IP per region, allocated on first provision and reused for all subsequent provisions. This ensures mobile clients (IRL Pro) never lose connectivity due to stale DNS caches.

- **Allocation**: First provision allocates an EIP via `ec2:AllocateAddress`, stored in `users.eip_allocation_id` and `users.eip_public_ip`.
- **Association**: Each provision calls `ec2:AssociateAddress` to bind the EIP to the new instance.
- **DNS**: The relay DNS record (`<slug>.relay.telemyapp.com`) is permanent â€” created once, never deleted on deprovision. Instance termination auto-disassociates the EIP (AWS handles this).
- **Fallback**: If EIP allocation fails (quota, permissions), the pipeline falls back to auto-assigned IP + DNS update.
- **Cost**: Free while attached to a running instance; ~$3.60/mo per user when idle.


## Authentication & Session Management

v0.0.5 introduces a browser-handoff plugin login flow, session management, and server-side entitlement enforcement for relay activation. The full auth model is documented in `AUTH_ENTITLEMENT_MODEL.md`; this section covers the architectural components.

### Plugin Login Flow (Browser Handoff)

The plugin authenticates users via a browser-handoff pattern — the plugin initiates login, opens the user's browser, and polls until the web tier completes the flow:

1. **Start** — Plugin calls `POST /api/v1/auth/plugin/login/start`. The backend creates a `plugin_login_attempt` row (UUID, short code, expiry) and returns a browser URL + `attempt_id`.
2. **Browser Launch** — Plugin opens the returned URL in the user's default browser. Currently resolves to a temporary operator-assisted completion page at `telemyapp.com/login/plugin?attempt=...`.
3. **Poll** — Plugin polls `POST /api/v1/auth/plugin/login/poll` with `attempt_id` every few seconds. The backend returns `pending`, `completed`, or `expired`.
4. **Complete** — When the web tier (or operator) approves the attempt, `POST /api/v1/auth/plugin/login/complete` binds the attempt to a `user_id` and issues auth material (`cp_access_jwt` + `cp_refresh_token`). This endpoint is protected by shared backend auth — it is not callable from the plugin.
5. **Token Storage** — On successful poll, the plugin receives the issued tokens and persists them in the DPAPI-backed `vault.json` via ConfigVault.

```
Plugin                          Backend                         Browser
  ¦                                ¦                                ¦
  ¦--- POST /auth/plugin/login/start -->¦                          ¦
  ¦<-- { attempt_id, browser_url } -----¦                          ¦
  ¦                                ¦                                ¦
  ¦-- launch browser_url ------------------------------------------>¦
  ¦                                ¦                                ¦
  ¦--- POST /auth/plugin/login/poll --->¦    (operator/user        ¦
  ¦<-- { status: "pending" } -----------¦     completes login)     ¦
  ¦                                ¦<-- POST /auth/plugin/login/complete --¦
  ¦--- POST /auth/plugin/login/poll --->¦                          ¦
  ¦<-- { status: "completed", tokens }--¦                          ¦
  ¦                                ¦                                ¦
  ¦-- store tokens in vault.json   ¦                                ¦
```

### Session Management Endpoints

Once authenticated, the plugin manages its session via:

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/v1/auth/session` | GET | Validate current session, returns user identity + entitlement status |
| `/api/v1/auth/refresh` | POST | Exchange `cp_refresh_token` for a new `cp_access_jwt` |
| `/api/v1/auth/logout` | POST | Invalidate session server-side, plugin clears vault |

The plugin proactively refreshes tokens before expiry and falls back to re-login on refresh failure.

### Token Regeneration

`POST /api/v1/user/regenerate-token` allows an authenticated user to regenerate their `stream_token`. This invalidates the previous `live_{stream_token}` / `play_{stream_token}` stream IDs on the relay and issues a new one. The plugin updates its local config after regeneration.

### Auth State in ConfigVault

Auth credentials are persisted in the DPAPI-encrypted `vault.json` alongside existing secrets:

| Key | Value | Notes |
|-----|-------|-------|
| `cp_access_jwt` | Short-lived JWT for control-plane API calls | Bearer token for all authenticated endpoints |
| `cp_refresh_token` | Long-lived refresh token | Used to rotate `cp_access_jwt` without re-login |

These are never exposed to dock JS. The dock receives only a boolean `authenticated` flag and entitlement status via the telemetry snapshot.

### Entitlement-Gated Relay Start

`POST /api/v1/relay/start` now enforces entitlement server-side. The backend:

1. Validates the `cp_access_jwt` Bearer token.
2. Looks up the user's entitlement tier.
3. Rejects the request if the user is not entitled to relay access.
4. Proceeds with relay provisioning only after both identity and entitlement checks pass.

The dock UI disables the relay start button when unauthenticated or unentitled, but this is a UX convenience — the backend is authoritative.

### Dock Auth Support

The dock UI supports the full auth lifecycle via native action dispatch:

- **Login** — `auth_login` action triggers the browser-handoff flow in C++.
- **Browser launch** — Native `ShellExecuteW` opens the auth URL.
- **Login polling** — C++ polls the backend on a worker thread; dock receives status updates via `receiveDockActionResultJson`.
- **Session refresh** — Automatic token refresh on the C++ side; dock is notified of auth state changes.
- **Logout** — `auth_logout` action clears vault credentials and calls the backend logout endpoint.
- **Entitlement-aware relay gating** — Relay start/stop controls are enabled or disabled based on the `entitled` flag in the telemetry snapshot.

### Database Migrations

Two new migrations support the auth flow:

- **`0008_auth_sessions.sql`** — Creates the `auth_sessions` table for server-side session tracking (session ID, user ID, refresh token hash, expiry, revocation).
- **`0009_plugin_login_attempts.sql`** — Creates the `plugin_login_attempts` table for the browser-handoff flow (attempt ID, short code, status, expiry, bound user ID).


## Dock Source Management

- **Source of truth**: `telemy-v0.0.5/obs-plugin/dock/`
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

OBS reads dock assets from staged location. C++ inlines all JS/HTML at startup into a `data:text/html` URL â€” files are not loaded by CEF from disk at runtime.

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

Always call `bfree()` after `obs_module_config_path()` â€” OBS uses its own allocator.


## BYOR (Bring Your Own Relay) Architecture

v0.0.5 introduces BYOR support, allowing free-tier users to connect their own relay infrastructure instead of using managed AWS provisioning.

### BYORProvisioner

BYORProvisioner implements the Provisioner interface alongside the existing AWSProvisioner. It reads user-stored relay configuration (host, port, stream key) from the database rather than launching cloud infrastructure. No cloud API calls are made � the provisioner simply returns the user's pre-configured relay details as a session.

### Per-User Routing in handleRelayStart()

handleRelayStart() routes relay requests based on the user's plan_tier:

- **Free-tier users** ? BYORProvisioner (must have relay config saved via POST /api/v1/user/relay-config)
- **Managed tiers (pro, etc.)** ? AWSProvisioner (cloud-provisioned relay with EIP)

If a free-tier user has not configured their BYOR relay, the API returns a yor_config_required error prompting them to set up their relay details first.

### relay_mode in Auth Session Response

GET /api/v1/auth/session now includes a elay_mode field indicating the user's relay access mode:

- yor � free-tier user with BYOR relay configured
- managed � paid-tier user with cloud-provisioned relay
- 
one � free-tier user without relay config (relay start disabled)

The dock UI uses this field to show the appropriate relay controls (BYOR settings panel vs. managed relay start).

### Plugin ConnectDirect / DisconnectDirect

For BYOR relays, the C++ plugin uses ConnectDirect and DisconnectDirect paths instead of the managed provisioning lifecycle. The dock bridge wires elay_connect_direct and elay_disconnect_direct actions to native handlers that connect to the user-specified relay host without any control-plane provisioning step.

### Stats Degradation for Non-SLS Relays

BYOR relays may not run the same SLS + srtla-receiver stack as managed relays. The plugin handles missing or unreachable stats endpoints gracefully � relay telemetry cards show "Stats unavailable" rather than erroring, and the core relay connection remains functional without stats polling.

## Platform Constraints

- **Windows-only**: Win32 DPAPI, WinHTTP, `GetSystemTimes`/`GlobalMemoryStatusEx`
- **NVIDIA GPU optional**: NVML loaded dynamically, graceful degradation
- **OBS Studio required**: Direct C API integration, CEF dock hosting
- **Qt6 required**: Bundled with OBS, used for dock widget + JSON parsing
