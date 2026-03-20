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

    if (!api_host.empty() && http) {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay_ = std::make_shared<RelayClient>(*http, api_host, relay_shared_key);
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

    // Cancel and join relay worker threads.
    start_cancel_.store(true);
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        if (start_worker_.joinable()) {
            start_worker_.join();
        }
        if (stop_worker_.joinable()) {
            stop_worker_.join();
        }
    }

    // Destroy relay client.
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay_.reset();
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

    std::lock_guard<std::mutex> lock(relay_mu_);
    if (relay_) {
        relay_->Reconfigure(api_host, relay_shared_key);
    } else if (!api_host.empty() && http_) {
        relay_ = std::make_shared<RelayClient>(*http_, api_host, relay_shared_key);
#if defined(AEGIS_OBS_PLUGIN_BUILD)
        blog(LOG_INFO, "[aegis-cm] relay client created on reconfigure: host=%s",
             api_host.c_str());
#endif
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

        std::shared_ptr<RelayClient> relay;
        {
            std::lock_guard<std::mutex> lock(relay_mu_);
            relay = relay_;
        }

        if (!relay || !relay->HasActiveSession()) {
            // Clear cached data when no relay is active.
            {
                std::lock_guard<std::mutex> lock(snapshot_mu_);
                snapshot_      = ConnectionSnapshot{};
                cached_stats_  = RelayStats{};
                cached_per_link_ = PerLinkSnapshot{};
            }
            // Downgrade any "connected" connections back to "connecting"
            // (relay lost session but connection hasn't been explicitly disconnected).
            {
                std::lock_guard<std::mutex> lock(connections_mu_);
                for (auto& conn : connections_) {
                    if (conn.status == "connected") {
                        conn.status = "connecting";
                    }
                }
            }
            continue;
        }

        const auto session = relay->CurrentSession();
        if (!session || session->public_ip.empty()) {
            continue;
        }

        // Network I/O happens HERE, not on the OBS tick thread.
        relay->PollRelayStats(session->public_ip);
        relay->PollPerLinkStats(session->public_ip);

        RelayStats     stats    = relay->CurrentStats();
        PerLinkSnapshot per_link = relay->CurrentPerLinkStats();

        ConnectionSnapshot snap;
        if (per_link.available && !per_link.links.empty()) {
            snap = BuildSnapshot(per_link, stats.available ? &stats : nullptr);
        }

        {
            std::lock_guard<std::mutex> lock(snapshot_mu_);
            snapshot_        = std::move(snap);
            cached_stats_    = stats;
            cached_per_link_ = per_link;
        }

        // Update connection statuses based on relay state.
        {
            std::lock_guard<std::mutex> lock(connections_mu_);
            for (auto& conn : connections_) {
                if (conn.status == "connecting" && relay->HasActiveSession()) {
                    conn.status = "connected";
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Legacy async relay lifecycle
// ---------------------------------------------------------------------------

#if defined(AEGIS_OBS_PLUGIN_BUILD)

void ConnectionManager::StartManagedRelayAsync(const std::string& jwt,
                                               const std::string& request_id,
                                               int heartbeat_interval_sec)
{
    std::shared_ptr<RelayClient> relay;
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay = relay_;
    }
    if (!relay) {
        blog(LOG_WARNING,
             "[aegis-cm] StartManagedRelayAsync: relay not configured request_id=%s",
             request_id.c_str());
        EmitDockActionResult("relay_start", request_id, "failed", false,
                             "relay_not_configured", "");
        return;
    }

    std::string req_id = request_id;
    // Cancel any in-flight relay-start worker and join it outside the lock so
    // it cannot outlive this object on plugin unload (use-after-free fix).
    {
        std::thread old_worker;
        {
            std::lock_guard<std::mutex> lk(worker_mu_);
            if (start_worker_.joinable()) {
                start_cancel_.store(true);
                old_worker = std::move(start_worker_);
            }
        }
        if (old_worker.joinable()) {
            old_worker.join();
            blog(LOG_INFO, "[aegis-cm] relay_start: cancelled previous worker");
        }
    }
    // Bail if Shutdown() ran while we were waiting for the old worker.
    if (!initialized_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        start_cancel_.store(false);
        start_worker_ = std::thread([this, relay, jwt, req_id, heartbeat_interval_sec]() {
            try {
                auto session = relay->Start(jwt);
                if (session) {
                    if (session->status == "provisioning") {
                        EmitDockActionResult("relay_start", req_id, "provisioning", true, "",
                                             BuildRelaySessionDetailJson(*session));

                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(180);
                        while (std::chrono::steady_clock::now() < deadline) {
                            if (start_cancel_.load()) {
                                blog(LOG_INFO,
                                     "[aegis-cm] relay_start cancelled: request_id=%s",
                                     req_id.c_str());
                                return;
                            }
                            auto polled = relay->GetActive(jwt);
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
                                if (start_cancel_.load()) return;
                                relay->StartHeartbeatLoop(jwt, polled->session_id,
                                                         heartbeat_interval_sec);
                                blog(LOG_INFO,
                                     "[aegis-cm] relay_start completed: request_id=%s"
                                     " region=%s status=%s",
                                     req_id.c_str(),
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
                                     " error=relay_start_failed",
                                     req_id.c_str());
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

                        if (start_cancel_.load()) {
                            blog(LOG_INFO,
                                 "[aegis-cm] relay_start cancelled: request_id=%s",
                                 req_id.c_str());
                            return;
                        }
                        blog(LOG_WARNING,
                             "[aegis-cm] relay_start failed: request_id=%s"
                             " error=provision_timeout",
                             req_id.c_str());
                        EmitDockActionResult("relay_start", req_id, "failed", false,
                                             "provision_timeout", "");
                        return;
                    }

                    if (session->status == "active") {
                        relay->StartHeartbeatLoop(jwt, session->session_id,
                                                  heartbeat_interval_sec);
                        blog(LOG_INFO,
                             "[aegis-cm] relay_start completed: request_id=%s"
                             " region=%s status=%s",
                             req_id.c_str(),
                             session->region.c_str(),
                             session->status.c_str());
                        EmitDockActionResult("relay_start", req_id, "completed", true, "",
                                             BuildRelaySessionDetailJson(*session));
                        stats_cv_.notify_all();
                    } else {
                        blog(LOG_WARNING,
                             "[aegis-cm] relay_start failed: request_id=%s"
                             " error=relay_start_failed",
                             req_id.c_str());
                        EmitDockActionResult("relay_start", req_id, "failed", false,
                                             "relay_start_failed", "");
                    }
                } else {
                    blog(LOG_WARNING,
                         "[aegis-cm] relay_start failed: request_id=%s"
                         " error=relay_start_failed",
                         req_id.c_str());
                    EmitDockActionResult("relay_start", req_id, "failed", false,
                                         "relay_start_failed", "");
                }
            } catch (const std::exception& e) {
                blog(LOG_WARNING,
                     "[aegis-cm] relay_start exception: request_id=%s error=%s",
                     req_id.c_str(), e.what());
                EmitDockActionResult("relay_start", req_id, "failed", false, e.what(), "");
            }
        });
    }
}

void ConnectionManager::StopManagedRelayAsync(const std::string& jwt,
                                              const std::string& request_id)
{
    std::shared_ptr<RelayClient> relay;
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay = relay_;
    }
    if (!relay || !relay->HasActiveSession()) {
        blog(LOG_WARNING,
             "[aegis-cm] StopManagedRelayAsync: no active session request_id=%s",
             request_id.c_str());
        EmitDockActionResult("relay_stop", request_id, "failed", false,
                             "no_active_session", "");
        return;
    }

    auto session = relay->CurrentSession();
    std::string sid = session ? session->session_id : "";
    std::string req_id = request_id;

    EmitDockActionResult("relay_stop", request_id, "queued", true, "", "queued_native");
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        if (stop_worker_.joinable()) {
            stop_worker_.join();
        }
        stop_worker_ = std::thread([this, relay, jwt, sid, req_id]() {
            try {
                relay->StopHeartbeatLoop();
                const bool ok = relay->Stop(jwt, sid);
                if (ok) {
                    blog(LOG_INFO,
                         "[aegis-cm] relay_stop completed: request_id=%s",
                         req_id.c_str());
                    EmitDockActionResult("relay_stop", req_id, "completed", true, "", "");
                } else {
                    blog(LOG_WARNING,
                         "[aegis-cm] relay_stop failed: request_id=%s"
                         " error=relay_stop_failed",
                         req_id.c_str());
                    EmitDockActionResult("relay_stop", req_id, "failed", false,
                                         "relay_stop_failed", "");
                }
                // Wake stats thread so it clears cached data promptly.
                stats_cv_.notify_all();
            } catch (const std::exception& e) {
                blog(LOG_WARNING,
                     "[aegis-cm] relay_stop exception: request_id=%s error=%s",
                     req_id.c_str(), e.what());
                EmitDockActionResult("relay_stop", req_id, "failed", false, e.what(), "");
            }
        });
    }
}

#else  // non-OBS build stubs

void ConnectionManager::StartManagedRelayAsync(const std::string&, const std::string&, int) {}
void ConnectionManager::StopManagedRelayAsync(const std::string&, const std::string&) {}

#endif  // AEGIS_OBS_PLUGIN_BUILD

// ---------------------------------------------------------------------------
// BYOR direct connect/disconnect
// ---------------------------------------------------------------------------

void ConnectionManager::ConnectDirect(const std::string& host, int port,
                                      const std::string& stream_id)
{
    std::lock_guard<std::mutex> lock(relay_mu_);
    if (relay_) {
        relay_->ConnectDirect(host, port, stream_id);
        stats_cv_.notify_all();
    }
}

void ConnectionManager::DisconnectDirect()
{
    std::shared_ptr<RelayClient> relay;
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay = relay_;
    }
    if (relay) {
        relay->DisconnectDirect();
        stats_cv_.notify_all();
    }
}

void ConnectionManager::CancelPendingStart()
{
    start_cancel_.store(true);
}

void ConnectionManager::EmergencyStop(const std::string& jwt)
{
    std::shared_ptr<RelayClient> relay;
    {
        std::lock_guard<std::mutex> lock(relay_mu_);
        relay = relay_;
    }
    if (!relay || !relay->HasActiveSession()) {
        return;
    }
    if (relay->IsBYORMode()) {
        relay->DisconnectDirect();
    } else if (!jwt.empty()) {
        relay->EmergencyRelayStop(jwt);
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
    std::lock_guard<std::mutex> lock(relay_mu_);
    return relay_ && relay_->HasActiveSession();
}

bool ConnectionManager::IsBYORMode() const
{
    std::lock_guard<std::mutex> lock(relay_mu_);
    return relay_ && relay_->IsBYORMode();
}

std::optional<RelaySession> ConnectionManager::CurrentSession() const
{
    std::lock_guard<std::mutex> lock(relay_mu_);
    return relay_ ? relay_->CurrentSession() : std::nullopt;
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
    SaveConnections();
}

std::vector<RelayConnectionConfig> ConnectionManager::ListConnections() const
{
    std::lock_guard<std::mutex> lock(connections_mu_);
    return connections_;
}

void ConnectionManager::Connect(const std::string& id, const std::string& jwt)
{
    RelayConnectionConfig config;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        auto it = std::find_if(connections_.begin(), connections_.end(),
                               [&id](const RelayConnectionConfig& c) { return c.id == id; });
        if (it == connections_.end()) return;
        it->status = "connecting";
        it->error_msg.clear();
        config = *it;
    }

    if (config.type == "byor") {
        ConnectDirect(config.relay_host, config.relay_port,
                      config.stream_id.empty() ? "" : config.stream_id);
    }
    // Managed: routed to StartManagedRelayAsync at the action dispatch layer
    // (connection_connect action). Nothing to do here for managed connections.
    (void)jwt;
}

void ConnectionManager::Disconnect(const std::string& id, const std::string& jwt)
{
    RelayConnectionConfig config;
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        auto it = std::find_if(connections_.begin(), connections_.end(),
                               [&id](const RelayConnectionConfig& c) { return c.id == id; });
        if (it == connections_.end()) return;
        it->status = "disconnecting";
        config = *it;
    }

    if (config.type == "byor") {
        DisconnectDirect();
    }

    // Update status to idle after disconnect
    {
        std::lock_guard<std::mutex> lock(connections_mu_);
        auto it = std::find_if(connections_.begin(), connections_.end(),
                               [&id](const RelayConnectionConfig& c) { return c.id == id; });
        if (it != connections_.end()) {
            it->status = "idle";
        }
    }
    // Managed: routed to StopManagedRelayAsync at the action dispatch layer
    // (connection_disconnect action). Nothing to do here for managed connections.
    (void)jwt;
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
