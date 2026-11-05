// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/static_panel_registry.h"
#include "lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

namespace helix {
class IMoonrakerAPI;
}

/**
 * @file ui_overlay_retraction_settings.h
 * @brief Firmware retraction settings overlay panel
 *
 * Configures Klipper firmware_retraction module parameters for G10/G11 retraction.
 * Provides sliders for retract length, speed, unretract extra, and unretract speed.
 *
 * ## Features
 * - Enable/disable firmware retraction
 * - Retract length (0-6mm, 0.1mm steps)
 * - Retract speed (10-80 mm/s)
 * - Unretract extra length (0-1mm, 0.1mm steps)
 * - Unretract speed (10-60 mm/s)
 *
 * ## Klipper G-codes
 * - SET_RETRACTION RETRACT_LENGTH=X RETRACT_SPEED=Y UNRETRACT_EXTRA_LENGTH=Z UNRETRACT_SPEED=W
 *
 * Values are stored in PrinterState subjects and synced from Moonraker subscription.
 */
class RetractionSettingsOverlay : public OverlayBase {
  public:
    /**
     * @param api IMoonrakerAPI for sending G-code (may be nullptr until set_api())
     */
    explicit RetractionSettingsOverlay(helix::IMoonrakerAPI* api = nullptr);
    ~RetractionSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    /// Builds the overlay and caches the slider and switch widgets.
    lv_obj_t* create(lv_obj_t* parent) override;

    [[nodiscard]] const char* get_name() const override {
        return "Retraction Settings";
    }
    const char* xml_component() const override {
        return "retraction_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    void on_activate() override;

    /**
     * @brief Update IMoonrakerAPI pointer
     * @param api New API pointer (may be nullptr)
     */
    void set_api(helix::IMoonrakerAPI* api) {
        api_ = api;
    }

    /**
     * @brief Rows that can be typed into, not just dragged.
     *
     * Doubles as the user_data on each setting_value_field in
     * retraction_settings_overlay.xml, so the order here and the numbers there
     * must agree.
     */
    enum class Field : int {
        RetractLength = 0,
        RetractSpeed,
        UnretractExtra,
        UnretractSpeed,
        Count
    };

  private:
    /**
     * @brief Send SET_RETRACTION G-code with current values
     */
    void send_retraction_settings();

    /**
     * @brief Update display labels from current slider values
     */
    void update_display_labels();

    /**
     * @brief Sync UI sliders from PrinterState subjects
     */
    void sync_from_printer_state();

    /// ui_keypad_callback_t; user_data is the owning overlay.
    static void on_keypad_value(float value, void* user_data);

    /// Open the numeric keypad for one row, ranged from that row's own slider.
    void handle_field_clicked(Field field);

    /// Apply a keypad value: move the slider, then take the normal change path.
    void handle_keypad_value(Field field, double value);

    /// The row's slider, or nullptr before the overlay is built.
    lv_obj_t* field_slider(Field field) const;

    // Widget references
    lv_obj_t* enable_switch_ = nullptr;
    /// Set while our own numeric keypad is open; see MachineLimitsOverlay for
    /// why returning from it must not re-sync.
    bool returning_from_keypad_ = false;

    /// Row the open keypad is editing; one keypad exists at a time.
    Field pending_keypad_field_ = Field::RetractLength;

    lv_obj_t* retract_length_slider_ = nullptr;
    lv_obj_t* retract_speed_slider_ = nullptr;
    lv_obj_t* unretract_extra_slider_ = nullptr;
    lv_obj_t* unretract_speed_slider_ = nullptr;

    /// A rebuilt tree has no typed value to keep, so its first activate syncs.
    void on_ui_destroyed() override {
        enable_switch_ = nullptr;
        retract_length_slider_ = nullptr;
        retract_speed_slider_ = nullptr;
        unretract_extra_slider_ = nullptr;
        unretract_speed_slider_ = nullptr;
        returning_from_keypad_ = false;
    }

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Display label subjects
    lv_subject_t retract_length_display_{};
    lv_subject_t retract_speed_display_{};
    lv_subject_t unretract_extra_display_{};
    lv_subject_t unretract_speed_display_{};

    // Static buffers for subject strings
    char retract_length_buf_[16];
    char retract_speed_buf_[16];
    char unretract_extra_buf_[16];
    char unretract_speed_buf_[16];

    //
    // === Injected Dependencies ===
    //

    helix::IMoonrakerAPI* api_ = nullptr;

    // Debounce - don't send G-code while syncing from printer state
    bool syncing_from_state_ = false;
};

inline RetractionSettingsOverlay& get_global_retraction_settings() {
    return helix::lazy_global<RetractionSettingsOverlay>("RetractionSettingsOverlay", nullptr);
}

/// Points the overlay at the API it sends G-code through.
inline void init_global_retraction_settings(helix::IMoonrakerAPI* api) {
    get_global_retraction_settings().set_api(api);
}
