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
    explicit TimelapseSettingsOverlay(helix::IMoonrakerAPI* api);

    [[nodiscard]] const char* get_name() const override {
        return "Timelapse Settings";
    }
    [[nodiscard]] const char* xml_component() const override {
        return "timelapse_settings_overlay";
    }
    bool destroy_on_close() const override {
        return true;
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void register_callbacks() override;
    void on_activate() override;

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
    void set_api(helix::IMoonrakerAPI* api) {
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

    //
    // === Injected Dependencies ===
    //

    helix::IMoonrakerAPI* api_;

    // Current settings (loaded from API)
    TimelapseSettings current_settings_;
    bool settings_loaded_ = false;

    // Widget references
    lv_obj_t* enable_switch_ = nullptr;
    lv_obj_t* mode_dropdown_ = nullptr;
    lv_obj_t* mode_info_text_ = nullptr;
    lv_obj_t* framerate_dropdown_ = nullptr;
    lv_obj_t* autorender_switch_ = nullptr;

    void on_ui_destroyed() override {
        enable_switch_ = nullptr;
        mode_dropdown_ = nullptr;
        mode_info_text_ = nullptr;
        framerate_dropdown_ = nullptr;
        autorender_switch_ = nullptr;
    }

    // Framerate values for dropdown index mapping
    static constexpr int kFramerateValues[] = {15, 24, 30, 60};
    static constexpr int kFramerateCount = 4;

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
void init_global_timelapse_settings(helix::IMoonrakerAPI* api);

/// Open the timelapse settings overlay (lazy-creates if needed)
void open_timelapse_settings();
