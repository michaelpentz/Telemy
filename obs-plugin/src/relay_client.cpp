#include "relay_client.h"

#include <obs-module.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>

#include <rpc.h>       // UuidCreate, UuidToStringA, RpcStringFreeA
// Link: rpcrt4.lib — added in CMakeLists.txt alongside winhttp and crypt32.

#include <chrono>
#include <utility>
#include <stdexcept>
#include <string>

namespace aegis {

namespace {

std::string JsonStringify(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QJsonObject ParseJsonObject(const std::string& json)
{
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isObject()) {
        return QJsonObject();
    }
    return doc.object();
}

AuthUser ParseAuthUser(const QJsonObject& obj)
{
    AuthUser user;
    user.id = obj.value("id").toString().toStdString();
    user.email = obj.value("email").toString().toStdString();
    user.display_name = obj.value("display_name").toString().toStdString();
    return user;
}

RelayEntitlement ParseRelayEntitlement(const QJsonObject& obj)
{
    RelayEntitlement entitlement;
    entitlement.relay_access_status = obj.value("relay_access_status").toString().toStdString();
    entitlement.reason_code = obj.value("reason_code").toString().toStdString();
    entitlement.plan_tier = obj.value("plan_tier").toString().toStdString();
    entitlement.plan_status = obj.value("plan_status").toString().toStdString();
    entitlement.max_concurrent_conns = obj.value("max_concurrent_conns").toInt(0);
    entitlement.active_managed_conns = obj.value("active_managed_conns").toInt(0);
    return entitlement;
}

UsageSnapshot ParseUsageSnapshot(const QJsonObject& obj)
{
    UsageSnapshot usage;
    usage.included_seconds = obj.value("included_seconds").toInt(0);
    usage.consumed_seconds = obj.value("consumed_seconds").toInt(0);
    usage.remaining_seconds = obj.value("remaining_seconds").toInt(0);
    usage.overage_seconds = obj.value("overage_seconds").toInt(0);
    return usage;
}

QJsonObject SerializeAuthUser(const AuthUser& user)
{
    QJsonObject obj;
    obj["id"] = QString::fromStdString(user.id);
    obj["email"] = QString::fromStdString(user.email);
    obj["display_name"] = QString::fromStdString(user.display_name);
    return obj;
}

QJsonObject SerializeRelayEntitlement(const RelayEntitlement& entitlement)
{
    QJsonObject obj;
    obj["relay_access_status"] = QString::fromStdString(entitlement.relay_access_status);
    obj["reason_code"] = QString::fromStdString(entitlement.reason_code);
    obj["plan_tier"] = QString::fromStdString(entitlement.plan_tier);
    obj["plan_status"] = QString::fromStdString(entitlement.plan_status);
    obj["max_concurrent_conns"] = entitlement.max_concurrent_conns;
    obj["active_managed_conns"] = entitlement.active_managed_conns;
    return obj;
}

QJsonObject SerializeUsageSnapshot(const UsageSnapshot& usage)
{
    QJsonObject obj;
    obj["included_seconds"] = usage.included_seconds;
    obj["consumed_seconds"] = usage.consumed_seconds;
    obj["remaining_seconds"] = usage.remaining_seconds;
    obj["overage_seconds"] = usage.overage_seconds;
    return obj;
}

StreamSlot ParseStreamSlot(const QJsonObject& obj)
{
    StreamSlot slot;
    slot.slot_number = obj.value("slot_number").toInt(0);
    slot.label = obj.value("label").toString().toStdString();
    slot.stream_token = obj.value("stream_token").toString().toStdString();
    return slot;
}

QJsonObject SerializeStreamSlot(const StreamSlot& slot)
{
    QJsonObject obj;
    obj["slot_number"] = slot.slot_number;
    obj["label"] = QString::fromStdString(slot.label);
    obj["stream_token"] = QString::fromStdString(slot.stream_token);
    return obj;
}

std::optional<AuthSessionSnapshot> ParseAuthSessionSnapshotObject(const QJsonObject& root)
{
    if (root.isEmpty()) {
        return std::nullopt;
    }

    AuthSessionSnapshot snapshot;
    snapshot.user = ParseAuthUser(root.value("user").toObject());
    snapshot.entitlement = ParseRelayEntitlement(root.value("entitlement").toObject());
    snapshot.usage = ParseUsageSnapshot(root.value("usage").toObject());

    const QJsonValue activeRelayValue = root.value("active_relay");
    if (activeRelayValue.isObject()) {
        ActiveRelaySummary activeRelay;
        const QJsonObject activeRelayObj = activeRelayValue.toObject();
        activeRelay.session_id = activeRelayObj.value("session_id").toString().toStdString();
        activeRelay.status = activeRelayObj.value("status").toString().toStdString();
        snapshot.active_relay = activeRelay;
    }

    const QJsonValue streamSlotsValue = root.value("stream_slots");
    if (streamSlotsValue.isArray()) {
        const QJsonArray streamSlots = streamSlotsValue.toArray();
        snapshot.stream_slots.reserve(streamSlots.size());
        for (const QJsonValue& value : streamSlots) {
            if (!value.isObject()) {
                continue;
            }
            snapshot.stream_slots.push_back(ParseStreamSlot(value.toObject()));
        }
    }

    if (snapshot.user.id.empty()) {
        return std::nullopt;
    }

    return snapshot;
}

}  // namespace

bool AuthTokens::Empty() const
{
    return cp_access_jwt.empty() && refresh_token.empty();
}

void AuthTokens::Clear()
{
    cp_access_jwt.clear();
    refresh_token.clear();
}

std::string AuthTokens::ToVaultJson() const
{
    QJsonObject obj;
    obj["cp_access_jwt"] = QString::fromStdString(cp_access_jwt);
    obj["refresh_token"] = QString::fromStdString(refresh_token);
    return JsonStringify(obj);
}

std::optional<AuthTokens> AuthTokens::FromVaultJson(const std::string& json)
{
    const QJsonObject obj = ParseJsonObject(json);
    if (obj.isEmpty()) {
        return std::nullopt;
    }

    AuthTokens tokens;
    tokens.cp_access_jwt = obj.value("cp_access_jwt").toString().toStdString();
    tokens.refresh_token = obj.value("refresh_token").toString().toStdString();
    if (tokens.Empty()) {
        return std::nullopt;
    }
    return tokens;
}

std::string AuthSessionSnapshot::ToVaultJson() const
{
    QJsonObject obj;
    obj["user"] = SerializeAuthUser(user);
    obj["entitlement"] = SerializeRelayEntitlement(entitlement);
    obj["usage"] = SerializeUsageSnapshot(usage);
    if (active_relay) {
        QJsonObject activeRelay;
        activeRelay["session_id"] = QString::fromStdString(active_relay->session_id);
        activeRelay["status"] = QString::fromStdString(active_relay->status);
        obj["active_relay"] = activeRelay;
    } else {
        obj["active_relay"] = QJsonValue(QJsonValue::Null);
    }

    QJsonArray streamSlots;
    for (const StreamSlot& slot : stream_slots) {
        streamSlots.append(SerializeStreamSlot(slot));
    }
    obj["stream_slots"] = streamSlots;
    return JsonStringify(obj);
}

std::optional<AuthSessionSnapshot> AuthSessionSnapshot::FromVaultJson(const std::string& json)
{
    return ParseAuthSessionSnapshotObject(ParseJsonObject(json));
}

bool PluginLoginAttempt::Empty() const
{
    return login_attempt_id.empty() && authorize_url.empty() && poll_token.empty();
}

void PluginLoginAttempt::Clear()
{
    login_attempt_id.clear();
    authorize_url.clear();
    poll_token.clear();
    expires_at.clear();
    poll_interval_seconds = 3;
}

void PluginAuthState::ClearAuthMaterial()
{
    tokens.Clear();
    authenticated = false;
}

void PluginAuthState::ClearLoginAttempt()
{
    login_attempt.Clear();
    last_error_code.clear();
    last_error_message.clear();
}

std::string PluginAuthState::ToVaultJson() const
{
    QJsonObject obj;
    obj["authenticated"] = authenticated;
    obj["tokens"] = ParseJsonObject(tokens.ToVaultJson());
    obj["session"] = ParseJsonObject(session.ToVaultJson());

    QJsonObject loginAttempt;
    loginAttempt["login_attempt_id"] = QString::fromStdString(login_attempt.login_attempt_id);
    loginAttempt["authorize_url"] = QString::fromStdString(login_attempt.authorize_url);
    loginAttempt["poll_token"] = QString::fromStdString(login_attempt.poll_token);
    loginAttempt["expires_at"] = QString::fromStdString(login_attempt.expires_at);
    loginAttempt["poll_interval_seconds"] = login_attempt.poll_interval_seconds;
    obj["login_attempt"] = loginAttempt;

    obj["last_error_code"] = QString::fromStdString(last_error_code);
    obj["last_error_message"] = QString::fromStdString(last_error_message);
    return JsonStringify(obj);
}

std::optional<PluginAuthState> PluginAuthState::FromVaultJson(const std::string& json)
{
    const QJsonObject obj = ParseJsonObject(json);
    if (obj.isEmpty()) {
        return std::nullopt;
    }

    PluginAuthState state;
    state.authenticated = obj.value("authenticated").toBool(false);
    if (obj.value("tokens").isObject()) {
        const auto tokens = AuthTokens::FromVaultJson(JsonStringify(obj.value("tokens").toObject()));
        if (tokens) {
            state.tokens = *tokens;
        }
    }
    if (obj.value("session").isObject()) {
        const auto session = AuthSessionSnapshot::FromVaultJson(JsonStringify(obj.value("session").toObject()));
        if (session) {
            state.session = *session;
        }
    }
    if (obj.value("login_attempt").isObject()) {
        const QJsonObject loginAttempt = obj.value("login_attempt").toObject();
        state.login_attempt.login_attempt_id = loginAttempt.value("login_attempt_id").toString().toStdString();
        state.login_attempt.authorize_url = loginAttempt.value("authorize_url").toString().toStdString();
        state.login_attempt.poll_token = loginAttempt.value("poll_token").toString().toStdString();
        state.login_attempt.expires_at = loginAttempt.value("expires_at").toString().toStdString();
        state.login_attempt.poll_interval_seconds = loginAttempt.value("poll_interval_seconds").toInt(3);
    }
    state.last_error_code = obj.value("last_error_code").toString().toStdString();
    state.last_error_message = obj.value("last_error_message").toString().toStdString();
    return state;
}

ControlPlaneAuthClient::ControlPlaneAuthClient(HttpsClient& http, const std::string& api_host)
    : http_(http)
    , api_host_w_(api_host.begin(), api_host.end())
{
}

void ControlPlaneAuthClient::Reconfigure(const std::string& api_host)
{
    api_host_w_ = std::wstring(api_host.begin(), api_host.end());
}

std::vector<std::pair<std::wstring, std::wstring>> ControlPlaneAuthClient::CommonClientHeaders()
{
    return {
        {L"X-Aegis-Client-Version", L"0.0.5"},
        {L"X-Aegis-Client-Platform", L"windows"},
    };
}

std::optional<AuthSessionSnapshot> ControlPlaneAuthClient::ParseAuthSessionSnapshot(const std::string& json)
{
    return ParseAuthSessionSnapshotObject(ParseJsonObject(json));
}

std::optional<AuthSessionSnapshot> ControlPlaneAuthClient::GetSession(const std::string& cp_access_jwt)
{
    std::wstring wideJwt(cp_access_jwt.begin(), cp_access_jwt.end());
    HttpResponse resp;
    try {
        resp = http_.Get(api_host_w_, L"/api/v1/auth/session", wideJwt, CommonClientHeaders());
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "auth: GetSession network error: %s", e.what());
        return std::nullopt;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "auth: GetSession failed, HTTP %lu: %s", resp.status_code, resp.body.c_str());
        return std::nullopt;
    }
    return ParseAuthSessionSnapshot(resp.body);
}

std::optional<PluginLoginAttempt> ControlPlaneAuthClient::StartPluginLogin(const std::string& device_name,
                                                                            const std::string& plugin_version,
                                                                            const std::string& platform)
{
    QJsonObject clientObj;
    clientObj["platform"] = QString::fromStdString(platform);
    clientObj["plugin_version"] = QString::fromStdString(plugin_version);
    clientObj["device_name"] = QString::fromStdString(device_name);

    QJsonObject bodyObj;
    bodyObj["client"] = clientObj;

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/auth/plugin/login/start", JsonStringify(bodyObj), L"", CommonClientHeaders());
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "auth: StartPluginLogin network error: %s", e.what());
        return std::nullopt;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "auth: StartPluginLogin failed, HTTP %lu: %s", resp.status_code, resp.body.c_str());
        return std::nullopt;
    }

    const QJsonObject obj = ParseJsonObject(resp.body);
    if (obj.isEmpty()) {
        return std::nullopt;
    }

    PluginLoginAttempt attempt;
    attempt.login_attempt_id = obj.value("login_attempt_id").toString().toStdString();
    attempt.authorize_url = obj.value("authorize_url").toString().toStdString();
    attempt.poll_token = obj.value("poll_token").toString().toStdString();
    attempt.expires_at = obj.value("expires_at").toString().toStdString();
    attempt.poll_interval_seconds = obj.value("poll_interval_seconds").toInt(3);
    if (attempt.login_attempt_id.empty() || attempt.poll_token.empty()) {
        return std::nullopt;
    }
    return attempt;
}

std::optional<AuthPollResult> ControlPlaneAuthClient::PollPluginLogin(const std::string& login_attempt_id,
                                                                      const std::string& poll_token)
{
    QJsonObject bodyObj;
    bodyObj["login_attempt_id"] = QString::fromStdString(login_attempt_id);
    bodyObj["poll_token"] = QString::fromStdString(poll_token);

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/auth/plugin/login/poll", JsonStringify(bodyObj), L"", CommonClientHeaders());
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "auth: PollPluginLogin network error: %s", e.what());
        return std::nullopt;
    }

    AuthPollResult result;
    if (resp.status_code == 202) {
        result.status = AuthPollStatus::Pending;
        return result;
    }
    if (resp.status_code == 410) {
        const QJsonObject obj = ParseJsonObject(resp.body);
        result.status = AuthPollStatus::Expired;
        result.reason_code = obj.value("error").toObject().value("code").toString().toStdString();
        if (result.reason_code.empty()) {
            result.reason_code = "login_attempt_expired";
        }
        return result;
    }
    if (resp.status_code == 403) {
        result.status = AuthPollStatus::Denied;
    } else if (!resp.ok()) {
        blog(LOG_WARNING, "auth: PollPluginLogin failed, HTTP %lu: %s", resp.status_code, resp.body.c_str());
        return std::nullopt;
    }

    const QJsonObject obj = ParseJsonObject(resp.body);
    if (obj.isEmpty()) {
        return std::nullopt;
    }

    if (result.status == AuthPollStatus::Denied) {
        const QJsonObject errorObj = obj.value("error").toObject();
        result.reason_code = errorObj.value("reason_code").toString().toStdString();
        if (result.reason_code.empty()) {
            result.reason_code = errorObj.value("code").toString().toStdString();
        }
        if (result.reason_code.empty()) {
            result.reason_code = "login_denied";
        }
        return result;
    }

    result.status = AuthPollStatus::Completed;
    const QJsonObject authObj = obj.value("auth").toObject();
    result.tokens.cp_access_jwt = authObj.value("cp_access_jwt").toString().toStdString();
    result.tokens.refresh_token = authObj.value("refresh_token").toString().toStdString();
    result.session = ParseAuthSessionSnapshotObject(obj);
    if (result.tokens.Empty() || !result.session) {
        return std::nullopt;
    }
    return result;
}

std::optional<AuthPollResult> ControlPlaneAuthClient::Refresh(const std::string& refresh_token)
{
    QJsonObject bodyObj;
    bodyObj["refresh_token"] = QString::fromStdString(refresh_token);

    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/auth/refresh", JsonStringify(bodyObj), L"", CommonClientHeaders());
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "auth: Refresh network error: %s", e.what());
        return std::nullopt;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "auth: Refresh failed, HTTP %lu: %s", resp.status_code, resp.body.c_str());
        return std::nullopt;
    }

    const QJsonObject obj = ParseJsonObject(resp.body);
    if (obj.isEmpty()) {
        return std::nullopt;
    }

    AuthPollResult result;
    result.status = AuthPollStatus::Completed;
    const QJsonObject authObj = obj.value("auth").toObject();
    result.tokens.cp_access_jwt = authObj.value("cp_access_jwt").toString().toStdString();
    result.tokens.refresh_token = authObj.value("refresh_token").toString().toStdString();
    result.session = ParseAuthSessionSnapshotObject(obj);
    if (result.tokens.Empty() || !result.session) {
        return std::nullopt;
    }
    return result;
}

bool ControlPlaneAuthClient::Logout(const std::string& cp_access_jwt)
{
    std::wstring wideJwt(cp_access_jwt.begin(), cp_access_jwt.end());
    HttpResponse resp;
    try {
        resp = http_.Post(api_host_w_, L"/api/v1/auth/logout", "{}", wideJwt, CommonClientHeaders());
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "auth: Logout network error: %s", e.what());
        return false;
    }

    if (resp.status_code == 204 || resp.ok()) {
        return true;
    }

    blog(LOG_WARNING, "auth: Logout failed, HTTP %lu: %s", resp.status_code, resp.body.c_str());
    return false;
}

bool ControlPlaneAuthClient::UpdateStreamSlotLabel(const std::string& cp_access_jwt,
                                                   int slot_number,
                                                   const std::string& label)
{
    if (slot_number < 0) {
        return false;
    }

    QJsonObject bodyObj;
    bodyObj["label"] = QString::fromStdString(label);

    const std::wstring path = L"/api/v1/user/stream-slots/" +
                              std::to_wstring(slot_number) +
                              L"/label";
    std::wstring wide_jwt(cp_access_jwt.begin(), cp_access_jwt.end());

    HttpResponse resp;
    try {
        resp = http_.Put(api_host_w_, path, JsonStringify(bodyObj), wide_jwt, CommonClientHeaders());
    } catch (const std::exception& e) {
        blog(LOG_WARNING, "auth: UpdateStreamSlotLabel network error: %s", e.what());
        return false;
    }

    if (!resp.ok()) {
        blog(LOG_WARNING, "auth: UpdateStreamSlotLabel failed, HTTP %lu: %s",
             resp.status_code, resp.body.c_str());
        return false;
    }

    return true;
}

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
    session.srt_port = relayObj["srt_port"].toInt(5000);
    session.relay_hostname = relayObj["relay_hostname"].toString().toStdString();

    // Credentials.
    QJsonObject creds = obj["credentials"].toObject();
    session.pair_token = creds["pair_token"].toString().toStdString();
    session.stream_token = creds["stream_token"].toString().toStdString();

    // Timers.
    QJsonObject timers = obj["timers"].toObject();
    session.grace_window_seconds = timers["grace_window_seconds"].toInt(0);
    session.max_session_seconds = timers["max_session_seconds"].toInt(0);
    session.provision_step = obj["provision_step"].toString().toStdString();
    session.instance_id = obj["instance_id"].toString().toStdString();

    // Reject if session_id is empty — indicates a malformed response.
    if (session.session_id.empty()) {
        return std::nullopt;
    }

    return session;
}

void RelayClient::ClearStatsSnapshots()
{
    {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_ = RelayStats{};
    }
    {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_ = PerLinkSnapshot{};
    }
}

void RelayClient::StoreJWT(const std::string& jwt)
{
    std::lock_guard<std::mutex> lock(jwt_mutex_);
    stored_jwt_ = jwt;
}

std::string RelayClient::GetStoredJWT() const
{
    std::lock_guard<std::mutex> lock(jwt_mutex_);
    return stored_jwt_;
}

// ---------------------------------------------------------------------------
// Control plane calls
// ---------------------------------------------------------------------------

std::optional<RelaySession> RelayClient::GetActive(const std::string& jwt,
                                                   const std::string& expected_session_id,
                                                   const std::string& expected_stream_token)
{
    StoreJWT(jwt);

    std::string path_str = "/api/v1/relay/active";
    {
        std::lock_guard<std::mutex> lock(connection_id_mutex_);
        if (!connection_id_.empty()) {
            path_str += "?connection_id=" + connection_id_;
        }
    }
    std::wstring path(path_str.begin(), path_str.end());
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

    // Store JWT for later use by stats polling loop.
    StoreJWT(jwt);

    auto session = ParseSessionResponse(resp.body);
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(resp.body));
    QJsonObject root = doc.object();
    QJsonObject sessionObj = root.contains("session") && root["session"].isObject()
                                 ? root["session"].toObject()
                                 : root;
    if (session) {
        if (sessionObj.contains("sender_url")) {
            session->sender_url = sessionObj["sender_url"].toString().toStdString();
        }
        if (sessionObj.contains("media_source_url")) {
            session->media_source_url = sessionObj["media_source_url"].toString().toStdString();
        }
    }
    if (session) {
        bool expect_match = false;
        bool matched = false;
        if (!expected_session_id.empty()) {
            expect_match = true;
            if (session->session_id == expected_session_id) {
                matched = true;
            }
        }
        if (!expected_stream_token.empty()) {
            expect_match = true;
            if (session->stream_token == expected_stream_token) {
                matched = true;
            }
        }
        if (expect_match && !matched) {
            blog(LOG_DEBUG,
                 "relay: GetActive ignoring non-matching session sid=%s token=%s"
                 " expected_sid=%s expected_token=%s",
                 session->session_id.c_str(),
                 session->stream_token.c_str(),
                 expected_session_id.c_str(),
                 expected_stream_token.c_str());
            return std::nullopt;
        }
    }
    if (session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = session;
        byor_mode_ = false;
    }

    // Parse per_link data from the API response if present.
    // The per_link object lives inside the "session" wrapper in the JSON.
    {
        // per_link is at root level in the API response (sibling of "session"), not inside it.
        QJsonObject perLinkSource = root.contains("per_link") ? root : QJsonObject{};
        if (!perLinkSource.contains("per_link")) {
            // Also check inside the session object as a fallback.
            if (sessionObj.contains("per_link")) {
                perLinkSource = sessionObj;
            }
        }
        if (perLinkSource.contains("per_link") && perLinkSource["per_link"].isObject()) {
            QJsonObject plObj = perLinkSource["per_link"].toObject();
            PerLinkSnapshot snap;
            snap.available = true;
            snap.stream_id = plObj.value("stream_id").toString().toStdString();
            QJsonArray connsArr = plObj.value("connections").toArray();
            snap.conn_count = connsArr.size();
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
            {
                std::lock_guard<std::mutex> lk(per_link_mutex_);
                per_link_ = std::move(snap);
            }
            blog(LOG_DEBUG, "[aegis-relay] per-link from API: stream_id=%s links=%d",
                 per_link_.stream_id.c_str(), per_link_.conn_count);
        }
    }

    return session;
}

std::optional<RelaySession> RelayClient::Start(const std::string& jwt)
{
    // Store JWT for later use by stats polling loop.
    StoreJWT(jwt);

    std::string uuid = GenerateUuidV4();
    QJsonObject clientContext;
    clientContext["obs_connected"] = true;
    clientContext["mode"] = QStringLiteral("auto");
    clientContext["requested_by"] = QStringLiteral("obs-plugin");

    QJsonObject bodyObj;
    bodyObj["client_context"] = clientContext;
    {
        std::lock_guard<std::mutex> lock(pending_managed_start_mutex_);
        bodyObj["region_preference"] =
            QString::fromStdString(pending_managed_region_preference_);
        if (!pending_managed_connection_id_.empty()) {
            bodyObj["connection_id"] =
                QString::fromStdString(pending_managed_connection_id_);
        }
        if (!pending_managed_stream_token_.empty()) {
            bodyObj["stream_token"] =
                QString::fromStdString(pending_managed_stream_token_);
        }
        pending_managed_connection_id_.clear();
        pending_managed_region_preference_.clear();
        pending_managed_stream_slot_number_ = 0;
        pending_managed_stream_token_.clear();
    }
    const std::string body = JsonStringify(bodyObj);
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
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(resp.body));
        QJsonObject root = doc.object();
        if (root.contains("session") && root["session"].isObject()) {
            root = root["session"].toObject();
        }
        if (root.contains("sender_url")) {
            session->sender_url = root["sender_url"].toString().toStdString();
        }
        if (root.contains("media_source_url")) {
            session->media_source_url = root["media_source_url"].toString().toStdString();
        }
    }
    if (session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = session;
        byor_mode_ = false;
    }
    return session;
}

void RelayClient::ConfigureNextManagedStart(const std::string& connection_id,
                                            const std::string& region_preference,
                                            int stream_slot_number,
                                            const std::string& stream_token)
{
    {
        std::lock_guard<std::mutex> lock(connection_id_mutex_);
        connection_id_ = connection_id;
    }
    std::lock_guard<std::mutex> lock(pending_managed_start_mutex_);
    pending_managed_connection_id_ = connection_id;
    pending_managed_region_preference_ = region_preference;
    pending_managed_stream_slot_number_ = stream_slot_number;
    pending_managed_stream_token_ = stream_token;
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
        byor_mode_ = false;
        ClearStatsSnapshots();
        blog(LOG_INFO, "relay: session %s stopped", session_id.c_str());
    } else {
        blog(LOG_WARNING, "relay: Stop failed, HTTP %lu: %s",
             resp.status_code, resp.body.c_str());
    }
    return resp.ok();
}

void RelayClient::ConnectDirect(const std::string& relay_host,
                                int relay_port,
                                const std::string& stream_token)
{
    StopHeartbeatLoop();

    RelaySession session;
    session.session_id = "byor-local-" + GenerateUuidV4();
    session.status = "active";
    session.public_ip = relay_host;
    session.srt_port = relay_port;
    session.relay_hostname = relay_host;
    session.stream_token = stream_token;

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = session;
        byor_mode_ = true;
    }
    ClearStatsSnapshots();
    blog(LOG_INFO, "relay: connected direct BYOR host=%s port=%d",
         relay_host.c_str(), relay_port);
}

void RelayClient::ConnectDirect(const BYORConfig& config, const std::string& stream_token)
{
    const std::string effective_stream_id =
        config.stream_id.empty() ? stream_token : config.stream_id;
    ConnectDirect(config.relay_host, config.relay_port, effective_stream_id);
}

void RelayClient::DisconnectDirect()
{
    StopHeartbeatLoop();
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        current_session_ = std::nullopt;
        byor_mode_ = false;
    }
    ClearStatsSnapshots();
    blog(LOG_INFO, "relay: disconnected direct BYOR session");
}

bool RelayClient::IsBYORMode() const
{
    return byor_mode_.load();
}

bool RelayClient::SendHeartbeat(const std::string& jwt, const std::string& session_id)
{
    // Fetch instance_id from stored session for the backend's identity binding check.
    std::string instance_id;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (current_session_) {
            instance_id = current_session_->instance_id;
        }
    }
    std::string body = "{\"session_id\":\"" + session_id
                     + "\",\"instance_id\":\"" + instance_id + "\"}";
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
        byor_mode_ = false;
        ClearStatsSnapshots();
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
    heartbeat_consecutive_failures_ = 0;
    heartbeat_current_interval_sec_ = interval_sec;

    heartbeat_thread_ = std::thread([this, jwt, session_id, interval_sec]() {
        blog(LOG_INFO, "relay: heartbeat loop started (session %s, interval %ds)",
             session_id.c_str(), interval_sec);

        while (heartbeat_running_) {
            // Wait for the current interval, but wake early if StopHeartbeatLoop() signals.
            {
                std::unique_lock<std::mutex> lock(heartbeat_cv_mutex_);
                heartbeat_cv_.wait_for(lock,
                    std::chrono::seconds(heartbeat_current_interval_sec_),
                    [this] { return !heartbeat_running_.load(); });
            }

            if (!heartbeat_running_) {
                break;
            }

            try {
                if (SendHeartbeat(jwt, session_id)) {
                    heartbeat_consecutive_failures_ = 0;
                    heartbeat_current_interval_sec_ = interval_sec;
                } else {
                    // Check if this was a terminal 404 (session expired) —
                    // SendHeartbeat clears current_session_ on 404.
                    bool session_expired;
                    {
                        std::lock_guard<std::mutex> lock(session_mutex_);
                        session_expired = !current_session_.has_value();
                    }
                    if (session_expired) {
                        blog(LOG_WARNING, "relay: heartbeat session expired (404), stopping loop");
                        heartbeat_running_ = false;
                    } else {
                        ++heartbeat_consecutive_failures_;
                        if (heartbeat_consecutive_failures_ >= kHeartbeatBackoffThreshold) {
                            heartbeat_current_interval_sec_ = std::min(
                                heartbeat_current_interval_sec_ * 2, kHeartbeatMaxIntervalSec);
                            blog(LOG_WARNING,
                                 "relay: heartbeat failed %d consecutive times, backing off to %ds",
                                 heartbeat_consecutive_failures_, heartbeat_current_interval_sec_);
                        } else {
                            blog(LOG_WARNING,
                                 "relay: heartbeat transient failure (%d/%d), will retry in %ds",
                                 heartbeat_consecutive_failures_,
                                 kHeartbeatBackoffThreshold,
                                 heartbeat_current_interval_sec_);
                        }
                    }
                }
            } catch (const std::exception& e) {
                ++heartbeat_consecutive_failures_;
                if (heartbeat_consecutive_failures_ >= kHeartbeatBackoffThreshold) {
                    heartbeat_current_interval_sec_ = std::min(
                        heartbeat_current_interval_sec_ * 2, kHeartbeatMaxIntervalSec);
                    blog(LOG_WARNING,
                         "relay: heartbeat exception (%d failures, backoff %ds): %s",
                         heartbeat_consecutive_failures_, heartbeat_current_interval_sec_, e.what());
                } else {
                    blog(LOG_WARNING,
                         "relay: heartbeat exception (%d/%d), will retry in %ds: %s",
                         heartbeat_consecutive_failures_,
                         kHeartbeatBackoffThreshold, heartbeat_current_interval_sec_, e.what());
                }
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
// Reconfigure — hot-swap api_host and relay_shared_key so the user does not
// need to restart OBS after changing relay settings in the dock.
// ---------------------------------------------------------------------------

void RelayClient::Reconfigure(const std::string& api_host,
                               const std::string& relay_shared_key)
{
    StopHeartbeatLoop();

    api_host_w_ = std::wstring(api_host.begin(), api_host.end());
    relay_shared_key_w_ = std::wstring(relay_shared_key.begin(), relay_shared_key.end());
    logged_missing_health_shared_key_ = false;

    blog(LOG_INFO, "relay: reconfigured api_host=%s", api_host.c_str());
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

    if (IsBYORMode()) {
        DisconnectDirect();
        return;
    }

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
    const bool is_byor = byor_mode_.load();
    if (relay_ip.empty()) {
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    // Build wide host string with SLS stats port (plaintext — relay-local endpoint).
    std::string host_port = "http://" + relay_ip + ":8090";
    std::wstring host_w(host_port.begin(), host_port.end());
    std::string stream_token;
    {
        std::lock_guard<std::mutex> lk(session_mutex_);
        if (current_session_)
            stream_token = current_session_->stream_token;
    }
    std::string stream_id = stream_token.empty() ? std::string("play_aegis") : stream_token;
    if (!stream_id.empty() &&
        stream_id.find('/') == std::string::npos &&
        stream_id.rfind("live_", 0) != 0 &&
        stream_id.rfind("play_", 0) != 0) {
        stream_id = "play_" + stream_id;
    }
    std::wstring stream_id_w(stream_id.begin(), stream_id.end());
    std::wstring path_w = L"/stats/" + stream_id_w + L"?legacy=1";

    HttpResponse resp;
    try {
        resp = http_.Get(host_w, path_w);
    } catch (const std::exception& e) {
        if (!is_byor) {
            blog(LOG_DEBUG, "[aegis-relay] stats poll http error: %s", e.what());
        }
        std::lock_guard<std::mutex> lk(stats_mutex_);
        stats_.available = false;
        return;
    }

    if (resp.status_code != 200 || resp.body.empty()) {
        if (!is_byor) {
            blog(LOG_DEBUG, "[aegis-relay] stats poll failed: status=%lu",
                 resp.status_code);
        }
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
    s.pkt_recv         = static_cast<uint64_t>(pub.value("pktRecv").toDouble(0));
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

void RelayClient::PollPerLinkStats(const std::string& relay_ip, const std::string& filter_stream_id)
{
    const bool is_byor = byor_mode_.load();
    if (relay_ip.empty()) {
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        per_link_.links.clear();
        return;
    }

    std::string host_port = "http://" + relay_ip + ":5080";
    std::wstring host_w(host_port.begin(), host_port.end());
    std::wstring path_w = L"/stats";

    HttpResponse resp;
    try {
        resp = http_.Get(host_w, path_w);
    } catch (const std::exception& e) {
        if (!is_byor) {
            blog(LOG_DEBUG, "[aegis-relay] per-link stats http error: %s", e.what());
        }
        std::lock_guard<std::mutex> lk(per_link_mutex_);
        per_link_.available = false;
        return;
    }

    if (resp.status_code != 200 || resp.body.empty()) {
        if (!is_byor) {
            blog(LOG_DEBUG, "[aegis-relay] per-link stats failed: status=%lu",
                 resp.status_code);
        }
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

    // Find the matching group. When filter_stream_id is set, only include the
    // group whose "stream_id" field matches. When empty (BYOR / backward compat),
    // take the first group as before.
    QJsonObject matched_group;
    bool found = false;
    for (int g = 0; g < groupsArr.size(); ++g) {
        QJsonObject group = groupsArr[g].toObject();
        if (filter_stream_id.empty()) {
            // No filter — take the first group (backward compat).
            matched_group = group;
            found = true;
            break;
        }
        std::string group_stream_id = group.value("stream_id").toString().toStdString();
        if (group_stream_id == filter_stream_id) {
            matched_group = group;
            found = true;
            break;
        }
    }

    if (found) {
        snap.stream_id = matched_group.value("stream_id").toString().toStdString();
        snap.conn_count = matched_group.value("conn_count").toInt(0);
        QJsonArray connsArr = matched_group.value("connections").toArray();

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
