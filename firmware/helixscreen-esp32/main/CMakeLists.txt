# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "app_main.c" "board_display.c" "lvgl_glue.c" "touch_input.c" "ota_health.c"
                             "storage_mount.c" "net_hil.cpp" "font_registration.c"
                       PRIV_REQUIRES esp_timer app_update esp_lcd driver helixcore helixnet helixapp pthread
                                     esp_wifi esp_netif esp_event nvs_flash joltwallet__littlefs
                                     jkent__frogfs
                       INCLUDE_DIRS "." "../boards")

# font_registration.c's helix_fonts_register() has no caller yet (Task 6
# wires the call site before theme init) and defines no symbol any other
# object needs, so two things would otherwise happen silently: (1) the
# linker's normal archive-extraction rule skips font_registration.c.o
# entirely — confirmed by a byte-for-byte identical binary size before/after
# adding the font sources without this fix — and (2) even after forcing
# extraction (tried WHOLE_ARCHIVE + __attribute__((used)) first), plain
# -Wl,--gc-sections still discarded the now-extracted-but-still-uncalled
# function, per the link map (.literal.helix_fonts_register / noto_sans_26
# etc. showed up with address 0x0 — collected, not placed). --undefined is
# the standard fix for exactly this "registered but not yet called" shape:
# it forces ld to (a) search archives for the symbol, extracting
# font_registration.c.obj (and, transitively, every font .obj it references)
# and (b) treat that symbol as a GC root so --gc-sections can't then discard
# it. Task 6 adding the real call site makes this redundant but harmless.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=helix_fonts_register")

# `storage` partition image: a packed frogfs (jkent/frogfs) compressed
# container, built from the staging tree scripts/esp32_stage_assets.py
# assembles (minified ui_xml in every shipped language, config JSON, printer
# image renditions) by scripts/esp32_pack_assets.py. Both are explicit,
# inspectable pre-steps — like the staging script itself, packing is
# deliberately NOT something CMake shells out to. Two build-time guards cover
# the two ways the packed image can be wrong: the EXISTS check right below
# only runs at CMake configure time and only catches it being MISSING; the
# check_storage_asset_staleness custom target further down has no OUTPUT so
# it re-runs on every `idf.py build` (not just reconfigure) and catches it
# being STALE (image exists but ui_xml/assets/staging changed since the last
# pack). Both fail loudly with next-step instructions instead of silently
# flashing a missing, stale, or zero-length image. `cfg` (settings) is
# unaffected — this partition swap only touches `storage`.
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
cmake_path(NORMAL_PATH REPO_ROOT)
set(STORAGE_IMAGE ${CMAKE_BINARY_DIR}/storage_frogfs.bin)
if(NOT EXISTS ${STORAGE_IMAGE})
    message(FATAL_ERROR
        "Packed asset container missing: ${STORAGE_IMAGE}\n"
        "Run 'python3 <repo_root>/scripts/esp32_stage_assets.py' then "
        "'python3 <repo_root>/scripts/esp32_pack_assets.py' first (writes here by "
        "default), then re-run idf.py build.")
endif()

# Build-time staleness guard (see comment above): compares the packed image's
# mtime against the newest mtime under every tree that feeds it. Checked
# sources: ui_xml/ and assets/ (what stage_assets.py reads), the printer-image
# rendition build dir (what stage_assets.py copies from, produced by
# esp32_printer_images.py), and the staging tree itself (what pack_assets.py
# reads) — covers both "edited a source and forgot to re-stage" and "re-staged
# but forgot to re-pack". No OUTPUT means ninja/make always considers this
# target out of date and re-runs the check every build.
idf_build_get_property(python PYTHON)
add_custom_target(check_storage_asset_staleness ALL
    COMMAND ${python} ${REPO_ROOT}/scripts/esp32_check_asset_staleness.py
            ${STORAGE_IMAGE}
            ${REPO_ROOT}/ui_xml
            ${CMAKE_CURRENT_LIST_DIR}/../ui_xml_overrides
            ${REPO_ROOT}/assets/config
            ${REPO_ROOT}/assets/filaments.json
            ${REPO_ROOT}/assets/images/printers
            ${REPO_ROOT}/build/esp32_printer_images
            ${CMAKE_BINARY_DIR}/littlefs_staging
    COMMENT "Checking packed storage asset image is not stale"
    VERBATIM)

partition_table_get_partition_info(storage_offset "--partition-name storage" "offset")
if(NOT storage_offset)
    message(FATAL_ERROR "Could not find offset of 'storage' partition in partitions.csv")
endif()
idf_component_get_property(main_args esptool_py FLASH_ARGS)
idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
esptool_py_flash_target(storage-flash "${main_args}" "${sub_args}")
esptool_py_flash_target_image(storage-flash "storage" "${storage_offset}" "${STORAGE_IMAGE}")
esptool_py_flash_target_image(flash "storage" "${storage_offset}" "${STORAGE_IMAGE}")

# net_hil.cpp (Task 10, gated behind CONFIG_HELIX_NET_HIL) calls into
# EspMoonrakerClient's IMoonrakerClient contract directly, which needs the same
# repo-header + shim include paths helixnet's own sources use — those are
# PRIVATE to the helixnet target and don't propagate to a sibling component.
# Mirrors components/helixnet/CMakeLists.txt; keep the two in lockstep.
# (REPO_ROOT set earlier, alongside the storage-image guards above.)
set(HELIXNET_SHIM ${CMAKE_CURRENT_LIST_DIR}/../components/helixnet/shim)

target_include_directories(${COMPONENT_LIB} PRIVATE
    ${REPO_ROOT}/include        # i_moonraker_client.h + moonraker_*/printer_* headers
    ${HELIXNET_SHIM}/include    # <spdlog/spdlog.h>, <hv/json.hpp> aliases
    ${HELIXNET_SHIM})           # spdlog_shim.h, hv_json_shim.h, platform_stubs.h
target_include_directories(${COMPONENT_LIB} SYSTEM PRIVATE
    ${REPO_ROOT}/lib)           # libhv/include/hv/json.hpp + spdlog bundled fmt

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-format>)
