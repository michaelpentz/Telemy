#pragma once

// dock_replay_cache.h — Dock replay cache and JS delivery infrastructure.
// Extracted from obs_plugin_entry.cpp (RF-028).
//
// Manages cached dock state (status snapshot, scene snapshot, pipe status, etc.)
// so that late-connecting or refreshed dock pages can receive the latest state.
// Also houses the JS delivery validation/fallback logging and the
// EmitDockActionResult / EmitDockSceneSwitchCompleted helpers.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#if defined(AEGIS_OBS_PLUGIN_BUILD)

// ---------------------------------------------------------------------------
// Dock replay cache — last-known state replayed to newly connected dock pages.
// ---------------------------------------------------------------------------
struct DockReplayCache {
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

// ---------------------------------------------------------------------------
// JS delivery validation state — tracks whether the dock page is ready and
// whether each delivery channel has been validated at least once.
// ---------------------------------------------------------------------------
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

struct DockJsSinkProbeState {
    bool js_sink_registered = false;
    bool page_ready = false;
};

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------
using DockSceneSnapshotEmitterFn = std::function<void(const std::string&)>;
using DockBrowserJsExecuteFn = std::function<bool(const std::string&)>;

// ---------------------------------------------------------------------------
// Fallback log kind — controls throttled logging for undelivered payloads.
// ---------------------------------------------------------------------------
enum class DockFallbackLogKind {
    PipeStatus,
    StatusSnapshotJson,
    SceneSnapshotJson,
    SceneSwitchCompletedJson,
    DockActionResultJson,
};

// ---------------------------------------------------------------------------
// JS sink probe
// ---------------------------------------------------------------------------
DockJsSinkProbeState GetDockJsSinkProbeState();

// ---------------------------------------------------------------------------
// JS execution sink management
// ---------------------------------------------------------------------------
void SetDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn);
bool TryExecuteDockBrowserJs(const std::string& js_code);
void RegisterDockBrowserJsExecuteSink(DockBrowserJsExecuteFn execute_fn);

// ---------------------------------------------------------------------------
// Scene snapshot emitter management
// ---------------------------------------------------------------------------
void SetDockSceneSnapshotEmitter(DockSceneSnapshotEmitterFn emitter);

// ---------------------------------------------------------------------------
// Fallback logging
// ---------------------------------------------------------------------------
bool ShouldLogDockFallbackPayload(
    DockFallbackLogKind kind,
    const char** out_phase,
    std::uint32_t* out_attempt);

// ---------------------------------------------------------------------------
// Dock JS emission helpers
// ---------------------------------------------------------------------------
bool EmitDockNativeJsonArgCall(const char* method_name, const std::string& payload_json);
bool EmitDockNativePipeStatus(const char* status, const char* reason);
bool EmitDockNativeCurrentScene(const std::string& scene_name);

// ---------------------------------------------------------------------------
// Cache write functions
// ---------------------------------------------------------------------------
void CacheDockSceneSnapshotForReplay(const std::string& payload_json);
void CacheDockPipeStatusForReplay(const char* status, const char* reason);
void CacheDockCurrentSceneForReplay(const std::string& scene_name);
void CacheDockSceneSwitchCompletedForReplay(const std::string& payload_json);
void CacheDockActionResultForReplay(const std::string& payload_json);

// ---------------------------------------------------------------------------
// Clear / replay
// ---------------------------------------------------------------------------
void ClearDockReplayCache();
void ReplayDockStateToJsSinkIfAvailable();

// ---------------------------------------------------------------------------
// Re-emit the cached status snapshot with fresh theme to the dock JS.
// ---------------------------------------------------------------------------
void ReemitDockStatusSnapshotWithCurrentTheme(const char* reason);

// ---------------------------------------------------------------------------
// High-level dock emission helpers
// ---------------------------------------------------------------------------
void EmitDockActionResult(const std::string& action_type,
                          const std::string& request_id,
                          const std::string& status,
                          bool ok,
                          const std::string& error,
                          const std::string& detail);

void EmitDockSceneSwitchCompleted(const std::string& request_id,
                                  const std::string& scene_name,
                                  bool ok,
                                  const std::string& error,
                                  const std::string& reason);

bool EmitDockSceneSnapshotPayload(const std::string& payload_json);

// ---------------------------------------------------------------------------
// JSON building helpers
// ---------------------------------------------------------------------------
std::string BuildSceneSwitchCompletedJson(const std::string& request_id,
                                          const std::string& scene_name,
                                          bool ok,
                                          const std::string& error,
                                          const std::string& reason);

std::string BuildDockActionResultJson(const std::string& action_type,
                                      const std::string& request_id,
                                      const std::string& status,
                                      bool ok,
                                      const std::string& error,
                                      const std::string& detail);

// ---------------------------------------------------------------------------
// Relay action result resolution
// ---------------------------------------------------------------------------
void HandleRelayActionResultIfPresent(const std::string& envelope_json);

// ---------------------------------------------------------------------------
// Access to replay cache mutex + data for status snapshot updates.
// These are exposed so obs_plugin_entry.cpp can update the status snapshot
// in the cache after building it.
// ---------------------------------------------------------------------------
std::mutex& GetDockReplayCacheMutex();
DockReplayCache& GetDockReplayCacheRef();

// Access to JS delivery validation mutex + data for page-ready notifications.
std::mutex& GetDockJsDeliveryValidationMutex();
DockJsDeliveryValidationState& GetDockJsDeliveryValidationRef();

#endif // AEGIS_OBS_PLUGIN_BUILD
