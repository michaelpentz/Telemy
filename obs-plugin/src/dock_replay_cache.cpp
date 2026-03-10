#include "dock_replay_cache.h"
#include "dock_theme.h"
#include "dock_action_dispatch.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)

#include <obs-module.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Shared utility — defined in obs_plugin_entry.cpp
// ---------------------------------------------------------------------------
std::string JsonEscape(const std::string& input);
std::string JsStringLiteral(const std::string& input);
bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value);
bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value);
std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json);

// ---------------------------------------------------------------------------
// Replay cache globals — defined here, the single owner.
// ---------------------------------------------------------------------------
static std::mutex g_dock_replay_cache_mu;
static DockReplayCache g_dock_replay_cache;

// ---------------------------------------------------------------------------
// JS delivery validation globals — defined here, the single owner.
// ---------------------------------------------------------------------------
static std::mutex g_dock_js_delivery_validation_mu;
static DockJsDeliveryValidationState g_dock_js_delivery_validation;

// ---------------------------------------------------------------------------
// JS execution sink globals
// ---------------------------------------------------------------------------
static std::mutex g_dock_browser_js_execute_mu;
static DockBrowserJsExecuteFn g_dock_browser_js_execute;

// ---------------------------------------------------------------------------
// Scene snapshot emitter globals
// ---------------------------------------------------------------------------
static std::mutex g_dock_scene_snapshot_emitter_mu;
static DockSceneSnapshotEmitterFn g_dock_scene_snapshot_emitter;

// ---------------------------------------------------------------------------
// Accessor functions for entry file to reach replay cache / validation state
// ---------------------------------------------------------------------------
std::mutex& GetDockReplayCacheMutex() {
    return g_dock_replay_cache_mu;
}

DockReplayCache& GetDockReplayCacheRef() {
    return g_dock_replay_cache;
}

std::mutex& GetDockJsDeliveryValidationMutex() {
    return g_dock_js_delivery_validation_mu;
}

DockJsDeliveryValidationState& GetDockJsDeliveryValidationRef() {
    return g_dock_js_delivery_validation;
}

// ---------------------------------------------------------------------------
// JS sink probe
// ---------------------------------------------------------------------------
DockJsSinkProbeState GetDockJsSinkProbeState() {
    std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
    DockJsSinkProbeState out;
    out.js_sink_registered = g_dock_js_delivery_validation.js_sink_registered;
    out.page_ready = g_dock_js_delivery_validation.page_ready;
    return out;
}

// ---------------------------------------------------------------------------
// JS execution sink management
// ---------------------------------------------------------------------------
void SetDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn) {
    const bool has_sink = static_cast<bool>(execute_fn);
    std::lock_guard<std::mutex> lock(g_dock_browser_js_execute_mu);
    g_dock_browser_js_execute = std::move(execute_fn);
    {
        std::lock_guard<std::mutex> validation_lock(g_dock_js_delivery_validation_mu);
        g_dock_js_delivery_validation.js_sink_registered = has_sink;
        if (!has_sink) {
            g_dock_js_delivery_validation.page_ready = false;
            g_dock_js_delivery_validation.fallback_pipe_status_count = 0;
            g_dock_js_delivery_validation.fallback_status_snapshot_count = 0;
            g_dock_js_delivery_validation.fallback_scene_snapshot_count = 0;
            g_dock_js_delivery_validation.fallback_scene_switch_completed_count = 0;
            g_dock_js_delivery_validation.fallback_dock_action_result_count = 0;
        }
    }
}

bool TryExecuteDockBrowserJs(const std::string& js_code) {
    DockBrowserJsExecuteFn exec_copy;
    {
        std::lock_guard<std::mutex> lock(g_dock_browser_js_execute_mu);
        exec_copy = g_dock_browser_js_execute;
    }
    if (!exec_copy) {
        return false;
    }
    return exec_copy(js_code);
}

void RegisterDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn) {
    SetDockBrowserJsExecuteSink(std::move(execute_fn));
    ReplayDockStateToJsSinkIfAvailable();
}

// ---------------------------------------------------------------------------
// Scene snapshot emitter management
// ---------------------------------------------------------------------------
void SetDockSceneSnapshotEmitter(DockSceneSnapshotEmitterFn emitter) {
    std::lock_guard<std::mutex> lock(g_dock_scene_snapshot_emitter_mu);
    g_dock_scene_snapshot_emitter = std::move(emitter);
}

// ---------------------------------------------------------------------------
// Fallback logging
// ---------------------------------------------------------------------------
bool ShouldLogDockFallbackPayload(
    DockFallbackLogKind kind,
    const char** out_phase,
    std::uint32_t* out_attempt) {
    if (out_phase) {
        *out_phase = "unknown";
    }
    if (out_attempt) {
        *out_attempt = 0;
    }

    std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
    std::uint32_t* count = nullptr;
    switch (kind) {
    case DockFallbackLogKind::PipeStatus:
        count = &g_dock_js_delivery_validation.fallback_pipe_status_count;
        break;
    case DockFallbackLogKind::StatusSnapshotJson:
        count = &g_dock_js_delivery_validation.fallback_status_snapshot_count;
        break;
    case DockFallbackLogKind::SceneSnapshotJson:
        count = &g_dock_js_delivery_validation.fallback_scene_snapshot_count;
        break;
    case DockFallbackLogKind::SceneSwitchCompletedJson:
        count = &g_dock_js_delivery_validation.fallback_scene_switch_completed_count;
        break;
    case DockFallbackLogKind::DockActionResultJson:
        count = &g_dock_js_delivery_validation.fallback_dock_action_result_count;
        break;
    }

    if (!count) {
        return true;
    }

    *count += 1;
    if (out_attempt) {
        *out_attempt = *count;
    }

    if (!g_dock_js_delivery_validation.js_sink_registered) {
        if (out_phase) {
            *out_phase = "no_js_sink";
        }
        return true;
    }

    if (!g_dock_js_delivery_validation.page_ready) {
        if (out_phase) {
            *out_phase = "pre_page_ready";
        }
        return (*count <= 3) || ((*count % 20) == 0);
    }

    if (out_phase) {
        *out_phase = "post_page_ready_sink_miss";
    }
    return (*count == 1) || ((*count % 50) == 0);
}

// ---------------------------------------------------------------------------
// Dock JS emission helpers
// ---------------------------------------------------------------------------
bool EmitDockNativeJsonArgCall(const char* method_name, const std::string& payload_json) {
    if (!method_name || payload_json.empty()) {
        return false;
    }
    std::ostringstream js;
    js << "if (window.aegisDockNative && typeof window.aegisDockNative." << method_name
       << " === 'function') { window.aegisDockNative." << method_name << "("
       << JsStringLiteral(payload_json) << "); }";
    const bool delivered = TryExecuteDockBrowserJs(js.str());
    if (delivered) {
        std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
        if (g_dock_js_delivery_validation.page_ready &&
            g_dock_js_delivery_validation.js_sink_registered) {
            bool* already_logged = nullptr;
            if (std::string(method_name) == "receiveStatusSnapshotJson") {
                already_logged = &g_dock_js_delivery_validation.logged_receive_status_snapshot_json;
            } else if (std::string(method_name) == "receiveSceneSnapshotJson") {
                already_logged = &g_dock_js_delivery_validation.logged_receive_scene_snapshot_json;
            } else if (std::string(method_name) == "receiveSceneSwitchCompletedJson") {
                already_logged =
                    &g_dock_js_delivery_validation.logged_receive_scene_switch_completed_json;
            } else if (std::string(method_name) == "receiveDockActionResultJson") {
                already_logged =
                    &g_dock_js_delivery_validation.logged_receive_dock_action_result_json;
            }
            if (already_logged && !*already_logged) {
                *already_logged = true;
                blog(
                    LOG_INFO,
                    "[aegis-obs-plugin] dock js sink delivery validated post-page-ready: method=%s payload_bytes=%d",
                    method_name,
                    static_cast<int>(payload_json.size()));
            }
        }
    }
    return delivered;
}

bool EmitDockNativePipeStatus(const char* status, const char* reason) {
    if (!status) {
        return false;
    }
    std::ostringstream js;
    js << "if (window.aegisDockNative && typeof window.aegisDockNative.receivePipeStatus === "
          "'function') { window.aegisDockNative.receivePipeStatus("
       << JsStringLiteral(status) << ",";
    if (reason && *reason) {
        js << JsStringLiteral(reason);
    } else {
        js << "null";
    }
    js << "); }";
    return TryExecuteDockBrowserJs(js.str());
}

bool EmitDockNativeCurrentScene(const std::string& scene_name) {
    std::ostringstream js;
    js << "if (window.aegisDockNative && typeof window.aegisDockNative.receiveCurrentScene === "
          "'function') { window.aegisDockNative.receiveCurrentScene(";
    if (scene_name.empty()) {
        js << "null";
    } else {
        js << JsStringLiteral(scene_name);
    }
    js << "); }";
    return TryExecuteDockBrowserJs(js.str());
}

// ---------------------------------------------------------------------------
// Cache write functions
// ---------------------------------------------------------------------------
void CacheDockSceneSnapshotForReplay(const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_scene_snapshot = !payload_json.empty();
    g_dock_replay_cache.scene_snapshot_json = payload_json;
}

void CacheDockPipeStatusForReplay(const char* status, const char* reason) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_pipe_status = (status != nullptr && *status != '\0');
    g_dock_replay_cache.pipe_status = status ? status : "";
    g_dock_replay_cache.pipe_reason = (reason && *reason) ? reason : "";
}

void CacheDockCurrentSceneForReplay(const std::string& scene_name) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_current_scene = true;
    g_dock_replay_cache.current_scene_name = scene_name;
}

void CacheDockSceneSwitchCompletedForReplay(const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_scene_switch_completed = !payload_json.empty();
    g_dock_replay_cache.scene_switch_completed_json = payload_json;
}

void CacheDockActionResultForReplay(const std::string& payload_json) {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache.has_dock_action_result = !payload_json.empty();
    g_dock_replay_cache.dock_action_result_json = payload_json;
}

// ---------------------------------------------------------------------------
// Clear / replay
// ---------------------------------------------------------------------------
void ClearDockReplayCache() {
    std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
    g_dock_replay_cache = DockReplayCache{};
}

void ReplayDockStateToJsSinkIfAvailable() {
    DockReplayCache snapshot;
    {
        std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
        snapshot = g_dock_replay_cache;
    }
    const DockJsSinkProbeState sink_state = GetDockJsSinkProbeState();

    if (snapshot.has_pipe_status) {
        EmitDockNativePipeStatus(snapshot.pipe_status.c_str(), snapshot.pipe_reason.c_str());
    }
    // v0.0.4: Replay native status snapshot with current Qt theme.
    if (snapshot.has_status_snapshot && !snapshot.status_snapshot_json.empty()) {
        const std::string themed = AugmentSnapshotJsonWithTheme(snapshot.status_snapshot_json);
        const bool delivered =
            EmitDockNativeJsonArgCall("receiveStatusSnapshotJson", themed);
        blog(
            delivered ? LOG_INFO : LOG_WARNING,
            "[aegis-obs-plugin] dock replay status snapshot: delivered=%s bytes=%d",
            delivered ? "true" : "false",
            static_cast<int>(themed.size()));
    } else {
        // No status snapshot cached yet -- synthesize a minimal one with just the theme
        // so the dock picks up OBS color scheme on page load / refresh.
        const ObsDockThemeSlots cached_theme = GetCachedObsDockTheme();
        if (cached_theme.valid) {
            QJsonObject obj;
            obj.insert(QStringLiteral("theme"), QtThemeToJsonObject(cached_theme));
            const std::string synthetic_json =
                QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
            const bool delivered =
                EmitDockNativeJsonArgCall("receiveStatusSnapshotJson", synthetic_json);
            blog(
                delivered ? LOG_INFO : LOG_WARNING,
                "[aegis-obs-plugin] dock replay: synthetic theme snapshot emitted "
                "delivered=%s bytes=%d",
                delivered ? "true" : "false",
                static_cast<int>(synthetic_json.size()));
        }
    }
    if (snapshot.has_scene_snapshot && !snapshot.scene_snapshot_json.empty()) {
        const bool delivered =
            EmitDockNativeJsonArgCall("receiveSceneSnapshotJson", snapshot.scene_snapshot_json);
        blog(
            delivered ? LOG_INFO : LOG_WARNING,
            "[aegis-obs-plugin] dock replay scene snapshot: delivered=%s bytes=%d js_sink=%s page_ready=%s",
            delivered ? "true" : "false",
            static_cast<int>(snapshot.scene_snapshot_json.size()),
            sink_state.js_sink_registered ? "true" : "false",
            sink_state.page_ready ? "true" : "false");
    } else {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock replay scene snapshot: skipped (cached_scene_snapshot=%s) js_sink=%s page_ready=%s",
            snapshot.has_scene_snapshot ? "empty_payload" : "none",
            sink_state.js_sink_registered ? "true" : "false",
            sink_state.page_ready ? "true" : "false");
    }
    if (snapshot.has_current_scene) {
        EmitDockNativeCurrentScene(snapshot.current_scene_name);
    }
    if (snapshot.has_scene_switch_completed && !snapshot.scene_switch_completed_json.empty()) {
        EmitDockNativeJsonArgCall(
            "receiveSceneSwitchCompletedJson",
            snapshot.scene_switch_completed_json);
    }
    if (snapshot.has_dock_action_result && !snapshot.dock_action_result_json.empty()) {
        EmitDockNativeJsonArgCall("receiveDockActionResultJson", snapshot.dock_action_result_json);
    }
}

// ---------------------------------------------------------------------------
// Re-emit status snapshot with fresh theme
// ---------------------------------------------------------------------------
void ReemitDockStatusSnapshotWithCurrentTheme(const char* reason) {
    std::string snapshot_json;
    {
        std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
        snapshot_json = g_dock_replay_cache.status_snapshot_json;
    }
    if (snapshot_json.empty()) {
        blog(LOG_DEBUG, "[aegis-obs-plugin] theme refresh skipped: no cached status_snapshot (reason=%s)",
             reason ? reason : "unknown");
        return;
    }
    const std::string themed = AugmentSnapshotJsonWithTheme(snapshot_json);
    // Update the cache with the themed version.
    {
        std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
        g_dock_replay_cache.status_snapshot_json = themed;
    }
    const bool delivered = EmitDockNativeJsonArgCall("receiveStatusSnapshotJson", themed);
    blog(
        delivered ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-plugin] dock theme refresh status_snapshot re-emitted: delivered=%s reason=%s bytes=%d",
        delivered ? "true" : "false",
        reason ? reason : "unknown",
        static_cast<int>(themed.size()));
}

// ---------------------------------------------------------------------------
// JSON building helpers
// ---------------------------------------------------------------------------
std::string BuildSceneSwitchCompletedJson(const std::string& request_id,
                                          const std::string& scene_name,
                                          bool ok,
                                          const std::string& error,
                                          const std::string& reason) {
    std::ostringstream os;
    os << "{";
    os << "\"requestId\":";
    if (request_id.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(request_id) << "\"";
    }
    os << ",";
    os << "\"sceneName\":";
    if (scene_name.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(scene_name) << "\"";
    }
    os << ",";
    os << "\"ok\":" << (ok ? "true" : "false") << ",";
    os << "\"error\":";
    if (ok || error.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(error) << "\"";
    }
    os << ",";
    os << "\"reason\":";
    if (reason.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(reason) << "\"";
    }
    os << "}";
    return os.str();
}

std::string BuildDockActionResultJson(const std::string& action_type,
                                      const std::string& request_id,
                                      const std::string& status,
                                      bool ok,
                                      const std::string& error,
                                      const std::string& detail) {
    std::ostringstream os;
    os << "{";
    os << "\"actionType\":";
    if (action_type.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(action_type) << "\"";
    }
    os << ",";
    os << "\"requestId\":";
    if (request_id.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(request_id) << "\"";
    }
    os << ",";
    os << "\"status\":\"" << JsonEscape(status.empty() ? "unknown" : status) << "\",";
    os << "\"ok\":" << (ok ? "true" : "false") << ",";
    os << "\"error\":";
    if (error.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(error) << "\"";
    }
    os << ",";
    os << "\"detail\":";
    if (detail.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(detail) << "\"";
    }
    os << "}";
    return os.str();
}

// ---------------------------------------------------------------------------
// High-level dock emission helpers
// ---------------------------------------------------------------------------
void EmitDockActionResult(const std::string& action_type,
                          const std::string& request_id,
                          const std::string& status,
                          bool ok,
                          const std::string& error,
                          const std::string& detail) {
    const std::string payload_json =
        BuildDockActionResultJson(action_type, request_id, status, ok, error, detail);
    blog(
        LOG_INFO,
        "[aegis-obs-plugin] dock action result: action_type=%s request_id=%s status=%s ok=%s error=%s detail=%s",
        action_type.empty() ? "" : action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str(),
        status.empty() ? "" : status.c_str(),
        ok ? "true" : "false",
        error.empty() ? "" : error.c_str(),
        detail.empty() ? "" : detail.c_str());
    CacheDockActionResultForReplay(payload_json);
    if (!EmitDockNativeJsonArgCall("receiveDockActionResultJson", payload_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(DockFallbackLogKind::DockActionResultJson, &phase, &attempt)) {
            blog(
                LOG_DEBUG,
                "[aegis-obs-plugin] dock bridge fallback payload phase=%s attempt=%u receiveDockActionResultJson=%s",
                phase ? phase : "unknown",
                attempt,
                payload_json.c_str());
        }
    }
}

void EmitDockSceneSwitchCompleted(const std::string& request_id,
                                  const std::string& scene_name,
                                  bool ok,
                                  const std::string& error,
                                  const std::string& reason) {
    const std::string payload_json =
        BuildSceneSwitchCompletedJson(request_id, scene_name, ok, error, reason);
    CacheDockSceneSwitchCompletedForReplay(payload_json);
    if (!EmitDockNativeJsonArgCall("receiveSceneSwitchCompletedJson", payload_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(
                DockFallbackLogKind::SceneSwitchCompletedJson, &phase, &attempt)) {
            blog(
                LOG_INFO,
                "[aegis-obs-plugin] dock bridge fallback payload phase=%s attempt=%u receiveSceneSwitchCompletedJson=%s",
                phase ? phase : "unknown",
                attempt,
                payload_json.c_str());
        }
    }
}

bool EmitDockSceneSnapshotPayload(const std::string& payload_json) {
    CacheDockSceneSnapshotForReplay(payload_json);
    DockSceneSnapshotEmitterFn emitter_copy;
    {
        std::lock_guard<std::mutex> lock(g_dock_scene_snapshot_emitter_mu);
        emitter_copy = g_dock_scene_snapshot_emitter;
    }
    bool delivered = false;
    if (!emitter_copy) {
        delivered = EmitDockNativeJsonArgCall("receiveSceneSnapshotJson", payload_json);
    } else {
        emitter_copy(payload_json);
        delivered = true;
    }
    const DockJsSinkProbeState sink_state = GetDockJsSinkProbeState();
    blog(
        delivered ? LOG_DEBUG : LOG_INFO,
        "[aegis-obs-plugin] dock scene snapshot dispatch: delivered=%s via=%s bytes=%d js_sink=%s page_ready=%s",
        delivered ? "true" : "false",
        emitter_copy ? "emitter" : "native_js_call",
        static_cast<int>(payload_json.size()),
        sink_state.js_sink_registered ? "true" : "false",
        sink_state.page_ready ? "true" : "false");
    return delivered;
}

// ---------------------------------------------------------------------------
// Relay action result resolution
// ---------------------------------------------------------------------------
void HandleRelayActionResultIfPresent(const std::string& envelope_json) {
    // Quick-exit: only process relay_action_result envelopes.
    if (envelope_json.find("relay_action_result") == std::string::npos) {
        return;
    }
    const std::string envelope_type = TryExtractEnvelopeTypeFromJson(envelope_json);
    if (envelope_type != "relay_action_result") {
        return;
    }

    std::string request_id;
    std::string action_type;
    bool ok = false;
    std::string error;

    (void)TryExtractJsonStringField(envelope_json, "request_id", &request_id);
    (void)TryExtractJsonStringField(envelope_json, "action_type", &action_type);
    (void)TryExtractJsonBoolField(envelope_json, "ok", &ok);
    (void)TryExtractJsonStringField(envelope_json, "error", &error);

    bool pending_match = false;
    if (!request_id.empty()) {
        std::lock_guard<std::mutex> lock(GetPendingRelayActionsMutex());
        auto& pending = GetPendingRelayActionsRef();
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->request_id == request_id) {
                if (action_type.empty()) {
                    action_type = it->action_type;
                }
                it = pending.erase(it);
                pending_match = true;
                break;
            }
        }
    }

    blog(
        LOG_INFO,
        "[aegis-obs-plugin] relay action result resolved: request_id=%s action_type=%s pending_match=%s ok=%s",
        request_id.c_str(),
        action_type.c_str(),
        pending_match ? "true" : "false",
        ok ? "true" : "false");

    if (pending_match && !action_type.empty() && !request_id.empty()) {
        EmitDockActionResult(
            action_type,
            request_id,
            ok ? "completed" : "failed",
            ok,
            ok ? "" : error,
            "");
    }
}

#endif // AEGIS_OBS_PLUGIN_BUILD
