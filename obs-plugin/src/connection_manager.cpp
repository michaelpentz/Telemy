#include "connection_manager.h"
#include "config_vault.h"
#include "relay_client.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#include "dock_replay_cache.h"
#include <obs-module.h>
#endif

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <utility>

// Forward declaration — defined in obs_plugin_entry.cpp
std::string BuildRelaySessionDetailJson(const aegis::RelaySession& session);

namespace aegis {

namespace {

// ---------------------------------------------------------------------------
// Carrier classification helpers (originally in old ConnectionManager)
// ---------------------------------------------------------------------------

std::string ToLower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool ContainsLabel(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

std::pair<std::string, std::string> ClassifyAsnOrg(const std::string& asn_org, std::size_t index)
{
    if (asn_org.empty()) {
        return {"Link " + std::to_string(index + 1), "Unknown"};
    }

    const std::string lower = ToLower(asn_org);
    struct KnownLabel {
        const char* needle;
        const char* label;
        const char* type;
    };
    static const KnownLabel known_labels[] = {
        {"google fiber", "Google Fiber", "Ethernet"},
        {"verizon fios", "Fios", "Ethernet"},
        {"at&t internet", "AT&T Fiber", "Ethernet"},
        {"comcast", "Comcast", "Ethernet"},
        {"charter", "Charter", "Ethernet"},
        {"spectrum", "Spectrum", "Ethernet"},
        {"cox", "Cox", "Ethernet"},
        {"centurylink", "CenturyLink", "Ethernet"},
        {"frontier", "Frontier", "Ethernet"},
        {"lumen", "Lumen", "Ethernet"},
        {"t-mobile", "T-Mobile", "Cellular"},
        {"at&t", "AT&T", "Cellular"},
        {"verizon", "Verizon", "Cellular"},
        {"sprint", "Sprint", "Cellular"},
        {"vodafone", "Vodafone", "Cellular"},
        {"ee limited", "EE", "Cellular"},
        {"softbank", "SoftBank", "Cellular"},
        {"ntt docomo", "Docomo", "Cellular"},
        {"rogers", "Rogers", "Cellular"},
        {"telus", "TELUS", "Cellular"},
    };

    for (const auto& known : known_labels) {
        if (ContainsLabel(lower, known.needle)) {
            return {known.label, known.type};
        }
    }

    const std::size_t split = asn_org.find_first_of(", ");
    std::string label = split == std::string::npos ? asn_org : asn_org.substr(0, split);
    if (label.empty()) {
        label = "Link " + std::to_string(index + 1);
    }
    if (label.size() > 16) {
        label = label.substr(0, 16);
    }
    return {label, "Unknown"};
}

int DeriveSignalBars(const PerLinkStats& link)
{
    if (link.last_ms_ago > 5000) return 1;
    if (link.last_ms_ago > 3000) return 2;
    if (link.last_ms_ago > 1500) return 3;
    return 4;
}

std::string DeriveStatus(const PerLinkStats& link)
{
    if (link.last_ms_ago > 5000) return "disconnected";
    if (link.last_ms_ago > 2000) return "degraded";
    return "connected";
}

uint32_t EstimateLinkBitrateKbps(const PerLinkStats& link, const RelayStats* relay_stats)
{
    if (!relay_stats || !relay_stats->available || relay_stats->bitrate_kbps == 0) {
        return 0;
    }
    const double clamped_share = std::clamp(link.share_pct, 0.0, 100.0);
    const double estimated =
        (static_cast<double>(relay_stats->bitrate_kbps) * clamped_share) / 100.0;
    return static_cast<uint32_t>(std::llround(estimated));
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helper — build per-link ConnectionSnapshot
// ---------------------------------------------------------------------------

/*static*/
ConnectionSnapshot ConnectionManager::BuildSnapshot(const PerLinkSnapshot& per_link,
                                                    const RelayStats* relay_stats)
{
    ConnectionSnapshot snapshot;
    if (!per_link.available || per_link.links.empty()) {
        return snapshot;
    }
    snapshot.available = true;
    snapshot.items.reserve(per_link.links.size());
    for (std::size_t i = 0; i < per_link.links.size(); ++i) {
        const PerLinkStats& link = per_link.links[i];
        auto label = ClassifyAsnOrg(link.asn_org, i);

        ConnectionInfo info;
        info.id = link.addr.empty() ? ("link-" + std::to_string(i + 1)) : link.addr;
        info.name = std::move(label.first);
        info.type = std::move(label.second);
        info.signal = DeriveSignalBars(link);
        info.bitrate_kbps = EstimateLinkBitrateKbps(link, relay_stats);
        info.status = DeriveStatus(link);
        info.addr = link.addr;
        info.asn_org = link.asn_org;
        info.share_pct = link.share_pct;
        info.last_ms_ago = link.last_ms_ago;
        info.uptime_s = link.uptime_s;
        snapshot.items.push_back(std::move(info));
    }
    return snapshot;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ConnectionManager::~ConnectionManager()
{
    Shutdown();
}

void ConnectionManager::Initialize(Vault* vault, HttpsClient* http,
                                   const std::string& api_host,
                                   const std::string& relay_shared_key,
                                   int heartbeat_interval_sec)
{
    vault_                  = vault;
    http_                   = http;
    api_host_               = api_host;
    relay_shared_key_       = relay_shared_key;
    heartbeat_interval_sec_ = heartbeat_interval_sec;
    initialized_            = true;
    auto_provision_stop_.store(false);

    if (!api_host.empty() && http) {
        std::lock_guard<std::mutex> lock(relay_mu_);
        byor_relay_ = std::make_shared<RelayClient>(*http, api_host, relay_shared_key);
    }

    // Start background stats polling thread.
    stats_stop_.store(false);
    stats_thread_ = std::thread(&ConnectionManager::StatsPollingLoop, this);

    LoadConnections();

#if defined(AEGIS_OBS_PLUGIN_BUILD)
    blog(LOG_INFO,
         "[aegis-cm] initialized: host=%s shared_key=%s",
         api_host.c_str(),
         relay_shared_key.empty() ? "missing" : "configured");
#endif
}

void ConnectionManager::Shutdown()
{
    if (!initialized_) {
        return;
    }
    initialized_ = false;

    // Stop background stats thread.
    stats_stop_.store(true);
    stats_cv_.notify_all();
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(auto_provision_mu_);
        auto_provision_stop_.store(true);
        if (auto_provision_worker_.joinable()) {
            auto_provision_worker_.join();
        }
    }

    // Cancel and join all per-connection worker threads.
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        for (auto& [id, w] : workers_) {
            w->start_cancel.store(true);
        }
    }
    // Join outside worker_mu_ to avoid deadlock with worker threads.
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        for (auto& [id, w] : workers_) {
            if (w->start_worker.joinable()) {
                w->start_worker.join();
            }
            if (w->stop_worker.joinable()) {
                w->stop_worker.join();
            }
        }
        workers_.clear();
    }

    // Destroy BYOR relay client.
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        byor_relay_.reset();
    }

    // Destroy all active managed clients.
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        active_clients_.clear();
    }

#if defined(AEGIS_OBS_PLUGIN_BUILD)
    blog(LOG_INFO, "[aegis-cm] shutdown complete");
#endif
}

void ConnectionManager::Reconfigure(const std::string& api_host,
                                    const std::string& relay_shared_key)
{
    api_host_         = api_host;
    relay_shared_key_ = relay_shared_key;

    // Reconfigure BYOR relay.
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        if (byor_relay_) {
            byor_relay_->Reconfigure(api_host, relay_shared_key);
        } else if (!api_host.empty() && http_) {
            byor_relay_ = std::make_shared<RelayClient>(*http_, api_host, relay_shared_key);
#if defined(AEGIS_OBS_PLUGIN_BUILD)
            blog(LOG_INFO, "[aegis-cm] byor relay client created on reconfigure: host=%s",
                 api_host.c_str());
#endif
        }
    }

    // Reconfigure all active managed clients.
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (auto& [id, client] : active_clients_) {
            client->Reconfigure(api_host, relay_shared_key);
        }
    }
}

// ---------------------------------------------------------------------------
// Background stats polling — runs in dedicated thread, NO tick-thread I/O
// ---------------------------------------------------------------------------

void ConnectionManager::StatsPollingLoop()
{
    using namespace std::chrono;

    while (!stats_stop_.load()) {
        // Wait up to 2 seconds, or wake early on notify_all().
        {
            std::unique_lock<std::mutex> lock(stats_cv_mu_);
            stats_cv_.wait_for(lock, seconds(2), [this] {
                return stats_stop_.load();
            });
        }
        if (stats_stop_.load()) {
            break;
        }

        // Collect BYOR relay + all active managed clients.
        std::shared_ptr<RelayClient> byor;
        {
            std::lock_guard<std::mutex> lock(relay_mu_);
            byor = byor_relay_;
        }

        // Snapshot active managed clients under lock.
        std::vector<std::pair<std::string, std::shared_ptr<RelayClient>>> managed_clients;
        {
            std::lock_guard<std::mutex> lock(connections_mu_);
            for (auto& [id, client] : active_clients_) {
                managed_clients.emplace_back(id, client);
            }
        }

        bool any_active = (byor && byor->HasActiveSession());

        // Sync managed connection rows with session details and provisioning->ready.
        for (auto& [conn_id, client] : managed_clients) {
            const auto session = client->CurrentSession();
            if (session && session->status != "stopped") {
                any_active = true;

                std::string effective_host;
                if (!session->relay_hostname.empty()) {
                    effective_host = session->relay_hostname;
                } else if (!session->public_ip.empty()) {
                    effective_host = session->public_ip;
                }

                const bool active = (session->status == "active");
                std::lock_guard<std::mutex> lock(connections_mu_);
                for (auto& conn : connections_) {
                    if (conn.id != conn_id) {
                        continue;
                    }
                    if (!session->session_id.empty()) {
                        conn.session_id = session->session_id;
                    }
                    if (!effective_host.empty()) {
                        conn.relay_host = effective_host;
                    }
                    if (session->srt_port > 0) {
                        conn.relay_port = session->srt_port;
                    }
                    if (!session->stream_token.empty()) {
                        conn.stream_token = session->stream_token;
                    }
                    if (!session->sender_url.empty()) {
                        conn.sender_url = session->sender_url;
                    }
                    if (!session->media_source_url.empty()) {
                        conn.media_source_url = session->media_source_url;
                    }
                    if (active && conn.status != "ready" && conn.status != "live" && conn.status != "error") {
                        conn.status = "ready";
                    }
                }
            } else {
                std::lock_guard<std::mutex> lock(connections_mu_);
                for (auto& conn : connections_) {
                    if (conn.id == conn_id) {
                        conn.session_id.clear();
                        if (conn.status == "ready" || conn.status == "live") {
                            conn.status = "provisioning";
                        }
                    }
                }
                active_clients_.erase(conn_id);
            }
        }

        // Poll stats from all active clients.
        if (byor && byor->HasActiveSession()) {
            const auto bs = byor->CurrentSession();
            if (bs && !bs->public_ip.empty()) {
                byor->PollRelayStats(bs->public_ip);
                byor->PollPerLinkStats(bs->public_ip);
            }
        }
        for (auto& [conn_id, client] : managed_clients) {
            if (!client->HasActiveSession()) {
                continue;
            }
            const auto s = client->CurrentSession();
            if (s && !s->public_ip.empty()) {
                client->PollRelayStats(s->public_ip);
                // Managed per-link data is refreshed via GetActive().
                const std::string jwt = client->GetStoredJWT();
                if (!jwt.empty()) {
                    client->GetActive(jwt);
                }
            }
        }

        // ready <-> live transitions for managed rows based on per-link freshness.
        for (auto& [conn_id, client] : managed_clients) {
            if (!client->HasActiveSession()) {
                continue;
            }
            const PerLinkSnapshot per_link = client->CurrentPerLinkStats();
            bool is_live = false;
            if (per_link.available) {
                for (const auto& link : per_link.links) {
                    if (link.last_ms_ago < 5000) {
                        is_live = true;
                        break;
                    }
                }
            }
            std::lock_guard<std::mutex> lock(connections_mu_);
            for (auto& conn : connections_) {
                if (conn.id != conn_id) {
                    continue;
                }
                if (is_live && conn.status == "ready") {
                    conn.status = "live";
                } else if (!is_live && conn.status == "live") {
                    conn.status = "ready";
                }
            }
        }

        // Apply the same transitions to BYOR connection rows.
        if (byor && byor->HasActiveSession()) {
            const PerLinkSnapshot byor_per_link = byor->CurrentPerLinkStats();
            bool byor_live = false;
            if (byor_per_link.available) {
                for (const auto& link : byor_per_link.links) {
                    if (link.last_ms_ago < 5000) {
                        byor_live = true;
                        break;
                    }
                }
            }
            std::lock_guard<std::mutex> lock(connections_mu_);
            for (auto& conn : connections_) {
                if (conn.type != "byor") {
                    continue;
                }
                if (conn.status == "provisioning") {
                    conn.status = "ready";
                }
                if (byor_live && conn.status == "ready") {
                    conn.status = "live";
                } else if (!byor_live && conn.status == "live") {
                    conn.status = "ready";
                }
            }
        } else {
            std::lock_guard<std::mutex> lock(connections_mu_);
            for (auto& conn : connections_) {
                if (conn.type == "byor" && (conn.status == "ready" || conn.status == "live")) {
                    conn.status = "provisioning";
                    conn.session_id.clear();
                }
            }
        }

        if (!any_active) {
            // Clear cached data when no relay is active.
            {
                std::lock_guard<std::mutex> lock(snapshot_mu_);
                snapshot_ = ConnectionSnapshot{};
                cached_stats_ = RelayStats{};
                cached_per_link_ = PerLinkSnapshot{};
            }
            continue;
        }

        // Find the client with the best per-link data for the global cached snapshot.
        RelayStats best_stats{};
        PerLinkSnapshot best_per_link{};

        if (byor && byor->HasActiveSession()) {
            best_per_link = byor->CurrentPerLinkStats();
            best_stats = byor->CurrentStats();
        }
        // Check all managed clients - prefer one with actual per-link data.
        for (auto& [conn_id, client] : managed_clients) {
            if (!client->HasActiveSession()) {
                continue;
            }
            auto pl = client->CurrentPerLinkStats();
            auto st = client->CurrentStats();
            // If we do not have per-link data yet, take whatever we find.
            if (!best_per_link.available && pl.available) {
                best_per_link = pl;
                best_stats = st;
            }
            // Prefer the client with non-empty links.
            if (pl.available && !pl.links.empty() &&
                (!best_per_link.available || best_per_link.links.empty())) {
                best_per_link = pl;
                best_stats = st;
            }
        }

        ConnectionSnapshot snap;
        if (best_per_link.available && !best_per_link.links.empty()) {
            snap = BuildSnapshot(best_per_link, best_stats.available ? &best_stats : nullptr);
        }

        {
            std::lock_guard<std::mutex> lock(snapshot_mu_);
            snapshot_ = std::move(snap);
            cached_stats_ = best_stats;
            cached_per_link_ = best_per_link;
        }
    }
}
// ---------------------------------------------------------------------------
// Legacy async relay lifecycle
// ---------------------------------------------------------------------------

#if defined(AEGIS_OBS_PLUGIN_BUILD)

void ConnectionManager::SetConnectionStatus(const std::string& id,
                                            const std::string& status,
                                            const std::string& error_msg)
{
    std::lock_guard<std::mutex> lock(connections_mu_);
    auto it = std::find_if(connections_.begin(), connections_.end(),
                           [&id](const RelayConnectionConfig& c) { return c.id == id; });
    if (it != connections_.end()) {
        it->status = status;
        it->error_msg = error_msg;
    }
}

void ConnectionManager::StartManagedRelayAsync(const std::string& jwt,
                                               const std::string& request_id,
                                               int heartbeat_interval_sec,
                                               const std::string& connection_id)
{
    // Create or reuse a per-connection RelayClient.
    std::shared_ptr<RelayClient> relay;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        auto it = active_clients_.find(connection_id);
        if (it != active_clients_.end()) {
            relay = it->second;
        } else if (!api_host_.empty() && http_) {
            relay = std::make_shared<RelayClient>(*http_, api_host_, relay_shared_key_);
            if (!connection_id.empty()) {
                active_clients_[connection_id] = relay;
            }
        }
    }

    if (!relay) {
        blog(LOG_WARNING,
             "[aegis-cm] StartManagedRelayAsync: relay not configured request_id=%s",
             request_id.c_str());
        if (!connection_id.empty()) {
            SetConnectionStatus(connection_id, "idle", "relay_not_configured");
            stats_cv_.notify_all();
        }
        EmitDockActionResult("relay_start", request_id, "failed", false,
                             "relay_not_configured", "");
        return;
    }

    // Pop pending config and configure the per-connection client.
    {
        std::lock_guard<std::mutex> lock(pending_start_mu_);
        auto pit = pending_starts_.find(connection_id);
        if (pit != pending_starts_.end()) {
            relay->ConfigureNextManagedStart(pit->second.connection_id,
                                             pit->second.region_preference,
                                             pit->second.stream_slot_number,
                                             pit->second.stream_token);
            pending_starts_.erase(pit);
        }
    }

    std::string req_id = request_id;
    std::string conn_id = connection_id;

    // Cancel only THIS connection's in-flight start worker and join it outside
    // the lock so it cannot outlive this object on plugin unload.
    {
        std::thread old_worker;
        {
            std::lock_guard<std::mutex> lk(worker_mu_);
            auto wit = workers_.find(conn_id);
            if (wit != workers_.end() && wit->second->start_worker.joinable()) {
                wit->second->start_cancel.store(true);
                old_worker = std::move(wit->second->start_worker);
            }
        }
        if (old_worker.joinable()) {
            old_worker.join();
            blog(LOG_INFO, "[aegis-cm] relay_start: cancelled previous worker for conn=%s",
                 conn_id.c_str());
        }
    }

    // Bail if Shutdown() ran while we were waiting for the old worker.
    if (!initialized_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        // Ensure a ConnectionWorker exists for this connection.
        auto& worker = workers_[conn_id];
        if (!worker) {
            worker = std::make_unique<ConnectionWorker>();
        }
        worker->start_cancel.store(false);

        // Capture a raw pointer to the cancel flag; the worker owns its lifetime
        // through workers_ map which outlives the thread.
        std::atomic<bool>* cancel_flag = &worker->start_cancel;

        worker->start_worker = std::thread([this, relay, jwt, req_id, conn_id,
                                             heartbeat_interval_sec, cancel_flag]() {
            // Helper: reset connection status to idle on failure and wake stats thread.
            auto reset_on_failure = [&]() {
                if (!conn_id.empty()) {
                    SetConnectionStatus(conn_id, "error", "relay_start_failed");
                    // Remove from active_clients_ ONLY if it's still our relay instance.
                    // A new worker may have already replaced it with a fresh client.
                    {
                        std::lock_guard<std::mutex> lock(connections_mu_);
                        auto it = active_clients_.find(conn_id);
                        if (it != active_clients_.end() && it->second == relay) {
                            active_clients_.erase(it);
                        }
                    }
                    stats_cv_.notify_all();
                }
            };
            try {
                auto session = relay->Start(jwt);
                if (session) {
                    const std::string expected_session_id = session->session_id;
                    const std::string expected_stream_token = session->stream_token;
                    if (session->status == "provisioning") {
                        EmitDockActionResult("relay_start", req_id, "provisioning", true, "",
                                             BuildRelaySessionDetailJson(*session));

                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(180);
                        while (std::chrono::steady_clock::now() < deadline) {
                            if (cancel_flag->load()) {
                                blog(LOG_INFO,
                                     "[aegis-cm] relay_start cancelled: request_id=%s conn=%s",
                                     req_id.c_str(), conn_id.c_str());
                                reset_on_failure();
                                return;
                            }
                            auto polled = relay->GetActive(jwt, expected_session_id,
                                                           expected_stream_token);
                            if (!polled) {
                                const auto now = std::chrono::steady_clock::now();
                                if (now >= deadline) {
                                    break;
                                }
                                const auto step_json = relay->CurrentSession();
                                if (step_json) {
                                    EmitDockActionResult("relay_start", req_id, "provisioning",
                                                         true, "",
                                                         BuildRelaySessionDetailJson(*step_json));
                                }
                                std::this_thread::sleep_for(std::chrono::seconds(3));
                                continue;
                            }

                            if (polled->status == "active") {
                                // Don't check cancel_flag here — the session is confirmed
                                // active. Start the heartbeat unconditionally. If a new
                                // worker was requested, it will stop this heartbeat via
                                // StopHeartbeatLoop when it takes over.
                                relay->StartHeartbeatLoop(jwt, polled->session_id,
                                                         heartbeat_interval_sec);
                                blog(LOG_INFO,
                                     "[aegis-cm] relay_start completed: request_id=%s"
                                     " conn=%s region=%s status=%s",
                                     req_id.c_str(),
                                     conn_id.c_str(),
                                     polled->region.c_str(),
                                     polled->status.c_str());
                                EmitDockActionResult("relay_start", req_id, "completed", true, "",
                                                     BuildRelaySessionDetailJson(*polled));
                                // Wake stats polling thread.
                                stats_cv_.notify_all();
                                return;
                            }

                            if (polled->status == "failed" || polled->status == "stopped") {
                                blog(LOG_WARNING,
                                     "[aegis-cm] relay_start failed: request_id=%s"
                                     " conn=%s error=relay_start_failed",
                                     req_id.c_str(), conn_id.c_str());
                                reset_on_failure();
                                EmitDockActionResult("relay_start", req_id, "failed", false,
                                                     "relay_start_failed", "");
                                return;
                            }

                            EmitDockActionResult("relay_start", req_id, "provisioning", true, "",
                                                 BuildRelaySessionDetailJson(*polled));
                            const auto sleep_for =
                                std::min(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             deadline - std::chrono::steady_clock::now()),
                                         std::chrono::milliseconds(3000));
                            std::this_thread::sleep_for(sleep_for);
                        }

                        if (cancel_flag->load()) {
                            blog(LOG_INFO,
                                 "[aegis-cm] relay_start cancelled: request_id=%s conn=%s",
                                 req_id.c_str(), conn_id.c_str());
                            reset_on_failure();
                            return;
                        }
                        blog(LOG_WARNING,
                             "[aegis-cm] relay_start failed: request_id=%s"
                             " conn=%s error=provision_timeout",
                             req_id.c_str(), conn_id.c_str());
                        reset_on_failure();
                        EmitDockActionResult("relay_start", req_id, "failed", false,
                                             "provision_timeout", "");
                        return;
                    }

                    if (session->status == "active") {
                        relay->StartHeartbeatLoop(jwt, session->session_id,
                                                  heartbeat_interval_sec);
                        blog(LOG_INFO,
                             "[aegis-cm] relay_start completed: request_id=%s"
                             " conn=%s region=%s status=%s",
                             req_id.c_str(),
                             conn_id.c_str(),
                             session->region.c_str(),
                             session->status.c_str());
                        EmitDockActionResult("relay_start", req_id, "completed", true, "",
                                             BuildRelaySessionDetailJson(*session));
                        stats_cv_.notify_all();
                    } else {
                        blog(LOG_WARNING,
                             "[aegis-cm] relay_start failed: request_id=%s"
                             " conn=%s error=relay_start_failed",
                             req_id.c_str(), conn_id.c_str());
                        reset_on_failure();
                        EmitDockActionResult("relay_start", req_id, "failed", false,
                                             "relay_start_failed", "");
                    }
                } else {
                    blog(LOG_WARNING,
                         "[aegis-cm] relay_start failed: request_id=%s"
                         " conn=%s error=relay_start_failed",
                         req_id.c_str(), conn_id.c_str());
                    reset_on_failure();
                    EmitDockActionResult("relay_start", req_id, "failed", false,
                                         "relay_start_failed", "");
                }
            } catch (const std::exception& e) {
                blog(LOG_WARNING,
                     "[aegis-cm] relay_start exception: request_id=%s conn=%s error=%s",
                     req_id.c_str(), conn_id.c_str(), e.what());
                reset_on_failure();
                EmitDockActionResult("relay_start", req_id, "failed", false, e.what(), "");
            }
        });
    }
}

void ConnectionManager::StopManagedRelayAsync(const std::string& jwt,
                                              const std::string& request_id,
                                              const std::string& connection_id)
{
    // Find the relay client for this connection.
    std::shared_ptr<RelayClient> relay;
    if (!connection_id.empty()) {
        std::lock_guard<std::mutex> lock(connections_mu_);
        auto it = active_clients_.find(connection_id);
        if (it != active_clients_.end()) {
            relay = it->second;
        }
    }

    // Fallback to BYOR relay for legacy callers with no connection_id.
    if (!relay) {
        std::lock_guard<std::mutex> lock(relay_mu_);
        if (byor_relay_ && byor_relay_->HasActiveSession()) {
            relay = byor_relay_;
        }
    }

    if (!relay || !relay->HasActiveSession()) {
        blog(LOG_WARNING,
             "[aegis-cm] StopManagedRelayAsync: no active session request_id=%s conn=%s",
             request_id.c_str(), connection_id.c_str());
        EmitDockActionResult("relay_stop", request_id, "failed", false,
                             "no_active_session", "");
        return;
    }

    auto session = relay->CurrentSession();
    std::string sid = session ? session->session_id : "";
    std::string req_id = request_id;
    std::string conn_id = connection_id;

    EmitDockActionResult("relay_stop", request_id, "queued", true, "", "queued_native");
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        // Ensure a ConnectionWorker exists for this connection.
        std::string worker_key = conn_id.empty() ? "__legacy__" : conn_id;
        auto& worker = workers_[worker_key];
        if (!worker) {
            worker = std::make_unique<ConnectionWorker>();
        }
        if (worker->stop_worker.joinable()) {
            worker->stop_worker.join();
        }
        worker->stop_worker = std::thread([this, relay, jwt, sid, req_id, conn_id]() {
            try {
                relay->StopHeartbeatLoop();
                const bool ok = relay->Stop(jwt, sid);
                if (ok) {
                    blog(LOG_INFO,
                         "[aegis-cm] relay_stop completed: request_id=%s conn=%s",
                         req_id.c_str(), conn_id.c_str());
                    EmitDockActionResult("relay_stop", req_id, "completed", true, "", "");
                } else {
                    blog(LOG_WARNING,
                         "[aegis-cm] relay_stop failed: request_id=%s"
                         " conn=%s error=relay_stop_failed",
                         req_id.c_str(), conn_id.c_str());
                    EmitDockActionResult("relay_stop", req_id, "failed", false,
                                         "relay_stop_failed", "");
                }
                // Remove from active_clients_ after stop.
                if (!conn_id.empty()) {
                    std::lock_guard<std::mutex> lock(connections_mu_);
                    active_clients_.erase(conn_id);
                }
                // Wake stats thread so it clears cached data promptly.
                stats_cv_.notify_all();
            } catch (const std::exception& e) {
                blog(LOG_WARNING,
                     "[aegis-cm] relay_stop exception: request_id=%s conn=%s error=%s",
                     req_id.c_str(), conn_id.c_str(), e.what());
                EmitDockActionResult("relay_stop", req_id, "failed", false, e.what(), "");
            }
        });
    }
}

bool ConnectionManager::HasActiveSessionForConnection(const std::string& connection_id) const
{
    if (connection_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(connections_mu_);
    const auto it = active_clients_.find(connection_id);
    return (it != active_clients_.end() && it->second && it->second->HasActiveSession());
}

void ConnectionManager::StopManagedRelaySync(const std::string& jwt,
                                             const std::string& request_id,
                                             const std::string& connection_id)
{
    StopManagedRelayAsync(jwt, request_id, connection_id);

    const std::string worker_key = connection_id.empty() ? "__legacy__" : connection_id;
    std::thread stop_worker;
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        auto wit = workers_.find(worker_key);
        if (wit != workers_.end() && wit->second->stop_worker.joinable()) {
            stop_worker = std::move(wit->second->stop_worker);
        }
    }
    if (stop_worker.joinable()) {
        stop_worker.join();
    }
}

void ConnectionManager::AutoProvisionSavedConnections(const std::string& jwt,
                                                      int heartbeat_interval_sec)
{
    if (jwt.empty()) {
        blog(LOG_INFO, "[aegis-cm] auto-provision skipped: no JWT");
        return;
    }

    std::vector<RelayConnectionConfig> managed;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (const auto& conn : connections_) {
            if (conn.type == "managed") {
                managed.push_back(conn);
            }
        }
    }

    if (managed.empty()) {
        blog(LOG_INFO, "[aegis-cm] auto-provision: no managed connections");
        return;
    }

    blog(LOG_INFO, "[aegis-cm] auto-provision: scheduling %zu managed connection(s)",
         managed.size());

    {
        std::lock_guard<std::mutex> lock(auto_provision_mu_);
        auto_provision_stop_.store(true);
        if (auto_provision_worker_.joinable()) {
            auto_provision_worker_.join();
        }
        auto_provision_stop_.store(false);

        auto_provision_worker_ = std::thread([this, jwt, heartbeat_interval_sec,
                                              managed = std::move(managed)]() {
            std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> jitter_ms(0, 2000);
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();

            for (size_t i = 0; i < managed.size(); ++i) {
                if (auto_provision_stop_.load() || !initialized_) {
                    return;
                }
                if (i > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms(rng)));
                }
                if (auto_provision_stop_.load() || !initialized_) {
                    return;
                }

                const auto& conn = managed[i];
                SetConnectionStatus(conn.id, "provisioning", "");
                ConfigureManagedConnectionStart(conn.id,
                                                conn.managed_region,
                                                conn.stream_slot_number,
                                                conn.stream_token);
                const std::string request_id =
                    "auto_provision_" + std::to_string(now_ms) + "_" + std::to_string(i + 1);
                blog(LOG_INFO,
                     "[aegis-cm] auto-provision start: conn=%s region=%s slot=%d",
                     conn.id.c_str(), conn.managed_region.c_str(), conn.stream_slot_number);
                StartManagedRelayAsync(jwt, request_id, heartbeat_interval_sec, conn.id);
            }
        });
    }
}

#else  // non-OBS build stubs

void ConnectionManager::StartManagedRelayAsync(const std::string&, const std::string&, int, const std::string&) {}
void ConnectionManager::StopManagedRelayAsync(const std::string&, const std::string&, const std::string&) {}
bool ConnectionManager::HasActiveSessionForConnection(const std::string&) const { return false; }
void ConnectionManager::StopManagedRelaySync(const std::string&, const std::string&, const std::string&) {}
void ConnectionManager::AutoProvisionSavedConnections(const std::string&, int) {}

#endif  // AEGIS_OBS_PLUGIN_BUILD

// ---------------------------------------------------------------------------
// BYOR direct connect/disconnect
// ---------------------------------------------------------------------------

void ConnectionManager::ConnectDirect(const std::string& host, int port,
                                      const std::string& stream_id)
{
    std::lock_guard<std::mutex> lock(relay_mu_);
    if (byor_relay_) {
        byor_relay_->ConnectDirect(host, port, stream_id);
        stats_cv_.notify_all();
    }
}

void ConnectionManager::DisconnectDirect()
{
    std::shared_ptr<RelayClient> relay;
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay = byor_relay_;
    }
    if (relay) {
        relay->DisconnectDirect();
        stats_cv_.notify_all();
    }
}

void ConnectionManager::CancelPendingStart()
{
    std::lock_guard<std::mutex> lk(worker_mu_);
    for (auto& [id, w] : workers_) {
        w->start_cancel.store(true);
    }
}

void ConnectionManager::EmergencyStop(const std::string& jwt)
{
    // Emergency stop BYOR relay.
    {
        std::shared_ptr<RelayClient> byor;
        {
            std::lock_guard<std::mutex> lock(relay_mu_);
            byor = byor_relay_;
        }
        if (byor && byor->HasActiveSession()) {
            if (byor->IsBYORMode()) {
                byor->DisconnectDirect();
            } else if (!jwt.empty()) {
                byor->EmergencyRelayStop(jwt);
            }
        }
    }

    // Emergency stop all active managed clients.
    std::vector<std::shared_ptr<RelayClient>> clients;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (auto& [id, client] : active_clients_) {
            clients.push_back(client);
        }
    }
    for (auto& client : clients) {
        if (client->HasActiveSession() && !jwt.empty()) {
            client->EmergencyRelayStop(jwt);
        }
    }
}

// ---------------------------------------------------------------------------
// Status accessors — all read cached values, no network I/O
// ---------------------------------------------------------------------------

ConnectionSnapshot ConnectionManager::CurrentSnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshot_mu_);
    return snapshot_;
}

bool ConnectionManager::HasActiveSession() const
{
    // Check BYOR relay.
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        if (byor_relay_ && byor_relay_->HasActiveSession()) {
            return true;
        }
    }
    // Check active managed clients.
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (auto& [id, client] : active_clients_) {
            if (client->HasActiveSession()) {
                return true;
            }
        }
    }
    return false;
}

bool ConnectionManager::IsBYORMode() const
{
    std::lock_guard<std::mutex> lock(relay_mu_);
    return byor_relay_ && byor_relay_->IsBYORMode();
}

std::optional<RelaySession> ConnectionManager::CurrentSession() const
{
    // Return BYOR session if active.
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        if (byor_relay_ && byor_relay_->HasActiveSession()) {
            return byor_relay_->CurrentSession();
        }
    }
    // Return first active managed session (legacy compat).
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (auto& [id, client] : active_clients_) {
            if (client->HasActiveSession()) {
                return client->CurrentSession();
            }
        }
    }
    return std::nullopt;
}

std::optional<RelaySession> ConnectionManager::CurrentSessionForConnection(const std::string& id) const
{
    if (id.empty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(connections_mu_);
    auto it = active_clients_.find(id);
    if (it == active_clients_.end() || !it->second) {
        return std::nullopt;
    }
    return it->second->CurrentSession();
}

RelayStats ConnectionManager::CurrentStats() const
{
    std::lock_guard<std::mutex> lock(snapshot_mu_);
    return cached_stats_;
}

PerLinkSnapshot ConnectionManager::CurrentPerLinkStats() const
{
    std::lock_guard<std::mutex> lock(snapshot_mu_);
    return cached_per_link_;
}

PerLinkSnapshot ConnectionManager::CurrentPerLinkStatsForConnection(const std::string& connection_id) const
{
    std::lock_guard<std::mutex> lock(connections_mu_);
    auto it = active_clients_.find(connection_id);
    if (it != active_clients_.end() && it->second) {
        return it->second->CurrentPerLinkStats();
    }
    return PerLinkSnapshot{};
}

RelayStats ConnectionManager::CurrentStatsForConnection(const std::string& connection_id) const
{
    std::lock_guard<std::mutex> lock(connections_mu_);
    auto it = active_clients_.find(connection_id);
    if (it != active_clients_.end() && it->second) {
        return it->second->CurrentStats();
    }
    return RelayStats{};
}

// ---------------------------------------------------------------------------
// Multi-connection CRUD stubs (Phase 2 — not yet wired to UI)
// ---------------------------------------------------------------------------

std::string ConnectionManager::AddConnection(const RelayConnectionConfig& config)
{
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        connections_.push_back(config);
    }
    SaveConnections();
    return config.id;
}

void ConnectionManager::UpdateConnection(const std::string& id,
                                         const RelayConnectionConfig& updated)
{
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (auto& conn : connections_) {
            if (conn.id == id) {
                conn = updated;
                break;
            }
        }
    }
    SaveConnections();
}

void ConnectionManager::RemoveConnection(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        connections_.erase(
            std::remove_if(connections_.begin(), connections_.end(),
                           [&id](const RelayConnectionConfig& c) { return c.id == id; }),
            connections_.end());
        active_clients_.erase(id);
    }
    // Also clean up any worker for this connection.
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        auto wit = workers_.find(id);
        if (wit != workers_.end()) {
            wit->second->start_cancel.store(true);
            if (wit->second->start_worker.joinable()) {
                wit->second->start_worker.join();
            }
            if (wit->second->stop_worker.joinable()) {
                wit->second->stop_worker.join();
            }
            workers_.erase(wit);
        }
    }
    SaveConnections();
}

void ConnectionManager::RemoveManagedConnections()
{
    std::vector<std::string> managed_ids;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        for (const auto& c : connections_) {
            if (c.type == "managed") {
                managed_ids.push_back(c.id);
            }
        }
    }
    for (const auto& id : managed_ids) {
        RemoveConnection(id);
    }
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    blog(LOG_INFO, "[aegis-cm] removed %zu managed connections (sign-out)",
         managed_ids.size());
#endif
}

std::vector<RelayConnectionConfig> ConnectionManager::ListConnections() const
{
    std::lock_guard<std::mutex> lock(connections_mu_);
    return connections_;
}

// ---------------------------------------------------------------------------
// Connection persistence
// ---------------------------------------------------------------------------

void ConnectionManager::SaveConnections()
{
    // Snapshot under lock, then do all I/O outside the lock.
    std::vector<RelayConnectionConfig> conns;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        conns = connections_;
    }

    // --- Non-sensitive fields → config.json "connections" array ---
    std::string path = PluginConfig::ConfigFilePath();
    if (!path.empty()) {
        // Load existing config.json to preserve all other keys.
        QJsonObject obj;
        QFile rf(QString::fromStdString(path));
        if (rf.exists() && rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QByteArray data = rf.readAll();
            rf.close();
            QJsonParseError err;
            QJsonDocument existing = QJsonDocument::fromJson(data, &err);
            if (err.error == QJsonParseError::NoError && existing.isObject())
                obj = existing.object();
        }

        QJsonArray arr;
        for (const auto& c : conns) {
            QJsonObject entry;
            entry["id"]             = QString::fromStdString(c.id);
            entry["name"]           = QString::fromStdString(c.name);
            entry["type"]           = QString::fromStdString(c.type);
            entry["managed_region"] = QString::fromStdString(c.managed_region);
            entry["stream_slot_number"] = c.stream_slot_number;
            entry["stream_slot_label"] = QString::fromStdString(c.stream_slot_label);
            arr.append(entry);
        }
        obj["connections"] = arr;

        QSaveFile sf(QString::fromStdString(path));
        if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            sf.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
            sf.commit();
        }
    }

    // --- Sensitive BYOR fields → vault, keyed by "conn_<id>" ---
    if (!vault_) return;

    std::set<std::string> current_ids;
    for (const auto& c : conns) {
        current_ids.insert(c.id);
        if (c.type == "byor") {
            QJsonObject secret;
            secret["relay_host"] = QString::fromStdString(c.relay_host);
            secret["relay_port"] = c.relay_port;
            secret["stream_id"]  = QString::fromStdString(c.stream_id);
            std::string blob =
                QJsonDocument(secret).toJson(QJsonDocument::Compact).toStdString();
            vault_->Set("conn_" + c.id, blob);
        }
    }

    // Prune vault entries for connections that no longer exist.
    for (const auto& key : vault_->Keys()) {
        if (key.size() > 5 && key.substr(0, 5) == "conn_") {
            std::string conn_id = key.substr(5);
            if (current_ids.find(conn_id) == current_ids.end()) {
                vault_->Remove(key);
            }
        }
    }

#if defined(AEGIS_OBS_PLUGIN_BUILD)
    blog(LOG_INFO, "[aegis-cm] saved %zu connections to disk",
         conns.size());
#endif
}

void ConnectionManager::LoadConnections()
{
    std::string path = PluginConfig::ConfigFilePath();
    if (path.empty()) return;

    QFile file(QString::fromStdString(path));
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;

    QJsonObject obj = doc.object();
    if (!obj.contains("connections") || !obj["connections"].isArray())
        return;

    std::vector<RelayConnectionConfig> loaded;
    for (const auto& item : obj["connections"].toArray()) {
        if (!item.isObject()) continue;
        QJsonObject entry = item.toObject();

        RelayConnectionConfig cfg;
        cfg.id             = entry["id"].toString().toStdString();
        cfg.name           = entry["name"].toString().toStdString();
        cfg.type           = entry["type"].toString().toStdString();
        cfg.managed_region = entry["managed_region"].toString().toStdString();
        cfg.stream_slot_number = entry["stream_slot_number"].toInt(0);
        cfg.stream_slot_label = entry["stream_slot_label"].toString().toStdString();
        cfg.status         = "idle";  // runtime state always starts fresh

        if (cfg.id.empty()) continue;

        // Load sensitive BYOR fields from vault.
        if (cfg.type == "byor" && vault_) {
            auto secret = vault_->Get("conn_" + cfg.id);
            if (secret) {
                QJsonDocument sd = QJsonDocument::fromJson(
                    QByteArray::fromStdString(*secret));
                if (sd.isObject()) {
                    QJsonObject s = sd.object();
                    cfg.relay_host = s["relay_host"].toString().toStdString();
                    cfg.relay_port = s["relay_port"].toInt(5000);
                    cfg.stream_id  = s["stream_id"].toString().toStdString();
                }
            }
        }

        loaded.push_back(std::move(cfg));
    }

    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        connections_ = std::move(loaded);
    }

#if defined(AEGIS_OBS_PLUGIN_BUILD)
    blog(LOG_INFO, "[aegis-cm] loaded %zu connections from disk",
         connections_.size());
#endif
}

}  // namespace aegis
