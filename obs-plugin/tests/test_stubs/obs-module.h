#pragma once

// Minimal OBS SDK stub for unit tests.
// Provides just enough of the obs-module.h interface so that plugin source
// files (config_vault.cpp, relay_client.cpp, etc.) compile without the
// full OBS Studio SDK.

#include <cstdio>
#include <cstdarg>

// Log levels matching OBS's enum values.
#define LOG_ERROR   100
#define LOG_WARNING 200
#define LOG_INFO    300
#define LOG_DEBUG   400

// blog() — the OBS logging macro.  In tests, silently discard all log output.
inline void blog(int /*log_level*/, const char* /*format*/, ...) {
    // No-op in test builds.
}
