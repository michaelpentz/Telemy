#pragma once

// DPAPI-based secret vault and JSON config manager for the Aegis OBS plugin.
// Windows-only: uses CryptProtectData / CryptUnprotectData for per-user encryption.
// Thread-safe: all public methods are guarded by internal mutexes.

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aegis {

// Returns true when a host string explicitly opts out of TLS via an
// "http://" prefix. Bare hosts and "https://" hosts return false.
bool IsExplicitInsecureHttpHost(const std::string& value);

// ---------------------------------------------------------------------------
// Vault — DPAPI-encrypted secret store persisted to %APPDATA%/Telemy/vault.json
// ---------------------------------------------------------------------------
class Vault {
public:
    // Load vault.json from disk into memory. Returns true on success or if
    // the file does not yet exist (starts with empty entries). Returns false
    // only on I/O or parse errors that leave the vault in an indeterminate state.
    bool Load();

    // Write the current in-memory entries to vault.json. Creates the directory
    // and file if they do not exist. Returns true on success.
    bool Save();

    // Encrypt |plaintext| with DPAPI and store under |key|. Automatically
    // persists to disk. Returns true on success.
    bool Set(const std::string& key, const std::string& plaintext);

    // Decrypt and return the value for |key|, or std::nullopt if the key is
    // absent or decryption fails.
    std::optional<std::string> Get(const std::string& key) const;

    // Remove |key| from the vault and persist. Returns true if the key existed.
    bool Remove(const std::string& key);

    // Return a sorted list of all stored key names.
    std::vector<std::string> Keys() const;

private:
    std::string VaultDirPath() const;   // %APPDATA%/Telemy
    std::string VaultFilePath() const;  // %APPDATA%/Telemy/vault.json
    bool EnsureDir() const;             // Create directory if missing

    // DPAPI helpers — static, no mutex needed.
    static std::string DpapiEncrypt(const std::string& plaintext);
    static std::optional<std::string> DpapiDecrypt(const std::string& base64blob);

    mutable std::mutex mutex_;
    std::map<std::string, std::string> entries_;  // key -> base64 DPAPI blob
};

// ---------------------------------------------------------------------------
// PluginConfig — JSON config persisted to %APPDATA%/Telemy/config.json
// ---------------------------------------------------------------------------
struct PluginConfig {
    std::string relay_api_host;                   // e.g. "api.aegis.example.com"
    int relay_heartbeat_interval_sec = 30;
    int metrics_poll_interval_ms     = 500;
    bool grafana_enabled             = false;     // future, default off
    std::string grafana_otlp_endpoint;            // future
    std::string dock_mode            = "studio";
    bool auto_scene_switch           = false;
    bool low_quality_fallback        = false;
    bool manual_override             = false;
    bool chat_bot                    = false;
    bool alerts                      = false;

    // Load config.json from %APPDATA%/Telemy/. Missing keys receive defaults;
    // returns true on success or if the file does not exist yet.
    bool LoadFromDisk();

    // Save to config.json. Extra keys already present in the file are preserved
    // (round-trip safe). Returns true on success.
    bool SaveToDisk();

    // Returns the full path to %APPDATA%/Telemy/config.json.
    static std::string ConfigFilePath();

private:
    // Serialised JSON snapshot from the last LoadFromDisk(), used to preserve
    // unknown keys across load/save cycles (round-trip safe).  Stored as a
    // string rather than QJsonObject to avoid pulling Qt headers into the
    // public header.
    std::string raw_json_snapshot_;
};

} // namespace aegis
