#include "bandwidth_checker.h"

#include <obs.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <numeric>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// ---------------------------------------------------------------------------
// EnumerateOutputs — iterate OBS outputs and read encoder bitrate
// ---------------------------------------------------------------------------

std::vector<OutputBandwidthInfo> BandwidthChecker::EnumerateOutputs() {
    std::vector<OutputBandwidthInfo> results;

    // OBS output enumeration callback
    auto enum_cb = [](void* param, obs_output_t* output) -> bool {
        auto* vec = static_cast<std::vector<OutputBandwidthInfo>*>(param);

        OutputBandwidthInfo info;
        info.name = obs_output_get_name(output);
        info.active = obs_output_active(output);

        // Get bitrate from the output's video encoder
        obs_encoder_t* venc = obs_output_get_video_encoder(output);
        if (venc) {
            obs_data_t* enc_settings = obs_encoder_get_settings(venc);
            if (enc_settings) {
                info.bitrate_kbps = (int)obs_data_get_int(enc_settings, "bitrate");
                obs_data_release(enc_settings);
            }
        }

        if (info.bitrate_kbps > 0) {
            vec->push_back(info);
        }
        return true; // continue enumeration
    };

    obs_enum_outputs(enum_cb, &results);

    // Detect platforms after enumeration (callback can't call member fn)
    for (auto& info : results) {
        info.platform = DetectPlatform(info.name, "");
    }

    // Sort by bitrate descending for readability
    std::sort(results.begin(), results.end(),
              [](const OutputBandwidthInfo& a, const OutputBandwidthInfo& b) {
                  return a.bitrate_kbps > b.bitrate_kbps;
              });

    return results;
}

// ---------------------------------------------------------------------------
// DetectPlatform — simple string matching on output name / type
// ---------------------------------------------------------------------------

std::string BandwidthChecker::DetectPlatform(const std::string& output_name,
                                              const std::string& output_type) {
    std::string name_lower = ToLower(output_name);
    std::string type_lower = ToLower(output_type);
    std::string combined = name_lower + " " + type_lower;

    if (combined.find("twitch") != std::string::npos)
        return "Twitch";
    if (combined.find("youtube") != std::string::npos || combined.find("yt") != std::string::npos)
        return "YouTube";
    if (combined.find("tiktok") != std::string::npos || combined.find("tik") != std::string::npos)
        return "TikTok";
    if (combined.find("kick") != std::string::npos)
        return "Kick";
    if (combined.find("instagram") != std::string::npos || combined.find("ig") != std::string::npos)
        return "Instagram";
    if (combined.find("facebook") != std::string::npos || combined.find("fb") != std::string::npos)
        return "Facebook";

    return "Stream";
}

// ---------------------------------------------------------------------------
// MeasureDownload / MeasureUpload
//
// Delegates to the control plane speed-test endpoints via HttpsClient.
// When speed_test_url_ is empty the test is skipped (returns 0).
//
// Full implementation will use HttpsClient to GET/POST the speed-test
// endpoint and time the transfer.  For now we return 0 so callers get a
// safe "unknown" verdict until the control-plane endpoint is wired up.
// ---------------------------------------------------------------------------

double BandwidthChecker::MeasureDownload() {
    if (speed_test_url_.empty())
        return 0.0;

    // TODO: GET speed_test_url_/download (1 MB payload), time the transfer,
    //       return bytes/time converted to Mbps.  Use HttpsClient with a
    //       10-second timeout.
    return 0.0;
}

double BandwidthChecker::MeasureUpload() {
    if (speed_test_url_.empty())
        return 0.0;

    // TODO: POST random payload to speed_test_url_/upload, time the
    //       transfer, return bytes/time converted to Mbps.  Use HttpsClient
    //       with a 10-second timeout.
    return 0.0;
}

// ---------------------------------------------------------------------------
// CalculateHeadroom — compare upload bandwidth against total output bitrate
// ---------------------------------------------------------------------------

void BandwidthChecker::CalculateHeadroom(BandwidthReport& report) {
    report.total_output_kbps = 0;
    for (const auto& out : report.outputs) {
        report.total_output_kbps += out.bitrate_kbps;
    }

    if (report.upload_mbps <= 0) {
        report.verdict = "unknown";
        report.message = "Speed test not available — bandwidth headroom cannot be calculated.";
        return;
    }

    double total_output_mbps = report.total_output_kbps / 1000.0;
    report.headroom_mbps = report.upload_mbps - total_output_mbps;

    if (report.headroom_mbps > 20.0) {
        report.verdict = "plenty";
        report.message = "Plenty of bandwidth.";
    } else if (report.headroom_mbps > 10.0) {
        report.verdict = "ok";
        report.message = "Good bandwidth headroom.";
    } else if (report.headroom_mbps > 3.0) {
        report.verdict = "tight";
        report.message = "Tight. You may experience drops if upload fluctuates.";
    } else {
        report.verdict = "insufficient";
        report.message = "Insufficient upload bandwidth for your configured outputs.";
    }
}

// ---------------------------------------------------------------------------
// RunCheck — full bandwidth diagnostics (blocks the calling thread)
// ---------------------------------------------------------------------------

BandwidthReport BandwidthChecker::RunCheck() {
    BandwidthReport report;

    report.outputs = EnumerateOutputs();
    report.download_mbps = MeasureDownload();
    report.upload_mbps = MeasureUpload();

    CalculateHeadroom(report);

    report.success = true;
    return report;
}
