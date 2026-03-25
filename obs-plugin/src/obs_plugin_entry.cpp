#include "dock_js_bridge_api.h"
#include "chatbot_runtime.h"
#include "config_vault.h"
#include "connection_manager.h"
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
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
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

// Mask a relay host for display: "my-relay.example.com" → "my-relay.e***.com"
static std::string MaskRelayHost(const std::string& host) {
    if (host.empty()) return host;
    const auto dot1 = host.find('.');
    if (dot1 == std::string::npos) {
        return host.substr(0, 1) + "***";
    }
    const auto dot2 = host.rfind('.');
    if (dot2 == dot1) {
        // One dot: "example.com" → "e***.com"
        return host.substr(0, 1) + "***" + host.substr(dot2);
    }
    // Two+ dots: "my-relay.example.com" → "my-relay.e***.com"
    const std::string prefix        = host.substr(0, dot1 + 1);
    const std::string domain        = host.substr(dot1 + 1, dot2 - dot1 - 1);
    const std::string tld           = host.substr(dot2);
    const std::string masked_domain = domain.empty() ? "***" : (domain.substr(0, 1) + "***");
    return prefix + masked_domain + tld;
}

// ---------------------------------------------------------------------------
// Multi-stream plugin output name resolution
// ---------------------------------------------------------------------------
// Reads StreamElements multi-streaming destinations.json to map UUID-based
// output names back to user-friendly labels (e.g. "YT_Horiz", "YT_Vert").
// Cached and refreshed every 60 seconds.

static std::map<std::string, std::string> g_output_display_names;
// Maps output ID (or prefix) → canvas display name (e.g. "Vertical")
static std::map<std::string, std::string> g_output_canvas_names;
static uint64_t g_output_names_last_load_ms = 0;
static constexpr uint64_t kOutputNamesRefreshMs = 60000; // 60s

static void LoadStreamElementsOutputNames() {
    const QString appdata = QDir::homePath() + "/AppData/Roaming";
    const QString se_base = appdata +
        "/obs-studio/plugin_config/obs-streamelements-core"
        "/scoped_config_storage/streamelements_multi-streaming";
    QDir dir(se_base);
    if (!dir.exists()) return;

    // ── Load canvas/composition names (shared across accounts) ──────
    // compositions/ sits at the multi-streaming level, NOT inside account subdirs.
    // compositions/{uuid}.json → { "name": "Vertical", ... }
    std::map<std::string, std::string> canvas_names; // compositionId → name
    {
        QDir comp_dir(se_base + "/compositions");
        if (comp_dir.exists()) {
            for (const auto& fname : comp_dir.entryList({"*.json"}, QDir::Files)) {
                QFile cf(comp_dir.filePath(fname));
                if (!cf.open(QIODevice::ReadOnly)) continue;
                const QJsonDocument cdoc = QJsonDocument::fromJson(cf.readAll());
                cf.close();
                if (!cdoc.isObject()) continue;
                const QString cname = cdoc.object().value("name").toString();
                const QString cid = fname.left(fname.size() - 5);
                if (!cname.isEmpty() && !cid.isEmpty()) {
                    canvas_names[cid.toStdString()] = cname.toStdString();
                }
            }
        }
    }

    for (const auto& sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString acct_base = se_base + "/" + sub;

        // ── Load destinations ─────────────────────────────────────────
        const QString dest_path = acct_base + "/destinations.json";
        QFile f(dest_path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = f.readAll();
        f.close();

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (!doc.isArray()) continue;

        for (const auto& val : doc.array()) {
            if (!val.isObject()) continue;
            const QJsonObject dest = val.toObject();
            const QString display_name = dest.value("displayName").toString();
            const QString dest_id = dest.value("id").toString();
            if (display_name.isEmpty() || dest_id.isEmpty()) continue;

            // Map destination ID → display name
            g_output_display_names[dest_id.toStdString()] = display_name.toStdString();

            // Each output has an ID and a videoCompositionId (canvas reference)
            const QJsonArray outputs = dest.value("outputs").toArray();
            for (const auto& out_val : outputs) {
                if (!out_val.isObject()) continue;
                const QJsonObject out_obj = out_val.toObject();
                const QString out_id = out_obj.value("id").toString();
                if (!out_id.isEmpty()) {
                    g_output_display_names[out_id.toStdString()] = display_name.toStdString();

                    // Resolve canvas name from videoCompositionId
                    const QString comp_id = out_obj.value("videoCompositionId").toString();
                    if (!comp_id.isEmpty() && comp_id != "default") {
                        auto cit = canvas_names.find(comp_id.toStdString());
                        if (cit != canvas_names.end()) {
                            g_output_canvas_names[out_id.toStdString()] = cit->second;
                            g_output_canvas_names[dest_id.toStdString()] = cit->second;
                        }
                    }
                    // "default" canvas = OBS native — no canvas_name entry needed
                }
            }
        }

    }
}

// Resolve an OBS output name to a user-friendly display name.
static std::string ResolveOutputDisplayName(const std::string& obs_name) {
    auto it = g_output_display_names.find(obs_name);
    if (it != g_output_display_names.end()) return it->second;
    for (const auto& kv : g_output_display_names) {
        if (obs_name.size() > kv.first.size() &&
            obs_name.compare(0, kv.first.size(), kv.first) == 0) {
            return kv.second;
        }
    }
    return obs_name;
}

// Resolve an OBS output name to its SE canvas name (e.g. "Vertical").
// Returns empty string if no canvas mapping exists.
static std::string ResolveOutputCanvasName(const std::string& obs_name) {
    auto it = g_output_canvas_names.find(obs_name);
    if (it != g_output_canvas_names.end()) return it->second;
    for (const auto& kv : g_output_canvas_names) {
        if (obs_name.size() > kv.first.size() &&
            obs_name.compare(0, kv.first.size(), kv.first) == 0) {
            return kv.second;
        }
    }
    return "";
}

static void EnsureOutputNamesLoaded() {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (now - g_output_names_last_load_ms < kOutputNamesRefreshMs &&
        !g_output_display_names.empty()) {
        return;
    }
    g_output_display_names.clear();
    g_output_canvas_names.clear();
    LoadStreamElementsOutputNames();
    g_output_names_last_load_ms = now;
}

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
aegis::Vault                                  g_vault;
aegis::PluginConfig                           g_config;
aegis::HttpsClient                            g_http;
aegis::ConnectionManager                      g_connection_manager;
aegis::ChatbotRuntime                         g_chatbot_runtime;
std::unique_ptr<aegis::ControlPlaneAuthClient> g_auth;
std::mutex                                    g_auth_state_mu;
aegis::PluginAuthState                        g_auth_state;

namespace {

constexpr const char* kPluginAuthVaultKey = "plugin_auth_state";
constexpr const char* kLegacyRelayJwtVaultKey = "relay_jwt";

bool HasExpiredIsoTimestamp(const std::string& expires_at)
{
    if (expires_at.empty()) {
        return true;
    }
    const QDateTime parsed = QDateTime::fromString(QString::fromStdString(expires_at), Qt::ISODate);
    if (!parsed.isValid()) {
        return true;
    }
    return parsed <= QDateTime::currentDateTimeUtc();
}

std::string CurrentControlPlaneJwt() {
    {
        std::lock_guard<std::mutex> lock(g_auth_state_mu);
        if (!g_auth_state.tokens.cp_access_jwt.empty()) {
            return g_auth_state.tokens.cp_access_jwt;
        }
    }

    const auto legacy = g_vault.Get(kLegacyRelayJwtVaultKey);
    return legacy.value_or("");
}

bool SavePluginAuthStateToVaultLocked() {
    const bool ok = g_vault.Set(kPluginAuthVaultKey, g_auth_state.ToVaultJson());
    if (!ok) {
        blog(LOG_WARNING, "[aegis-obs-plugin] auth state persist failed");
    }
    if (!g_auth_state.tokens.cp_access_jwt.empty()) {
        (void)g_vault.Set(kLegacyRelayJwtVaultKey, g_auth_state.tokens.cp_access_jwt);
    }
    return ok;
}

void PersistPluginAuthState() {
    std::lock_guard<std::mutex> lock(g_auth_state_mu);
    SavePluginAuthStateToVaultLocked();
}

void ClearPluginAuthState(bool clear_legacy_jwt) {
    std::lock_guard<std::mutex> lock(g_auth_state_mu);
    g_auth_state = aegis::PluginAuthState{};
    (void)g_vault.Remove(kPluginAuthVaultKey);
    if (clear_legacy_jwt) {
        (void)g_vault.Remove(kLegacyRelayJwtVaultKey);
    }
}

void LoadPluginAuthStateFromVault() {
    std::lock_guard<std::mutex> lock(g_auth_state_mu);
    g_auth_state = aegis::PluginAuthState{};
    const auto raw = g_vault.Get(kPluginAuthVaultKey);
    if (!raw) {
        return;
    }
    const auto parsed = aegis::PluginAuthState::FromVaultJson(*raw);
    if (!parsed) {
        blog(LOG_WARNING, "[aegis-obs-plugin] auth state parse failed");
        return;
    }
    g_auth_state = *parsed;
    if (!g_auth_state.login_attempt.Empty()) {
        const bool have_session =
            g_auth_state.authenticated ||
            !g_auth_state.tokens.Empty() ||
            !g_auth_state.session.user.id.empty();
        if (have_session || HasExpiredIsoTimestamp(g_auth_state.login_attempt.expires_at)) {
            blog(LOG_INFO, "[aegis-obs-plugin] clearing stale persisted login attempt");
            g_auth_state.login_attempt.Clear();
            if (g_auth_state.last_error_code.empty()) {
                g_auth_state.last_error_code = "stale_login_attempt_cleared";
            }
            if (g_auth_state.last_error_message.empty()) {
                g_auth_state.last_error_message = "stale_login_attempt_cleared";
            }
            SavePluginAuthStateToVaultLocked();
        }
    }
}

QJsonObject BuildAuthSnapshotJson() {
    std::lock_guard<std::mutex> lock(g_auth_state_mu);

    QJsonObject authObj;
    authObj.insert(QStringLiteral("authenticated"), g_auth_state.authenticated);
    authObj.insert(QStringLiteral("has_tokens"), !g_auth_state.tokens.Empty());

    QJsonObject userObj;
    userObj.insert(QStringLiteral("id"), QString::fromStdString(g_auth_state.session.user.id));
    userObj.insert(QStringLiteral("email"), QString::fromStdString(g_auth_state.session.user.email));
    userObj.insert(QStringLiteral("display_name"), QString::fromStdString(g_auth_state.session.user.display_name));
    authObj.insert(QStringLiteral("user"), userObj);

    QJsonObject entitlementObj;
    entitlementObj.insert(QStringLiteral("relay_access_status"), QString::fromStdString(g_auth_state.session.entitlement.relay_access_status));
    entitlementObj.insert(QStringLiteral("reason_code"), QString::fromStdString(g_auth_state.session.entitlement.reason_code));
    entitlementObj.insert(QStringLiteral("plan_tier"), QString::fromStdString(g_auth_state.session.entitlement.plan_tier));
    entitlementObj.insert(QStringLiteral("plan_status"), QString::fromStdString(g_auth_state.session.entitlement.plan_status));
    entitlementObj.insert(QStringLiteral("max_concurrent_conns"), g_auth_state.session.entitlement.max_concurrent_conns);
    entitlementObj.insert(QStringLiteral("active_managed_conns"), g_auth_state.session.entitlement.active_managed_conns);
    authObj.insert(QStringLiteral("entitlement"), entitlementObj);

    QJsonObject usageObj;
    usageObj.insert(QStringLiteral("included_seconds"), g_auth_state.session.usage.included_seconds);
    usageObj.insert(QStringLiteral("consumed_seconds"), g_auth_state.session.usage.consumed_seconds);
    usageObj.insert(QStringLiteral("remaining_seconds"), g_auth_state.session.usage.remaining_seconds);
    usageObj.insert(QStringLiteral("overage_seconds"), g_auth_state.session.usage.overage_seconds);
    authObj.insert(QStringLiteral("usage"), usageObj);

    if (g_auth_state.session.active_relay) {
        QJsonObject activeRelay;
        activeRelay.insert(QStringLiteral("session_id"), QString::fromStdString(g_auth_state.session.active_relay->session_id));
        activeRelay.insert(QStringLiteral("status"), QString::fromStdString(g_auth_state.session.active_relay->status));
        authObj.insert(QStringLiteral("active_relay"), activeRelay);
    } else {
        authObj.insert(QStringLiteral("active_relay"), QJsonValue(QJsonValue::Null));
    }

    QJsonArray streamSlots;
    for (const auto& slot : g_auth_state.session.stream_slots) {
        QJsonObject streamSlotObj;
        streamSlotObj.insert(QStringLiteral("slot_number"), slot.slot_number);
        streamSlotObj.insert(QStringLiteral("label"), QString::fromStdString(slot.label));
        streamSlotObj.insert(QStringLiteral("stream_token"), QString::fromStdString(slot.stream_token));
        streamSlots.append(streamSlotObj);
    }
    authObj.insert(QStringLiteral("stream_slots"), streamSlots);

    // Pass through linked_accounts as raw JSON from the API response.
    if (!g_auth_state.session.linked_accounts_json.empty()) {
        authObj.insert(QStringLiteral("linked_accounts"),
            QJsonDocument::fromJson(QByteArray::fromStdString(g_auth_state.session.linked_accounts_json)).object());
    }

    QJsonObject loginObj;
    const bool loginPending = !g_auth_state.login_attempt.Empty();
    loginObj.insert(QStringLiteral("pending"), loginPending);
    loginObj.insert(QStringLiteral("login_attempt_id"), QString::fromStdString(g_auth_state.login_attempt.login_attempt_id));
    loginObj.insert(QStringLiteral("authorize_url"), QString::fromStdString(g_auth_state.login_attempt.authorize_url));
    loginObj.insert(QStringLiteral("expires_at"), QString::fromStdString(g_auth_state.login_attempt.expires_at));
    loginObj.insert(QStringLiteral("poll_interval_seconds"), g_auth_state.login_attempt.poll_interval_seconds);
    authObj.insert(QStringLiteral("login"), loginObj);

    authObj.insert(QStringLiteral("last_error_code"), QString::fromStdString(g_auth_state.last_error_code));
    authObj.insert(QStringLiteral("last_error_message"), QString::fromStdString(g_auth_state.last_error_message));
    return authObj;
}

bool OpenBrowserUrl(const std::string& url_text) {
    if (url_text.empty()) {
        return false;
    }
    const QUrl url(QString::fromStdString(url_text));
    if (!url.isValid()) {
        return false;
    }
    return QDesktopServices::openUrl(url);
}

void ApplyCompletedAuthResult(const aegis::AuthPollResult& result) {
    if (!result.session) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_auth_state_mu);
        g_auth_state.tokens = result.tokens;
        g_auth_state.session = *result.session;
        g_auth_state.authenticated = true;
        g_auth_state.login_attempt.Clear();
        g_auth_state.last_error_code.clear();
        g_auth_state.last_error_message.clear();
        SavePluginAuthStateToVaultLocked();
    }
}

} // namespace

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
bool EmitCurrentStatusSnapshotToDock(const char* reason, bool /*force_poll*/) {
    // Poll() now runs in its own background thread (Start()/PollLoop()).
    const auto& snapshot = g_metrics.Latest();
    const std::string mode   = EffectiveDockModeFromConfig();
    const std::string health = DeriveHealthFromSnapshot(snapshot);

    // All relay state and stats come from ConnectionManager's cached values
    // (updated by its background stats thread — no network I/O on the tick thread).
    const auto relay_session_holder = g_connection_manager.CurrentSession();
    const aegis::RelaySession* relay_session_ptr = relay_session_holder.has_value()
                                                       ? &(*relay_session_holder) : nullptr;
    std::string relay_status = "inactive";
    std::string relay_region;
    if (relay_session_ptr) {
        relay_status = relay_session_ptr->status;
        relay_region = relay_session_ptr->region;
    }

    aegis::RelayStats relay_stats = g_connection_manager.CurrentStats();
    const aegis::RelayStats* relay_stats_ptr =
        relay_stats.available ? &relay_stats : nullptr;

    aegis::PerLinkSnapshot per_link = g_connection_manager.CurrentPerLinkStats();
    const aegis::PerLinkSnapshot* per_link_ptr =
        per_link.available ? &per_link : nullptr;

    aegis::ConnectionSnapshot connection_snapshot = g_connection_manager.CurrentSnapshot();

    std::string json =
        g_metrics.BuildStatusSnapshotJson(mode, health, relay_status, relay_region,
                                           relay_session_ptr, relay_stats_ptr, per_link_ptr,
                                           &connection_snapshot);
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (doc.isObject()) {
        QJsonObject payload = doc.object();
        payload.insert(QStringLiteral("settings"), BuildDockSettingsJsonFromConfig());
        payload.insert(QStringLiteral("auth"), BuildAuthSnapshotJson());
        payload.insert(QStringLiteral("chatbot"), g_chatbot_runtime.BuildSnapshotJson(g_config.chat_bot));
        // Serialize relay connection configs (v0.0.5 multi-connection model)
        {
            const auto conn_configs = g_connection_manager.ListConnections();
            // Build each row from that connection's own session/client state.
            QJsonArray relay_conns_arr;
            for (const auto& c : conn_configs) {
                QJsonObject obj;
                obj[QStringLiteral("id")]             = QString::fromStdString(c.id);
                obj[QStringLiteral("name")]           = QString::fromStdString(c.name);
                obj[QStringLiteral("type")]           = QString::fromStdString(c.type);

                std::string effective_host = c.relay_host;
                int effective_port         = c.relay_port;
                std::string effective_stream_token = c.stream_token;
                std::string effective_session_id   = c.session_id;
                if (c.type == "managed") {
                    const auto session_opt =
                        g_connection_manager.CurrentSessionForConnection(c.id);
                    if (session_opt) {
                        if (!session_opt->relay_hostname.empty()) {
                            effective_host = session_opt->relay_hostname;
                        } else if (!session_opt->public_ip.empty()) {
                            effective_host = session_opt->public_ip;
                        }
                        if (session_opt->srt_port > 0) {
                            effective_port = session_opt->srt_port;
                        }
                        if (!session_opt->stream_token.empty()) {
                            effective_stream_token = session_opt->stream_token;
                        }
                        if (!session_opt->session_id.empty()) {
                            effective_session_id = session_opt->session_id;
                        }
                    }
                }
                // For managed connections stream_id is not stored in config - derive from token.
                std::string effective_stream_id = c.stream_id;
                if (c.type == "managed" && effective_stream_id.empty() && !effective_stream_token.empty()) {
                    effective_stream_id = "live_" + effective_stream_token;
                }
                obj[QStringLiteral("relay_host")]     = QString::fromStdString(effective_host);
                obj[QStringLiteral("relay_host_masked")] =
                    QString::fromStdString(MaskRelayHost(effective_host));
                obj[QStringLiteral("relay_port")]     = effective_port;
                obj[QStringLiteral("stream_id")]      = QString::fromStdString(effective_stream_id);
                obj[QStringLiteral("stream_token")]   = QString::fromStdString(effective_stream_token);
                obj[QStringLiteral("sender_url")]     = QString::fromStdString(c.sender_url);
                obj[QStringLiteral("media_source_url")] =
                    QString::fromStdString(c.media_source_url);
                obj[QStringLiteral("managed_region")] = QString::fromStdString(c.managed_region);
                obj[QStringLiteral("stream_slot_number")] = c.stream_slot_number;
                obj[QStringLiteral("stream_slot_label")] = QString::fromStdString(c.stream_slot_label);
                obj[QStringLiteral("session_id")]     = QString::fromStdString(effective_session_id);
                obj[QStringLiteral("status")]         = QString::fromStdString(
                    c.status.empty() ? "idle" : c.status);
                obj[QStringLiteral("error_msg")]      = QString::fromStdString(c.error_msg);

                // Per-connection per-link stats (filtered by stream_id on the C++ side).
                if (c.type == "managed" && (c.status == "ready" || c.status == "live")) {
                    const auto conn_pl = g_connection_manager.CurrentPerLinkStatsForConnection(c.id);
                    const auto conn_st = g_connection_manager.CurrentStatsForConnection(c.id);
                    if (conn_pl.available && !conn_pl.links.empty()) {
                        obj[QStringLiteral("per_link_available")] = true;
                        obj[QStringLiteral("per_link_conn_count")] = conn_pl.conn_count;
                        obj[QStringLiteral("per_link_stream_id")] = QString::fromStdString(conn_pl.stream_id);
                        QJsonArray links_arr;
                        for (const auto& link : conn_pl.links) {
                            QJsonObject lobj;
                            lobj[QStringLiteral("addr")]       = QString::fromStdString(link.addr);
                            lobj[QStringLiteral("asn_org")]    = QString::fromStdString(link.asn_org);
                            lobj[QStringLiteral("bytes")]      = static_cast<double>(link.bytes);
                            lobj[QStringLiteral("pkts")]       = static_cast<double>(link.pkts);
                            lobj[QStringLiteral("share_pct")]  = link.share_pct;
                            lobj[QStringLiteral("last_ms_ago")]= static_cast<int>(link.last_ms_ago);
                            lobj[QStringLiteral("uptime_s")]   = static_cast<int>(link.uptime_s);
                            links_arr.append(lobj);
                        }
                        obj[QStringLiteral("per_link_links")] = links_arr;
                    } else {
                        obj[QStringLiteral("per_link_available")] = false;
                    }
                    if (conn_st.available) {
                        obj[QStringLiteral("stats_available")]    = true;
                        obj[QStringLiteral("stats_bitrate_kbps")] = static_cast<int>(conn_st.bitrate_kbps);
                        obj[QStringLiteral("stats_rtt_ms")]       = conn_st.rtt_ms;
                        obj[QStringLiteral("stats_pkt_loss")]     = static_cast<double>(conn_st.pkt_loss);
                        obj[QStringLiteral("stats_pkt_recv")]     = static_cast<double>(conn_st.pkt_recv);
                    } else {
                        obj[QStringLiteral("stats_available")] = false;
                    }
                }

                relay_conns_arr.append(obj);
            }
            payload.insert(QStringLiteral("relay_connections"), relay_conns_arr);
        }

        // Query the native OBS streaming service platform (e.g. "Twitch", "YouTube").
        // obs_service_get_name() returns the instance name ("default_service"), not the platform.
        // The platform name is in the service settings under the "service" key.
        QString nativeServiceName;
        {
            obs_service_t* svc = obs_frontend_get_streaming_service();
            if (svc) {
                obs_data_t* svc_settings = obs_service_get_settings(svc);
                if (svc_settings) {
                    const char* platform = obs_data_get_string(svc_settings, "service");
                    if (platform && platform[0]) {
                        nativeServiceName = QString::fromUtf8(platform);
                    }
                    // If no "service" key (custom RTMP), derive from server URL
                    if (nativeServiceName.isEmpty()) {
                        const char* server = obs_data_get_string(svc_settings, "server");
                        if (server) {
                            QString u = QString::fromUtf8(server).toLower();
                            if (u.contains("twitch.tv")) nativeServiceName = "Twitch";
                            else if (u.contains("youtube.com")) nativeServiceName = "YouTube";
                            else if (u.contains("facebook.com")) nativeServiceName = "Facebook";
                            else if (u.contains("live-video.net")) nativeServiceName = "Kick";
                        }
                    }
                    obs_data_release(svc_settings);
                }
            }
        }

        // Resolve output display names:
        // 1. Native OBS outputs (adv_stream, simple_stream) → platform name from service API
        // 2. StreamElements outputs (UUID-based) → user label from destinations.json
        // Also mark native stream as "primary" for sort ordering.
        EnsureOutputNamesLoaded();
        {
            QJsonArray outputs = payload.value("multistream_outputs").toArray();
            bool changed = false;
            for (int i = 0; i < outputs.size(); ++i) {
                QJsonObject out = outputs[i].toObject();
                const std::string raw_name = out.value("name").toString().toStdString();

                // Native OBS stream outputs → use platform service name
                if (raw_name == "adv_stream" || raw_name == "simple_stream" ||
                    raw_name == "adv_stream2" || raw_name == "default_service") {
                    if (!nativeServiceName.isEmpty()) {
                        out[QStringLiteral("display_name")] = nativeServiceName;
                    }
                    out[QStringLiteral("is_primary")] = true;
                    outputs[i] = out;
                    changed = true;
                    continue;
                }

                // Multi-stream plugin outputs → resolve from cached map
                if (!g_output_display_names.empty()) {
                    const std::string resolved = ResolveOutputDisplayName(raw_name);
                    if (resolved != raw_name) {
                        out[QStringLiteral("display_name")] = QString::fromStdString(resolved);
                        changed = true;
                    }
                }

                // Resolve canvas name (e.g. "Vertical") from SE compositions
                const std::string canvas = ResolveOutputCanvasName(raw_name);
                if (!canvas.empty()) {
                    out[QStringLiteral("canvas_name")] = QString::fromStdString(canvas);
                    changed = true;
                }

                if (changed) {
                    outputs[i] = out;
                }
            }
            if (changed) {
                payload[QStringLiteral("multistream_outputs")] = outputs;
            }
        }

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
        before.scrollbar != after.scrollbar ||
        before.fontFamily != after.fontFamily ||
        before.fontSizePx != after.fontSizePx ||
        before.densityLevel != after.densityLevel;
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
    g_chatbot_runtime.LoadPrefsFromDisk();
    LoadPluginAuthStateFromVault();
    const std::optional<std::string> relay_shared_key = g_vault.Get("relay_shared_key");

    if (aegis::IsExplicitInsecureHttpHost(g_config.relay_api_host)) {
        blog(LOG_WARNING,
             "[aegis-obs-plugin] relay client skipped: relay_api_host uses insecure http://");
        g_connection_manager.Initialize(&g_vault, &g_http, "", "",
                                        g_config.relay_heartbeat_interval_sec);
    } else if (!g_config.relay_api_host.empty()) {
        g_auth = std::make_unique<aegis::ControlPlaneAuthClient>(g_http, g_config.relay_api_host);
        g_connection_manager.Initialize(&g_vault, &g_http,
                                        g_config.relay_api_host,
                                        relay_shared_key.value_or(""),
                                        g_config.relay_heartbeat_interval_sec);
        blog(LOG_INFO,
            "[aegis-obs-plugin] connection manager initialized: host=%s shared_key=%s",
            g_config.relay_api_host.c_str(),
            relay_shared_key.has_value() ? "configured" : "missing");
    } else {
        blog(LOG_INFO,
             "[aegis-obs-plugin] relay client skipped: relay_api_host not configured");
        g_connection_manager.Initialize(&g_vault, &g_http, "", "",
                                        g_config.relay_heartbeat_interval_sec);
    }
    // Refresh stream tokens on saved connections from auth state before auto-provisioning.
    // Saved configs don't persist stream_token; it must come from per-slot auth state.
    {
        std::lock_guard<std::mutex> lock(g_auth_state_mu);
        auto conns = g_connection_manager.ListConnections();
        for (auto& c : conns) {
            if (c.type != "managed" || c.stream_slot_number < 0) continue;
            for (const auto& slot : g_auth_state.session.stream_slots) {
                if (slot.slot_number == c.stream_slot_number &&
                    !slot.stream_token.empty() &&
                    c.stream_token != slot.stream_token) {
                    c.stream_token = slot.stream_token;
                    g_connection_manager.UpdateConnection(c.id, c);
                    blog(LOG_INFO,
                         "[aegis-obs-plugin] refreshed stream_token for conn=%s slot=%d",
                         c.id.c_str(), c.stream_slot_number);
                    break;
                }
            }
        }
    }
    g_connection_manager.AutoProvisionSavedConnections(
        CurrentControlPlaneJwt(), g_config.relay_heartbeat_interval_sec);

    g_metrics.Start(g_config.metrics_poll_interval_ms);

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

    CacheDockPipeStatusForReplay("ok", "native plugin v0.0.5");

    if (!g_frontend_event_callback_registered) {
        obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
        g_frontend_event_callback_registered = true;
        blog(LOG_INFO, "[aegis-obs-plugin] registered frontend event callback");
    }
    if (!g_tools_menu_show_dock_registered) {
        obs_frontend_add_tools_menu_item("Show Telemy Dock", OnToolsMenuShowDock, nullptr);
        g_tools_menu_show_dock_registered = true;
        blog(LOG_INFO, "[aegis-obs-plugin] registered Tools menu item: Show Telemy Dock");
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

    // Stop metrics background thread before relay teardown.
    g_metrics.Stop();

    // Emergency relay teardown + shutdown ConnectionManager.
    if (g_connection_manager.HasActiveSession()) {
        blog(LOG_INFO, "[aegis-obs-plugin] relay emergency stop on unload");
        const std::string jwt = CurrentControlPlaneJwt();
        g_connection_manager.EmergencyStop(jwt);
    }
    // Shutdown ConnectionManager (stops background thread, joins workers,
    // destroys RelayClient) before g_http is destroyed.
    g_connection_manager.Shutdown();
    // Drain any in-flight auth worker futures before destroying g_auth to
    // prevent use-after-free if a thread is still referencing g_auth.
    DrainAuthWorkers();
    g_auth.reset();
    blog(LOG_INFO, "[aegis-obs-plugin] connection manager destroyed");
}

const char* obs_module_description(void) {
    return "Telemy OBS Plugin (v0.0.5)";
}

#else
int aegis_obs_plugin_entry_placeholder() {
    return 0;
}
#endif
