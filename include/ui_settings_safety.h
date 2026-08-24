// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_safety.h
 * @brief Safety & Notifications overlay - e-stop, cancel escalation, completion alerts
 *
 * This overlay allows users to configure:
 * - E-Stop confirmation toggle
 * - Cancel escalation toggle and timeout
 * - Print completion alert mode
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SafetySettingsManager for persistence
 * @see AudioSettingsManager for completion alert persistence
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class SafetySettingsOverlay
 * @brief Overlay for configuring safety and notification settings
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_safety_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class SafetySettingsOverlay : public OverlayBase {
  public:
    SafetySettingsOverlay();
    ~SafetySettingsOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Safety & Notifications";
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

    //
    // === Event Handlers (public for static callbacks) ===
    //

    void handle_estop_confirm_changed(bool enabled);
    void handle_cancel_escalation_changed(bool enabled);
    void handle_cancel_escalation_timeout_changed(int index);
    void handle_completion_alert_changed(int index);
    void handle_min_toast_severity_changed(int index);
    void handle_macro_confirm_changed(bool enabled);
    void handle_allow_cold_extrude_changed(bool enabled);
    void handle_filament_auto_cooldown_changed(bool enabled);

  private:
    //
    // === Internal Methods ===
    //

    void init_estop_toggle();
    void init_completion_alert_dropdown();

    //
    // === Static Callbacks ===
    //

    static void on_estop_confirm_changed(lv_event_t* e);
    static void on_cancel_escalation_changed(lv_event_t* e);
    static void on_cancel_escalation_timeout_changed(lv_event_t* e);
    static void on_completion_alert_changed(lv_event_t* e);
    static void on_min_toast_severity_changed(lv_event_t* e);
    static void on_macro_confirm_changed(lv_event_t* e);
    static void on_allow_cold_extrude_changed(lv_event_t* e);
    static void on_filament_auto_cooldown_changed(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton SafetySettingsOverlay
 */
SafetySettingsOverlay& get_safety_settings_overlay();

} // namespace helix::settings
