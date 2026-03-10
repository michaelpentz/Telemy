// Standalone compilation unit for the JSON helper functions.
//
// These functions are defined in obs_plugin_entry.cpp as file-local functions.
// Rather than pulling in the entire 2600+ line plugin entry file (which has
// deep OBS SDK dependencies), we copy the implementations here for the test
// binary.  The canonical source remains obs_plugin_entry.cpp; if those
// functions change, this file must be updated to match.
//
// A future refactor (RF-009b) should move these functions out of
// obs_plugin_entry.cpp into a shared json_helpers.cpp that both the plugin
// and the test binary can compile.

#include "json_helpers.h"

#include <sstream>
#include <cctype>

std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json) {
    const std::string needle = "\"type\":\"";
    const std::size_t start = envelope_json.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + needle.size();
    const std::size_t end = envelope_json.find('"', value_start);
    if (end == std::string::npos || end <= value_start) {
        return {};
    }
    return envelope_json.substr(value_start, end - value_start);
}

bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value) {
    if (!field_name || !out_value) {
        return false;
    }

    std::ostringstream needle;
    needle << "\"" << field_name << "\"";
    const std::string needle_str = needle.str();
    const std::size_t key_pos = json_text.find(needle_str);
    if (key_pos == std::string::npos) {
        return false;
    }

    const std::size_t colon_pos = json_text.find(':', key_pos + needle_str.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::size_t pos = colon_pos + 1;
    while (pos < json_text.size() && std::isspace(static_cast<unsigned char>(json_text[pos])) != 0) {
        pos += 1;
    }
    if (pos >= json_text.size() || json_text[pos] != '"') {
        return false;
    }
    pos += 1;

    std::string value;
    value.reserve(64);
    bool escaping = false;
    for (; pos < json_text.size(); ++pos) {
        const char ch = json_text[pos];
        if (escaping) {
            switch (ch) {
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            *out_value = std::move(value);
            return true;
        }
        value.push_back(ch);
    }
    return false;
}

bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value) {
    if (!field_name || !out_value) {
        return false;
    }

    std::ostringstream needle;
    needle << "\"" << field_name << "\"";
    const std::string needle_str = needle.str();
    const std::size_t key_pos = json_text.find(needle_str);
    if (key_pos == std::string::npos) {
        return false;
    }

    const std::size_t colon_pos = json_text.find(':', key_pos + needle_str.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::size_t pos = colon_pos + 1;
    while (pos < json_text.size() && std::isspace(static_cast<unsigned char>(json_text[pos])) != 0) {
        pos += 1;
    }

    if (pos + 4 <= json_text.size() && json_text.substr(pos, 4) == "true") {
        *out_value = true;
        return true;
    }
    if (pos + 5 <= json_text.size() && json_text.substr(pos, 5) == "false") {
        *out_value = false;
        return true;
    }

    return false;
}
