# SPDX-License-Identifier: GPL-3.0-or-later
#
# helixnet — EspMoonrakerClient over the managed esp_websocket_client component,
# plus (Task 10) EspHttpLane + the MoonrakerRestAPI/MoonrakerFileTransferAPI
# concrete overrides over esp_http_client (the app's HTTP-side sub-APIs — see
# esp_rest_api.cpp for the R1 scope: only download_file_partial is real,
# everything else is an asserting stub). Compiles the repo's IMoonrakerClient
# contract (include/i_moonraker_client.h and its transitive headers) UNMODIFIED
# via relative paths into the main checkout, using the same spdlog→esp_log /
# hv-json→nlohmann shim the Plan 2 native-audit proved (those headers are audit
# bucket B: clean on Xtensa with the shim).

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
cmake_path(NORMAL_PATH REPO_ROOT)
set(SHIM ${CMAKE_CURRENT_LIST_DIR}/shim)

# MoonrakerError::timeout() (inline in moonraker_error.h) calls
# helix::format::duration(); src/format_utils.cpp is its only definition and is
# self-contained (format_utils.h + <cstdio> + <ctime> — no spdlog/lvgl/i18n), so
# it compiles UNMODIFIED here rather than being stubbed.
# EspMoonrakerClient calls PrinterDiscovery::parse_objects(), defined out of line
# in printer_discovery_parse.cpp; helixapp links before helixnet, so its copy
# of that TU cannot satisfy a reference from here.
# MoonrakerError::from_json_rpc() (src/api/moonraker_error_rpc.cpp) decodes
# firmware error envelopes via src/printer/firmware_error_envelope.cpp; each needs
# only hv/json.hpp, and both compile here for the same link-order reason.
# EspMoonrakerClient asks src/api/moonraker_auth.cpp for the printer's API key,
# also for the link-order reason.
set(REPO_SRCS ${REPO_ROOT}/src/format_utils.cpp
              ${REPO_ROOT}/src/api/moonraker_auth.cpp
              ${REPO_ROOT}/src/api/moonraker_error_rpc.cpp
              ${REPO_ROOT}/src/printer/firmware_error_envelope.cpp
              ${REPO_ROOT}/src/printer/printer_discovery_parse.cpp)

idf_component_register(
    SRCS esp_moonraker_client.cpp esp_http_lane.cpp esp_rest_api.cpp coverage_assert.cpp
         shim/platform_stubs.cpp ${REPO_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp_websocket_client esp_http_client esp_timer esp_event
    PRIV_REQUIRES pthread)

# Repo headers + shim aliases are PRIVATE: only helixnet's own TUs compile
# against i_moonraker_client.h / the sub-API headers. The C-linkage
# helixnet_link_probe() is the only symbol app_main references directly; the
# app-layer MoonrakerFileTransferAPI/MoonrakerRestAPI unique_ptr members
# (moonraker_api.cpp, KEPT in helixapp) are the consumers of the vtables this
# component now emits for real (esp_rest_api.cpp), so no consumer needs these
# include paths beyond that existing link-time relationship.
target_include_directories(${COMPONENT_LIB} PRIVATE
    ${REPO_ROOT}/include        # i_moonraker_client.h + moonraker_*/printer_* headers
    ${SHIM}/include             # <spdlog/spdlog.h>, <hv/json.hpp> aliases
    ${SHIM})                    # spdlog_shim.h, hv_json_shim.h, platform_stubs.h
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    ${REPO_ROOT}/lib)           # libhv/include/hv/json.hpp + spdlog bundled fmt

# server.connection.identify reports the app version to Moonraker.
idf_build_get_property(helix_version HELIX_VERSION)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HELIX_VERSION="${helix_version}")

# Repo headers are C++17; nlohmann/fmt trip -Wformat on Xtensa GCC. Match the
# audit's flags (compile/link viability, not warning hygiene).
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    # GCC clones inline functions per translation unit ($isra, $constprop, $part)
    # as local symbols the linker cannot merge, so every TU carries its own copy of
    # the same std::string and json helpers.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-ipa-sra>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-ipa-cp-clone>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-partial-inlining>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-format>)
