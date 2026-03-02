mod aegis;
mod dashboard;
mod settings;

use crate::aegis::RelaySession;
use crate::config::{Config, ThemeConfig};
use crate::ipc::{CoreIpcCommandSender, IpcDebugStatusHandle};
use crate::model::TelemetryFrame;
use crate::security::Vault;
use axum::{
    extract::{
        ws::{Message, WebSocket, WebSocketUpgrade},
        Query, State,
    },
    http::{HeaderMap, StatusCode},
    response::IntoResponse,
    routing::{get, post},
    Form, Router,
};
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
pub(super) struct ServerState {
    pub(super) token: String,
    pub(super) rx: watch::Receiver<TelemetryFrame>,
    pub(super) theme: ThemeConfig,
    pub(super) vault: Arc<Mutex<Vault>>,
    pub(super) grafana_configured: Arc<Mutex<bool>>,
    pub(super) aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
    pub(super) ipc_cmd_tx: CoreIpcCommandSender,
    pub(super) ipc_debug_status: IpcDebugStatusHandle,
}

pub async fn start(
    addr: SocketAddr,
    token: String,
    rx: watch::Receiver<TelemetryFrame>,
    mut shutdown_rx: watch::Receiver<bool>,
    theme: ThemeConfig,
    vault: Arc<Mutex<Vault>>,
    grafana_configured: bool,
    aegis_session_snapshot: Arc<Mutex<Option<RelaySession>>>,
    ipc_cmd_tx: CoreIpcCommandSender,
    ipc_debug_status: IpcDebugStatusHandle,
) -> Result<(), Box<dyn std::error::Error>> {
    let state = Arc::new(ServerState {
        token,
        rx,
        theme,
        vault,
        grafana_configured: Arc::new(Mutex::new(grafana_configured)),
        aegis_session_snapshot,
        ipc_cmd_tx,
        ipc_debug_status,
    });

    let app = Router::new()
        .route("/health", get(health_check))
        .route("/obs", get(dashboard::obs_page))
        .route("/ws", get(ws_handler))
        .route("/setup", get(setup_page))
        .route("/settings", get(settings::settings_page))
        .route("/settings", post(settings::settings_submit))
        .route("/output-names", get(get_output_names))
        .route("/output-names", post(save_output_names))
        .route("/grafana-dashboard", get(grafana_dashboard_download))
        .route("/grafana-dashboard/import", post(grafana_dashboard_import))
        .route("/aegis/status", get(aegis::get_aegis_status))
        .route("/aegis/start", post(aegis::post_aegis_start))
        .route("/aegis/stop", post(aegis::post_aegis_stop))
        .route("/ipc/status", get(aegis::get_ipc_status))
        .route("/ipc/switch-scene", post(aegis::post_ipc_switch_scene))
        .with_state(state);

    let listener = TcpListener::bind(addr).await?;
    axum::serve(listener, app)
        .with_graceful_shutdown(async move {
            let _ = shutdown_rx.changed().await;
        })
        .await?;

    Ok(())
}

async fn setup_page(query: Query<HashMap<String, String>>) -> impl IntoResponse {
    // Redirect /setup to /settings (Grafana config is now in settings)
    let token = query.0.get("token").cloned().unwrap_or_default();
    axum::response::Redirect::temporary(&format!("/settings?token={}", html_escape(&token)))
        .into_response()
}

async fn ws_handler(
    State(state): State<Arc<ServerState>>,
    ws: WebSocketUpgrade,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    // Native browser WebSocket clients cannot set Authorization headers directly.
    // Keep query-token fallback here for local dashboard compatibility.
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum QueryTokenPolicy {
    Allow,
    Deny,
}

pub(super) fn is_token_valid(
    headers: &HeaderMap,
    query: &HashMap<String, String>,
    token: &str,
    query_policy: QueryTokenPolicy,
) -> bool {
    // First check Authorization header (preferred for API access)
    // Format: "Bearer <token>"
    if let Some(auth_header) = headers.get("authorization") {
        if let Ok(auth_str) = auth_header.to_str() {
            if let Some(provided_token) = auth_str.strip_prefix("Bearer ") {
                return provided_token == token;
            }
        }
    }

    if query_policy == QueryTokenPolicy::Allow {
        // Fall back to query parameter for browser/Dock GET routes.
        return query.get("token").map(|t| t == token).unwrap_or(false);
    }

    false
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
        })),
    )
}

pub(super) fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#x27;")
}

pub(super) fn theme_css(theme: &ThemeConfig) -> String {
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
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    // Load current config to get latest names
    match Config::load() {
        Ok(config) => (StatusCode::OK, axum::Json(config.output_names)).into_response(),
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
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
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

const GRAFANA_DASHBOARD_JSON: &str = include_str!("../../assets/grafana-dashboard.json");

async fn grafana_dashboard_download(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    (
        StatusCode::OK,
        [
            ("content-type", "application/json"),
            (
                "content-disposition",
                "attachment; filename=\"telemy-dashboard.json\"",
            ),
        ],
        GRAFANA_DASHBOARD_JSON,
    )
        .into_response()
}

#[derive(Deserialize)]
struct GrafanaImportForm {
    grafana_url: String,
    grafana_api_key: String,
}

async fn grafana_dashboard_import(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Form(form): Form<GrafanaImportForm>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized".to_string()).into_response();
    }

    let url = form.grafana_url.trim().trim_end_matches('/');
    let api_key = form.grafana_api_key.trim();

    if url.is_empty() || api_key.is_empty() {
        return (
            StatusCode::BAD_REQUEST,
            "Grafana URL and API key are required".to_string(),
        )
            .into_response();
    }

    let import_url = format!("{}/api/dashboards/db", url);

    let client = match reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()
    {
        Ok(c) => c,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("HTTP client error: {}", e),
            )
                .into_response()
        }
    };

    let res = client
        .post(&import_url)
        .header("Authorization", format!("Bearer {}", api_key))
        .header("Content-Type", "application/json")
        .body(GRAFANA_DASHBOARD_JSON)
        .send()
        .await;

    match res {
        Ok(resp) => {
            let status = resp.status();
            let body = resp.text().await.unwrap_or_default();
            if status.is_success() {
                (
                    StatusCode::OK,
                    "Dashboard imported successfully into Grafana.".to_string(),
                )
                    .into_response()
            } else {
                (
                    StatusCode::BAD_GATEWAY,
                    format!("Grafana returned {}: {}", status, body),
                )
                    .into_response()
            }
        }
        Err(e) => (
            StatusCode::BAD_GATEWAY,
            format!("Failed to reach Grafana: {}", e),
        )
            .into_response(),
    }
}

#[cfg(test)]
mod tests {
    use super::{is_token_valid, QueryTokenPolicy};
    use axum::http::{HeaderMap, HeaderValue};
    use std::collections::HashMap;

    #[test]
    fn token_valid_accepts_bearer_header_when_query_denied() {
        let mut headers = HeaderMap::new();
        headers.insert(
            "authorization",
            HeaderValue::from_static("Bearer test-token"),
        );
        let query = HashMap::from([("token".to_string(), "wrong-token".to_string())]);

        let ok = is_token_valid(&headers, &query, "test-token", QueryTokenPolicy::Deny);
        assert!(ok);
    }

    #[test]
    fn token_valid_rejects_query_when_policy_denied() {
        let headers = HeaderMap::new();
        let query = HashMap::from([("token".to_string(), "test-token".to_string())]);

        let ok = is_token_valid(&headers, &query, "test-token", QueryTokenPolicy::Deny);
        assert!(!ok);
    }

    #[test]
    fn token_valid_accepts_query_when_policy_allowed() {
        let headers = HeaderMap::new();
        let query = HashMap::from([("token".to_string(), "test-token".to_string())]);

        let ok = is_token_valid(&headers, &query, "test-token", QueryTokenPolicy::Allow);
        assert!(ok);
    }
}
