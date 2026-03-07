#include "dock_js_bridge_api.h"
#include "config_vault.h"
#include "metrics_collector.h"
#include "https_client.h"
#include "relay_client.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
#include "obs_browser_dock_host_scaffold.h"
#endif
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QTimer>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("aegis-obs-plugin", "en-US")

namespace {

struct PendingSwitchRequest {
    std::string request_id;
    std::string scene_name;
    std::string reason;
};

std::mutex g_pending_switch_requests_mu;
std::vector<PendingSwitchRequest> g_pending_switch_requests;
std::mutex g_pending_request_status_action_ids_mu;
std::vector<std::string> g_pending_request_status_action_ids;
struct PendingSetModeAction {
    std::string request_id;
    std::string mode;
    std::chrono::steady_clock::time_point queued_at;
};
struct PendingSetSettingAction {
    std::string request_id;
    std::string key;
    bool value = false;
    std::chrono::steady_clock::time_point queued_at;
};
struct PendingRelayAction {
    std::string request_id;
    std::string action_type;
    std::chrono::steady_clock::time_point queued_at;
};
std::mutex g_pending_set_mode_actions_mu;
std::vector<PendingSetModeAction> g_pending_set_mode_actions;
std::mutex g_pending_set_setting_actions_mu;
std::vector<PendingSetSettingAction> g_pending_set_setting_actions;
std::mutex g_pending_relay_actions_mu;
std::vector<PendingRelayAction> g_pending_relay_actions;
constexpr std::chrono::milliseconds kDockActionCompletionTimeoutMs(3000);
constexpr std::chrono::milliseconds kDockActionDuplicateWindowMs(1500);
std::uint64_t g_local_dock_action_seq = 0;
std::mutex g_recent_dock_actions_mu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_recent_dock_actions;
bool g_obs_timer_registered = false;
bool g_frontend_event_callback_registered = false;
bool g_frontend_exit_seen = false;
bool g_tools_menu_show_dock_registered = false;
float g_switch_pump_accum_seconds = 0.0f;
float g_theme_poll_accum_seconds = 0.0f;
float g_metrics_poll_accum_seconds = 0.0f;
aegis::MetricsCollector g_metrics;
using DockSceneSnapshotEmitterFn = std::function<void(const std::string&)>;
using DockBrowserJsExecuteFn = std::function<bool(const std::string&)>;
std::mutex g_dock_scene_snapshot_emitter_mu;
DockSceneSnapshotEmitterFn g_dock_scene_snapshot_emitter;
std::mutex g_dock_browser_js_execute_mu;
DockBrowserJsExecuteFn g_dock_browser_js_execute;
struct DockJsDeliveryValidationState {
    bool page_ready = false;
    bool js_sink_registered = false;
    bool logged_receive_status_snapshot_json = false;
    bool logged_receive_scene_snapshot_json = false;
    bool logged_receive_scene_switch_completed_json = false;
    bool logged_receive_dock_action_result_json = false;
    std::uint32_t fallback_pipe_status_count = 0;
    std::uint32_t fallback_status_snapshot_count = 0;
    std::uint32_t fallback_scene_snapshot_count = 0;
    std::uint32_t fallback_scene_switch_completed_count = 0;
    std::uint32_t fallback_dock_action_result_count = 0;
};
std::mutex g_dock_js_delivery_validation_mu;
DockJsDeliveryValidationState g_dock_js_delivery_validation;
struct DockReplayCache {
    // v0.0.4: native status snapshot (from BuildStatusSnapshotJson)
    bool has_status_snapshot = false;
    std::string status_snapshot_json;
    bool has_scene_snapshot = false;
    std::string scene_snapshot_json;
    bool has_pipe_status = false;
    std::string pipe_status;
    std::string pipe_reason;
    bool has_current_scene = false;
    std::string current_scene_name;
    bool has_scene_switch_completed = false;
    std::string scene_switch_completed_json;
    bool has_dock_action_result = false;
    std::string dock_action_result_json;
};
std::mutex g_dock_replay_cache_mu;
DockReplayCache g_dock_replay_cache;
bool g_dock_action_selftest_attempted = false;

// Vault and config â€” loaded once in obs_module_load, accessible throughout.
aegis::Vault       g_vault;
aegis::PluginConfig g_config;

// HTTPS client and relay client â€” g_http owns the WinHTTP session;
// g_relay holds a reference to g_http so it must be destroyed first.
aegis::HttpsClient g_http;
std::unique_ptr<aegis::RelayClient> g_relay;

struct DockJsSinkProbeState {
    bool js_sink_registered = false;
    bool page_ready = false;
};

std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json);
bool EmitDockNativeJsonArgCall(const char* method_name, const std::string& payload_json);
std::string AugmentSnapshotJsonWithTheme(const std::string& snapshot_json);
void ReemitDockStatusSnapshotWithCurrentTheme(const char* reason);
void EmitDockActionResult(const std::string& action_type,
                          const std::string& request_id,
                          const std::string& status,
                          bool ok,
                          const std::string& error,
                          const std::string& detail);
void HandleRelayActionResultIfPresent(const std::string& envelope_json);

struct ObsDockThemeSlots {
    std::string bg;
    std::string surface;
    std::string panel;
    std::string text;
    std::string textMuted;
    std::string accent;
    std::string border;
    std::string scrollbar;
    bool valid = false;
};
std::mutex g_obs_dock_theme_mu;
ObsDockThemeSlots g_obs_dock_theme_cache;
std::string g_obs_dock_theme_signature;

DockJsSinkProbeState GetDockJsSinkProbeState() {
    std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
    DockJsSinkProbeState out;
    out.js_sink_registered = g_dock_js_delivery_validation.js_sink_registered;
    out.page_ready = g_dock_js_delivery_validation.page_ready;
    return out;
}

QString ColorToCssHex(const QColor& color) {
    if (!color.isValid()) {
        return QStringLiteral("#000000");
    }
    return color.name(QColor::HexRgb);
}

QColor BlendTowardWhite(const QColor& color, double ratio) {
    if (!color.isValid()) {
        return QColor(0, 0, 0);
    }
    const double clamped = std::max(0.0, std::min(1.0, ratio));
    const int r = static_cast<int>(color.red() + (255 - color.red()) * clamped);
    const int g = static_cast<int>(color.green() + (255 - color.green()) * clamped);
    const int b = static_cast<int>(color.blue() + (255 - color.blue()) * clamped);
    return QColor(r, g, b);
}

QColor BlendTowardBlack(const QColor& color, double ratio) {
    if (!color.isValid()) {
        return QColor(0, 0, 0);
    }
    const double clamped = std::max(0.0, std::min(1.0, ratio));
    const int r = static_cast<int>(color.red() * (1.0 - clamped));
    const int g = static_cast<int>(color.green() * (1.0 - clamped));
    const int b = static_cast<int>(color.blue() * (1.0 - clamped));
    return QColor(r, g, b);
}

QColor DerivedAccentLike(const QColor& base, double ratio) {
    int h = 0;
    int s = 0;
    int l = 0;
    int a = 255;
    if (!base.isValid()) {
        return QColor(96, 128, 160);
    }
    base.getHsl(&h, &s, &l, &a);
    if (l < 128) {
        return BlendTowardWhite(base, ratio);
    }
    return BlendTowardBlack(base, ratio);
}

double SrgbToLinear01(double c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return std::pow((c + 0.055) / 1.055, 2.4);
}

double RelativeLuminance(const QColor& c) {
    if (!c.isValid()) {
        return 0.0;
    }
    const double r = SrgbToLinear01(static_cast<double>(c.red()) / 255.0);
    const double g = SrgbToLinear01(static_cast<double>(c.green()) / 255.0);
    const double b = SrgbToLinear01(static_cast<double>(c.blue()) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double ContrastRatio(const QColor& a, const QColor& b) {
    const double l1 = RelativeLuminance(a);
    const double l2 = RelativeLuminance(b);
    const double hi = std::max(l1, l2);
    const double lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

double MinContrastAgainst(const QColor& fg, const std::vector<QColor>& bgs) {
    if (!fg.isValid() || bgs.empty()) {
        return 0.0;
    }
    double best = 1e9;
    for (const auto& bg : bgs) {
        if (!bg.isValid()) {
            continue;
        }
        best = std::min(best, ContrastRatio(fg, bg));
    }
    return (best == 1e9) ? 0.0 : best;
}

QColor PickReadableTextColor(
    const std::vector<QColor>& candidates,
    const std::vector<QColor>& backgrounds,
    double min_ratio) {
    QColor best = QColor(0, 0, 0);
    double best_score = -1.0;
    for (const auto& c : candidates) {
        if (!c.isValid()) {
            continue;
        }
        const double score = MinContrastAgainst(c, backgrounds);
        if (score >= min_ratio) {
            return c;
        }
        if (score > best_score) {
            best_score = score;
            best = c;
        }
    }
    const QColor black(0, 0, 0);
    const QColor white(255, 255, 255);
    const double black_score = MinContrastAgainst(black, backgrounds);
    const double white_score = MinContrastAgainst(white, backgrounds);
    return (black_score >= white_score) ? black : white;
}

ObsDockThemeSlots qt_palette_to_theme() {
    ObsDockThemeSlots out;
    const QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) {
        return out;
    }

    const QPalette pal = app->palette();
    const QColor bg = pal.color(QPalette::Window);
    const QColor surface = pal.color(QPalette::Base);
    const QColor panel = pal.color(QPalette::Button);
    const QColor raw_window_text = pal.color(QPalette::WindowText);
    const QColor raw_text = pal.color(QPalette::Text);
    const QColor raw_button_text = pal.color(QPalette::ButtonText);
    const std::vector<QColor> text_bgs = {bg, surface, panel};
    const QColor text = PickReadableTextColor(
        {raw_window_text, raw_text, raw_button_text},
        text_bgs,
        4.5);
    QColor text_muted = pal.color(QPalette::PlaceholderText);
    if (!text_muted.isValid() || text_muted.alpha() == 0) {
        text_muted = text;
        text_muted.setAlpha(153); // ~60%
    }
    // Some themes expose placeholder text with poor contrast; derive a safer muted color from text if needed.
    if (MinContrastAgainst(text_muted, text_bgs) < 2.4) {
        text_muted = (RelativeLuminance(text) < 0.5)
            ? BlendTowardWhite(text, 0.35)
            : BlendTowardBlack(text, 0.35);
    }
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor border = DerivedAccentLike(bg, 0.10);
    const QColor scrollbar = DerivedAccentLike(surface, 0.15);

    out.bg = ColorToCssHex(bg).toStdString();
    out.surface = ColorToCssHex(surface).toStdString();
    out.panel = ColorToCssHex(panel).toStdString();
    out.text = ColorToCssHex(text).toStdString();
    out.textMuted = ColorToCssHex(text_muted).toStdString();
    out.accent = ColorToCssHex(accent).toStdString();
    out.border = ColorToCssHex(border).toStdString();
    out.scrollbar = ColorToCssHex(scrollbar).toStdString();
    out.valid = true;
    return out;
}

QJsonObject QtThemeToJsonObject(const ObsDockThemeSlots& theme) {
    QJsonObject obj;
    if (!theme.valid) {
        return obj;
    }
    obj.insert(QStringLiteral("bg"), QString::fromStdString(theme.bg));
    obj.insert(QStringLiteral("surface"), QString::fromStdString(theme.surface));
    obj.insert(QStringLiteral("panel"), QString::fromStdString(theme.panel));
    obj.insert(QStringLiteral("text"), QString::fromStdString(theme.text));
    obj.insert(QStringLiteral("textMuted"), QString::fromStdString(theme.textMuted));
    obj.insert(QStringLiteral("accent"), QString::fromStdString(theme.accent));
    obj.insert(QStringLiteral("border"), QString::fromStdString(theme.border));
    obj.insert(QStringLiteral("scrollbar"), QString::fromStdString(theme.scrollbar));
    return obj;
}

ObsDockThemeSlots GetCachedObsDockTheme() {
    std::lock_guard<std::mutex> lock(g_obs_dock_theme_mu);
    return g_obs_dock_theme_cache;
}

void RefreshCachedObsDockThemeFromQt(const char* reason) {
    const ObsDockThemeSlots theme = qt_palette_to_theme();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_obs_dock_theme_mu);
        const std::string next_sig = theme.valid
            ? (theme.bg + "|" + theme.surface + "|" + theme.panel + "|" + theme.text + "|" +
               theme.textMuted + "|" + theme.accent + "|" + theme.border + "|" + theme.scrollbar)
            : std::string();
        changed = (next_sig != g_obs_dock_theme_signature);
        g_obs_dock_theme_cache = theme;
        g_obs_dock_theme_signature = next_sig;
    }
    blog(
        (theme.valid && changed) ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-plugin] obs dock theme cache refresh: valid=%s changed=%s reason=%s",
        theme.valid ? "true" : "false",
        changed ? "true" : "false",
        reason ? reason : "unknown");
}

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

std::string AugmentSnapshotJsonWithTheme(const std::string& snapshot_json) {
    const ObsDockThemeSlots cached_theme = GetCachedObsDockTheme();
    if (!cached_theme.valid || snapshot_json.empty()) {
        return snapshot_json;
    }
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(snapshot_json));
    if (!doc.isObject()) {
        return snapshot_json;
    }
    QJsonObject obj = doc.object();
    obj.insert(QStringLiteral("theme"), QtThemeToJsonObject(cached_theme));
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

void ReemitDockStatusSnapshotWithCurrentTheme(const char* reason) {
    // v0.0.4: Re-emit the cached v0.0.4 status snapshot with fresh theme.
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

struct ProvisionStepInfo {
    int number = 0;
    const char* label = "";
};

ProvisionStepInfo stepToInfo(const std::string& step) {
    if (step == "launching_instance") {
        return {1, "Launching instance..."};
    }
    if (step == "waiting_for_instance") {
        return {2, "Waiting for instance..."};
    }
    if (step == "starting_docker") {
        return {3, "Starting services..."};
    }
    if (step == "starting_containers") {
        return {4, "Starting containers..."};
    }
    if (step == "creating_stream") {
        return {5, "Creating stream..."};
    }
    if (step == "ready") {
        return {6, "Ready"};
    }
    return {};
}

std::string BuildRelaySessionDetailJson(const aegis::RelaySession& session) {
    std::ostringstream detail;
    detail << "{\"session_id\":\"" << JsonEscape(session.session_id) << "\""
           << ",\"status\":\"" << JsonEscape(session.status) << "\""
           << ",\"region\":\"" << JsonEscape(session.region) << "\""
           << ",\"public_ip\":\"" << JsonEscape(session.public_ip) << "\""
           << ",\"srt_port\":" << session.srt_port
           << ",\"pair_token\":\"" << JsonEscape(session.pair_token) << "\""
           << ",\"ws_url\":\"" << JsonEscape(session.ws_url) << "\"";
    if (!session.relay_hostname.empty()) {
        detail << ",\"relay_hostname\":\"" << JsonEscape(session.relay_hostname) << "\"";
    }
    detail << ",\"grace_window_seconds\":" << session.grace_window_seconds
           << ",\"max_session_seconds\":" << session.max_session_seconds
           << ",\"provision_step\":\"" << JsonEscape(session.provision_step) << "\""
           << "}";
    return detail.str();
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

void SetDockSceneSnapshotEmitter(DockSceneSnapshotEmitterFn emitter) {
    std::lock_guard<std::mutex> lock(g_dock_scene_snapshot_emitter_mu);
    g_dock_scene_snapshot_emitter = std::move(emitter);
}

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

bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value);
bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value);
bool IsEnvEnabled(const char* name);
bool IsRecognizedDockMode(const std::string& mode);
bool IsRecognizedDockSettingKey(const std::string& key);

enum class DockFallbackLogKind {
    PipeStatus,
    StatusSnapshotJson,
    SceneSnapshotJson,
    SceneSwitchCompletedJson,
    DockActionResultJson,
};

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
    if (pos >= json_text.size() || json_text[pos] != '"') {
        return false;
    }
    pos += 1;

    std::string value;
    value.reserve(64);
    bool escaping = false;
    for (; pos < json_text.size(); ++pos) {
        const char ch = json_text[pos];
        if (escaping) {
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            *out_value = std::move(value);
            return true;
        }
        value.push_back(ch);
    }
    return false;
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

bool IsRecognizedDockMode(const std::string& mode) {
    return mode == "studio" || mode == "irl";
}

bool IsRecognizedDockSettingKey(const std::string& key) {
    return key == "auto_scene_switch" ||
           key == "low_quality_fallback" ||
           key == "manual_override" ||
           key == "chat_bot" ||
           key == "alerts";
}

std::string EffectiveDockModeFromConfig() {
    if (IsRecognizedDockMode(g_config.dock_mode)) {
        return g_config.dock_mode;
    }
    return "studio";
}

bool SetDockSettingValueByKey(const std::string& key, bool value) {
    if (key == "auto_scene_switch") {
        g_config.auto_scene_switch = value;
        return true;
    }
    if (key == "low_quality_fallback") {
        g_config.low_quality_fallback = value;
        return true;
    }
    if (key == "manual_override") {
        g_config.manual_override = value;
        return true;
    }
    if (key == "chat_bot") {
        g_config.chat_bot = value;
        return true;
    }
    if (key == "alerts") {
        g_config.alerts = value;
        return true;
    }
    return false;
}

QJsonObject BuildDockSettingsJsonFromConfig() {
    QJsonObject settings;
    settings.insert(QStringLiteral("auto_scene_switch"), g_config.auto_scene_switch);
    settings.insert(QStringLiteral("low_quality_fallback"), g_config.low_quality_fallback);
    settings.insert(QStringLiteral("manual_override"), g_config.manual_override);
    settings.insert(QStringLiteral("chat_bot"), g_config.chat_bot);
    settings.insert(QStringLiteral("alerts"), g_config.alerts);
    return settings;
}

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
    if (active_count > 0 && (total_drop / active_count) > 0.05f) {
        health = "degraded";
    }
    if (snapshot.obs.render_total_frames > 0) {
        const float miss_pct = static_cast<float>(snapshot.obs.render_missed_frames) /
                               static_cast<float>(snapshot.obs.render_total_frames);
        if (miss_pct > 0.05f) {
            health = "degraded";
        }
    }
    return health;
}

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
        std::lock_guard<std::mutex> lock(g_dock_replay_cache_mu);
        g_dock_replay_cache.has_status_snapshot = true;
        g_dock_replay_cache.status_snapshot_json = json;
    }
    blog(
        delivered ? LOG_INFO : LOG_DEBUG,
        "[aegis-obs-plugin] status snapshot emitted: delivered=%s reason=%s mode=%s",
        delivered ? "true" : "false",
        reason ? reason : "unknown",
        mode.c_str());
    return delivered;
}

bool ShouldDeduplicateDockActionByRequestId(
    const std::string& action_type,
    const std::string& request_id) {
    if (request_id.empty() || action_type.empty()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const std::string dedupe_key = action_type + "|" + request_id;
    std::lock_guard<std::mutex> lock(g_recent_dock_actions_mu);

    for (auto it = g_recent_dock_actions.begin(); it != g_recent_dock_actions.end();) {
        if ((now - it->second) > kDockActionDuplicateWindowMs) {
            it = g_recent_dock_actions.erase(it);
        } else {
            ++it;
        }
    }

    auto found = g_recent_dock_actions.find(dedupe_key);
    if (found != g_recent_dock_actions.end()) {
        return true;
    }
    g_recent_dock_actions.emplace(dedupe_key, now);
    return false;
}

void TrackPendingDockRequestStatusAction(const std::string& request_id) {
    if (request_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_request_status_action_ids_mu);
    g_pending_request_status_action_ids.push_back(request_id);
}

std::string ConsumePendingDockRequestStatusActionId() {
    std::lock_guard<std::mutex> lock(g_pending_request_status_action_ids_mu);
    if (g_pending_request_status_action_ids.empty()) {
        return {};
    }
    std::string request_id = g_pending_request_status_action_ids.front();
    g_pending_request_status_action_ids.erase(g_pending_request_status_action_ids.begin());
    return request_id;
}

void TrackPendingDockSetModeAction(const std::string& request_id, const std::string& mode) {
    if (request_id.empty() || mode.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
    g_pending_set_mode_actions.push_back(
        PendingSetModeAction{request_id, mode, std::chrono::steady_clock::now()});
}

void TrackPendingDockSetSettingAction(
    const std::string& request_id,
    const std::string& key,
    bool value) {
    if (request_id.empty() || key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
    g_pending_set_setting_actions.push_back(
        PendingSetSettingAction{request_id, key, value, std::chrono::steady_clock::now()});
}

struct StatusSnapshotProjection {
    bool valid = false;
    bool has_mode = false;
    std::string mode;
    bool has_auto_scene_switch = false;
    bool auto_scene_switch = false;
    bool has_low_quality_fallback = false;
    bool low_quality_fallback = false;
    bool has_manual_override = false;
    bool manual_override = false;
    bool has_chat_bot = false;
    bool chat_bot = false;
    bool has_alerts = false;
    bool alerts = false;
};

bool TryProjectStatusSnapshot(const std::string& envelope_json, StatusSnapshotProjection* out) {
    if (!out) {
        return false;
    }
    *out = StatusSnapshotProjection{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(envelope_json));
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject envelope = doc.object();
    const QJsonValue type_val = envelope.value(QStringLiteral("type"));
    if (!type_val.isString() || type_val.toString() != QStringLiteral("status_snapshot")) {
        return false;
    }
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (payload.isEmpty()) {
        return false;
    }
    out->valid = true;
    const QJsonValue mode_val = payload.value(QStringLiteral("mode"));
    if (mode_val.isString()) {
        out->has_mode = true;
        out->mode = mode_val.toString().toStdString();
    }
    const QJsonObject settings = payload.value(QStringLiteral("settings")).toObject();
    auto read_setting_bool = [&settings](const char* key, bool* has, bool* value) {
        if (!key || !has || !value) {
            return;
        }
        const QJsonValue v = settings.value(QString::fromUtf8(key));
        if (!v.isBool()) {
            return;
        }
        *has = true;
        *value = v.toBool();
    };
    read_setting_bool("auto_scene_switch", &out->has_auto_scene_switch, &out->auto_scene_switch);
    read_setting_bool("low_quality_fallback", &out->has_low_quality_fallback, &out->low_quality_fallback);
    read_setting_bool("manual_override", &out->has_manual_override, &out->manual_override);
    read_setting_bool("chat_bot", &out->has_chat_bot, &out->chat_bot);
    read_setting_bool("alerts", &out->has_alerts, &out->alerts);
    return true;
}

bool TryGetStatusSnapshotSettingBool(
    const StatusSnapshotProjection& snap,
    const std::string& key,
    bool* out_value) {
    if (!out_value) {
        return false;
    }
    if (key == "auto_scene_switch" && snap.has_auto_scene_switch) {
        *out_value = snap.auto_scene_switch;
        return true;
    }
    if (key == "low_quality_fallback" && snap.has_low_quality_fallback) {
        *out_value = snap.low_quality_fallback;
        return true;
    }
    if (key == "manual_override" && snap.has_manual_override) {
        *out_value = snap.manual_override;
        return true;
    }
    if (key == "chat_bot" && snap.has_chat_bot) {
        *out_value = snap.chat_bot;
        return true;
    }
    if (key == "alerts" && snap.has_alerts) {
        *out_value = snap.alerts;
        return true;
    }
    return false;
}

void ResolvePendingDockActionCompletionsFromStatusSnapshot(const std::string& envelope_json) {
    StatusSnapshotProjection snap;
    if (!TryProjectStatusSnapshot(envelope_json, &snap) || !snap.valid) {
        return;
    }

    std::vector<std::string> completed_mode_ids;
    std::vector<std::string> completed_setting_ids;
    {
        std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
        auto it = g_pending_set_mode_actions.begin();
        while (it != g_pending_set_mode_actions.end()) {
            if (snap.has_mode && it->mode == snap.mode) {
                completed_mode_ids.push_back(it->request_id);
                it = g_pending_set_mode_actions.erase(it);
                continue;
            }
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
        auto it = g_pending_set_setting_actions.begin();
        while (it != g_pending_set_setting_actions.end()) {
            bool current_value = false;
            if (TryGetStatusSnapshotSettingBool(snap, it->key, &current_value) &&
                current_value == it->value) {
                completed_setting_ids.push_back(it->request_id);
                it = g_pending_set_setting_actions.erase(it);
                continue;
            }
            ++it;
        }
    }

    for (const auto& request_id : completed_mode_ids) {
        EmitDockActionResult(
            "set_mode",
            request_id,
            "completed",
            true,
            "",
            "status_snapshot_applied");
    }
    for (const auto& request_id : completed_setting_ids) {
        EmitDockActionResult(
            "set_setting",
            request_id,
            "completed",
            true,
            "",
            "status_snapshot_applied");
    }
}

void DrainExpiredPendingDockActions() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> timed_out_set_mode_ids;
    std::vector<std::string> timed_out_set_setting_ids;
    {
        std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
        auto it = g_pending_set_mode_actions.begin();
        while (it != g_pending_set_mode_actions.end()) {
            if (now - it->queued_at >= kDockActionCompletionTimeoutMs) {
                timed_out_set_mode_ids.push_back(it->request_id);
                it = g_pending_set_mode_actions.erase(it);
                continue;
            }
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
        auto it = g_pending_set_setting_actions.begin();
        while (it != g_pending_set_setting_actions.end()) {
            if (now - it->queued_at >= kDockActionCompletionTimeoutMs) {
                timed_out_set_setting_ids.push_back(it->request_id);
                it = g_pending_set_setting_actions.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (const auto& request_id : timed_out_set_mode_ids) {
        EmitDockActionResult(
            "set_mode",
            request_id,
            "failed",
            false,
            "completion_timeout",
            "status_snapshot_not_observed");
    }
    for (const auto& request_id : timed_out_set_setting_ids) {
        EmitDockActionResult(
            "set_setting",
            request_id,
            "failed",
            false,
            "completion_timeout",
            "status_snapshot_not_observed");
    }
}

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
        // No status snapshot cached yet â€” synthesize a minimal one with just the theme
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

void RegisterDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn) {
    SetDockBrowserJsExecuteSink(std::move(execute_fn));
    ReplayDockStateToJsSinkIfAvailable();
}

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
        std::lock_guard<std::mutex> lock(g_pending_relay_actions_mu);
        for (auto it = g_pending_relay_actions.begin(); it != g_pending_relay_actions.end(); ++it) {
            if (it->request_id == request_id) {
                if (action_type.empty()) {
                    action_type = it->action_type;
                }
                it = g_pending_relay_actions.erase(it);
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

#if defined(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
void InitializeBrowserDockHostBridge() {
    // Delegate to a dedicated scaffold module so future Qt/CEF embedding can evolve
    // without expanding this plugin entry file.
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
        // OBS shutdown is in progress; by module unload time the frontend callback
        // registry may already be gone. Avoid a noisy remove callback warning.
        g_frontend_exit_seen = true;
        // Tear down the browser dock host early while frontend/obs-browser are still in a
        // healthier state, rather than waiting until module unload during shutdown.
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

bool IsCurrentSceneName(const std::string& expected_scene_name) {
    if (expected_scene_name.empty()) {
        return false;
    }
    return expected_scene_name == CurrentSceneName();
}

bool IsDockUiActionReason(const std::string& reason) {
    return reason == "dock_ui";
}

void HandleSwitchSceneRequestOnObsThread(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason) {
    if (scene_name.empty()) {
        blog(
            LOG_WARNING,
            "[aegis-obs-plugin] switch_scene request missing scene_name (request_id=%s reason=%s)",
            request_id.c_str(),
            reason.c_str());
        if (!request_id.empty() && IsDockUiActionReason(reason)) {
            EmitDockActionResult(
                "switch_scene", request_id, "failed", false, "missing_scene_name", "scene_name missing");
        }
        EmitDockSceneSwitchCompleted(request_id, scene_name, false, "missing_scene_name", reason);
        return;
    }

    obs_source_t* scene_source = obs_get_source_by_name(scene_name.c_str());
    if (!scene_source) {
        blog(
            LOG_WARNING,
            "[aegis-obs-plugin] switch_scene target not found: request_id=%s scene=%s reason=%s",
            request_id.c_str(),
            scene_name.c_str(),
            reason.c_str());
        if (!request_id.empty() && IsDockUiActionReason(reason)) {
            EmitDockActionResult(
                "switch_scene", request_id, "failed", false, "scene_not_found", "");
        }
        EmitDockSceneSwitchCompleted(request_id, scene_name, false, "scene_not_found", reason);
        return;
    }

    blog(
        LOG_INFO,
        "[aegis-obs-plugin] switch_scene applying: request_id=%s scene=%s reason=%s",
        request_id.c_str(),
        scene_name.c_str(),
        reason.c_str());

    obs_frontend_set_current_scene(scene_source);
    obs_source_release(scene_source);

    if (!request_id.empty()) {
        if (IsCurrentSceneName(scene_name)) {
            CacheDockCurrentSceneForReplay(scene_name);
            EmitDockNativeCurrentScene(scene_name);
            if (IsDockUiActionReason(reason)) {
                EmitDockActionResult(
                    "switch_scene", request_id, "completed", true, "", "scene_switch_applied");
            }
            EmitDockSceneSwitchCompleted(request_id, scene_name, true, "", reason);
        } else {
            blog(
                LOG_WARNING,
                "[aegis-obs-plugin] switch_scene verify failed: request_id=%s scene=%s reason=%s",
                request_id.c_str(),
                scene_name.c_str(),
                reason.c_str());
            if (IsDockUiActionReason(reason)) {
                EmitDockActionResult(
                    "switch_scene", request_id, "failed", false, "switch_verify_failed", "");
            }
            EmitDockSceneSwitchCompleted(request_id, scene_name, false, "switch_verify_failed", reason);
        }
    }
}

void EnqueueSwitchSceneRequest(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason) {
    std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
    g_pending_switch_requests.push_back(PendingSwitchRequest{request_id, scene_name, reason});
}

void DrainSwitchSceneRequestsOnObsThread() {
    std::vector<PendingSwitchRequest> pending;
    {
        std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
        if (g_pending_switch_requests.empty()) {
            return;
        }
        pending.swap(g_pending_switch_requests);
    }

    for (const auto& req : pending) {
        HandleSwitchSceneRequestOnObsThread(req.request_id, req.scene_name, req.reason);
    }
}

void SwitchScenePumpTick(void*, float seconds) {
    if (seconds > 0.0f) {
        g_switch_pump_accum_seconds += seconds;
        g_theme_poll_accum_seconds += seconds;
        g_metrics_poll_accum_seconds += seconds;
    }
    DrainExpiredPendingDockActions();
    if (g_theme_poll_accum_seconds >= 0.5f) {
        g_theme_poll_accum_seconds = 0.0f;
        PollObsThemeChangesOnObsThread();
    }
    if (g_metrics_poll_accum_seconds >= 0.5f) {
        g_metrics_poll_accum_seconds = 0.0f;
        EmitCurrentStatusSnapshotToDock("metrics_poll", true);
    }
    if (g_switch_pump_accum_seconds < 0.05f) {
        return;
    }
    g_switch_pump_accum_seconds = 0.0f;
    DrainSwitchSceneRequestsOnObsThread();
}

} // namespace

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
        std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
        g_dock_js_delivery_validation.page_ready = true;
        g_dock_js_delivery_validation.logged_receive_status_snapshot_json = false;
        g_dock_js_delivery_validation.logged_receive_scene_snapshot_json = false;
        g_dock_js_delivery_validation.logged_receive_scene_switch_completed_json = false;
        g_dock_js_delivery_validation.logged_receive_dock_action_result_json = false;
        g_dock_js_delivery_validation.fallback_pipe_status_count = 0;
        g_dock_js_delivery_validation.fallback_scene_snapshot_count = 0;
        g_dock_js_delivery_validation.fallback_scene_switch_completed_count = 0;
        g_dock_js_delivery_validation.fallback_dock_action_result_count = 0;
    }
    ReplayDockStateToJsSinkIfAvailable();
    MaybeRunDockActionSelfTestAfterPageReady();

    // Safety net: fire a deferred fresh scene enumeration 1s after page-ready.
    // The initial replay's executeJavaScript is fire-and-forget into CEF's IPC
    // pipeline â€” if the render process hasn't fully committed the V8 context,
    // the scene snapshot JS call can be silently dropped.  This deferred
    // re-enumeration ensures the dock always gets scene data.
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
        std::lock_guard<std::mutex> lock(g_dock_js_delivery_validation_mu);
        g_dock_js_delivery_validation.page_ready = false;
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

    auto ensure_request_id = [&request_id]() {
        if (!request_id.empty()) {
            return;
        }
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        {
            std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
            g_local_dock_action_seq += 1;
            request_id = "dock_" + std::to_string(now_ms) + "_" + std::to_string(g_local_dock_action_seq);
        }
    };
    ensure_request_id();
    if (ShouldDeduplicateDockActionByRequestId(action_type, request_id)) {
        blog(
            LOG_DEBUG,
            "[aegis-obs-plugin] dock action deduplicated: type=%s request_id=%s",
            action_type.c_str(),
            request_id.c_str());
        return true;
    }

    if (action_type == "switch_scene") {
        std::string scene_name;
        (void)TryExtractJsonStringField(action_json, "sceneName", &scene_name);
        if (scene_name.empty()) {
            (void)TryExtractJsonStringField(action_json, "scene_name", &scene_name);
        }
        if (scene_name.empty()) {
            blog(
                LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=switch_scene request_id=%s error=missing_scene_name",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_scene_name", "");
            return false;
        }

        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action queued: type=switch_scene request_id=%s scene=%s",
            request_id.c_str(),
            scene_name.c_str());
        EnqueueSwitchSceneRequest(request_id, scene_name, "dock_ui");
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_for_obs_thread");
        return true;
    }

    if (action_type == "request_status") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action request_status: request_id=%s",
            request_id.c_str());
        const bool delivered = EmitCurrentStatusSnapshotToDock("dock_request_status", true);
        EmitDockActionResult(
            action_type,
            request_id,
            "completed",
            true,
            "",
            delivered ? "status_snapshot_emitted" : "status_snapshot_cached");
        return true;
    }

    if (action_type == "set_mode") {
        std::string mode;
        (void)TryExtractJsonStringField(action_json, "mode", &mode);
        if (!IsRecognizedDockMode(mode)) {
            blog(
                LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=set_mode request_id=%s mode=%s error=invalid_mode",
                request_id.c_str(),
                mode.empty() ? "" : mode.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "invalid_mode", "");
            return false;
        }
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action set_mode: request_id=%s mode=%s",
            request_id.c_str(),
            mode.c_str());
        g_config.dock_mode = mode;
        const bool saved = g_config.SaveToDisk();
        if (!saved) {
            EmitDockActionResult(action_type, request_id, "failed", false, "save_failed", "config_write_failed");
            return false;
        }
        const bool delivered = EmitCurrentStatusSnapshotToDock("dock_set_mode", false);
        EmitDockActionResult(
            action_type,
            request_id,
            "completed",
            true,
            "",
            delivered ? "mode_applied_snapshot_emitted" : "mode_applied_snapshot_cached");
        return true;
    }

    if (action_type == "set_setting") {
        std::string key;
        bool value = false;
        const bool has_value = TryExtractJsonBoolField(action_json, "value", &value);
        (void)TryExtractJsonStringField(action_json, "key", &key);
        if (key.empty()) {
            blog(
                LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=set_setting request_id=%s error=missing_setting_key",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_setting_key", "");
            return false;
        }
        if (!has_value) {
            blog(
                LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=set_setting request_id=%s key=%s error=missing_setting_value",
                request_id.c_str(),
                key.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_setting_value", "");
            return false;
        }
        if (!IsRecognizedDockSettingKey(key)) {
            blog(
                LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=set_setting request_id=%s key=%s error=unsupported_setting_key",
                request_id.c_str(),
                key.c_str());
            EmitDockActionResult(
                action_type, request_id, "rejected", false, "unsupported_setting_key", key);
            return false;
        }
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action set_setting: request_id=%s key=%s value=%s",
            request_id.c_str(),
            key.c_str(),
            value ? "true" : "false");
        if (!SetDockSettingValueByKey(key, value)) {
            EmitDockActionResult(action_type, request_id, "rejected", false, "unsupported_setting_key", key);
            return false;
        }
        const bool saved = g_config.SaveToDisk();
        if (!saved) {
            EmitDockActionResult(action_type, request_id, "failed", false, "save_failed", "config_write_failed");
            return false;
        }
        const bool delivered = EmitCurrentStatusSnapshotToDock("dock_set_setting", false);
        EmitDockActionResult(
            action_type,
            request_id,
            "completed",
            true,
            "",
            delivered ? "setting_applied_snapshot_emitted" : "setting_applied_snapshot_cached");
        return true;
    }

    // â”€â”€ load_scene_prefs: read dock_scene_prefs.json from plugin config dir â”€â”€
    if (action_type == "load_scene_prefs") {
        char* config_dir = obs_module_config_path("");
        if (!config_dir) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=load_scene_prefs request_id=%s error=no_config_path",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_config_path", "");
            return false;
        }
        std::string prefs_path = std::string(config_dir) + "/dock_scene_prefs.json";
        bfree(config_dir);

        std::ifstream in(prefs_path, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            // v0.0.4 migration: try old aegis-obs-shim config path
            std::string old_prefs_path;
            {
                // prefs_path is .../plugin_config/aegis-obs-plugin/dock_scene_prefs.json
                // We need .../plugin_config/aegis-obs-shim/dock_scene_prefs.json
                auto pos = prefs_path.rfind("aegis-obs-plugin");
                if (pos != std::string::npos) {
                    old_prefs_path = prefs_path.substr(0, pos) + "aegis-obs-shim/dock_scene_prefs.json";
                }
            }
            if (!old_prefs_path.empty()) {
                std::ifstream old_in(old_prefs_path, std::ios::in | std::ios::binary);
                if (old_in.is_open()) {
                    std::ostringstream old_contents;
                    old_contents << old_in.rdbuf();
                    old_in.close();
                    const std::string old_data = old_contents.str();
                    blog(LOG_INFO,
                        "[aegis-obs-plugin] dock action completed: type=load_scene_prefs request_id=%s "
                        "migrated_from=aegis-obs-shim bytes=%d",
                        request_id.c_str(),
                        static_cast<int>(old_data.size()));
                    // Save to new path for future loads
                    auto dir_end = prefs_path.rfind('/');
                    if (dir_end != std::string::npos) {
                        QDir().mkpath(QString::fromStdString(prefs_path.substr(0, dir_end)));
                    }
                    std::ofstream migrate_out(prefs_path, std::ios::out | std::ios::trunc | std::ios::binary);
                    if (migrate_out.is_open()) {
                        migrate_out.write(old_data.data(), static_cast<std::streamsize>(old_data.size()));
                        migrate_out.close();
                    }
                    EmitDockActionResult(action_type, request_id, "completed", true, "", old_data);
                    return true;
                }
            }

            // No prefs file at either path â€” return empty object so the dock hydrates cleanly
            blog(LOG_INFO,
                "[aegis-obs-plugin] dock action completed: type=load_scene_prefs request_id=%s detail=no_prefs_file",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "completed", true, "", "{}");
            return true;
        }
        std::ostringstream contents;
        contents << in.rdbuf();
        in.close();
        const std::string data = contents.str();
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=load_scene_prefs request_id=%s bytes=%d",
            request_id.c_str(),
            static_cast<int>(data.size()));
        EmitDockActionResult(action_type, request_id, "completed", true, "", data);
        return true;
    }

    // â”€â”€ save_scene_prefs: write prefsJson to dock_scene_prefs.json â”€â”€
    if (action_type == "save_scene_prefs") {
        std::string prefs_json;
        (void)TryExtractJsonStringField(action_json, "prefsJson", &prefs_json);
        if (prefs_json.empty()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=save_scene_prefs request_id=%s error=missing_prefs_json",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_prefs_json", "");
            return false;
        }

        char* config_dir = obs_module_config_path("");
        if (!config_dir) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=save_scene_prefs request_id=%s error=no_config_path",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_config_path", "");
            return false;
        }
        const std::string config_dir_str(config_dir);
        bfree(config_dir);

        // Ensure the config directory exists
        QDir().mkpath(QString::fromStdString(config_dir_str));

        const std::string prefs_path = config_dir_str + "/dock_scene_prefs.json";
        std::ofstream out(prefs_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=save_scene_prefs request_id=%s error=file_write_failed path=%s",
                request_id.c_str(),
                prefs_path.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "file_write_failed", "");
            return false;
        }
        out.write(prefs_json.data(), static_cast<std::streamsize>(prefs_json.size()));
        out.close();
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=save_scene_prefs request_id=%s bytes=%d",
            request_id.c_str(),
            static_cast<int>(prefs_json.size()));
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        return true;
    }

    // â”€â”€ request_scene_snapshot: force a fresh OBS scene enumeration â”€â”€
    if (action_type == "request_scene_snapshot") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=request_scene_snapshot request_id=%s",
            request_id.c_str());
        LogSceneSnapshot("dock_request");
        EmitDockActionResult(action_type, request_id, "completed", true, "", "scene_snapshot_emitted");
        return true;
    }

    if (action_type == "relay_start") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action relay_start: request_id=%s",
            request_id.c_str());
        if (!g_relay) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_start request_id=%s error=relay_not_configured",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "relay_not_configured", "");
            return false;
        }
        auto jwt_opt = g_vault.Get("relay_jwt");
        if (!jwt_opt) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_start request_id=%s error=no_relay_jwt",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_relay_jwt", "");
            return false;
        }
        // Emit immediate "queued" so the dock knows the action was received.
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        // Capture by value â€” never capture g_relay or g_config by reference in a detached thread.
        std::string jwt = *jwt_opt;
        std::string req_id = request_id;
        int heartbeat_interval = g_config.relay_heartbeat_interval_sec;
        std::thread([jwt, req_id, heartbeat_interval]() {
            try {
                auto session = g_relay->Start(jwt);
                if (session) {
                    if (session->status == "provisioning") {
                        EmitDockActionResult("relay_start", req_id, "provisioning", true, "",
                                             BuildRelaySessionDetailJson(*session));

                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(180);
                        while (std::chrono::steady_clock::now() < deadline) {
                            auto polled = g_relay->GetActive(jwt);
                            if (!polled) {
                                const auto now = std::chrono::steady_clock::now();
                                if (now >= deadline) {
                                    break;
                                }
                                const auto remaining = deadline - now;
                                const auto sleep_for = std::min(
                                    std::chrono::seconds(2),
                                    std::chrono::duration_cast<std::chrono::seconds>(remaining));
                                std::this_thread::sleep_for(sleep_for);
                                continue;
                            }

                            if (!polled->provision_step.empty()) {
                                ProvisionStepInfo info = stepToInfo(polled->provision_step);
                                std::ostringstream progress;
                                progress << "{\"step\":\"" << JsonEscape(polled->provision_step) << "\""
                                         << ",\"stepNumber\":" << info.number
                                         << ",\"totalSteps\":6"
                                         << ",\"label\":\"" << JsonEscape(info.label) << "\""
                                         << "}";
                                EmitDockActionResult("relay_provision_progress", req_id, "progress", true, "",
                                                     progress.str());
                            }

                            if (polled->status == "active") {
                                g_relay->StartHeartbeatLoop(jwt, polled->session_id, heartbeat_interval);
                                blog(LOG_INFO,
                                    "[aegis-obs-plugin] relay_start completed: request_id=%s session_id=[redacted] region=%s status=%s",
                                    req_id.c_str(),
                                    polled->region.c_str(),
                                    polled->status.c_str());
                                EmitDockActionResult("relay_start", req_id, "completed", true, "",
                                                     BuildRelaySessionDetailJson(*polled));
                                return;
                            }

                            if (polled->status == "failed" || polled->status == "stopped") {
                                blog(LOG_WARNING,
                                    "[aegis-obs-plugin] relay_start failed: request_id=%s error=relay_start_failed",
                                    req_id.c_str());
                                EmitDockActionResult("relay_start", req_id, "failed", false,
                                                     "relay_start_failed", "");
                                return;
                            }

                            const auto now = std::chrono::steady_clock::now();
                            if (now >= deadline) {
                                break;
                            }
                            const auto remaining = deadline - now;
                            const auto sleep_for = std::min(
                                std::chrono::seconds(2),
                                std::chrono::duration_cast<std::chrono::seconds>(remaining));
                            std::this_thread::sleep_for(sleep_for);
                        }

                        blog(LOG_WARNING,
                            "[aegis-obs-plugin] relay_start failed: request_id=%s error=provision_timeout",
                            req_id.c_str());
                        EmitDockActionResult("relay_start", req_id, "failed", false, "provision_timeout", "");
                        return;
                    }

                    if (session->status == "active") {
                        g_relay->StartHeartbeatLoop(jwt, session->session_id, heartbeat_interval);
                        blog(LOG_INFO,
                            "[aegis-obs-plugin] relay_start completed: request_id=%s session_id=[redacted] region=%s status=%s",
                            req_id.c_str(),
                            session->region.c_str(),
                            session->status.c_str());
                        EmitDockActionResult("relay_start", req_id, "completed", true, "",
                                             BuildRelaySessionDetailJson(*session));
                    } else {
                        blog(LOG_WARNING,
                            "[aegis-obs-plugin] relay_start failed: request_id=%s error=relay_start_failed",
                            req_id.c_str());
                        EmitDockActionResult("relay_start", req_id, "failed", false, "relay_start_failed", "");
                    }
                } else {
                    blog(LOG_WARNING,
                        "[aegis-obs-plugin] relay_start failed: request_id=%s error=relay_start_failed",
                        req_id.c_str());
                    EmitDockActionResult("relay_start", req_id, "failed", false, "relay_start_failed", "");
                }
            } catch (const std::exception& e) {
                blog(LOG_WARNING,
                    "[aegis-obs-plugin] relay_start exception: request_id=%s error=%s",
                    req_id.c_str(), e.what());
                EmitDockActionResult("relay_start", req_id, "failed", false, JsonEscape(e.what()), "");
            }
        }).detach();
        return true;
    }

    if (action_type == "relay_stop") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action relay_stop: request_id=%s",
            request_id.c_str());
        if (!g_relay || !g_relay->HasActiveSession()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_stop request_id=%s error=no_active_session",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_active_session", "");
            return false;
        }
        auto jwt_opt = g_vault.Get("relay_jwt");
        auto session = g_relay->CurrentSession();
        std::string jwt = jwt_opt.value_or("");
        std::string sid = session ? session->session_id : "";
        // Emit immediate "queued" so the dock knows the action was received.
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        std::string req_id = request_id;
        std::thread([jwt, sid, req_id]() {
            try {
                g_relay->StopHeartbeatLoop();
                bool ok = g_relay->Stop(jwt, sid);
                if (ok) {
                    blog(LOG_INFO,
                        "[aegis-obs-plugin] relay_stop completed: request_id=%s",
                        req_id.c_str());
                    EmitDockActionResult("relay_stop", req_id, "completed", true, "", "");
                } else {
                    blog(LOG_WARNING,
                        "[aegis-obs-plugin] relay_stop failed: request_id=%s error=relay_stop_failed",
                        req_id.c_str());
                    EmitDockActionResult("relay_stop", req_id, "failed", false, "relay_stop_failed", "");
                }
            } catch (const std::exception& e) {
                blog(LOG_WARNING,
                    "[aegis-obs-plugin] relay_stop exception: request_id=%s error=%s",
                    req_id.c_str(), e.what());
                EmitDockActionResult("relay_stop", req_id, "failed", false, JsonEscape(e.what()), "");
            }
        }).detach();
        return true;
    }

    // â”€â”€ load_config: return current plugin config fields to dock â”€â”€
    if (action_type == "load_config") {
        const std::optional<std::string> relay_shared_key = g_vault.Get("relay_shared_key");
        std::ostringstream detail;
        detail << "{";
        detail << "\"relay_api_host\":" << JsStringLiteral(g_config.relay_api_host) << ",";
        detail << "\"relay_shared_key\":" << JsStringLiteral(relay_shared_key.value_or("")) << ",";
        detail << "\"relay_heartbeat_interval_sec\":" << g_config.relay_heartbeat_interval_sec << ",";
        detail << "\"metrics_poll_interval_ms\":" << g_config.metrics_poll_interval_ms << ",";
        detail << "\"grafana_enabled\":" << (g_config.grafana_enabled ? "true" : "false") << ",";
        detail << "\"grafana_otlp_endpoint\":" << JsStringLiteral(g_config.grafana_otlp_endpoint) << ",";
        detail << "\"dock_mode\":" << JsStringLiteral(EffectiveDockModeFromConfig()) << ",";
        detail << "\"auto_scene_switch\":" << (g_config.auto_scene_switch ? "true" : "false") << ",";
        detail << "\"low_quality_fallback\":" << (g_config.low_quality_fallback ? "true" : "false") << ",";
        detail << "\"manual_override\":" << (g_config.manual_override ? "true" : "false") << ",";
        detail << "\"chat_bot\":" << (g_config.chat_bot ? "true" : "false") << ",";
        detail << "\"alerts\":" << (g_config.alerts ? "true" : "false");
        detail << "}";
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=load_config request_id=%s",
            request_id.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "", detail.str());
        return true;
    }

    // â”€â”€ save_config: receive config fields from dock and persist â”€â”€
    if (action_type == "save_config") {
        std::string relay_api_host;
        std::string relay_shared_key;
        std::string relay_heartbeat_interval_sec_str;
        std::string metrics_poll_interval_ms_str;
        std::string grafana_enabled_str;
        std::string grafana_otlp_endpoint;

        (void)TryExtractJsonStringField(action_json, "relay_api_host", &relay_api_host);
        const bool has_relay_shared_key =
            TryExtractJsonStringField(action_json, "relay_shared_key", &relay_shared_key);
        (void)TryExtractJsonStringField(action_json, "relay_heartbeat_interval_sec",
                                         &relay_heartbeat_interval_sec_str);
        (void)TryExtractJsonStringField(action_json, "metrics_poll_interval_ms",
                                         &metrics_poll_interval_ms_str);
        bool grafana_enabled_val = false;
        const bool has_grafana_enabled =
            TryExtractJsonBoolField(action_json, "grafana_enabled", &grafana_enabled_val);
        (void)TryExtractJsonStringField(action_json, "grafana_otlp_endpoint",
                                         &grafana_otlp_endpoint);

        if (!relay_api_host.empty()) {
            g_config.relay_api_host = relay_api_host;
        }
        if (has_relay_shared_key) {
            bool vault_saved = true;
            if (relay_shared_key.empty()) {
                // Clearing is idempotent: Remove() returns false when absent.
                (void)g_vault.Remove("relay_shared_key");
            } else {
                vault_saved = g_vault.Set("relay_shared_key", relay_shared_key);
            }
            if (!vault_saved) {
                blog(LOG_WARNING,
                    "[aegis-obs-plugin] dock action failed: type=save_config request_id=%s error=relay_shared_key_vault_failed",
                    request_id.c_str());
                EmitDockActionResult(action_type, request_id, "failed", false, "relay_shared_key_vault_failed", "");
                return false;
            }
        }
        if (!relay_heartbeat_interval_sec_str.empty()) {
            try {
                const int v = std::stoi(relay_heartbeat_interval_sec_str);
                if (v > 0) {
                    g_config.relay_heartbeat_interval_sec = v;
                }
            } catch (...) {}
        }
        if (!metrics_poll_interval_ms_str.empty()) {
            try {
                const int v = std::stoi(metrics_poll_interval_ms_str);
                if (v > 0) {
                    g_config.metrics_poll_interval_ms = v;
                }
            } catch (...) {}
        }
        if (has_grafana_enabled) {
            g_config.grafana_enabled = grafana_enabled_val;
        }
        if (!grafana_otlp_endpoint.empty()) {
            g_config.grafana_otlp_endpoint = grafana_otlp_endpoint;
        }

        const bool saved = g_config.SaveToDisk();
        if (!saved) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=save_config request_id=%s error=save_failed",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "save_failed", "");
            return false;
        }
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=save_config request_id=%s",
            request_id.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        return true;
    }

    // â”€â”€ vault_set: encrypt and store a secret â”€â”€
    if (action_type == "vault_set") {
        std::string key;
        std::string value;
        (void)TryExtractJsonStringField(action_json, "key", &key);
        (void)TryExtractJsonStringField(action_json, "value", &value);
        if (key.empty()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=vault_set request_id=%s error=missing_key",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_key", "");
            return false;
        }
        // NOTE: secret value is never logged.
        const bool ok = g_vault.Set(key, value);
        if (!ok) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=vault_set request_id=%s key=%s error=vault_set_failed",
                request_id.c_str(), key.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "vault_set_failed", "");
            return false;
        }
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=vault_set request_id=%s key=%s",
            request_id.c_str(), key.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        return true;
    }

    // â”€â”€ vault_get: retrieve a decrypted secret â”€â”€
    if (action_type == "vault_get") {
        std::string key;
        (void)TryExtractJsonStringField(action_json, "key", &key);
        if (key.empty()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=vault_get request_id=%s error=missing_key",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_key", "");
            return false;
        }
        const std::optional<std::string> secret = g_vault.Get(key);
        // NOTE: secret value is never logged.
        std::ostringstream detail;
        if (secret.has_value()) {
            detail << "{\"key\":" << JsStringLiteral(key)
                   << ",\"value\":" << JsStringLiteral(secret.value()) << "}";
        } else {
            detail << "{\"key\":" << JsStringLiteral(key) << ",\"value\":null}";
        }
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=vault_get request_id=%s key=%s found=%s",
            request_id.c_str(), key.c_str(), secret.has_value() ? "true" : "false");
        EmitDockActionResult(action_type, request_id, "completed", true, "", detail.str());
        return true;
    }

    // â”€â”€ vault_keys: return sorted list of stored vault key names â”€â”€
    if (action_type == "vault_keys") {
        const std::vector<std::string> keys = g_vault.Keys();
        std::ostringstream detail;
        detail << "{\"keys\":[";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) {
                detail << ",";
            }
            detail << JsStringLiteral(keys[i]);
        }
        detail << "]}";
        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=vault_keys request_id=%s count=%d",
            request_id.c_str(), static_cast<int>(keys.size()));
        EmitDockActionResult(action_type, request_id, "completed", true, "", detail.str());
        return true;
    }

    blog(
        LOG_INFO,
        "[aegis-obs-plugin] dock action rejected: type=%s request_id=%s error=unsupported_action_type",
        action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str());
    EmitDockActionResult(action_type, request_id, "rejected", false, "unsupported_action_type", "");
    return false;
#else
    (void)action_json_utf8;
    return false;
#endif
}

bool obs_module_load(void) {
    blog(LOG_INFO, "[aegis-obs-plugin] module load");

    // Load persisted vault and config from %APPDATA%/Telemy/ on startup.
    g_vault.Load();
    g_config.LoadFromDisk();
    const std::optional<std::string> relay_shared_key = g_vault.Get("relay_shared_key");

    // Initialize relay client if a relay API host is configured.
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

    // No IPC layer in v0.0.4 â€” dock bridge assets are loaded directly by the
    // CEF host. Pipe status is always "ok" since there is no pipe.
    SetDockSceneSnapshotEmitter({});
    InitializeBrowserDockHostBridge();
    g_frontend_exit_seen = false;

    // Emit pipe status "ok" once on startup so the dock bridge hydrates.
    // There is no named-pipe IPC in v0.0.4; this call satisfies the bridge
    // contract that expects at least one receivePipeStatus("ok", ...) call.
    CacheDockPipeStatusForReplay("ok", "native plugin v0.0.4");
    // (The actual emission to JS will happen on page-ready via ReplayDockStateToJsSinkIfAvailable)

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
    {
        std::lock_guard<std::mutex> lock(g_pending_switch_requests_mu);
        g_pending_switch_requests.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_request_status_action_ids_mu);
        g_pending_request_status_action_ids.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_mode_actions_mu);
        g_pending_set_mode_actions.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_set_setting_actions_mu);
        g_pending_set_setting_actions.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_pending_relay_actions_mu);
        g_pending_relay_actions.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_recent_dock_actions_mu);
        g_recent_dock_actions.clear();
    }
    g_dock_action_selftest_attempted = false;
    SetDockSceneSnapshotEmitter({});
    ShutdownBrowserDockHostBridge();
    ClearDockReplayCache();

    // Emergency relay teardown â€” blocks up to 3 seconds to send a graceful
    // stop to the relay API before the WinHTTP session is destroyed.
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
    // Destroy g_relay before g_http â€” RelayClient holds a reference to g_http.
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

