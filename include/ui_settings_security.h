// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_security.h
 * @brief Security Settings overlay — PIN management and auto-lock configuration.
 *
 * Exposes:
 * - Set PIN (when no PIN is configured)
 * - Change PIN (when PIN is configured)
 * - Remove PIN (when PIN is configured)
 * - Auto-lock toggle (when PIN is configured)
 *
 * Visibility of rows is driven by the global `lock_pin_set` subject from
 * LockManager (0 = no PIN, 1 = PIN set).
 *
 * @pattern Overlay (lazy init, StaticPanelRegistry cleanup)
 * @threading Main thread only
 *
 * @see LockManager for PIN storage and hashing
 * @see PinEntryModal for the numeric keypad dialog
 */

#pragma once

#include "core/static_panel_registry.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class SecuritySettingsOverlay
 * @brief Overlay for configuring screen lock PIN and auto-lock behaviour.
 */
class SecuritySettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Security Settings";
    }
    const char* xml_component() const override {
        return "security_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void register_callbacks() override;
    void on_activate() override;

    void handle_change_pin_clicked();
    void handle_remove_pin_clicked();
    /// Two-step "Enter New PIN" then "Confirm PIN" dialog.
    void run_set_pin_flow();

  private:
    void init_auto_lock_toggle();
};

inline SecuritySettingsOverlay& get_security_settings_overlay() {
    return lazy_global<SecuritySettingsOverlay>("SecuritySettingsOverlay");
}

} // namespace helix::settings
