// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

class IMoonrakerAPI;

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
     * @brief Construct RetractionSettingsOverlay
     * @param api Pointer to IMoonrakerAPI for sending G-code
     */
    explicit RetractionSettingsOverlay(IMoonrakerAPI* api);
    ~RetractionSettingsOverlay() override;

    //
    // === OverlayBase Implementation ===
    //

    /**
     * @brief Initialize subjects for XML binding
     */
    void init_subjects() override;

    /**
     * @brief Create overlay UI from XML
     *
     * @param parent Parent widget to attach overlay to (usually screen)
     * @return Root object of overlay, or nullptr on failure
     */
    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Get human-readable overlay name
     * @return "Retraction Settings"
     */
    [[nodiscard]] const char* get_name() const override {
        return "Retraction Settings";
    }

    /**
     * @brief Called when overlay becomes visible
     */
    void on_activate() override;

    /**
     * @brief Called when overlay is hidden
     */
    void on_deactivate() override;

    /**
     * @brief Clean up resources for async-safe destruction
     */
    void cleanup() override;

    //
    // === Legacy Compatibility ===
    //

    /**
     * @brief Get XML component name for lv_xml_create()
     * @return "retraction_settings_overlay"
     */
    [[nodiscard]] const char* get_xml_component_name() const {
        return "retraction_settings_overlay";
    }

    /**
     * @brief Get root panel object (alias for get_root())
     * @return Panel object, or nullptr if not yet created
     */
    lv_obj_t* get_panel() const {
        return overlay_root_;
    }

    /**
     * @brief Update IMoonrakerAPI pointer
     * @param api New API pointer (may be nullptr)
     */
    void set_api(IMoonrakerAPI* api) {
        api_ = api;
    }

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

    // Event handlers
    static void on_enabled_changed(lv_event_t* e);
    static void on_setting_changed(lv_event_t* e);

    // Widget references
    lv_obj_t* enable_switch_ = nullptr;
    lv_obj_t* retract_length_slider_ = nullptr;
    lv_obj_t* retract_speed_slider_ = nullptr;
    lv_obj_t* unretract_extra_slider_ = nullptr;
    lv_obj_t* unretract_speed_slider_ = nullptr;

    // Subject manager for automatic cleanup
    SubjectManager subjects_;

    // Display label subjects
    lv_subject_t retract_length_display_;
    lv_subject_t retract_speed_display_;
    lv_subject_t unretract_extra_display_;
    lv_subject_t unretract_speed_display_;

    // Static buffers for subject strings
    char retract_length_buf_[16];
    char retract_speed_buf_[16];
    char unretract_extra_buf_[16];
    char unretract_speed_buf_[16];

    //
    // === Injected Dependencies ===
    //

    IMoonrakerAPI* api_ = nullptr;

    // Debounce - don't send G-code while syncing from printer state
    bool syncing_from_state_ = false;
};

// Global accessor
RetractionSettingsOverlay& get_global_retraction_settings();
void init_global_retraction_settings(IMoonrakerAPI* api);
