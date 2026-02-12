# Development Guide - OBS Telemetry Bridge

Technical architecture and implementation specifications for developers.

---

## 📐 System Architecture

### Component Interaction Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                         User's Machine                          │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              Rust Telemetry Engine                        │  │
│  │                                                            │  │
│  │  ┌─────────────┐  ┌──────────────┐  ┌────────────────┐  │  │
│  │  │   Config    │  │   Metrics    │  │    Security    │  │  │
│  │  │   Manager   │  │  Collector   │  │     Vault      │  │  │
│  │  └──────┬──────┘  └──────┬───────┘  └────────┬───────┘  │  │
│  │         │                │                    │           │  │
│  │         └────────────────┴────────────────────┘           │  │
│  │                          │                                │  │
│  │         ┌────────────────┴───────────────┐               │  │
│  │         │                                 │               │  │
│  │         ▼                                 ▼               │  │
│  │  ┌─────────────┐                  ┌─────────────┐        │  │
│  │  │ OBS WebSock │                  │  NVML/Sys   │        │  │
│  │  │   Client    │                  │  Monitors   │        │  │
│  │  └──────┬──────┘                  └──────┬──────┘        │  │
│  │         │                                 │               │  │
│  │         └────────────┬────────────────────┘               │  │
│  │                      │                                    │  │
│  │                      ▼                                    │  │
│  │         ┌────────────────────────┐                       │  │
│  │         │  Telemetry Aggregator  │                       │  │
│  │         └───────────┬────────────┘                       │  │
│  │                     │                                    │  │
│  │         ┌───────────┴───────────┐                       │  │
│  │         │                       │                       │  │
│  │         ▼                       ▼                       │  │
│  │  ┌─────────────┐        ┌──────────────┐              │  │
│  │  │   Bridge    │        │   OTLP       │              │  │
│  │  │   Writer    │        │   Exporter   │              │  │
│  │  │  (JSON)     │        │  (Grafana)   │              │  │
│  │  └──────┬──────┘        └──────┬───────┘              │  │
│  └─────────┼────────────────────────┼─────────────────────┘  │
│            │                        │                        │
│            ▼                        │                        │
│  ┌─────────────────┐               │                        │
│  │  bridge.json    │               │                        │
│  │  (Filesystem)   │               │                        │
│  └────────┬────────┘               │                        │
│           │                        │                        │
│           ▼                        │                        │
│  ┌─────────────────┐               │                        │
│  │  OBS Studio     │               │                        │
│  │                 │               │                        │
│  │  ┌───────────┐  │               │                        │
│  │  │ Lua Script│  │               │                        │
│  │  │ Dashboard │  │               │                        │
│  │  └───────────┘  │               │                        │
│  └─────────────────┘               │                        │
│                                    │                        │
└────────────────────────────────────┼────────────────────────┘
                                     │
                                     ▼
                          ┌──────────────────┐
                          │  Grafana Cloud   │
                          │                  │
                          │ ┌──────────────┐ │
                          │ │  Prometheus  │ │
                          │ │   Storage    │ │
                          │ └──────────────┘ │
                          │ ┌──────────────┐ │
                          │ │  Dashboard   │ │
                          │ │   Renderer   │ │
                          │ └──────────────┘ │
                          └──────────────────┘
```

---

## 🔧 Rust Engine Architecture

### Module Structure

```
obs-telemetry-bridge/
├── src/
│   ├── main.rs                 # Entry point, CLI, update checks
│   ├── config/
│   │   ├── mod.rs              # Config module exports
│   │   ├── loader.rs           # TOML parsing and validation
│   │   ├── schema.rs           # Config structure definitions
│   │   └── watcher.rs          # Hot-reload on config changes
│   ├── security/
│   │   ├── mod.rs              # Security module exports
│   │   ├── vault.rs            # DPAPI encryption/decryption
│   │   ├── credential.rs       # Credential struct and handling
│   │   └── memory.rs           # Secure memory wiping
│   ├── metrics/
│   │   ├── mod.rs              # Metrics module exports
│   │   ├── collector.rs        # Metric collection orchestration
│   │   ├── obs.rs              # OBS WebSocket metrics
│   │   ├── system.rs           # CPU, memory, disk metrics
│   │   ├── gpu.rs              # NVML GPU metrics
│   │   └── network.rs          # Bandwidth and latency
│   ├── bridge/
│   │   ├── mod.rs              # Bridge module exports
│   │   ├── writer.rs           # JSON bridge file writer
│   │   ├── schema.rs           # Bridge data structures
│   │   └── validator.rs        # Data validation before write
│   ├── cloud/
│   │   ├── mod.rs              # Cloud module exports
│   │   ├── otlp.rs             # OpenTelemetry OTLP client
│   │   ├── batch.rs            # Metric batching logic
│   │   └── retry.rs            # Exponential backoff retry
│   ├── preflight/
│   │   ├── mod.rs              # Pre-flight module exports
│   │   ├── tcp.rs              # TCP handshake checks
│   │   ├── rtmp.rs             # RTMP connection validation
│   │   └── bandwidth.rs        # Bandwidth test streams
│   ├── storage/
│   │   ├── mod.rs              # Storage module exports
│   │   ├── sqlite.rs           # Local SQLite operations
│   │   └── migrations.rs       # Schema migrations
│   └── ui/
│       ├── mod.rs              # UI module exports (future TUI)
│       └── tray.rs             # System tray icon (future)
├── lua/
│   └── obs_dashboard.lua       # OBS Lua script
├── Cargo.toml                  # Rust dependencies
└── build.rs                    # Build-time code generation
```

### Key Dependencies

```toml
[dependencies]
# Async runtime
tokio = { version = "1.35", features = ["full"] }
tokio-util = "0.7"

# OBS WebSocket client
obws = "0.11"  # OBS WebSocket v5 protocol

# Metrics and telemetry
opentelemetry = "0.21"
opentelemetry-otlp = "0.14"
prometheus = "0.13"

# GPU monitoring
nvml-wrapper = "0.9"  # NVIDIA NVML bindings

# System metrics
sysinfo = "0.30"
psutil = "3.2"

# Configuration
serde = { version = "1.0", features = ["derive"] }
toml = "0.8"
config = "0.13"

# Security
winapi = { version = "0.3", features = ["dpapi", "wincrypt"] }
zeroize = "1.7"  # Secure memory wiping

# Storage
rusqlite = { version = "0.30", features = ["bundled"] }
diesel = { version = "2.1", features = ["sqlite"] }

# Networking
reqwest = { version = "0.11", features = ["json"] }
hyper = "0.14"

# Error handling
anyhow = "1.0"
thiserror = "1.0"

# Logging
tracing = "0.1"
tracing-subscriber = { version = "0.3", features = ["env-filter"] }

# Serialization
serde_json = "1.0"

# CLI
clap = { version = "4.4", features = ["derive"] }

[dev-dependencies]
mockall = "0.12"  # Mocking for tests
criterion = "0.5"  # Benchmarking
```

---

## 📊 Data Structures

### Bridge Schema (JSON)

The local bridge file is written at a configurable interval (default: 500ms).

```json
{
  "version": "1.0.0",
  "timestamp": 1704067200,
  "system": {
    "cpu_usage_percent": 45.2,
    "memory_used_gb": 12.4,
    "memory_total_gb": 32.0,
    "gpu": {
      "name": "NVIDIA GeForce RTX 4060",
      "utilization_percent": 78,
      "temperature_celsius": 72,
      "vram_used_mb": 6800,
      "vram_total_mb": 8192,
      "encoder_utilization_percent": 85
    }
  },
  "obs": {
    "connected": true,
    "streaming": true,
    "recording": false,
    "outputs": [
      {
        "name": "Twitch",
        "active": true,
        "dropped_frames": 12,
        "total_frames": 18000,
        "drop_percentage": 0.067,
        "bitrate_kbps": 6000,
        "fps": 60.0,
        "encoding_lag_ms": 3
      },
      {
        "name": "YouTube",
        "active": true,
        "dropped_frames": 5,
        "total_frames": 18000,
        "drop_percentage": 0.028,
        "bitrate_kbps": 8000,
        "fps": 60.0,
        "encoding_lag_ms": 2
      }
    ],
    "total_dropped_frames": 17,
    "total_frames": 36000,
    "overall_drop_percentage": 0.047
  },
  "network": {
    "upload_speed_mbps": 48.5,
    "download_speed_mbps": 450.2,
    "latency_ms": 18
  },
  "health": {
    "overall": "healthy",
    "alerts": []
  }
}
```

### Configuration Schema (TOML)

User configuration file, encrypted fields stored separately.

```toml
[engine]
# Metrics collection interval in seconds
metrics_interval = 5

# Bridge update rate in milliseconds
bridge_update_rate = 500

# Enable GPU monitoring (requires NVIDIA GPU)
enable_gpu_metrics = true

# Log level: trace, debug, info, warn, error
log_level = "info"

# Local storage retention in hours
local_retention_hours = 24

[obs]
# OBS WebSocket connection
host = "localhost"
port = 4455
# Password is encrypted and stored in vault

[platforms.twitch]
enabled = true
ingest_url = "rtmps://live.twitch.tv:443/app/"
# stream_key is encrypted and stored in vault

[platforms.youtube]
enabled = true
ingest_url = "rtmps://a.rtmps.youtube.com:443/live2/"
# stream_key is encrypted and stored in vault

[platforms.kick]
enabled = true
ingest_url = "rtmps://fra.stream.kick.com:443/app/"
# stream_key is encrypted and stored in vault

[platforms.tiktok]
enabled = false
ingest_url = "rtmps://live.tiktok.com:443/live/"
# stream_key is encrypted and stored in vault

[grafana]
enabled = true
# api_url and api_token are encrypted and stored in vault
push_interval = 10  # seconds
batch_size = 100    # metrics per batch

[preflight]
# Enable pre-flight checks
enabled = true

# Run checks automatically on startup
auto_run = false

# Bandwidth test duration in seconds
bandwidth_test_duration = 10

# Timeout for connection tests in seconds
connection_timeout = 5
```

### Encrypted Vault Structure

Stored in `%APPDATA%\OBSTelemetryBridge\vault.dat` (binary format, DPAPI encrypted).

```rust
#[derive(Serialize, Deserialize, Zeroize)]
#[zeroize(drop)]
struct Vault {
    version: u32,
    credentials: Vec<Credential>,
}

#[derive(Serialize, Deserialize, Zeroize)]
#[zeroize(drop)]
struct Credential {
    id: String,           // "obs.websocket.password"
    value: String,        // Actual secret
    created_at: i64,      // Unix timestamp
    last_used: i64,       // Unix timestamp
}
```

---

## 🔌 OBS WebSocket Integration

### Connection Management

```rust
use obws::Client;
use tokio::time::{interval, Duration};

pub struct OBSClient {
    client: Client,
    host: String,
    port: u16,
    password: String,
}

impl OBSClient {
    pub async fn connect(&mut self) -> Result<()> {
        self.client = Client::connect(
            &self.host, 
            self.port, 
            Some(&self.password)
        ).await?;
        Ok(())
    }

    pub async fn collect_metrics(&self) -> Result<OBSMetrics> {
        let stats = self.client.outputs().get_output_status("").await?;
        let scenes = self.client.scenes().get_scene_list().await?;
        
        // Collect metrics from all active outputs
        let mut outputs = Vec::new();
        for output in self.client.outputs().list_outputs().await? {
            if output.active {
                outputs.push(OutputMetrics {
                    name: output.name,
                    dropped_frames: stats.output_skipped_frames,
                    total_frames: stats.output_total_frames,
                    // ... more fields
                });
            }
        }

        Ok(OBSMetrics {
            connected: true,
            streaming: stats.output_active,
            outputs,
            // ... more fields
        })
    }
}
```

### Metric Collection Loop

```rust
pub async fn start_metrics_loop(
    obs: Arc<Mutex<OBSClient>>,
    bridge: Arc<BridgeWriter>,
    cloud: Arc<OTLPExporter>,
    interval_secs: u64,
) {
    let mut ticker = interval(Duration::from_secs(interval_secs));

    loop {
        ticker.tick().await;

        // Collect all metrics in parallel
        let (obs_metrics, system_metrics, gpu_metrics) = tokio::join!(
            collect_obs_metrics(&obs),
            collect_system_metrics(),
            collect_gpu_metrics(),
        );

        // Aggregate into unified structure
        let telemetry = Telemetry {
            timestamp: Utc::now().timestamp(),
            obs: obs_metrics?,
            system: system_metrics?,
            gpu: gpu_metrics,
            network: collect_network_metrics().await?,
            health: calculate_health_status(&obs_metrics?, &system_metrics?),
        };

        // Write to local bridge (non-blocking)
        bridge.write(telemetry.clone()).await?;

        // Push to cloud (buffered, non-critical)
        cloud.push(telemetry).await.ok(); // Don't fail if cloud is down
    }
}
```

---

## 🔐 Security Implementation

### DPAPI Encryption Workflow

```rust
use winapi::um::dpapi::{CryptProtectData, CryptUnprotectData};
use zeroize::Zeroize;

pub struct Vault {
    path: PathBuf,
}

impl Vault {
    /// Encrypt and save a credential
    pub fn store(&self, id: &str, value: &str) -> Result<()> {
        // Create credential struct
        let mut credential = Credential {
            id: id.to_string(),
            value: value.to_string(),
            created_at: Utc::now().timestamp(),
            last_used: Utc::now().timestamp(),
        };

        // Serialize to bytes
        let plaintext = bincode::serialize(&credential)?;

        // Encrypt using DPAPI (user-scoped)
        let ciphertext = unsafe {
            let mut blob_in = DATA_BLOB {
                cbData: plaintext.len() as u32,
                pbData: plaintext.as_ptr() as *mut u8,
            };
            let mut blob_out = std::mem::zeroed();

            if CryptProtectData(
                &mut blob_in,
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                0,
                &mut blob_out,
            ) == 0 {
                return Err(anyhow!("DPAPI encryption failed"));
            }

            // Copy encrypted data
            let encrypted = std::slice::from_raw_parts(
                blob_out.pbData,
                blob_out.cbData as usize,
            ).to_vec();

            // Free DPAPI memory
            winapi::um::winbase::LocalFree(blob_out.pbData as *mut _);
            encrypted
        };

        // Zero out plaintext memory
        credential.zeroize();
        drop(credential);

        // Write to vault file
        std::fs::write(&self.path, ciphertext)?;

        Ok(())
    }

    /// Retrieve and decrypt a credential
    pub fn retrieve(&self, id: &str) -> Result<String> {
        // Read encrypted vault
        let ciphertext = std::fs::read(&self.path)?;

        // Decrypt using DPAPI
        let plaintext = unsafe {
            let mut blob_in = DATA_BLOB {
                cbData: ciphertext.len() as u32,
                pbData: ciphertext.as_ptr() as *mut u8,
            };
            let mut blob_out = std::mem::zeroed();

            if CryptUnprotectData(
                &mut blob_in,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                0,
                &mut blob_out,
            ) == 0 {
                return Err(anyhow!("DPAPI decryption failed"));
            }

            // Copy decrypted data
            let decrypted = std::slice::from_raw_parts(
                blob_out.pbData,
                blob_out.cbData as usize,
            ).to_vec();

            // Free DPAPI memory
            winapi::um::winbase::LocalFree(blob_out.pbData as *mut _);
            decrypted
        };

        // Deserialize
        let credential: Credential = bincode::deserialize(&plaintext)?;

        // Verify ID matches
        if credential.id != id {
            credential.zeroize();
            return Err(anyhow!("Credential ID mismatch"));
        }

        let value = credential.value.clone();
        credential.zeroize();

        Ok(value)
    }
}
```

### Secure Memory Handling

All credential structs implement `Zeroize` trait to prevent secrets from lingering in memory:

```rust
use zeroize::Zeroize;

#[derive(Zeroize)]
#[zeroize(drop)]
struct StreamKey {
    platform: String,
    key: String,
}

// When dropped or explicitly zeroized, memory is overwritten
```

---

## 🌐 Grafana Cloud Integration

### OpenTelemetry OTLP Exporter

```rust
use opentelemetry::sdk::export::metrics::aggregation;
use opentelemetry_otlp::{Protocol, WithExportConfig};

pub struct GrafanaExporter {
    client: MetricsExporter,
    batch_size: usize,
    buffer: Vec<Metric>,
}

impl GrafanaExporter {
    pub fn new(api_url: &str, api_token: &str) -> Result<Self> {
        let exporter = opentelemetry_otlp::new_exporter()
            .http()
            .with_endpoint(api_url)
            .with_headers(HashMap::from([
                ("Authorization".to_string(), format!("Bearer {}", api_token)),
            ]))
            .with_protocol(Protocol::HttpBinary)
            .build_metrics_exporter(
                Box::new(aggregation::cumulative_temporality_selector()),
                Box::new(aggregation::stateless_temporality_selector()),
            )?;

        Ok(Self {
            client: exporter,
            batch_size: 100,
            buffer: Vec::new(),
        })
    }

    pub async fn push(&mut self, telemetry: Telemetry) -> Result<()> {
        // Convert to OTLP metrics
        let metrics = self.convert_to_otlp(telemetry);

        self.buffer.extend(metrics);

        // Flush if batch is full
        if self.buffer.len() >= self.batch_size {
            self.flush().await?;
        }

        Ok(())
    }

    async fn flush(&mut self) -> Result<()> {
        if self.buffer.is_empty() {
            return Ok(());
        }

        // Send batch to Grafana Cloud
        self.client.export(&self.buffer).await?;

        self.buffer.clear();
        Ok(())
    }

    fn convert_to_otlp(&self, telemetry: Telemetry) -> Vec<Metric> {
        vec![
            Metric::new(
                "obs.dropped_frames",
                telemetry.obs.total_dropped_frames as f64,
                MetricKind::Counter,
            ),
            Metric::new(
                "obs.bitrate",
                telemetry.obs.outputs[0].bitrate_kbps as f64,
                MetricKind::Gauge,
            ),
            Metric::new(
                "system.cpu.usage",
                telemetry.system.cpu_usage_percent,
                MetricKind::Gauge,
            ),
            Metric::new(
                "system.gpu.utilization",
                telemetry.system.gpu.utilization_percent as f64,
                MetricKind::Gauge,
            ),
            // ... more metrics
        ]
    }
}
```

### Retry Logic with Exponential Backoff

```rust
use tokio::time::{sleep, Duration};

pub async fn push_with_retry(
    exporter: &mut GrafanaExporter,
    telemetry: Telemetry,
) -> Result<()> {
    let mut attempts = 0;
    let max_attempts = 5;
    let base_delay = Duration::from_secs(1);

    loop {
        match exporter.push(telemetry.clone()).await {
            Ok(_) => return Ok(()),
            Err(e) if attempts < max_attempts => {
                attempts += 1;
                let delay = base_delay * 2_u32.pow(attempts - 1);
                tracing::warn!(
                    "Grafana push failed (attempt {}/{}): {}. Retrying in {:?}",
                    attempts,
                    max_attempts,
                    e,
                    delay
                );
                sleep(delay).await;
            }
            Err(e) => {
                tracing::error!("Grafana push failed after {} attempts: {}", attempts, e);
                return Err(e);
            }
        }
    }
}
```

---

## 🧪 Pre-Flight Checks

### TCP Handshake Validation

```rust
use tokio::net::TcpStream;
use tokio::time::timeout;

pub async fn check_tcp_connection(host: &str, port: u16) -> Result<bool> {
    let address = format!("{}:{}", host, port);
    
    match timeout(
        Duration::from_secs(5),
        TcpStream::connect(&address)
    ).await {
        Ok(Ok(_stream)) => {
            tracing::info!("TCP connection to {} successful", address);
            Ok(true)
        }
        Ok(Err(e)) => {
            tracing::error!("TCP connection to {} failed: {}", address, e);
            Ok(false)
        }
        Err(_) => {
            tracing::error!("TCP connection to {} timed out", address);
            Ok(false)
        }
    }
}
```

### RTMP Handshake Simulation

```rust
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio_rustls::TlsConnector;

pub async fn check_rtmp_handshake(ingest_url: &str) -> Result<bool> {
    // Parse RTMPS URL
    let url = Url::parse(ingest_url)?;
    let host = url.host_str().ok_or(anyhow!("Invalid host"))?;
    let port = url.port().unwrap_or(443);

    // Establish TLS connection
    let connector = TlsConnector::from(Arc::new(rustls_config));
    let tcp_stream = TcpStream::connect((host, port)).await?;
    let domain = rustls::ServerName::try_from(host)?;
    let mut tls_stream = connector.connect(domain, tcp_stream).await?;

    // Send RTMP handshake C0+C1
    let c0 = [0x03]; // RTMP version 3
    let c1 = [0u8; 1536]; // Timestamp + random bytes (simplified)
    
    tls_stream.write_all(&c0).await?;
    tls_stream.write_all(&c1).await?;

    // Read S0+S1+S2 response
    let mut s0 = [0u8; 1];
    let mut s1 = [0u8; 1536];
    
    timeout(
        Duration::from_secs(5),
        tls_stream.read_exact(&mut s0)
    ).await??;

    if s0[0] != 0x03 {
        return Ok(false); // Server doesn't support RTMP v3
    }

    timeout(
        Duration::from_secs(5),
        tls_stream.read_exact(&mut s1)
    ).await??;

    tracing::info!("RTMP handshake to {} successful", ingest_url);
    Ok(true)
}
```

### Bandwidth Test Stream

```rust
pub async fn run_bandwidth_test(platform: Platform, stream_key: &str) -> Result<f64> {
    match platform {
        Platform::Twitch => {
            // Append bandwidthtest flag to key
            let test_key = format!("{}?bandwidthtest=true", stream_key);
            send_test_stream(&test_key, 10).await
        }
        Platform::YouTube => {
            // Use separate test stream key (user must configure)
            send_test_stream(stream_key, 10).await
        }
        Platform::Kick | Platform::TikTok => {
            // No official test mode, simulate with short burst
            tracing::warn!("{:?} doesn't support bandwidth testing", platform);
            Ok(0.0)
        }
    }
}

async fn send_test_stream(stream_key: &str, duration_secs: u64) -> Result<f64> {
    // Create minimal video stream (black frames)
    // Send for specified duration
    // Measure accepted bitrate
    // Return average mbps
    todo!("Implement RTMP test stream")
}
```

---

## 🖥️ OBS Lua Dashboard

### Script Structure

```lua
-- obs_dashboard.lua

obs = obslua

-- Configuration
local BRIDGE_FILE_PATH = ""
local UPDATE_INTERVAL_MS = 500
local last_update = 0
local telemetry_data = nil

-- UI Elements
local source = nil
local text_source_name = "Telemetry Dashboard"

-- ═══════════════════════════════════════════════════════════
-- Script Description
-- ═══════════════════════════════════════════════════════════

function script_description()
    return [[
<h2>OBS Telemetry Dashboard</h2>
<p>Displays real-time stream health metrics from the Rust Telemetry Engine.</p>
<p><b>Requirements:</b></p>
<ul>
    <li>Rust Telemetry Engine must be running</li>
    <li>Bridge file path must be configured</li>
</ul>
]]
end

-- ═══════════════════════════════════════════════════════════
-- Script Properties (Settings UI)
-- ═══════════════════════════════════════════════════════════

function script_properties()
    local props = obs.obs_properties_create()

    obs.obs_properties_add_path(
        props,
        "bridge_file",
        "Bridge File Path",
        obs.OBS_PATH_FILE,
        "JSON Files (*.json)",
        nil
    )

    obs.obs_properties_add_int(
        props,
        "update_interval",
        "Update Interval (ms)",
        100,
        5000,
        100
    )

    obs.obs_properties_add_bool(
        props,
        "auto_create_source",
        "Auto-create Text Source"
    )

    return props
end

-- ═══════════════════════════════════════════════════════════
-- Script Defaults
-- ═══════════════════════════════════════════════════════════

function script_defaults(settings)
    local default_path = os.getenv("APPDATA") .. "\\OBSTelemetryBridge\\bridge.json"
    obs.obs_data_set_default_string(settings, "bridge_file", default_path)
    obs.obs_data_set_default_int(settings, "update_interval", 500)
    obs.obs_data_set_default_bool(settings, "auto_create_source", true)
end

-- ═══════════════════════════════════════════════════════════
-- Script Update (called when settings change)
-- ═══════════════════════════════════════════════════════════

function script_update(settings)
    BRIDGE_FILE_PATH = obs.obs_data_get_string(settings, "bridge_file")
    UPDATE_INTERVAL_MS = obs.obs_data_get_int(settings, "update_interval")

    local auto_create = obs.obs_data_get_bool(settings, "auto_create_source")
    if auto_create then
        create_text_source()
    end
end

-- ═══════════════════════════════════════════════════════════
-- Script Load
-- ═══════════════════════════════════════════════════════════

function script_load(settings)
    obs.timer_add(update_dashboard, UPDATE_INTERVAL_MS)
end

-- ═══════════════════════════════════════════════════════════
-- Script Unload
-- ═══════════════════════════════════════════════════════════

function script_unload()
    obs.timer_remove(update_dashboard)
end

-- ═══════════════════════════════════════════════════════════
-- Core Logic: Read Bridge File
-- ═══════════════════════════════════════════════════════════

function read_bridge_file()
    local file = io.open(BRIDGE_FILE_PATH, "r")
    if not file then
        return nil, "Failed to open bridge file"
    end

    local content = file:read("*all")
    file:close()

    -- Parse JSON (OBS Lua doesn't have built-in JSON parser)
    -- Using simple pattern matching for known structure
    local data = parse_json(content)
    return data
end

-- Simple JSON parser (handles our specific bridge format)
function parse_json(json_str)
    -- This is a simplified parser for our known structure
    -- For production, consider bundling a proper JSON library
    local data = {}
    
    -- Extract timestamp
    data.timestamp = json_str:match('"timestamp":%s*(%d+)')
    
    -- Extract OBS streaming status
    data.streaming = json_str:match('"streaming":%s*(%a+)') == "true"
    
    -- Extract outputs (simplified)
    data.outputs = {}
    for output_json in json_str:gmatch('"outputs":%s*%[(.-)%]') do
        for output in output_json:gmatch("{(.-)}") do
            local name = output:match('"name":%s*"(.-)"')
            local drop_pct = tonumber(output:match('"drop_percentage":%s*([%d.]+)'))
            local bitrate = tonumber(output:match('"bitrate_kbps":%s*(%d+)'))
            
            table.insert(data.outputs, {
                name = name,
                drop_percentage = drop_pct,
                bitrate_kbps = bitrate
            })
        end
    end
    
    -- Extract GPU metrics
    data.gpu_temp = tonumber(json_str:match('"temperature_celsius":%s*(%d+)'))
    data.gpu_util = tonumber(json_str:match('"utilization_percent":%s*(%d+)'))
    
    return data
end

-- ═══════════════════════════════════════════════════════════
-- Core Logic: Update Dashboard
-- ═══════════════════════════════════════════════════════════

function update_dashboard()
    local current_time = os.time()
    
    -- Rate limit updates
    if current_time - last_update < (UPDATE_INTERVAL_MS / 1000) then
        return
    end
    
    last_update = current_time
    
    -- Read latest telemetry
    local data, err = read_bridge_file()
    if not data then
        set_dashboard_text("⚠️ Telemetry Unavailable\n" .. (err or "Unknown error"))
        return
    end
    
    telemetry_data = data
    
    -- Render dashboard
    local dashboard_text = render_dashboard(data)
    set_dashboard_text(dashboard_text)
end

-- ═══════════════════════════════════════════════════════════
-- UI Rendering
-- ═══════════════════════════════════════════════════════════

function render_dashboard(data)
    local lines = {}
    
    -- Header
    table.insert(lines, "╔═══════════════════════════════╗")
    table.insert(lines, "║   STREAM HEALTH MONITOR       ║")
    table.insert(lines, "╚═══════════════════════════════╝")
    table.insert(lines, "")
    
    -- Status indicator
    if data.streaming then
        table.insert(lines, "🔴 LIVE")
    else
        table.insert(lines, "⚫ OFFLINE")
    end
    table.insert(lines, "")
    
    -- Output health
    table.insert(lines, "─── OUTPUTS ───")
    for _, output in ipairs(data.outputs) do
        local health_icon = get_health_icon(output.drop_percentage)
        local line = string.format(
            "%s %s: %.2f%% dropped | %d kbps",
            health_icon,
            output.name,
            output.drop_percentage,
            output.bitrate_kbps
        )
        table.insert(lines, line)
    end
    table.insert(lines, "")
    
    -- System metrics
    table.insert(lines, "─── SYSTEM ───")
    table.insert(lines, string.format("GPU: %d%% | %d°C", data.gpu_util, data.gpu_temp))
    
    return table.concat(lines, "\n")
end

function get_health_icon(drop_percentage)
    if drop_percentage < 0.1 then
        return "✅"
    elseif drop_percentage < 1.0 then
        return "⚠️"
    else
        return "❌"
    end
end

-- ═══════════════════════════════════════════════════════════
-- OBS Source Management
-- ═══════════════════════════════════════════════════════════

function create_text_source()
    -- Check if source already exists
    source = obs.obs_get_source_by_name(text_source_name)
    if source ~= nil then
        obs.obs_source_release(source)
        return
    end
    
    -- Create new text source (GDI+)
    local settings = obs.obs_data_create()
    obs.obs_data_set_string(settings, "text", "Initializing telemetry...")
    obs.obs_data_set_string(settings, "font", "Consolas")
    obs.obs_data_set_int(settings, "font_size", 14)
    
    source = obs.obs_source_create("text_gdiplus", text_source_name, settings, nil)
    obs.obs_data_release(settings)
end

function set_dashboard_text(text)
    if source == nil then
        source = obs.obs_get_source_by_name(text_source_name)
        if source == nil then
            return
        end
    end
    
    local settings = obs.obs_data_create()
    obs.obs_data_set_string(settings, "text", text)
    obs.obs_source_update(source, settings)
    obs.obs_data_release(settings)
end
```

---

## 🗄️ Local Storage Schema

### SQLite Database

```sql
-- schema.sql

CREATE TABLE IF NOT EXISTS telemetry_snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    data BLOB NOT NULL,  -- Compressed JSON
    UNIQUE(timestamp)
);

CREATE INDEX idx_timestamp ON telemetry_snapshots(timestamp);

-- Retention policy: delete entries older than 24 hours
CREATE TRIGGER cleanup_old_snapshots
AFTER INSERT ON telemetry_snapshots
BEGIN
    DELETE FROM telemetry_snapshots
    WHERE timestamp < (NEW.timestamp - 86400);  -- 24 hours in seconds
END;

CREATE TABLE IF NOT EXISTS stream_sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    start_time INTEGER NOT NULL,
    end_time INTEGER,
    platform TEXT NOT NULL,
    total_frames INTEGER DEFAULT 0,
    dropped_frames INTEGER DEFAULT 0,
    avg_bitrate REAL DEFAULT 0.0,
    peak_bitrate REAL DEFAULT 0.0,
    avg_gpu_temp REAL DEFAULT 0.0,
    notes TEXT
);

CREATE INDEX idx_session_start ON stream_sessions(start_time);
CREATE INDEX idx_session_platform ON stream_sessions(platform);

CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    severity TEXT NOT NULL CHECK(severity IN ('info', 'warning', 'error', 'critical')),
    message TEXT NOT NULL,
    metric_name TEXT,
    metric_value REAL,
    resolved INTEGER DEFAULT 0
);

CREATE INDEX idx_alerts_timestamp ON alerts(timestamp);
CREATE INDEX idx_alerts_resolved ON alerts(resolved);
```

---

## 🧪 Testing Strategy

### Unit Tests

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_obs_websocket_connection() {
        let client = OBSClient::new("localhost", 4455, "password");
        assert!(client.connect().await.is_ok());
    }

    #[test]
    fn test_vault_encryption() {
        let vault = Vault::new("test_vault.dat");
        vault.store("test_key", "secret_value").unwrap();
        let retrieved = vault.retrieve("test_key").unwrap();
        assert_eq!(retrieved, "secret_value");
    }

    #[test]
    fn test_bridge_json_serialization() {
        let telemetry = Telemetry {
            timestamp: 1234567890,
            // ... populate fields
        };
        let json = serde_json::to_string(&telemetry).unwrap();
        assert!(json.contains("\"timestamp\":1234567890"));
    }
}
```

### Integration Tests

```rust
#[cfg(test)]
mod integration_tests {
    use super::*;

    #[tokio::test]
    async fn test_end_to_end_metrics_flow() {
        // Start mock OBS WebSocket server
        let mock_obs = start_mock_obs_server().await;

        // Initialize components
        let obs_client = OBSClient::new("localhost", 4455, "test");
        let bridge = BridgeWriter::new("test_bridge.json");
        let exporter = GrafanaExporter::new("http://localhost:9090", "test_token");

        // Collect metrics
        let metrics = obs_client.collect_metrics().await.unwrap();

        // Write to bridge
        bridge.write(metrics.clone()).await.unwrap();

        // Verify bridge file
        let bridge_data = std::fs::read_to_string("test_bridge.json").unwrap();
        assert!(bridge_data.contains("\"streaming\":"));

        // Push to cloud (mock)
        exporter.push(metrics).await.unwrap();

        // Cleanup
        mock_obs.stop().await;
    }
}
```

### Performance Benchmarks

```rust
use criterion::{black_box, criterion_group, criterion_main, Criterion};

fn bench_bridge_write(c: &mut Criterion) {
    let bridge = BridgeWriter::new("bench_bridge.json");
    let telemetry = create_sample_telemetry();

    c.bench_function("bridge_write", |b| {
        b.iter(|| {
            bridge.write(black_box(telemetry.clone()))
        })
    });
}

fn bench_vault_encryption(c: &mut Criterion) {
    let vault = Vault::new("bench_vault.dat");

    c.bench_function("vault_encrypt", |b| {
        b.iter(|| {
            vault.store(black_box("key"), black_box("secret"))
        })
    });
}

criterion_group!(benches, bench_bridge_write, bench_vault_encryption);
criterion_main!(benches);
```

---

## 🚀 Build & Deployment

### Build Configuration

```toml
# Cargo.toml

[profile.release]
opt-level = 3
lto = true
codegen-units = 1
strip = true
panic = "abort"

[profile.dev]
opt-level = 0
debug = true

[target.x86_64-pc-windows-msvc]
rustflags = ["-C", "target-cpu=native"]
```

### Windows Installer

Use `cargo-wix` for MSI installer generation:

```bash
cargo install cargo-wix
cargo wix init
cargo wix --nocapture
```

### Portable Release

```bash
# Build release binary
cargo build --release

# Create portable package
mkdir obs-telemetry-bridge-portable
cp target/release/obs-telemetry-bridge.exe obs-telemetry-bridge-portable/
cp lua/obs_dashboard.lua obs-telemetry-bridge-portable/
cp config.example.toml obs-telemetry-bridge-portable/config.toml
cp README.md obs-telemetry-bridge-portable/

# Create ZIP
7z a obs-telemetry-bridge-v1.0.0-win64.zip obs-telemetry-bridge-portable/
```

---

## 📊 Performance Targets

| Metric | Target | Measurement |
|--------|--------|-------------|
| Bridge Write Latency | < 2ms | Time from data ready to file written |
| Lua Read Latency | < 1ms | Time to parse bridge JSON |
| Memory Footprint | < 50MB | Rust process RSS |
| CPU Usage (Idle) | < 1% | When OBS is not streaming |
| CPU Usage (Streaming) | < 3% | During active stream with 5 outputs |
| Network Bandwidth | < 1KB/s | Grafana Cloud push overhead |

---

## 🐛 Debugging & Diagnostics

### Logging Configuration

```rust
use tracing_subscriber::{fmt, EnvFilter};

fn init_logging() {
    let filter = EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| EnvFilter::new("info"));

    fmt()
        .with_env_filter(filter)
        .with_target(false)
        .with_thread_ids(true)
        .with_file(true)
        .with_line_number(true)
        .init();
}
```

### Log Levels

- `TRACE`: Very verbose, includes all data flows
- `DEBUG`: Detailed diagnostic information
- `INFO`: General operational messages (default)
- `WARN`: Warning messages (recoverable errors)
- `ERROR`: Error messages (operation failed)

### Common Issues & Solutions

| Issue | Symptoms | Solution |
|-------|----------|----------|
| OBS WebSocket connection fails | "Connection refused" error | Check OBS WebSocket port and password in config |
| Bridge file not updating | Lua script shows old data | Verify Rust engine is running and has write permissions |
| Grafana Cloud push fails | "401 Unauthorized" in logs | Check API token in encrypted vault |
| High CPU usage | System sluggish during stream | Increase `metrics_interval` in config |
| GPU metrics unavailable | No GPU data in dashboard | Verify NVIDIA GPU and drivers installed |

---

## 🔜 Future Enhancements

### Phase 2 Optimizations
- Shared memory bridge (replace JSON file for sub-millisecond latency)
- Multi-threaded metric collection (parallel OBS + system metrics)
- Binary protocol for Lua (faster than JSON parsing)

### Phase 3 Features
- Web-based configuration UI (Tauri or Dioxus)
- System tray icon with quick status
- Alert system (push notifications via Pushover/Telegram)
- Stream session analytics (track performance trends)

### Phase 4 Scaling
- Multi-client support (monitor multiple streamers from one dashboard)
- AWS Lambda for metric processing
- CloudWatch Logs integration
- S3 archival for historical data

---

## 📚 Additional Resources

### OBS Development
- [OBS WebSocket Documentation](https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md)
- [OBS Lua Scripting Guide](https://obsproject.com/docs/scripting.html)

### Rust Crates
- [tokio](https://docs.rs/tokio): Async runtime
- [obws](https://docs.rs/obws): OBS WebSocket client
- [nvml-wrapper](https://docs.rs/nvml-wrapper): NVIDIA GPU monitoring
- [opentelemetry](https://docs.rs/opentelemetry): Telemetry SDK

### Grafana & Observability
- [Grafana Cloud OTLP](https://grafana.com/docs/grafana-cloud/send-data/otlp/)
- [OpenTelemetry Specification](https://opentelemetry.io/docs/specs/otel/)
- [Prometheus Metric Types](https://prometheus.io/docs/concepts/metric_types/)

---

**Document Version**: 1.0.0  
**Last Updated**: 2024-02-11  
**Status**: Phase 1 Planning Complete
