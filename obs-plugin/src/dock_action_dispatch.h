#pragma once

// dock_action_dispatch.h — Dock UI action dispatch and handler logic.
// Extracted from obs_plugin_entry.cpp (RF-028).
//
// Contains the action handler dispatch table invoked by
// aegis_obs_shim_receive_dock_action_json, plus supporting utilities
// (dedup, pending action tracking, config-setting helpers).

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#include <QJsonObject>

// ---------------------------------------------------------------------------
// Pending action structs
// ---------------------------------------------------------------------------
struct PendingSwitchRequest {
    std::string request_id;
    std::string scene_name;
    std::string reason;
};

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

// ---------------------------------------------------------------------------
// Action deduplication
// ---------------------------------------------------------------------------
bool ShouldDeduplicateDockActionByRequestId(
    const std::string& action_type,
    const std::string& request_id);

// ---------------------------------------------------------------------------
// Pending action tracking — request_status
// ---------------------------------------------------------------------------
void TrackPendingDockRequestStatusAction(const std::string& request_id);
std::string ConsumePendingDockRequestStatusActionId();

// ---------------------------------------------------------------------------
// Pending action tracking — set_mode / set_setting
// ---------------------------------------------------------------------------
void TrackPendingDockSetModeAction(const std::string& request_id, const std::string& mode);
void TrackPendingDockSetSettingAction(
    const std::string& request_id,
    const std::string& key,
    bool value);

// ---------------------------------------------------------------------------
// Status snapshot projection (for resolving pending actions)
// ---------------------------------------------------------------------------
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

bool TryProjectStatusSnapshot(const std::string& envelope_json, StatusSnapshotProjection* out);
bool TryGetStatusSnapshotSettingBool(
    const StatusSnapshotProjection& snap,
    const std::string& key,
    bool* out_value);
void ResolvePendingDockActionCompletionsFromStatusSnapshot(const std::string& envelope_json);
void DrainExpiredPendingDockActions();

// ---------------------------------------------------------------------------
// Config/settings helpers used by action handlers
// ---------------------------------------------------------------------------
bool IsRecognizedDockMode(const std::string& mode);
bool IsRecognizedDockSettingKey(const std::string& key);
std::string EffectiveDockModeFromConfig();
bool SetDockSettingValueByKey(const std::string& key, bool value);
QJsonObject BuildDockSettingsJsonFromConfig();

// ---------------------------------------------------------------------------
// Scene switch request queue
// ---------------------------------------------------------------------------
void EnqueueSwitchSceneRequest(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason);
void HandleSwitchSceneRequestOnObsThread(
    const std::string& request_id,
    const std::string& scene_name,
    const std::string& reason);
void DrainSwitchSceneRequestsOnObsThread();

// ---------------------------------------------------------------------------
// Relay session detail JSON builder
// ---------------------------------------------------------------------------
struct ProvisionStepInfo {
    int number = 0;
    const char* label = "";
};
ProvisionStepInfo stepToInfo(const std::string& step);

// ---------------------------------------------------------------------------
// Dispatch the body of a dock action JSON payload.
// Called from aegis_obs_shim_receive_dock_action_json in obs_plugin_entry.cpp.
// Returns true if the action was accepted for handling/queueing.
// ---------------------------------------------------------------------------
bool DispatchDockAction(const std::string& action_json,
                        const std::string& action_type,
                        const std::string& request_id);

// ---------------------------------------------------------------------------
// Pending action accessors (for cross-module use by dock_replay_cache)
// ---------------------------------------------------------------------------
std::mutex& GetPendingRelayActionsMutex();
std::vector<PendingRelayAction>& GetPendingRelayActionsRef();

// ---------------------------------------------------------------------------
// Clear all pending action queues (called from obs_module_unload).
// ---------------------------------------------------------------------------
void ClearAllPendingDockActions();

// ---------------------------------------------------------------------------
// Drain in-flight auth worker futures (called from obs_module_unload before
// g_auth.reset() to prevent use-after-free on plugin unload).
// ---------------------------------------------------------------------------
void DrainAuthWorkers();

#endif // AEGIS_OBS_PLUGIN_BUILD
