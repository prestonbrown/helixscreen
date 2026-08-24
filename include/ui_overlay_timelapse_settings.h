// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "i_moonraker_api.h"
#include "lvgl.h"
#include "moonraker_types.h"
#include "overlay_base.h"

#include <string>

/**
 * @file ui_overlay_timelapse_settings.h
 * @brief Timelapse settings overlay panel
 *
 * Configures Moonraker-Timelapse plugin settings for recording prints.
 * Provides UI for enabling timelapse, selecting recording mode, and
 * configuring output settings.
 *
 * ## Features
 * - Enable/disable timelapse recording
 * - Recording mode: Layer Macro (per-layer) or Hyperlapse (time-based)
 * - Output framerate selection (15/24/30/60 fps)
 * - Auto-render toggle (create video when print completes)
 *
 * ## Moonraker API
 * - machine.timelapse.get_settings - Fetch current settings
 * - machine.timelapse.post_settings - Update settings
 *
 * @see docs/FEATURE_STATUS.md for implementation progress
 */
class TimelapseSettingsOverlay : public OverlayBase {
  public:
    /**
     * @brief Construct TimelapseSettingsOverlay
     * @param api Pointer to IMoonrakerAPI (may be nullptr in test mode)
     */
    explicit TimelapseSettingsOverlay(IMoonrakerAPI* api);

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
     * @return "Timelapse Settings"
     */
    [[nodiscard]] const char* get_name() const override {
        return "Timelapse Settings";
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
     * @return "timelapse_settings_overlay"
     */
    [[nodiscard]] const char* get_xml_component_name() const {
        return "timelapse_settings_overlay";
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
     * @brief Fetch current settings from Moonraker
     */
    void fetch_settings();

    /**
     * @brief Update settings to Moonraker
     */
    void save_settings();

    /**
     * @brief Update mode info text based on current selection
     * @param mode_index 0=Layer Macro, 1=Hyperlapse
     */
    void update_mode_info(int mode_index);

    // Event handlers
    static void on_enabled_changed(lv_event_t* e);
    static void on_mode_changed(lv_event_t* e);
    static void on_framerate_changed(lv_event_t* e);
    static void on_autorender_changed(lv_event_t* e);

    //
    // === Injected Dependencies ===
    //

    IMoonrakerAPI* api_;

    // Current settings (loaded from API)
    TimelapseSettings current_settings_;
    bool settings_loaded_ = false;

    // Widget references
    lv_obj_t* enable_switch_ = nullptr;
    lv_obj_t* mode_dropdown_ = nullptr;
    lv_obj_t* mode_info_text_ = nullptr;
    lv_obj_t* framerate_dropdown_ = nullptr;
    lv_obj_t* autorender_switch_ = nullptr;

    // Framerate values for dropdown index mapping
    static constexpr int FRAMERATE_VALUES[] = {15, 24, 30, 60};
    static constexpr int FRAMERATE_COUNT = 4;

    /**
     * @brief Convert framerate value to dropdown index
     */
    static int framerate_to_index(int framerate);

    /**
     * @brief Convert dropdown index to framerate value
     */
    static int index_to_framerate(int index);
};

// Global accessor
TimelapseSettingsOverlay& get_global_timelapse_settings();
void init_global_timelapse_settings(IMoonrakerAPI* api);

/// Open the timelapse settings overlay (lazy-creates if needed)
void open_timelapse_settings();
