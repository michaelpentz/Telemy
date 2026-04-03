#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

struct OutputBandwidthInfo {
    std::string name;
    std::string platform;    // detected from output name/type
    int bitrate_kbps = 0;
    bool active = false;
};

struct BandwidthReport {
    // Measured speeds (Mbps)
    double download_mbps = 0.0;
    double upload_mbps = 0.0;

    // OBS output analysis
    std::vector<OutputBandwidthInfo> outputs;
    int total_output_kbps = 0;

    // Headroom
    double headroom_mbps = 0.0;
    std::string verdict;   // "plenty", "ok", "tight", "insufficient"
    std::string message;

    // Error
    bool success = false;
    std::string error;
};

class BandwidthChecker {
public:
    // Run full bandwidth check (BLOCKS — call from worker thread only)
    BandwidthReport RunCheck();

    // Just enumerate OBS outputs without speed test
    std::vector<OutputBandwidthInfo> EnumerateOutputs();

    // Set speed test endpoint (control plane URL)
    void SetSpeedTestEndpoint(const std::string& url) { speed_test_url_ = url; }

private:
    std::string speed_test_url_;

    double MeasureDownload();
    double MeasureUpload();
    std::string DetectPlatform(const std::string& output_name, const std::string& output_type);
    void CalculateHeadroom(BandwidthReport& report);
};
