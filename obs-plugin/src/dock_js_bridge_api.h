#pragma once

// Stable C ABI hooks for wiring a browser-dock JS executor into the OBS plugin shim.
// Intended for a future Qt/CEF integration layer that can execute JS in the dock page.

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*telemy_dock_js_execute_fn)(const char* js_utf8, void* user_data);

// Registers or replaces the JS execution callback used by the shim to call
// window.telemyDockNative.* in the dock page. Passing null clears the executor.
void telemy_obs_shim_register_dock_js_executor(
    telemy_dock_js_execute_fn fn,
    void* user_data);

// Clears the registered JS executor callback.
void telemy_obs_shim_clear_dock_js_executor(void);

// Replays the shim's cached dock state (IPC/status/scene snapshot/current scene)
// through the registered executor. Safe to call after the dock page bootstrap is ready.
void telemy_obs_shim_replay_dock_state(void);

// Semantic alias for page/bootstrap readiness. Future dock integrations should call this
// after window.telemyDockNative and bridge-host bootstrap are available.
void telemy_obs_shim_notify_dock_page_ready(void);

// Semantic alias for dock page unload/teardown. Future integrations should call this
// before destroying the page/widget or when navigation invalidates the JS context.
void telemy_obs_shim_notify_dock_page_unloaded(void);

// Receives a dock UI action payload encoded as JSON object text (e.g. {"type":"switch_scene",...}).
// Returns true if the action was accepted for handling/queueing; false if rejected/invalid/unsupported.
bool telemy_obs_shim_receive_dock_action_json(const char* action_json_utf8);

#ifdef __cplusplus
}
#endif
