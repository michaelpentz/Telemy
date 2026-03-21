#include "metrics_collector.h"
#include "connection_manager.h"
#include "relay_client.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#include <obs-module.h>
#include <obs-frontend-api.h>
#endif

// Windows headers — order matters (windows.h before psapi.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

// ─── Logging helper ──────────────────────────────────────────────────────────
// In an OBS plugin build, blog() is available via obs-module.h.
// Outside OBS (unit-test / standalone harness), fall back to fprintf.

#if defined(AEGIS_OBS_PLUGIN_BUILD)
#define METRICS_LOG_INFO(fmt, ...) blog(LOG_INFO, "[aegis-metrics] " fmt, ##__VA_ARGS__)
#define METRICS_LOG_WARN(fmt, ...) blog(LOG_WARNING, "[aegis-metrics] " fmt, ##__VA_ARGS__)
#define METRICS_LOG_ERROR(fmt, ...) blog(LOG_ERROR, "[aegis-metrics] " fmt, ##__VA_ARGS__)
#else
#define METRICS_LOG_INFO(fmt, ...) fprintf(stderr, "[aegis-metrics][info] " fmt "\n", ##__VA_ARGS__)
#define METRICS_LOG_WARN(fmt, ...) fprintf(stderr, "[aegis-metrics][warn] " fmt "\n", ##__VA_ARGS__)
#define METRICS_LOG_ERROR(fmt, ...) fprintf(stderr, "[aegis-metrics][error] " fmt "\n", ##__VA_ARGS__)
#endif

namespace aegis {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static uint64_t NowUnixMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

static uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

/// JSON-escape a string (matches the pattern in obs_plugin_entry.cpp).
static std::string JsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out.push_back(ch); break;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

MetricsCollector::MetricsCollector() {
    InitNvml();
    METRICS_LOG_INFO("MetricsCollector created (nvml=%s)", nvml_available_ ? "yes" : "no");
}

MetricsCollector::~MetricsCollector() {
    Stop();
    ShutdownNvml();
    METRICS_LOG_INFO("MetricsCollector destroyed");
}

void MetricsCollector::Start(int poll_interval_ms) {
    poll_stop_.store(false);
    poll_thread_ = std::thread(&MetricsCollector::PollLoop, this, poll_interval_ms);
    METRICS_LOG_INFO("MetricsCollector poll thread started (interval=%dms)", poll_interval_ms);
}

void MetricsCollector::Stop() {
    poll_stop_.store(true);
    poll_cv_.notify_all();
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

void MetricsCollector::PollLoop(int poll_interval_ms) {
    using namespace std::chrono;
    const auto interval = milliseconds(std::max(100, std::min(poll_interval_ms, 5000)));
    while (!poll_stop_.load()) {
        Poll();
        std::unique_lock<std::mutex> lock(poll_cv_mu_);
        poll_cv_.wait_for(lock, interval, [this] { return poll_stop_.load(); });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// NVML dynamic loading
// ─────────────────────────────────────────────────────────────────────────────

void MetricsCollector::InitNvml() {
    HMODULE lib = LoadLibraryW(L"nvml.dll");
    if (!lib) {
        METRICS_LOG_INFO("nvml.dll not found — GPU metrics disabled");
        return;
    }

    nvml_lib_ = static_cast<void*>(lib);

    // Resolve symbols.
    nvml_init_       = reinterpret_cast<NvmlInit_t>(GetProcAddress(lib, "nvmlInit_v2"));
    nvml_shutdown_   = reinterpret_cast<NvmlShutdown_t>(GetProcAddress(lib, "nvmlShutdown"));
    nvml_get_handle_ = reinterpret_cast<NvmlGetHandle_t>(GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2"));
    nvml_get_util_   = reinterpret_cast<NvmlGetUtil_t>(GetProcAddress(lib, "nvmlDeviceGetUtilizationRates"));
    nvml_get_temp_   = reinterpret_cast<NvmlGetTemp_t>(GetProcAddress(lib, "nvmlDeviceGetTemperature"));

    if (!nvml_init_ || !nvml_shutdown_ || !nvml_get_handle_ || !nvml_get_util_ || !nvml_get_temp_) {
        METRICS_LOG_WARN("nvml.dll loaded but one or more symbols missing — GPU metrics disabled");
        FreeLibrary(lib);
        nvml_lib_ = nullptr;
        return;
    }

    // Initialize NVML.
    int ret = nvml_init_();
    if (ret != 0) {
        METRICS_LOG_WARN("nvmlInit_v2 failed (ret=%d) — GPU metrics disabled", ret);
        FreeLibrary(lib);
        nvml_lib_ = nullptr;
        nvml_init_ = nullptr;
        nvml_shutdown_ = nullptr;
        nvml_get_handle_ = nullptr;
        nvml_get_util_ = nullptr;
        nvml_get_temp_ = nullptr;
        return;
    }

    // Get handle for GPU index 0 (primary GPU).
    void* device = nullptr;
    ret = nvml_get_handle_(0, &device);
    if (ret != 0) {
        METRICS_LOG_WARN("nvmlDeviceGetHandleByIndex_v2(0) failed (ret=%d) — GPU metrics disabled", ret);
        nvml_shutdown_();
        FreeLibrary(lib);
        nvml_lib_ = nullptr;
        nvml_init_ = nullptr;
        nvml_shutdown_ = nullptr;
        nvml_get_handle_ = nullptr;
        nvml_get_util_ = nullptr;
        nvml_get_temp_ = nullptr;
        return;
    }

    nvml_device_ = device;
    nvml_available_ = true;
    METRICS_LOG_INFO("NVML initialized — GPU metrics enabled");
}

void MetricsCollector::ShutdownNvml() {
    if (nvml_available_ && nvml_shutdown_) {
        nvml_shutdown_();
    }
    if (nvml_lib_) {
        FreeLibrary(static_cast<HMODULE>(nvml_lib_));
        nvml_lib_ = nullptr;
    }
    nvml_device_ = nullptr;
    nvml_available_ = false;
    nvml_init_ = nullptr;
    nvml_shutdown_ = nullptr;
    nvml_get_handle_ = nullptr;
    nvml_get_util_ = nullptr;
    nvml_get_temp_ = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Poll — main collection entry point
// ─────────────────────────────────────────────────────────────────────────────

void MetricsCollector::Poll() {
    std::lock_guard<std::mutex> lock(mu_);
    current_.timestamp_unix_ms = NowUnixMs();
    CollectObsGlobal();
    CollectOutputs();
    CollectSystem();
}

MetricsSnapshot MetricsCollector::Latest() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
}

// ─────────────────────────────────────────────────────────────────────────────
// OBS global metrics
// ─────────────────────────────────────────────────────────────────────────────

void MetricsCollector::CollectObsGlobal() {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    current_.obs.streaming = obs_frontend_streaming_active();
    current_.obs.recording = obs_frontend_recording_active();
    current_.obs.studio_mode = obs_frontend_preview_program_mode_active();
    current_.obs.active_fps = obs_get_active_fps();
    current_.obs.render_missed_frames = obs_get_lagged_frames();
    current_.obs.render_total_frames = obs_get_total_frames();

    // Disk space — query the drive containing the OBS working directory.
    // Default to C:\ which is the most common installation drive.
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(L"C:\\", &free_bytes, nullptr, nullptr)) {
        current_.obs.available_disk_space_mb =
            static_cast<float>(free_bytes.QuadPart / (1024ULL * 1024ULL));
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-output enumeration
// ─────────────────────────────────────────────────────────────────────────────

#if defined(AEGIS_OBS_PLUGIN_BUILD)

struct OutputEnumContext {
    MetricsCollector* self;
    uint64_t now_ms;
    float encoding_lag_ms;
    std::vector<OutputMetrics>* outputs;
    std::map<std::string, MetricsCollector::OutputDelta>* deltas;
};

// Callback for obs_enum_outputs(). The output pointer is NOT ref-counted here
// — do NOT call obs_output_release().
static bool OutputEnumCallback(void* param, obs_output_t* output) {
    auto* ctx = static_cast<OutputEnumContext*>(param);

    const char* raw_name = obs_output_get_name(output);
    std::string name = raw_name ? raw_name : "(unnamed)";

    OutputMetrics om;
    om.name = name;
    om.active = obs_output_active(output);
    om.total_bytes = obs_output_get_total_bytes(output);
    om.frames_dropped = obs_output_get_frames_dropped(output);
    om.total_frames = obs_output_get_total_frames(output);
    om.congestion = obs_output_get_congestion(output);
    om.encoding_lag_ms = ctx->encoding_lag_ms;
    om.width = obs_output_get_width(output);
    om.height = obs_output_get_height(output);

    // Encoder info (non-owning pointer — do NOT release).
    obs_encoder_t* enc = obs_output_get_video_encoder(output);
    if (enc) {
        const char* enc_name = obs_encoder_get_name(enc);
        const char* enc_codec = obs_encoder_get_codec(enc);
        om.encoder_name = enc_name ? enc_name : "";
        om.encoder_codec = enc_codec ? enc_codec : "";
        // Prefer encoder dimensions if available (may differ from output).
        uint32_t enc_w = obs_encoder_get_width(enc);
        uint32_t enc_h = obs_encoder_get_height(enc);
        if (enc_w > 0 && enc_h > 0) {
            om.width = enc_w;
            om.height = enc_h;
        }
    }

    // Drop percentage.
    if (om.total_frames > 0) {
        om.drop_pct = static_cast<float>(om.frames_dropped) /
                      static_cast<float>(om.total_frames);
    }

    // Delta-based bitrate and FPS.
    auto it = ctx->deltas->find(name);
    if (it != ctx->deltas->end()) {
        auto& delta = it->second;
        uint64_t dt_ms = ctx->now_ms - delta.prev_timestamp_ms;
        if (dt_ms > 0) {
            // Bitrate: byte delta -> kilobits per second.
            if (om.total_bytes >= delta.prev_bytes) {
                uint64_t byte_delta = om.total_bytes - delta.prev_bytes;
                // kbps = (bytes * 8 / 1000) / (dt_ms / 1000) = bytes * 8 / dt_ms
                om.bitrate_kbps = static_cast<uint32_t>(
                    (byte_delta * 8ULL) / dt_ms);
            }

            // FPS: frame delta over time.
            if (om.total_frames >= delta.prev_frames) {
                int frame_delta = om.total_frames - delta.prev_frames;
                om.fps = static_cast<float>(frame_delta) /
                         (static_cast<float>(dt_ms) / 1000.0f);
            }
        }
        delta.prev_bytes = om.total_bytes;
        delta.prev_frames = om.total_frames;
        delta.prev_timestamp_ms = ctx->now_ms;
    } else {
        // First observation — seed the delta tracker, no rate yet.
        MetricsCollector::OutputDelta new_delta;
        new_delta.prev_bytes = om.total_bytes;
        new_delta.prev_frames = om.total_frames;
        new_delta.prev_timestamp_ms = ctx->now_ms;
        (*ctx->deltas)[name] = new_delta;
    }

    ctx->outputs->push_back(std::move(om));
    return true;  // continue enumeration
}

#endif  // AEGIS_OBS_PLUGIN_BUILD

void MetricsCollector::CollectOutputs() {
#if defined(AEGIS_OBS_PLUGIN_BUILD)
    std::vector<OutputMetrics> outputs;

    // Encoding lag proxy from average render frame time.
    float encoding_lag_ms = static_cast<float>(
        obs_get_average_frame_time_ns() / 1000000.0);

    OutputEnumContext ctx;
    ctx.self = this;
    ctx.now_ms = current_.timestamp_unix_ms;
    ctx.encoding_lag_ms = encoding_lag_ms;
    ctx.outputs = &outputs;
    ctx.deltas = &output_deltas_;

    obs_enum_outputs(OutputEnumCallback, &ctx);

    // Prune stale delta entries for outputs that disappeared.
    // Collect keys to remove first (avoid mutating during iteration).
    std::vector<std::string> stale_keys;
    for (const auto& kv : output_deltas_) {
        bool found = false;
        for (const auto& om : outputs) {
            if (om.name == kv.first) {
                found = true;
                break;
            }
        }
        if (!found) {
            stale_keys.push_back(kv.first);
        }
    }
    for (const auto& key : stale_keys) {
        output_deltas_.erase(key);
    }

    current_.outputs = std::move(outputs);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// System metrics (CPU, memory, GPU)
// ─────────────────────────────────────────────────────────────────────────────

void MetricsCollector::CollectSystem() {
    // ── CPU utilization via GetSystemTimes delta ─────────────────────────
    FILETIME idle_ft{}, kernel_ft{}, user_ft{};
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        uint64_t idle = FileTimeToU64(idle_ft);
        uint64_t kernel = FileTimeToU64(kernel_ft);
        uint64_t user = FileTimeToU64(user_ft);
        uint64_t total = kernel + user;  // kernel already includes idle

        if (prev_total_ > 0) {
            uint64_t total_delta = total - prev_total_;
            uint64_t idle_delta = idle - prev_idle_;
            if (total_delta > 0) {
                current_.system.cpu_percent = 100.0f *
                    (1.0f - static_cast<float>(idle_delta) / static_cast<float>(total_delta));
            }
        }

        prev_idle_ = idle;
        prev_total_ = total;
    }

    // ── Memory utilization ──────────────────────────────────────────────
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&mem)) {
        current_.system.mem_percent = static_cast<float>(mem.dwMemoryLoad);
    }

    // ── GPU via NVML ────────────────────────────────────────────────────
    if (nvml_available_ && nvml_device_) {
        // nvmlUtilization_t: { unsigned int gpu; unsigned int memory; }
        struct {
            unsigned int gpu;
            unsigned int memory;
        } util{};

        int ret = nvml_get_util_(nvml_device_, &util);
        if (ret == 0) {
            current_.system.gpu_percent = static_cast<float>(util.gpu);
        } else {
            // Permanent failure — disable for the rest of the session.
            METRICS_LOG_WARN("nvmlDeviceGetUtilizationRates failed (ret=%d) — disabling GPU metrics", ret);
            nvml_available_ = false;
            current_.system.gpu_percent = -1.0f;
            current_.system.gpu_temp_c = -1.0f;
            return;
        }

        unsigned int temp = 0;
        // Sensor type 0 = NVML_TEMPERATURE_GPU (GPU core).
        ret = nvml_get_temp_(nvml_device_, 0, &temp);
        if (ret == 0) {
            current_.system.gpu_temp_c = static_cast<float>(temp);
        } else {
            METRICS_LOG_WARN("nvmlDeviceGetTemperature failed (ret=%d) — disabling GPU metrics", ret);
            nvml_available_ = false;
            current_.system.gpu_percent = -1.0f;
            current_.system.gpu_temp_c = -1.0f;
        }
    } else {
        current_.system.gpu_percent = -1.0f;
        current_.system.gpu_temp_c = -1.0f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON serialisation — dock bridge status_snapshot contract
// ─────────────────────────────────────────────────────────────────────────────

std::string MetricsCollector::BuildStatusSnapshotJson(
    const std::string& mode,
    const std::string& health,
    const std::string& relay_status,
    const std::string& relay_region,
    const aegis::RelaySession* relay_session,
    const aegis::RelayStats* relay_stats,
    const aegis::PerLinkSnapshot* per_link_stats,
    const aegis::ConnectionSnapshot* connection_snapshot) const
{
    const auto& snap = current_;

    // Derive state_mode from mode.
    std::string state_mode;
    if (mode == "studio") {
        state_mode = "STUDIO";
    } else if (mode == "irl") {
        state_mode = "IRL_ACTIVE";
    } else {
        state_mode = "UNKNOWN";
    }

    // Sum active output bitrates for top-level bitrate_kbps.
    uint32_t total_bitrate_kbps = 0;
    for (const auto& out : snap.outputs) {
        if (out.active) {
            total_bitrate_kbps += out.bitrate_kbps;
        }
    }

    std::ostringstream os;
    os << "{";

    // ── Top-level fields ─────────────────────────────────────────────────
    os << "\"mode\":\"" << JsonEscape(mode) << "\",";
    os << "\"state_mode\":\"" << JsonEscape(state_mode) << "\",";
    os << "\"health\":\"" << JsonEscape(health) << "\",";
    os << "\"bitrate_kbps\":" << total_bitrate_kbps << ",";
    os << "\"rtt_ms\":0,";

    // ── Relay ────────────────────────────────────────────────────────────
    os << "\"relay\":{";
    os << "\"status\":\"" << JsonEscape(relay_status) << "\",";
    if (relay_region.empty()) {
        os << "\"region\":null,";
    } else {
        os << "\"region\":\"" << JsonEscape(relay_region) << "\",";
    }
    os << "\"grace_remaining_seconds\":0";
    if (relay_session) {
        os << ",\"public_ip\":\"" << JsonEscape(relay_session->public_ip) << "\"";
        os << ",\"srt_port\":" << relay_session->srt_port;
        os << ",\"grace_window_seconds\":" << relay_session->grace_window_seconds;
        os << ",\"max_session_seconds\":" << relay_session->max_session_seconds;
        if (!relay_session->stream_token.empty()) {
            os << ",\"stream_token\":\"" << JsonEscape(relay_session->stream_token) << "\"";
        }
    }
    os << "},";

    // ── Relay connection info (top-level for bridge reliability) ─────────
    if (relay_session) {
        os << "\"relay_public_ip\":\"" << JsonEscape(relay_session->public_ip) << "\",";
        os << "\"relay_srt_port\":" << relay_session->srt_port << ",";
        if (!relay_session->relay_hostname.empty()) {
            os << "\"relay_hostname\":\"" << JsonEscape(relay_session->relay_hostname) << "\",";
        }
        if (!relay_session->stream_token.empty()) {
            os << "\"relay_stream_token\":\"" << JsonEscape(relay_session->stream_token) << "\",";
        }
    }

    // ── Relay telemetry (from SLS stats) ─────────────────────────────────
    if (relay_stats && relay_stats->available) {
        os << "\"relay_ingest_bitrate_kbps\":" << relay_stats->bitrate_kbps << ",";
        os << "\"relay_rtt_ms\":" << relay_stats->rtt_ms << ",";
        os << "\"relay_pkt_loss\":" << relay_stats->pkt_loss << ",";
        os << "\"relay_pkt_drop\":" << relay_stats->pkt_drop << ",";
        os << "\"relay_recv_rate_mbps\":" << relay_stats->recv_rate_mbps << ",";
        os << "\"relay_bandwidth_mbps\":" << relay_stats->bandwidth_mbps << ",";
        os << "\"relay_latency_ms\":" << relay_stats->latency_ms << ",";
        os << "\"relay_uptime_seconds\":" << relay_stats->uptime_seconds << ",";
        os << "\"relay_stats_available\":true,";
    } else {
        os << "\"relay_stats_available\":false,";
    }

    // ── Per-link relay telemetry (from srtla_rec fork) ───────────────
    if (per_link_stats && per_link_stats->available && !per_link_stats->links.empty()) {
        os << "\"relay_per_link_available\":true,";
        os << "\"relay_conn_count\":" << per_link_stats->conn_count << ",";
        os << "\"relay_links\":[";
        for (size_t i = 0; i < per_link_stats->links.size(); ++i) {
            if (i > 0) os << ",";
            const auto& link = per_link_stats->links[i];
            os << "{";
            os << "\"addr\":\"" << JsonEscape(link.addr) << "\",";
            os << "\"bytes\":" << link.bytes << ",";
            os << "\"pkts\":" << link.pkts << ",";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.1f", link.share_pct);
            os << "\"share_pct\":" << buf << ",";
            os << "\"last_ms_ago\":" << link.last_ms_ago << ",";
            os << "\"uptime_s\":" << link.uptime_s;
            if (!link.asn_org.empty()) {
                os << ",\"asn_org\":\"" << JsonEscape(link.asn_org) << "\"";
            }
            os << "}";
        }
        os << "],";
    } else {
        os << "\"relay_per_link_available\":false,";
    }

    if (connection_snapshot && connection_snapshot->available && !connection_snapshot->items.empty()) {
        os << "\"connections\":[";
        for (size_t i = 0; i < connection_snapshot->items.size(); ++i) {
            if (i > 0) os << ",";
            const auto& conn = connection_snapshot->items[i];
            os << "{";
            os << "\"id\":\"" << JsonEscape(conn.id) << "\",";
            os << "\"name\":\"" << JsonEscape(conn.name) << "\",";
            os << "\"type\":\"" << JsonEscape(conn.type) << "\",";
            os << "\"signal\":" << conn.signal << ",";
            os << "\"bitrate_kbps\":" << conn.bitrate_kbps << ",";
            os << "\"status\":\"" << JsonEscape(conn.status) << "\"";
            if (!conn.addr.empty()) {
                os << ",\"addr\":\"" << JsonEscape(conn.addr) << "\"";
            }
            if (!conn.asn_org.empty()) {
                os << ",\"asn_org\":\"" << JsonEscape(conn.asn_org) << "\"";
            }
            os << "}";
        }
        os << "],";
    } else {
        os << "\"connections\":[],";
    }

    // ── Multistream outputs ──────────────────────────────────────────────
    os << "\"multistream_outputs\":[";
    for (size_t i = 0; i < snap.outputs.size(); ++i) {
        if (i > 0) os << ",";
        const auto& out = snap.outputs[i];
        os << "{";
        os << "\"name\":\"" << JsonEscape(out.name) << "\",";
        os << "\"bitrate_kbps\":" << out.bitrate_kbps << ",";

        // Format floats with controlled precision.
        char buf[64];

        std::snprintf(buf, sizeof(buf), "%.4f", out.drop_pct);
        os << "\"drop_pct\":" << buf << ",";

        std::snprintf(buf, sizeof(buf), "%.2f", out.fps);
        os << "\"fps\":" << buf << ",";

        std::snprintf(buf, sizeof(buf), "%.1f", out.encoding_lag_ms);
        os << "\"encoding_lag_ms\":" << buf << ",";

        os << "\"encoder\":\"" << JsonEscape(out.encoder_name) << "\",";
        os << "\"codec\":\"" << JsonEscape(out.encoder_codec) << "\",";
        os << "\"width\":" << out.width << ",";
        os << "\"height\":" << out.height << ",";
        os << "\"active\":" << (out.active ? "true" : "false") << ",";

        std::snprintf(buf, sizeof(buf), "%.4f", out.congestion);
        os << "\"congestion\":" << buf;

        os << "}";
    }
    os << "],";

    // ── OBS stats ────────────────────────────────────────────────────────
    os << "\"obs_stats\":{";

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", snap.obs.active_fps);
        os << "\"active_fps\":" << buf << ",";
    }

    os << "\"render_missed_frames\":" << snap.obs.render_missed_frames << ",";
    os << "\"render_total_frames\":" << snap.obs.render_total_frames << ",";

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", snap.obs.available_disk_space_mb);
        os << "\"available_disk_space_mb\":" << buf;
    }

    os << "},";

    // ── System ───────────────────────────────────────────────────────────
    os << "\"system\":{";

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", snap.system.cpu_percent);
        os << "\"cpu_percent\":" << buf << ",";
    }

    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", snap.system.mem_percent);
        os << "\"mem_percent\":" << buf << ",";
    }

    // GPU values: null when -1.0 (unavailable).
    if (snap.system.gpu_percent < 0.0f) {
        os << "\"gpu_percent\":null,";
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", snap.system.gpu_percent);
        os << "\"gpu_percent\":" << buf << ",";
    }

    if (snap.system.gpu_temp_c < 0.0f) {
        os << "\"gpu_temp_c\":null";
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", snap.system.gpu_temp_c);
        os << "\"gpu_temp_c\":" << buf;
    }

    os << "}";

    os << "}";
    return os.str();
}

}  // namespace aegis
