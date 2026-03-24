#include "chatbot_runtime.h"
#include "config_vault.h"

#if defined(AEGIS_OBS_PLUGIN_BUILD)

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace aegis {

namespace {

bool HasCommandPrefix(const std::string& text, const std::string& prefix) {
    if (prefix.empty() || text.size() < prefix.size()) {
        return false;
    }
    if (text.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return text.size() == prefix.size() ||
           std::isspace(static_cast<unsigned char>(text[prefix.size()])) != 0;
}

bool IsBroadcasterRole(const std::string& role) {
    return role == "broadcaster" || role == "streamer" || role == "owner" || role == "admin";
}

bool IsModeratorRole(const std::string& role) {
    return IsBroadcasterRole(role) || role == "moderator" || role == "mod";
}

std::string BuildRuleListLabel(const std::vector<ChatbotRuleConfig>& rules) {
    if (rules.empty()) {
        return "No scene aliases configured";
    }
    std::ostringstream os;
    bool first = true;
    for (const auto& rule : rules) {
        if (rule.aliases.empty() || rule.linked_scene_name.empty()) {
            continue;
        }
        if (!first) {
            os << ", ";
        }
        first = false;
        os << rule.aliases.front() << " -> " << rule.linked_scene_name;
    }
    return first ? "No scene aliases configured" : os.str();
}

} // namespace

std::string ChatbotRuntime::PrefsFilePath() {
    const std::string config_path = PluginConfig::ConfigFilePath();
    if (config_path.empty()) {
        return {};
    }
    QFileInfo info(QString::fromStdString(config_path));
    return info.dir().filePath(QStringLiteral("dock_scene_prefs.json")).toStdString();
}

std::string ChatbotRuntime::TrimCopy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start += 1;
    }
    std::size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end -= 1;
    }
    return value.substr(start, end - start);
}

std::string ChatbotRuntime::ToLowerCopy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

void ChatbotRuntime::LoadPrefsFromDisk() {
    const std::string prefs_path = PrefsFilePath();
    if (prefs_path.empty()) {
        return;
    }
    QFile file(QString::fromStdString(prefs_path));
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray data = file.readAll();
    file.close();
    (void)ApplyPrefsJson(data.toStdString());
}

bool ChatbotRuntime::ApplyPrefsJson(const std::string& prefs_json) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(prefs_json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject chatbot = root.value(QStringLiteral("chatbotConfig")).toObject();
    const QJsonObject scene_names = root.value(QStringLiteral("sceneIntentLinksByName")).toObject();
    const QJsonArray rules = root.value(QStringLiteral("autoSceneRules")).toArray();

    Config next;
    next.provider = chatbot.value(QStringLiteral("provider")).toString(QStringLiteral("twitch")).toStdString();
    next.channel = chatbot.value(QStringLiteral("channel")).toString().toStdString();
    next.command_prefix = chatbot.value(QStringLiteral("commandPrefix")).toString(QStringLiteral("!telemy")).toStdString();
    next.announce_scene_switches = !chatbot.contains(QStringLiteral("announceSceneSwitches")) ||
        chatbot.value(QStringLiteral("announceSceneSwitches")).toBool(true);
    next.announce_auto_resume = !chatbot.contains(QStringLiteral("announceAutoResume")) ||
        chatbot.value(QStringLiteral("announceAutoResume")).toBool(true);
    next.send_status_replies = !chatbot.contains(QStringLiteral("sendStatusReplies")) ||
        chatbot.value(QStringLiteral("sendStatusReplies")).toBool(true);
    next.broadcaster_only = !chatbot.contains(QStringLiteral("broadcasterOnly")) ||
        chatbot.value(QStringLiteral("broadcasterOnly")).toBool(true);
    next.identity_label = chatbot.value(QStringLiteral("identityLabel")).toString(
        QStringLiteral("@TelemyBot")).toStdString();

    for (const QJsonValue& rule_value : rules) {
        if (!rule_value.isObject()) {
            continue;
        }
        const QJsonObject rule_obj = rule_value.toObject();
        if (rule_obj.contains(QStringLiteral("chatEnabled")) &&
            !rule_obj.value(QStringLiteral("chatEnabled")).toBool()) {
            continue;
        }

        ChatbotRuleConfig rule;
        rule.id = rule_obj.value(QStringLiteral("id")).toString().toStdString();
        rule.label = rule_obj.value(QStringLiteral("label")).toString().toStdString();
        rule.linked_scene_name = scene_names.value(QString::fromStdString(rule.id)).toString().toStdString();

        const QJsonArray aliases = rule_obj.value(QStringLiteral("chatAliases")).toArray();
        for (const QJsonValue& alias_value : aliases) {
            const std::string alias = ToLowerCopy(TrimCopy(alias_value.toString().toStdString()));
            if (!alias.empty()) {
                rule.aliases.push_back(alias);
            }
        }

        if (!rule.aliases.empty()) {
            next.rules.push_back(std::move(rule));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        config_ = std::move(next);
        prefs_loaded_ = true;
    }
    return true;
}

QJsonObject ChatbotRuntime::BuildSnapshotJson(bool enabled) const {
    Config cfg;
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg = config_;
        loaded = prefs_loaded_;
    }

    QString runtime_status;
    QString runtime_label;
    if (!enabled) {
        runtime_status = QStringLiteral("disabled");
        runtime_label = QStringLiteral("Chatbot disabled");
    } else if (!loaded) {
        runtime_status = QStringLiteral("loading");
        runtime_label = QStringLiteral("Waiting for dock prefs");
    } else if (cfg.provider == "none") {
        runtime_status = QStringLiteral("disabled");
        runtime_label = QStringLiteral("Provider off");
    } else if (TrimCopy(cfg.channel).empty()) {
        runtime_status = QStringLiteral("config_error");
        runtime_label = QStringLiteral("Set a channel to enable commands");
    } else {
        runtime_status = QStringLiteral("ready");
        runtime_label = QStringLiteral("Native command parser ready");
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("provider"), QString::fromStdString(cfg.provider));
    obj.insert(QStringLiteral("channel"), QString::fromStdString(cfg.channel));
    obj.insert(QStringLiteral("commandPrefix"), QString::fromStdString(cfg.command_prefix));
    obj.insert(QStringLiteral("announceSceneSwitches"), cfg.announce_scene_switches);
    obj.insert(QStringLiteral("announceAutoResume"), cfg.announce_auto_resume);
    obj.insert(QStringLiteral("sendStatusReplies"), cfg.send_status_replies);
    obj.insert(QStringLiteral("broadcasterOnly"), cfg.broadcaster_only);
    obj.insert(QStringLiteral("identityLabel"), QString::fromStdString(cfg.identity_label));
    obj.insert(QStringLiteral("runtimeStatus"), runtime_status);
    obj.insert(QStringLiteral("runtimeLabel"), runtime_label);
    obj.insert(QStringLiteral("loadedRuleCount"), static_cast<int>(cfg.rules.size()));
    obj.insert(QStringLiteral("ruleSummary"), QString::fromStdString(BuildRuleListLabel(cfg.rules)));
    return obj;
}

ChatbotCommandResult ChatbotRuntime::HandleCommand(
    bool enabled,
    const ChatbotCommandRequest& request) const {
    Config cfg;
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg = config_;
        loaded = prefs_loaded_;
    }

    ChatbotCommandResult result;
    const std::string text = TrimCopy(request.message_text);
    if (!enabled || text.empty()) {
        return result;
    }
    if (!loaded) {
        result.handled = true;
        result.ok = false;
        result.error_code = "prefs_not_loaded";
        return result;
    }
    if (cfg.provider == "none") {
        result.handled = true;
        result.ok = false;
        result.error_code = "provider_disabled";
        return result;
    }
    if (!HasCommandPrefix(text, cfg.command_prefix)) {
        return result;
    }

    const std::string role = ToLowerCopy(TrimCopy(request.sender_role));
    const bool is_broadcaster = IsBroadcasterRole(role);
    const bool can_mutate = cfg.broadcaster_only ? is_broadcaster : IsModeratorRole(role);

    std::string body = TrimCopy(text.substr(cfg.command_prefix.size()));
    body = ToLowerCopy(body);

    result.handled = true;
    if (body.empty()) {
        result.ok = false;
        result.error_code = "missing_command";
        return result;
    }

    if (body == "status") {
        result.ok = cfg.send_status_replies;
        result.wants_status_reply = cfg.send_status_replies;
        result.action = "status";
        result.matched_command = "status";
        result.error_code = cfg.send_status_replies ? "" : "status_replies_disabled";
        return result;
    }

    if (!can_mutate) {
        result.ok = false;
        result.error_code = cfg.broadcaster_only ? "broadcaster_only" : "insufficient_role";
        return result;
    }

    if (body == "auto") {
        result.ok = true;
        result.mutates_state = true;
        result.resume_automation = true;
        result.action = "resume_auto";
        result.matched_command = "auto";
        if (cfg.announce_auto_resume) {
            result.reply_text = "Auto scene switching resumed.";
        }
        return result;
    }

    if (body.rfind("scene ", 0) == 0) {
        const std::size_t prefix_pos = request.message_text.find(cfg.command_prefix);
        const std::size_t scene_pos =
            prefix_pos == std::string::npos ? std::string::npos : prefix_pos + cfg.command_prefix.size();
        const std::string command_text = scene_pos == std::string::npos
            ? request.message_text
            : request.message_text.substr(scene_pos);
        const std::size_t direct_scene_pos = ToLowerCopy(command_text).find("scene");
        const std::string scene_name = direct_scene_pos == std::string::npos
            ? std::string()
            : TrimCopy(command_text.substr(direct_scene_pos + 5));
        if (scene_name.empty()) {
            result.ok = false;
            result.error_code = "missing_scene_name";
            return result;
        }
        result.ok = true;
        result.mutates_state = true;
        result.pause_automation = true;
        result.action = "switch_scene";
        result.scene_name = scene_name;
        result.matched_command = "scene";
        if (cfg.announce_scene_switches) {
            result.reply_text = "Switching scene to " + scene_name + ".";
        }
        return result;
    }

    for (const auto& rule : cfg.rules) {
        for (const auto& alias : rule.aliases) {
            if (body != alias) {
                continue;
            }
            result.matched_command = alias;
            if (rule.linked_scene_name.empty()) {
                result.ok = false;
                result.error_code = "scene_alias_unlinked";
                return result;
            }
            result.ok = true;
            result.mutates_state = true;
            result.pause_automation = true;
            result.action = "switch_scene";
            result.scene_name = rule.linked_scene_name;
            if (cfg.announce_scene_switches) {
                result.reply_text = "Switching scene to " + rule.label + ".";
            }
            return result;
        }
    }

    result.ok = false;
    result.error_code = "unknown_command";
    return result;
}

} // namespace aegis

#endif // AEGIS_OBS_PLUGIN_BUILD
