#include "upnp_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

#ifndef TELEMY_NO_OBS_LOG
#include <obs-module.h>
#define UPNP_LOG(level, fmt, ...) blog(level, "[upnp] " fmt, ##__VA_ARGS__)
#else
#define UPNP_LOG(level, fmt, ...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// SSDP / SOAP constants
// ---------------------------------------------------------------------------

static const char* SSDP_MULTICAST = "239.255.255.250";
static const int SSDP_PORT = 1900;

static const char* SSDP_MSEARCH =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 3\r\n"
    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
    "\r\n";

// Service types we look for (WANIPConnection v1 and v2)
static const char* WANIP_SERVICE_V1 =
    "urn:schemas-upnp-org:service:WANIPConnection:1";
static const char* WANIP_SERVICE_V2 =
    "urn:schemas-upnp-org:service:WANIPConnection:2";
static const char* WANPPP_SERVICE_V1 =
    "urn:schemas-upnp-org:service:WANPPPConnection:1";

// SOAP envelope templates
static const char* SOAP_ADD_PORT_MAPPING =
    "<?xml version=\"1.0\"?>\r\n"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
    "<s:Body>\r\n"
    "<u:AddPortMapping xmlns:u=\"%s\">\r\n"
    "<NewRemoteHost></NewRemoteHost>\r\n"
    "<NewExternalPort>%u</NewExternalPort>\r\n"
    "<NewProtocol>UDP</NewProtocol>\r\n"
    "<NewInternalPort>%u</NewInternalPort>\r\n"
    "<NewInternalClient>%s</NewInternalClient>\r\n"
    "<NewEnabled>1</NewEnabled>\r\n"
    "<NewPortMappingDescription>%s</NewPortMappingDescription>\r\n"
    "<NewLeaseDuration>3600</NewLeaseDuration>\r\n"
    "</u:AddPortMapping>\r\n"
    "</s:Body>\r\n"
    "</s:Envelope>\r\n";

static const char* SOAP_DELETE_PORT_MAPPING =
    "<?xml version=\"1.0\"?>\r\n"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
    "<s:Body>\r\n"
    "<u:DeletePortMapping xmlns:u=\"%s\">\r\n"
    "<NewRemoteHost></NewRemoteHost>\r\n"
    "<NewExternalPort>%u</NewExternalPort>\r\n"
    "<NewProtocol>UDP</NewProtocol>\r\n"
    "</u:DeletePortMapping>\r\n"
    "</s:Body>\r\n"
    "</s:Envelope>\r\n";

static const char* SOAP_GET_EXTERNAL_IP =
    "<?xml version=\"1.0\"?>\r\n"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
    "<s:Body>\r\n"
    "<u:GetExternalIPAddress xmlns:u=\"%s\">\r\n"
    "</u:GetExternalIPAddress>\r\n"
    "</s:Body>\r\n"
    "</s:Envelope>\r\n";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Case-insensitive substring search.
static std::string::size_type ifind(const std::string& haystack,
                                    const std::string& needle) {
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower((unsigned char)a) ==
                                     std::tolower((unsigned char)b);
                          });
    return (it == haystack.end()) ? std::string::npos
                                 : (std::string::size_type)(it - haystack.begin());
}

// Extract value between <tag> and </tag> (case-sensitive, first match).
static std::string extractXmlTag(const std::string& xml,
                                 const std::string& tag) {
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    auto start = xml.find(open);
    if (start == std::string::npos) return "";
    start += open.size();
    auto end = xml.find(close, start);
    if (end == std::string::npos) return "";
    return xml.substr(start, end - start);
}

// Get the local LAN IP address by connecting a UDP socket to an external
// address (doesn't actually send anything).
static std::string getLocalIP() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    if (connect(s, (sockaddr*)&remote, sizeof(remote)) != 0) {
        closesocket(s);
        return "";
    }

    sockaddr_in local{};
    int len = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &len) != 0) {
        closesocket(s);
        return "";
    }
    closesocket(s);

    char buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    return buf;
}

// Parse a URL into scheme, host, port, path.
struct UrlParts {
    bool use_tls = false;
    std::string host;
    uint16_t port = 80;
    std::string path;
};

static bool parseUrl(const std::string& url, UrlParts& out) {
    out = {};
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;

    std::string scheme = url.substr(0, scheme_end);
    if (scheme == "https") {
        out.use_tls = true;
        out.port = 443;
    } else if (scheme == "http") {
        out.use_tls = false;
        out.port = 80;
    } else {
        return false;
    }

    size_t host_start = scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    std::string authority;
    if (path_start == std::string::npos) {
        authority = url.substr(host_start);
        out.path = "/";
    } else {
        authority = url.substr(host_start, path_start - host_start);
        out.path = url.substr(path_start);
    }

    size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        out.host = authority.substr(0, colon);
        int p = 0;
        try { p = std::stoi(authority.substr(colon + 1)); } catch (...) {}
        if (p > 0 && p <= 65535) out.port = (uint16_t)p;
    } else {
        out.host = authority;
    }
    return !out.host.empty();
}

// Widen a std::string to std::wstring (ASCII safe for URLs).
static std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// Perform an HTTP request using WinHTTP. Returns response body on success,
// empty string on failure. Supports both GET and POST.
static std::string httpRequest(const std::string& method,
                               const std::string& url,
                               const std::string& body = "",
                               const std::string& content_type = "",
                               const std::string& soap_action = "") {
    UrlParts parts;
    if (!parseUrl(url, parts)) {
        UPNP_LOG(LOG_WARNING, "httpRequest: invalid URL: %s", url.c_str());
        return "";
    }

    HINTERNET session = WinHttpOpen(L"Telemy-UPnP/1.0",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return "";

    // Short timeouts: 3s resolve, 3s connect, 5s send, 5s receive.
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);

    HINTERNET conn = WinHttpConnect(session, widen(parts.host).c_str(),
                                    parts.port, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return "";
    }

    DWORD flags = parts.use_tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, widen(method).c_str(),
                                       widen(parts.path).c_str(),
                                       NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return "";
    }

    // Build headers.
    std::wstring headers;
    if (!content_type.empty()) {
        headers += L"Content-Type: " + widen(content_type) + L"\r\n";
    }
    if (!soap_action.empty()) {
        headers += L"SOAPAction: \"" + widen(soap_action) + L"\"\r\n";
    }

    BOOL ok;
    if (method == "POST") {
        ok = WinHttpSendRequest(req,
                                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                (DWORD)headers.size(),
                                (LPVOID)body.c_str(), (DWORD)body.size(),
                                (DWORD)body.size(), 0);
    } else {
        ok = WinHttpSendRequest(req,
                                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                (DWORD)headers.size(),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }

    if (!ok) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return "";
    }

    if (!WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return "";
    }

    // Read response body.
    std::string result;
    DWORD bytes_available = 0;
    while (WinHttpQueryDataAvailable(req, &bytes_available) && bytes_available > 0) {
        std::vector<char> buf(bytes_available);
        DWORD bytes_read = 0;
        if (WinHttpReadData(req, buf.data(), bytes_available, &bytes_read)) {
            result.append(buf.data(), bytes_read);
        }
        bytes_available = 0;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return result;
}

// Send a SOAP request and return the response body.
static std::string soapRequest(const std::string& control_url,
                               const std::string& service_type,
                               const std::string& action,
                               const std::string& body) {
    std::string soap_action = service_type + "#" + action;
    return httpRequest("POST", control_url, body,
                       "text/xml; charset=\"utf-8\"", soap_action);
}

// snprintf into a std::string.
template<typename... Args>
static std::string fmt(const char* format, Args... args) {
    int sz = std::snprintf(nullptr, 0, format, args...);
    if (sz <= 0) return "";
    std::string result(sz, '\0');
    std::snprintf(&result[0], sz + 1, format, args...);
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// UpnpClient implementation
// ---------------------------------------------------------------------------

UpnpClient::~UpnpClient() {
    StopRefreshLoop();
}

bool UpnpClient::Discover() {
    // Initialize Winsock (idempotent if already initialized).
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        UPNP_LOG(LOG_WARNING, "WSAStartup failed");
        return false;
    }

    // Get LAN address for port mapping.
    std::string local_ip = getLocalIP();
    if (local_ip.empty()) {
        UPNP_LOG(LOG_WARNING, "could not determine local IP");
        WSACleanup();
        return false;
    }

    // Send SSDP M-SEARCH.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        UPNP_LOG(LOG_WARNING, "SSDP socket creation failed");
        WSACleanup();
        return false;
    }

    // Set socket timeout to 3 seconds.
    DWORD timeout_ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms,
               sizeof(timeout_ms));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SSDP_PORT);
    inet_pton(AF_INET, SSDP_MULTICAST, &dest.sin_addr);

    int sent = sendto(s, SSDP_MSEARCH, (int)strlen(SSDP_MSEARCH), 0,
                      (sockaddr*)&dest, sizeof(dest));
    if (sent <= 0) {
        UPNP_LOG(LOG_WARNING, "SSDP M-SEARCH send failed");
        closesocket(s);
        WSACleanup();
        return false;
    }

    // Receive responses (try up to 5 responses or until timeout).
    std::string location_url;
    char buf[4096];

    for (int i = 0; i < 5; ++i) {
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
        if (n <= 0) break;
        buf[n] = '\0';

        std::string response(buf, n);

        // Check if this is an IGD response.
        if (ifind(response, "InternetGatewayDevice") == std::string::npos &&
            ifind(response, "WANIPConnection") == std::string::npos &&
            ifind(response, "WANPPPConnection") == std::string::npos) {
            continue;
        }

        // Extract Location header.
        auto loc_pos = ifind(response, "location:");
        if (loc_pos == std::string::npos) continue;

        size_t val_start = loc_pos + 9;  // strlen("location:")
        while (val_start < response.size() && response[val_start] == ' ')
            ++val_start;
        size_t val_end = response.find("\r\n", val_start);
        if (val_end == std::string::npos) val_end = response.size();

        location_url = response.substr(val_start, val_end - val_start);
        UPNP_LOG(LOG_INFO, "SSDP discovered: %s", location_url.c_str());
        break;
    }

    closesocket(s);

    if (location_url.empty()) {
        UPNP_LOG(LOG_WARNING, "no UPnP gateway discovered");
        WSACleanup();
        return false;
    }

    // Fetch the device description XML.
    std::string desc_xml = httpRequest("GET", location_url);
    if (desc_xml.empty()) {
        UPNP_LOG(LOG_WARNING, "failed to fetch device description from %s",
                 location_url.c_str());
        WSACleanup();
        return false;
    }

    // Find WANIPConnection or WANPPPConnection service and its controlURL.
    // We search for the service type and then find the associated controlURL.
    std::string found_service_type;
    std::string found_control_url;

    // Try WANIPConnection:1, WANIPConnection:2, then WANPPPConnection:1.
    const char* service_types[] = {WANIP_SERVICE_V1, WANIP_SERVICE_V2,
                                   WANPPP_SERVICE_V1};

    for (const char* svc : service_types) {
        auto pos = desc_xml.find(svc);
        if (pos == std::string::npos) continue;

        found_service_type = svc;

        // Find the <controlURL> within the same <service> block.
        // Search forward from the serviceType match for controlURL.
        auto ctrl_pos = desc_xml.find("<controlURL>", pos);
        if (ctrl_pos == std::string::npos) continue;

        auto ctrl_end = desc_xml.find("</controlURL>", ctrl_pos);
        if (ctrl_end == std::string::npos) continue;

        found_control_url = desc_xml.substr(ctrl_pos + 12,
                                            ctrl_end - (ctrl_pos + 12));
        break;
    }

    if (found_control_url.empty()) {
        UPNP_LOG(LOG_WARNING, "no WANIPConnection controlURL in device XML");
        WSACleanup();
        return false;
    }

    // Build the full control URL. If controlURL is relative, combine with
    // the location URL base.
    std::string full_control_url;
    if (found_control_url.find("http") == 0) {
        full_control_url = found_control_url;
    } else {
        // Extract base URL from location.
        UrlParts loc_parts;
        if (!parseUrl(location_url, loc_parts)) {
            UPNP_LOG(LOG_WARNING, "failed to parse location URL");
            WSACleanup();
            return false;
        }
        std::string base = (loc_parts.use_tls ? "https://" : "http://") +
                           loc_parts.host + ":" + std::to_string(loc_parts.port);
        if (found_control_url[0] != '/')
            base += "/";
        full_control_url = base + found_control_url;
    }

    UPNP_LOG(LOG_INFO, "UPnP control URL: %s  service: %s",
             full_control_url.c_str(), found_service_type.c_str());

    // Store results under lock.
    {
        std::lock_guard<std::mutex> lk(mu_);
        discovered_ = true;
        available_ = true;
        control_url_ = full_control_url;
        service_type_ = found_service_type;
        lan_address_ = local_ip;
    }

    return true;
}

bool UpnpClient::MapPort(uint16_t internal_port, uint16_t external_port,
                          const std::string& description) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!discovered_) {
            // Release lock before discovery (network I/O).
            mu_.unlock();
            if (!Discover()) return false;
            mu_.lock();
        }
    }

    std::string ctrl, svc, lan;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ctrl = control_url_;
        svc = service_type_;
        lan = lan_address_;
    }

    // Build SOAP body for AddPortMapping.
    std::string body = fmt(SOAP_ADD_PORT_MAPPING,
                           svc.c_str(),
                           (unsigned)external_port,
                           (unsigned)internal_port,
                           lan.c_str(),
                           description.c_str());

    std::string response = soapRequest(ctrl, svc, "AddPortMapping", body);
    if (response.empty()) {
        UPNP_LOG(LOG_WARNING, "AddPortMapping request failed");
        return false;
    }

    // Check for SOAP fault.
    if (response.find("AddPortMappingResponse") != std::string::npos ||
        response.find("<u:AddPortMapping") != std::string::npos) {
        // Success — some routers return the action name in the response tag.
    } else if (response.find("<faultstring>") != std::string::npos) {
        std::string fault = extractXmlTag(response, "faultstring");
        UPNP_LOG(LOG_WARNING, "AddPortMapping SOAP fault: %s", fault.c_str());
        return false;
    }
    // Many routers return a simple 200 OK with an empty-ish envelope on
    // success, so we treat non-fault responses as success.

    UPNP_LOG(LOG_INFO, "mapped UDP %s:%u -> %s:%u (%s)",
             "external", (unsigned)external_port,
             lan.c_str(), (unsigned)internal_port,
             description.c_str());

    // Also fetch external IP while we're at it.
    std::string ip_body = fmt(SOAP_GET_EXTERNAL_IP, svc.c_str());
    std::string ip_resp = soapRequest(ctrl, svc, "GetExternalIPAddress",
                                      ip_body);
    std::string ext_ip = extractXmlTag(ip_resp, "NewExternalIPAddress");

    {
        std::lock_guard<std::mutex> lk(mu_);
        mapped_ = true;
        if (!ext_ip.empty()) {
            external_ip_ = ext_ip;
        }
    }

    if (!ext_ip.empty()) {
        UPNP_LOG(LOG_INFO, "external IP: %s", ext_ip.c_str());
    }

    return true;
}

bool UpnpClient::UnmapPort(uint16_t external_port) {
    std::string ctrl, svc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!discovered_) return false;
        ctrl = control_url_;
        svc = service_type_;
    }

    std::string body = fmt(SOAP_DELETE_PORT_MAPPING,
                           svc.c_str(),
                           (unsigned)external_port);

    std::string response = soapRequest(ctrl, svc, "DeletePortMapping", body);
    if (response.empty()) {
        UPNP_LOG(LOG_WARNING, "DeletePortMapping request failed");
        return false;
    }

    if (response.find("<faultstring>") != std::string::npos) {
        std::string fault = extractXmlTag(response, "faultstring");
        UPNP_LOG(LOG_WARNING, "DeletePortMapping SOAP fault: %s",
                 fault.c_str());
        return false;
    }

    UPNP_LOG(LOG_INFO, "unmapped UDP external port %u",
             (unsigned)external_port);

    {
        std::lock_guard<std::mutex> lk(mu_);
        mapped_ = false;
    }
    return true;
}

std::string UpnpClient::GetExternalIP() const {
    std::lock_guard<std::mutex> lk(mu_);
    return external_ip_;
}

void UpnpClient::StartRefreshLoop(uint16_t internal_port,
                                   uint16_t external_port,
                                   int refresh_interval_sec) {
    StopRefreshLoop();

    refresh_running_.store(true);
    refresh_thread_ = std::thread([this, internal_port, external_port,
                                   refresh_interval_sec]() {
        while (refresh_running_.load()) {
            // Sleep in 1-second increments so we can check the stop flag.
            for (int i = 0; i < refresh_interval_sec && refresh_running_.load();
                 ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!refresh_running_.load()) break;

            // Re-send AddPortMapping (idempotent, extends lease).
            UPNP_LOG(LOG_INFO, "refreshing UPnP port mapping %u -> %u",
                     (unsigned)external_port, (unsigned)internal_port);
            MapPort(internal_port, external_port);
        }
    });
}

void UpnpClient::StopRefreshLoop() {
    refresh_running_.store(false);
    if (refresh_thread_.joinable()) {
        refresh_thread_.join();
    }
}
