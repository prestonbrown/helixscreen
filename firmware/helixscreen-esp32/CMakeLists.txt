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
project(helixscreen_esp32)

# The ELF's SHA256 beside the image. A crash prints the first nine hex digits of the
# flashed ELF's hash ("ELF file SHA256:"), and symbolizing is only exact against that
# same ELF, so the sidecar is what a crash report is matched against.
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
    COMMAND sh -c "sha256sum ${CMAKE_PROJECT_NAME}.elf > ${CMAKE_PROJECT_NAME}.elf.sha256"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM)
