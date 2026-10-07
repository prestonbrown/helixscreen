// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_touch.h"

#include "ui_callback_helpers.h"
#include "ui_panel_settings.h"
#include "ui_touch_calibration_overlay.h"

#include "config.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "input_settings_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "page_scroll_auto_inject.h"
#include "runtime_config.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

namespace {

// The slider rows nest as: row > slider_container > slider, so the row is the
// slider's grandparent.
void sync_slider_value_label(lv_obj_t* slider, int value) {
    lv_obj_t* row = lv_obj_get_parent(lv_obj_get_parent(slider));
    if (!row)
        return;
    if (lv_obj_t* value_label = helix::ui::find_required(row, "value_label", "ui_settings_touch")) {
        lv_label_set_text_fmt(value_label, "%d", value);
    }
}

bool touch_calibration_applies() {
#ifdef HELIX_DISPLAY_SDL
    return get_runtime_config()->is_test_mode();
#else
    // supports_ (any real touch panel), NOT needs_ (auto-fire the first-run
    // wizard). The auto-fire heuristic keys off controller name and ABS range,
    // and neither can see a touch panel mounted 90° from the display, so gating
    // the manual entry point on it left those users with no way in
    // (prestonbrown/helixscreen#1259).
    DisplayManager* dm = DisplayManager::instance();
    return dm && dm->supports_touch_calibration();
#endif
}

} // namespace

TouchSettingsOverlay::~TouchSettingsOverlay() {
    deinit_subjects();
}

void TouchSettingsOverlay::deinit_subjects() {
    deinit_subjects_base(subjects_);
}

void TouchSettingsOverlay::init_subjects() {
    init_subjects_guarded([this]() {
        UI_MANAGED_SUBJECT_INT(show_touch_calibration_subject_, touch_calibration_applies() ? 1 : 0,
                               "show_touch_calibration", subjects_);
        UI_MANAGED_SUBJECT_STRING(touch_cal_status_subject_, touch_cal_status_buf_, "",
                                  "touch_cal_status", subjects_);
    });
}

void TouchSettingsOverlay::register_callbacks() {
    using helix::ui::event_checked;
    using helix::ui::event_selected;
    register_xml_callbacks({
        {"on_touch_calibration_clicked",
         [](lv_event_t*) { get_touch_settings_overlay().handle_touch_calibration_clicked(); }},
        {"on_debug_touches_changed",
         [](lv_event_t* e) {
             InputSettingsManager::instance().set_debug_touches(event_checked(e));
         }},
        {"on_scroll_limit_changed",
         [](lv_event_t* e) {
             lv_obj_t* slider = lv_event_get_current_target_obj(e);
             int value = static_cast<int>(lv_slider_get_value(slider));
             sync_slider_value_label(slider, value);
             // set_scroll_limit live-applies, so no restart prompt.
             InputSettingsManager::instance().set_scroll_limit(value);
         }},
        {"on_long_press_time_changed",
         [](lv_event_t* e) {
             lv_obj_t* slider = lv_event_get_current_target_obj(e);
             int value = static_cast<int>(lv_slider_get_value(slider));
             sync_slider_value_label(slider, value);
             // set_long_press_time live-applies, so no restart prompt.
             InputSettingsManager::instance().set_long_press_time(value);
         }},
        {"on_home_edit_mode_changed",
         [](lv_event_t* e) {
             // should_suppress_edit_mode checks this live, so no restart prompt.
             InputSettingsManager::instance().set_home_edit_mode_enabled(event_checked(e));
         }},
        {"on_keypad_layout_changed",
         [](lv_event_t* e) {
             // The keypad XML binds settings_keypad_layout, so no restart prompt.
             InputSettingsManager::instance().set_keypad_layout(
                 static_cast<KeypadLayout>(event_selected(e)));
         }},
        {"on_scroll_guard_changed",
         [](lv_event_t* e) {
             InputSettingsManager::instance().set_scroll_guard(event_checked(e));
             get_global_settings_panel().show_restart_prompt();
         }},
        {"on_system_keyboard_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_use_system_keyboard(event_checked(e));
         }},
        {"on_hide_keyboard_with_hardware_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_hide_keyboard_with_hardware(event_checked(e));
         }},
        {"on_keep_navbar_changed",
         [](lv_event_t* e) {
             DisplaySettingsManager::instance().set_keep_navbar_visible(event_checked(e));
         }},
        {"on_page_scroll_buttons_changed",
         [](lv_event_t* e) {
             bool on = event_checked(e);
             DisplaySettingsManager::instance().set_page_scroll_buttons(on);
             // The callback is the authoritative user-toggle signal; a subject observer
             // cannot be used (see PageScrollAutoInject::init).
             helix::ui::PageScrollAutoInject::instance().on_setting_toggled(on);
         }},
    });
}

void TouchSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    refresh_calibration_status();
    init_input_sliders();
}

// Formatted on every open, so it is in the language of the latest visit.
void TouchSettingsOverlay::refresh_calibration_status() {
    Config* config = Config::get_instance();
    show_calibration_status(config->get<bool>(config->df() + "input/calibration/valid", false));
}

void TouchSettingsOverlay::show_calibration_status(bool calibrated) {
    if (!subjects_initialized_) {
        return;
    }
    lv_subject_copy_string(&touch_cal_status_subject_,
                           calibrated ? lv_tr("Calibrated") : lv_tr("Not calibrated"));
}

void TouchSettingsOverlay::handle_touch_calibration_clicked() {
    DisplayManager* dm = DisplayManager::instance();
    if (dm && !dm->supports_touch_calibration()) {
        spdlog::debug("[{}] No calibratable touch device", get_name());
        return;
    }

    spdlog::debug("[{}] Touch Calibration clicked", get_name());

    helix::ui::get_touch_calibration_overlay().show(parent_screen_, [this](bool success) {
        if (success) {
            show_calibration_status(true);
            spdlog::info("[{}] Touch calibration completed - updated status", get_name());
        }
    });
}

// Sliders capture their value at XML construction from the static `value`
// prop, so push the persisted value on every activate. Toggle rows bind to
// subjects directly and need no manual sync.
void TouchSettingsOverlay::init_input_sliders() {
    if (!overlay_root_) {
        return;
    }

    auto& input = helix::InputSettingsManager::instance();

    auto sync_slider = [this](const char* row_name, int value) {
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, row_name);
        if (!row) {
            return;
        }
        if (lv_obj_t* slider = helix::ui::find_required(row, "slider", get_name())) {
            lv_slider_set_value(slider, value, LV_ANIM_OFF);
            sync_slider_value_label(slider, value);
        }
    };

    sync_slider("row_scroll_limit", input.get_scroll_limit());
    sync_slider("row_long_press_time", input.get_long_press_time());
}

} // namespace helix::settings
