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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(helixscreen_esp32)
