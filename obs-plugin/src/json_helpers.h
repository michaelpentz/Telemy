#pragma once

// Lightweight JSON string extraction helpers.
//
// These are pure functions that parse JSON manually (no Qt dependency).
// Originally defined in obs_plugin_entry.cpp; extracted here so they can
// be unit-tested independently.

#include <string>

// Extract the string value of |field_name| from a JSON text blob.
// Returns true and writes to *out_value on success; returns false if the
// field is absent, the value is not a string, or the JSON is malformed.
// Handles standard JSON escape sequences (\\, \", \n, \t, etc.).
bool TryExtractJsonStringField(
    const std::string& json_text,
    const char* field_name,
    std::string* out_value);

// Extract a boolean value for |field_name|. Returns true if found and
// writes the parsed boolean to *out_value.
bool TryExtractJsonBoolField(
    const std::string& json_text,
    const char* field_name,
    bool* out_value);

// Quick extraction of the "type" field from a JSON envelope string.
// Returns empty string if not found.
std::string TryExtractEnvelopeTypeFromJson(const std::string& envelope_json);
