// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class HardwareSettingsOverlay
 * @brief Overlay for hardware and device settings
 *
 * Provides action rows for hardware health, camera, fans, sensors,
 * LEDs, power devices and spoolman. Each row opens an existing overlay
 * for the respective feature.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */
class HardwareSettingsOverlay : public OverlayBase {
  public:
    void register_callbacks() override;

    const char* get_name() const override {
        return "Devices";
    }
    const char* xml_component() const override {
        return "settings_hardware_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    /// Default create() plus the live Hardware Health row binding.
    lv_obj_t* create(lv_obj_t* parent) override;
};

inline HardwareSettingsOverlay& get_hardware_settings_overlay() {
    return lazy_global<HardwareSettingsOverlay>("HardwareSettingsOverlay");
}

/**
 * @brief Bind the Hardware Health row to live validation state
 *
 * Points the row's label at the issue-count summary and its icon colour at
 * hardware_status_level. The row lives inside settings_hardware_overlay, so
 * only something holding that tree can reach the widgets by name.
 *
 * @param overlay_root A settings_hardware_overlay tree
 */
void bind_hardware_health_row(lv_obj_t* overlay_root);

} // namespace helix::settings
