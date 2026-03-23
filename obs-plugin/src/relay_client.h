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
    std::string instance_id;   // Relay instance ID — identifies relay node for health tracking
    int         grace_window_seconds = 0;
    int         max_session_seconds = 0;
    std::string provision_step;
    std::string sender_url;
    std::string media_source_url;
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
    std::string stream_id;  // The stream_id this per-link data belongs to (e.g. "live_<token>")
    std::vector<PerLinkStats> links;
};

struct AuthTokens {
    std::string cp_access_jwt;
    std::string refresh_token;

    bool Empty() const;
    void Clear();
    std::string ToVaultJson() const;
    static std::optional<AuthTokens> FromVaultJson(const std::string& json);
};

struct AuthUser {
    std::string id;
    std::string email;
    std::string display_name;
};

struct RelayEntitlement {
    std::string relay_access_status;
    std::string reason_code;
    std::string plan_tier;
    std::string plan_status;
    int max_concurrent_conns = 0;
    int active_managed_conns = 0;

    bool RelayEnabled() const { return relay_access_status == "enabled"; }
};

struct UsageSnapshot {
    int included_seconds = 0;
    int consumed_seconds = 0;
    int remaining_seconds = 0;
    int overage_seconds = 0;
};

struct ActiveRelaySummary {
    std::string session_id;
    std::string status;
};

struct StreamSlot {
    int slot_number = 0;
    std::string label;
    std::string stream_token;
};

struct BYORConfig {
    std::string relay_host;
    int         relay_port = 5000;
    std::string stream_id;
};

struct AuthSessionSnapshot {
    AuthUser user;
    RelayEntitlement entitlement;
    UsageSnapshot usage;
    std::optional<ActiveRelaySummary> active_relay;
    std::vector<StreamSlot> stream_slots;

    std::string ToVaultJson() const;
    static std::optional<AuthSessionSnapshot> FromVaultJson(const std::string& json);
};

enum class AuthPollStatus {
    Pending,
    Completed,
    Denied,
    Expired,
    Failed,
};

struct PluginLoginAttempt {
    std::string login_attempt_id;
    std::string authorize_url;
    std::string poll_token;
    std::string expires_at;
    int poll_interval_seconds = 3;

    bool Empty() const;
    void Clear();
};

struct PluginAuthState {
    AuthTokens tokens;
    AuthSessionSnapshot session;
    PluginLoginAttempt login_attempt;
    bool authenticated = false;
    std::string last_error_code;
    std::string last_error_message;

    void ClearAuthMaterial();
    void ClearLoginAttempt();
    std::string ToVaultJson() const;
    static std::optional<PluginAuthState> FromVaultJson(const std::string& json);
};

struct AuthPollResult {
    AuthPollStatus status = AuthPollStatus::Failed;
    std::string reason_code;
    AuthTokens tokens;
    std::optional<AuthSessionSnapshot> session;
};

class ControlPlaneAuthClient {
public:
    ControlPlaneAuthClient(HttpsClient& http, const std::string& api_host);

    void Reconfigure(const std::string& api_host);

    std::optional<AuthSessionSnapshot> GetSession(const std::string& cp_access_jwt);
    std::optional<PluginLoginAttempt> StartPluginLogin(const std::string& device_name,
                                                       const std::string& plugin_version = "0.0.5",
                                                       const std::string& platform = "windows");
    std::optional<AuthPollResult> PollPluginLogin(const std::string& login_attempt_id,
                                                  const std::string& poll_token);
    std::optional<AuthPollResult> Refresh(const std::string& refresh_token);
    bool Logout(const std::string& cp_access_jwt);
    bool UpdateStreamSlotLabel(const std::string& cp_access_jwt,
                               int slot_number,
                               const std::string& label);

private:
    HttpsClient& http_;
    std::wstring api_host_w_;

    static std::vector<std::pair<std::wstring, std::wstring>> CommonClientHeaders();
    static std::optional<AuthSessionSnapshot> ParseAuthSessionSnapshot(const std::string& json);
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
    std::optional<RelaySession> GetActive(const std::string& jwt,
                                          const std::string& expected_session_id = "",
                                          const std::string& expected_stream_token = "");
    std::optional<RelaySession> Start(const std::string& jwt);
    bool Stop(const std::string& jwt, const std::string& session_id);
    bool SendHeartbeat(const std::string& jwt, const std::string& session_id);

    // BYOR direct relay connection path (no control-plane session).
    void ConnectDirect(const std::string& relay_host, int relay_port, const std::string& stream_token);
    void ConnectDirect(const BYORConfig& config, const std::string& stream_token);
    void DisconnectDirect();
    bool IsBYORMode() const;
    void ConfigureNextManagedStart(const std::string& connection_id,
                                   const std::string& region_preference,
                                   int stream_slot_number,
                                   const std::string& stream_token);

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
    // When filter_stream_id is non-empty, only include the group whose stream_id matches.
    // When empty (backward compat / BYOR), include all groups as before.
    void PollPerLinkStats(const std::string& relay_ip, const std::string& filter_stream_id = "");
    PerLinkSnapshot CurrentPerLinkStats() const;

    // JWT storage for managed connections — allows stats polling loop to call
    // GetActive() without needing the JWT from the start/heartbeat path.
    void StoreJWT(const std::string& jwt);
    std::string GetStoredJWT() const;

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

    std::string        stored_jwt_;
    mutable std::mutex jwt_mutex_;

    std::string        connection_id_;  // persistent, set by ConfigureNextManagedStart
    mutable std::mutex connection_id_mutex_;

    std::atomic<bool> byor_mode_{false};
    mutable std::mutex pending_managed_start_mutex_;
    std::string pending_managed_connection_id_;
    std::string pending_managed_region_preference_;
    int pending_managed_stream_slot_number_ = 0;
    std::string pending_managed_stream_token_;

    // UUID v4 generation
    static std::string GenerateUuidV4();

    // JSON helpers
    static std::optional<RelaySession> ParseSessionResponse(const std::string& json);
    void ClearStatsSnapshots();
};

}  // namespace aegis

