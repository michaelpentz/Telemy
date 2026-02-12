use crate::config::{Config, ThemeConfig};
use crate::model::TelemetryFrame;
use crate::security::Vault;
use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        Query,
        State,
    },
    http::{HeaderMap, StatusCode},
    response::{Html, IntoResponse},
    routing::{get, post},
    Form, Router,
};
use base64::{engine::general_purpose, Engine as _};
use serde::Deserialize;
use std::{
    collections::HashMap,
    net::SocketAddr,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::net::TcpListener;
use tokio::sync::watch;

#[derive(Clone)]
#[allow(dead_code)]
struct ServerState {
    token: String,
    rx: watch::Receiver<TelemetryFrame>,
    theme: ThemeConfig,
    vault: Arc<Mutex<Vault>>,
    grafana_configured: Arc<Mutex<bool>>,
    output_names: HashMap<String, String>,
}

#[derive(Deserialize)]
struct SetupForm {
    endpoint: String,
    instance_id: String,
    api_token: String,
}

pub async fn start(
    addr: SocketAddr,
    token: String,
    rx: watch::Receiver<TelemetryFrame>,
    mut shutdown_rx: watch::Receiver<bool>,
    theme: ThemeConfig,
    vault: Arc<Mutex<Vault>>,
    grafana_configured: bool,
    output_names: HashMap<String, String>,
) -> Result<(), Box<dyn std::error::Error>> {
    let state = Arc::new(ServerState {
        token,
        rx,
        theme,
        vault,
        grafana_configured: Arc::new(Mutex::new(grafana_configured)),
        output_names,
    });

    let app = Router::new()
        .route("/health", get(health_check))
        .route("/obs", get(obs_page))
        .route("/ws", get(ws_handler))
        .route("/setup", get(setup_page))
        .route("/setup", post(setup_submit))
        .route("/output-names", get(get_output_names))
        .route("/output-names", post(save_output_names))
        .with_state(state);

    let listener = TcpListener::bind(addr).await?;
    axum::serve(listener, app)
        .with_graceful_shutdown(async move {
            let _ = shutdown_rx.changed().await;
        })
        .await?;

    Ok(())
}

async fn obs_page(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    // Support both Authorization header (for API access) and query param (for browser/Dock access)
    if !is_token_valid(&headers, &query.0, &state.token) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let css = theme_css(&state.theme);
    
    let html = r##"<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>OBS Telemetry</title>
  <style>
    :root {
      {{THEME_VARS}}
    }
    body { margin: 0; font-family: var(--font); background: var(--bg); color: #e6f0ff; }
    .wrap { padding: 12px 16px; }
    .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .badge { padding: 4px 8px; background: var(--panel); border-radius: 4px; font-size: 12px; border: 1px solid var(--line); }
    .grid { display: grid; grid-template-columns: 1fr; gap: 8px; margin-top: 10px; }
    .output { background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 8px 10px; }
    .output-inactive { background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 8px 10px; opacity: 0.5; }
    .name { font-size: 13px; margin-bottom: 6px; }
    .bar { height: 8px; background: #0f141c; border: 1px solid var(--line); border-radius: 4px; overflow: hidden; }
    .fill { height: 100%; background: var(--good); width: 0%; }
    canvas { width: 100%; height: 140px; background: #0f141c; border: 1px solid var(--line); border-radius: 6px; }
    .muted { color: var(--muted); }
    .edit-btn { cursor: pointer; color: var(--muted); font-size: 11px; text-decoration: underline; margin-left: 10px; }
    .edit-btn:hover { color: var(--good); }
    .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); z-index: 1000; }
    .modal-content { background: var(--panel); margin: 50px auto; padding: 20px; width: 90%; max-width: 600px; border: 1px solid var(--line); border-radius: 8px; max-height: 80vh; overflow-y: auto; }
    .modal-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
    .modal-title { font-size: 16px; font-weight: bold; }
    .close-btn { cursor: pointer; font-size: 20px; color: var(--muted); }
    .close-btn:hover { color: var(--bad); }
    .name-row { display: flex; gap: 10px; margin-bottom: 10px; align-items: center; }
    .name-row input { flex: 1; background: var(--bg); border: 1px solid var(--line); color: #e6f0ff; padding: 6px; border-radius: 4px; }
    .name-row .id-label { width: 150px; font-size: 11px; color: var(--muted); word-break: break-all; }
    .save-btn { background: var(--good); color: #0b0e12; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; font-weight: bold; margin-top: 10px; }
    .save-btn:hover { opacity: 0.9; }
    .add-btn { background: var(--panel); color: var(--good); border: 1px solid var(--good); padding: 6px 12px; border-radius: 4px; cursor: pointer; font-size: 12px; margin-bottom: 10px; }
    .test-mode { border: 1px solid var(--warn); color: var(--warn); font-weight: bold; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="row">
      <div class="badge" id="status">DISCONNECTED</div>
      <div class="badge" id="time">--</div>
      <div class="badge" id="health">Health: --</div>
      <div class="badge" id="obs">OBS: --</div>
      <div class="badge" id="testmode" style="display:none;" class="test-mode">TEST MODE</div>
      <div class="badge" id="sys">SYS: --</div>
      <div class="badge" id="net">NET: --</div>
    </div>
    <div class="grid" id="outputs"></div>
    <div style="margin-top:10px;"><canvas id="graph" width="600" height="140"></canvas></div>
    <div style="margin-top:10px;"><span class="edit-btn" id="editNamesBtn">Edit Output Names</span></div>
    <div class="muted" style="margin-top:6px; font-size:11px;">Graph shows overall health (1.0 = best)</div>
  </div>
  
  <!-- Modal for editing output names -->
  <div class="modal" id="nameModal">
    <div class="modal-content">
      <div class="modal-header">
        <span class="modal-title">Edit Output Names</span>
        <span class="close-btn" id="closeModal">&times;</span>
      </div>
      <div id="nameEditor"></div>
      <button class="save-btn" id="saveNames">Save Changes</button>
      <div id="saveMsg" style="margin-top:10px; font-size:13px;"></div>
    </div>
  </div>
  
  <script>
    // Default pretty names for known outputs
    const defaultNames = {
      'adv_stream': 'Main Stream',
      'adv_file_output': 'Recording',
      'virtualcam_output': 'Virtual Camera'
    };
    
    // Output name mappings - will be loaded dynamically
    let outputNameMap = {};
    
    const params = new URLSearchParams(window.location.search);
    const token = params.get('token');
    const ws = new WebSocket(`ws://${window.location.host}/ws?token=${token}`);
    
    // Load output names from server
    async function loadOutputNames() {
      try {
        const res = await fetch(`/output-names?token=${token}`);
        if (res.ok) {
          outputNameMap = await res.json();
        }
      } catch (e) {
        console.error('Failed to load output names:', e);
      }
    }
    
    // Load names on startup
    loadOutputNames();

    const statusEl = document.getElementById("status");
    const timeEl = document.getElementById("time");
    const healthEl = document.getElementById("health");
    const obsEl = document.getElementById("obs");
    const testModeEl = document.getElementById("testmode");
    const sysEl = document.getElementById("sys");
    const netEl = document.getElementById("net");
    const outputsEl = document.getElementById("outputs");
    const canvas = document.getElementById("graph");
    const ctx = canvas.getContext("2d");
    const values = [];
    const maxPoints = 120;

    function healthColor(v) {
      if (v >= 0.95) return "var(--good)";
      if (v >= 0.90) return "var(--warn)";
      return "var(--bad)";
    }

    function draw() {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      
      // Draw grid lines
      ctx.strokeStyle = "#1f2a3a";
      ctx.lineWidth = 1;
      ctx.beginPath();
      // 0.5 line (50%)
      ctx.moveTo(30, canvas.height / 2);
      ctx.lineTo(canvas.width, canvas.height / 2);
      // 0.0 line (0%)
      ctx.moveTo(30, canvas.height - 1);
      ctx.lineTo(canvas.width, canvas.height - 1);
      // 1.0 line (100%)
      ctx.moveTo(30, 1);
      ctx.lineTo(canvas.width, 1);
      ctx.stroke();
      
      // Draw labels
      ctx.fillStyle = "#8da3c1";
      ctx.font = "10px Arial";
      ctx.textAlign = "right";
      ctx.textBaseline = "middle";
      ctx.fillText("100%", 25, 6);
      ctx.fillText("50%", 25, canvas.height / 2);
      ctx.fillText("0%", 25, canvas.height - 6);
      
      // Draw graph
      ctx.strokeStyle = "#33d17a";
      ctx.lineWidth = 2;
      ctx.beginPath();
      
      const graphWidth = canvas.width - 30;
      values.forEach((v, i) => {
        const x = 30 + (i / Math.max(1, maxPoints - 1)) * graphWidth;
        const y = canvas.height - (v * canvas.height);
        // Clamp y to canvas bounds
        const clampedY = Math.max(0, Math.min(canvas.height, y));
        
        if (i === 0) ctx.moveTo(x, clampedY); else ctx.lineTo(x, clampedY);
      });
      ctx.stroke();
    }

    function renderOutputs(outputs) {
      outputsEl.innerHTML = "";
      outputs.forEach(o => {
        // Check if output is active
        const isActive = o.bitrate_kbps > 0 || o.fps > 0;
        
        // Get pretty name from config map, then defaults, then original
        let displayName = outputNameMap[o.name] || defaultNames[o.name] || o.name;
        
        // Add indicator for special outputs when inactive
        if (!isActive && (o.name === "adv_file_output" || o.name === "virtualcam_output")) {
          displayName += " (Inactive)";
        }
        
        const box = document.createElement("div");
        box.className = isActive ? "output" : "output-inactive";
        box.dataset.outputId = o.name; // Store real ID for editing
        
        const name = document.createElement("div");
        name.className = "name";
        name.textContent = `${displayName} | ${o.bitrate_kbps} kbps | ${o.fps.toFixed(0)} fps | ${(o.drop_pct*100).toFixed(2)}% drop | ${o.encoding_lag_ms.toFixed(1)} ms lag`;
        
        const bar = document.createElement("div");
        bar.className = "bar";
        const fill = document.createElement("div");
        fill.className = "fill";
        const health = 1 - o.drop_pct;
        fill.style.width = `${Math.max(0, Math.min(100, health*100))}%`;
        fill.style.background = healthColor(health);
        bar.appendChild(fill);
        box.appendChild(name);
        box.appendChild(bar);
        outputsEl.appendChild(box);
      });
    }

    ws.onopen = () => { statusEl.textContent = "CONNECTED"; };
    ws.onclose = () => { statusEl.textContent = "DISCONNECTED"; };
    ws.onmessage = (event) => {
      const data = JSON.parse(event.data);
      timeEl.textContent = new Date(data.ts * 1000).toLocaleTimeString();
      healthEl.textContent = `Health: ${(data.health*100).toFixed(1)}%`;
      healthEl.style.borderColor = healthColor(data.health);
      obsEl.textContent = `OBS: ${data.obs.streaming ? "LIVE" : "IDLE"} | dropped ${data.obs.total_dropped_frames}`;
      
      // Update Test Mode badge
      if (data.obs.test_mode) {
        testModeEl.style.display = "block";
      } else {
        testModeEl.style.display = "none";
      }
      
      sysEl.textContent = `SYS: CPU ${data.system.cpu_percent.toFixed(0)}% | MEM ${data.system.mem_percent.toFixed(0)}% | GPU ${data.system.gpu_percent ?? 0}%`;
      netEl.textContent = `NET: UP ${data.network.upload_mbps.toFixed(1)} Mb/s | LAT ${data.network.latency_ms.toFixed(0)} ms`;
      values.push(data.health);
      if (values.length > maxPoints) values.shift();
      draw();
      renderOutputs(data.outputs);
    };
    
    // Modal functionality for editing output names
    const modal = document.getElementById("nameModal");
    const editBtn = document.getElementById("editNamesBtn");
    const closeBtn = document.getElementById("closeModal");
    const nameEditor = document.getElementById("nameEditor");
    const saveBtn = document.getElementById("saveNames");
    const saveMsg = document.getElementById("saveMsg");
    
    editBtn.onclick = () => {
      modal.style.display = "block";
      populateNameEditor();
    };
    
    closeBtn.onclick = () => {
      modal.style.display = "none";
    };
    
    window.onclick = (e) => {
      if (e.target === modal) modal.style.display = "none";
    };
    
    function populateNameEditor() {
      nameEditor.innerHTML = "";
      
      // Add currently visible outputs
      const currentOutputs = Array.from(document.querySelectorAll(".output, .output-inactive"));
      const seenIds = new Set();
      
      currentOutputs.forEach(box => {
        // Use the real ID stored in dataset
        const id = box.dataset.outputId;
        
        if (id && !seenIds.has(id) && !defaultNames[id]) {
          seenIds.add(id);
          const currentName = outputNameMap[id] || id;
          addNameRow(id, currentName);
        }
      });
      
      if (seenIds.size === 0) {
        nameEditor.innerHTML = "<div class=\"muted\">No custom outputs detected yet. Start streaming to see outputs.</div>";
      }
    }
    
    function addNameRow(id, name) {
      const row = document.createElement("div");
      row.className = "name-row";
      row.innerHTML = `
        <span class="id-label">${id}</span>
        <input type="text" data-id="${id}" value="${name}" placeholder="Display name">
      `;
      nameEditor.appendChild(row);
    }
    
    saveBtn.onclick = async () => {
      const inputs = nameEditor.querySelectorAll("input");
      const mappings = {};
      
      inputs.forEach(input => {
        const id = input.getAttribute("data-id");
        const name = input.value.trim();
        if (name && name !== id) {
          mappings[id] = name;
        }
      });
      
      try {
        const res = await fetch("/output-names", {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "Authorization": "Bearer " + token
          },
          body: JSON.stringify(mappings)
        });
        
        if (res.ok) {
          saveMsg.textContent = "Saved! Refresh the page to see changes.";
          saveMsg.style.color = "var(--good)";
          setTimeout(() => {
            modal.style.display = "none";
            location.reload();
          }, 1500);
        } else {
          saveMsg.textContent = "Failed to save.";
          saveMsg.style.color = "var(--bad)";
        }
      } catch (err) {
        saveMsg.textContent = "Error: " + err.message;
        saveMsg.style.color = "var(--bad)";
      }
    };
  </script>
</body>
</html>"##;

    let html = html.replace("{{THEME_VARS}}", &css);
    Html(html).into_response()
}

async fn setup_page(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let configured = *state.grafana_configured.lock().unwrap();
    let status_html = if configured {
        r#"<div class="status status-ok">Grafana Cloud: Configured</div>"#
    } else {
        r#"<div class="status status-off">Grafana Cloud: Not Configured</div>"#
    };
    let css = theme_css(&state.theme);

    let html = format!(
        r#"<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Telemy - Grafana Setup</title>
  <style>
    :root {{ {css} }}
    body {{ margin:0; font-family:var(--font); background:var(--bg); color:#e6f0ff; }}
    .wrap {{ max-width:480px; margin:40px auto; padding:0 16px; }}
    h1 {{ font-size:20px; margin-bottom:4px; }}
    .sub {{ color:var(--muted); font-size:13px; margin-bottom:20px; }}
    .status {{ padding:8px 12px; border-radius:6px; margin-bottom:20px; font-size:13px; }}
    .status-ok {{ background:#1a2e1a; border:1px solid var(--good); color:var(--good); }}
    .status-off {{ background:#2e1a1a; border:1px solid var(--bad); color:var(--bad); }}
    label {{ display:block; font-size:13px; color:var(--muted); margin-bottom:4px; margin-top:14px; }}
    input {{ width:100%; box-sizing:border-box; padding:8px 10px; background:var(--panel);
             border:1px solid var(--line); border-radius:4px; color:#e6f0ff; font-size:14px;
             font-family:var(--font); }}
    input:focus {{ outline:none; border-color:var(--good); }}
    button {{ margin-top:20px; padding:10px 20px; background:var(--good); color:#0b0e12;
              border:none; border-radius:4px; font-size:14px; font-weight:bold; cursor:pointer; }}
    button:hover {{ opacity:0.9; }}
    .msg {{ margin-top:14px; padding:8px 12px; border-radius:6px; font-size:13px; display:none; }}
    .msg-ok {{ background:#1a2e1a; border:1px solid var(--good); color:var(--good); display:block; }}
    .msg-err {{ background:#2e1a1a; border:1px solid var(--bad); color:var(--bad); display:block; }}
    .note {{ color:var(--muted); font-size:12px; margin-top:16px; }}
    .help {{ color:var(--muted); font-size:11px; margin-top:2px; }}
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Grafana Cloud Setup</h1>
    <div class="sub">Push stream telemetry to Grafana Cloud via OTLP</div>
    {status_html}
    <div id="msg" class="msg"></div>
    <form id="setupForm">
      <label for="endpoint">OTLP Endpoint</label>
      <input id="endpoint" name="endpoint" type="url" required
             placeholder="https://otlp-gateway-prod-us-east-0.grafana.net/otlp" />
      <div class="help">Found in Grafana Cloud &rarr; OpenTelemetry &rarr; Configure</div>

      <label for="instance_id">Instance ID</label>
      <input id="instance_id" name="instance_id" type="text" required
             placeholder="123456" />
      <div class="help">Your Grafana Cloud stack instance number</div>

      <label for="api_token">API Token</label>
      <input id="api_token" name="api_token" type="password" required
             placeholder="glc_eyJ..." />
      <div class="help">Generate under Security &rarr; API Keys with MetricsPublisher role</div>

      <button type="submit">Save &amp; Enable</button>
    </form>
    <div class="note">After saving, restart Telemy for the exporter to begin pushing metrics.</div>
  </div>
  <script>
    document.getElementById("setupForm").addEventListener("submit", async (e) => {{
      e.preventDefault();
      const msg = document.getElementById("msg");
      const data = new URLSearchParams(new FormData(e.target));
      const params = new URLSearchParams(window.location.search);
      const token = params.get("token");
      try {{
        const res = await fetch("/setup", {{
          method: "POST",
          headers: {{ 
            "Content-Type": "application/x-www-form-urlencoded",
            "Authorization": "Bearer " + token
          }},
          body: data,
        }});
        const text = await res.text();
        msg.textContent = text;
        msg.className = res.ok ? "msg msg-ok" : "msg msg-err";
      }} catch (err) {{
        msg.textContent = "Request failed: " + err.message;
        msg.className = "msg msg-err";
      }}
    }});
  </script>
</body>
</html>"#
    );

    Html(html).into_response()
}

async fn setup_submit(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Form(form): Form<SetupForm>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized".to_string()).into_response();
    }

    let endpoint = form.endpoint.trim().to_string();
    let instance_id = form.instance_id.trim().to_string();
    let api_token = form.api_token.trim().to_string();

    if endpoint.is_empty() || instance_id.is_empty() || api_token.is_empty() {
        return (
            StatusCode::BAD_REQUEST,
            "All fields are required".to_string(),
        )
            .into_response();
    }

    // Construct Basic auth: "Basic base64(instance_id:api_token)"
    let credentials = format!("{}:{}", instance_id, api_token);
    let encoded = general_purpose::STANDARD.encode(credentials.as_bytes());
    let auth_value = format!("Basic {}", encoded);

    // Store encrypted in DPAPI vault
    let vault_key = "grafana_auth";
    {
        let mut vault = state.vault.lock().unwrap();
        if let Err(e) = vault.store(vault_key, &auth_value) {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to store credentials: {}", e),
            )
                .into_response();
        }
    }

    // Load config -> update grafana section -> save
    let config_result: Result<(), Box<dyn std::error::Error>> = (|| {
        let mut config = Config::load()?;
        config.grafana.enabled = true;
        config.grafana.endpoint = Some(endpoint);
        config.grafana.auth_value_key = Some(vault_key.to_string());
        config.save()?;
        Ok(())
    })();

    match config_result {
        Ok(()) => {
            *state.grafana_configured.lock().unwrap() = true;
            (
                StatusCode::OK,
                "Grafana Cloud configured. Restart Telemy to begin pushing metrics.".to_string(),
            )
                .into_response()
        }
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to update config: {}", e),
        )
            .into_response(),
    }
}

async fn ws_handler(
    State(state): State<Arc<ServerState>>,
    ws: WebSocketUpgrade,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let rx = state.rx.clone();
    ws.on_upgrade(move |socket| handle_socket(socket, rx))
}

async fn handle_socket(mut socket: WebSocket, rx: watch::Receiver<TelemetryFrame>) {
    let mut ticker = tokio::time::interval(Duration::from_millis(500));

    loop {
        tokio::select! {
            _ = ticker.tick() => {
                let frame = rx.borrow().clone();
                let payload = serde_json::json!({
                    "ts": frame.timestamp_unix,
                    "health": frame.health,
                    "obs": frame.obs,
                    "system": frame.system,
                    "network": frame.network,
                    "outputs": frame.streams,
                });
                if socket.send(Message::Text(payload.to_string())).await.is_err() {
                    break;
                }
            }
            msg = socket.recv() => {
                match msg {
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {}
                }
            }
        }
    }
}

fn is_token_valid(headers: &HeaderMap, query: &HashMap<String, String>, token: &str) -> bool {
    // First check Authorization header (preferred for API access)
    // Format: "Bearer <token>"
    if let Some(auth_header) = headers.get("authorization") {
        if let Ok(auth_str) = auth_header.to_str() {
            if auth_str.starts_with("Bearer ") {
                let provided_token = &auth_str[7..]; // Skip "Bearer "
                return provided_token == token;
            }
        }
    }
    
    // Fall back to query parameter (for browser/Dock access)
    query.get("token").map(|t| t == token).unwrap_or(false)
}

async fn health_check() -> impl IntoResponse {
    (
        StatusCode::OK,
        axum::Json(serde_json::json!({
            "status": "healthy",
            "timestamp": std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_secs()
        }))
    )
}

fn theme_css(theme: &ThemeConfig) -> String {
    format!(
        "--font: {}; --bg: {}; --panel: {}; --muted: {}; --good: {}; --warn: {}; --bad: {}; --line: {};",
        theme.font_family,
        theme.bg,
        theme.panel,
        theme.muted,
        theme.good,
        theme.warn,
        theme.bad,
        theme.line
    )
}

#[derive(Deserialize)]
struct OutputNamesPayload {
    #[serde(flatten)]
    names: HashMap<String, String>,
}

async fn get_output_names(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    // Load current config to get latest names
    match Config::load() {
        Ok(config) => {
            (StatusCode::OK, axum::Json(config.output_names)).into_response()
        }
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to load config: {}", e),
        )
            .into_response(),
    }
}

async fn save_output_names(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    axum::Json(payload): axum::Json<OutputNamesPayload>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    // Load current config
    let mut config = match Config::load() {
        Ok(cfg) => cfg,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to load config: {}", e),
            )
                .into_response();
        }
    };

    // Merge new names with existing
    for (id, name) in payload.names {
        if name.trim().is_empty() {
            config.output_names.remove(&id);
        } else {
            config.output_names.insert(id, name);
        }
    }

    // Save config
    match config.save() {
        Ok(()) => (StatusCode::OK, "Output names saved").into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to save config: {}", e),
        )
            .into_response(),
    }
}
