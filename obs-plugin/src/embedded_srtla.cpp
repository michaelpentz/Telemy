#include "embedded_srtla.h"
#include "srtla_lib.h"

#include <obs-module.h>

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

EmbeddedSrtla::~EmbeddedSrtla()
{
    StopAll();
}

// ---------------------------------------------------------------------------
// StartInstance
// ---------------------------------------------------------------------------

bool EmbeddedSrtla::StartInstance(const std::string& connection_id,
                                  const std::string& label,
                                  uint16_t srtla_port,
                                  uint16_t srt_port,
                                  const std::string& srt_host)
{
    std::lock_guard<std::mutex> lock(mu_);

    // Reject duplicate IDs
    if (instances_.count(connection_id)) {
        blog(LOG_WARNING,
             "[EmbeddedSrtla] Instance '%s' already exists, ignoring start",
             connection_id.c_str());
        return false;
    }

    // Build config with conservative defaults for home/desktop use
    srtla_config cfg{};
    cfg.srtla_port          = srtla_port;
    cfg.srt_hostname        = srt_host;
    cfg.srt_port            = srt_port;
    cfg.bind_addr           = "0.0.0.0";
    cfg.stats_bind          = "127.0.0.1";
    cfg.max_groups          = 5;
    cfg.max_conns_per_group = 8;
    cfg.reg_rate_limit      = 5;
    cfg.reg_rate_window     = 60;
    cfg.stats_port          = 0;   // disabled
    cfg.verbose             = false;

    srtla_ctx* ctx = srtla_create(cfg);
    if (!ctx) {
        blog(LOG_ERROR,
             "[EmbeddedSrtla] srtla_create failed for '%s' (port %u)",
             connection_id.c_str(), srtla_port);
        return false;
    }

    int rc = srtla_start(ctx);
    if (rc != 0) {
        blog(LOG_ERROR,
             "[EmbeddedSrtla] srtla_start failed for '%s' (port %u, rc=%d)",
             connection_id.c_str(), srtla_port, rc);
        srtla_destroy(ctx);
        return false;
    }

    auto inst = std::make_unique<SrtlaInstance>();
    inst->id         = connection_id;
    inst->label      = label;
    inst->srtla_port = srtla_port;
    inst->srt_port   = srt_port;
    inst->ctx        = ctx;
    inst->running    = true;

    instances_[connection_id] = std::move(inst);

    blog(LOG_INFO,
         "[EmbeddedSrtla] Started instance '%s' — SRTLA :%u -> SRT %s:%u",
         connection_id.c_str(), srtla_port, srt_host.c_str(), srt_port);
    return true;
}

// ---------------------------------------------------------------------------
// StopInstance
// ---------------------------------------------------------------------------

void EmbeddedSrtla::StopInstance(const std::string& connection_id)
{
    std::lock_guard<std::mutex> lock(mu_);

    auto it = instances_.find(connection_id);
    if (it == instances_.end())
        return;

    SrtlaInstance* inst = it->second.get();
    if (inst->ctx) {
        srtla_stop(inst->ctx);
        srtla_destroy(inst->ctx);
        inst->ctx = nullptr;
    }
    inst->running = false;

    blog(LOG_INFO, "[EmbeddedSrtla] Stopped instance '%s'",
         connection_id.c_str());

    instances_.erase(it);
}

// ---------------------------------------------------------------------------
// StopAll
// ---------------------------------------------------------------------------

void EmbeddedSrtla::StopAll()
{
    std::lock_guard<std::mutex> lock(mu_);

    for (auto& [id, inst] : instances_) {
        if (inst->ctx) {
            srtla_stop(inst->ctx);
            srtla_destroy(inst->ctx);
            inst->ctx = nullptr;
        }
        inst->running = false;
        blog(LOG_INFO, "[EmbeddedSrtla] Stopped instance '%s' (StopAll)",
             id.c_str());
    }
    instances_.clear();
}

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

bool EmbeddedSrtla::IsRunning(const std::string& connection_id) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = instances_.find(connection_id);
    if (it == instances_.end())
        return false;
    if (!it->second->ctx)
        return false;
    return srtla_is_running(it->second->ctx);
}

uint16_t EmbeddedSrtla::GetSrtlaPort(const std::string& connection_id) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = instances_.find(connection_id);
    if (it == instances_.end())
        return 0;
    return it->second->srtla_port;
}

uint16_t EmbeddedSrtla::GetSrtPort(const std::string& connection_id) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = instances_.find(connection_id);
    if (it == instances_.end())
        return 0;
    return it->second->srt_port;
}

int EmbeddedSrtla::GetGroupCount(const std::string& connection_id) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = instances_.find(connection_id);
    if (it == instances_.end())
        return 0;
    if (!it->second->ctx)
        return 0;
    return srtla_get_group_count(it->second->ctx);
}

std::vector<SrtlaInstance> EmbeddedSrtla::ListInstances() const
{
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SrtlaInstance> result;
    result.reserve(instances_.size());
    for (const auto& [id, inst] : instances_) {
        SrtlaInstance copy;
        copy.id         = inst->id;
        copy.label      = inst->label;
        copy.srtla_port = inst->srtla_port;
        copy.srt_port   = inst->srt_port;
        copy.ctx        = nullptr;  // don't expose raw pointer
        copy.running    = inst->ctx ? srtla_is_running(inst->ctx) : false;
        result.push_back(std::move(copy));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Port allocation
// ---------------------------------------------------------------------------

uint16_t EmbeddedSrtla::AllocateSrtlaPort(uint16_t base)
{
    std::lock_guard<std::mutex> lock(mu_);
    uint16_t port = (base > next_srtla_port_) ? base : next_srtla_port_;
    while (IsPortInUse(port) && port < 65535)
        ++port;
    next_srtla_port_ = port + 1;
    return port;
}

uint16_t EmbeddedSrtla::AllocateSrtPort(uint16_t base)
{
    std::lock_guard<std::mutex> lock(mu_);
    uint16_t port = (base > next_srt_port_) ? base : next_srt_port_;
    while (IsPortInUse(port) && port < 65535)
        ++port;
    next_srt_port_ = port + 1;
    return port;
}

// ---------------------------------------------------------------------------
// Internal helpers (caller must hold mu_)
// ---------------------------------------------------------------------------

bool EmbeddedSrtla::IsPortInUse(uint16_t port) const
{
    for (const auto& [id, inst] : instances_) {
        if (inst->srtla_port == port || inst->srt_port == port)
            return true;
    }
    return false;
}
