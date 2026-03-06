// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_led.h
 * @brief LED Settings overlay - startup preference, auto state, macro devices
 *
 * This overlay allows users to configure:
 * - LED on at start preference
 * - Automatic LED state control (color changes based on printer state)
 * - View auto-detected macro LED devices
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see LedController for LED config persistence
 * @see LedAutoState for automatic state control
 */

#pragma once

#include "led/led_auto_state.h"
#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

#include <set>
#include <string>
#include <vector>

namespace helix::settings {

/**
 * @class LedSettingsOverlay
 * @brief Overlay for configuring LED-related settings
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_led_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class LedSettingsOverlay : public OverlayBase {
  public:
    LedSettingsOverlay();
    ~LedSettingsOverlay() override;

    // Non-copyable
    LedSettingsOverlay(const LedSettingsOverlay&) = delete;
    LedSettingsOverlay& operator=(const LedSettingsOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "LED Settings";
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

    void handle_led_on_at_start_changed(bool enabled);
    void handle_startup_brightness_changed(int value);
    void handle_auto_state_changed(bool enabled);
    void handle_add_macro_device();
    void handle_edit_macro_device(int index);
    void handle_delete_macro_device(int index);
    void handle_save_macro_device(int index);

  private:
    //
    // === Internal Methods ===
    //

    void init_led_on_at_start_toggle();
    void init_auto_state_toggle();
    void discard_unsaved_macro_entry();
    void populate_macro_devices();
    void populate_macro_devices_impl();
    void rebuild_macro_edit_controls(lv_obj_t* container, int index);
    void populate_led_chips();
    void populate_led_chips_impl();
    void handle_led_chip_clicked(const std::string& led_name);

    // Auto-state mapping editor
    void populate_auto_state_rows();
    void rebuild_contextual_controls(const std::string& state_key, lv_obj_t* container);
    void handle_action_type_changed(const std::string& state_key, int dropdown_index);
    void handle_brightness_changed(const std::string& state_key, int value);
    void handle_color_selected(const std::string& state_key, uint32_t color);
    void handle_effect_selected(const std::string& state_key, const std::string& name);
    void handle_wled_preset_selected(const std::string& state_key, int preset_id);
    void handle_macro_selected(const std::string& state_key, const std::string& gcode);
    void save_and_evaluate(const std::string& state_key);

    // LED chip selection state
    std::vector<std::string> discovered_leds_;
    std::set<std::string> selected_leds_;

    // Auto-state editor state
    SubjectManager subjects_;
    lv_subject_t auto_state_enabled_subject_{};
    lv_subject_t led_on_at_start_subject_{};
    std::vector<std::string> action_type_options_; // Maps dropdown index to action type string
    int editing_macro_index_ = -1;                 // -1 = no macro device being edited

    //
    // === Static Callbacks ===
    //

    static void on_led_on_at_start_changed(lv_event_t* e);
    static void on_startup_brightness_changed(lv_event_t* e);
    static void on_auto_state_changed(lv_event_t* e);
    static void on_add_macro_device(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton LedSettingsOverlay
 */
LedSettingsOverlay& get_led_settings_overlay();

} // namespace helix::settings
