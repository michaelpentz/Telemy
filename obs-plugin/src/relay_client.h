#pragma once
#include "https_client.h"
#include <string>
#include <optional>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace aegis {

struct RelaySession {
    std::string session_id;
    std::string status;    // "provisioning", "active", "grace", "stopped"
    std::string region;
    std::string public_ip;
    int         srt_port = 5000;
    std::string relay_hostname;  // e.g. "k7mx2p.telemyapp.com"
    std::string pair_token;
    std::string stream_token;
    std::string instance_id;   // AWS instance ID — needed for /relay/health
    int         grace_window_seconds = 0;
    int         max_session_seconds = 0;
    std::string provision_step;
};

struct RelayStats {
    bool     available = false;
    uint32_t bitrate_kbps = 0;
    double   rtt_ms = 0.0;
    uint64_t pkt_loss = 0;
    uint64_t pkt_drop = 0;
    double   recv_rate_mbps = 0.0;
    double   bandwidth_mbps = 0.0;
    uint32_t latency_ms = 0;
    uint32_t uptime_seconds = 0;
};

struct PerLinkStats {
    std::string addr;
    std::string asn_org;
    uint64_t bytes = 0;
    uint64_t pkts = 0;
    double share_pct = 0.0;
    uint32_t last_ms_ago = 0;
    uint32_t uptime_s = 0;
};

struct PerLinkSnapshot {
    bool available = false;
    int conn_count = 0;
    std::vector<PerLinkStats> links;
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

    // Hot-reconfigure api_host and relay_shared_key without restarting OBS.
    void Reconfigure(const std::string& api_host, const std::string& relay_shared_key);

    // Called from obs_module_unload — blocks up to 3 seconds
    void EmergencyRelayStop(const std::string& jwt);

    // Current state (thread-safe read)
    std::optional<RelaySession> CurrentSession() const;
    bool HasActiveSession() const;

    // SLS stats polling
    void PollRelayStats(const std::string& relay_ip);
    RelayStats CurrentStats() const;

    // Per-link stats polling (srtla_rec fork)
    void PollPerLinkStats(const std::string& relay_ip);
    PerLinkSnapshot CurrentPerLinkStats() const;

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
    int heartbeat_consecutive_failures_{0};
    int heartbeat_current_interval_sec_{30};
    static constexpr int kHeartbeatBackoffThreshold = 3;
    static constexpr int kHeartbeatMaxIntervalSec = 120;

    RelayStats         stats_;
    mutable std::mutex stats_mutex_;

    PerLinkSnapshot    per_link_;
    mutable std::mutex per_link_mutex_;

    // UUID v4 generation
    static std::string GenerateUuidV4();

    // JSON helpers
    static std::optional<RelaySession> ParseSessionResponse(const std::string& json);
};

}  // namespace aegis
