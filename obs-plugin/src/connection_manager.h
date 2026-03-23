#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "relay_client.h"

namespace aegis {

struct RelaySession;
struct RelayStats;
struct PerLinkSnapshot;
class  RelayClient;
class  Vault;
class  HttpsClient;

// ---------------------------------------------------------------------------
// Per-link carrier aggregation — used by metrics JSON output
// ---------------------------------------------------------------------------
struct ConnectionInfo {
    std::string id;
    std::string name;
    std::string type;
    int signal = 0;
    uint32_t bitrate_kbps = 0;
    std::string status;
    std::string addr;
    std::string asn_org;
    double share_pct = 0.0;
    uint32_t last_ms_ago = 0;
    uint32_t uptime_s = 0;
};

struct ConnectionSnapshot {
    bool available = false;
    std::vector<ConnectionInfo> items;
};

// ---------------------------------------------------------------------------
// Multi-connection config (Phase 2)
// ---------------------------------------------------------------------------
struct RelayConnectionConfig {
    std::string id;               // UUID
    std::string name;             // User display name
    std::string type;             // "byor" or "managed"
    // BYOR fields (sensitive — stored in vault, keyed by id):
    std::string relay_host;
    int         relay_port = 5000;
    std::string stream_id;
    // Managed fields:
    std::string managed_region;
    int         stream_slot_number = 0;
    std::string stream_slot_label;
    std::string stream_token;
    std::string session_id;
    // Runtime state (not persisted):
    std::string status;           // "idle", "connecting", "connected", "error"
    std::string error_msg;
};

// ---------------------------------------------------------------------------
// Per-connection async worker (start/stop threads + cancellation flag).
// Uses unique_ptr storage because std::atomic<bool> is not movable.
// ---------------------------------------------------------------------------
struct ConnectionWorker {
    std::thread       start_worker;
    std::thread       stop_worker;
    std::atomic<bool> start_cancel{false};
};

// ---------------------------------------------------------------------------
// Pending managed start config — stored before StartManagedRelayAsync is called
// ---------------------------------------------------------------------------
struct PendingManagedStart {
    std::string connection_id;
    std::string region_preference;
    int stream_slot_number = 0;
    std::string stream_token;
};

// ---------------------------------------------------------------------------
// ConnectionManager — primary connection owner
//
// Owns all RelayClient instances. Runs background stats polling so that
// CurrentSnapshot()/CurrentStats()/CurrentPerLinkStats() never perform
// network I/O on the OBS tick thread. Thread-safe.
// ---------------------------------------------------------------------------
class ConnectionManager {
public:
    ConnectionManager() = default;
    ~ConnectionManager();

    // Non-copyable, non-movable (owns threads).
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    // Called from obs_module_load.
    void Initialize(Vault* vault, HttpsClient* http,
                    const std::string& api_host,
                    const std::string& relay_shared_key,
                    int heartbeat_interval_sec = 30);

    // Called from obs_module_unload. Blocks until all threads join.
    void Shutdown();

    // Hot-reconfigure api_host + relay_shared_key (called from save_config).
    void Reconfigure(const std::string& api_host, const std::string& relay_shared_key);

    // ── Multi-connection CRUD (Phase 2) ──────────────────────────────────
    std::string AddConnection(const RelayConnectionConfig& config); // returns new UUID
    void UpdateConnection(const std::string& id, const RelayConnectionConfig& config);
    void RemoveConnection(const std::string& id);
    std::vector<RelayConnectionConfig> ListConnections() const;

    // ── Multi-connection lifecycle ────────────────────────────────────────
    void Connect(const std::string& id, const std::string& jwt = "");
    void Disconnect(const std::string& id, const std::string& jwt = "");

    // ── Legacy single-relay operations (backward compat with v0.0.4 dock) ──

    // Starts managed relay asynchronously. Emits dock results via
    // EmitDockActionResult during provisioning and on completion/failure.
    // connection_id: if non-empty, resets that connection's status to "idle" on failure.
    void StartManagedRelayAsync(const std::string& jwt,
                                const std::string& request_id,
                                int heartbeat_interval_sec,
                                const std::string& connection_id = "");

    // Directly set a connection's runtime status (e.g. to reset to "idle" on failure).
    void SetConnectionStatus(const std::string& id, const std::string& status);

    // Store pending managed start config — called before StartManagedRelayAsync.
    // Stores in pending_starts_ map instead of directly on a single RelayClient.
    void ConfigureManagedConnectionStart(const std::string& connection_id,
                                         const std::string& region_preference,
                                         int stream_slot_number,
                                         const std::string& stream_token)
    {
        std::lock_guard<std::mutex> lock(pending_start_mu_);
        pending_starts_[connection_id] = {connection_id, region_preference,
                                          stream_slot_number, stream_token};
    }

    // Stops managed relay asynchronously. Gets current session internally.
    // connection_id identifies which managed relay to stop.
    void StopManagedRelayAsync(const std::string& jwt,
                               const std::string& request_id,
                               const std::string& connection_id = "");

    // BYOR direct connect/disconnect (synchronous).
    void ConnectDirect(const std::string& host, int port, const std::string& stream_id);
    void DisconnectDirect();

    // Cancel any in-flight relay_start worker(s).
    void CancelPendingStart();

    // Emergency stop on plugin unload (blocks up to 3 seconds).
    void EmergencyStop(const std::string& jwt);

    // ── Status access — NO network I/O, reads cached values only ─────────
    ConnectionSnapshot          CurrentSnapshot() const;
    bool                        HasActiveSession() const;
    bool                        IsBYORMode() const;
    std::optional<RelaySession> CurrentSession() const;
    std::optional<RelaySession> CurrentSessionForConnection(const std::string& id) const;
    RelayStats                  CurrentStats() const;
    PerLinkSnapshot             CurrentPerLinkStats() const;

private:
    // BYOR relay client (shared_ptr prevents use-after-free on Disconnect).
    std::shared_ptr<RelayClient> byor_relay_;
    mutable std::mutex           relay_mu_;

    // Background stats polling thread.
    std::thread             stats_thread_;
    std::atomic<bool>       stats_stop_{false};
    std::mutex              stats_cv_mu_;
    std::condition_variable stats_cv_;
    void StatsPollingLoop();

    // Per-connection worker threads (run async relay lifecycle ops).
    std::mutex worker_mu_;
    std::unordered_map<std::string, std::unique_ptr<ConnectionWorker>> workers_;

    // Pending managed start configs — stored before StartManagedRelayAsync.
    std::mutex pending_start_mu_;
    std::unordered_map<std::string, PendingManagedStart> pending_starts_;

    // Cached stats — written by stats thread, read by OBS tick thread.
    mutable std::mutex snapshot_mu_;
    ConnectionSnapshot snapshot_;
    RelayStats         cached_stats_;
    PerLinkSnapshot    cached_per_link_;

    // Multi-connection list (Phase 2).
    mutable std::mutex                                              connections_mu_;
    std::vector<RelayConnectionConfig>                             connections_;
    std::unordered_map<std::string, std::shared_ptr<RelayClient>> active_clients_;

    // Dependencies (set by Initialize).
    Vault*       vault_                = nullptr;
    HttpsClient* http_                 = nullptr;
    std::string  api_host_;
    std::string  relay_shared_key_;
    int          heartbeat_interval_sec_ = 30;
    std::atomic<bool> initialized_{false};

    // Carrier classification helpers (moved from old ConnectionManager).
    static ConnectionSnapshot BuildSnapshot(const PerLinkSnapshot& per_link,
                                            const RelayStats* stats);

    // Connection persistence — serialize/deserialize connections_ to/from disk.
    // Non-sensitive fields go to config.json "connections" array.
    // Sensitive BYOR fields go to vault.json keyed by "conn_<id>".
    // Neither method holds connections_mu_ — callers must release it first.
    void SaveConnections();
    void LoadConnections();
};

}  // namespace aegis
