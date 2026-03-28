#pragma once

#if defined(TELEMY_OBS_PLUGIN_BUILD) && defined(TELEMY_ENABLE_OBS_BROWSER_DOCK_HOST)

#include "dock_js_bridge_api.h"

// Compile-gated scaffold for future OBS browser dock (Qt/CEF) embedding.
// Current implementation is a no-op that preserves the JS executor ABI seam.

void telemy_obs_browser_dock_host_scaffold_initialize();
void telemy_obs_browser_dock_host_scaffold_shutdown();

// Helpers for the future dock page lifecycle wiring.
void telemy_obs_browser_dock_host_scaffold_set_js_executor(
    telemy_dock_js_execute_fn fn,
    void* user_data);
void telemy_obs_browser_dock_host_scaffold_on_page_ready();
void telemy_obs_browser_dock_host_scaffold_on_page_unloaded();
bool telemy_obs_browser_dock_host_scaffold_show_dock();

#endif
