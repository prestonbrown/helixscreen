// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "esp_err.h"

// Mounts both partitions the app needs before UI/asset access:
//   /assets    <- "storage" partition: a read-only packed frogfs container
//                 (minified ui_xml in every shipped language + config JSON +
//                 printer image renditions — see scripts/esp32_pack_assets.py).
//                 Reflashed whole on asset updates; frogfs_init() either
//                 finds a valid container or fails loudly, never silently.
//   /config    <- "cfg" partition (LittleFS, persistent settings; survives
//                 asset reflashes; format-on-fail so a first boot or corrupt
//                 filesystem self-heals instead of bricking settings access)
// Logs mounted/total bytes for both. Returns the first mount failure (storage
// checked first); callers decide whether that's fatal.
esp_err_t storage_mount(void);
