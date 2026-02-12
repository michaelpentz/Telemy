# OBS Telemetry Bridge

A professional-grade monitoring solution for multi-encode OBS streaming setups. This system bridges OBS Studio with remote monitoring via Grafana Cloud while providing real-time health metrics through an in-OBS Lua display.

## 🎯 Project Vision

Enable streamers running complex multi-encode/multi-stream configurations to:
- Monitor stream health in real-time within OBS
- Access remote telemetry via Grafana Cloud for team monitoring
- Perform pre-flight checks before going live
- Maintain secure, encrypted credential storage
- Scale to cloud-based monitoring and storage (future phases)

## 🏗️ Architecture Overview

The system consists of three core components:

### 1. **Telemetry Engine** (Rust Executable)
- Standalone background application
- Collects metrics from OBS via WebSocket
- Monitors system resources (GPU via NVML, CPU, network)
- Performs pre-flight stream validation
- Pushes telemetry to Grafana Cloud
- Manages encrypted credential vault

### 2. **Local Bridge** (JSON File)
- Lightweight data exchange mechanism
- Written by Rust engine, read by Lua script
- Contains real-time health metrics
- No network calls = zero OBS performance impact

### 3. **OBS Display** (Lua Script)
- Native OBS script (Scripts menu)
- Reads local bridge file
- Displays multi-stream health dashboard
- No direct network access or key handling

```
┌─────────────────┐
│  Rust Engine    │
│  (Background)   │
└────────┬────────┘
         │
         ├──────────────────┐
         │                  │
         ▼                  ▼
  ┌─────────────┐    ┌──────────────┐
  │ Local Bridge│    │ Grafana Cloud│
  │  (JSON)     │    │   (OTLP)     │
  └──────┬──────┘    └──────────────┘
         │
         ▼
  ┌─────────────┐
  │ OBS Lua     │
  │ Dashboard   │
  └─────────────┘
```

## 🔐 Security Architecture

### Credential Management
- **Windows DPAPI Encryption**: Stream keys encrypted using Windows User Profile
- **Zero-Key Lua**: OBS script never accesses raw credentials
- **Blind Pre-Flight**: TCP/RTMP handshake checks without exposing keys until stream start

### Data Flow Security
- Local bridge uses file system permissions (user-only read/write)
- Grafana Cloud push uses API token authentication
- No plaintext keys in memory dumps or logs

## 📊 Monitored Metrics

### OBS Health Metrics (via WebSocket)
- Dropped frames (network & rendering)
- Encoding lag/overload
- Bitrate fluctuations
- FPS stability
- Stream status per output

### System Metrics
- GPU utilization, temperature, VRAM usage (NVENC)
- CPU usage per core
- Network throughput and latency
- Disk I/O (for recording)

### Pre-Flight Checks
- RTMP/RTMPS port availability
- Bandwidth test streams (platform-specific)
  - Twitch: `?bandwidthtest=true` key append
  - YouTube: Private test stream key
  - Kick: TBD investigation
  - TikTok: TBD investigation
- Ingest server latency measurement

## 🚀 Deployment Model

### User Workflow
1. **Launch Rust Engine**: User starts executable before streaming
2. **Pre-Flight Validation**: Run connection tests to all configured platforms
3. **Start OBS**: Engine detects OBS launch, begins metric collection
4. **Monitor**: View real-time health in OBS Lua dashboard
5. **Remote Access**: Team views Grafana Cloud dashboard via shared link

### Data Storage
- **Local**: SQLite database (24-hour rolling history)
- **Cloud**: Grafana Cloud (retention based on user's plan)
- **Future**: User-provided cloud storage (AWS S3, Azure Blob, GCP Storage)

## 🎛️ Configuration

### Initial Setup
```yaml
# config.toml (encrypted by Rust engine)
[platforms]
  [platforms.twitch]
    enabled = true
    ingest_url = "rtmps://live.twitch.tv:443/app/"
    stream_key = "<encrypted>"
  
  [platforms.youtube]
    enabled = true
    ingest_url = "rtmps://a.rtmps.youtube.com:443/live2/"
    stream_key = "<encrypted>"
  
  [platforms.kick]
    enabled = true
    ingest_url = "rtmps://fra.stream.kick.com:443/app/"
    stream_key = "<encrypted>"

[grafana]
  api_url = "https://prometheus-prod-01-eu-west-0.grafana.net/api/prom/push"
  api_token = "<encrypted>"

[obs]
  websocket_port = 4455
  websocket_password = "<encrypted>"

[engine]
  metrics_interval = 5  # seconds
  bridge_update_rate = 500  # milliseconds
```

## 📦 Installation

### Prerequisites
- Windows 10/11 (64-bit)
- OBS Studio 28.0+ with WebSocket plugin enabled
- Grafana Cloud free account (or paid tier)
- NVIDIA GPU with NVENC support (optional, for GPU metrics)

### Steps
1. Download latest release from GitHub
2. Run installer or extract portable ZIP
3. Launch `obs-telemetry-bridge.exe`
4. Configure stream keys and Grafana Cloud token
5. Install OBS Lua script via OBS Scripts menu
6. Run pre-flight checks

## 🔄 Update Mechanism

The Rust engine checks for updates on launch:
- Queries GitHub Releases API
- Compares semantic versions
- Downloads and verifies installer (SHA256)
- Prompts user to install update

## 🌐 Grafana Cloud Integration

### Dashboard Features
- Multi-stream health matrix (all platforms at a glance)
- Historical performance graphs (dropped frames over time)
- Encoding efficiency metrics (bitrate vs quality)
- System resource correlation (GPU load vs frame drops)
- Alert annotations (manual or automated)

### Sharing & Collaboration
- Generate shareable dashboard links
- No login required for viewers (read-only access)
- Real-time updates (5-second refresh)

## 📈 Development Roadmap

### Phase 1: Core MVP ✅ (Current Focus)
- [x] Architecture planning
- [ ] Rust engine: OBS WebSocket client
- [ ] Rust engine: Local JSON bridge writer
- [ ] Rust engine: System metrics collector
- [ ] OBS Lua: Dashboard UI
- [ ] OBS Lua: Bridge reader
- [ ] Windows DPAPI credential vault
- [ ] Configuration file parser

### Phase 2: Pre-Flight & Validation
- [ ] TCP/RTMP handshake checks
- [ ] Bandwidth test stream integration
- [ ] Multi-output discovery (auto-detect encodes)
- [ ] Pre-launch validation suite

### Phase 3: Cloud Integration
- [ ] OpenTelemetry OTLP exporter
- [ ] Grafana Cloud authentication
- [ ] Metric batching and buffering
- [ ] Connection resilience (offline queue)
- [ ] Grafana dashboard templates

### Phase 4: Polish & Optimization
- [ ] Auto-update system
- [ ] Installer/uninstaller
- [ ] Shared memory bridge (performance upgrade)
- [ ] User documentation and tutorials
- [ ] Error recovery and diagnostics

### Phase 5: Multi-Tenant Scaling (Future)
- [ ] BYOD (Bring Your Own Device) client model
- [ ] AWS integration (S3, CloudWatch, Elemental)
- [ ] Azure/GCP storage adapters
- [ ] Client ID and access token system
- [ ] Central monitoring dashboard (multi-client)

## 🛠️ Technology Stack

- **Engine**: Rust 1.75+ (tokio async runtime)
- **OBS Integration**: obs-websocket v5 protocol
- **GPU Metrics**: NVIDIA NVML bindings
- **Encryption**: Windows DPAPI via `winapi` crate
- **Cloud Protocol**: OpenTelemetry OTLP/HTTP
- **Local Storage**: SQLite via `rusqlite`
- **Configuration**: TOML via `serde`
- **UI**: OBS Lua 5.1 (native OBS scripting)

## 🤝 Contributing

This is a focused MVP build. Contributions welcome after Phase 1 completion.

### Code Guidelines
- Rust: Follow Clippy pedantic lints
- Lua: Adhere to OBS scripting best practices
- Security: Never log or expose credentials
- Performance: Profile before optimizing

## 📄 License

TBD - To be determined after initial development

## 🔗 Resources

- [OBS WebSocket Protocol](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md)
- [Grafana Cloud OTLP Guide](https://grafana.com/docs/grafana-cloud/send-data/otlp/)
- [NVIDIA NVML Documentation](https://docs.nvidia.com/deploy/nvml-api/)
- [Windows DPAPI Overview](https://learn.microsoft.com/en-us/windows/win32/api/dpapi/)

## 💬 Support

- GitHub Issues: Bug reports and feature requests
- Discussions: Architecture questions and usage help

---

**Status**: 🏗️ In Active Development - Phase 1 (Planning Complete)
