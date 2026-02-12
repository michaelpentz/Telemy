# OBS Telemetry Bridge (v1.2)

Minimal Rust telemetry engine that:
- Collects OBS + system + network metrics
- Streams them to an in-OBS dashboard (local HTTP + WebSocket)
- Pushes metrics to Grafana Cloud (OTLP/HTTP)

## Quick Start

1. Initialize a default config:

```
obs-telemetry-bridge config-init
```

2. Store secrets in the vault:

```
obs-telemetry-bridge vault-set obs_password "<OBS_WEBSOCKET_PASSWORD>"
obs-telemetry-bridge vault-set grafana_auth "Bearer <GRAFANA_TOKEN>"
```

3. Run the app:

```
obs-telemetry-bridge
```

It prints a local OBS dashboard URL like:

```
http://127.0.0.1:7070/obs?token=... 
```

Add that URL to OBS as a **Browser Source**.

## Commands

- `obs-telemetry-bridge config-init`
- `obs-telemetry-bridge vault-set <key> <value>`
- `obs-telemetry-bridge vault-get <key>`
- `obs-telemetry-bridge vault-list`
- `obs-telemetry-bridge autostart-enable`
- `obs-telemetry-bridge autostart-disable`

## Logging

Set `RUST_LOG=info` (or `debug`, `trace`) to control log output.

## Grafana

A starter template is provided in `grafana-dashboard-template.json`.

## System Tray

The tray icon provides:
- Open Dashboard
- Quit

The tray icon is loaded from `telemy.ico` in the install folder.

Disable via:

```
[tray]
enable = false
```

## Theme

Customize the OBS dashboard theme in `config.toml`:

```
[theme]
font_family = "Arial, sans-serif"
bg = "#0b0e12"
panel = "#111723"
muted = "#8da3c1"
good = "#33d17a"
warn = "#f6d32d"
bad = "#e01b24"
line = "#1f2a3a"
```

## OBS Auto-Detect

When enabled, the app will only attempt OBS WebSocket connection while `obs64.exe` is running.

```
[obs]
auto_detect_process = true
process_name = "obs64.exe"
```

## Auto-start

Enable automatic launch on Windows:

```
obs-telemetry-bridge autostart-enable
```

Disable:

```
obs-telemetry-bridge autostart-disable
```

## Notes

- The dashboard is local-only and protected by a token.
- If Grafana is enabled in `config.toml`, OTLP metrics export is enabled.
- GPU metrics require an NVIDIA GPU with NVML available.

## Packaging (Windows)

A draft Inno Setup script is included at `installer/setup.iss`.

Recommended approach:
- Build a release binary with `cargo build --release`
- Package the binary, `config.example.toml`, the dashboard template, and `telemy.ico`
- Use the Inno Setup script to build an installer

## Build

```
cargo build --release
```
