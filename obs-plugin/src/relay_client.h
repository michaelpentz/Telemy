#pragma once
#include "https_client.h"
#include <string>
#include <optional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace aegis {

struct RelaySession {
    std::string session_id;
    std::string status;    // "provisioning", "active", "grace", "stopped"
    std::string region;
};

class RelayClient {
public:
    // api_host is just the hostname, e.g. "api.aegis.example.com"
    RelayClient(HttpsClient& http,
                const std::string& api_host,
                const std::string& relay_shared_key = "");
    ~RelayClient();

    // Non-copyable, non-movable — owns a background thread.
    RelayClient(const RelayClient&) = delete;
    RelayClient& operator=(const RelayClient&) = delete;

    // Control plane calls — MUST be called from worker thread, NOT OBS UI thread
    // Returns session info on success, nullopt on failure
    std::optional<RelaySession> GetActive(const std::string& jwt);
    std::optional<RelaySession> Start(const std::string& jwt);
    bool Stop(const std::string& jwt, const std::string& session_id);
    bool SendHeartbeat(const std::string& jwt, const std::string& session_id);

    // Heartbeat loop management
    void StartHeartbeatLoop(const std::string& jwt, const std::string& session_id,
                            int interval_sec = 30);
    void StopHeartbeatLoop();

    // Called from obs_module_unload — blocks up to 3 seconds
    void EmergencyRelayStop(const std::string& jwt);

    // Current state (thread-safe read)
    std::optional<RelaySession> CurrentSession() const;
    bool HasActiveSession() const;

private:
    HttpsClient& http_;
    std::wstring api_host_w_;  // wide string for WinHTTP
    std::wstring relay_shared_key_w_;
    std::atomic<bool> logged_missing_health_shared_key_{false};

    mutable std::mutex session_mutex_;
    std::optional<RelaySession> current_session_;

    // Heartbeat thread
    std::atomic<bool> heartbeat_running_{false};
    std::thread heartbeat_thread_;
    std::mutex heartbeat_cv_mutex_;
    std::condition_variable heartbeat_cv_;

    // UUID v4 generation
    static std::string GenerateUuidV4();

    // JSON helpers
    static std::optional<RelaySession> ParseSessionResponse(const std::string& json);
};

}  // namespace aegis
