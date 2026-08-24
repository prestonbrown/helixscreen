# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "audit_main.c" "noto_sans_cjk_16_compiled.c"
                       PRIV_REQUIRES helixcore helixapp esp_lcd driver esp_timer joltwallet__littlefs
                       INCLUDE_DIRS "")

# Task 3: pack the repo's real ui_xml/ tree into the LittleFS "storage"
# partition. Configure-time copy — rerun cmake to pick up XML edits (fine for
# an audit; the app loads XML at runtime from /littlefs/ui_xml/...).
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
cmake_path(NORMAL_PATH REPO_ROOT)
set(FS_ROOT ${CMAKE_BINARY_DIR}/fs_root)
file(MAKE_DIRECTORY ${FS_ROOT})
file(COPY ${REPO_ROOT}/ui_xml DESTINATION ${FS_ROOT})
# Task 4 CJK viability: one runtime .bin font on LittleFS (Test A — measures
# lv_binfont_create PSRAM cost + load time; Test B is the compiled-in subset
# above, which lives in .rodata and is XIP'd from flash).
file(COPY ${REPO_ROOT}/assets/fonts/cjk/noto_sans_cjk_16.bin
     DESTINATION ${FS_ROOT}/assets/fonts/cjk)
littlefs_create_partition_image(storage ${FS_ROOT} FLASH_IN_PROJECT)
