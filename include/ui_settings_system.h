// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_system.h
 * @brief System settings overlay - security, network, host, admin
 *
 * This overlay provides access to system administration settings:
 * - Security (PIN lock)
 * - Network settings
 * - Host configuration
 * - Touch calibration
 * - Hardware health
 * - Plugins
 * - Telemetry
 * - Restart / Factory reset
 * - Restore Stock Screen (stock-firmware Snapmaker U1 only)
 *
 * Most callbacks are delegated to the global SettingsPanel which already
 * owns the complex logic (change host modal, factory reset dialog, etc.).
 * This overlay simply registers matching callback names and initializes
 * toggle/description state on activate.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class SystemSettingsOverlay
 * @brief Overlay for system administration settings
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_system_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class SystemSettingsOverlay : public OverlayBase {
  public:
    SystemSettingsOverlay();
    ~SystemSettingsOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "System";
    }

    void on_activate() override;
    void on_deactivate() override;

    //
    // === UI Creation ===
    //

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

  private:
    void init_telemetry_toggle();
    void init_touch_cal_description();
    void init_host_description();
    void init_log_level_dropdown();
    void handle_restore_stock_screen_clicked();
    void perform_restore_stock_screen();
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton SystemSettingsOverlay
 */
SystemSettingsOverlay& get_system_settings_overlay();

} // namespace helix::settings
