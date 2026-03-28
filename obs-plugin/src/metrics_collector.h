#pragma once

// Native OBS metrics collector for the Telemy OBS plugin.
// Gathers telemetry directly from OBS C APIs, Win32 system APIs, and
// optionally NVIDIA NVML — no network hop, no external process.
// Thread-safety: Poll() runs in a dedicated background thread started by Start().
// BuildStatusSnapshotJson() and Latest() are safe to call from any thread.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace telemy {

struct RelaySession;
struct RelayStats;
struct PerLinkSnapshot;
struct ConnectionSnapshot;

// ─── Per-output telemetry ────────────────────────────────────────────────────

struct OutputMetrics {
    std::string name;
    bool active = false;
    uint64_t total_bytes = 0;
    uint32_t bitrate_kbps = 0;       // computed from byte delta between polls
    uint32_t target_bitrate_kbps = 0;
    int frames_dropped = 0;
    int total_frames = 0;
    float drop_pct = 0.0f;           // 0.0–1.0
    float fps = 0.0f;                // from frame count delta
    float congestion = 0.0f;         // 0.0–1.0 (from obs_output_get_congestion)
    float encoding_lag_ms = 0.0f;    // proxy: obs_get_average_frame_time_ns
    std::string encoder_name;
    std::string encoder_codec;        // e.g. "h264", "hevc", "av1"
    uint32_t width = 0;
    uint32_t height = 0;
};

// ─── System-level metrics ────────────────────────────────────────────────────

struct SystemMetrics {
    float cpu_percent = 0.0f;
    float mem_percent = 0.0f;
    float gpu_percent = -1.0f;       // -1 = NVML unavailable
    float gpu_temp_c = -1.0f;        // -1 = NVML unavailable
};

// ─── OBS global stats ────────────────────────────────────────────────────────

struct ObsGlobalMetrics {
    bool streaming = false;
    bool recording = false;
    bool studio_mode = false;
    double active_fps = 0.0;
    uint32_t render_missed_frames = 0;
    uint32_t render_total_frames = 0;
    float available_disk_space_mb = 0.0f;
};

// ─── Combined snapshot ───────────────────────────────────────────────────────

struct MetricsSnapshot {
    uint64_t timestamp_unix_ms = 0;
    ObsGlobalMetrics obs;
    SystemMetrics system;
    std::vector<OutputMetrics> outputs;
};

// ─── Collector ───────────────────────────────────────────────────────────────

class MetricsCollector {
public:
    MetricsCollector();
    ~MetricsCollector();

    // Non-copyable, non-movable (owns NVML handle).
    MetricsCollector(const MetricsCollector&) = delete;
    MetricsCollector& operator=(const MetricsCollector&) = delete;
    MetricsCollector(MetricsCollector&&) = delete;
    MetricsCollector& operator=(MetricsCollector&&) = delete;

    /// Start background poll thread. Call from obs_module_load.
    void Start(int poll_interval_ms = 500);

    /// Stop background poll thread. Blocks until joined. Call from obs_module_unload.
    void Stop();

    /// Poll once — collects OBS/system/GPU metrics into current_.
    void Poll();

    /// Build a JSON string matching the dock state contract.
    /// Parameters supply context that the metrics collector cannot derive
    /// on its own (mode, health classification, relay state).
    std::string BuildStatusSnapshotJson(
        const std::string& mode,            // "studio" or "irl"
        const std::string& health,          // "good", "degraded", "offline"
        const std::string& relay_status,    // "inactive", "provisioning", "active", "grace"
        const std::string& relay_region,    // "" if none
        const telemy::RelaySession* relay_session = nullptr,
        const telemy::RelayStats* relay_stats = nullptr,
        const telemy::PerLinkSnapshot* per_link_stats = nullptr,
        const telemy::ConnectionSnapshot* connection_snapshot = nullptr
    ) const;

    /// Returns a copy of the most recent snapshot. Safe to call from any thread.
    MetricsSnapshot Latest() const;

    // Per-output delta tracking (public so enum callback can reference the type).
    struct OutputDelta {
        uint64_t prev_bytes = 0;
        int prev_frames = 0;
        uint64_t prev_timestamp_ms = 0;
    };

private:
    // Background poll thread.
    std::thread             poll_thread_;
    std::atomic<bool>       poll_stop_{false};
    std::mutex              poll_cv_mu_;
    std::condition_variable poll_cv_;
    void PollLoop(int poll_interval_ms);

    void CollectObsGlobal();
    void CollectOutputs();
    void CollectSystem();
    void InitNvml();
    void ShutdownNvml();

    mutable std::mutex mu_;    // guards current_
    MetricsSnapshot current_{};

    // ── CPU delta tracking ───────────────────────────────────────────────
    uint64_t prev_idle_ = 0;
    uint64_t prev_total_ = 0;

    // ── Streaming start time (for elapsed calculation) ────────────────────
    uint64_t streaming_start_ms_ = 0;
    bool prev_streaming_ = false;

    // ── Per-output delta tracking (bitrate / fps) ────────────────────────
    std::map<std::string, OutputDelta> output_deltas_;

    // ── NVML (dynamically loaded) ────────────────────────────────────────
    void* nvml_lib_ = nullptr;         // HMODULE cast to void*
    void* nvml_device_ = nullptr;      // nvmlDevice_t cast to void*
    bool nvml_available_ = false;

    // NVML function pointer typedefs (avoid #include <nvml.h>).
    // All NVML functions return nvmlReturn_t (int); 0 = NVML_SUCCESS.
    using NvmlInit_t = int (*)();
    using NvmlShutdown_t = int (*)();
    using NvmlGetHandle_t = int (*)(unsigned int index, void** device);
    using NvmlGetUtil_t = int (*)(void* device, void* utilization);
    using NvmlGetTemp_t = int (*)(void* device, int sensor_type, unsigned int* temp);

    NvmlInit_t nvml_init_ = nullptr;
    NvmlShutdown_t nvml_shutdown_ = nullptr;
    NvmlGetHandle_t nvml_get_handle_ = nullptr;
    NvmlGetUtil_t nvml_get_util_ = nullptr;
    NvmlGetTemp_t nvml_get_temp_ = nullptr;
};

}  // namespace telemy
