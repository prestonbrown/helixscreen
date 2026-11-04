// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.h
 * @brief Printing Settings overlay - machine limits, retraction, multi-filament system,
 *        filament temps and cooldown behaviour, timelapse
 *
 * This overlay allows users to configure:
 * - Machine velocity/acceleration limits
 * - Firmware retraction settings (when available)
 * - Enclosure marking
 * - Multi-filament system management (when an AMS is detected)
 * - Material temperature presets
 * - Cold extrude / post-op nozzle cooldown behaviour
 * - Timelapse recording (when available)
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SettingsManager for persistence
 * @see DisplaySettingsManager for G-code render mode
 */

#pragma once

#include "core/observer_factory.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

/**
 * @class PrintingSettingsOverlay
 * @brief Overlay for configuring printing-related settings
 *
 * @code
 * helix::settings::get_printing_settings_overlay().show(parent_screen);
 * @endcode
 */
class PrintingSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Printing Settings";
    }
    const char* xml_component() const override {
        return "settings_printing_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

  private:
    /// Re-asks the mesh-include status whenever the plr feature publishes a
    /// non-Absent state, so the rows cannot sit on a stale or missing answer
    /// while the page is open.
    ObserverGuard plr_feature_observer_;
};

inline PrintingSettingsOverlay& get_printing_settings_overlay() {
    return lazy_global<PrintingSettingsOverlay>("PrintingSettingsOverlay");
}

} // namespace helix::settings
