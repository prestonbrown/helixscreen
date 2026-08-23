// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_display_sound.h
 * @brief Display & Sound settings overlay - appearance, display, and sound in one panel
 *
 * Merges DisplaySettingsOverlay, SoundSettingsOverlay, and appearance items
 * (Language, Timezone, Time Format, Animations) from the main settings panel
 * into a single consolidated sub-panel with three sections:
 * - APPEARANCE: Language, timezone, time format, animations
 * - DISPLAY: Dark mode, theme colors, brightness, widget labels, bed mesh,
 *            screen dim, display sleep, screensaver, sleep while printing
 * - SOUND: Master toggle, volume, UI sounds, sound theme, test beep/tracker
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see DisplaySettingsManager for display persistence
 * @see AudioSettingsManager for sound persistence
 * @see SystemSettingsManager for language persistence
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "theme_loader.h"

#include <string>
#include <vector>

namespace helix::settings {

/**
 * @class DisplaySoundSettingsOverlay
 * @brief Overlay for configuring appearance, display, and sound settings
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_display_sound_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class DisplaySoundSettingsOverlay : public OverlayBase {
  public:
    DisplaySoundSettingsOverlay();
    ~DisplaySoundSettingsOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Display & Sound";
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

    // Appearance handlers
    void handle_language_changed(int index);
    void handle_timezone_changed(int index);
    void handle_time_format_changed(int index);
    void handle_animations_changed(bool enabled);
    void handle_system_keyboard_changed(bool enabled);
    void handle_keep_navbar_changed(bool enabled);

    // Display handlers
    void handle_display_rotation_changed(int index);
    void handle_dark_mode_changed(bool enabled);
    void handle_theme_settings_clicked();
    void handle_brightness_changed(int value);
    void handle_brightness_commit(int value);
    void handle_widget_labels_changed(bool enabled);
    void handle_page_scroll_buttons_changed(bool enabled);
    void handle_ui_scale_changed(int index);
    void handle_bed_mesh_mode_changed(int mode);
    void handle_dim_changed(int index);
    void handle_sleep_changed(int index);
    void handle_sleep_while_printing_changed(bool enabled);

    // Theme explorer handlers
    void handle_theme_preset_changed(int index);
    void handle_apply_theme_clicked();
    void handle_edit_colors_clicked();
    void handle_explorer_theme_changed(int index);
    void handle_preview_dark_mode_toggled(bool is_dark);
    void apply_preview_palette_to_screen_popups();

    // Sound handlers
    void handle_sounds_changed(bool enabled);
    void handle_volume_changed(int value);
    void handle_volume_commit(int value);
    void handle_ui_sounds_changed(bool enabled);
    void handle_sound_theme_changed(int index);
    void handle_audio_device_changed(int index);
    void handle_preview_sounds();
    void handle_test_tracker();

    /**
     * @brief Show theme preview overlay directly (for CLI access)
     */
    void show_theme_preview(lv_obj_t* parent_screen);

  private:
    //
    // === Internal Methods ===
    //

    // Appearance init
    void init_language_dropdown();
    void init_timezone_dropdown();
    void init_time_format_dropdown();
    void init_animations_toggle();

    // Display init
    void init_display_rotation_dropdown();
    void init_brightness_controls();
    void init_dim_dropdown();
    void init_sleep_dropdown();
    void init_sleep_while_printing_toggle();
    void init_ui_scale_dropdown();
    void init_bed_mesh_dropdown();
    void init_theme_preset_dropdown(lv_obj_t* root);

#ifdef HELIX_ENABLE_SCREENSAVER
    void init_screensaver_dropdown();
    void handle_test_screensaver();
#endif

    // Sound init
    void init_sounds_toggle();
    void init_volume_slider();
    void init_sound_theme_dropdown();
    void init_audio_device_dropdown();

    //
    // === State (Display) ===
    //

    /// Theme Editor overlay (secondary - for detailed color editing)
    lv_obj_t* theme_settings_overlay_{nullptr};
    /// Theme Explorer overlay (primary - for browsing and selecting themes)
    lv_obj_t* theme_explorer_overlay_{nullptr};

    /// Tracks original theme index for Apply button state
    int original_theme_index_{-1};
    /// Snapshot of active theme when explorer opens (for revert on close)
    helix::ThemeData original_theme_;
    /// Current preview dark mode state
    bool preview_is_dark_{true};
    /// Currently previewed theme name (for passing to editor)
    std::string preview_theme_name_;
    /// Cached theme list (populated when explorer opens, avoids re-parsing on every toggle)
    std::vector<helix::ThemeInfo> cached_themes_;

    /// Subject for brightness value label binding
    lv_subject_t brightness_value_subject_;
    char brightness_value_buf_[8]; // e.g., "100%"

    /// Subject for theme Apply button disabled state (1=disabled, 0=enabled)
    lv_subject_t theme_apply_disabled_subject_;

    //
    // === State (Sound) ===
    //

    /// Subject for volume value label binding
    char volume_value_buf_[8]; // e.g., "100%"

    //
    // === Static Callbacks ===
    //

    // Appearance
    static void on_language_changed(lv_event_t* e);
    static void on_timezone_changed(lv_event_t* e);
    static void on_time_format_changed(lv_event_t* e);
    static void on_animations_changed(lv_event_t* e);
    static void on_system_keyboard_changed(lv_event_t* e);
    static void on_keep_navbar_changed(lv_event_t* e);

    // Display
    static void on_display_rotation_changed(lv_event_t* e);
    static void on_dark_mode_changed(lv_event_t* e);
    static void on_brightness_changed(lv_event_t* e);
    static void on_brightness_commit(lv_event_t* e);
    static void on_widget_labels_changed(lv_event_t* e);
    static void on_page_scroll_buttons_changed(lv_event_t* e);
    static void on_ui_scale_changed(lv_event_t* e);
    static void on_bed_mesh_mode_changed(lv_event_t* e);
    static void on_dim_changed(lv_event_t* e);
    static void on_sleep_changed(lv_event_t* e);
    static void on_sleep_while_printing_changed(lv_event_t* e);
#ifdef HELIX_ENABLE_SCREENSAVER
    static void on_screensaver_changed(lv_event_t* e);
    static void on_test_screensaver(lv_event_t* e);
#endif

    // Theme explorer
    static void on_theme_preset_changed(lv_event_t* e);
    static void on_theme_settings_clicked(lv_event_t* e);
    static void on_apply_theme_clicked(lv_event_t* e);
    static void on_edit_colors_clicked(lv_event_t* e);
    static void on_preview_dark_mode_toggled(lv_event_t* e);
    static void on_preview_open_modal(lv_event_t* e);

    // Sound
    static void on_sounds_changed(lv_event_t* e);
    static void on_volume_changed(lv_event_t* e);
    static void on_volume_commit(lv_event_t* e);
    static void on_volume_released(lv_event_t* e);
    static void on_ui_sounds_changed(lv_event_t* e);
    static void on_sound_theme_changed(lv_event_t* e);
    static void on_audio_device_changed(lv_event_t* e);
    static void on_preview_sounds(lv_event_t* e);
    static void on_test_tracker(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton DisplaySoundSettingsOverlay
 */
DisplaySoundSettingsOverlay& get_display_sound_settings_overlay();

} // namespace helix::settings
