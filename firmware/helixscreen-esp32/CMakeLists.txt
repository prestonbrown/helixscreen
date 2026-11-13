# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

# sdkconfig.local carries real WiFi/Moonraker credentials for the Task 10
# network HIL scenario (CONFIG_HELIX_NET_HIL) and is git-ignored — never
# committed. When present, layer it on top of sdkconfig.defaults; absent by
# default so a clean checkout builds with HIL disabled and empty credentials.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
endif()
# A debug overlay named by the build (scripts/esp32_build_debug_heap.sh) layers on last.
if(DEFINED ENV{HELIX_SDKCONFIG_OVERLAY})
    list(APPEND SDKCONFIG_DEFAULTS "$ENV{HELIX_SDKCONFIG_OVERLAY}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# fmt's locale support references std::locale from every object that formats a
# float, and one reference links all of libstdc++'s facets. No format string here
# uses {:L}.
idf_build_set_property(COMPILE_DEFINITIONS "FMT_USE_LOCALE=0" APPEND)
# __FILE__ is stored in the image (the subject debug registry keeps one per
# registration); repo-relative, it is the same name in ~7KB less flash.
get_filename_component(HELIX_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
idf_build_set_property(COMPILE_OPTIONS "-fmacro-prefix-map=${HELIX_REPO_ROOT}/=" APPEND)

# The app version is VERSION.txt, the same file the Makefile reads, so a firmware image
# reports the version a Linux build of the same commit does. The numeric fields come from
# the core triple, with any prerelease or build suffix cut off. helixapp and helixnet read
# these build properties; editing VERSION.txt reconfigures.
set(HELIX_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../VERSION.txt")
file(STRINGS "${HELIX_VERSION_FILE}" HELIX_VERSION LIMIT_COUNT 1)
string(STRIP "${HELIX_VERSION}" HELIX_VERSION)
if(NOT HELIX_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "VERSION.txt does not start with MAJOR.MINOR.PATCH: '${HELIX_VERSION}'")
endif()
idf_build_set_property(HELIX_VERSION "${HELIX_VERSION}")
idf_build_set_property(HELIX_VERSION_MAJOR "${CMAKE_MATCH_1}")
idf_build_set_property(HELIX_VERSION_MINOR "${CMAKE_MATCH_2}")
idf_build_set_property(HELIX_VERSION_PATCH "${CMAKE_MATCH_3}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HELIX_VERSION_FILE}")
project(helixscreen_esp32)

# The ELF's SHA256 beside the image. A crash prints the first nine hex digits of the
# flashed ELF's hash ("ELF file SHA256:"), and symbolizing is only exact against that
# same ELF, so the sidecar is what a crash report is matched against.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND sh -c "sha256sum ${CMAKE_PROJECT_NAME}.elf > ${CMAKE_PROJECT_NAME}.elf.sha256"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM)

# The token database for this ELF's tokenized log records (scripts/detokenize_log.py
# reads it; the release zip ships it beside the image).
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND python3 ${HELIX_REPO_ROOT}/scripts/esp32_logtok_db.py
            ${CMAKE_PROJECT_NAME}.elf ${CMAKE_PROJECT_NAME}.logtok.json
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM)

# LTO on the app component only: 573 KB off the image. ESP-IDF and its components
# stay out because their linker fragments place code by object name.
idf_component_get_property(helix_app_lib helixapp COMPONENT_LIB)
target_compile_options(${helix_app_lib} PRIVATE -flto)
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE -flto -Os)
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf PRE_LINK
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/tools/lto_bss_to_psram.py
            ${CMAKE_BINARY_DIR}/esp-idf/esp_system/ld/sections.ld
    VERBATIM)
