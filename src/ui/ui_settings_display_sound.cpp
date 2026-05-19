// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_display_sound.cpp
 * @brief Implementation of DisplaySoundSettingsOverlay
 */

#include "ui_settings_display_sound.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_sound_preview_overlay.h"
#include "ui_theme_editor_overlay.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "alsa_device_enum.h"
#include "audio_settings_manager.h"
#include "border_radius_sizes.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "page_scroll_auto_inject.h"
#include "settings_manager.h"
#include "sound_manager.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<DisplaySoundSettingsOverlay> g_display_sound_settings_overlay;

DisplaySoundSettingsOverlay& get_display_sound_settings_overlay() {
    if (!g_display_sound_settings_overlay) {
        g_display_sound_settings_overlay = std::make_unique<DisplaySoundSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "DisplaySoundSettingsOverlay", []() { g_display_sound_settings_overlay.reset(); });
    }
    return *g_display_sound_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

DisplaySoundSettingsOverlay::DisplaySoundSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

DisplaySoundSettingsOverlay::~DisplaySoundSettingsOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&brightness_value_subject_);
        lv_subject_deinit(&theme_apply_disabled_subject_);
        lv_subject_deinit(&volume_value_subject_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void DisplaySoundSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Brightness value subject for label binding
    snprintf(brightness_value_buf_, sizeof(brightness_value_buf_), "100%%");
    lv_subject_init_string(&brightness_value_subject_, brightness_value_buf_, nullptr,
                           sizeof(brightness_value_buf_), brightness_value_buf_);
    lv_xml_register_subject(nullptr, "brightness_value", &brightness_value_subject_);

    // Theme Apply button disabled subject (1=disabled initially)
    lv_subject_init_int(&theme_apply_disabled_subject_, 1);
    lv_xml_register_subject(nullptr, "theme_apply_disabled", &theme_apply_disabled_subject_);

    // Volume value subject for label binding
    snprintf(volume_value_buf_, sizeof(volume_value_buf_), "80%%");
    lv_subject_init_string(&volume_value_subject_, volume_value_buf_, nullptr,
                           sizeof(volume_value_buf_), volume_value_buf_);
    lv_xml_register_subject(nullptr, "volume_value", &volume_value_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void DisplaySoundSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        // Appearance
        {"on_language_changed", on_language_changed},
        {"on_timezone_changed", on_timezone_changed},
        {"on_time_format_changed", on_time_format_changed},
        {"on_animations_changed", on_animations_changed},
        {"on_system_keyboard_changed", on_system_keyboard_changed},
        {"on_keep_navbar_changed", on_keep_navbar_changed},

        // Display
        {"on_dark_mode_changed", on_dark_mode_changed},
        {"on_brightness_changed", on_brightness_changed},
        {"on_widget_labels_changed", on_widget_labels_changed},
        {"on_page_scroll_buttons_changed", on_page_scroll_buttons_changed},
        {"on_bed_mesh_mode_changed", on_bed_mesh_mode_changed},
        {"on_dim_changed", on_dim_changed},
        {"on_sleep_changed", on_sleep_changed},
        {"on_sleep_while_printing_changed", on_sleep_while_printing_changed},
#ifdef HELIX_ENABLE_SCREENSAVER
        {"on_screensaver_changed", on_screensaver_changed},
        {"on_test_screensaver", on_test_screensaver},
#else
        {"on_screensaver_changed", [](lv_event_t*) {}},
        {"on_test_screensaver", [](lv_event_t*) {}},
#endif

        // Theme explorer
        {"on_theme_preset_changed", on_theme_preset_changed},
        {"on_theme_settings_clicked", on_theme_settings_clicked},
        {"on_preview_dark_mode_toggled", on_preview_dark_mode_toggled},
        {"on_edit_colors_clicked", on_edit_colors_clicked},
        {"on_preview_open_modal", on_preview_open_modal},
        {"on_apply_theme_clicked", on_apply_theme_clicked},

        // Sound
        {"on_sounds_changed", on_sounds_changed},
        {"on_volume_changed", on_volume_changed},
        {"on_ui_sounds_changed", on_ui_sounds_changed},
        {"on_sound_theme_changed", on_sound_theme_changed},
        {"on_audio_device_changed", on_audio_device_changed},
        {"on_preview_sounds", on_preview_sounds},
        {"on_test_tracker", on_test_tracker},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* DisplaySoundSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_display_sound_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void DisplaySoundSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void DisplaySoundSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    // Appearance
    init_language_dropdown();
    init_timezone_dropdown();
    init_time_format_dropdown();
    init_animations_toggle();

    // Display
    init_brightness_controls();
    init_dim_dropdown();
    init_sleep_dropdown();
    init_sleep_while_printing_toggle();
    init_bed_mesh_dropdown();

#ifdef HELIX_ENABLE_SCREENSAVER
    init_screensaver_dropdown();
#else
    lv_obj_t* ss_row = lv_obj_find_by_name(overlay_root_, "row_screensaver");
    if (ss_row) {
        lv_obj_add_flag(ss_row, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    // Sound
    init_sounds_toggle();
    init_volume_slider();
    init_sound_theme_dropdown();
    init_audio_device_dropdown();

#ifndef HELIX_HAS_TRACKER
    if (overlay_root_) {
        lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "container_test_tracker");
        if (container) {
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

void DisplaySoundSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// APPEARANCE INIT METHODS
// ============================================================================

void DisplaySoundSettingsOverlay::init_language_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_language");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        lv_dropdown_set_options(dropdown, SystemSettingsManager::get_language_options());
        int index = SystemSettingsManager::instance().get_language_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Language dropdown initialized (index={})", get_name(), index);
    }
}

void DisplaySoundSettingsOverlay::init_timezone_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_timezone");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        std::string options = DisplaySettingsManager::get_timezone_options();
        lv_dropdown_set_options(dropdown, options.c_str());
        int index = DisplaySettingsManager::instance().get_timezone_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Timezone dropdown initialized (index={}, tz={})", get_name(), index,
                      DisplaySettingsManager::instance().get_timezone());
    }
}

void DisplaySoundSettingsOverlay::init_time_format_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_time_format");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        auto current_format = DisplaySettingsManager::instance().get_time_format();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current_format));
        spdlog::trace("[{}] Time format dropdown initialized (format={})", get_name(),
                      static_cast<int>(current_format));
    }
}

void DisplaySoundSettingsOverlay::init_animations_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_animations");
    if (!row)
        return;

    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    if (toggle) {
        if (DisplaySettingsManager::instance().get_animations_enabled()) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        spdlog::trace("[{}] Animations toggle initialized", get_name());
    }
}

// ============================================================================
// DISPLAY INIT METHODS
// ============================================================================

void DisplaySoundSettingsOverlay::init_brightness_controls() {
    if (!overlay_root_)
        return;

    lv_obj_t* brightness_slider = lv_obj_find_by_name(overlay_root_, "brightness_slider");
    if (brightness_slider) {
        int brightness = DisplaySettingsManager::instance().get_brightness();
        lv_slider_set_value(brightness_slider, brightness, LV_ANIM_OFF);

        helix::format::format_percent(brightness, brightness_value_buf_,
                                      sizeof(brightness_value_buf_));
        lv_subject_copy_string(&brightness_value_subject_, brightness_value_buf_);

        spdlog::debug("[{}] Brightness initialized to {}%", get_name(), brightness);
    }
}

void DisplaySoundSettingsOverlay::init_dim_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* dim_row = lv_obj_find_by_name(overlay_root_, "row_display_dim");
    lv_obj_t* dim_dropdown = dim_row ? lv_obj_find_by_name(dim_row, "dropdown") : nullptr;
    if (dim_dropdown) {
        int current_sec = DisplaySettingsManager::instance().get_display_dim_sec();
        int index = DisplaySettingsManager::dim_seconds_to_index(current_sec);
        lv_dropdown_set_selected(dim_dropdown, index);

        spdlog::debug("[{}] Dim dropdown initialized to index {} ({}s)", get_name(), index,
                      current_sec);
    }
}

void DisplaySoundSettingsOverlay::init_sleep_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* sleep_row = lv_obj_find_by_name(overlay_root_, "row_display_sleep");
    lv_obj_t* sleep_dropdown = sleep_row ? lv_obj_find_by_name(sleep_row, "dropdown") : nullptr;
    if (sleep_dropdown) {
        int current_sec = DisplaySettingsManager::instance().get_display_sleep_sec();
        int index = DisplaySettingsManager::sleep_seconds_to_index(current_sec);
        lv_dropdown_set_selected(sleep_dropdown, index);

        spdlog::debug("[{}] Sleep dropdown initialized to index {} ({}s)", get_name(), index,
                      current_sec);
    }
}

void DisplaySoundSettingsOverlay::init_sleep_while_printing_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_sleep_while_printing");
    if (!row)
        return;

    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    if (toggle) {
        if (DisplaySettingsManager::instance().get_sleep_while_printing()) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        spdlog::trace("[{}] Sleep while printing toggle initialized", get_name());
    }
}

void DisplaySoundSettingsOverlay::init_bed_mesh_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* bed_mesh_row = lv_obj_find_by_name(overlay_root_, "row_bed_mesh_mode");
    lv_obj_t* bed_mesh_dropdown =
        bed_mesh_row ? lv_obj_find_by_name(bed_mesh_row, "dropdown") : nullptr;
    if (bed_mesh_dropdown) {
        int current_mode = DisplaySettingsManager::instance().get_bed_mesh_render_mode();
        lv_dropdown_set_selected(bed_mesh_dropdown, current_mode);

        spdlog::debug("[{}] Bed mesh mode dropdown initialized to {} ({})", get_name(),
                      current_mode, current_mode == 0 ? "Auto" : (current_mode == 1 ? "3D" : "2D"));
    }
}

void DisplaySoundSettingsOverlay::init_theme_preset_dropdown(lv_obj_t* root) {
    if (!root)
        return;

    lv_obj_t* theme_preset_dropdown = lv_obj_find_by_name(root, "theme_preset_dropdown");
    if (theme_preset_dropdown) {
        std::string options = DisplaySettingsManager::instance().get_theme_options();
        lv_dropdown_set_options(theme_preset_dropdown, options.c_str());

        int current_index = DisplaySettingsManager::instance().get_theme_index();
        lv_dropdown_set_selected(theme_preset_dropdown, static_cast<uint32_t>(current_index));

        spdlog::debug("[{}] Theme dropdown initialized to index {} ({})", get_name(), current_index,
                      DisplaySettingsManager::instance().get_theme_name());
    }
}

#ifdef HELIX_ENABLE_SCREENSAVER
void DisplaySoundSettingsOverlay::init_screensaver_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* ss_row = lv_obj_find_by_name(overlay_root_, "row_screensaver");
    lv_obj_t* ss_dropdown = ss_row ? lv_obj_find_by_name(ss_row, "dropdown") : nullptr;
    if (ss_dropdown) {
        int current_type = DisplaySettingsManager::instance().get_screensaver_type();
        lv_dropdown_set_selected(ss_dropdown, current_type);

        spdlog::debug("[{}] Screensaver dropdown initialized to type {}", get_name(), current_type);
    }
}
#endif

// ============================================================================
// SOUND INIT METHODS
// ============================================================================

void DisplaySoundSettingsOverlay::init_sounds_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* sounds_row = lv_obj_find_by_name(overlay_root_, "row_sounds");
    if (sounds_row) {
        lv_obj_t* toggle = lv_obj_find_by_name(sounds_row, "toggle");
        if (toggle) {
            if (AudioSettingsManager::instance().get_sounds_enabled()) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}] Sounds toggle initialized", get_name());
        }
    }
}

void DisplaySoundSettingsOverlay::init_volume_slider() {
    if (!overlay_root_)
        return;

    lv_obj_t* volume_row = lv_obj_find_by_name(overlay_root_, "row_volume");
    if (!volume_row)
        return;

    lv_obj_t* slider = lv_obj_find_by_name(volume_row, "slider");
    if (slider) {
        int volume = AudioSettingsManager::instance().get_volume();
        lv_slider_set_value(slider, volume, LV_ANIM_OFF);

        helix::format::format_percent(volume, volume_value_buf_, sizeof(volume_value_buf_));
        lv_subject_copy_string(&volume_value_subject_, volume_value_buf_);

        lv_obj_add_event_cb(slider, on_volume_released, LV_EVENT_RELEASED, nullptr);

        spdlog::debug("[{}] Volume slider initialized to {}%", get_name(), volume);
    }

    lv_obj_t* value_label = lv_obj_find_by_name(volume_row, "value_label");
    if (value_label) {
        lv_label_set_text(value_label, volume_value_buf_);
    }
}

void DisplaySoundSettingsOverlay::init_sound_theme_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* theme_row = lv_obj_find_by_name(overlay_root_, "row_sound_theme");
    if (!theme_row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(theme_row, "dropdown");
    if (dropdown) {
        auto& settings = AudioSettingsManager::instance();
        auto themes = SoundManager::instance().get_available_themes();
        std::string current_theme = settings.get_sound_theme();

        std::string options;
        int selected_index = 0;
        for (int i = 0; i < static_cast<int>(themes.size()); i++) {
            if (i > 0)
                options += "\n";
            options += themes[i];
            if (themes[i] == current_theme) {
                selected_index = i;
            }
        }

        if (!options.empty()) {
            lv_dropdown_set_options(dropdown, options.c_str());
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected_index));
        }
        spdlog::trace("[{}] Sound theme dropdown ({} themes, current={})", get_name(),
                      themes.size(), current_theme);
    }
}

void DisplaySoundSettingsOverlay::init_audio_device_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "container_audio_device");
    if (!container)
        return;

    // Only meaningful with the runtime-selectable ALSA backend. Hide the whole
    // row otherwise (matches the screensaver/tracker capability-gating in this file).
    if (!SoundManager::instance().has_alsa_backend()) {
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_t* row = lv_obj_find_by_name(container, "row_audio_device");
    lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    if (!dropdown)
        return;

    auto devices = helix::audio::list();
    std::string current = helix::audio::resolve_alsa_device();

    std::string options;
    int selected_index = 0;
    for (int i = 0; i < static_cast<int>(devices.size()); i++) {
        if (i > 0)
            options += "\n";
        options += devices[i].label;
        if (devices[i].pcm == current) {
            selected_index = i;
        }
    }
    if (!options.empty()) {
        lv_dropdown_set_options(dropdown, options.c_str());
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected_index));
    }

    // Env lock: HELIX_ALSA_DEVICE wins, so the picker can't change anything.
    if (const char* e = std::getenv("HELIX_ALSA_DEVICE"); e && e[0] != '\0') {
        lv_obj_add_state(dropdown, LV_STATE_DISABLED);
        spdlog::info("[{}] Output device picker disabled (HELIX_ALSA_DEVICE override)", get_name());
    }
}

void DisplaySoundSettingsOverlay::handle_audio_device_changed(int index) {
    auto devices = helix::audio::list();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        spdlog::warn("[{}] Output device index {} out of range ({})", get_name(), index,
                     devices.size());
        return;
    }
    const std::string& pcm = devices[index].pcm;
    spdlog::info("[{}] Output device changed: {} ({})", get_name(), devices[index].label, pcm);

    if (!SoundManager::instance().set_output_device(pcm)) {
        // Device failed (or fell back) — re-sync the dropdown to what's actually open.
        spdlog::warn("[{}] Output device '{}' did not apply; re-syncing dropdown", get_name(), pcm);
        init_audio_device_dropdown();
    }
}

void DisplaySoundSettingsOverlay::on_audio_device_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_audio_device_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_audio_device_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// APPEARANCE EVENT HANDLERS
// ============================================================================

void DisplaySoundSettingsOverlay::handle_language_changed(int index) {
    std::string lang_code = SystemSettingsManager::language_index_to_code(index);
    spdlog::info("[{}] Language changed: index {} ({})", get_name(), index, lang_code);
    SystemSettingsManager::instance().set_language_by_index(index);
}

void DisplaySoundSettingsOverlay::handle_timezone_changed(int index) {
    spdlog::info("[{}] Timezone changed to index {}", get_name(), index);
    DisplaySettingsManager::instance().set_timezone_by_index(index);
}

void DisplaySoundSettingsOverlay::handle_time_format_changed(int index) {
    auto format = static_cast<TimeFormat>(index);
    spdlog::info("[{}] Time format changed: {} ({})", get_name(), index,
                 index == 0 ? "12 Hour" : "24 Hour");
    DisplaySettingsManager::instance().set_time_format(format);
}

void DisplaySoundSettingsOverlay::handle_animations_changed(bool enabled) {
    spdlog::info("[{}] Animations toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_animations_enabled(enabled);
}

void DisplaySoundSettingsOverlay::handle_system_keyboard_changed(bool enabled) {
    spdlog::info("[{}] System keyboard toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_use_system_keyboard(enabled);
}

void DisplaySoundSettingsOverlay::handle_keep_navbar_changed(bool enabled) {
    spdlog::info("[{}] Keep navbar toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_keep_navbar_visible(enabled);
}

// ============================================================================
// DISPLAY EVENT HANDLERS
// ============================================================================

void DisplaySoundSettingsOverlay::handle_dark_mode_changed(bool enabled) {
    spdlog::info("[{}] Dark mode toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_dark_mode(enabled);
    theme_manager_apply_theme(theme_manager_get_active_theme(), enabled);
}

void DisplaySoundSettingsOverlay::handle_brightness_changed(int value) {
    DisplaySettingsManager::instance().set_brightness(value);

    helix::format::format_percent(value, brightness_value_buf_, sizeof(brightness_value_buf_));
    lv_subject_copy_string(&brightness_value_subject_, brightness_value_buf_);
}

void DisplaySoundSettingsOverlay::handle_widget_labels_changed(bool enabled) {
    spdlog::info("[{}] Widget labels toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_show_widget_labels(enabled);
}

void DisplaySoundSettingsOverlay::handle_page_scroll_buttons_changed(bool enabled) {
    spdlog::info("[{}] Page scroll buttons toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_page_scroll_buttons(enabled);
    // Apply immediately to the current screen — this callback is the authoritative
    // user-toggle signal (a subject observer can't be used; see PageScrollAutoInject::init).
    helix::ui::PageScrollAutoInject::instance().on_setting_toggled(enabled);
}

void DisplaySoundSettingsOverlay::handle_bed_mesh_mode_changed(int mode) {
    spdlog::info("[{}] Bed mesh render mode changed: {} ({})", get_name(), mode,
                 mode == 0 ? "Auto" : (mode == 1 ? "3D" : "2D"));
    DisplaySettingsManager::instance().set_bed_mesh_render_mode(mode);
}

void DisplaySoundSettingsOverlay::handle_dim_changed(int index) {
    int seconds = DisplaySettingsManager::index_to_dim_seconds(index);
    spdlog::info("[{}] Display dim changed: index {} = {}s", get_name(), index, seconds);
    DisplaySettingsManager::instance().set_display_dim_sec(seconds);
}

void DisplaySoundSettingsOverlay::handle_sleep_changed(int index) {
    int seconds = DisplaySettingsManager::index_to_sleep_seconds(index);
    spdlog::info("[{}] Display sleep changed: index {} = {}s", get_name(), index, seconds);
    DisplaySettingsManager::instance().set_display_sleep_sec(seconds);
}

void DisplaySoundSettingsOverlay::handle_sleep_while_printing_changed(bool enabled) {
    spdlog::info("[{}] Sleep while printing toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_sleep_while_printing(enabled);
}

void DisplaySoundSettingsOverlay::handle_theme_preset_changed(int index) {
    if (theme_explorer_overlay_ && lv_obj_is_visible(theme_explorer_overlay_)) {
        handle_explorer_theme_changed(index);
        return;
    }

    DisplaySettingsManager::instance().set_theme_by_index(index);

    spdlog::info("[{}] Theme changed to index {} ({})", get_name(), index,
                 DisplaySettingsManager::instance().get_theme_name());
}

void DisplaySoundSettingsOverlay::handle_explorer_theme_changed(int index) {
    if (index < 0 || index >= static_cast<int>(cached_themes_.size())) {
        spdlog::error("[{}] Invalid theme index {}", get_name(), index);
        return;
    }

    std::string theme_name = cached_themes_[index].filename;
    helix::ThemeData theme = helix::load_theme_from_file(theme_name);

    if (!theme.is_valid()) {
        spdlog::error("[{}] Failed to load theme '{}' for preview", get_name(), theme_name);
        return;
    }

    preview_theme_name_ = theme_name;

    bool supports_dark = theme.supports_dark();
    bool supports_light = theme.supports_light();

    if (theme_explorer_overlay_) {
        lv_obj_t* dark_toggle =
            lv_obj_find_by_name(theme_explorer_overlay_, "preview_dark_mode_toggle");
        lv_obj_t* toggle_container =
            lv_obj_find_by_name(theme_explorer_overlay_, "dark_mode_toggle_container");

        if (dark_toggle) {
            if (supports_dark && supports_light) {
                lv_obj_remove_state(dark_toggle, LV_STATE_DISABLED);
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' supports both modes, toggle enabled", get_name(),
                              theme_name);
            } else if (supports_dark) {
                lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
                lv_obj_add_state(dark_toggle, LV_STATE_CHECKED);
                preview_is_dark_ = true;
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' is dark-only, forcing dark mode", get_name(),
                              theme_name);
            } else if (supports_light) {
                lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
                lv_obj_remove_state(dark_toggle, LV_STATE_CHECKED);
                preview_is_dark_ = false;
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' is light-only, forcing light mode", get_name(),
                              theme_name);
            }
        }
    }

    theme_manager_preview(theme);

    lv_subject_set_int(&theme_apply_disabled_subject_, index == original_theme_index_ ? 1 : 0);

    handle_preview_dark_mode_toggled(preview_is_dark_);

    spdlog::debug("[{}] Explorer preview: theme '{}' (index {})", get_name(), theme_name, index);
}

void DisplaySoundSettingsOverlay::handle_theme_settings_clicked() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Theme settings clicked without parent screen", get_name());
        return;
    }

    if (!theme_explorer_overlay_) {
        spdlog::debug("[{}] Creating theme explorer overlay...", get_name());
        theme_explorer_overlay_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "theme_preview_overlay", nullptr));
        if (!theme_explorer_overlay_) {
            spdlog::error("[{}] Failed to create theme explorer overlay", get_name());
            return;
        }

        lv_obj_add_flag(theme_explorer_overlay_, LV_OBJ_FLAG_HIDDEN);

        NavigationManager::instance().register_overlay_instance(theme_explorer_overlay_, nullptr);
        NavigationManager::instance().register_overlay_close_callback(
            theme_explorer_overlay_, [this]() {
                theme_manager_apply_theme(original_theme_, theme_manager_is_dark_mode());
                if (theme_explorer_overlay_) {
                    helix::ui::defocus_tree(theme_explorer_overlay_);
                    lv_obj_add_flag(theme_explorer_overlay_, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_t* to_delete = theme_explorer_overlay_;
                    theme_explorer_overlay_ = nullptr;
                    lv_obj_delete_async(to_delete);
                }
                cached_themes_.clear();
            });
    }

    init_theme_preset_dropdown(theme_explorer_overlay_);

    cached_themes_ = helix::discover_themes(helix::get_themes_directory());

    original_theme_index_ = DisplaySettingsManager::instance().get_theme_index();
    preview_theme_name_ = DisplaySettingsManager::instance().get_theme_name();
    original_theme_ = theme_manager_get_active_theme();

    preview_is_dark_ = theme_manager_is_dark_mode();
    lv_obj_t* dark_toggle =
        lv_obj_find_by_name(theme_explorer_overlay_, "preview_dark_mode_toggle");
    if (dark_toggle) {
        if (preview_is_dark_) {
            lv_obj_add_state(dark_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(dark_toggle, LV_STATE_CHECKED);
        }

        bool supports_dark = theme_manager_supports_dark_mode();
        bool supports_light = theme_manager_supports_light_mode();
        if (supports_dark && supports_light) {
            lv_obj_remove_state(dark_toggle, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
        }
    }

    lv_subject_set_int(&theme_apply_disabled_subject_, 1);

    NavigationManager::instance().push_overlay(theme_explorer_overlay_);
}

void DisplaySoundSettingsOverlay::handle_apply_theme_clicked() {
    lv_obj_t* dropdown = theme_explorer_overlay_
                             ? lv_obj_find_by_name(theme_explorer_overlay_, "theme_preset_dropdown")
                             : nullptr;
    if (!dropdown) {
        spdlog::warn("[{}] Apply clicked but dropdown not found", get_name());
        return;
    }

    int selected_index = lv_dropdown_get_selected(dropdown);

    DisplaySettingsManager::instance().set_theme_by_index(selected_index);
    std::string theme_name = DisplaySettingsManager::instance().get_theme_name();

    theme_manager_apply_theme(theme_manager_get_active_theme(), theme_manager_is_dark_mode());

    original_theme_index_ = selected_index;
    original_theme_ = theme_manager_get_active_theme();

    spdlog::info("[{}] Theme '{}' applied (index {})", get_name(), theme_name, selected_index);

    std::string display_name = theme_name;
    if (selected_index >= 0 && selected_index < static_cast<int>(cached_themes_.size())) {
        display_name = cached_themes_[selected_index].display_name;
    }
    std::string toast_msg = "Theme set to " + display_name;
    ToastManager::instance().show(ToastSeverity::SUCCESS, toast_msg.c_str());

    NavigationManager::instance().go_back();
}

void DisplaySoundSettingsOverlay::handle_edit_colors_clicked() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Theme settings clicked without parent screen", get_name());
        return;
    }

    if (!theme_settings_overlay_) {
        spdlog::debug("[{}] Creating theme editor overlay...", get_name());
        auto& overlay = get_theme_editor_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        theme_settings_overlay_ = overlay.create(parent_screen_);
        if (!theme_settings_overlay_) {
            spdlog::error("[{}] Failed to create theme editor overlay", get_name());
            return;
        }

        NavigationManager::instance().register_overlay_instance(theme_settings_overlay_, &overlay);
    }

    if (theme_settings_overlay_) {
        std::string theme_name = !preview_theme_name_.empty()
                                     ? preview_theme_name_
                                     : DisplaySettingsManager::instance().get_theme_name();
        get_theme_editor_overlay().set_editing_dark_mode(preview_is_dark_);
        get_theme_editor_overlay().load_theme(theme_name);
        NavigationManager::instance().push_overlay(theme_settings_overlay_);
    }
}

void DisplaySoundSettingsOverlay::handle_preview_dark_mode_toggled(bool is_dark) {
    preview_is_dark_ = is_dark;

    if (!theme_explorer_overlay_) {
        return;
    }

    lv_obj_t* dropdown = lv_obj_find_by_name(theme_explorer_overlay_, "theme_preset_dropdown");
    if (!dropdown) {
        return;
    }

    int selected_index = lv_dropdown_get_selected(dropdown);
    if (selected_index < 0 || selected_index >= static_cast<int>(cached_themes_.size())) {
        return;
    }

    helix::ThemeData theme = helix::load_theme_from_file(cached_themes_[selected_index].filename);

    if (!theme.is_valid()) {
        return;
    }

    theme_manager_preview(theme, is_dark);

    spdlog::debug("[{}] Preview dark mode toggled to {}", get_name(), is_dark ? "dark" : "light");
}

void DisplaySoundSettingsOverlay::apply_preview_palette_to_screen_popups() {
    if (!theme_explorer_overlay_ || cached_themes_.empty()) {
        return;
    }

    lv_obj_t* dropdown = lv_obj_find_by_name(theme_explorer_overlay_, "theme_preset_dropdown");
    if (!dropdown) {
        return;
    }

    uint32_t selected_index = lv_dropdown_get_selected(dropdown);
    if (selected_index >= cached_themes_.size()) {
        return;
    }

    helix::ThemeData theme = helix::load_theme_from_file(cached_themes_[selected_index].filename);
    if (!theme.is_valid()) {
        return;
    }

    const helix::ModePalette* palette = nullptr;
    if (preview_is_dark_ && theme.supports_dark()) {
        palette = &theme.dark;
    } else if (!preview_is_dark_ && theme.supports_light()) {
        palette = &theme.light;
    } else {
        palette = theme.supports_dark() ? &theme.dark : &theme.light;
    }

    theme_apply_palette_to_screen_dropdowns(*palette);

    lv_obj_t* modal_dialog = lv_obj_find_by_name(lv_screen_active(), "modal_dialog");
    if (modal_dialog) {
        const char* suffix = theme_manager_get_breakpoint_suffix(responsive_dimension(nullptr));
        int radius_px =
            helix::BorderRadiusSizes::pixels(theme.properties.border_radius_size, suffix);
        lv_obj_set_style_radius(modal_dialog, radius_px, LV_PART_MAIN);
    }
}

void DisplaySoundSettingsOverlay::show_theme_preview(lv_obj_t* parent_screen) {
    parent_screen_ = parent_screen;
    register_callbacks();
    handle_theme_settings_clicked();
}

// ============================================================================
// SOUND EVENT HANDLERS
// ============================================================================

void DisplaySoundSettingsOverlay::handle_sounds_changed(bool enabled) {
    spdlog::info("[{}] Sounds toggled: {}", get_name(), enabled ? "ON" : "OFF");
    AudioSettingsManager::instance().set_sounds_enabled(enabled);

    if (enabled) {
        SoundManager::instance().play_test_beep();
    }
}

void DisplaySoundSettingsOverlay::handle_volume_changed(int value) {
    AudioSettingsManager::instance().set_volume(value);

    helix::format::format_percent(value, volume_value_buf_, sizeof(volume_value_buf_));
    lv_subject_copy_string(&volume_value_subject_, volume_value_buf_);

    if (overlay_root_) {
        lv_obj_t* volume_row = lv_obj_find_by_name(overlay_root_, "row_volume");
        if (volume_row) {
            lv_obj_t* value_label = lv_obj_find_by_name(volume_row, "value_label");
            if (value_label) {
                lv_label_set_text(value_label, volume_value_buf_);
            }
        }
    }
}

void DisplaySoundSettingsOverlay::handle_ui_sounds_changed(bool enabled) {
    spdlog::info("[{}] UI Sounds toggled: {}", get_name(), enabled ? "ON" : "OFF");
    AudioSettingsManager::instance().set_ui_sounds_enabled(enabled);
}

void DisplaySoundSettingsOverlay::handle_sound_theme_changed(int index) {
    auto themes = SoundManager::instance().get_available_themes();
    if (index >= 0 && index < static_cast<int>(themes.size())) {
        const auto& theme_name = themes[index];
        spdlog::info("[{}] Sound theme changed: {} (index {})", get_name(), theme_name, index);
        AudioSettingsManager::instance().set_sound_theme(theme_name);
        SoundManager::instance().set_theme(theme_name);
        SoundManager::instance().play("test_beep");
    } else {
        spdlog::warn("[{}] Sound theme index {} out of range ({})", get_name(), index,
                     themes.size());
    }
}

void DisplaySoundSettingsOverlay::handle_preview_sounds() {
    spdlog::info("[{}] Opening sound preview", get_name());
    get_sound_preview_overlay().show(parent_screen_);
}

void DisplaySoundSettingsOverlay::handle_test_tracker() {
#ifdef HELIX_HAS_TRACKER
    auto& sm = SoundManager::instance();
    if (sm.is_tracker_playing()) {
        spdlog::info("[{}] Stopping tracker playback", get_name());
        sm.stop_tracker();
    } else {
        spdlog::info("[{}] Starting tracker playback: crocketts_theme.mod", get_name());
        sm.play_file("assets/sounds/crocketts_theme.mod");
    }
#else
    spdlog::warn("[{}] Tracker playback not available (HELIX_HAS_TRACKER not defined)", get_name());
#endif
}

// ============================================================================
// STATIC CALLBACKS - APPEARANCE
// ============================================================================

void DisplaySoundSettingsOverlay::on_language_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_language_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_language_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_timezone_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_timezone_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_timezone_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_time_format_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_time_format_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_time_format_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_animations_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_animations_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_animations_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_system_keyboard_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_system_keyboard_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_system_keyboard_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_keep_navbar_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_keep_navbar_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_keep_navbar_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// STATIC CALLBACKS - DISPLAY
// ============================================================================

void DisplaySoundSettingsOverlay::on_dark_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_dark_mode_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_dark_mode_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_brightness_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_brightness_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_display_sound_settings_overlay().handle_brightness_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_widget_labels_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_widget_labels_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_widget_labels_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_page_scroll_buttons_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_page_scroll_buttons_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_page_scroll_buttons_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_bed_mesh_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_bed_mesh_mode_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int mode = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_bed_mesh_mode_changed(mode);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_dim_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_dim_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_dim_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_sleep_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_sleep_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_sleep_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_sleep_while_printing_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_sleep_while_printing_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_sleep_while_printing_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_theme_preset_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_theme_preset_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = lv_dropdown_get_selected(dropdown);
    get_display_sound_settings_overlay().handle_theme_preset_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_theme_settings_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_theme_settings_clicked");
    LV_UNUSED(e);
    get_display_sound_settings_overlay().handle_theme_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_apply_theme_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_apply_theme_clicked");
    LV_UNUSED(e);
    get_display_sound_settings_overlay().handle_apply_theme_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_edit_colors_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_edit_colors_clicked");
    LV_UNUSED(e);
    get_display_sound_settings_overlay().handle_edit_colors_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_preview_dark_mode_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_preview_dark_mode_toggled");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool is_dark = lv_obj_has_state(target, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_preview_dark_mode_toggled(is_dark);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_preview_open_modal(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_preview_open_modal");
    LV_UNUSED(e);

    helix::ui::modal_show_confirmation(
        lv_tr("Sample Dialog"),
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod "
        "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
        "veniam, quis nostrud exercitation ullamco laboris.",
        ModalSeverity::Info, "OK", nullptr, nullptr, nullptr); // i18n: universal

    auto& overlay = get_display_sound_settings_overlay();
    overlay.apply_preview_palette_to_screen_popups();

    LVGL_SAFE_EVENT_CB_END();
}

#ifdef HELIX_ENABLE_SCREENSAVER
void DisplaySoundSettingsOverlay::on_screensaver_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_screensaver_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = lv_dropdown_get_selected(dropdown);
    spdlog::info("[DisplaySoundSettingsOverlay] Screensaver changed to type {}", index);
    DisplaySettingsManager::instance().set_screensaver_type(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_test_screensaver(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_test_screensaver");
    get_display_sound_settings_overlay().handle_test_screensaver();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::handle_test_screensaver() {
    int type = DisplaySettingsManager::instance().get_screensaver_type();
    if (type <= 0) {
        return; // "Off" — button should have been hidden, but guard anyway
    }
    auto* dm = DisplayManager::instance();
    if (!dm) {
        spdlog::warn("[{}] DisplayManager not available, cannot preview screensaver", get_name());
        return;
    }
    spdlog::info("[{}] User-initiated screensaver preview (type {})", get_name(), type);
    dm->preview_screensaver(type);
}
#endif

// ============================================================================
// STATIC CALLBACKS - SOUND
// ============================================================================

void DisplaySoundSettingsOverlay::on_sounds_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_sounds_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_sounds_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_volume_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_volume_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_display_sound_settings_overlay().handle_volume_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_volume_released(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_volume_released");
    SoundManager::instance().play_test_beep();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_ui_sounds_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_ui_sounds_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_sound_settings_overlay().handle_ui_sounds_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_sound_theme_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_sound_theme_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_sound_settings_overlay().handle_sound_theme_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_preview_sounds(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_preview_sounds");
    get_display_sound_settings_overlay().handle_preview_sounds();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySoundSettingsOverlay::on_test_tracker(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySoundSettingsOverlay] on_test_tracker");
    get_display_sound_settings_overlay().handle_test_tracker();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
