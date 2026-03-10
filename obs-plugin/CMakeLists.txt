cmake_minimum_required(VERSION 3.20)
project(aegis_obs_plugin LANGUAGES CXX)

option(AEGIS_BUILD_OBS_PLUGIN "Build OBS plugin target (requires OBS Studio SDK)" OFF)
option(AEGIS_BUILD_TESTS "Build Aegis unit tests (Catch2 v3, requires Qt6::Core)" OFF)
option(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST "Enable OBS plugin browser-dock host wiring scaffold" OFF)
option(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF "Enable OBS browser-panel/QCef-backed dock host implementation" OFF)
option(AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE "Enable Qt WebEngine-backed OBS browser dock host implementation" OFF)
option(AEGIS_STAGE_DOCK_BRIDGE_ASSETS "Stage dock bridge JS assets next to OBS plugin build outputs" ON)
set(AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT "" CACHE PATH "Optional override root containing aegis-dock-bridge*.js assets")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (AEGIS_BUILD_OBS_PLUGIN)
  if (NOT OBS_INCLUDE_DIRS)
    message(FATAL_ERROR "AEGIS_BUILD_OBS_PLUGIN=ON requires OBS_INCLUDE_DIRS (path(s) containing obs-module.h / obs-frontend-api.h).")
  endif()
  if (NOT OBS_LIBRARIES)
    message(FATAL_ERROR "AEGIS_BUILD_OBS_PLUGIN=ON requires OBS_LIBRARIES (e.g. obs + obs-frontend-api import libs).")
  endif()

  add_library(aegis-obs-plugin MODULE src/obs_plugin_entry.cpp)
  target_sources(aegis-obs-plugin PRIVATE
    src/config_vault.cpp
    src/metrics_collector.cpp
    src/relay_client.cpp
    src/dock_theme.cpp
    src/dock_replay_cache.cpp
    src/dock_action_dispatch.cpp
  )
  target_include_directories(aegis-obs-plugin PRIVATE ${OBS_INCLUDE_DIRS})
  if (OBS_LIBRARY_DIRS)
    target_link_directories(aegis-obs-plugin PRIVATE ${OBS_LIBRARY_DIRS})
  endif()
  target_link_libraries(aegis-obs-plugin PRIVATE ${OBS_LIBRARIES})
  target_compile_definitions(aegis-obs-plugin PRIVATE AEGIS_OBS_PLUGIN_BUILD=1)
  if (WIN32)
    target_compile_definitions(aegis-obs-plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()

  # Qt6::Core is required for config_vault.cpp (QJsonDocument, QFile, QDir).
  find_package(Qt6 COMPONENTS Core REQUIRED)
  target_link_libraries(aegis-obs-plugin PRIVATE Qt6::Core)

  if (WIN32)
    target_link_libraries(aegis-obs-plugin PRIVATE winhttp crypt32 rpcrt4)
    target_sources(aegis-obs-plugin PRIVATE src/https_client.cpp)
  endif()

  if (AEGIS_STAGE_DOCK_BRIDGE_ASSETS)
    # Locate the directory containing aegis-dock-bridge.js and its companions.
    set(_aegis_dock_asset_root "")
    if (AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT)
      set(_aegis_dock_asset_root "${AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT}")
    else()
      foreach(_candidate
          "${CMAKE_CURRENT_LIST_DIR}/dock"
          "${CMAKE_CURRENT_LIST_DIR}/.."
          "${CMAKE_CURRENT_LIST_DIR}/../..")
        if (EXISTS "${_candidate}/aegis-dock-bridge.js"
            AND EXISTS "${_candidate}/aegis-dock-bridge-host.js"
            AND EXISTS "${_candidate}/aegis-dock-browser-host-bootstrap.js")
          set(_aegis_dock_asset_root "${_candidate}")
          break()
        endif()
      endforeach()
    endif()

    if (_aegis_dock_asset_root)
      file(TO_CMAKE_PATH "${_aegis_dock_asset_root}" _aegis_dock_asset_root_norm)
      set(_aegis_dock_stage_dir "$<TARGET_FILE_DIR:aegis-obs-plugin>/data/obs-plugins/aegis-obs-plugin")

      add_custom_command(
        TARGET aegis-obs-plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_aegis_dock_stage_dir}"
        VERBATIM)

      foreach(_asset_name
          "aegis-dock-bridge.js"
          "aegis-dock-bridge-host.js"
          "aegis-dock-browser-host-bootstrap.js"
          "aegis-dock-app.js")
        add_custom_command(
          TARGET aegis-obs-plugin POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_aegis_dock_asset_root_norm}/${_asset_name}"
            "${_aegis_dock_stage_dir}/${_asset_name}"
          VERBATIM)
      endforeach()

      # Copy aegis-dock.html if present alongside the bridge assets.
      foreach(_html_candidate
          "${_aegis_dock_asset_root}/aegis-dock.html"
          "${CMAKE_CURRENT_LIST_DIR}/dock/aegis-dock.html"
          "${CMAKE_CURRENT_LIST_DIR}/../aegis-dock.html"
          "${CMAKE_CURRENT_LIST_DIR}/../../aegis-dock.html")
        if (EXISTS "${_html_candidate}")
          file(TO_CMAKE_PATH "${_html_candidate}" _aegis_dock_html_norm)
          add_custom_command(
            TARGET aegis-obs-plugin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${_aegis_dock_html_norm}"
              "${_aegis_dock_stage_dir}/aegis-dock.html"
            VERBATIM)
          break()
        endif()
      endforeach()

      message(STATUS "Aegis OBS dock assets will be staged from: ${_aegis_dock_asset_root_norm}")
    else()
      message(WARNING "AEGIS_STAGE_DOCK_BRIDGE_ASSETS=ON but aegis-dock-bridge.js was not found; set AEGIS_DOCK_BRIDGE_ASSET_SOURCE_ROOT or rely on AEGIS_DOCK_BRIDGE_ROOT at runtime.")
    endif()
  endif()

  if (AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST)
    target_sources(aegis-obs-plugin PRIVATE src/obs_browser_dock_host_scaffold.cpp)
    target_compile_definitions(aegis-obs-plugin PRIVATE AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST=1)
    if (AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF)
      find_package(Qt6 COMPONENTS Core Widgets QUIET)
      set(_obs_browser_panel_include "")
      if (OBS_BROWSER_PANEL_DIR AND EXISTS "${OBS_BROWSER_PANEL_DIR}/browser-panel.hpp")
        set(_obs_browser_panel_include "${OBS_BROWSER_PANEL_DIR}")
      elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/obs-studio/plugins/obs-browser/panel/browser-panel.hpp")
        set(_obs_browser_panel_include "${CMAKE_CURRENT_LIST_DIR}/../third_party/obs-studio/plugins/obs-browser/panel")
      elseif (EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/obs-studio/plugins/obs-browser/panel/browser-panel.hpp")
        set(_obs_browser_panel_include "${CMAKE_CURRENT_LIST_DIR}/third_party/obs-studio/plugins/obs-browser/panel")
      endif()

      if (Qt6Widgets_FOUND AND _obs_browser_panel_include)
        target_compile_definitions(aegis-obs-plugin PRIVATE AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=1)
        target_include_directories(aegis-obs-plugin PRIVATE "${_obs_browser_panel_include}")
        target_link_libraries(aegis-obs-plugin PRIVATE
          Qt6::Core
          Qt6::Widgets
        )
      else()
        message(WARNING "AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_OBS_CEF=ON but Qt6 Core/Widgets and/or obs-browser panel headers were not found; building scaffold fallback only.")
      endif()
    endif()
    if (AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE)
      find_package(Qt6 COMPONENTS Core Widgets WebEngineCore WebEngineWidgets QUIET)
      if (Qt6WebEngineCore_FOUND AND Qt6WebEngineWidgets_FOUND)
        target_compile_definitions(aegis-obs-plugin PRIVATE AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE=1)
        target_link_libraries(aegis-obs-plugin PRIVATE
          Qt6::Core
          Qt6::Widgets
          Qt6::WebEngineCore
          Qt6::WebEngineWidgets
        )
      else()
        message(WARNING "AEGIS_ENABLE_OBS_BROWSER_DOCK_HOST_QT_WEBENGINE=ON but Qt6 WebEngine components were not found; building scaffold fallback only.")
      endif()
    endif()
  endif()
endif()

# --------------------------------------------------------------------------
# Unit tests (Catch2 v3) — opt-in, does not require OBS SDK.
# --------------------------------------------------------------------------
if (AEGIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
