// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_sound.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_sound_preview_overlay.h"

#include "alsa_device_enum.h"
#include "audio_settings_manager.h"
#include "format_utils.h"
#include "sound_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;
using helix::ui::find_required;

namespace {

void on_volume_released(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_volume_released");
    SoundManager::instance().play_test_beep();
    LVGL_SAFE_EVENT_CB_END();
}

int event_slider_value(lv_event_t* e) {
    return lv_slider_get_value(lv_event_get_current_target_obj(e));
}

} // namespace

void SoundSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_sounds_changed",
         [](lv_event_t* e) {
             get_sound_settings_overlay().handle_sounds_changed(event_checked(e));
         }},
        {"on_volume_changed",
         [](lv_event_t* e) {
             get_sound_settings_overlay().handle_volume_changed(event_slider_value(e));
         }},
        {"on_volume_commit",
         [](lv_event_t* e) { AudioSettingsManager::instance().set_volume(event_slider_value(e)); }},
        {"on_ui_sounds_changed",
         [](lv_event_t* e) {
             AudioSettingsManager::instance().set_ui_sounds_enabled(event_checked(e));
         }},
        {"on_sound_theme_changed",
         [](lv_event_t* e) {
             get_sound_settings_overlay().handle_sound_theme_changed(event_selected(e));
         }},
        {"on_audio_device_changed",
         [](lv_event_t* e) {
             get_sound_settings_overlay().handle_audio_device_changed(event_selected(e));
         }},
        {"on_preview_sounds",
         [](lv_event_t*) { get_sound_settings_overlay().handle_preview_sounds(); }},
        {"on_test_tracker",
         [](lv_event_t*) { get_sound_settings_overlay().handle_test_tracker(); }},
    });
}

void SoundSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_volume_slider();
    init_sound_theme_dropdown();
    init_audio_device_dropdown();

#ifndef HELIX_HAS_TRACKER
    if (lv_obj_t* container = find_required(overlay_root_, "container_test_tracker", get_name())) {
        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

// ============================================================================
// INIT METHODS
// ============================================================================

void SoundSettingsOverlay::init_volume_slider() {
    lv_obj_t* row = find_required(overlay_root_, "row_volume", get_name());
    if (lv_obj_t* slider = find_required(row, "slider", get_name())) {
        int volume = AudioSettingsManager::instance().get_volume();
        lv_slider_set_value(slider, volume, LV_ANIM_OFF);
        helix::format::format_percent(volume, volume_value_buf_, sizeof(volume_value_buf_));
        lv_obj_add_event_cb(slider, on_volume_released, LV_EVENT_RELEASED, nullptr);
    }
    if (lv_obj_t* label = find_required(row, "value_label", get_name())) {
        lv_label_set_text(label, volume_value_buf_);
    }
}

void SoundSettingsOverlay::init_sound_theme_dropdown() {
    lv_obj_t* row = find_required(overlay_root_, "row_sound_theme", get_name());
    lv_obj_t* dropdown = find_required(row, "dropdown", get_name());
    if (!dropdown) {
        return;
    }
    auto themes = SoundManager::instance().get_available_themes();
    std::string current_theme = AudioSettingsManager::instance().get_sound_theme();

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
}

void SoundSettingsOverlay::init_audio_device_dropdown() {
    // Visibility is driven declaratively by the compound cond on
    // container_audio_device (sounds_enabled OR audio_device_available). On
    // non-ALSA backends the row is hidden by the binding, so there is nothing
    // to populate.
    if (!SoundManager::instance().has_alsa_backend()) {
        return;
    }

    lv_obj_t* container = find_required(overlay_root_, "container_audio_device", get_name());
    lv_obj_t* row = find_required(container, "row_audio_device", get_name());
    lv_obj_t* dropdown = find_required(row, "dropdown", get_name());
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

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SoundSettingsOverlay::handle_audio_device_changed(int index) {
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

void SoundSettingsOverlay::handle_sounds_changed(bool enabled) {
    AudioSettingsManager::instance().set_sounds_enabled(enabled);
    if (enabled) {
        SoundManager::instance().play_test_beep();
    }
}

void SoundSettingsOverlay::handle_volume_changed(int value) {
    // Per drag tick: subject + readout only. The commit callback persists.
    AudioSettingsManager::instance().preview_volume(value);
    helix::format::format_percent(value, volume_value_buf_, sizeof(volume_value_buf_));

    lv_obj_t* row = find_required(overlay_root_, "row_volume", get_name());
    if (lv_obj_t* label = find_required(row, "value_label", get_name())) {
        lv_label_set_text(label, volume_value_buf_);
    }
}

void SoundSettingsOverlay::handle_sound_theme_changed(int index) {
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

void SoundSettingsOverlay::handle_preview_sounds() {
    get_sound_preview_overlay().show(parent_screen_);
}

void SoundSettingsOverlay::handle_test_tracker() {
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

} // namespace helix::settings
