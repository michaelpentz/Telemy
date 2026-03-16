#include "dock_js_bridge_api.h"
#include "config_vault.h"
#include "metrics_collector.h"
#include "https_client.h"
#include "relay_client.h"
#include "dock_theme.h"
#include "dock_replay_cache.h"
#include "dock_action_dispatch.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
#include "obs_browser_dock_host_scaffold.h"
#endif
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("aegis-obs-plugin", "en-US")

namespace {

// ---------------------------------------------------------------------------
// Named constants (RF-031)
// ---------------------------------------------------------------------------
static constexpr float kThemePollIntervalSec       = 0.5f;   // OBS theme change poll
static constexpr float kSwitchPumpMinIntervalSec   = 0.05f;  // scene-switch drain cadence
static constexpr float kHealthDegradedDropPct      = 0.05f;  // per-output drop % threshold
static constexpr float kHealthDegradedMissPct      = 0.05f;  // render-miss % threshold

bool g_obs_timer_registered = false;
bool g_frontend_event_callback_registered = false;
bool g_frontend_exit_seen = false;
bool g_tools_menu_show_dock_registered = false;
float g_switch_pump_accum_seconds = 0.0f;
float g_theme_poll_accum_seconds = 0.0f;
float g_metrics_poll_accum_seconds = 0.0f;
aegis::MetricsCollector g_metrics;
bool g_dock_action_selftest_attempted = false;

} // namespace

// ---------------------------------------------------------------------------
// Globals — defined here (single definition), referenced via extern by
// dock_action_dispatch.cpp.
// ---------------------------------------------------------------------------
aegis::Vault       g_vault;
aegis::PluginConfig g_config;
aegis::HttpsClient g_http;
std::unique_ptr<aegis::RelayClient> g_relay;
std::mutex g_relay_worker_mu;
std::thread g_relay_start_worker;
std::thread g_relay_stop_worker;
std::atomic<bool> g_relay_start_cancel{false};

// ---------------------------------------------------------------------------
// Shared utility functions — called from dock_replay_cache.cpp and
// dock_action_dispatch.cpp via forward declarations.
// ---------------------------------------------------------------------------

std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string JsStringLiteral(const std::string& input) {
    return std::string("\"") + JsonEscape(input) + "\"";
}

std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json) {
    const std::string needle = "\"type\":\"";
    const std::size_t start = envelope_json.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + needle.size();
    const std::size_t end = envelope_json.find('"', value_start);
    if (end == std::string::npos || end <= value_start) {
        return {};
    }
    return envelope_json.substr(value_start, end - value_start);
}

bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value) {
    if (!field_name || !out_value) {
        return false;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(json_text), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    QJsonValue val = doc.object().value(QString::fromUtf8(field_name));
    if (!val.isString()) {
        return false;
    }
    *out_value = val.toString().toStdString();
    return true;
}

bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value) {
    if (!field_name || !out_value) {
        return false;
    }

    std::ostringstream needle;
    needle << "\"" << field_name << "\"";
    const std::string needle_str = needle.str();
    const std::size_t key_pos = json_text.find(needle_str);
    if (key_pos == std::string::npos) {
        return false;
    }

    const std::size_t colon_pos = json_text.find(':', key_pos + needle_str.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::size_t pos = colon_pos + 1;
    while (pos < json_text.size() && std::isspace(static_cast<unsigned char>(json_text[pos])) != 0) {
        pos += 1;
    }
    if (json_text.compare(pos, 4, "true") == 0) {
        *out_value = true;
        return true;
    }
    if (json_text.compare(pos, 5, "false") == 0) {
        *out_value = false;
        return true;
    }
    return false;
}

namespace {

bool IsEnvEnabled(const char* name) {
    if (!name || *name == '\0') {
        return false;
    }
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return false;
    }
    std::string value(raw);
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return !(value == "0" || value == "false" || value == "no" || value == "off");
}

} // namespace

// ---------------------------------------------------------------------------
// Relay session detail JSON builder — called from dock_action_dispatch.cpp
// ---------------------------------------------------------------------------
std::string BuildRelaySessionDetailJson(const aegis::RelaySession& session) {
    std::ostringstream detail;
    detail << "{\"session_id\":\"" << JsonEscape(session.session_id) << "\""
           << ",\"status\":\"" << JsonEscape(session.status) << "\""
           << ",\"region\":\"" << JsonEscape(session.region) << "\""
           << ",\"public_ip\":\"" << JsonEscape(session.public_ip) << "\""
           << ",\"srt_port\":" << session.srt_port;
    if (!session.relay_hostname.empty()) {
        detail << ",\"relay_hostname\":\"" << JsonEscape(session.relay_hostname) << "\"";
    }
    detail << ",\"grace_window_seconds\":" << session.grace_window_seconds
           << ",\"max_session_seconds\":" << session.max_session_seconds
           << ",\"provision_step\":\"" << JsonEscape(session.provision_step) << "\"";
    if (!session.stream_token.empty()) {
        detail << ",\"stream_token\":\"" << JsonEscape(session.stream_token) << "\"";
    }
    detail << "}";
    return detail.str();
}

// ---------------------------------------------------------------------------
// Health derivation
// ---------------------------------------------------------------------------
namespace {

std::string DeriveHealthFromSnapshot(const aegis::MetricsSnapshot& snapshot) {
    std::string health = "good";
    if (!snapshot.obs.streaming && !snapshot.obs.recording) {
        return "offline";
    }
    float total_drop = 0.0f;
    int active_count = 0;
    for (const auto& out : snapshot.outputs) {
        if (!out.active) {
            continue;
        }
        total_drop += out.drop_pct;
        active_count++;
    }
    if (active_count > 0 && (total_drop / active_count) > kHealthDegradedDropPct) {
        health = "degraded";
    }
    if (snapshot.obs.render_total_frames > 0) {
        const float miss_pct = static_cast<float>(snapshot.obs.render_missed_frames) /
                               static_cast<float>(snapshot.obs.render_total_frames);
        if (miss_pct > kHealthDegradedMissPct) {
            health = "degraded";
        }
    }
    return health;
}

} // namespace

// ---------------------------------------------------------------------------
// Status snapshot emission — called from dock_action_dispatch.cpp
// ---------------------------------------------------------------------------
bool EmitCurrentStatusSnapshotToDock(const char* reason, bool force_poll) {
    if (force_poll) {
        g_metrics.Poll();
    }

    const auto& snapshot = g_metrics.Latest();
    const std::string mode = EffectiveDockModeFromConfig();
    const std::string health = DeriveHealthFromSnapshot(snapshot);
    std::string relay_status = "inactive";
    std::string relay_region;
    const aegis::RelaySession* relay_session_ptr = nullptr;
    std::optional<aegis::RelaySession> relay_session_holder;
    if (g_relay && g_relay->HasActiveSession()) {
        relay_session_holder = g_relay->CurrentSession();
        if (relay_session_holder) {
            relay_status = relay_session_holder->status;
            relay_region = relay_session_holder->region;
            relay_session_ptr = &(*relay_session_holder);
        }
    }

    // Poll SLS stats every ~2 seconds (4 ticks at 500ms) when relay is active
    const aegis::RelayStats* relay_stats_ptr = nullptr;
    aegis::RelayStats relay_stats;
    if (g_relay && g_relay->HasActiveSession() && relay_session_ptr) {
        static int stats_poll_counter = 0;
        if (++stats_poll_counter >= 4) {
            stats_poll_counter = 0;
            g_relay->PollRelayStats(relay_session_ptr->public_ip);
        }
        relay_stats = g_relay->CurrentStats();
        relay_stats_ptr = &relay_stats;
    }

    // Poll per-link stats on the same cadence as SLS stats
    const aegis::PerLinkSnapshot* per_link_ptr = nullptr;
    aegis::PerLinkSnapshot per_link_stats;
    if (g_relay && g_relay->HasActiveSession() && relay_session_ptr) {
        static int per_link_poll_counter = 0;
        if (++per_link_poll_counter >= 4) {
            per_link_poll_counter = 0;
            g_relay->PollPerLinkStats(relay_session_ptr->public_ip);
        }
        per_link_stats = g_relay->CurrentPerLinkStats();
        per_link_ptr = &per_link_stats;
    }

    std::string json =
        g_metrics.BuildStatusSnapshotJson(mode, health, relay_status, relay_region,
                                           relay_session_ptr, relay_stats_ptr, per_link_ptr);
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (doc.isObject()) {
        QJsonObject payload = doc.object();
        payload.insert(QStringLiteral("settings"), BuildDockSettingsJsonFromConfig());
        json = QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
    }
    json = AugmentSnapshotJsonWithTheme(json);

    const bool delivered = EmitDockNativeJsonArgCall("receiveStatusSnapshotJson", json);
    {
        std::lock_guard<std::mutex> lock(GetDockReplayCacheMutex());
        auto& cache = GetDockReplayCacheRef();
        cache.has_status_snapshot = true;
        cache.status_snapshot_json = json;
    }
    blog(
        delivered ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-plugin] status snapshot emitted: delivered=%s reason=%s mode=%s",
        delivered ? "true" : "false",
        reason ? reason : "unknown",
        mode.c_str());
    return delivered;
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------
namespace {

std::string CurrentSceneName() {
    obs_source_t* current = obs_frontend_get_current_scene();
    if (!current) {
        return {};
    }

    const char* current_name = obs_source_get_name(current);
    std::string out = current_name ? std::string(current_name) : std::string();
    obs_source_release(current);
    return out;
}

std::vector<std::string> SnapshotSceneNames() {
    obs_frontend_source_list sources = {};
    obs_frontend_get_scenes(&sources);

    std::vector<std::string> names;
    names.reserve(sources.sources.num);
    for (size_t i = 0; i < sources.sources.num; ++i) {
        obs_source_t* src = sources.sources.array[i];
        if (!src) {
            continue;
        }
        const char* name = obs_source_get_name(src);
        names.push_back(name ? std::string(name) : std::string());
    }
    obs_frontend_source_list_free(&sources);
    return names;
}

std::string BuildDockSceneSnapshotPayloadJson(
    const char* reason,
    const std::vector<std::string>& scene_names,
    const std::string& current_scene_name) {
    std::ostringstream os;
    os << "{";
    os << "\"reason\":\"" << JsonEscape(reason ? reason : "unknown") << "\",";
    os << "\"sceneNames\":[";
    for (size_t i = 0; i < scene_names.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << "\"" << JsonEscape(scene_names[i]) << "\"";
    }
    os << "],";
    os << "\"currentSceneName\":";
    if (current_scene_name.empty()) {
        os << "null";
    } else {
        os << "\"" << JsonEscape(current_scene_name) << "\"";
    }
    os << "}";
    return os.str();
}

} // namespace

// ---------------------------------------------------------------------------
// Scene snapshot logging — called from dock_action_dispatch.cpp
// ---------------------------------------------------------------------------
void LogSceneSnapshot(const char* reason) {
    const auto names = SnapshotSceneNames();
    const std::string current = CurrentSceneName();
    const std::string dock_payload_json =
        BuildDockSceneSnapshotPayloadJson(reason, names, current);

    blog(
        LOG_INFO,
        "[aegis-obs-plugin] obs scene snapshot: reason=%s current=\"%s\" count=%d",
        reason ? reason : "unknown",
        current.empty() ? "" : current.c_str(),
        static_cast<int>(names.size()));
    if (!EmitDockSceneSnapshotPayload(dock_payload_json)) {
        const char* phase = nullptr;
        std::uint32_t attempt = 0;
        if (ShouldLogDockFallbackPayload(
                DockFallbackLogKind::SceneSnapshotJson, &phase, &attempt)) {
            blog(
                LOG_INFO,
                "[aegis-obs-plugin] dock bridge fallback payload phase=%s attempt=%u setObsSceneSnapshot=%s",
                phase ? phase : "unknown",
                attempt,
                dock_payload_json.c_str());
        }
    }

    for (size_t i = 0; i < names.size(); ++i) {
        blog(LOG_DEBUG, "[aegis-obs-plugin] scene[%d]=\"%s\"", static_cast<int>(i), names[i].c_str());
    }
}

// ---------------------------------------------------------------------------
// Theme poll wrapper — bridges dock_theme and dock_replay_cache
// ---------------------------------------------------------------------------
namespace {

void PollObsThemeChangesOnObsThread() {
    const ObsDockThemeSlots before = GetCachedObsDockTheme();
    RefreshCachedObsDockThemeFromQt("tick_poll");
    const ObsDockThemeSlots after = GetCachedObsDockTheme();
    if (!after.valid) {
        return;
    }
    const bool changed = !before.valid ||
        before.bg != after.bg ||
        before.surface != after.surface ||
        before.panel != after.panel ||
        before.text != after.text ||
        before.textMuted != after.textMuted ||
        before.accent != after.accent ||
        before.border != after.border ||
        before.scrollbar != after.scrollbar;
    if (changed) {
        ReemitDockStatusSnapshotWithCurrentTheme("tick_poll");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Browser dock host bridge
// ---------------------------------------------------------------------------
namespace {

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
void InitializeBrowserDockHostBridge() {
    aegis_obs_browser_dock_host_scaffold_initialize();
}

void ShutdownBrowserDockHostBridge() {
    aegis_obs_browser_dock_host_scaffold_shutdown();
}
#else
void InitializeBrowserDockHostBridge() {
    RegisterDockBrowserJsExecuteSink({});
    blog(LOG_INFO, "[aegis-obs-plugin] browser dock host scaffold disabled (build flag off)");
}

void ShutdownBrowserDockHostBridge() {
    SetDockBrowserJsExecuteSink({});
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// Selftest
// ---------------------------------------------------------------------------
namespace {

void MaybeRunDockActionSelfTestAfterPageReady() {
    if (g_dock_action_selftest_attempted) {
        return;
    }
    g_dock_action_selftest_attempted = true;

    if (!IsEnvEnabled("AEGIS_DOCK_ENABLE_SELFTEST")) {
        return;
    }

    const char* raw_action_json = std::getenv("AEGIS_DOCK_SELFTEST_ACTION_JSON");
    if (!raw_action_json || !*raw_action_json) {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock selftest enabled but no action json provided (AEGIS_DOCK_SELFTEST_ACTION_JSON)");
        return;
    }

    const std::string action_json(raw_action_json);
    const char* raw_direct = std::getenv("AEGIS_DOCK_SELFTEST_DIRECT_PLUGIN_INTAKE");
    const bool direct_intake = (raw_direct && *raw_direct && std::string(raw_direct) != "0");
    if (direct_intake) {
        const bool accepted = aegis_obs_shim_receive_dock_action_json(action_json.c_str());
        blog(
            accepted ? LOG_INFO : LOG_WARNING,
            "[aegis-obs-plugin] dock selftest direct plugin intake ok=%s json=%s",
            accepted ? "true" : "false",
            action_json.c_str());
        return;
    }

    std::ostringstream js;
    js << "(function(){"
          "var payload="
       << JsStringLiteral(action_json)
       << ";"
          "var sent=false;"
          "if(window.aegisDockNative&&typeof window.aegisDockNative.sendDockActionJson==='function'){"
          "  try{ window.aegisDockNative.sendDockActionJson(payload); sent=true; }catch(_e){}"
          "}"
          "if(typeof document!=='undefined'&&typeof document.title==='string'&&typeof encodeURIComponent==='function'){"
          "  try{ document.title='__AEGIS_DOCK_ACTION__:'+encodeURIComponent(payload); sent=true; }catch(_e){}"
          "}"
          "if(typeof location!=='undefined'&&typeof location.hash==='string'&&typeof encodeURIComponent==='function'){"
          "  try{ location.hash='__AEGIS_DOCK_ACTION__:'+encodeURIComponent(payload); sent=true; }catch(_e){}"
          "}"
          "return sent; })();";

    const bool dispatched = TryExecuteDockBrowserJs(js.str());
    blog(
        dispatched ? LOG_INFO : LOG_WARNING,
        "[aegis-obs-plugin] dock selftest action dispatch page_ready ok=%s json=%s (path=native_api_plus_title_hash)",
        dispatched ? "true" : "false",
        action_json.c_str());
}

} // namespace

// ---------------------------------------------------------------------------
// Frontend event handling
// ---------------------------------------------------------------------------
namespace {

const char* FrontendEventName(enum obs_frontend_event event) {
    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        return "SCENE_CHANGED";
    case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
        return "SCENE_LIST_CHANGED";
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        return "SCENE_COLLECTION_CHANGED";
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
        return "SCENE_COLLECTION_CHANGING";
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        return "FINISHED_LOADING";
    case OBS_FRONTEND_EVENT_THEME_CHANGED:
        return "THEME_CHANGED";
    case OBS_FRONTEND_EVENT_EXIT:
        return "EXIT";
    default:
        return nullptr;
    }
}

void OnFrontendEvent(enum obs_frontend_event event, void*) {
    const char* event_name = FrontendEventName(event);
    if (!event_name) {
        return;
    }

    blog(LOG_INFO, "[aegis-obs-plugin] frontend event: %s", event_name);

    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        LogSceneSnapshot(event_name);
        RefreshCachedObsDockThemeFromQt(event_name);
        break;
    case OBS_FRONTEND_EVENT_THEME_CHANGED:
        RefreshCachedObsDockThemeFromQt(event_name);
        ReemitDockStatusSnapshotWithCurrentTheme(event_name);
        break;
    case OBS_FRONTEND_EVENT_EXIT:
        g_frontend_exit_seen = true;
        ShutdownBrowserDockHostBridge();
        break;
    default:
        break;
    }
}

void OnToolsMenuShowDock(void*) {
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
    const bool ok = aegis_obs_browser_dock_host_scaffold_show_dock();
    blog(
        ok ? LOG_INFO : LOG_WARNING,
        "[aegis-obs-plugin] tools menu action: show dock -> %s",
        ok ? "ok" : "no_dock_widget");
#else
    blog(LOG_WARNING, "[aegis-obs-plugin] tools menu action: show dock unavailable (dock host disabled)");
#endif
}

// ---------------------------------------------------------------------------
// Tick callback
// ---------------------------------------------------------------------------
void SwitchScenePumpTick(void*, float seconds) {
    if (seconds > 0.0f) {
        g_switch_pump_accum_seconds += seconds;
        g_theme_poll_accum_seconds += seconds;
        g_metrics_poll_accum_seconds += seconds;
    }
    DrainExpiredPendingDockActions();
    if (g_theme_poll_accum_seconds >= kThemePollIntervalSec) {
        g_theme_poll_accum_seconds = 0.0f;
        PollObsThemeChangesOnObsThread();
    }
    const float metrics_interval_sec = static_cast<float>(
        std::max(100, std::min(g_config.metrics_poll_interval_ms, 5000))) / 1000.0f;
    if (g_metrics_poll_accum_seconds >= metrics_interval_sec) {
        g_metrics_poll_accum_seconds = 0.0f;
        EmitCurrentStatusSnapshotToDock("metrics_poll", true);
    }
    if (g_switch_pump_accum_seconds < kSwitchPumpMinIntervalSec) {
        return;
    }
    g_switch_pump_accum_seconds = 0.0f;
    DrainSwitchSceneRequestsOnObsThread();
}

} // namespace

// ---------------------------------------------------------------------------
// Extern "C" dock JS bridge API implementations
// ---------------------------------------------------------------------------

extern "C" void aegis_obs_shim_register_dock_js_executor(
    aegis_dock_js_execute_fn fn,
    void* user_data) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    if (!fn) {
        RegisterDockBrowserJsExecuteSink({});
        return;
    }
    RegisterDockBrowserJsExecuteSink(
        [fn, user_data](const std::string& js_code) -> bool {
            return fn(js_code.c_str(), user_data);
        });
#else
    (void)fn;
    (void)user_data;
#endif
}

extern "C" void aegis_obs_shim_clear_dock_js_executor(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    RegisterDockBrowserJsExecuteSink({});
#endif
}

extern "C" void aegis_obs_shim_replay_dock_state(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    ReplayDockStateToJsSinkIfAvailable();
#endif
}

extern "C" void aegis_obs_shim_notify_dock_page_ready(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    {
        std::lock_guard<std::mutex> lock(GetDockJsDeliveryValidationMutex());
        auto& state = GetDockJsDeliveryValidationRef();
        state.page_ready = true;
        state.logged_receive_status_snapshot_json = false;
        state.logged_receive_scene_snapshot_json = false;
        state.logged_receive_scene_switch_completed_json = false;
        state.logged_receive_dock_action_result_json = false;
        state.fallback_pipe_status_count = 0;
        state.fallback_scene_snapshot_count = 0;
        state.fallback_scene_switch_completed_count = 0;
        state.fallback_dock_action_result_count = 0;
    }
    ReplayDockStateToJsSinkIfAvailable();
    MaybeRunDockActionSelfTestAfterPageReady();

    QTimer::singleShot(1000, qApp, []() {
        blog(LOG_INFO,
             "[aegis-obs-plugin] deferred page-ready scene snapshot firing");
        LogSceneSnapshot("page_ready_deferred");
    });
#endif
}

extern "C" void aegis_obs_shim_notify_dock_page_unloaded(void) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    {
        std::lock_guard<std::mutex> lock(GetDockJsDeliveryValidationMutex());
        GetDockJsDeliveryValidationRef().page_ready = false;
    }
    RegisterDockBrowserJsExecuteSink({});
#endif
}

extern "C" bool aegis_obs_shim_receive_dock_action_json(const char* action_json_utf8) {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    if (!action_json_utf8 || *action_json_utf8 == '\0') {
        EmitDockActionResult("", "", "rejected", false, "empty_action_json", "");
        return false;
    }

    const std::string action_json(action_json_utf8);
    std::string action_type;
    if (!TryExtractJsonStringField(action_json, "type", &action_type) || action_type.empty()) {
        blog(LOG_WARNING, "[aegis-obs-plugin] dock action parse rejected: missing type");
        EmitDockActionResult("", "", "rejected", false, "missing_action_type", "");
        return false;
    }

    std::string request_id;
    (void)TryExtractJsonStringField(action_json, "requestId", &request_id);
    if (request_id.empty()) {
        (void)TryExtractJsonStringField(action_json, "request_id", &request_id);
    }
    blog(
        LOG_INFO,
        "[aegis-obs-plugin] dock action parse: type=%s request_id=%s bytes=%d",
        action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str(),
        static_cast<int>(action_json.size()));

    // Ensure a request_id exists for dedup and result tracking.
    if (request_id.empty()) {
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        // Use a simple local counter for uniqueness.
        static std::mutex seq_mu;
        static std::uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(seq_mu);
            seq += 1;
            request_id = "dock_" + std::to_string(now_ms) + "_" + std::to_string(seq);
        }
    }
    if (ShouldDeduplicateDockActionByRequestId(action_type, request_id)) {
        blog(
            LOG_DEBUG,
            "[aegis-obs-plugin] dock action deduplicated: type=%s request_id=%s",
            action_type.c_str(),
            request_id.c_str());
        return true;
    }

    return DispatchDockAction(action_json, action_type, request_id);
#else
    (void)action_json_utf8;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// OBS module lifecycle
// ---------------------------------------------------------------------------

bool obs_module_load(void) {
    blog(LOG_INFO, "[aegis-obs-plugin] module load");

    g_vault.Load();
    g_config.LoadFromDisk();
    const std::optional<std::string> relay_shared_key = g_vault.Get("relay_shared_key");

    if (!g_config.relay_api_host.empty()) {
        g_relay = std::make_unique<aegis::RelayClient>(
            g_http, g_config.relay_api_host, relay_shared_key.value_or(""));
        blog(LOG_INFO,
            "[aegis-obs-plugin] relay client initialized: host=%s shared_key=%s",
            g_config.relay_api_host.c_str(),
            relay_shared_key.has_value() ? "configured" : "missing");
    } else {
        blog(LOG_INFO, "[aegis-obs-plugin] relay client skipped: relay_api_host not configured");
    }

    if (!g_obs_timer_registered) {
        obs_add_tick_callback(SwitchScenePumpTick, nullptr);
        g_obs_timer_registered = true;
        g_switch_pump_accum_seconds = 0.0f;
        g_theme_poll_accum_seconds = 0.0f;
        g_metrics_poll_accum_seconds = 0.0f;
        blog(LOG_INFO, "[aegis-obs-plugin] registered switch-scene pump timer");
    }

    SetDockSceneSnapshotEmitter({});
    InitializeBrowserDockHostBridge();
    g_frontend_exit_seen = false;

    CacheDockPipeStatusForReplay("ok", "native plugin v0.0.4");

    if (!g_frontend_event_callback_registered) {
        obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
        g_frontend_event_callback_registered = true;
        blog(LOG_INFO, "[aegis-obs-plugin] registered frontend event callback");
    }
    if (!g_tools_menu_show_dock_registered) {
        obs_frontend_add_tools_menu_item("Show Aegis Dock (Telemy)", OnToolsMenuShowDock, nullptr);
        g_tools_menu_show_dock_registered = true;
        blog(LOG_INFO, "[aegis-obs-plugin] registered Tools menu item: Show Aegis Dock (Telemy)");
    }
    LogSceneSnapshot("module_load");

    return true;
}

void obs_module_unload(void) {
    blog(LOG_INFO, "[aegis-obs-plugin] module unload");
    if (g_frontend_event_callback_registered) {
        if (!g_frontend_exit_seen) {
            obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
        } else {
            blog(
                LOG_INFO,
                "[aegis-obs-plugin] skipping frontend callback remove after EXIT event");
        }
        g_frontend_event_callback_registered = false;
    }
    g_tools_menu_show_dock_registered = false;
    if (g_obs_timer_registered) {
        obs_remove_tick_callback(SwitchScenePumpTick, nullptr);
        g_obs_timer_registered = false;
        g_switch_pump_accum_seconds = 0.0f;
        g_theme_poll_accum_seconds = 0.0f;
        g_metrics_poll_accum_seconds = 0.0f;
    }

    ClearAllPendingDockActions();
    g_dock_action_selftest_attempted = false;
    SetDockSceneSnapshotEmitter({});
    ShutdownBrowserDockHostBridge();
    ClearDockReplayCache();

    // Emergency relay teardown
    if (g_relay && g_relay->HasActiveSession()) {
        blog(LOG_INFO, "[aegis-obs-plugin] relay emergency stop on unload");
        auto jwt = g_vault.Get("relay_jwt");
        if (jwt) {
            g_relay->EmergencyRelayStop(*jwt);
        } else {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] relay emergency stop skipped: no relay_jwt in vault");
        }
    }
    // Signal cancellation and join any outstanding relay worker threads
    // before destroying g_relay.
    g_relay_start_cancel.store(true);
    {
        std::lock_guard<std::mutex> lk(g_relay_worker_mu);
        if (g_relay_start_worker.joinable()) {
            blog(LOG_INFO, "[aegis-obs-plugin] joining relay_start worker on unload");
            g_relay_start_worker.join();
        }
        if (g_relay_stop_worker.joinable()) {
            blog(LOG_INFO, "[aegis-obs-plugin] joining relay_stop worker on unload");
            g_relay_stop_worker.join();
        }
    }
    // Destroy g_relay before g_http -- RelayClient holds a reference to g_http.
    g_relay.reset();
    blog(LOG_INFO, "[aegis-obs-plugin] relay client destroyed");
}

const char* obs_module_description(void) {
    return "Aegis OBS Plugin (v0.0.4)";
}

#else
int aegis_obs_plugin_entry_placeholder() {
    return 0;
}
#endif
