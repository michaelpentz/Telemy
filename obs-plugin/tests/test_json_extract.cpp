// Tests for TryExtractJsonStringField — a pure function that extracts a
// string value from a JSON blob by field name, using manual parsing (no Qt).
//
// The function is defined in obs_plugin_entry.cpp and declared in
// json_helpers.h.  For the test binary we compile a standalone copy
// (see json_helpers_impl.cpp) so we don't pull in the entire plugin entry.

#include <catch2/catch_test_macros.hpp>

#include "json_helpers.h"

// ---------------------------------------------------------------------------
// TryExtractJsonStringField tests
// ---------------------------------------------------------------------------

TEST_CASE("TryExtractJsonStringField: key present with simple value", "[json]") {
    std::string json = R"({"name":"Alice","age":"30"})";
    std::string out;

    REQUIRE(TryExtractJsonStringField(json, "name", &out));
    CHECK(out == "Alice");
}

TEST_CASE("TryExtractJsonStringField: key not present", "[json]") {
    std::string json = R"({"name":"Alice"})";
    std::string out = "unchanged";

    REQUIRE_FALSE(TryExtractJsonStringField(json, "missing", &out));
    CHECK(out == "unchanged");  // out should not be modified on failure
}

TEST_CASE("TryExtractJsonStringField: empty JSON object", "[json]") {
    std::string json = "{}";
    std::string out;

    REQUIRE_FALSE(TryExtractJsonStringField(json, "key", &out));
}

TEST_CASE("TryExtractJsonStringField: empty string input", "[json]") {
    std::string out;
    REQUIRE_FALSE(TryExtractJsonStringField("", "key", &out));
}

TEST_CASE("TryExtractJsonStringField: null field_name", "[json]") {
    std::string json = R"({"key":"val"})";
    std::string out;

    REQUIRE_FALSE(TryExtractJsonStringField(json, nullptr, &out));
}

TEST_CASE("TryExtractJsonStringField: null out_value", "[json]") {
    std::string json = R"({"key":"val"})";

    REQUIRE_FALSE(TryExtractJsonStringField(json, "key", nullptr));
}

TEST_CASE("TryExtractJsonStringField: escaped characters in value", "[json]") {
    // JSON with escaped quotes, backslash, newline, tab
    std::string json = R"({"msg":"hello \"world\"\nline2\ttab\\end"})";
    std::string out;

    REQUIRE(TryExtractJsonStringField(json, "msg", &out));
    CHECK(out == "hello \"world\"\nline2\ttab\\end");
}

TEST_CASE("TryExtractJsonStringField: escaped forward slash", "[json]") {
    std::string json = R"({"url":"http:\/\/example.com"})";
    std::string out;

    REQUIRE(TryExtractJsonStringField(json, "url", &out));
    CHECK(out == "http://example.com");
}

TEST_CASE("TryExtractJsonStringField: multiple keys, extract correct one", "[json]") {
    std::string json = R"({"first":"aaa","second":"bbb","third":"ccc"})";
    std::string out;

    REQUIRE(TryExtractJsonStringField(json, "second", &out));
    CHECK(out == "bbb");
}

TEST_CASE("TryExtractJsonStringField: value with spaces around colon", "[json]") {
    std::string json = R"({"key"  :  "value with spaces"})";
    std::string out;

    REQUIRE(TryExtractJsonStringField(json, "key", &out));
    CHECK(out == "value with spaces");
}

TEST_CASE("TryExtractJsonStringField: non-string value (number)", "[json]") {
    // The field exists but its value is a number, not a quoted string.
    std::string json = R"({"count": 42})";
    std::string out;

    REQUIRE_FALSE(TryExtractJsonStringField(json, "count", &out));
}

TEST_CASE("TryExtractJsonStringField: empty string value", "[json]") {
    std::string json = R"({"key":""})";
    std::string out = "not_empty";

    REQUIRE(TryExtractJsonStringField(json, "key", &out));
    CHECK(out.empty());
}

// ---------------------------------------------------------------------------
// TryExtractEnvelopeTypeFromJson tests
// ---------------------------------------------------------------------------

TEST_CASE("TryExtractEnvelopeTypeFromJson: type present", "[json]") {
    std::string json = R"({"type":"action","payload":{}})";
    CHECK(TryExtractEnvelopeTypeFromJson(json) == "action");
}

TEST_CASE("TryExtractEnvelopeTypeFromJson: type missing", "[json]") {
    std::string json = R"({"payload":"data"})";
    CHECK(TryExtractEnvelopeTypeFromJson(json).empty());
}

TEST_CASE("TryExtractEnvelopeTypeFromJson: empty string", "[json]") {
    CHECK(TryExtractEnvelopeTypeFromJson("").empty());
}
