#pragma once

#include <mutex>
#include <string>
#include <vector>

#if defined(TELEMY_OBS_PLUGIN_BUILD)
#include <QJsonObject>
#endif

namespace telemy {

struct ChatbotRuleConfig {
    std::string id;
    std::string label;
    std::string linked_scene_name;
    std::vector<std::string> aliases;
};

struct ChatbotCommandRequest {
    std::string provider;
    std::string channel;
    std::string sender_name;
    std::string sender_role;
    std::string message_text;
};

struct ChatbotCommandResult {
    bool handled = false;
    bool ok = false;
    bool mutates_state = false;
    bool pause_automation = false;
    bool resume_automation = false;
    bool wants_status_reply = false;
    std::string action;
    std::string error_code;
    std::string scene_name;
    std::string reply_text;
    std::string matched_command;
};

class ChatbotRuntime {
public:
    void LoadPrefsFromDisk();
    bool ApplyPrefsJson(const std::string& prefs_json);

#if defined(TELEMY_OBS_PLUGIN_BUILD)
    QJsonObject BuildSnapshotJson(bool enabled) const;
#endif

    ChatbotCommandResult HandleCommand(
        bool enabled,
        const ChatbotCommandRequest& request) const;

    std::string GetCommandPrefix() const;
    std::string GetAllowedRole() const;
    bool GetAnnounceSceneSwitches() const;
    bool GetAnnounceAutoResume() const;
    bool GetSendStatusReplies() const;
    void SetRuntimeStatus(const std::string& status, const std::string& label);

#if defined(TELEMY_OBS_PLUGIN_BUILD)
    QJsonArray GetRulesJson() const;
#endif

private:
    struct Config {
        std::string provider = "twitch";
        std::string channel;
        std::string command_prefix = "!telemy";
        bool announce_scene_switches = true;
        bool announce_auto_resume = true;
        bool send_status_replies = true;
        bool broadcaster_only = true;
        std::string allowed_role = "broadcaster";
        std::string identity_label = "@TelemyBot";
        std::string runtimeStatus;
        std::string runtimeLabel;
        std::vector<ChatbotRuleConfig> rules;
    };

    static std::string PrefsFilePath();
    static std::string TrimCopy(const std::string& value);
    static std::string ToLowerCopy(const std::string& value);

    mutable std::mutex mu_;
    Config config_;
    bool prefs_loaded_ = false;
};

} // namespace telemy
