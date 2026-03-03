#include "https_client.h"

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <stdexcept>
#include <sstream>
#include <vector>

// OBS logging — available in plugin builds; silently omitted in unit tests
// that define AEGIS_NO_OBS_LOG.
#ifndef AEGIS_NO_OBS_LOG
#include <obs-module.h>
#define AEGIS_LOG_WARN(msg) blog(LOG_WARNING, "https: %s", (msg))
#else
#define AEGIS_LOG_WARN(msg) ((void)0)
#endif

namespace {

// Helper: build a std::runtime_error with the failing function name and the
// GetLastError() code baked into the message.
[[noreturn]] void throw_winhttp_error(const char* fn_name, DWORD err) {
    std::ostringstream ss;
    ss << fn_name << " failed (WinHTTP error " << err << ")";
    std::string msg = ss.str();
    AEGIS_LOG_WARN(msg.c_str());
    throw std::runtime_error(msg);
}

// Helper: parse a host string that may include ":port" suffix.
// Returns the bare hostname and resolved port.  When no port is present
// the caller-supplied default_port is used.
struct HostPort {
    std::wstring host;
    INTERNET_PORT port;
    bool use_tls;
};

static HostPort parse_host_port(const std::wstring& raw) {
    HostPort hp;
    auto colon = raw.rfind(L':');
    if (colon != std::wstring::npos && colon > 0) {
        std::wstring port_str = raw.substr(colon + 1);
        int port_val = 0;
        try { port_val = std::stoi(port_str); } catch (...) { port_val = 0; }
        if (port_val > 0 && port_val <= 65535) {
            hp.host = raw.substr(0, colon);
            hp.port = static_cast<INTERNET_PORT>(port_val);
            hp.use_tls = (hp.port == 443);
            return hp;
        }
    }
    // No port suffix — default to HTTPS on 443.
    hp.host = raw;
    hp.port = INTERNET_DEFAULT_HTTPS_PORT;
    hp.use_tls = true;
    return hp;
}

}  // namespace

namespace aegis {

// ---------------------------------------------------------------------------
// WinHttpHandle destructor — defined here so HINTERNET is in scope.
// ---------------------------------------------------------------------------
WinHttpHandle::~WinHttpHandle()
{
    if (h) {
        WinHttpCloseHandle(reinterpret_cast<HINTERNET>(h));
        h = nullptr;
    }
}

// ---------------------------------------------------------------------------
// HttpsClient — constructor / destructor
// ---------------------------------------------------------------------------
HttpsClient::HttpsClient()
{
    HINTERNET s = WinHttpOpen(
        L"Telemy/0.0.4",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!s) {
        throw_winhttp_error("WinHttpOpen", GetLastError());
    }

    // Timeouts: resolve=5s, connect=5s, send=10s, receive=30s
    if (!WinHttpSetTimeouts(s, 5000, 5000, 10000, 30000)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(s);
        throw_winhttp_error("WinHttpSetTimeouts", err);
    }

    session_ = reinterpret_cast<void*>(s);
}

HttpsClient::~HttpsClient()
{
    if (session_) {
        WinHttpCloseHandle(reinterpret_cast<HINTERNET>(session_));
        session_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Internal helper: read the full response body and populate HttpResponse.
// The request handle must already have received a successful WinHttpReceiveResponse.
// ---------------------------------------------------------------------------
static HttpResponse read_response(HINTERNET request)
{
    HttpResponse resp;

    // Query the numeric status code.
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders(STATUS_CODE)", GetLastError());
    }
    resp.status_code = static_cast<unsigned long>(status_code);

    // Read the body in chunks.
    DWORD bytes_available = 0;
    do {
        bytes_available = 0;
        if (!WinHttpQueryDataAvailable(request, &bytes_available)) {
            throw_winhttp_error("WinHttpQueryDataAvailable", GetLastError());
        }

        if (bytes_available == 0) {
            break;
        }

        std::vector<char> buffer(bytes_available, '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, buffer.data(), bytes_available, &bytes_read)) {
            throw_winhttp_error("WinHttpReadData", GetLastError());
        }

        resp.body.append(buffer.data(), bytes_read);

    } while (bytes_available > 0);

    return resp;
}

// ---------------------------------------------------------------------------
// GET
// ---------------------------------------------------------------------------
HttpResponse HttpsClient::Get(const std::wstring& host,
                               const std::wstring& path,
                               const std::wstring& bearer_token)
{
    auto hp = parse_host_port(host);

    // 1. Open a connection to the host.
    WinHttpHandle connect;
    connect.h = reinterpret_cast<void*>(
        WinHttpConnect(
            reinterpret_cast<HINTERNET>(session_),
            hp.host.c_str(),
            hp.port,
            0));
    if (!connect.valid()) {
        throw_winhttp_error("WinHttpConnect", GetLastError());
    }

    // 2. Open a GET request (TLS only when port is 443).
    WinHttpHandle request;
    request.h = reinterpret_cast<void*>(
        WinHttpOpenRequest(
            reinterpret_cast<HINTERNET>(connect.h),
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            hp.use_tls ? WINHTTP_FLAG_SECURE : 0));
    if (!request.valid()) {
        throw_winhttp_error("WinHttpOpenRequest(GET)", GetLastError());
    }

    // 3. Add Authorization header if a token was provided.
    if (!bearer_token.empty()) {
        std::wstring auth_header = L"Authorization: Bearer " + bearer_token;
        if (!WinHttpAddRequestHeaders(
                reinterpret_cast<HINTERNET>(request.h),
                auth_header.c_str(),
                static_cast<DWORD>(-1L),
                WINHTTP_ADDREQ_FLAG_ADD)) {
            throw_winhttp_error("WinHttpAddRequestHeaders(Authorization/GET)", GetLastError());
        }
    }

    // 4. Send the request (no body for GET).
    if (!WinHttpSendRequest(
            reinterpret_cast<HINTERNET>(request.h),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0)) {
        throw_winhttp_error("WinHttpSendRequest(GET)", GetLastError());
    }

    // 5. Receive the response headers.
    if (!WinHttpReceiveResponse(reinterpret_cast<HINTERNET>(request.h), nullptr)) {
        throw_winhttp_error("WinHttpReceiveResponse(GET)", GetLastError());
    }

    // 6. Read status + body.
    return read_response(reinterpret_cast<HINTERNET>(request.h));
}

// ---------------------------------------------------------------------------
// POST
// ---------------------------------------------------------------------------
HttpResponse HttpsClient::Post(const std::wstring& host,
                                 const std::wstring& path,
                                 const std::string& json_body,
                                 const std::wstring& bearer_token,
                                 const std::vector<std::pair<std::wstring, std::wstring>>& extra_headers)
{
    auto hp = parse_host_port(host);

#ifndef AEGIS_NO_OBS_LOG
    {
        // Convert wide host to narrow for logging.
        int n = WideCharToMultiByte(CP_UTF8, 0, hp.host.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(n > 0 ? n : 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, hp.host.c_str(), -1, narrow.data(), n, nullptr, nullptr);
        blog(LOG_INFO, "https: POST connect host=%s port=%d tls=%d body_len=%d",
             narrow.c_str(), (int)hp.port, (int)hp.use_tls, (int)json_body.size());
    }
#endif

    // 1. Open a connection to the host.
    WinHttpHandle connect;
    connect.h = reinterpret_cast<void*>(
        WinHttpConnect(
            reinterpret_cast<HINTERNET>(session_),
            hp.host.c_str(),
            hp.port,
            0));
    if (!connect.valid()) {
        throw_winhttp_error("WinHttpConnect", GetLastError());
    }

    // 2. Open a POST request (TLS only when port is 443).
    WinHttpHandle request;
    request.h = reinterpret_cast<void*>(
        WinHttpOpenRequest(
            reinterpret_cast<HINTERNET>(connect.h),
            L"POST",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            hp.use_tls ? WINHTTP_FLAG_SECURE : 0));
    if (!request.valid()) {
        throw_winhttp_error("WinHttpOpenRequest(POST)", GetLastError());
    }

    // 3. Add Authorization header if a token was provided.
    if (!bearer_token.empty()) {
        std::wstring auth_header = L"Authorization: Bearer " + bearer_token;
        if (!WinHttpAddRequestHeaders(
                reinterpret_cast<HINTERNET>(request.h),
                auth_header.c_str(),
                static_cast<DWORD>(-1L),
                WINHTTP_ADDREQ_FLAG_ADD)) {
            throw_winhttp_error("WinHttpAddRequestHeaders(Authorization/POST)", GetLastError());
        }
    }

    // 4. Add Content-Type header.
    if (!WinHttpAddRequestHeaders(
            reinterpret_cast<HINTERNET>(request.h),
            L"Content-Type: application/json",
            static_cast<DWORD>(-1L),
            WINHTTP_ADDREQ_FLAG_ADD)) {
        throw_winhttp_error("WinHttpAddRequestHeaders(Content-Type)", GetLastError());
    }

    for (const auto& kv : extra_headers) {
        if (kv.first.empty()) {
            continue;
        }
        std::wstring header_line = kv.first + L": " + kv.second;
        if (!WinHttpAddRequestHeaders(
                reinterpret_cast<HINTERNET>(request.h),
                header_line.c_str(),
                static_cast<DWORD>(-1L),
                WINHTTP_ADDREQ_FLAG_ADD)) {
            throw_winhttp_error("WinHttpAddRequestHeaders(Extra)", GetLastError());
        }
    }

    // 5. Send the request with the JSON body.
    //    WinHttpSendRequest takes a non-const void* for the optional body, so
    //    we const_cast the data pointer (WinHTTP does not modify the buffer).
    DWORD body_len = static_cast<DWORD>(json_body.size());
    if (!WinHttpSendRequest(
            reinterpret_cast<HINTERNET>(request.h),
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            const_cast<void*>(reinterpret_cast<const void*>(json_body.data())),
            body_len,
            body_len,
            0)) {
        throw_winhttp_error("WinHttpSendRequest(POST)", GetLastError());
    }

    // 6. Receive the response headers.
    if (!WinHttpReceiveResponse(reinterpret_cast<HINTERNET>(request.h), nullptr)) {
        throw_winhttp_error("WinHttpReceiveResponse(POST)", GetLastError());
    }

    // 7. Read status + body.
    return read_response(reinterpret_cast<HINTERNET>(request.h));
}

}  // namespace aegis
