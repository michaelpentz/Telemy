#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdint>

// Forward declaration — actual struct defined in srtla-fork
struct srtla_ctx;
struct srtla_config;

struct SrtlaInstance {
    std::string id;          // matches connection ID
    std::string label;       // user-visible name
    uint16_t srtla_port;     // SRTLA listen port (incoming from phone)
    uint16_t srt_port;       // local SRT forward port (to OBS source)
    srtla_ctx* ctx = nullptr;
    bool running = false;
};

class EmbeddedSrtla {
public:
    EmbeddedSrtla() = default;
    ~EmbeddedSrtla();

    // Start a new srtla receiver instance
    bool StartInstance(const std::string& connection_id,
                       const std::string& label,
                       uint16_t srtla_port,
                       uint16_t srt_port = 4001,
                       const std::string& srt_host = "127.0.0.1");

    // Stop and remove an instance
    void StopInstance(const std::string& connection_id);

    // Stop all instances (called on plugin unload)
    void StopAll();

    // Query status
    bool IsRunning(const std::string& connection_id) const;
    uint16_t GetSrtlaPort(const std::string& connection_id) const;
    uint16_t GetSrtPort(const std::string& connection_id) const;
    int GetGroupCount(const std::string& connection_id) const;
    std::vector<SrtlaInstance> ListInstances() const;

    // Port allocation — finds next available port starting from base
    uint16_t AllocateSrtlaPort(uint16_t base = 5000);
    uint16_t AllocateSrtPort(uint16_t base = 4001);

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<SrtlaInstance>> instances_;
    uint16_t next_srtla_port_ = 5000;
    uint16_t next_srt_port_ = 4001;

    bool IsPortInUse(uint16_t port) const; // check against all instances
};
