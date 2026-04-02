// Tests for RelayClient::ParseSessionResponse — a static method that parses
// JSON from the control-plane into a RelaySession struct.
//
// ParseSessionResponse uses Qt6 QJsonDocument internally, so Qt6::Core must
// be linked.  No RelayClient instance is needed (static method).

#include <catch2/catch_test_macros.hpp>

#include "relay_client.h"

using telemy::RelayClient;
using telemy::RelaySession;

// ---------------------------------------------------------------------------
// Helper: build a fully-populated control-plane JSON response.
// ---------------------------------------------------------------------------
static std::string MakeFullSessionJson() {
    return R"({
        "session": {
            "session_id": "ses-abc123",
            "status": "active",
            "region": "us-west-2",
            "instance_id": "i-0123456789abcdef0",
            "provision_step": "complete",
            "relay": {
                "public_ip": "198.51.100.1",
                "srt_port": 5000,
                "relay_hostname": "abc123.relay.example.com"
            },
            "credentials": {
                "pair_token": "tok-secret"
            },
            "timers": {
                "grace_window_seconds": 300,
                "max_session_seconds": 7200
            }
        }
    })";
}

// ---------------------------------------------------------------------------
// TEST: Valid JSON with all fields populated
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: full valid JSON", "[relay][parse]") {
    auto result = RelayClient::ParseSessionResponse(MakeFullSessionJson());

    REQUIRE(result.has_value());

    const RelaySession& s = *result;
    CHECK(s.session_id == "ses-abc123");
    CHECK(s.status == "active");
    CHECK(s.region == "us-west-2");
    CHECK(s.public_ip == "198.51.100.1");
    CHECK(s.srt_port == 5000);
    CHECK(s.relay_hostname == "abc123.relay.example.com");
    CHECK(s.pair_token == "tok-secret");
    CHECK(s.instance_id == "i-0123456789abcdef0");
    CHECK(s.grace_window_seconds == 300);
    CHECK(s.max_session_seconds == 7200);
    CHECK(s.provision_step == "complete");
}

// ---------------------------------------------------------------------------
// TEST: Valid JSON with missing optional fields — should use defaults
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: minimal valid JSON (defaults)", "[relay][parse]") {
    // Only session_id is truly required (non-empty).
    std::string json = R"({
        "session": {
            "session_id": "ses-minimal"
        }
    })";

    auto result = RelayClient::ParseSessionResponse(json);
    REQUIRE(result.has_value());

    const RelaySession& s = *result;
    CHECK(s.session_id == "ses-minimal");
    CHECK(s.status.empty());
    CHECK(s.region.empty());
    CHECK(s.public_ip.empty());
    CHECK(s.srt_port == 5000);  // default from toInt(5000)
    CHECK(s.relay_hostname.empty());
    CHECK(s.pair_token.empty());
    CHECK(s.instance_id.empty());
    CHECK(s.grace_window_seconds == 0);
    CHECK(s.max_session_seconds == 0);
    CHECK(s.provision_step.empty());
}

// ---------------------------------------------------------------------------
// TEST: Flat JSON (no "session" wrapper) with "id" fallback
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: flat JSON with id fallback", "[relay][parse]") {
    std::string json = R"({
        "id": "ses-flat",
        "status": "provisioning",
        "region": "eu-west-1"
    })";

    auto result = RelayClient::ParseSessionResponse(json);
    REQUIRE(result.has_value());
    CHECK(result->session_id == "ses-flat");
    CHECK(result->status == "provisioning");
}

// ---------------------------------------------------------------------------
// TEST: Empty string input
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: empty string", "[relay][parse]") {
    auto result = RelayClient::ParseSessionResponse("");
    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// TEST: Malformed JSON
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: malformed JSON", "[relay][parse]") {
    auto result = RelayClient::ParseSessionResponse("{not valid json!!!");
    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// TEST: Valid JSON but missing session_id (required) -> nullopt
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: missing session_id", "[relay][parse]") {
    // Has fields but no session_id and no id.
    std::string json = R"({
        "session": {
            "status": "active",
            "region": "us-east-1"
        }
    })";

    auto result = RelayClient::ParseSessionResponse(json);
    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// TEST: JSON array instead of object
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: JSON array", "[relay][parse]") {
    auto result = RelayClient::ParseSessionResponse("[1, 2, 3]");
    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// TEST: Literal "null"
// ---------------------------------------------------------------------------
TEST_CASE("ParseSessionResponse: literal null", "[relay][parse]") {
    auto result = RelayClient::ParseSessionResponse("null");
    REQUIRE_FALSE(result.has_value());
}
