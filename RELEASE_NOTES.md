# Release Notes — v0.0.5

## Overview

v0.0.5 builds on the v0.0.4 all-native C++ plugin with BYOR (Bring Your Own Relay) support, multi-connection management, and relay provisioning improvements.

## What's New

### BYOR (Bring Your Own Relay)

Free-tier users can now connect their own relay infrastructure instead of managed AWS provisioning. The dock shows a BYOR settings panel with host/port/stream inputs. The C++ plugin uses `ConnectDirect`/`DisconnectDirect` paths that bypass the managed provisioning lifecycle.

### ConnectionManager

A new `ConnectionManager` class (`src/connection_manager.cpp`) sits between `PluginEntry` and `RelayClient`. It owns all `RelayClient` instances, runs background stats polling off the OBS tick thread, and manages connection persistence. Sensitive BYOR fields are stored in `vault.json` (DPAPI-encrypted); non-sensitive fields in `config.json`.

### Relay Provision Progress UI

Real-time 6-step provisioning feedback replaces the static "Provisioning relay..." message. Steps: `launching_instance` -> `waiting_for_instance` -> `starting_docker` -> `starting_containers` -> `creating_stream` -> `ready`. Each step has a minimum 3-second dwell. The dock shows the current step label, step counter (N/6), animated progress bar, and blinking dots.

### Per-Link Bitrate Bars

Per-link bitrate bars are now nested under the Relay section with carrier labels (T-Mobile, AT&T, Verizon, etc.) derived from IPinfo Lite ASN data on the relay instance.

### Provider Abstraction

`AWSProvisioner` and `BYORProvisioner` both implement a common `Provisioner` interface. `InstanceID` field replaces `AWSInstanceID` across the control plane. `relay_mode` field in `/api/v1/auth/session` response (`yor`/`managed`/`none`) lets the dock show appropriate controls.

## Platform Requirements

- **Windows only** (Win32 DPAPI, WinHTTP)
- **OBS Studio** with browser source support (CEF)
- **Qt6** (bundled with OBS)
- **NVIDIA GPU optional** — NVML metrics gracefully degrade

---

# Release Notes — v0.0.4

## Overview

v0.0.4 is a complete architecture rewrite. The hybrid Rust bridge + C++ shim + IPC pipe system from v0.0.3 has been replaced with a **single all-native C++ OBS plugin DLL**. No standalone process, no IPC layer, no Rust dependency.

## What's New

### Single DLL Plugin

The entire plugin is now one file: `aegis-obs-plugin.dll`. It loads directly into OBS as a standard plugin module. Metrics collection, relay communication, config management, and dock hosting all run inside the OBS process.

### Native Metrics Collection

Telemetry is collected directly via the OBS C API — per-output stats, active FPS, dropped/missed/skipped frame counts, studio mode detection. System metrics come from Win32 APIs (CPU, memory) and NVIDIA NVML (GPU utilization, temperature). No intermediary process or protocol.

### SRTLA Relay Stack

EC2 relay instances now run [OpenIRL srtla-receiver](https://github.com/OpenIRL/srtla-receiver) — providing bonded SRTLA ingest (UDP 5000), SRT player output (UDP 4000), and a management API. Instances bootstrap automatically via Docker Compose user-data. Connect your IRL Pro or SRT encoder to `srt://<relay-ip>:5000` and pull the clean stream in OBS via `srt://<relay-ip>:4000`.

### Dock UI Improvements

- **Encoders & Uploads section** — per-output health visualization with rolling-max self-calibrating thresholds, grouped by encoder (e.g., Horizontal 1080p, Vertical 1080p).
- **Scene linking persistence** — scene-to-rule mappings now survive OBS restarts (native disk storage).
- **Theme integration** — dock matches OBS theme immediately on load, including light theme support.

### Simplified Bridge

The JavaScript bridge between C++ and the React dock is now a thin pass-through. C++ produces the complete dock state JSON directly — no reducer, no projection layer, no intermediate transformations.

## What's Gone

| Removed Component | Why |
|---|---|
| Rust bridge binary | Replaced by native C++ |
| IPC named pipes | No separate process needed |
| obs-websocket dependency | Direct OBS C API instead |
| Web dashboard (port 7070) | All UI in the OBS dock |
| System tray icon | No standalone process |
| Autostart registry | OBS loads the plugin directly |

## Testing Milestones (2026-03-05)

- **E2E Relay Telemetry Validated** — Confirmed the full telemetry path from an IRL Pro bonded stream (WiFi + T-Mobile) through the relay back to the OBS Dock.
- **Data Flow Confirmed** — C++ native code successfully polls the SLS stats API via WinHTTP, injects fields into the telemetry snapshot, and surfaces them in the React UI via the JS bridge.
- **Relay Performance** — Observed stable metrics during bonding: 4.6 Mbps aggregate bitrate, 53ms RTT, and 1000ms latency.
- **Known Gap** — Per-link telemetry (individual cellular/WiFi stats) is currently pending a custom `srtla_rec` fork to expose link-level metadata.

## Platform Requirements

- **Windows only** (Win32 DPAPI, WinHTTP)
- **OBS Studio** with browser source support (CEF)
- **Qt6** (bundled with OBS)
- **NVIDIA GPU optional** — NVML metrics gracefully degrade

## Known Limitations

- Relay activation requires `relay_api_host` configured in `%APPDATA%/Telemy/config.json`
- Custom AMI for relay instances deferred (user-data bootstrap sufficient for testing)
- srtla-receiver management UI is unauthenticated (restricted to control plane IP via security group)
- Grafana OTLP export deferred to future version
