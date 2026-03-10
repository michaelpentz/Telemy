#include "dock_action_dispatch.h"
#include "dock_replay_cache.h"
#include "config_vault.h"
#include "relay_client.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <algorithm>
#include <chrono>
#include <fstream>
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
extern aegis::Vault       g_vault;
extern aegis::PluginConfig g_config;
extern std::unique_ptr<aegis::RelayClient> g_relay;
extern std::mutex g_relay_worker_mu;
extern std::thread g_relay_start_worker;
extern std::thread g_relay_stop_worker;

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
static std::uint64_t g_local_dock_action_seq = 0;
static std::mutex g_recent_dock_actions_mu;
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_recent_dock_actions;

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
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        std::string jwt = *jwt_opt;
        std::string req_id = request_id;
        int heartbeat_interval = g_config.relay_heartbeat_interval_sec;
        {
            std::lock_guard<std::mutex> lk(g_relay_worker_mu);
            if (g_relay_start_worker.joinable()) {
                g_relay_start_worker.join();
            }
            g_relay_start_worker = std::thread([jwt, req_id, heartbeat_interval]() {
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
            });
        }
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
        EmitDockActionResult(action_type, request_id, "queued", true, "", "queued_native");
        std::string req_id = request_id;
        {
            std::lock_guard<std::mutex> lk(g_relay_worker_mu);
            if (g_relay_stop_worker.joinable()) {
                g_relay_stop_worker.join();
            }
            g_relay_stop_worker = std::thread([jwt, sid, req_id]() {
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
            });
        }
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
        if (g_relay) {
            const std::string new_host = g_config.relay_api_host;
            const auto new_key = g_vault.Get("relay_shared_key");
            g_relay->Reconfigure(new_host, new_key.value_or(""));
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

    blog(
        LOG_INFO,
        "[aegis-obs-plugin] dock action rejected: type=%s request_id=%s error=unsupported_action_type",
        action_type.c_str(),
        request_id.empty() ? "" : request_id.c_str());
    EmitDockActionResult(action_type, request_id, "rejected", false, "unsupported_action_type", "");
    return false;
}

#endif // AEGIS_OBS_PLUGIN_BUILD
