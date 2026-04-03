# Telemy OBS Plugin

Open-source OBS plugin for IRL streaming telemetry, relay management, and bonded stream health monitoring.

## What It Is

Telemy is a single-DLL OBS plugin (`telemy-obs-plugin.dll`) that runs entirely inside the OBS process. It collects metrics directly from the OBS C API, manages SRT/SRTLA relay connections, and drives a dock UI via CEF JavaScript injection.

No standalone process. No IPC. No Rust dependency.

## Features

- **Real-time telemetry** — CPU, GPU (NVIDIA NVML), memory, encoder stats, per-output bitrate monitoring
- **Relay management** — managed pool relays and Bring Your Own Relay (BYOR) support
- **Bonded stream health** — per-link SRTLA stats, connection quality indicators
- **Dock UI** — embedded React panel in OBS with scene linking, encoder health bars, relay controls
- **Secure config** — DPAPI-encrypted vault for tokens and credentials

## Build from Source

Requires: OBS Studio SDK headers, Qt6, CMake, MSVC

```bash
cd obs-plugin

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

## Install

1. Copy `telemy-obs-plugin.dll` to `<OBS>/obs-plugins/64bit/`
2. Copy dock assets to `<OBS>/data/obs-plugins/telemy-obs-plugin/`
3. Restart OBS

## Hosted Relay Service

For managed relay infrastructure (no self-hosting required), visit [telemyapp.com](https://telemyapp.com).

## Architecture

```
OBS C API
Win32 APIs  -->  MetricsCollector (500ms) --> JSON snapshot --> CEF ExecuteJavaScript()
NVML                                                |                     |
                                                    v                     v
                                          ConnectionManager         Dock UI (React)
                                               |
                                               v
                                          RelayClient --> HTTPS --> Control Plane API
```

- **MetricsCollector** — polls OBS C API, Win32 system stats, NVIDIA NVML every 500ms
- **ConnectionManager** — manages RelayClient instances, background stats polling
- **ConfigVault** — DPAPI-encrypted storage for sensitive fields
- **DockHost** — CEF browser dock panel with JS bridge for telemetry injection

## Platform

- **Windows only** — Win32 DPAPI, WinHTTP, Windows system APIs
- **OBS Studio** with browser source support (CEF required)
- **NVIDIA GPU optional** — NVML metrics degrade gracefully if absent

## Contributing

Issues and pull requests welcome. Please open an issue before starting large changes.

## License

[MIT](LICENSE)
