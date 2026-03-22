#include "dock_action_dispatch.h"
#include "dock_replay_cache.h"
#include "config_vault.h"
#include "relay_client.h"
#include "connection_manager.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QDir>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Shared utilities — defined in obs_plugin_entry.cpp
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
std::string BuildRelaySessionDetailJson(const aegis::RelaySession& session);
bool EmitCurrentStatusSnapshotToDock(const char* reason, bool force_poll);
void LogSceneSnapshot(const char* reason);

// ---------------------------------------------------------------------------
// Extern globals — defined in obs_plugin_entry.cpp
// ---------------------------------------------------------------------------
extern aegis::HttpsClient                             g_http;
extern aegis::Vault                                   g_vault;
extern aegis::PluginConfig                            g_config;
extern std::unique_ptr<aegis::ControlPlaneAuthClient> g_auth;
extern std::mutex                                     g_auth_state_mu;
extern aegis::PluginAuthState                         g_auth_state;
extern aegis::ConnectionManager                       g_connection_manager;

// ---------------------------------------------------------------------------
// Module-local globals
// ---------------------------------------------------------------------------
static std::mutex g_pending_switch_requests_mu;
static std::vector<PendingSwitchRequest> g_pending_switch_requests;
static std::mutex g_pending_request_status_action_ids_mu;
static std::vector<std::string> g_pending_request_status_action_ids;
static std::mutex g_pending_set_mode_actions_mu;
static std::vector<PendingSetModeAction> g_pending_set_mode_actions;
static std::mutex g_pending_set_setting_actions_mu;
static std::vector<PendingSetSettingAction> g_pending_set_setting_actions;
static std::mutex g_pending_relay_actions_mu;
static std::vector<PendingRelayAction> g_pending_relay_actions;
static constexpr std::chrono::milliseconds kDockActionCompletionTimeoutMs(3000);
static constexpr std::chrono::milliseconds kDockActionDuplicateWindowMs(1500);
static constexpr std::chrono::seconds      kConnectRateLimitWindow(3);
static std::uint64_t g_local_dock_action_seq = 0;
static std::mutex g_recent_dock_actions_mu;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_recent_dock_actions;
static std::mutex g_connect_rate_limit_mu;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_connect_rate_limit;

// ---------------------------------------------------------------------------
// Auth worker lifetime tracking (use-after-free fix for detached auth threads)
// All auth action handlers post futures here instead of detach()-ing threads.
// DrainAuthWorkers() is called from obs_module_unload before g_auth.reset().
// ---------------------------------------------------------------------------
static std::mutex g_auth_workers_mu;
static std::vector<std::future<void>> g_auth_workers;

static void TrackAuthWorker(std::future<void>&& f)
{
    std::lock_guard<std::mutex> lk(g_auth_workers_mu);
    // Prune already-completed futures to avoid unbounded growth.
    g_auth_workers.erase(
        std::remove_if(g_auth_workers.begin(), g_auth_workers.end(),
                       [](std::future<void>& fut) {
                           return fut.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready;
                       }),
        g_auth_workers.end());
    g_auth_workers.push_back(std::move(f));
}

void DrainAuthWorkers()
{
    std::lock_guard<std::mutex> lk(g_auth_workers_mu);
    for (auto& f : g_auth_workers) {
        if (f.valid()) {
            f.wait();
        }
    }
    g_auth_workers.clear();
}

namespace {

constexpr const char* kPluginAuthVaultKey = "plugin_auth_state";
constexpr const char* kLegacyRelayJwtVaultKey = "relay_jwt";

bool PersistAuthStateLocked() {
    const bool ok = g_vault.Set(kPluginAuthVaultKey, g_auth_state.ToVaultJson());
    if (!g_auth_state.tokens.cp_access_jwt.empty()) {
        (void)g_vault.Set(kLegacyRelayJwtVaultKey, g_auth_state.tokens.cp_access_jwt);
    }
    return ok;
}

void ClearLoginAttemptStateLocked(const std::string& error_code,
                                  const std::string& error_message) {
    g_auth_state.login_attempt.Clear();
    g_auth_state.last_error_code = error_code;
    g_auth_state.last_error_message = error_message;
    PersistAuthStateLocked();
}

std::optional<aegis::StreamSlot> FindStreamSlotLocked(int slot_number) {
    if (slot_number < 0) {
        return std::nullopt;
    }
    const auto it = std::find_if(
        g_auth_state.session.stream_slots.begin(),
        g_auth_state.session.stream_slots.end(),
        [slot_number](const aegis::StreamSlot& slot) {
            return slot.slot_number == slot_number;
        });
    if (it == g_auth_state.session.stream_slots.end()) {
        return std::nullopt;
    }
    return *it;
}

void UpdateStreamSlotLabelInAuthStateLocked(int slot_number, const std::string& label) {
    for (auto& slot : g_auth_state.session.stream_slots) {
        if (slot.slot_number == slot_number) {
            slot.label = label;
            break;
        }
    }
}

std::string CurrentControlPlaneJwtForActions() {
    {
        std::lock_guard<std::mutex> lock(g_auth_state_mu);
        if (!g_auth_state.tokens.cp_access_jwt.empty()) {
            return g_auth_state.tokens.cp_access_jwt;
        }
    }
    return g_vault.Get(kLegacyRelayJwtVaultKey).value_or("");
}

std::string CurrentRefreshTokenForActions() {
    std::lock_guard<std::mutex> lock(g_auth_state_mu);
    return g_auth_state.tokens.refresh_token;
}

void ClearAuthStateAndPersist(bool clear_legacy_jwt) {
    std::lock_guard<std::mutex> lock(g_auth_state_mu);
    g_auth_state = aegis::PluginAuthState{};
    (void)g_vault.Remove(kPluginAuthVaultKey);
    if (clear_legacy_jwt) {
        (void)g_vault.Remove(kLegacyRelayJwtVaultKey);
    }
}

void ApplyCompletedAuthResultAndPersist(const aegis::AuthPollResult& result) {
    if (!result.session) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_auth_state_mu);
    g_auth_state.tokens = result.tokens;
    g_auth_state.session = *result.session;
    g_auth_state.authenticated = true;
    g_auth_state.login_attempt.Clear();
    g_auth_state.last_error_code.clear();
    g_auth_state.last_error_message.clear();
    PersistAuthStateLocked();
}

std::string BuildAuthStateDetailJson() {
    QJsonObject authObj;
    {
        std::lock_guard<std::mutex> lock(g_auth_state_mu);
        authObj["authenticated"] = g_auth_state.authenticated;
        authObj["has_tokens"] = !g_auth_state.tokens.Empty();

        QJsonObject userObj;
        userObj["id"] = QString::fromStdString(g_auth_state.session.user.id);
        userObj["email"] = QString::fromStdString(g_auth_state.session.user.email);
        userObj["display_name"] = QString::fromStdString(g_auth_state.session.user.display_name);
        authObj["user"] = userObj;

        QJsonObject entitlementObj;
        entitlementObj["relay_access_status"] = QString::fromStdString(g_auth_state.session.entitlement.relay_access_status);
        entitlementObj["reason_code"] = QString::fromStdString(g_auth_state.session.entitlement.reason_code);
        entitlementObj["plan_tier"] = QString::fromStdString(g_auth_state.session.entitlement.plan_tier);
        entitlementObj["plan_status"] = QString::fromStdString(g_auth_state.session.entitlement.plan_status);
        authObj["entitlement"] = entitlementObj;

        QJsonObject usageObj;
        usageObj["included_seconds"] = g_auth_state.session.usage.included_seconds;
        usageObj["consumed_seconds"] = g_auth_state.session.usage.consumed_seconds;
        usageObj["remaining_seconds"] = g_auth_state.session.usage.remaining_seconds;
        usageObj["overage_seconds"] = g_auth_state.session.usage.overage_seconds;
        authObj["usage"] = usageObj;

        QJsonArray streamSlots;
        for (const auto& slot : g_auth_state.session.stream_slots) {
            QJsonObject slotObj;
            slotObj["slot_number"] = slot.slot_number;
            slotObj["label"] = QString::fromStdString(slot.label);
            slotObj["stream_token"] = QString::fromStdString(slot.stream_token);
            streamSlots.append(slotObj);
        }
        authObj["stream_slots"] = streamSlots;

        QJsonObject loginObj;
        loginObj["pending"] = !g_auth_state.login_attempt.Empty();
        loginObj["login_attempt_id"] = QString::fromStdString(g_auth_state.login_attempt.login_attempt_id);
        loginObj["authorize_url"] = QString::fromStdString(g_auth_state.login_attempt.authorize_url);
        loginObj["expires_at"] = QString::fromStdString(g_auth_state.login_attempt.expires_at);
        loginObj["poll_interval_seconds"] = g_auth_state.login_attempt.poll_interval_seconds;
        authObj["login"] = loginObj;

        authObj["last_error_code"] = QString::fromStdString(g_auth_state.last_error_code);
        authObj["last_error_message"] = QString::fromStdString(g_auth_state.last_error_message);
    }
    return QJsonDocument(authObj).toJson(QJsonDocument::Compact).toStdString();
}

bool TryOpenBrowserUrl(const std::string& url_text) {
    if (url_text.empty()) {
        return false;
    }
    const QUrl url(QString::fromStdString(url_text));
    if (!url.isValid()) {
        return false;
    }
    return QDesktopServices::openUrl(url);
}

bool TryExtractJsonIntField(
    const std::string& json_text,
    const char* field_name,
    int* out_value) {
    if (!field_name || !out_value) {
        return false;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(json_text), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonValue val = doc.object().value(QString::fromUtf8(field_name));
    if (!val.isDouble()) {
        return false;
    }
    *out_value = val.toInt();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Pending action accessors (cross-module)
// ---------------------------------------------------------------------------
std::mutex& GetPendingRelayActionsMutex() {
    return g_pending_relay_actions_mu;
}

std::vector<PendingRelayAction>& GetPendingRelayActionsRef() {
    return g_pending_relay_actions;
}

// ---------------------------------------------------------------------------
// Action deduplication
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Pending action tracking
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Status snapshot projection
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Config/settings helpers
// ---------------------------------------------------------------------------
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
    settings.insert(QStringLiteral("byor_enabled"), g_config.byor_enabled);
    settings.insert(QStringLiteral("byor_relay_host"), QString::fromStdString(g_config.byor_relay_host));
    settings.insert(QStringLiteral("byor_relay_port"), g_config.byor_relay_port);
    settings.insert(QStringLiteral("byor_stream_id"), QString::fromStdString(g_config.byor_stream_id));
    return settings;
}

// ---------------------------------------------------------------------------
// Scene switch helpers
// ---------------------------------------------------------------------------
static std::string CurrentSceneName() {
    obs_source_t* current = obs_frontend_get_current_scene();
    if (!current) {
        return {};
    }
    const char* current_name = obs_source_get_name(current);
    std::string out = current_name ? std::string(current_name) : std::string();
    obs_source_release(current);
    return out;
}

static bool IsCurrentSceneName(const std::string& expected_scene_name) {
    if (expected_scene_name.empty()) {
        return false;
    }
    return expected_scene_name == CurrentSceneName();
}

static bool IsDockUiActionReason(const std::string& reason) {
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

// ---------------------------------------------------------------------------
// Provision step info
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Clear all pending action queues
// ---------------------------------------------------------------------------
void ClearAllPendingDockActions() {
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
    {
        std::lock_guard<std::mutex> lk(g_connect_rate_limit_mu);
        g_connect_rate_limit.clear();
    }
}

// ---------------------------------------------------------------------------
// Main action dispatch — the body of aegis_obs_shim_receive_dock_action_json
// ---------------------------------------------------------------------------
bool DispatchDockAction(const std::string& action_json,
                        const std::string& action_type,
                        const std::string& request_id) {

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

    // -- load_scene_prefs: read dock_scene_prefs.json from plugin config dir --
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

            // No prefs file at either path -- return empty object so the dock hydrates cleanly
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

    // -- save_scene_prefs: write prefsJson to dock_scene_prefs.json --
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

    // -- request_scene_snapshot: force a fresh OBS scene enumeration --
    if (action_type == "request_scene_snapshot") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=request_scene_snapshot request_id=%s",
            request_id.c_str());
        LogSceneSnapshot("dock_request");
        EmitDockActionResult(action_type, request_id, "completed", true, "", "scene_snapshot_emitted");
        return true;
    }

    if (action_type == "auth_request_status") {
        EmitCurrentStatusSnapshotToDock("auth_request_status", false);
        EmitDockActionResult(action_type, request_id, "completed", true, "", BuildAuthStateDetailJson());
        return true;
    }

    if (action_type == "auth_login_start") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action auth_login_start: request_id=%s",
             request_id.c_str());
        if (!g_auth) {
            EmitDockActionResult(action_type, request_id, "failed", false, "auth_not_configured", "");
            return false;
        }

        std::string device_name = "OBS Desktop";
        (void)TryExtractJsonStringField(action_json, "deviceName", &device_name);
        if (device_name.empty()) {
            device_name = "OBS Desktop";
        }

        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        TrackAuthWorker(std::async(std::launch::async, [req_id = request_id, device_name]() {
            try {
                auto attempt = g_auth->StartPluginLogin(device_name);
                if (!attempt) {
                    EmitDockActionResult("auth_login_start", req_id, "failed", false, "login_start_failed", "");
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(g_auth_state_mu);
                    g_auth_state.login_attempt = *attempt;
                    g_auth_state.last_error_code.clear();
                    g_auth_state.last_error_message.clear();
                    PersistAuthStateLocked();
                }

                const bool browser_ok = TryOpenBrowserUrl(attempt->authorize_url);
                QJsonObject detailObj;
                detailObj["login_attempt_id"] = QString::fromStdString(attempt->login_attempt_id);
                detailObj["authorize_url"] = QString::fromStdString(attempt->authorize_url);
                detailObj["expires_at"] = QString::fromStdString(attempt->expires_at);
                detailObj["poll_interval_seconds"] = attempt->poll_interval_seconds;
                detailObj["browser_opened"] = browser_ok;
                EmitCurrentStatusSnapshotToDock("auth_login_start", false);
                EmitDockActionResult("auth_login_start", req_id, "completed", true, "",
                                     QJsonDocument(detailObj).toJson(QJsonDocument::Compact).toStdString());
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(g_auth_state_mu);
                    g_auth_state.last_error_code = "login_start_exception";
                    g_auth_state.last_error_message = e.what();
                    PersistAuthStateLocked();
                }
                EmitCurrentStatusSnapshotToDock("auth_login_start_exception", false);
                EmitDockActionResult("auth_login_start", req_id, "failed", false, e.what(), "");
            }
        }));
        return true;
    }

    if (action_type == "auth_login_poll") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action auth_login_poll: request_id=%s",
             request_id.c_str());
        if (!g_auth) {
            EmitDockActionResult(action_type, request_id, "failed", false, "auth_not_configured", "");
            return false;
        }

        std::string login_attempt_id;
        std::string poll_token;
        {
            std::lock_guard<std::mutex> lock(g_auth_state_mu);
            login_attempt_id = g_auth_state.login_attempt.login_attempt_id;
            poll_token = g_auth_state.login_attempt.poll_token;
        }
        (void)TryExtractJsonStringField(action_json, "loginAttemptId", &login_attempt_id);
        (void)TryExtractJsonStringField(action_json, "pollToken", &poll_token);
        if (login_attempt_id.empty() || poll_token.empty()) {
            EmitDockActionResult(action_type, request_id, "failed", false, "no_login_attempt", "");
            return false;
        }

        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        TrackAuthWorker(std::async(std::launch::async, [req_id = request_id, login_attempt_id, poll_token]() {
            try {
                auto result = g_auth->PollPluginLogin(login_attempt_id, poll_token);
                if (!result) {
                    {
                        std::lock_guard<std::mutex> lock(g_auth_state_mu);
                        if (g_auth_state.login_attempt.login_attempt_id == login_attempt_id) {
                            ClearLoginAttemptStateLocked("login_poll_failed", "login_poll_failed");
                        }
                    }
                    EmitCurrentStatusSnapshotToDock("auth_login_poll_failed", false);
                    EmitDockActionResult("auth_login_poll", req_id, "failed", false, "login_poll_failed", "");
                    return;
                }

                if (result->status == aegis::AuthPollStatus::Pending) {
                    EmitDockActionResult("auth_login_poll", req_id, "pending", true, "", "");
                    return;
                }

                if (result->status == aegis::AuthPollStatus::Completed) {
                    ApplyCompletedAuthResultAndPersist(*result);
                    EmitCurrentStatusSnapshotToDock("auth_login_poll_completed", false);
                    EmitDockActionResult("auth_login_poll", req_id, "completed", true, "",
                                         BuildAuthStateDetailJson());
                    return;
                }

                std::string reason = result->reason_code.empty() ? "login_denied" : result->reason_code;
                {
                    std::lock_guard<std::mutex> lock(g_auth_state_mu);
                    g_auth_state.authenticated = false;
                    g_auth_state.tokens.Clear();
                    g_auth_state.login_attempt.Clear();
                    g_auth_state.last_error_code = reason;
                    g_auth_state.last_error_message = reason;
                    PersistAuthStateLocked();
                }
                EmitCurrentStatusSnapshotToDock("auth_login_poll_terminal", false);
                const char* status = result->status == aegis::AuthPollStatus::Expired ? "expired" : "denied";
                EmitDockActionResult("auth_login_poll", req_id, status, false, reason, "");
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> lock(g_auth_state_mu);
                    if (g_auth_state.login_attempt.login_attempt_id == login_attempt_id) {
                        ClearLoginAttemptStateLocked("login_poll_exception", e.what());
                    }
                }
                EmitCurrentStatusSnapshotToDock("auth_login_poll_exception", false);
                EmitDockActionResult("auth_login_poll", req_id, "failed", false, e.what(), "");
            }
        }));
        return true;
    }

    if (action_type == "auth_refresh") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action auth_refresh: request_id=%s",
             request_id.c_str());
        if (!g_auth) {
            EmitDockActionResult(action_type, request_id, "failed", false, "auth_not_configured", "");
            return false;
        }

        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        TrackAuthWorker(std::async(std::launch::async, [req_id = request_id]() {
            try {
                const std::string refresh_token = CurrentRefreshTokenForActions();
                if (refresh_token.empty()) {
                    const std::string jwt = CurrentControlPlaneJwtForActions();
                    if (jwt.empty()) {
                        EmitDockActionResult("auth_refresh", req_id, "failed", false, "no_auth_session", "");
                        return;
                    }
                    auto session = g_auth->GetSession(jwt);
                    if (!session) {
                        EmitDockActionResult("auth_refresh", req_id, "failed", false, "session_refresh_failed", "");
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_auth_state_mu);
                        g_auth_state.session = *session;
                        g_auth_state.authenticated = true;
                        g_auth_state.last_error_code.clear();
                        g_auth_state.last_error_message.clear();
                        PersistAuthStateLocked();
                    }
                } else {
                    auto result = g_auth->Refresh(refresh_token);
                    if (!result || result->status != aegis::AuthPollStatus::Completed || !result->session) {
                        EmitDockActionResult("auth_refresh", req_id, "failed", false, "session_refresh_failed", "");
                        return;
                    }
                    ApplyCompletedAuthResultAndPersist(*result);
                }
                EmitCurrentStatusSnapshotToDock("auth_refresh", false);
                EmitDockActionResult("auth_refresh", req_id, "completed", true, "", BuildAuthStateDetailJson());
            } catch (const std::exception& e) {
                EmitDockActionResult("auth_refresh", req_id, "failed", false, e.what(), "");
            }
        }));
        return true;
    }

    if (action_type == "auth_open_browser") {
        std::string authorize_url;
        {
            std::lock_guard<std::mutex> lock(g_auth_state_mu);
            authorize_url = g_auth_state.login_attempt.authorize_url;
        }
        (void)TryExtractJsonStringField(action_json, "authorizeUrl", &authorize_url);
        if (authorize_url.empty()) {
            EmitDockActionResult(action_type, request_id, "failed", false, "no_authorize_url", "");
            return false;
        }
        const bool opened = TryOpenBrowserUrl(authorize_url);
        EmitDockActionResult(action_type, request_id, opened ? "completed" : "failed",
                             opened, opened ? "" : "browser_open_failed", "");
        return opened;
    }

    if (action_type == "auth_login_cancel") {
        {
            std::lock_guard<std::mutex> lock(g_auth_state_mu);
            if (!g_auth_state.login_attempt.Empty()) {
                ClearLoginAttemptStateLocked("login_cancelled", "login_cancelled");
            } else {
                g_auth_state.last_error_code.clear();
                g_auth_state.last_error_message.clear();
                PersistAuthStateLocked();
            }
        }
        EmitCurrentStatusSnapshotToDock("auth_login_cancel", false);
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        return true;
    }

    if (action_type == "auth_logout") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action auth_logout: request_id=%s",
             request_id.c_str());
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        TrackAuthWorker(std::async(std::launch::async, [req_id = request_id]() {
            const std::string jwt = CurrentControlPlaneJwtForActions();
            if (g_auth && !jwt.empty()) {
                (void)g_auth->Logout(jwt);
            }
            ClearAuthStateAndPersist(true);
            EmitCurrentStatusSnapshotToDock("auth_logout", false);
            EmitDockActionResult("auth_logout", req_id, "completed", true, "", "");
        }));
        return true;
    }

    if (action_type == "relay_start") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action relay_start: request_id=%s",
            request_id.c_str());
        const std::string jwt = CurrentControlPlaneJwtForActions();
        if (jwt.empty()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_start request_id=%s error=no_control_plane_auth",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_control_plane_auth", "");
            return false;
        }
        g_connection_manager.StartManagedRelayAsync(
            jwt, request_id, g_config.relay_heartbeat_interval_sec);
        return true;
    }

    if (action_type == "relay_stop") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action relay_stop: request_id=%s",
            request_id.c_str());
        if (!g_connection_manager.HasActiveSession()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_stop request_id=%s error=no_active_session",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_active_session", "");
            return false;
        }
        const std::string jwt = CurrentControlPlaneJwtForActions();
        g_connection_manager.StopManagedRelayAsync(jwt, request_id);
        return true;
    }

    if (action_type == "relay_connect_direct") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action relay_connect_direct: request_id=%s",
            request_id.c_str());
        std::string relay_host = g_config.byor_relay_host;
        std::string stream_id = g_config.byor_stream_id;
        int relay_port = g_config.byor_relay_port;
        std::string stream_token;
        (void)TryExtractJsonStringField(action_json, "relay_host", &relay_host);
        (void)TryExtractJsonStringField(action_json, "stream_id", &stream_id);
        (void)TryExtractJsonStringField(action_json, "stream_token", &stream_token);
        (void)TryExtractJsonIntField(action_json, "relay_port", &relay_port);

        if (relay_host.empty()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_connect_direct request_id=%s error=missing_byor_relay_host",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "missing_byor_relay_host", "");
            return false;
        }
        if (relay_port < 1 || relay_port > 65535) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_connect_direct request_id=%s error=invalid_byor_relay_port",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "invalid_byor_relay_port", "");
            return false;
        }

        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        const std::string effective_stream_id =
            stream_id.empty() ? stream_token : stream_id;
        g_connection_manager.ConnectDirect(relay_host, relay_port, effective_stream_id);
        auto session = g_connection_manager.CurrentSession();
        if (!session) {
            EmitDockActionResult(action_type, request_id, "failed", false, "relay_connect_direct_failed", "");
            return false;
        }
        blog(LOG_INFO,
            "[aegis-obs-plugin] relay_connect_direct completed: request_id=%s host=%s port=%d",
            request_id.c_str(),
            relay_host.c_str(),
            relay_port);
        EmitDockActionResult(action_type, request_id, "completed", true, "",
                             BuildRelaySessionDetailJson(*session));
        return true;
    }

    if (action_type == "relay_disconnect_direct") {
        blog(
            LOG_INFO,
            "[aegis-obs-plugin] dock action relay_disconnect_direct: request_id=%s",
            request_id.c_str());
        if (!g_connection_manager.IsBYORMode() || !g_connection_manager.HasActiveSession()) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action failed: type=relay_disconnect_direct request_id=%s error=no_active_byor_session",
                request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "no_active_byor_session", "");
            return false;
        }

        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        g_connection_manager.DisconnectDirect();
        blog(LOG_INFO,
            "[aegis-obs-plugin] relay_disconnect_direct completed: request_id=%s",
            request_id.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        return true;
    }

    // -- load_config: return current plugin config fields to dock --
    if (action_type == "load_config") {
        std::ostringstream detail;
        detail << "{";
        detail << "\"relay_api_host\":" << JsStringLiteral(g_config.relay_api_host) << ",";
        detail << "\"relay_shared_key_present\":" << (g_vault.Get("relay_shared_key").has_value() ? "true" : "false") << ",";
        detail << "\"relay_heartbeat_interval_sec\":" << g_config.relay_heartbeat_interval_sec << ",";
        detail << "\"metrics_poll_interval_ms\":" << g_config.metrics_poll_interval_ms << ",";
        detail << "\"byor_enabled\":" << (g_config.byor_enabled ? "true" : "false") << ",";
        detail << "\"byor_relay_host\":" << JsStringLiteral(g_config.byor_relay_host) << ",";
        detail << "\"byor_relay_port\":" << g_config.byor_relay_port << ",";
        detail << "\"byor_stream_id\":" << JsStringLiteral(g_config.byor_stream_id) << ",";
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

    // -- save_config: receive config fields from dock and persist --
    if (action_type == "save_config") {
        std::string relay_api_host;
        std::string relay_shared_key;
        std::string relay_heartbeat_interval_sec_str;
        std::string metrics_poll_interval_ms_str;
        std::string byor_relay_host;
        std::string byor_stream_id;
        std::string grafana_otlp_endpoint;

        (void)TryExtractJsonStringField(action_json, "relay_api_host", &relay_api_host);
        const bool has_relay_shared_key =
            TryExtractJsonStringField(action_json, "relay_shared_key", &relay_shared_key);
        (void)TryExtractJsonStringField(action_json, "relay_heartbeat_interval_sec",
                                         &relay_heartbeat_interval_sec_str);
        (void)TryExtractJsonStringField(action_json, "metrics_poll_interval_ms",
                                         &metrics_poll_interval_ms_str);
        const bool has_byor_relay_host =
            TryExtractJsonStringField(action_json, "byor_relay_host", &byor_relay_host);
        const bool has_byor_stream_id =
            TryExtractJsonStringField(action_json, "byor_stream_id", &byor_stream_id);
        bool grafana_enabled_val = false;
        bool byor_enabled_val = false;
        const bool has_grafana_enabled =
            TryExtractJsonBoolField(action_json, "grafana_enabled", &grafana_enabled_val);
        const bool has_byor_enabled =
            TryExtractJsonBoolField(action_json, "byor_enabled", &byor_enabled_val);
        int byor_relay_port = g_config.byor_relay_port;
        const bool has_byor_relay_port =
            TryExtractJsonIntField(action_json, "byor_relay_port", &byor_relay_port);
        (void)TryExtractJsonStringField(action_json, "grafana_otlp_endpoint",
                                         &grafana_otlp_endpoint);

        if (aegis::IsExplicitInsecureHttpHost(relay_api_host)) {
            blog(LOG_WARNING,
                "[aegis-obs-plugin] dock action rejected: type=save_config request_id=%s error=insecure_relay_api_host",
                request_id.c_str());
            EmitDockActionResult(
                action_type,
                request_id,
                "rejected",
                false,
                "insecure_relay_api_host",
                "{\"message\":\"relay_api_host must use https:// or omit the scheme\"}");
            return true;
        }
        if (!relay_api_host.empty()) {
            g_config.relay_api_host = relay_api_host;
        }
        if (has_relay_shared_key) {
            bool vault_saved = true;
            if (relay_shared_key.empty()) {
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
        if (has_byor_enabled) {
            g_config.byor_enabled = byor_enabled_val;
        }
        if (has_byor_relay_host) {
            g_config.byor_relay_host = byor_relay_host;
        }
        if (has_byor_stream_id) {
            g_config.byor_stream_id = byor_stream_id;
        }
        if (has_byor_relay_port && byor_relay_port >= 1 && byor_relay_port <= 65535) {
            g_config.byor_relay_port = byor_relay_port;
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
        if (!g_config.relay_api_host.empty()) {
            const auto new_key = g_vault.Get("relay_shared_key");
            g_connection_manager.Reconfigure(g_config.relay_api_host, new_key.value_or(""));
        }

        blog(LOG_INFO,
            "[aegis-obs-plugin] dock action completed: type=save_config request_id=%s",
            request_id.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        return true;
    }

    // -- vault_set: encrypt and store a secret --
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

    // vault_get: disabled -- secrets must not be readable from dock JS.
    if (action_type == "vault_get") {
        blog(LOG_WARNING,
            "[aegis-obs-plugin] dock action rejected: type=vault_get request_id=%s error=secret_readback_disabled",
            request_id.c_str());
        EmitDockActionResult(action_type, request_id, "rejected", false, "secret_readback_disabled", "");
        return false;
    }

    // vault_keys: disabled -- vault key enumeration must not be exposed to dock JS.
    if (action_type == "vault_keys") {
        blog(LOG_WARNING,
            "[aegis-obs-plugin] dock action rejected: type=vault_keys request_id=%s error=vault_keys_disabled",
            request_id.c_str());
        EmitDockActionResult(action_type, request_id, "rejected", false, "vault_keys_disabled", "");
        return false;
    }

    if (action_type == "connection_add") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action connection_add: request_id=%s",
             request_id.c_str());
        std::string name, type, relay_host, stream_id, managed_region;
        int stream_slot_number = 0;
        int relay_port = 5000;
        (void)TryExtractJsonStringField(action_json, "name", &name);
        (void)TryExtractJsonStringField(action_json, "conn_type", &type);
        (void)TryExtractJsonStringField(action_json, "relay_host", &relay_host);
        (void)TryExtractJsonStringField(action_json, "stream_id", &stream_id);
        (void)TryExtractJsonStringField(action_json, "managed_region", &managed_region);
        (void)TryExtractJsonIntField(action_json, "stream_slot", &stream_slot_number);
        (void)TryExtractJsonIntField(action_json, "relay_port", &relay_port);
        if (name.empty()) {
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_name", "");
            return false;
        }
        aegis::RelayConnectionConfig config;
        config.name           = name;
        config.type           = type.empty() ? "byor" : type;
        config.relay_host     = relay_host;
        config.relay_port     = relay_port;
        config.stream_id      = stream_id;
        config.managed_region = managed_region;
        config.status         = "idle";
        if (config.type == "managed") {
            std::lock_guard<std::mutex> lock(g_auth_state_mu);
            const auto slot = FindStreamSlotLocked(stream_slot_number);
            if (!slot) {
                EmitDockActionResult(action_type, request_id, "rejected", false,
                                     "invalid_stream_slot", "");
                return false;
            }
            config.stream_slot_number = slot->slot_number;
            config.stream_slot_label = slot->label;
            config.stream_token = slot->stream_token;
        }
        // Generate a simple UUID-like ID from timestamp + counter.
        {
            static std::mutex id_mu;
            static uint64_t   id_seq = 0;
            std::lock_guard<std::mutex> lk(id_mu);
            const auto now_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            config.id = "conn_" + std::to_string(now_ms) + "_" + std::to_string(++id_seq);
        }
        const std::string new_id = g_connection_manager.AddConnection(config);
        blog(LOG_INFO,
             "[aegis-obs-plugin] connection_add completed: id=%s name=%s type=%s",
             new_id.c_str(), name.c_str(), config.type.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "",
                             "{\"id\":\"" + new_id + "\"}");
        EmitCurrentStatusSnapshotToDock("connection_add", false);
        return true;
    }

    if (action_type == "connection_remove") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action connection_remove: request_id=%s",
             request_id.c_str());
        std::string conn_id;
        (void)TryExtractJsonStringField(action_json, "id", &conn_id);
        if (conn_id.empty()) {
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_id", "");
            return false;
        }
        g_connection_manager.RemoveConnection(conn_id);
        blog(LOG_INFO,
             "[aegis-obs-plugin] connection_remove completed: id=%s", conn_id.c_str());
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        EmitCurrentStatusSnapshotToDock("connection_remove", false);
        return true;
    }

    if (action_type == "connection_connect") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action connection_connect: request_id=%s",
             request_id.c_str());
        std::string conn_id;
        (void)TryExtractJsonStringField(action_json, "id", &conn_id);
        if (conn_id.empty()) {
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_id", "");
            return false;
        }
        // Rate-limit: reject connection_connect if a previous attempt for this
        // connection was dispatched within kConnectRateLimitWindow (3s).
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(g_connect_rate_limit_mu);
            auto it2 = g_connect_rate_limit.find(conn_id);
            if (it2 != g_connect_rate_limit.end() &&
                (now - it2->second) < kConnectRateLimitWindow) {
                blog(LOG_WARNING,
                     "[aegis-obs-plugin] connection_connect rate-limited:"
                     " conn_id=%s request_id=%s",
                     conn_id.c_str(), request_id.c_str());
                return true; // silently drop
            }
            g_connect_rate_limit[conn_id] = now;
        }
        // Managed connections go through the async relay start path; BYOR is synchronous.
        const auto conns = g_connection_manager.ListConnections();
        const auto it = std::find_if(conns.begin(), conns.end(),
                                     [&conn_id](const aegis::RelayConnectionConfig& c) {
                                         return c.id == conn_id;
                                     });
        if (it != conns.end() && it->type == "managed") {
            const std::string jwt = CurrentControlPlaneJwtForActions();
            if (jwt.empty()) {
                blog(LOG_WARNING,
                     "[aegis-obs-plugin] dock action failed: type=connection_connect"
                     " request_id=%s error=no_control_plane_auth",
                     request_id.c_str());
                EmitDockActionResult(action_type, request_id, "failed", false,
                                     "no_control_plane_auth", "");
                return false;
            }
            aegis::RelayConnectionConfig managed = *it;
            bool managed_slot_refreshed = false;
            {
                std::lock_guard<std::mutex> lock(g_auth_state_mu);
                if (managed.stream_slot_number > 0) {
                    if (const auto slot = FindStreamSlotLocked(managed.stream_slot_number)) {
                        managed.stream_slot_label = slot->label;
                        managed.stream_token = slot->stream_token;
                        managed_slot_refreshed = true;
                    }
                }
            }
            if (managed_slot_refreshed) {
                g_connection_manager.UpdateConnection(managed.id, managed);
            }
            // Set status "connecting" so the dock reflects the pending state immediately.
            // This also allows StatsPollingLoop to advance it to "connected" once active.
            g_connection_manager.Connect(conn_id, jwt);
            EmitCurrentStatusSnapshotToDock("connection_connect", false);
            g_connection_manager.ConfigureManagedConnectionStart(
                managed.id,
                managed.managed_region,
                managed.stream_slot_number,
                managed.stream_token);
            // Async — StartManagedRelayAsync emits provisioning/completed/failed results.
            // Pass conn_id so it can reset status to "idle" on failure.
            g_connection_manager.StartManagedRelayAsync(
                jwt, request_id, g_config.relay_heartbeat_interval_sec, conn_id);
            return true;
        }
        if (it == conns.end()) {
            blog(LOG_WARNING,
                 "[aegis-obs-plugin] dock action failed: type=connection_connect"
                 " request_id=%s error=not_found",
                 request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "not_found", "");
            return false;
        }
        // BYOR: synchronous direct connect.
        const std::string jwt = CurrentControlPlaneJwtForActions();
        g_connection_manager.Connect(conn_id, jwt);
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        EmitCurrentStatusSnapshotToDock("connection_connect", false);
        return true;
    }

    if (action_type == "connection_disconnect") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action connection_disconnect: request_id=%s",
             request_id.c_str());
        std::string conn_id;
        (void)TryExtractJsonStringField(action_json, "id", &conn_id);
        if (conn_id.empty()) {
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_id", "");
            return false;
        }
        // Managed connections go through the async relay stop path; BYOR is synchronous.
        const auto conns = g_connection_manager.ListConnections();
        const auto it = std::find_if(conns.begin(), conns.end(),
                                     [&conn_id](const aegis::RelayConnectionConfig& c) {
                                         return c.id == conn_id;
                                     });
        if (it != conns.end() && it->type == "managed") {
            if (!g_connection_manager.HasActiveSession()) {
                blog(LOG_WARNING,
                     "[aegis-obs-plugin] dock action failed: type=connection_disconnect"
                     " request_id=%s error=no_active_session",
                     request_id.c_str());
                EmitDockActionResult(action_type, request_id, "failed", false,
                                     "no_active_session", "");
                return false;
            }
            const std::string jwt = CurrentControlPlaneJwtForActions();
            // Set status "idle" immediately so dock button reverts to "Connect".
            // The relay stops asynchronously in the background.
            g_connection_manager.Disconnect(conn_id, jwt);
            EmitCurrentStatusSnapshotToDock("connection_disconnect", false);
            // Async — StopManagedRelayAsync emits queued/completed/failed results.
            g_connection_manager.StopManagedRelayAsync(jwt, request_id);
            return true;
        }
        if (it == conns.end()) {
            blog(LOG_WARNING,
                 "[aegis-obs-plugin] dock action failed: type=connection_disconnect"
                 " request_id=%s error=not_found",
                 request_id.c_str());
            EmitDockActionResult(action_type, request_id, "failed", false, "not_found", "");
            return false;
        }
        // BYOR: synchronous direct disconnect.
        const std::string jwt = CurrentControlPlaneJwtForActions();
        g_connection_manager.Disconnect(conn_id, jwt);
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        EmitCurrentStatusSnapshotToDock("connection_disconnect", false);
        return true;
    }

    if (action_type == "rename_stream_slot") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action rename_stream_slot: request_id=%s",
             request_id.c_str());
        if (!g_auth) {
            EmitDockActionResult(action_type, request_id, "failed", false, "auth_not_configured", "");
            return false;
        }

        int slot_number = 0;
        std::string label;
        (void)TryExtractJsonIntField(action_json, "slot_number", &slot_number);
        (void)TryExtractJsonStringField(action_json, "label", &label);
        if (slot_number <= 0) {
            EmitDockActionResult(action_type, request_id, "rejected", false,
                                 "invalid_stream_slot", "");
            return false;
        }

        const std::string jwt = CurrentControlPlaneJwtForActions();
        if (jwt.empty()) {
            EmitDockActionResult(action_type, request_id, "failed", false,
                                 "no_control_plane_auth", "");
            return false;
        }

        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        TrackAuthWorker(std::async(std::launch::async, [req_id = request_id, jwt, slot_number, label]() {
            try {
                if (!g_auth->UpdateStreamSlotLabel(jwt, slot_number, label)) {
                    EmitDockActionResult("rename_stream_slot", req_id, "failed", false,
                                         "stream_slot_rename_failed", "");
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(g_auth_state_mu);
                    UpdateStreamSlotLabelInAuthStateLocked(slot_number, label);
                    PersistAuthStateLocked();
                }

                const auto conns = g_connection_manager.ListConnections();
                for (const auto& conn : conns) {
                    if (conn.stream_slot_number != slot_number) {
                        continue;
                    }
                    aegis::RelayConnectionConfig updated = conn;
                    updated.stream_slot_label = label;
                    g_connection_manager.UpdateConnection(updated.id, updated);
                }

                EmitCurrentStatusSnapshotToDock("rename_stream_slot", false);
                EmitDockActionResult("rename_stream_slot", req_id, "completed", true, "",
                                     BuildAuthStateDetailJson());
            } catch (const std::exception& e) {
                EmitDockActionResult("rename_stream_slot", req_id, "failed", false, e.what(), "");
            }
        }));
        return true;
    }

    if (action_type == "connection_update") {
        blog(LOG_INFO,
             "[aegis-obs-plugin] dock action connection_update: request_id=%s",
             request_id.c_str());
        std::string conn_id, name, relay_host, stream_id, managed_region;
        int relay_port = 5000;
        int stream_slot_number = -1;
        (void)TryExtractJsonStringField(action_json, "id", &conn_id);
        (void)TryExtractJsonStringField(action_json, "name", &name);
        (void)TryExtractJsonStringField(action_json, "relay_host", &relay_host);
        (void)TryExtractJsonStringField(action_json, "stream_id", &stream_id);
        (void)TryExtractJsonStringField(action_json, "managed_region", &managed_region);
        (void)TryExtractJsonIntField(action_json, "relay_port", &relay_port);
        (void)TryExtractJsonIntField(action_json, "stream_slot", &stream_slot_number);
        if (conn_id.empty()) {
            EmitDockActionResult(action_type, request_id, "rejected", false, "missing_id", "");
            return false;
        }
        // Resolve new slot (if provided) from auth state.
        std::optional<aegis::StreamSlot> new_slot;
        if (stream_slot_number >= 0) {
            std::lock_guard<std::mutex> lock(g_auth_state_mu);
            new_slot = FindStreamSlotLocked(stream_slot_number);
            if (!new_slot) {
                EmitDockActionResult(action_type, request_id, "rejected", false,
                                     "invalid_stream_slot", "");
                return false;
            }
        }
        // Find and update existing connection.
        auto conns = g_connection_manager.ListConnections();
        for (auto& c : conns) {
            if (c.id == conn_id) {
                if (!name.empty())           c.name = name;
                if (!relay_host.empty())     c.relay_host = relay_host;
                if (relay_port > 0)          c.relay_port = relay_port;
                if (!stream_id.empty())      c.stream_id = stream_id;
                if (!managed_region.empty()) c.managed_region = managed_region;
                if (new_slot) {
                    c.stream_slot_number = new_slot->slot_number;
                    c.stream_slot_label  = new_slot->label;
                    c.stream_token       = new_slot->stream_token;
                }
                g_connection_manager.UpdateConnection(conn_id, c);
                break;
            }
        }
        EmitDockActionResult(action_type, request_id, "completed", true, "", "");
        EmitCurrentStatusSnapshotToDock("connection_update", false);
        return true;
    }

    blog(
        LOG_INFO,
        "[aegis-obs-plugin] dock action rejected: type=%s request_id=%s error=unsupported_action_type",
        action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str());
    EmitDockActionResult(action_type, request_id, "rejected", false, "unsupported_action_type", "");
    return false;
}

#endif // AEGIS_OBS_PLUGIN_BUILD
