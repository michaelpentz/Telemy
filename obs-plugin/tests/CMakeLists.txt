cmake_minimum_required(VERSION 3.20)

# --------------------------------------------------------------------------
# Aegis OBS plugin — unit test suite (Catch2 v3)
#
# Enable with: cmake -DAEGIS_BUILD_TESTS=ON ...
# Run with:    ctest --test-dir <build>/tests --output-on-failure
# --------------------------------------------------------------------------

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(Catch2)

# Include the plugin source directory so tests can #include headers.
set(PLUGIN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# --------------------------------------------------------------------------
# Test executable
# --------------------------------------------------------------------------
add_executable(aegis-tests
    test_parse_session_response.cpp
    test_json_extract.cpp
    test_config_vault.cpp
    # Standalone copy of JSON helper functions (extracted from obs_plugin_entry.cpp).
    json_helpers_impl.cpp
    # Compile the source files under test directly into the test binary.
    # This avoids needing to link against the full OBS plugin module.
    ${PLUGIN_SRC_DIR}/relay_client.cpp
    ${PLUGIN_SRC_DIR}/config_vault.cpp
)

target_include_directories(aegis-tests PRIVATE
    ${PLUGIN_SRC_DIR}
    # Stub <obs-module.h> so plugin sources compile without the OBS SDK.
    "${CMAKE_CURRENT_SOURCE_DIR}/test_stubs"
)

# C++17 to match the plugin.
target_compile_features(aegis-tests PRIVATE cxx_std_17)

# Catch2 v3
target_link_libraries(aegis-tests PRIVATE Catch2::Catch2WithMain)

# Qt6::Core — required by relay_client.cpp (QJsonDocument) and
# config_vault.cpp (QFile, QDir, QJsonDocument).
find_package(Qt6 COMPONENTS Core REQUIRED)
target_link_libraries(aegis-tests PRIVATE Qt6::Core)

# Platform libraries (Windows)
if (WIN32)
    target_compile_definitions(aegis-tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(aegis-tests PRIVATE crypt32 rpcrt4 winhttp)

    # https_client.cpp is needed by relay_client.cpp (HttpsClient&).
    target_sources(aegis-tests PRIVATE ${PLUGIN_SRC_DIR}/https_client.cpp)
endif()

# --------------------------------------------------------------------------
# CTest integration — auto-discovers TEST_CASE names from the binary.
# --------------------------------------------------------------------------
include(CTest)

# Catch2 v3 FetchContent puts the extras module on the CMAKE_MODULE_PATH.
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch OPTIONAL RESULT_VARIABLE _catch_extras_found)

if (_catch_extras_found AND COMMAND catch_discover_tests)
    catch_discover_tests(aegis-tests)
else()
    # Simple fallback: register the whole executable as a single CTest test.
    add_test(NAME aegis-tests COMMAND aegis-tests)
endif()
