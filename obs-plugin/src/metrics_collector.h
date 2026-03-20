#pragma once

// Native OBS metrics collector for the Aegis OBS plugin.
// Gathers telemetry directly from OBS C APIs, Win32 system APIs, and
// optionally NVIDIA NVML — no network hop, no external process.
// Thread-safety: Poll() and BuildStatusSnapshotJson() are expected to be called
// from the same OBS tick thread. Latest() returns a const ref suitable for
// read-only access from the same thread; cross-thread use requires external
// synchronization.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aegis {

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

    /// Called from OBS tick callback at ~2 Hz (every 500 ms).
    void Poll();

    /// Build a JSON string matching the dock state contract.
    /// Parameters supply context that the metrics collector cannot derive
    /// on its own (mode, health classification, relay state).
    std::string BuildStatusSnapshotJson(
        const std::string& mode,            // "studio" or "irl"
        const std::string& health,          // "good", "degraded", "offline"
        const std::string& relay_status,    // "inactive", "provisioning", "active", "grace"
        const std::string& relay_region,    // "" if none
        const aegis::RelaySession* relay_session = nullptr,
        const aegis::RelayStats* relay_stats = nullptr,
        const aegis::PerLinkSnapshot* per_link_stats = nullptr,
        const aegis::ConnectionSnapshot* connection_snapshot = nullptr
    ) const;

    /// Read-only access to the most recent snapshot.
    const MetricsSnapshot& Latest() const { return current_; }

    // Per-output delta tracking (public so enum callback can reference the type).
    struct OutputDelta {
        uint64_t prev_bytes = 0;
        int prev_frames = 0;
        uint64_t prev_timestamp_ms = 0;
    };

private:
    void CollectObsGlobal();
    void CollectOutputs();
    void CollectSystem();
    void InitNvml();
    void ShutdownNvml();

    MetricsSnapshot current_{};

    // ── CPU delta tracking ───────────────────────────────────────────────
    uint64_t prev_idle_ = 0;
    uint64_t prev_total_ = 0;

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

}  // namespace aegis
