use super::{html_escape, is_token_valid, theme_css, QueryTokenPolicy, ServerState};
use crate::config::Config;
use crate::util::MutexExt;
use axum::{
    extract::{Query, State},
    http::{HeaderMap, StatusCode},
    response::{Html, IntoResponse},
    Form,
};
use base64::{engine::general_purpose, Engine as _};
use serde::Deserialize;
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Deserialize)]
pub(super) struct SettingsForm {
    obs_host: String,
    obs_port: u16,
    obs_password: Option<String>,
    grafana_interval: u64,
    grafana_endpoint: Option<String>,
    grafana_instance_id: Option<String>,
    grafana_api_token: Option<String>,
}

pub(super) async fn settings_page(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let config = match Config::load() {
        Ok(c) => c,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to load config: {}", e),
            )
                .into_response()
        }
    };
    let css = theme_css(&state.theme);

    let grafana_configured = *state.grafana_configured.lock_or_recover();
    let grafana_status = if grafana_configured {
        r#"<div class="status status-ok">Grafana Cloud: Connected</div>"#
    } else {
        r#"<div class="status status-off">Grafana Cloud: Not Configured</div>"#
    };

    let grafana_endpoint = config.grafana.endpoint.as_deref().unwrap_or("");

    let html = format!(
        r#"<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>Telemy - Settings</title>
  <style>
    :root {{ {css} }}
    body {{ margin:0; font-family:var(--font); background:var(--bg); color:#e6f0ff; }}
    .wrap {{ max-width:480px; margin:40px auto; padding:0 16px; }}
    h1 {{ font-size:20px; margin-bottom:20px; }}
    h2 {{ font-size:16px; margin-top:28px; margin-bottom:8px; border-top:1px solid var(--line); padding-top:18px; }}
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
    .back {{ font-size:12px; color:var(--muted); text-decoration:none; margin-bottom:20px; display:inline-block; }}
    .back:hover {{ color:#e6f0ff; }}
    .help {{ color:var(--muted); font-size:11px; margin-top:2px; }}
    .status {{ padding:8px 12px; border-radius:6px; margin-bottom:12px; font-size:13px; }}
    .status-ok {{ background:#1a2e1a; border:1px solid var(--good); color:var(--good); }}
    .status-off {{ background:#2e1a1a; border:1px solid var(--bad); color:var(--bad); }}
    .note {{ color:var(--muted); font-size:12px; margin-top:8px; }}
  </style>
</head>
<body>
  <div class="wrap">
    <a href="/obs?token={token}" class="back">&larr; Back to Dashboard</a>
    <h1>Settings</h1>
    <div id="msg" class="msg"></div>
    <form id="settingsForm">

      <h2>OBS Connection</h2>
      <label for="obs_host">OBS Host</label>
      <input id="obs_host" name="obs_host" type="text" value="{obs_host}" required />

      <label for="obs_port">OBS WebSocket Port</label>
      <input id="obs_port" name="obs_port" type="number" value="{obs_port}" required />

      <label for="obs_password">OBS WebSocket Password</label>
      <input id="obs_password" name="obs_password" type="password" placeholder="Leave blank to keep current" />
      <div class="help">Only fill in to change the stored password</div>

      <h2>Grafana Cloud</h2>
      {grafana_status}

      <label for="grafana_endpoint">OTLP Endpoint</label>
      <input id="grafana_endpoint" name="grafana_endpoint" type="url" value="{grafana_endpoint}"
             placeholder="https://otlp-gateway-prod-us-east-0.grafana.net/otlp" />
      <div class="help">Found in Grafana Cloud &rarr; OpenTelemetry &rarr; Configure</div>

      <label for="grafana_instance_id">Instance ID</label>
      <input id="grafana_instance_id" name="grafana_instance_id" type="text"
             placeholder="123456" />
      <div class="help">Your Grafana Cloud stack instance number</div>

      <label for="grafana_api_token">API Token</label>
      <input id="grafana_api_token" name="grafana_api_token" type="password"
             placeholder="glc_eyJ..." />
      <div class="help">Generate under Security &rarr; API Keys with MetricsPublisher role</div>

      <label for="grafana_interval">Push Interval (ms)</label>
      <input id="grafana_interval" name="grafana_interval" type="number" value="{grafana_interval}" required />

      <div class="note">Restart Telemy after saving for connection changes to take effect.</div>

      <button type="submit">Save Changes</button>
    </form>

    <h2>Grafana Dashboard</h2>
    <div class="note" style="margin-bottom:12px;">Import a pre-built Telemy dashboard into Grafana to visualize your metrics.</div>
    <a href="/grafana-dashboard?token={token}" download="telemy-dashboard.json"
       style="display:inline-block; padding:8px 16px; background:var(--panel); border:1px solid var(--line);
              border-radius:4px; color:#e6f0ff; text-decoration:none; font-size:13px; cursor:pointer;">
      Download Dashboard JSON
    </a>
    <div class="help" style="margin-top:6px;">Import this file in Grafana &rarr; Dashboards &rarr; Import</div>

    <details style="margin-top:16px;">
      <summary style="cursor:pointer; color:var(--muted); font-size:13px;">Auto-import via Grafana API (optional)</summary>
      <div style="margin-top:10px;">
        <label for="grafana_url">Grafana URL</label>
        <input id="grafana_url" type="url" placeholder="https://yourstack.grafana.net" />
        <div class="help">Your Grafana instance URL (not the OTLP endpoint)</div>

        <label for="grafana_org_key">Service Account Token</label>
        <input id="grafana_org_key" type="password" placeholder="glsa_..." />
        <div class="help">Needs Dashboard Editor permissions. Create under Administration &rarr; Service Accounts.</div>

        <button type="button" id="importBtn"
                style="margin-top:12px; padding:8px 16px; background:var(--panel); border:1px solid var(--good);
                       color:var(--good); border-radius:4px; font-size:13px; cursor:pointer;">
          Import Dashboard
        </button>
        <div id="importMsg" class="msg" style="margin-top:8px;"></div>
      </div>
    </details>
  </div>
  <script>
    const params = new URLSearchParams(window.location.search);
    const token = params.get("token");

    document.getElementById("settingsForm").addEventListener("submit", async (e) => {{
      e.preventDefault();
      const msg = document.getElementById("msg");
      const data = new URLSearchParams(new FormData(e.target));
      try {{
        const res = await fetch("/settings", {{
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

    document.getElementById("importBtn").addEventListener("click", async () => {{
      const importMsg = document.getElementById("importMsg");
      const grafanaUrl = document.getElementById("grafana_url").value.trim();
      const grafanaKey = document.getElementById("grafana_org_key").value.trim();
      if (!grafanaUrl || !grafanaKey) {{
        importMsg.textContent = "Both Grafana URL and API key are required.";
        importMsg.className = "msg msg-err";
        return;
      }}
      const data = new URLSearchParams({{ grafana_url: grafanaUrl, grafana_api_key: grafanaKey }});
      try {{
        const res = await fetch("/grafana-dashboard/import?token=" + token, {{
          method: "POST",
          headers: {{
            "Content-Type": "application/x-www-form-urlencoded",
            "Authorization": "Bearer " + token
          }},
          body: data,
        }});
        const text = await res.text();
        importMsg.textContent = text;
        importMsg.className = res.ok ? "msg msg-ok" : "msg msg-err";
      }} catch (err) {{
        importMsg.textContent = "Request failed: " + err.message;
        importMsg.className = "msg msg-err";
      }}
    }});
  </script>
</body>
</html>"#,
        css = css,
        token = html_escape(&state.token),
        obs_host = html_escape(&config.obs.host),
        obs_port = config.obs.port,
        grafana_status = grafana_status,
        grafana_endpoint = html_escape(grafana_endpoint),
        grafana_interval = config.grafana.push_interval_ms
    );

    Html(html).into_response()
}

pub(super) async fn settings_submit(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Form(form): Form<SettingsForm>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized".to_string()).into_response();
    }

    let mut config = match Config::load() {
        Ok(c) => c,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("Failed to load config: {}", e),
            )
                .into_response()
        }
    };

    // OBS settings
    config.obs.host = form.obs_host;
    config.obs.port = form.obs_port;

    // OBS password — only update if user provided a new one
    if let Some(ref pw) = form.obs_password {
        if !pw.is_empty() {
            let mut vault = state.vault.lock_or_recover();
            if let Err(e) = vault.store("obs_password", pw) {
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    format!("Failed to store OBS password: {}", e),
                )
                    .into_response();
            }
            config.obs.password_key = Some("obs_password".to_string());
        }
    }

    // Grafana settings
    config.grafana.push_interval_ms = form.grafana_interval;

    // Grafana credentials — only update if all three fields are provided
    let endpoint = form
        .grafana_endpoint
        .as_deref()
        .unwrap_or("")
        .trim()
        .to_string();
    let instance_id = form
        .grafana_instance_id
        .as_deref()
        .unwrap_or("")
        .trim()
        .to_string();
    let api_token = form
        .grafana_api_token
        .as_deref()
        .unwrap_or("")
        .trim()
        .to_string();

    if !endpoint.is_empty() && !instance_id.is_empty() && !api_token.is_empty() {
        let credentials = format!("{}:{}", instance_id, api_token);
        let encoded = general_purpose::STANDARD.encode(credentials.as_bytes());
        let auth_value = format!("Basic {}", encoded);

        {
            let mut vault = state.vault.lock_or_recover();
            if let Err(e) = vault.store("grafana_auth", &auth_value) {
                return (
                    StatusCode::INTERNAL_SERVER_ERROR,
                    format!("Failed to store Grafana credentials: {}", e),
                )
                    .into_response();
            }
        }

        config.grafana.enabled = true;
        config.grafana.endpoint = Some(endpoint);
        config.grafana.auth_value_key = Some("grafana_auth".to_string());
        *state.grafana_configured.lock_or_recover() = true;
    } else if !endpoint.is_empty() {
        // Allow updating just the endpoint without re-entering credentials
        config.grafana.endpoint = Some(endpoint);
    }

    match config.save() {
        Ok(_) => (
            StatusCode::OK,
            "Settings saved. Restart required for connection changes to take effect.".to_string(),
        )
            .into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("Failed to save config: {}", e),
        )
            .into_response(),
    }
}
