#pragma once

#include <string>
#include <stdexcept>

// WinHTTP RAII handle wrapper — forward-declared here, defined in .cpp
// to keep <windows.h>/<winhttp.h> out of every translation unit that
// includes this header.

namespace aegis {

// -----------------------------------------------------------------------
// RAII wrapper for HINTERNET handles (connect / request scoped handles).
// The session-level handle is owned by HttpsClient directly.
// -----------------------------------------------------------------------
struct WinHttpHandle {
    void* h = nullptr;  // HINTERNET stored as void* to avoid pulling in winhttp.h
    ~WinHttpHandle();
    operator void*() const { return h; }
    bool valid() const { return h != nullptr; }
    WinHttpHandle() = default;
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

// -----------------------------------------------------------------------
// Result of a completed HTTP request.
// -----------------------------------------------------------------------
struct HttpResponse {
    unsigned long status_code = 0;  // HTTP status (e.g. 200, 401, 500)
    std::string body;               // Response body as UTF-8 bytes

    // Returns true for 2xx status codes.
    bool ok() const { return status_code >= 200 && status_code < 300; }
};

// -----------------------------------------------------------------------
// Thin synchronous HTTPS client backed by WinHTTP.
//
// The WinHTTP session is created once in the constructor and reused for
// all subsequent requests (sessions are expensive to open).
//
// IMPORTANT: All methods are synchronous and block the calling thread.
// Never call Get() / Post() from the OBS UI thread — always dispatch to
// a background/worker thread.
// -----------------------------------------------------------------------
class HttpsClient {
public:
    // Creates the WinHTTP session with a "Telemy/0.0.4" user-agent and
    // configures timeouts (5s resolve, 5s connect, 10s send, 30s receive).
    // Throws std::runtime_error if WinHttpOpen fails.
    HttpsClient();

    // Closes the session handle.
    ~HttpsClient();

    // Non-copyable, non-movable — the session handle is a unique resource.
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    // -----------------------------------------------------------------------
    // Synchronous GET to https://<host><path>
    //
    // @param host          Hostname only, e.g. L"api.example.com"
    // @param path          Absolute path + query, e.g. L"/v1/status?foo=bar"
    // @param bearer_token  Optional Bearer token (omit or pass L"" to skip).
    //
    // Throws std::runtime_error on any WinHTTP failure.
    // -----------------------------------------------------------------------
    HttpResponse Get(const std::wstring& host,
                     const std::wstring& path,
                     const std::wstring& bearer_token = L"");

    // -----------------------------------------------------------------------
    // Synchronous POST to https://<host><path> with a JSON body.
    //
    // @param host          Hostname only, e.g. L"api.example.com"
    // @param path          Absolute path + query, e.g. L"/v1/ingest"
    // @param json_body     UTF-8 encoded JSON string (Content-Type is set
    //                      automatically to application/json).
    // @param bearer_token  Optional Bearer token (omit or pass L"" to skip).
    //
    // Throws std::runtime_error on any WinHTTP failure.
    // -----------------------------------------------------------------------
    HttpResponse Post(const std::wstring& host,
                      const std::wstring& path,
                      const std::string& json_body,
                      const std::wstring& bearer_token = L"");

private:
    void* session_ = nullptr;  // HINTERNET — opaque in header, cast in .cpp
};

}  // namespace aegis
