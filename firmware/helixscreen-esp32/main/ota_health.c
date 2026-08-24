// SPDX-License-Identifier: GPL-3.0-or-later
#include "ota_health.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char* TAG = "ota";

void ota_health_confirm(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_app_desc_t* desc = esp_app_get_description();
    ESP_LOGI(TAG, "running slot=%s version=%s", running->label, desc->version);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA image confirmed valid (rollback cancelled)");
    }
}
