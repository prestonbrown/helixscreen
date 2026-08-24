// SPDX-License-Identifier: GPL-3.0-or-later
//
// spdlog-shim funnel → esp_log. See platform_stubs.h.

#include "platform_stubs.h"

#include "esp_log.h"

static const char* TAG = "helixnet";

void helix_shim_log(int level, const char* msg) {
    switch (level) {
    case 0: // spdlog::level::trace
        esp_log_write(ESP_LOG_VERBOSE, TAG, "%s\n", msg);
        break;
    case 1: // debug
        esp_log_write(ESP_LOG_DEBUG, TAG, "%s\n", msg);
        break;
    case 2: // info
        esp_log_write(ESP_LOG_INFO, TAG, "%s\n", msg);
        break;
    case 3: // warn
        esp_log_write(ESP_LOG_WARN, TAG, "%s\n", msg);
        break;
    default: // err, critical
        esp_log_write(ESP_LOG_ERROR, TAG, "%s\n", msg);
        break;
    }
}
