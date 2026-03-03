#include "relay_client.h"

#include <obs-module.h>

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
RelayClient::RelayClient(HttpsClient& http, const std::string& api_host)
    : http_(http)
    , api_host_w_(api_host.begin(), api_host.end())
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

    RelaySession session;
    session.session_id = obj["session_id"].toString().toStdString();
    session.status     = obj["status"].toString().toStdString();
    session.region     = obj["region"].toString().toStdString();

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

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/relay/start", body, wide_jwt);
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

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/relay/health", body, wide_jwt);
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

}  // namespace aegis
