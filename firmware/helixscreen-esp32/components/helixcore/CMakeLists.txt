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

# Medium-tier (800x480 K-Touch) compiled font faces. These are FIRMWARE-LOCAL
# RLE-COMPRESSED twins of the assets/fonts/ faces (bitmap_format=1), regenerated
# from the SAME TTFs / codepoint ranges / bpp by scripts/esp32_regen_compressed_
# fonts.sh with lv_font_conv compression enabled — identical symbol names, glyph
# sets, and sizes, ~44% smaller in flash. Decompressed at render time via
# LV_USE_FONT_COMPRESSED=1 (lv_conf.h). We do NOT reuse assets/fonts/*.c: those
# stay plain (bitmap_format=0) because they're shared with desktop and weak ARM
# devices where per-glyph decompression cost is untested. Kept in lockstep with
# LV_FONT_CUSTOM_DECLARE in lv_conf.h (all moved faces non-const; only the
# noto_sans_18 anchor stays compiled). CJK companions are Plan 5.
#
# 10 faces (noto_sans_bold_28, noto_sans_light_16, noto_sans_light_12,
# mdi_icons_48, mdi_icons_64, noto_sans_26, source_code_pro_14, mdi_icons_16,
# mdi_icons_24, mdi_icons_32) are NO LONGER compiled in here: their glyph data
# now lives in runtime .bin files in the frogfs storage partition (Stage B
# fonts->frogfs enabler) and is loaded at boot by main/font_registration.c.
# moved_fonts_shim.c (component root, NOT under fonts/) provides their zero-init
# writable lv_font_t symbols so every &symbol call site keeps linking; the boot
# loader struct-copies the .bin-loaded font into each shim. Only noto_sans_18
# remains compiled in — it is the boot-time fallback anchor for the shim loads.
set(HELIX_FONT_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/fonts/noto_sans_18.c
    ${CMAKE_CURRENT_LIST_DIR}/moved_fonts_shim.c)

# ESP32-S3 PIE SIMD blend/fill assembly (vendored esp_lvgl_port, Apache-2.0 —
# see simd/esp_lvgl_port_lv_blend.h for provenance). The *_esp32s3.S files
# define the generic lv_*_esp symbols the glue header's static inlines call
# from inside LVGL's blend translation units; lv_macro_*.S are include-only
# macro libraries the others pull in.
file(GLOB HELIX_SIMD_SRCS ${CMAKE_CURRENT_LIST_DIR}/simd/*_esp32s3.S)

idf_component_register(
    SRCS ${LVGL_SRCS} ${HELIX_XML_SRCS} ${HELIX_FONT_SRCS} ${HELIX_SIMD_SRCS}
         anomaly_stub.c helix_lvgl_abi_check.c
    INCLUDE_DIRS
        ${CMAKE_CURRENT_LIST_DIR}      # adapted lv_conf.h (RGB565)
        ${CMAKE_CURRENT_LIST_DIR}/simd # esp_lvgl_port_lv_blend.h (ASM_CUSTOM)
        ${REPO_ROOT}/lib/lvgl          # lvgl.h, lvgl_private.h
        ${REPO_ROOT}/lib/lvgl/src      # helix-xml submodule includes <misc/lv_types.h>, <lv_conf_internal.h> (desktop parity: LVGL_INC has both)
        ${REPO_ROOT}/lib/helix-xml     # helix_xml.h
    PRIV_REQUIRES pthread esp_timer)

# Render hot path at -O2. The image ships -Os globally (a global -O2 measured
# +1.4MB over BOTH ota slots — a hard overflow), but the software renderer's
# blend/fill/glyph loops are where full-screen repaint time goes (measured
# ~183ms/frame at -Os on the S3), so only lib/lvgl/src/draw/** gets -O2.
# GCC honors the last -O flag, so appending -O2 per-source overrides the
# global -Os for exactly these files.
file(GLOB_RECURSE LVGL_DRAW_SRCS ${REPO_ROOT}/lib/lvgl/src/draw/*.c)
set_source_files_properties(${LVGL_DRAW_SRCS} PROPERTIES COMPILE_OPTIONS "-O2")

# LV_KCONFIG_IGNORE: helix-xml ships lv_conf_internal.h without the sibling
# lv_conf_kconfig.h; we configure via lv_conf.h, not ESP-IDF Kconfig.
# LV_LVGL_H_INCLUDE_SIMPLE: the font .c sources under assets/fonts/ guard
# their include with this macro (desktop resolves the #else branch,
# "lvgl/lvgl.h", via its own -isystem lib path instead) — firmware only
# exposes lib/lvgl as an include dir, so take the #include "lvgl.h" branch.
# LV_CONF_PATH pins the ONE lv_conf.h every ESP component compiles against.
# Without it, helixapp's include path (which carries ${REPO_ROOT}) resolves the
# repo-root DESKTOP lv_conf.h (LV_COLOR_DEPTH=32) while helixcore/LVGL resolves
# this firmware one (16) — two configs in one image, divergent lv_color_t and
# lv_style_t layouts, memory corruption across the boundary. LV_CONF_PATH takes
# precedence over LV_CONF_INCLUDE_SIMPLE in lv_conf_internal.h, so the repo-root
# copy becomes unreachable in firmware builds regardless of -I ordering.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    "LV_CONF_PATH=\"${CMAKE_CURRENT_LIST_DIR}/lv_conf.h\""
    LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE
    LV_LVGL_H_INCLUDE_SIMPLE)
# LVGL 9.5 sources trip -Wformat and old-style warnings on Xtensa GCC; the
# audit measures compile/link viability, not warning hygiene.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-unused-variable)
