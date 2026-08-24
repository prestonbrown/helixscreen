# SPDX-License-Identifier: GPL-3.0-or-later
#
# helixapp — the curated Core+AMS app core, compiled UNMODIFIED from ../../src
# via relative paths (same rule as helixcore/helixnet). A separate component so
# `idf.py size-components` attributes app-core bytes distinctly from LVGL and
# the network layer.
#
# The source list lives in app_srcs.txt (checked in, exact relative paths) so it
# can be curated without CMake edits; every include/exclude decision is recorded
# there as a comment. Derived from the Plan 2 native-audit 491-file manifest
# minus the subsystems gated off for the v1 Core+AMS cut (label printers,
# camera, gcode/bed-mesh 3D rendering, plugins, timelapse viewer, screensaver,
# calibration panels, sound backends, mocks) and the Task 10 HTTP sub-APIs, PLUS
# the real src/api Moonraker command surface that the audit had stubbed
# (helixnet's EspMoonrakerClient makes the transport real).

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
cmake_path(NORMAL_PATH REPO_ROOT)

# The drift-prone logging/JSON/platform shim is single-sourced in helixnet — we
# do NOT copy it. helixapp adds only the parse-only hv/ transport headers that
# helixnet legitimately does not need: ~21 app TUs transitively include the
# concrete helix::MoonrakerClient (moonraker_client.h -> hv/WebSocketClient.h),
# which is never instantiated on ESP32 (MoonrakerManager's ESP arm builds
# EspMoonrakerClient). See shim/hv/WebSocketClient.h.
set(HELIXNET_SHIM ${CMAKE_CURRENT_LIST_DIR}/../helixnet/shim)
set(HELIXAPP_SHIM ${CMAKE_CURRENT_LIST_DIR}/shim)

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/app_srcs.txt APP_SRCS_REL
     REGEX "^[^#].*\\.(cpp|c)$")
set(APP_SRCS "")
foreach(rel ${APP_SRCS_REL})
    list(APPEND APP_SRCS ${REPO_ROOT}/${rel})
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_LIST_DIR}/app_srcs.txt)

# CONFIG_HELIX_MOCK_PRINTER (main/Kconfig.projbuild, default n): drive mock
# printer data on-device. It does NOT pull the app-layer mock sources or define
# HELIX_ENABLE_MOCKS — that path is unlinkable on ESP32: MoonrakerClientMock
# derives helix::MoonrakerClient (the concrete libhv WebSocket client, whose
# transport base is intentionally undefined on ESP — see helixnet/helixapp
# shims), and MoonrakerManager::create_api() compiles a `MoonrakerAPIMock`
# construction + a `dynamic_cast<MoonrakerClient*>` under `#ifdef
# HELIX_ENABLE_MOCKS`, both of which demand the excluded concrete client's
# ctor/typeinfo. Defining the macro therefore breaks the link regardless of
# runtime test_mode. Instead, CONFIG_HELIX_MOCK_PRINTER is a plain sdkconfig
# macro that app_boot.cpp reads to run a firmware-local synthetic PrinterState
# driver (see app_boot.cpp `#if CONFIG_HELIX_MOCK_PRINTER`), feeding the same
# production PrinterState::update_from_status() path with zero network. Most
# `# MOCK:` lines in app_srcs.txt are retained as documentation of the desktop
# mock set but are deliberately NOT compiled here.
#
# Task 15 R3 exception: ams_backend_mock.cpp has no dependency on the concrete
# libhv MoonrakerClient (its ctor takes only a slot count; the optional
# MoonrakerClientMock hookup in ams_backend.cpp's create_mock_with_features()
# degrades gracefully to nullptr) — its only repo dependencies
# (afc_defaults.cpp, hh_defaults.cpp, slot_registry.cpp, filament_database.h)
# are already in APP_SRCS above. It links cleanly, so it's the one `# MOCK:`
# source actually compiled, and only for mock builds. app_boot.cpp's
# mock_seed_ams() constructs it directly (bypassing the
# RuntimeConfig::should_mock_ams() factory gate, which requires test_mode —
# deliberately never set on ESP, see app_boot_ui()'s NOTE).
set(MOCK_SRCS "")
if(CONFIG_HELIX_MOCK_PRINTER)
    list(APPEND MOCK_SRCS ${REPO_ROOT}/src/printer/ams_backend_mock.cpp)
endif()

# Local firmware-tree sources: platform seams (Linux-bound singletons the app
# reaches through may-be-null accessors), the Task 10 HTTP link stubs, missing
# font-face aliases, the app_main link probe, and the Task 13 WifiBackend impl.
set(LOCAL_SRCS
    app_boot.cpp
    helixapp_platform_stubs.cpp
    helixapp_platform_stubs2.cpp
    task10_pending_stubs.cpp
    excluded_subsystems_stub.cpp
    font_aliases.cpp
    stb_impl.c
    helix_lvgl_abi_check.c
    link_probe.cpp
    wifi_backend_esp.cpp
    provisioning_esp.cpp)

idf_component_register(
    SRCS ${APP_SRCS} ${MOCK_SRCS} ${LOCAL_SRCS}
    INCLUDE_DIRS "."
    REQUIRES helixcore helixnet
    # esp_wifi/esp_netif/esp_event/nvs_flash: the non-mock connect path
    # (app_boot.cpp, Task 8) brings up the WiFi station before connecting.
    # esp_http_server: Task 14's provisioning captive portal (provisioning_esp.cpp).
    PRIV_REQUIRES pthread esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_server)

# The PCH -include is C++-only: font/image .c sources compile as plain C.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    -w
    $<$<COMPILE_LANGUAGE:CXX>:-include>
    $<$<COMPILE_LANGUAGE:CXX>:${REPO_ROOT}/include/lvgl_pch.h>)

# Real v1 Core+AMS gate set (the plan's compile definitions). Excluded
# subsystems compile out via these; kept files that touch them see the #else
# no-op branch. HELIX_ENABLE_MOCKS is added only when CONFIG_HELIX_MOCK_PRINTER.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HELIX_VERSION="0.0.0-esp32"
    HELIX_VERSION_MAJOR=0 HELIX_VERSION_MINOR=0 HELIX_VERSION_PATCH=0
    HELIX_GIT_HASH="esp32"
    INSTALLER_FILENAME="install.sh"
    HELIX_MAX_FONT_TIER=6
    HELIX_PLATFORM_ESP32=1
    HELIX_HAS_PLUGINS=0
    HELIX_HAS_TIMELAPSE_VIEWER=0
    HELIX_HAS_CAMERA=0
    HELIX_HAS_GCODE_VIEWER=0
    HELIX_HAS_BED_MESH_3D=0
    HELIX_HAS_LABEL_PRINTER=0
    HELIX_HAS_CFS=1
    HELIX_HAS_IFS=1
    HELIX_HAS_SOUND=0
    HELIX_ENABLE_SCREENSAVER=0
    HELIX_RELEASE_BUILD)
# NOTE: HELIX_ENABLE_MOCKS is intentionally NEVER defined on ESP32 — the
# app-layer mock stack is unlinkable here (see the MOCK_SRCS comment above).
# CONFIG_HELIX_MOCK_PRINTER drives the firmware-local synthetic driver in
# app_boot.cpp instead.

target_include_directories(${COMPONENT_LIB} PRIVATE
    ${REPO_ROOT}
    ${REPO_ROOT}/include
    ${REPO_ROOT}/src/generated
    ${REPO_ROOT}/build/generated
    ${HELIXAPP_SHIM}            # parse-only hv/Event.h + hv/WebSocketClient.h
    ${HELIXNET_SHIM}/include    # <spdlog/spdlog.h>, <hv/json.hpp> aliases
    ${HELIXNET_SHIM})           # spdlog_shim.h, hv_json_shim.h, platform_stubs.h
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    ${REPO_ROOT}/lib
    ${REPO_ROOT}/lib/glm
    ${REPO_ROOT}/lib/lvgl
    ${REPO_ROOT}/lib/lvgl/src
    ${REPO_ROOT}/lib/stb
    ${REPO_ROOT}/lib/lv_markdown/src
    ${REPO_ROOT}/lib/lv_markdown/deps/md4c)
