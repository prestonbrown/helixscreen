// SPDX-License-Identifier: GPL-3.0-or-later
// Stub for the telemetry hook that patches/*.patch inject into LVGL
// (grid walk-off guard, event-list instrumentation). On Linux the app
// provides this via src/system/helix_lvgl_anomaly.cpp; here it logs.
#include "esp_log.h"

void helix_lvgl_anomaly(const char *code, const char *context) {
    ESP_LOGW("lvgl_anomaly", "%s: %s", code ? code : "?", context ? context : "");
}
