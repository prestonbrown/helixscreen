# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task 3: the vertical-slice app core, compiled UNMODIFIED from src/ via
# relative paths (same rule as helixcore). A separate component so
# `idf.py size-components` attributes app-core bytes separately from
# LVGL/helix-xml. Grown source-by-source by the link loop; every stub the
# loop forced lives in audit_stubs.cpp / audit_moonraker_stub.cpp with a
# rationale comment.

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)
cmake_path(NORMAL_PATH REPO_ROOT)
set(SHIM ${CMAKE_CURRENT_LIST_DIR}/../helixcore/shim)

# Source list lives in app_srcs.txt so link_loop.py can grow it without
# CMake edits; CONFIGURE_DEPENDS makes edits trigger reconfigure.
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/app_srcs.txt APP_SRCS_REL
     REGEX "^[^#].*\\.(cpp|c)$")
# "overrides/..." lines are audit-tree copies of src/ files that need an
# ESP-specific tweak (plan-permitted; never edit src/). Everything else is
# repo-relative and compiled UNMODIFIED.
set(APP_SRCS "")
foreach(rel ${APP_SRCS_REL})
    if(rel MATCHES "^overrides/")
        list(APPEND APP_SRCS ${CMAKE_CURRENT_LIST_DIR}/../../${rel})
    else()
        list(APPEND APP_SRCS ${REPO_ROOT}/${rel})
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_LIST_DIR}/app_srcs.txt)

idf_component_register(
    SRCS ${APP_SRCS} audit_app.cpp audit_stubs.cpp audit_moonraker_stub.cpp audit_platform_stubs2.cpp audit_stb_impl.c audit_fake_typeinfo.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES helixcore pthread esp_timer)

# The PCH -include is C++-only: font/image .c sources compile as plain C.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    -w
    $<$<COMPILE_LANGUAGE:CXX>:-include>
    $<$<COMPILE_LANGUAGE:CXX>:${REPO_ROOT}/include/lvgl_pch.h>)

# Mirrors sweep.py / the Linux Makefile compile line, minus Linux-only flags.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HELIX_VERSION="0.0.0-audit"
    HELIX_VERSION_MAJOR=0 HELIX_VERSION_MINOR=0 HELIX_VERSION_PATCH=0
    HELIX_GIT_HASH="audit"
    INSTALLER_FILENAME="install.sh"
    HELIX_MAX_FONT_TIER=6
    HELIX_ENABLE_MOCKS
    HELIX_ENABLE_SCREENSAVER
    HELIX_HAS_LABEL_PRINTER=1
    HELIX_HAS_CFS=1
    HELIX_HAS_IFS=1
    # Renderers compiled out on ESP32: 2D gcode preview allocates 3x
    # widget-size ARGB8888 buffers, bed-mesh 3D 2x render-size + a thread —
    # both untenable in 8MB PSRAM next to the framebuffer.
    HELIX_HAS_GCODE_VIEWER=0
    HELIX_HAS_BED_MESH_3D=0)

target_include_directories(${COMPONENT_LIB} PRIVATE
    ${REPO_ROOT}
    ${REPO_ROOT}/include
    ${REPO_ROOT}/src/generated
    ${REPO_ROOT}/build/generated
    ${SHIM}/include
    ${SHIM}/hv_stub
    ${SHIM})
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    ${REPO_ROOT}/lib
    ${REPO_ROOT}/lib/glm
    ${REPO_ROOT}/lib/lvgl
    ${REPO_ROOT}/lib/lvgl/src
    ${REPO_ROOT}/lib/stb
    ${REPO_ROOT}/lib/lv_markdown/src
    ${REPO_ROOT}/lib/lv_markdown/deps/md4c
    ${REPO_ROOT}/lib/quirc/lib)
