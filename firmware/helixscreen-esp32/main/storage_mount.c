// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_mount.h"

#include "esp_littlefs.h"
#include "esp_log.h"
#include "frogfs/frogfs.h"
#include "frogfs/vfs.h"

#include <stdbool.h>

static const char* TAG = "storage_mount";

// Kept alive for the process lifetime (never deinit'd — assets are read for
// as long as the app runs); frogfs_init() mmaps the `storage` partition
// directly, so this costs no RAM beyond the frogfs_fs_t bookkeeping struct.
static frogfs_fs_t* s_assets_fs;

static esp_err_t mount_assets(void) {
    // storage: a read-only packed frogfs container (see
    // scripts/esp32_pack_assets.py). No format-on-fail concept applies here —
    // frogfs_init() either finds a valid container or it doesn't; a missing
    // FROGFS_MAGIC fails loudly instead of silently degrading, since nothing
    // on this partition is disposable app state.
    frogfs_config_t frogfs_config = {
        .part_label = "storage",
    };
    s_assets_fs = frogfs_init(&frogfs_config);
    if (!s_assets_fs) {
        ESP_LOGE(TAG, "frogfs_init failed for partition 'storage'");
        return ESP_FAIL;
    }

    frogfs_vfs_conf_t vfs_conf = {
        .base_path = "/assets",
        .fs = s_assets_fs,
        .max_files = 8,
    };
    esp_err_t err = frogfs_vfs_register(&vfs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frogfs_vfs_register('/assets') failed: %s", esp_err_to_name(err));
        frogfs_deinit(s_assets_fs);
        s_assets_fs = NULL;
        return err;
    }

    ESP_LOGI(TAG, "mounted 'storage' (frogfs, read-only) -> /assets");
    return ESP_OK;
}

static esp_err_t mount_cfg(void) {
    // cfg: format-on-fail. First boot (blank flash) and a corrupt filesystem
    // both self-heal instead of leaving settings permanently unreachable.
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/config",
        .partition_label = "cfg",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount 'cfg' -> /config failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("cfg", &total, &used);
    ESP_LOGI(TAG, "mounted 'cfg' -> /config: %u/%u KB used", (unsigned)(used / 1024),
             (unsigned)(total / 1024));
    return ESP_OK;
}

esp_err_t storage_mount(void) {
    esp_err_t storage_err = mount_assets();
    esp_err_t cfg_err = mount_cfg();
    return storage_err != ESP_OK ? storage_err : cfg_err;
}
