// Tests for telemy::Vault — DPAPI-encrypted secret store.
//
// These tests exercise the Vault class with real DPAPI calls, so they only
// run on Windows.  The tests use a temporary directory for vault files to
// avoid polluting the user's %APPDATA%/Telemy/.
//
// Note: Vault's path helpers (VaultDirPath/VaultFilePath) are private and
// hardcoded to %APPDATA%/Telemy.  For true isolation we would need to
// refactor Vault to accept a configurable base path.  For now, these tests
// exercise the DPAPI encrypt/decrypt round-trip indirectly through Set/Get,
// which writes to the real vault location.  A future iteration (RF-009b)
// should add a path override to Vault for hermetic tests.

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include "config_vault.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Helper: create a Vault instance with a loaded (possibly empty) state.
// WARNING: These tests use the real %APPDATA%/Telemy/vault.json file.
// They clean up after themselves, but running them concurrently with OBS
// could cause conflicts.  This is acceptable for a first-pass test harness;
// a path-injection refactor is tracked as follow-up work.
// ---------------------------------------------------------------------------

TEST_CASE("Vault: Set then Get roundtrip", "[vault][windows]") {
    telemy::Vault vault;
    REQUIRE(vault.Load());

    // Use a key name that won't collide with real config.
    const std::string test_key = "__telemy_test_roundtrip__";
    const std::string test_value = "hello-dpapi-world-12345";

    // Set — encrypts with DPAPI and persists.
    REQUIRE(vault.Set(test_key, test_value));

    // Get — decrypts and returns the original plaintext.
    auto result = vault.Get(test_key);
    REQUIRE(result.has_value());
    CHECK(*result == test_value);

    // Cleanup.
    vault.Remove(test_key);
}

TEST_CASE("Vault: Get nonexistent key returns nullopt", "[vault][windows]") {
    telemy::Vault vault;
    REQUIRE(vault.Load());

    auto result = vault.Get("__telemy_test_no_such_key_ever__");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Vault: overwrite existing key", "[vault][windows]") {
    telemy::Vault vault;
    REQUIRE(vault.Load());

    const std::string test_key = "__telemy_test_overwrite__";

    REQUIRE(vault.Set(test_key, "value-one"));
    REQUIRE(vault.Set(test_key, "value-two"));

    auto result = vault.Get(test_key);
    REQUIRE(result.has_value());
    CHECK(*result == "value-two");

    // Cleanup.
    vault.Remove(test_key);
}

TEST_CASE("Vault: Remove returns false for missing key", "[vault][windows]") {
    telemy::Vault vault;
    REQUIRE(vault.Load());

    CHECK_FALSE(vault.Remove("__telemy_test_never_set_key__"));
}

TEST_CASE("Vault: Keys() includes set keys and excludes removed", "[vault][windows]") {
    telemy::Vault vault;
    REQUIRE(vault.Load());

    const std::string k1 = "__telemy_test_keys_a__";
    const std::string k2 = "__telemy_test_keys_b__";

    vault.Set(k1, "a");
    vault.Set(k2, "b");

    auto keys = vault.Keys();
    bool found_k1 = false, found_k2 = false;
    for (const auto& k : keys) {
        if (k == k1) found_k1 = true;
        if (k == k2) found_k2 = true;
    }
    CHECK(found_k1);
    CHECK(found_k2);

    // Remove k1, verify it's gone.
    vault.Remove(k1);
    keys = vault.Keys();
    found_k1 = false;
    for (const auto& k : keys) {
        if (k == k1) found_k1 = true;
    }
    CHECK_FALSE(found_k1);

    // Cleanup.
    vault.Remove(k2);
}

TEST_CASE("Vault: Set and Get with special characters", "[vault][windows]") {
    telemy::Vault vault;
    REQUIRE(vault.Load());

    const std::string test_key = "__telemy_test_special__";
    // Value with unicode, newlines, and quotes.
    const std::string test_value = "p@ss\nw0rd\twith\"quotes\"&\xC3\xA9";

    REQUIRE(vault.Set(test_key, test_value));

    auto result = vault.Get(test_key);
    REQUIRE(result.has_value());
    CHECK(*result == test_value);

    vault.Remove(test_key);
}

#else
// On non-Windows, just register a trivial test so the suite isn't empty.
TEST_CASE("Vault: skipped on non-Windows", "[vault][!mayfail]") {
    SUCCEED("Vault tests are Windows-only (DPAPI)");
}
#endif

TEST_CASE("Config: explicit http relay host is rejected", "[config][host]") {
    CHECK(telemy::IsExplicitInsecureHttpHost("http://relay.telemyapp.com"));
    CHECK(telemy::IsExplicitInsecureHttpHost(" HTTP://relay.telemyapp.com:443"));
}

TEST_CASE("Config: https and bare relay hosts remain allowed", "[config][host]") {
    CHECK_FALSE(telemy::IsExplicitInsecureHttpHost(""));
    CHECK_FALSE(telemy::IsExplicitInsecureHttpHost("relay.telemyapp.com"));
    CHECK_FALSE(telemy::IsExplicitInsecureHttpHost("https://relay.telemyapp.com"));
}

#ifdef _WIN32
TEST_CASE("Config: BYOR fields persist across save and load", "[config][byor][windows]") {
    const std::filesystem::path config_path(telemy::PluginConfig::ConfigFilePath());
    struct ConfigRestoreGuard {
        std::filesystem::path path;
        bool had_original = false;
        std::string original_contents;

        ~ConfigRestoreGuard() {
            if (had_original) {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out << original_contents;
            } else {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } restore_guard;
    restore_guard.path = config_path;
    restore_guard.had_original = std::filesystem::exists(config_path);
    if (restore_guard.had_original) {
        std::ifstream in(config_path, std::ios::binary);
        restore_guard.original_contents.assign(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    }

    telemy::PluginConfig config;
    config.LoadFromDisk();
    config.byor_enabled = true;
    config.byor_relay_host = "byor.telemy.test";
    config.byor_relay_port = 6000;
    config.byor_stream_id = "live/custom";
    REQUIRE(config.SaveToDisk());

    telemy::PluginConfig loaded;
    REQUIRE(loaded.LoadFromDisk());
    CHECK(loaded.byor_enabled);
    CHECK(loaded.byor_relay_host == "byor.telemy.test");
    CHECK(loaded.byor_relay_port == 6000);
    CHECK(loaded.byor_stream_id == "live/custom");
}
#endif
