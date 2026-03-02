use super::{is_token_valid, QueryTokenPolicy, ServerState};
use crate::aegis::{RelaySession, RelayStartClientContext, RelayStartRequest, RelayStopRequest};
use crate::aegis_client::{build_aegis_client, generate_idempotency_key};
use crate::config::Config;
use crate::ipc::{CoreIpcCommand, IpcDebugStatus};
use crate::util::MutexExt;
use axum::{
    extract::{Json, Query, State},
    http::{HeaderMap, StatusCode},
    response::IntoResponse,
};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Serialize)]
struct AegisStatusResponse {
    enabled: bool,
    session: Option<RelaySession>,
    refreshed: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Serialize)]
struct AegisActionResponse {
    ok: bool,
    message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    session: Option<RelaySession>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<String>,
}

#[derive(Debug, Deserialize)]
pub(super) struct IpcSwitchSceneRequest {
    scene_name: String,
    #[serde(default)]
    reason: Option<String>,
    #[serde(default)]
    deadline_ms: Option<u64>,
    #[serde(default)]
    allow_empty: Option<bool>,
}

#[derive(Debug, Serialize)]
struct IpcSwitchSceneResponse {
    ok: bool,
    message: String,
}

pub(super) async fn get_ipc_status(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let snapshot: IpcDebugStatus = state.ipc_debug_status.lock_or_recover().clone();
    (StatusCode::OK, axum::Json(snapshot)).into_response()
}

pub(super) async fn get_aegis_status(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Allow) {
        return StatusCode::UNAUTHORIZED.into_response();
    }

    let refresh_requested = query
        .0
        .get("refresh")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);

    let config = match Config::load() {
        Ok(cfg) => cfg,
        Err(err) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                axum::Json(AegisStatusResponse {
                    enabled: false,
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    refreshed: false,
                    error: Some(format!("config load failed: {err}")),
                }),
            )
                .into_response();
        }
    };

    if !config.aegis.enabled {
        return (
            StatusCode::OK,
            axum::Json(AegisStatusResponse {
                enabled: false,
                session: None,
                refreshed: false,
                error: None,
            }),
        )
            .into_response();
    }

    if refresh_requested {
        let client = {
            let vault = state.vault.lock_or_recover();
            build_aegis_client(&config, &vault).map_err(|err| err.to_string())
        };
        let refreshed = match client {
            Ok(client) => match client.relay_active().await {
                Ok(session) => {
                    *state.aegis_session_snapshot.lock_or_recover() = session.clone();
                    Ok(session)
                }
                Err(err) => Err(format!("{err}")),
            },
            Err(err) => Err(format!("{err}")),
        };

        return match refreshed {
            Ok(session) => (
                StatusCode::OK,
                axum::Json(AegisStatusResponse {
                    enabled: true,
                    session,
                    refreshed: true,
                    error: None,
                }),
            )
                .into_response(),
            Err(err) => (
                StatusCode::BAD_GATEWAY,
                axum::Json(AegisStatusResponse {
                    enabled: true,
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    refreshed: false,
                    error: Some(err),
                }),
            )
                .into_response(),
        };
    }

    (
        StatusCode::OK,
        axum::Json(AegisStatusResponse {
            enabled: true,
            session: state.aegis_session_snapshot.lock_or_recover().clone(),
            refreshed: false,
            error: None,
        }),
    )
        .into_response()
}

pub(super) async fn post_aegis_start(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let config = match Config::load() {
        Ok(cfg) => cfg,
        Err(err) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "config load failed".to_string(),
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    error: Some(err.to_string()),
                }),
            )
                .into_response()
        }
    };

    let client = {
        let vault = state.vault.lock_or_recover();
        build_aegis_client(&config, &vault).map_err(|err| err.to_string())
    };

    let client = match client {
        Ok(client) => client,
        Err(err) => {
            return (
                StatusCode::BAD_REQUEST,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "aegis client config invalid".to_string(),
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    error: Some(err),
                }),
            )
                .into_response()
        }
    };

    let request = RelayStartRequest {
        region_preference: Some("auto".to_string()),
        client_context: Some(RelayStartClientContext {
            obs_connected: None,
            mode: Some("studio".to_string()),
            requested_by: Some("dashboard".to_string()),
        }),
    };
    let idem = generate_idempotency_key();

    match client.relay_start(&idem, &request).await {
        Ok(session) => {
            *state.aegis_session_snapshot.lock_or_recover() = Some(session.clone());
            (
                StatusCode::OK,
                axum::Json(AegisActionResponse {
                    ok: true,
                    message: format!("relay start ok ({})", session.status),
                    session: Some(session),
                    error: None,
                }),
            )
                .into_response()
        }
        Err(err) => (
            StatusCode::BAD_GATEWAY,
            axum::Json(AegisActionResponse {
                ok: false,
                message: "relay start failed".to_string(),
                session: state.aegis_session_snapshot.lock_or_recover().clone(),
                error: Some(err.to_string()),
            }),
        )
            .into_response(),
    }
}

pub(super) async fn post_aegis_stop(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let config = match Config::load() {
        Ok(cfg) => cfg,
        Err(err) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "config load failed".to_string(),
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    error: Some(err.to_string()),
                }),
            )
                .into_response()
        }
    };

    let client = {
        let vault = state.vault.lock_or_recover();
        build_aegis_client(&config, &vault).map_err(|err| err.to_string())
    };
    let client = match client {
        Ok(client) => client,
        Err(err) => {
            return (
                StatusCode::BAD_REQUEST,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "aegis client config invalid".to_string(),
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    error: Some(err),
                }),
            )
                .into_response()
        }
    };

    let current = match client.relay_active().await {
        Ok(session) => session,
        Err(err) => {
            return (
                StatusCode::BAD_GATEWAY,
                axum::Json(AegisActionResponse {
                    ok: false,
                    message: "relay active lookup failed".to_string(),
                    session: state.aegis_session_snapshot.lock_or_recover().clone(),
                    error: Some(err.to_string()),
                }),
            )
                .into_response()
        }
    };

    let Some(session) = current else {
        *state.aegis_session_snapshot.lock_or_recover() = None;
        return (
            StatusCode::OK,
            axum::Json(AegisActionResponse {
                ok: true,
                message: "no active relay session".to_string(),
                session: None,
                error: None,
            }),
        )
            .into_response();
    };

    let stop_req = RelayStopRequest {
        session_id: session.session_id.clone(),
        reason: "user_requested".to_string(),
    };
    match client.relay_stop(&stop_req).await {
        Ok(_) => {
            *state.aegis_session_snapshot.lock_or_recover() = None;
            (
                StatusCode::OK,
                axum::Json(AegisActionResponse {
                    ok: true,
                    message: format!("relay stop ok ({})", stop_req.session_id),
                    session: None,
                    error: None,
                }),
            )
                .into_response()
        }
        Err(err) => (
            StatusCode::BAD_GATEWAY,
            axum::Json(AegisActionResponse {
                ok: false,
                message: "relay stop failed".to_string(),
                session: state.aegis_session_snapshot.lock_or_recover().clone(),
                error: Some(err.to_string()),
            }),
        )
            .into_response(),
    }
}

pub(super) async fn post_ipc_switch_scene(
    State(state): State<Arc<ServerState>>,
    headers: HeaderMap,
    query: Query<HashMap<String, String>>,
    Json(body): Json<IpcSwitchSceneRequest>,
) -> impl IntoResponse {
    if !is_token_valid(&headers, &query.0, &state.token, QueryTokenPolicy::Deny) {
        return (StatusCode::UNAUTHORIZED, "Unauthorized").into_response();
    }

    let scene_name = body.scene_name.trim();
    let allow_empty = body.allow_empty.unwrap_or(false);
    if scene_name.is_empty() && !allow_empty {
        return (
            StatusCode::BAD_REQUEST,
            axum::Json(IpcSwitchSceneResponse {
                ok: false,
                message: "scene_name is required (set allow_empty=true for debug negative-path validation)".to_string(),
            }),
        )
            .into_response();
    }

    let reason = body
        .reason
        .as_deref()
        .unwrap_or("manual_debug")
        .trim()
        .to_string();
    let deadline_ms = body.deadline_ms.unwrap_or(550).clamp(50, 5000);

    match state.ipc_cmd_tx.send(CoreIpcCommand::SwitchScene {
        scene_name: scene_name.to_string(),
        reason: if reason.is_empty() {
            "manual_debug".to_string()
        } else {
            reason
        },
        deadline_ms,
    }) {
        Ok(_receiver_count) => (
            StatusCode::OK,
            axum::Json(IpcSwitchSceneResponse {
                ok: true,
                message: format!(
                    "queued ipc switch_scene '{}' (deadline={}ms{})",
                    scene_name,
                    deadline_ms,
                    if scene_name.is_empty() { ", empty scene debug case" } else { "" }
                ),
            }),
        )
            .into_response(),
        Err(err) => (
            StatusCode::SERVICE_UNAVAILABLE,
            axum::Json(IpcSwitchSceneResponse {
                ok: false,
                message: format!("ipc switch_scene unavailable: {err}"),
            }),
        )
            .into_response(),
    }
}
