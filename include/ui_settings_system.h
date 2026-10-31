// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_system.h
 * @brief System settings overlay - security, telemetry, logging, admin
 *
 * This overlay provides access to system administration settings:
 * - Security (PIN lock)
 * - Storage
 * - Performance
 * - Telemetry
 * - Log level
 * - Restart / Factory reset
 *
 * The telemetry and log-level rows bind to their SystemSettingsManager subjects;
 * this overlay owns the row callbacks and the factory reset dialog.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "ui_widget_ref.h"

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

/**
 * @class SystemSettingsOverlay
 * @brief Overlay for system administration settings
 *
 * @code
 * helix::settings::get_system_settings_overlay().show(parent_screen);
 * @endcode
 */
class SystemSettingsOverlay : public OverlayBase {
  public:
    ~SystemSettingsOverlay() override;

    const char* get_name() const override {
        return "System";
    }
    const char* xml_component() const override {
        return "settings_system_overlay";
    }

    void register_callbacks() override;

    /// The Performance row's action: open the Performance overlay.
    void open_performance();

  private:
    void handle_restart_helix_clicked();
    void handle_factory_reset_clicked();
    void perform_factory_reset();

    /// Created on first use, deleted when its close animation finishes.
    helix::ui::WidgetRef factory_reset_dialog_;
};

inline SystemSettingsOverlay& get_system_settings_overlay() {
    return lazy_global<SystemSettingsOverlay>("SystemSettingsOverlay");
}

} // namespace helix::settings
