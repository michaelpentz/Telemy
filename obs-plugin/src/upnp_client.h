#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>

struct UpnpMapping {
    uint16_t external_port = 0;
    uint16_t internal_port = 0;
    std::string protocol = "UDP";
    std::string description;
    bool active = false;
};

class UpnpClient {
public:
    UpnpClient() = default;
    ~UpnpClient();

    // Try to map a UDP port. Returns true on success.
    bool MapPort(uint16_t internal_port, uint16_t external_port,
                 const std::string& description = "Telemy SRTLA");

    // Remove a port mapping.
    bool UnmapPort(uint16_t external_port);

    // Get discovered external IP.
    std::string GetExternalIP() const;

    // Start background refresh thread.
    void StartRefreshLoop(uint16_t internal_port, uint16_t external_port,
                          int refresh_interval_sec = 1800);
    void StopRefreshLoop();

    bool IsAvailable() const { return available_; }
    bool IsMapped() const { return mapped_; }

private:
    mutable std::mutex mu_;
    bool available_ = false;
    bool mapped_ = false;
    std::string external_ip_;

    std::atomic<bool> refresh_running_{false};
    std::thread refresh_thread_;

    // UPnP discovery state
    bool discovered_ = false;
    std::string control_url_;
    std::string service_type_;
    std::string lan_address_;

    bool Discover();
};
