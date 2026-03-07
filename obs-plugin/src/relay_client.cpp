#include "relay_client.h"

#include <obs-module.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>

#include <rpc.h>       // UuidCreate, UuidToStringA, RpcStringFreeA
// Link: rpcrt4.lib — added in CMakeLists.txt alongside winhttp and crypt32.

#include <chrono>
#include <stdexcept>
#include <string>

namespace aegis {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------
RelayClient::RelayClient(HttpsClient& http,
                         const std::string& api_host,
                         const std::string& relay_shared_key)
    : http_(http)
    , api_host_w_(api_host.begin(), api_host.end())
    , relay_shared_key_w_(relay_shared_key.begin(), relay_shared_key.end())
{
}

RelayClient::~RelayClient()
{
    StopHeartbeatLoop();
}

// ---------------------------------------------------------------------------
// UUID v4 generation via Windows RPC API
// ---------------------------------------------------------------------------
std::string RelayClient::GenerateUuidV4()
{
    UUID uuid;
    UuidCreate(&uuid);

    RPC_CSTR str = nullptr;
    UuidToStringA(&uuid, &str);
    std::string result(reinterpret_cast<char*>(str));
    RpcStringFreeA(&str);

    return result;
}

// ---------------------------------------------------------------------------
// JSON helpers — parse a relay session from the control-plane JSON response.
// Uses Qt6 QJsonDocument which is already linked for config_vault.cpp.
// ---------------------------------------------------------------------------
std::optional<RelaySession> RelayClient::ParseSessionResponse(const std::string& json)
{
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isObject()) {
        return std::nullopt;
    }

    QJsonObject obj = doc.object();
    if (obj.contains("session") && obj["session"].isObject()) {
        obj = obj["session"].toObject();
    }

    RelaySession session;
    session.session_id = obj["session_id"].toString().toStdString();
    if (session.session_id.empty()) {
        session.session_id = obj["id"].toString().toStdString();
    }
    session.status     = obj["status"].toString().toStdString();
    session.region     = obj["region"].toString().toStdString();

    // Relay connection info (nested under "relay" key in Go response).
    QJsonObject relayObj = obj["relay"].toObject();
    session.public_ip = relayObj["public_ip"].toString().toStdString();
    session.srt_port = relayObj["srt_port"].toInt(9000);
    session.ws_url = relayObj["ws_url"].toString().toStdString();
    session.relay_hostname = relayObj["relay_hostname"].toString().toStdString();

    // Credentials.
    QJsonObject creds = obj["credentials"].toObject();
    session.pair_token = creds["pair_token"].toString().toStdString();

    // Timers.
    QJsonObject timers = obj["timers"].toObject();
    session.grace_window_seconds = timers["grace_window_seconds"].toInt(0);
    session.max_session_seconds = timers["max_session_seconds"].toInt(0);
    session.provision_step = obj["provision_step"].toString().toStdString();

    // Reject if session_id is empty — indicates a malformed response.
    if (session.session_id.empty()) {
        return std::nullopt;
    }

    return session;
}

// ---------------------------------------------------------------------------
// Control plane calls
// ---------------------------------------------------------------------------

std::optional<RelaySession> RelayClient::GetActive(const std::string& jwt)
{
    std::wstring path = L"/api/v1/relay/active";
    std::wstring wide_jwt(jwt.begin(), jwt.end());

    HttpResponse resp;
    try {
        resp = http_.Get(api_host_w_, path, wide_jwt);
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "relay: GetActive network error: %s", e.what());
        return std::nullopt;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "relay: GetActive failed, HTTP %lu", resp.status_code);
        return std::nullopt;
    }

    // The server returns literal "null" (or empty) when no session is active.
    if (resp.body == "null" || resp.body.empty()) {
        return std::nullopt;
    }

    auto session = ParseSessionResponse(resp.body);
    if (session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = session;
    }
    return session;
}

std::optional<RelaySession> RelayClient::Start(const std::string& jwt)
{
    std::string uuid = GenerateUuidV4();
    std::string body = "{\"mode\":\"auto\",\"idempotency_key\":\"" + uuid + "\"}";
    std::wstring wide_jwt(jwt.begin(), jwt.end());
    std::wstring wide_uuid(uuid.begin(), uuid.end());

    HttpResponse resp;
    try {
        resp = http_.Post(
            api_host_w_,
            L"/api/v1/relay/start",
            body,
            wide_jwt,
            {{L"Idempotency-Key", wide_uuid}});
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "relay: Start network error: %s", e.what());
        return std::nullopt;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "relay: Start failed, HTTP %lu: %s",
             resp.status_code, resp.body.c_str());
        return std::nullopt;
    }

    auto session = ParseSessionResponse(resp.body);
    if (session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = session;
    }
    return session;
}

bool RelayClient::Stop(const std::string& jwt, const std::string& session_id)
{
    std::string body = "{\"session_id\":\"" + session_id + "\"}";
    std::wstring wide_jwt(jwt.begin(), jwt.end());

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/relay/stop", body, wide_jwt);
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "relay: Stop network error: %s", e.what());
        return false;
    }

    if (resp.ok()) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = std::nullopt;
        blog(LOG_INFO, "relay: session %s stopped", session_id.c_str());
    } else {
        blog(LOG_WARNING, "relay: Stop failed, HTTP %lu: %s",
             resp.status_code, resp.body.c_str());
    }
    return resp.ok();
}

bool RelayClient::SendHeartbeat(const std::string& jwt, const std::string& session_id)
{
    std::string body = "{\"session_id\":\"" + session_id + "\"}";
    std::wstring wide_jwt(jwt.begin(), jwt.end());
    std::vector<std::pair<std::wstring, std::wstring>> extra_headers;
    // Control-plane auth split (telemy-v0.0.3):
    // - /relay/start and /relay/stop use JWT auth middleware.
    // - /relay/health additionally requires X-Relay-Auth (shared key).
    if (!relay_shared_key_w_.empty()) {
        extra_headers.push_back({L"X-Relay-Auth", relay_shared_key_w_});
    } else if (!logged_missing_health_shared_key_.exchange(true)) {
        blog(LOG_WARNING, "relay: relay_shared_key missing; health requests will likely be rejected");
    }

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/relay/health", body, wide_jwt, extra_headers);
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "relay: health ping network error: %s", e.what());
        return false;
    }

    if (resp.status_code == 404) {
        // Session expired by server TTL — clean up local state.
        blog(LOG_WARNING, "relay: health ping 404 — session expired by server");
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = std::nullopt;
        return false;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "relay: health ping failed, HTTP %lu", resp.status_code);
    }
    return resp.ok();
}

// ---------------------------------------------------------------------------
// Heartbeat loop — runs on a dedicated thread with a condition-variable-based
// sleep so that StopHeartbeatLoop() can wake it immediately rather than
// waiting for the full interval to elapse.
// ---------------------------------------------------------------------------

void RelayClient::StartHeartbeatLoop(const std::string& jwt,
                                     const std::string& session_id,
                                     int interval_sec)
{
    // Stop any existing loop before starting a new one.
    StopHeartbeatLoop();

    heartbeat_running_ = true;

    heartbeat_thread_ = std::thread([this, jwt, session_id, interval_sec]() {
        blog(LOG_INFO, "relay: heartbeat loop started (session %s, interval %ds)",
             session_id.c_str(), interval_sec);

        while (heartbeat_running_) {
            // Wait for the interval, but wake early if StopHeartbeatLoop() signals.
            {
                std::unique_lock<std::mutex> lock(heartbeat_cv_mutex_);
                heartbeat_cv_.wait_for(lock,
                    std::chrono::seconds(interval_sec),
                    [this] { return !heartbeat_running_.load(); });
            }

            if (!heartbeat_running_) {
                break;
            }

            try {
                if (!SendHeartbeat(jwt, session_id)) {
                    blog(LOG_WARNING, "relay: heartbeat failed, stopping loop");
                    heartbeat_running_ = false;
                }
            } catch (const std::exception& e) {
                blog(LOG_WARNING, "relay: heartbeat exception: %s", e.what());
                // Continue the loop — transient network errors should not kill
                // the heartbeat permanently. The next attempt may succeed.
            }
        }

        blog(LOG_INFO, "relay: heartbeat loop exited");
    });
}

void RelayClient::StopHeartbeatLoop()
{
    if (heartbeat_running_) {
        heartbeat_running_ = false;
        heartbeat_cv_.notify_all();
    }

    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
}

// ---------------------------------------------------------------------------
// Emergency stop — called from obs_module_unload(). Best-effort: stops the
// heartbeat loop, then issues a synchronous Stop() call.  The whole operation
// should complete within the WinHTTP timeouts (configured in HttpsClient to
// 5s connect + 30s receive, but a Stop call is typically sub-second).
// ---------------------------------------------------------------------------

void RelayClient::EmergencyRelayStop(const std::string& jwt)
{
    StopHeartbeatLoop();

    std::optional<RelaySession> session;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session = current_session_;
    }

    if (session && session->status != "stopped") {
        blog(LOG_INFO, "relay: emergency stop for session %s",
             session->session_id.c_str());
        try {
            Stop(jwt, session->session_id);
        } catch (const std::exception& e) {
            blog(LOG_WARNING, "relay: emergency stop failed: %s", e.what());
        }
    }
}

// ---------------------------------------------------------------------------
// Thread-safe state accessors
// ---------------------------------------------------------------------------

std::optional<RelaySession> RelayClient::CurrentSession() const
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    return current_session_;
}

bool RelayClient::HasActiveSession() const
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    return current_session_.has_value()
        && current_session_->status != "stopped";
}

// ---------------------------------------------------------------------------
// SLS stats polling
// ---------------------------------------------------------------------------

void RelayClient::PollRelayStats(const std::string& relay_ip)
{
    if (relay_ip.empty()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // Build wide host string with SLS stats port
    std::string host_port = relay_ip + ":8090";
    std::wstring host_w(host_port.begin(), host_port.end());
    std::wstring path_w = L"/stats/play_aegis?legacy=1";

    HttpResponse resp;
    try {
        resp = http_.Get(host_w, path_w);
    } catch (const std::exception& e) {
        blog(LOG_DEBUG, "[aegis-relay] stats poll http error: %s", e.what());
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    if (resp.status_code != 200 || resp.body.empty()) {
        blog(LOG_DEBUG, "[aegis-relay] stats poll failed: status=%lu",
             resp.status_code);
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // Parse legacy JSON: { "status":"ok", "publishers": { "<key>": { ... } } }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(resp.body.c_str(), static_cast<int>(resp.body.size())), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject pubs = root.value("publishers").toObject();
    if (pubs.isEmpty()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // Take the first (and typically only) publisher
    QJsonObject pub = pubs.begin()->toObject();

    RelayStats s;
    s.available        = true;
    s.bitrate_kbps     = static_cast<uint32_t>(pub.value("bitrate").toInt(0));
    s.rtt_ms           = pub.value("rtt").toDouble(0.0);
    s.pkt_loss         = static_cast<uint64_t>(pub.value("pktRcvLoss").toDouble(0));
    s.pkt_drop         = static_cast<uint64_t>(pub.value("pktRcvDrop").toDouble(0));
    s.recv_rate_mbps   = pub.value("mbpsRecvRate").toDouble(0.0);
    s.bandwidth_mbps   = pub.value("mbpsBandwidth").toDouble(0.0);
    s.latency_ms       = static_cast<uint32_t>(pub.value("latency").toInt(0));
    s.uptime_seconds   = static_cast<uint32_t>(pub.value("uptime").toInt(0));

    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_ = s;
    }

    blog(LOG_DEBUG,
         "[aegis-relay] stats poll ok: bitrate=%u rtt=%.1f loss=%llu drop=%llu latency=%u",
         s.bitrate_kbps, s.rtt_ms,
         static_cast<unsigned long long>(s.pkt_loss),
         static_cast<unsigned long long>(s.pkt_drop),
         s.latency_ms);
}

RelayStats RelayClient::CurrentStats() const
{
    std::lock_guard<std::mutex> lk(stats_mutex_);
    return stats_;
}

// ---------------------------------------------------------------------------
// Per-link (srtla_rec) stats polling
// ---------------------------------------------------------------------------

void RelayClient::PollPerLinkStats(const std::string& relay_ip)
{
    if (relay_ip.empty()) {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        per_link_.links.clear();
        return;
    }

    std::string host_port = relay_ip + ":5080";
    std::wstring host_w(host_port.begin(), host_port.end());
    std::wstring path_w = L"/stats";

    HttpResponse resp;
    try {
        resp = http_.Get(host_w, path_w);
    } catch (const std::exception& e) {
        blog(LOG_DEBUG, "[aegis-relay] per-link stats http error: %s", e.what());
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    if (resp.status_code != 200 || resp.body.empty()) {
        blog(LOG_DEBUG, "[aegis-relay] per-link stats failed: status=%lu",
             resp.status_code);
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(resp.body.c_str(), static_cast<int>(resp.body.size())), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray groupsArr = root.value("groups").toArray();

    PerLinkSnapshot snap;
    snap.available = true;

    // We expect one group (single stream). Take the first.
    if (!groupsArr.isEmpty()) {
        QJsonObject group = groupsArr[0].toObject();
        snap.conn_count = group.value("conn_count").toInt(0);
        QJsonArray connsArr = group.value("connections").toArray();

        for (int i = 0; i < connsArr.size(); ++i) {
            QJsonObject c = connsArr[i].toObject();
            PerLinkStats link;
            link.addr = c.value("addr").toString().toStdString();
            link.asn_org = c.value("asn_org").toString().toStdString();
            link.bytes = static_cast<uint64_t>(c.value("bytes").toDouble(0));
            link.pkts = static_cast<uint64_t>(c.value("pkts").toDouble(0));
            link.share_pct = c.value("share_pct").toDouble(0.0);
            link.last_ms_ago = static_cast<uint32_t>(c.value("last_ms_ago").toInt(0));
            link.uptime_s = static_cast<uint32_t>(c.value("uptime_s").toInt(0));
            snap.links.push_back(std::move(link));
        }
    }

    {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_ = std::move(snap);
    }

    blog(LOG_DEBUG, "[aegis-relay] per-link stats ok: conn_count=%d links=%zu",
         per_link_.conn_count, per_link_.links.size());
}

PerLinkSnapshot RelayClient::CurrentPerLinkStats() const
{
    std::lock_guard<std::mutex> lk(per_link_mutex_);
    return per_link_;
}

}  // namespace aegis
