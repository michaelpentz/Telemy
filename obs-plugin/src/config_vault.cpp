#include "config_vault.h"

#include <obs-module.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

// Link against crypt32.lib for CryptProtectData / CryptUnprotectData /
// CryptBinaryToStringA / CryptStringToBinaryA.  (Also listed in CMakeLists.)
#pragma comment(lib, "crypt32.lib")

namespace aegis {

bool IsExplicitInsecureHttpHost(const std::string& value)
{
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        start += 1;
    }
    if (value.size() - start < 7) {
        return false;
    }

    static const char kHttpPrefix[] = "http://";
    for (size_t i = 0; i < 7; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[start + i]);
        if (std::tolower(ch) != kHttpPrefix[i]) {
            return false;
        }
    }
    return true;
}

// ===== Vault helpers =======================================================

std::string Vault::VaultDirPath() const
{
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        return std::string();

    // Convert wide path to UTF-8 via WideCharToMultiByte.
    int needed = WideCharToMultiByte(CP_UTF8, 0, appdata, -1,
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, appdata, -1,
                        utf8.data(), needed, nullptr, nullptr);
    // Remove trailing NUL that WideCharToMultiByte includes.
    if (!utf8.empty() && utf8.back() == '\0')
        utf8.pop_back();

    return utf8 + "\\Telemy";
}

std::string Vault::VaultFilePath() const
{
    std::string dir = VaultDirPath();
    if (dir.empty())
        return std::string();
    return dir + "\\vault.json";
}

bool Vault::EnsureDir() const
{
    std::string dir = VaultDirPath();
    if (dir.empty())
        return false;

    // Convert to wide for CreateDirectoryW.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return false;
    std::wstring wdir(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir.data(), wlen);

    if (!CreateDirectoryW(wdir.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

// ===== DPAPI encrypt / decrypt =============================================

std::string Vault::DpapiEncrypt(const std::string& plaintext)
{
    DATA_BLOB dataIn = {};
    dataIn.cbData = static_cast<DWORD>(plaintext.size());
    dataIn.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(plaintext.data()));

    DATA_BLOB dataOut = {};
    if (!CryptProtectData(&dataIn, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        blog(LOG_WARNING, "vault: CryptProtectData failed (err=%lu)",
             GetLastError());
        return std::string();
    }

    // Base64-encode the encrypted blob.
    DWORD b64Len = 0;
    if (!CryptBinaryToStringA(dataOut.pbData, dataOut.cbData,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &b64Len)) {
        LocalFree(dataOut.pbData);
        blog(LOG_WARNING, "vault: CryptBinaryToStringA size query failed");
        return std::string();
    }

    std::string base64(static_cast<size_t>(b64Len), '\0');
    if (!CryptBinaryToStringA(dataOut.pbData, dataOut.cbData,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              base64.data(), &b64Len)) {
        LocalFree(dataOut.pbData);
        blog(LOG_WARNING, "vault: CryptBinaryToStringA encode failed");
        return std::string();
    }

    LocalFree(dataOut.pbData);

    // Trim trailing NUL if the API wrote one inside the buffer.
    while (!base64.empty() && base64.back() == '\0')
        base64.pop_back();

    return base64;
}

std::optional<std::string> Vault::DpapiDecrypt(const std::string& base64blob)
{
    if (base64blob.empty())
        return std::nullopt;

    // Decode base64 -> raw encrypted bytes.
    DWORD rawLen = 0;
    if (!CryptStringToBinaryA(base64blob.c_str(),
                              static_cast<DWORD>(base64blob.size()),
                              CRYPT_STRING_BASE64, nullptr, &rawLen,
                              nullptr, nullptr)) {
        blog(LOG_WARNING, "vault: CryptStringToBinaryA size query failed");
        return std::nullopt;
    }

    std::vector<BYTE> raw(static_cast<size_t>(rawLen));
    if (!CryptStringToBinaryA(base64blob.c_str(),
                              static_cast<DWORD>(base64blob.size()),
                              CRYPT_STRING_BASE64, raw.data(), &rawLen,
                              nullptr, nullptr)) {
        blog(LOG_WARNING, "vault: CryptStringToBinaryA decode failed");
        return std::nullopt;
    }

    // DPAPI decrypt.
    DATA_BLOB dataIn = {};
    dataIn.cbData = rawLen;
    dataIn.pbData = raw.data();

    DATA_BLOB dataOut = {};
    if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        blog(LOG_WARNING, "vault: CryptUnprotectData failed (err=%lu)",
             GetLastError());
        return std::nullopt;
    }

    std::string result(reinterpret_cast<char*>(dataOut.pbData),
                       static_cast<size_t>(dataOut.cbData));
    LocalFree(dataOut.pbData);
    return result;
}

// ===== Vault public API ====================================================

// Internal save (caller must already hold mutex_).
static bool SaveLocked(const std::map<std::string, std::string>& entries,
                       const std::string& path, bool ensureDir,
                       const std::function<bool()>& ensureDirFn)
{
    if (ensureDir && !ensureDirFn()) {
        blog(LOG_WARNING, "vault: failed to create vault directory");
        return false;
    }

    if (path.empty())
        return false;

    QJsonObject obj;
    for (const auto& [k, v] : entries)
        obj.insert(QString::fromStdString(k), QString::fromStdString(v));

    QJsonDocument doc(obj);
    QByteArray json = doc.toJson(QJsonDocument::Indented);

    QSaveFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        blog(LOG_WARNING, "vault: failed to open %s for writing",
             path.c_str());
        return false;
    }

    file.write(json);
    return file.commit();
}

bool Vault::Load()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string path = VaultFilePath();
    if (path.empty()) {
        blog(LOG_WARNING, "vault: could not determine vault file path");
        return false;
    }

    QFile file(QString::fromStdString(path));
    if (!file.exists()) {
        // No vault file yet — start empty, not an error.
        entries_.clear();
        return true;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        blog(LOG_WARNING, "vault: failed to open %s for reading",
             path.c_str());
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        blog(LOG_WARNING, "vault: failed to parse %s: %s", path.c_str(),
             parseErr.errorString().toStdString().c_str());
        return false;
    }

    entries_.clear();
    QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.value().isString()) {
            entries_[it.key().toStdString()] =
                it.value().toString().toStdString();
        }
    }

    blog(LOG_INFO, "vault: loaded %zu entries from %s",
         entries_.size(), path.c_str());
    return true;
}

bool Vault::Save()
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string path = VaultFilePath();
    return SaveLocked(entries_, path, true,
                      [this]() { return EnsureDir(); });
}

bool Vault::Set(const std::string& key, const std::string& plaintext)
{
    // DpapiEncrypt is static and does not touch entries_; call outside lock.
    std::string blob = DpapiEncrypt(plaintext);
    if (blob.empty())
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = blob;

    std::string path = VaultFilePath();
    return SaveLocked(entries_, path, true,
                      [this]() { return EnsureDir(); });
}

std::optional<std::string> Vault::Get(const std::string& key) const
{
    std::string blob;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end())
            return std::nullopt;
        blob = it->second;
    }
    // DpapiDecrypt is static; safe to call without lock.
    return DpapiDecrypt(blob);
}

bool Vault::Remove(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.erase(key) == 0)
        return false;

    std::string path = VaultFilePath();
    return SaveLocked(entries_, path, true,
                      [this]() { return EnsureDir(); });
}

std::vector<std::string> Vault::Keys() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& [k, v] : entries_)
        keys.push_back(k);
    // std::map is already sorted, but be explicit.
    std::sort(keys.begin(), keys.end());
    return keys;
}

// ===== PluginConfig ========================================================

// Reuse the same %APPDATA%/Telemy directory logic.
static std::string TelmeyAppDataDir()
{
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        return std::string();

    int needed = WideCharToMultiByte(CP_UTF8, 0, appdata, -1,
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, appdata, -1,
                        utf8.data(), needed, nullptr, nullptr);
    if (!utf8.empty() && utf8.back() == '\0')
        utf8.pop_back();
    return utf8 + "\\Telemy";
}

static bool EnsureTelmeyDir()
{
    std::string dir = TelmeyAppDataDir();
    if (dir.empty())
        return false;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return false;
    std::wstring wdir(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir.data(), wlen);
    if (!CreateDirectoryW(wdir.c_str(), nullptr)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

std::string PluginConfig::ConfigFilePath()
{
    std::string dir = TelmeyAppDataDir();
    if (dir.empty())
        return std::string();
    return dir + "\\config.json";
}

bool PluginConfig::LoadFromDisk()
{
    std::string path = ConfigFilePath();
    if (path.empty()) {
        blog(LOG_WARNING, "config: could not determine config file path");
        return false;
    }

    QFile file(QString::fromStdString(path));
    if (!file.exists()) {
        // No config file yet — keep defaults, not an error.
        blog(LOG_INFO, "config: %s does not exist, using defaults",
             path.c_str());
        return true;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        blog(LOG_WARNING, "config: failed to open %s", path.c_str());
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        blog(LOG_WARNING, "config: parse error in %s: %s", path.c_str(),
             parseErr.errorString().toStdString().c_str());
        return false;
    }

    // Snapshot the raw JSON for round-trip preservation of unknown keys.
    raw_json_snapshot_ = QString(doc.toJson(QJsonDocument::Compact))
                             .toStdString();

    QJsonObject obj = doc.object();

    // Read known fields; missing keys silently keep their defaults.
    if (obj.contains("relay_api_host") && obj["relay_api_host"].isString())
        relay_api_host = obj["relay_api_host"].toString().toStdString();
    if (IsExplicitInsecureHttpHost(relay_api_host)) {
        blog(LOG_WARNING,
             "config: relay_api_host uses insecure http://; update to https:// or a bare host");
    }

    if (obj.contains("relay_heartbeat_interval_sec") &&
        obj["relay_heartbeat_interval_sec"].isDouble())
        relay_heartbeat_interval_sec =
            obj["relay_heartbeat_interval_sec"].toInt(30);

    if (obj.contains("metrics_poll_interval_ms") &&
        obj["metrics_poll_interval_ms"].isDouble())
        metrics_poll_interval_ms =
            obj["metrics_poll_interval_ms"].toInt(500);

    if (obj.contains("grafana_enabled") && obj["grafana_enabled"].isBool())
        grafana_enabled = obj["grafana_enabled"].toBool(false);

    if (obj.contains("grafana_otlp_endpoint") &&
        obj["grafana_otlp_endpoint"].isString())
        grafana_otlp_endpoint =
            obj["grafana_otlp_endpoint"].toString().toStdString();

    if (obj.contains("dock_mode") && obj["dock_mode"].isString()) {
        const std::string mode = obj["dock_mode"].toString().toStdString();
        if (mode == "studio" || mode == "irl") {
            dock_mode = mode;
        }
    }

    if (obj.contains("auto_scene_switch") && obj["auto_scene_switch"].isBool())
        auto_scene_switch = obj["auto_scene_switch"].toBool(false);

    if (obj.contains("low_quality_fallback") && obj["low_quality_fallback"].isBool())
        low_quality_fallback = obj["low_quality_fallback"].toBool(false);

    if (obj.contains("manual_override") && obj["manual_override"].isBool())
        manual_override = obj["manual_override"].toBool(false);

    if (obj.contains("chat_bot") && obj["chat_bot"].isBool())
        chat_bot = obj["chat_bot"].toBool(false);

    if (obj.contains("alerts") && obj["alerts"].isBool())
        alerts = obj["alerts"].toBool(false);

    blog(LOG_INFO, "config: loaded from %s", path.c_str());
    return true;
}

bool PluginConfig::SaveToDisk()
{
    if (!EnsureTelmeyDir()) {
        blog(LOG_WARNING, "config: failed to create config directory");
        return false;
    }

    std::string path = ConfigFilePath();
    if (path.empty())
        return false;

    // Start from the last-read raw JSON so unknown keys are preserved.
    QJsonObject obj;
    if (!raw_json_snapshot_.empty()) {
        QJsonDocument prev = QJsonDocument::fromJson(
            QByteArray::fromStdString(raw_json_snapshot_));
        if (prev.isObject())
            obj = prev.object();
    }

    // Overwrite known fields.
    obj.remove("relay_shared_key");  // Migrated to DPAPI vault.json.
    obj["relay_api_host"] =
        QString::fromStdString(relay_api_host);
    obj["relay_heartbeat_interval_sec"] = relay_heartbeat_interval_sec;
    obj["metrics_poll_interval_ms"]     = metrics_poll_interval_ms;
    obj["grafana_enabled"]              = grafana_enabled;
    obj["grafana_otlp_endpoint"] =
        QString::fromStdString(grafana_otlp_endpoint);
    obj["dock_mode"] = QString::fromStdString(dock_mode);
    obj["auto_scene_switch"] = auto_scene_switch;
    obj["low_quality_fallback"] = low_quality_fallback;
    obj["manual_override"] = manual_override;
    obj["chat_bot"] = chat_bot;
    obj["alerts"] = alerts;

    QJsonDocument doc(obj);
    QByteArray json = doc.toJson(QJsonDocument::Indented);

    QSaveFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        blog(LOG_WARNING, "config: failed to open %s for writing",
             path.c_str());
        return false;
    }

    file.write(json);
    if (!file.commit()) {
        blog(LOG_WARNING, "config: atomic commit failed for %s", path.c_str());
        return false;
    }

    // Update snapshot so the next save preserves what we just wrote.
    raw_json_snapshot_ = QString(doc.toJson(QJsonDocument::Compact))
                             .toStdString();

    blog(LOG_INFO, "config: saved to %s", path.c_str());
    return true;
}

} // namespace aegis
