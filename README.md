# Telemy v0.0.5

IRL streaming platform for OBS — native telemetry, relay management, and bonded stream health monitoring.

## What It Is

Telemy is a single-DLL OBS plugin (`telemy-obs-plugin.dll`) that runs entirely inside the OBS process. It collects metrics directly from the OBS C API, manages relay connections (managed pool relays and Bring Your Own Relay), and drives a React dock UI via CEF JavaScript injection.

No standalone process. No IPC. No Rust dependency.

## Components

| Component | Description |
|-----------|-------------|
| `obs-plugin/` | C++ OBS plugin — metrics collection, relay management, dock hosting |
| `control-plane/` | Go backend — relay provisioning, JWT auth, session state |
| `srtla-fork/` | Forked `srtla_rec` with per-link stats + ASN lookup |
| `srtla-receiver-fork/` | Forked Docker image (`michaelpentz/srtla-receiver`) |
| `obs-plugin/dock/` | React dock UI source (esbuild bundle) |

## Quick Build

From `telemy-v0.0.5/obs-plugin/`:

```bash
cmake -B build-obs-cef \
  -DTELEMY_BUILD_OBS_PLUGIN=ON \
  -DTELEMY_ENABLE_OBS_BROWSER_DOCK_HOST=ON \
  -DTELEMY_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON \
  -DOBS_INCLUDE_DIRS="<path-to-obs-libobs>;<path-to-obs-frontend-api>" \
  -DOBS_LIBRARIES="obs;obs-frontend-api" \
  -DOBS_LIBRARY_DIRS="<path-to-obs-import-libs>" \
  -DOBS_BROWSER_PANEL_DIR="<path-to-obs-browser-panel-headers>" \
  -DCMAKE_PREFIX_PATH="<path-to-qt6>"

cmake --build build-obs-cef --config Release
```

**Deploy**: Copy `telemy-obs-plugin.dll` to `<OBS>/obs-plugins/64bit/`. Copy dock assets to `<OBS>/data/obs-plugins/telemy-obs-plugin/`. Restart OBS.

## Dock JS Build

From `telemy-v0.0.5/obs-plugin/dock/`:

```bash
NODE_PATH=../../../dock-preview/node_modules npx esbuild telemy-dock-entry.jsx \
  --bundle --format=iife --jsx=automatic --outfile=telemy-dock-app.js \
  --target=es2020 --minify
```

## Architecture Overview

```
OBS C API
Win32 APIs  -->  MetricsCollector (500ms) --> JSON snapshot --> CEF ExecuteJavaScript()
NVML                                                |                     |
                                                    v                     v
                                          ConnectionManager         Dock UI (React)
                                               |
                                               v
                                          RelayClient --> HTTPS --> Go Control Plane
```

Key design decisions:
- **ConnectionManager** owns all `RelayClient` instances and runs background stats polling so the OBS tick thread is never blocked by network I/O.
- **ConfigVault** stores sensitive fields (JWT tokens, BYOR stream keys) in DPAPI-encrypted `vault.json`; non-sensitive settings in plain `config.json`.
- **BYOR support** — users can connect their own relay (no account required) via `ConnectDirect`/`DisconnectDirect`, bypassing managed provisioning.

See `docs/ARCHITECTURE.md` for the full architecture reference.

## Documentation

| Doc | Description |
|-----|-------------|
| `docs/ARCHITECTURE.md` | Component architecture, data flows, bridge contract |
| `docs/API_SPEC_v1.md` | Control plane REST API reference |
| `docs/DB_SCHEMA_v1.md` | PostgreSQL schema |
| `docs/AUTH_ENTITLEMENT_MODEL.md` | Auth layers and relay access model |
| `docs/RELAY_DEPLOYMENT.md` | Relay pool deployment guide |
| `docs/STATE_MACHINE_v1.md` | IRL/Studio state machine spec |
| `docs/QA_CHECKLIST_RELAY_TELEMETRY.md` | QA test checklist |
| `CHANGELOG.md` | Version history |
| `RELEASE_NOTES.md` | Release summaries |

## Platform

- **Windows only** — Win32 DPAPI, WinHTTP, `GetSystemTimes`/`GlobalMemoryStatusEx`
- **OBS Studio** with browser source support (CEF required)
- **NVIDIA GPU optional** — NVML metrics degrade gracefully if absent
