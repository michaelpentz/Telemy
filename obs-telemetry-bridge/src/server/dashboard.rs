use super::{html_escape, is_token_valid, theme_css, QueryTokenPolicy, ServerState};
use axum::{
    extract::{Query, State},
    http::{HeaderMap, StatusCode},
    response::{Html, IntoResponse},
};
use std::collections::HashMap;
use std::sync::Arc;

pub(super) async fn obs_page(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    // Support both Authorization header (for API access) and query param (for browser/Dock access)
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
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
    body {
      margin: 0;
      font-family: var(--font);
      background:
        radial-gradient(circle at 10% 0%, rgba(51,209,122,0.09), transparent 42%),
        radial-gradient(circle at 100% 0%, rgba(246,211,45,0.07), transparent 34%),
        linear-gradient(180deg, #07090d 0%, var(--bg) 38%, #090d14 100%);
      color: #e6f0ff;
    }
    .wrap { max-width: 1180px; margin: 0 auto; padding: 18px 16px 24px; }
    .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .badge {
      padding: 7px 10px;
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0));
      border-radius: 999px;
      font-size: 12px;
      border: 1px solid var(--line);
      box-shadow: inset 0 0 0 1px rgba(255,255,255,0.01);
    }
    .shell { display: grid; gap: 12px; }
    .hero {
      background: linear-gradient(180deg, rgba(255,255,255,0.025), rgba(255,255,255,0.01));
      border: 1px solid var(--line);
      border-radius: 14px;
      padding: 14px;
      box-shadow: 0 14px 32px rgba(0,0,0,0.24);
    }
    .hero-header { display:flex; gap:12px; justify-content:space-between; align-items:flex-start; flex-wrap:wrap; }
    .hero-title { font-size: 18px; font-weight: 700; letter-spacing: 0.02em; }
    .hero-sub { color: var(--muted); font-size: 12px; margin-top: 4px; }
    .hero-right { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
    .link-badge { text-decoration:none; color:inherit; cursor:pointer; }
    .grid { display: grid; grid-template-columns: 1fr; gap: 8px; }
    .panel-card {
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0.005));
      border: 1px solid var(--line);
      border-radius: 12px;
      padding: 12px;
    }
    .section-head { display:flex; justify-content:space-between; align-items:center; gap:8px; margin-bottom:8px; }
    .section-title { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.08em; }
    .output { background: rgba(255,255,255,0.015); border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px; }
    .output-inactive { background: rgba(255,255,255,0.01); border: 1px solid var(--line); border-radius: 8px; padding: 8px 10px; opacity: 0.5; }
    .name { font-size: 13px; margin-bottom: 6px; }
    .bar { height: 8px; background: #0f141c; border: 1px solid var(--line); border-radius: 4px; overflow: hidden; }
    .fill { height: 100%; background: var(--good); width: 0%; }
    canvas { width: 100%; height: 140px; background: #0d121a; border: 1px solid var(--line); border-radius: 8px; }
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
    .add-btn { background: rgba(255,255,255,0.015); color: var(--good); border: 1px solid var(--good); padding: 7px 12px; border-radius: 999px; cursor: pointer; font-size: 12px; margin-bottom: 10px; }
    .add-btn:hover { background: rgba(51,209,122,0.08); }
    .test-mode { border: 1px solid var(--warn); color: var(--warn); font-weight: bold; }
    .rec-badge { border: 1px solid var(--bad); color: var(--bad); font-weight: bold; }
    .toggle-row { display: flex; align-items: center; gap: 6px; margin-top: 10px; font-size: 11px; color: var(--muted); }
    .toggle-row input { accent-color: var(--good); }
    .stats-row { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 8px; }
    .stat { padding: 6px 8px; background: rgba(255,255,255,0.015); border-radius: 8px; font-size: 11px; border: 1px solid var(--line); color: var(--muted); }
    .dashboard-grid { display:grid; grid-template-columns: 1.15fr 0.85fr; gap:12px; align-items:start; }
    .summary-grid { display:grid; grid-template-columns: repeat(3, minmax(0,1fr)); gap:10px; }
    .summary-box { border:1px solid var(--line); border-radius:10px; padding:10px; background: rgba(255,255,255,0.015); }
    .summary-label { color: var(--muted); font-size: 10px; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 6px; }
    .summary-value { font-size: 12px; line-height: 1.45; }
    .details-shell { margin-top: 10px; border: 1px solid var(--line); border-radius: 10px; background: rgba(255,255,255,0.01); overflow: hidden; }
    .details-shell > summary { cursor: pointer; list-style: none; padding: 10px 12px; color: var(--muted); font-size: 12px; user-select: none; }
    .details-shell > summary::-webkit-details-marker { display: none; }
    .details-shell > summary::before { content: "▸ "; color: var(--good); }
    .details-shell[open] > summary::before { content: "▾ "; }
    .details-content { padding: 0 12px 12px; }
    .aegis-controls { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
    .aegis-actions { margin-top: 8px; }
    .toolbar-row { display:flex; justify-content:space-between; gap:8px; align-items:center; flex-wrap:wrap; margin-top:8px; }
    .toolbar-links { display:flex; align-items:center; gap:2px; flex-wrap:wrap; }
    @media (max-width: 860px) {
      .dashboard-grid { grid-template-columns: 1fr; }
      .summary-grid { grid-template-columns: 1fr; }
      .hero-header { align-items: stretch; }
      .hero-right { width: 100%; }
      .hero-right .badge, .hero-right .link-badge { width: fit-content; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="shell">
      <div class="hero">
        <div class="hero-header">
          <div>
            <div class="hero-title">Telemy Control Surface</div>
            <div class="hero-sub">Legacy dashboard shell with v0.0.3 Aegis controls and live status plumbing</div>
          </div>
          <div class="hero-right">
            <div class="badge" id="status">DISCONNECTED</div>
            <div class="badge" id="time">--</div>
            <a href="/settings?token={{TOKEN}}" class="badge link-badge">Settings</a>
          </div>
        </div>
        <div class="row" style="margin-top:10px;">
          <div class="badge" id="health">Health: --</div>
          <div class="badge" id="obs">OBS: --</div>
          <div class="badge" id="testmode" style="display:none;" class="test-mode">STUDIO MODE</div>
          <div class="badge rec-badge" id="recbadge" style="display:none;">REC</div>
          <div class="badge" id="sys">SYS: --</div>
          <div class="badge" id="net">NET: --</div>
          <div class="badge" id="aegis">AEGIS: --</div>
        </div>
      </div>

      <div class="dashboard-grid">
        <div class="panel-card">
          <div class="section-head">
            <div class="section-title">Live Summary</div>
            <div class="muted" style="font-size:11px;">Connection, system, and main stream info</div>
          </div>
          <div class="summary-grid">
            <div class="summary-box">
              <div class="summary-label">Connection</div>
              <div class="summary-value" id="summaryConn">OBS: --<br>Latency: --<br>Aegis: --</div>
            </div>
            <div class="summary-box">
              <div class="summary-label">System</div>
              <div class="summary-value" id="summarySystem">CPU: --<br>RAM: --<br>GPU/VRAM: --</div>
            </div>
            <div class="summary-box">
              <div class="summary-label">Main Stream / Encoder</div>
              <div class="summary-value" id="summaryMain">Bitrate: --<br>Drops: --<br>Lag/FPS: --</div>
            </div>
          </div>
          <details class="details-shell" id="diagDetails">
            <summary>Expanded Diagnostics</summary>
            <div class="details-content">
              <div class="section-head" style="margin-top:8px;">
                <div class="section-title">OBS Health Trend</div>
                <div class="muted" style="font-size:11px;">Graph shows overall health (1.0 = best)</div>
              </div>
              <canvas id="graph" width="600" height="140"></canvas>
              <div class="stats-row" id="statsRow">
                <div class="stat" id="statDisk">Disk: --</div>
                <div class="stat" id="statRender">Render missed: --</div>
                <div class="stat" id="statOutput">Encoder skipped: --</div>
                <div class="stat" id="statFps">FPS: --</div>
              </div>
            </div>
          </details>
        </div>

        <div class="panel-card">
          <div class="section-head">
            <div class="section-title">Aegis Relay Controls</div>
          </div>
          <div class="aegis-controls">
            <button class="add-btn" id="aegisStartBtn" style="margin-bottom:0;">Aegis Start</button>
            <button class="add-btn" id="aegisStopBtn" style="margin-bottom:0;">Aegis Stop</button>
            <span class="edit-btn" id="refreshAegisBtn" style="margin-left:0;">Refresh Aegis</span>
          </div>
          <div class="row aegis-actions" style="margin-top:8px;">
            <input id="ipcSceneName" type="text" value="BRB" placeholder="Scene name"
              style="background:var(--bg); border:1px solid var(--line); color:#e6f0ff; padding:7px 9px; border-radius:8px; min-width:110px;">
            <input id="ipcSceneReason" type="text" value="manual_debug" placeholder="Reason"
              style="background:var(--bg); border:1px solid var(--line); color:#e6f0ff; padding:7px 9px; border-radius:8px; min-width:130px;">
            <label style="display:flex; align-items:center; gap:6px; color:#9cb0d0; font-size:12px;">
              <input id="ipcAllowEmptyScene" type="checkbox">
              empty (debug)
            </label>
            <button class="add-btn" id="ipcSwitchSceneBtn" style="margin-bottom:0;">IPC Switch Scene</button>
          </div>
          <div class="stats-row aegis-actions">
            <div class="stat" id="aegisActionMsg" style="min-width:220px;">Aegis action: idle</div>
            <div class="stat" id="ipcStatusMsg" style="min-width:280px;">IPC: --</div>
          </div>
          <div class="toolbar-row">
            <div class="toggle-row" style="margin-top:0;">
              <input type="checkbox" id="hideInactive" /> <label for="hideInactive">Hide inactive outputs</label>
            </div>
            <div class="toolbar-links">
              <span class="edit-btn" id="editNamesBtn" style="margin-left:0;">Edit Output Names</span>
            </div>
          </div>
        </div>
      </div>

      <details class="panel-card details-shell" id="outputsDetails" open>
        <summary>Outputs</summary>
        <div class="details-content">
          <div class="section-head">
            <div class="section-title">Outputs</div>
          </div>
          <div class="grid" id="outputs"></div>
        </div>
      </details>
    </div>
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
        const res = await fetch(`/output-names`, {
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        if (res.ok) {
          outputNameMap = await res.json();
        }
      } catch (e) {
        console.error('Failed to load output names:', e);
      }
    }

    // Load names on startup
    loadOutputNames();

    async function loadAegisStatus(refresh = false) {
      try {
        const url = refresh ? "/aegis/status?refresh=1" : "/aegis/status";
        const res = await fetch(url, {
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        if (!res.ok) return;
        const data = await res.json();
        const session = data.session;
        if (!data.enabled) {
          aegisEl.textContent = "AEGIS: disabled";
          aegisEl.style.borderColor = "var(--line)";
          return;
        }
        if (!session) {
          aegisEl.textContent = "AEGIS: none";
          aegisEl.style.borderColor = "var(--line)";
          return;
        }
        const region = session.region ? ` @ ${session.region}` : "";
        aegisEl.textContent = `AEGIS: ${session.status}${region}`;
        aegisEl.style.borderColor = session.status === "active" ? "var(--good)" : "var(--warn)";
      } catch (e) {
        aegisEl.textContent = "AEGIS: error";
        aegisEl.style.borderColor = "var(--bad)";
      }
    }

    async function aegisAction(path) {
      try {
        aegisActionMsg.textContent = `Aegis action: ${path === "/aegis/start" ? "starting..." : "stopping..."}`;
        const res = await fetch(path, {
          method: "POST",
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        const data = await res.json().catch(() => ({}));
        if (!res.ok) {
          aegisActionMsg.textContent = `Aegis action error: ${data.error || res.status}`;
          return;
        }
        aegisActionMsg.textContent = `Aegis action: ${data.message || "ok"}`;
        await loadAegisStatus(true);
      } catch (e) {
        aegisActionMsg.textContent = `Aegis action error: ${e.message}`;
      }
    }

    async function loadIpcStatus() {
      try {
        const res = await fetch("/ipc/status", {
          headers: {
            "Authorization": "Bearer " + token
          }
        });
        if (!res.ok) {
          ipcStatusMsg.textContent = `IPC: status error (${res.status})`;
          return;
        }
        const data = await res.json();
        const conn = data.session_connected ? "connected" : "disconnected";
        const pending = Number(data.pending_switch_count || 0);
        let tail = "";
        if (data.last_switch_result) {
          const r = data.last_switch_result;
          tail = ` | last=${r.status}${r.error ? ` (${r.error})` : ""}`;
        } else if (data.last_switch_request) {
          const r = data.last_switch_request;
          tail = ` | queued=${r.scene_name}`;
        }
        ipcStatusMsg.textContent = `IPC: ${conn} | pending=${pending}${tail}`;
      } catch (e) {
        ipcStatusMsg.textContent = `IPC: status error (${e.message})`;
      }
    }

    async function ipcSwitchScene() {
      try {
        const sceneName = (ipcSceneNameEl.value || "").trim();
        const reason = (ipcSceneReasonEl.value || "").trim();
        const allowEmpty = !!(ipcAllowEmptySceneEl && ipcAllowEmptySceneEl.checked);
        if (!sceneName && !allowEmpty) {
          aegisActionMsg.textContent = "Aegis action error: scene name required";
          return;
        }
        const displayScene = sceneName || "<empty>";
        aegisActionMsg.textContent = `Aegis action: queueing IPC switch '${displayScene}'...`;
        const res = await fetch("/ipc/switch-scene", {
          method: "POST",
          headers: {
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json"
          },
          body: JSON.stringify({
            scene_name: sceneName,
            reason: reason || "manual_debug",
            deadline_ms: 550,
            allow_empty: allowEmpty
          })
        });
        const data = await res.json().catch(() => ({}));
        if (!res.ok) {
          aegisActionMsg.textContent = `Aegis action error: ${data.message || res.status}`;
          return;
        }
        aegisActionMsg.textContent = `Aegis action: ${data.message || "IPC switch queued"}`;
      } catch (e) {
        aegisActionMsg.textContent = `Aegis action error: ${e.message}`;
      }
    }

    const statusEl = document.getElementById("status");
    const timeEl = document.getElementById("time");
    const healthEl = document.getElementById("health");
    const obsEl = document.getElementById("obs");
    const testModeEl = document.getElementById("testmode");
    const recBadgeEl = document.getElementById("recbadge");
    const sysEl = document.getElementById("sys");
    const netEl = document.getElementById("net");
    const aegisEl = document.getElementById("aegis");
    const statDisk = document.getElementById("statDisk");
    const statRender = document.getElementById("statRender");
    const statOutput = document.getElementById("statOutput");
    const statFps = document.getElementById("statFps");
    const hideInactiveEl = document.getElementById("hideInactive");
    const summaryConnEl = document.getElementById("summaryConn");
    const summarySystemEl = document.getElementById("summarySystem");
    const summaryMainEl = document.getElementById("summaryMain");
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
      const hideInactive = hideInactiveEl.checked;
      outputs.forEach(o => {
        const isActive = o.bitrate_kbps > 0 || o.fps > 0;

        if (hideInactive && !isActive) return;

        let displayName = outputNameMap[o.name] || defaultNames[o.name] || o.name;
        if (!isActive) displayName += " (Inactive)";

        const box = document.createElement("div");
        box.className = isActive ? "output" : "output-inactive";
        box.dataset.outputId = o.name;

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

    function pickMainOutput(outputs) {
      if (!outputs || outputs.length === 0) return null;
      return outputs.find(o => o.name === "adv_stream")
        || outputs.find(o => o.bitrate_kbps > 0 || o.fps > 0)
        || outputs[0];
    }

    function updateSummaryPanels(data) {
      const aegisText = (aegisEl.textContent || "AEGIS: --").replace(/^AEGIS:\s*/, "");
      const obsConn = data.obs.connected ? "Connected" : "Disconnected";
      const obsMode = data.obs.streaming ? "Streaming" : "Idle";
      summaryConnEl.innerHTML = `OBS: ${obsConn} (${obsMode})<br>Latency: ${data.network.latency_ms.toFixed(0)} ms<br>Aegis: ${aegisText}`;

      const gpuPctText = data.system.gpu_percent != null ? `${data.system.gpu_percent.toFixed(0)}%` : "n/a";
      const gpuTempText = data.system.gpu_temp_c != null ? ` ${data.system.gpu_temp_c.toFixed(0)}C` : "";
      summarySystemEl.innerHTML = `CPU: ${data.system.cpu_percent.toFixed(0)}%<br>RAM: ${data.system.mem_percent.toFixed(0)}%<br>GPU/VRAM: ${gpuPctText}${gpuTempText} / n/a`;

      const main = pickMainOutput(data.outputs);
      if (!main) {
        summaryMainEl.innerHTML = "Bitrate: --<br>Drops: --<br>Lag/FPS: --";
        return;
      }
      summaryMainEl.innerHTML =
        `Bitrate: ${main.bitrate_kbps} kbps (${main.name})<br>` +
        `Drops: ${(main.drop_pct * 100).toFixed(2)}%<br>` +
        `Lag/FPS: ${main.encoding_lag_ms.toFixed(1)} ms / ${main.fps.toFixed(1)} fps`;
    }

    ws.onopen = () => { statusEl.textContent = "CONNECTED"; };
    ws.onclose = () => { statusEl.textContent = "DISCONNECTED"; };
    ws.onmessage = (event) => {
      const data = JSON.parse(event.data);
      timeEl.textContent = new Date(data.ts * 1000).toLocaleTimeString();
      healthEl.textContent = `Health: ${(data.health*100).toFixed(1)}%`;
      healthEl.style.borderColor = healthColor(data.health);
      obsEl.textContent = `OBS: ${data.obs.streaming ? "LIVE" : "IDLE"} | dropped ${data.obs.total_dropped_frames}`;

      // Studio mode badge
      testModeEl.style.display = data.obs.studio_mode ? "block" : "none";

      // Recording badge
      recBadgeEl.style.display = data.obs.recording ? "block" : "none";

      // System: include GPU temp if available
      const gpuPct = data.system.gpu_percent ?? 0;
      const gpuTemp = data.system.gpu_temp_c != null ? ` ${data.system.gpu_temp_c.toFixed(0)}C` : "";
      sysEl.textContent = `SYS: CPU ${data.system.cpu_percent.toFixed(0)}% | MEM ${data.system.mem_percent.toFixed(0)}% | GPU ${gpuPct}%${gpuTemp}`;

      // Network: show both upload and download
      netEl.textContent = `NET: UP ${data.network.upload_mbps.toFixed(1)} | DN ${data.network.download_mbps.toFixed(1)} Mb/s | LAT ${data.network.latency_ms.toFixed(0)} ms`;

      // OBS Stats row
      const diskGb = (data.obs.available_disk_space_mb / 1024).toFixed(1);
      statDisk.textContent = `Disk: ${diskGb} GB`;
      statRender.textContent = `Render missed: ${data.obs.render_missed_frames} / ${data.obs.render_total_frames}`;
      statOutput.textContent = `Encoder skipped: ${data.obs.output_skipped_frames} / ${data.obs.output_total_frames}`;
      statFps.textContent = `FPS: ${data.obs.active_fps.toFixed(1)}`;
      updateSummaryPanels(data);

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
    const refreshAegisBtn = document.getElementById("refreshAegisBtn");
    const aegisStartBtn = document.getElementById("aegisStartBtn");
    const aegisStopBtn = document.getElementById("aegisStopBtn");
    const ipcSceneNameEl = document.getElementById("ipcSceneName");
    const ipcSceneReasonEl = document.getElementById("ipcSceneReason");
    const ipcAllowEmptySceneEl = document.getElementById("ipcAllowEmptyScene");
    const ipcSwitchSceneBtn = document.getElementById("ipcSwitchSceneBtn");
    const aegisActionMsg = document.getElementById("aegisActionMsg");
    const ipcStatusMsg = document.getElementById("ipcStatusMsg");

    loadAegisStatus();
    loadIpcStatus();
    setInterval(() => loadAegisStatus(false), 10000);
    setInterval(() => loadIpcStatus(), 2000);
    refreshAegisBtn.onclick = () => loadAegisStatus(true);
    aegisStartBtn.onclick = () => aegisAction("/aegis/start");
    aegisStopBtn.onclick = () => aegisAction("/aegis/stop");
    ipcSwitchSceneBtn.onclick = () => ipcSwitchScene();

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

    let html = html
        .replace("{{THEME_VARS}}", &css)
        .replace("{{TOKEN}}", &html_escape(&state.token));
    Html(html).into_response()
}
