# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compiles the repo's LVGL submodule (lib/lvgl, pinned 9.5.0 + patches) and
# lib/helix-xml UNMODIFIED, via relative paths into the main checkout.
#
# Why not the ESP-IDF managed lvgl/lvgl component: helix-xml includes
# <lvgl_private.h> (private internals) — it must be compiled against the exact
# LVGL tree it was extracted from. Version drift is guaranteed breakage, so the
# audit goes straight to the repo submodule (the plan's documented fallback).
#
# Source list mirrors Makefile:283+300 — LVGL's internal xml/expat are excluded
# because lib/helix-xml replaces them.

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
cmake_path(NORMAL_PATH REPO_ROOT)

file(GLOB_RECURSE LVGL_SRCS ${REPO_ROOT}/lib/lvgl/src/*.c)
file(GLOB LVGL_INTERNAL_XML
     ${REPO_ROOT}/lib/lvgl/src/xml/*.c
     ${REPO_ROOT}/lib/lvgl/src/xml/parsers/*.c
     ${REPO_ROOT}/lib/lvgl/src/libs/expat/*.c)
if(LVGL_INTERNAL_XML)
    list(REMOVE_ITEM LVGL_SRCS ${LVGL_INTERNAL_XML})
endif()

file(GLOB HELIX_XML_SRCS
     ${REPO_ROOT}/lib/helix-xml/src/xml/*.c
     ${REPO_ROOT}/lib/helix-xml/src/xml/parsers/*.c
     ${REPO_ROOT}/lib/helix-xml/src/libs/expat/*.c)

idf_component_register(
    SRCS ${LVGL_SRCS} ${HELIX_XML_SRCS} anomaly_stub.c shim/platform_stubs.cpp
    INCLUDE_DIRS
        ${CMAKE_CURRENT_LIST_DIR}      # adapted lv_conf.h (RGB565)
        ${REPO_ROOT}/lib/lvgl          # lvgl.h, lvgl_private.h
        ${REPO_ROOT}/lib/helix-xml     # helix_xml.h
        ${CMAKE_CURRENT_LIST_DIR}/shim         # spdlog_shim.h, hv_json_shim.h, platform_stubs.h
        ${CMAKE_CURRENT_LIST_DIR}/shim/include # <spdlog/spdlog.h>, <hv/json.hpp> aliases (Task 2 sweep + Task 3 slice)
    PRIV_REQUIRES pthread esp_timer)

# LV_KCONFIG_IGNORE: helix-xml ships lv_conf_internal.h without the sibling
# lv_conf_kconfig.h; we configure via lv_conf.h, not ESP-IDF Kconfig.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
# LVGL 9.5 sources trip -Wformat and old-style warnings on Xtensa GCC; the
# audit measures compile/link viability, not warning hygiene.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-unused-variable)
