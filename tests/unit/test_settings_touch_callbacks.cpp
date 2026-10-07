// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Touch & Input page's rows act on the setting they show. The XML names its
// callbacks by string and silently skips one that is not registered, so a row
// whose callback went missing stays on screen and does nothing. These tests
// build the page the way the overlay does (callbacks registered, then the XML)
// and drive each row.

#include "ui_modal.h"
#include "ui_panel_settings.h"
#include "ui_settings_touch.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "input_settings_manager.h"
#include "input_settings_test_helpers.h"
#include "settings_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct TouchCallbacksFixture : LVGLUITestFixture {
    lv_obj_t* root_ = nullptr;

    TouchCallbacksFixture() {
        SettingsManager::instance().init_subjects();
        helix_test::reset_input_settings_to_defaults();
        get_global_settings_panel().init_subjects();
        helix::settings::get_touch_settings_overlay().init_subjects();
        helix::settings::get_touch_settings_overlay().register_callbacks();
        root_ =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "settings_touch_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~TouchCallbacksFixture() override {
        dismiss_restart_prompt();
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        helix::settings::get_touch_settings_overlay().deinit_subjects();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* part(const char* row, const char* name) {
        lv_obj_t* r = lv_obj_find_by_name(root_, row);
        REQUIRE(r != nullptr);
        lv_obj_t* p = lv_obj_find_by_name(r, name);
        REQUIRE(p != nullptr);
        return p;
    }

    void set_toggle(const char* row, bool on) {
        lv_obj_t* toggle = part(row, "toggle");
        if (on) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Drag a slider row to @p value and release it, which is when it commits.
    void release_slider(const char* row, int value) {
        lv_obj_t* slider = part(row, "slider");
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }

    std::string slider_label(const char* row) {
        return lv_label_get_text(part(row, "value_label"));
    }

    bool restart_prompt_up() {
        return ModalStack::instance().top_dialog() != nullptr;
    }

    /// Answer the restart prompt with "Later" so the panel's handle is released.
    void dismiss_restart_prompt() {
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        if (!dialog) {
            return;
        }
        if (lv_obj_t* later = lv_obj_find_by_name(dialog, "btn_secondary")) {
            lv_obj_send_event(later, LV_EVENT_CLICKED, nullptr);
        }
        helix::ui::UpdateQueue::instance().drain();
        ModalStack::instance().clear();
    }
};

} // namespace

TEST_CASE_METHOD(TouchCallbacksFixture, "Touch page: the debug-touches toggle drives the setting",
                 "[settings][touch_callbacks]") {
    REQUIRE_FALSE(helix::InputSettingsManager::instance().get_debug_touches());

    set_toggle("row_debug_touches", true);
    CHECK(helix::InputSettingsManager::instance().get_debug_touches());

    set_toggle("row_debug_touches", false);
    CHECK_FALSE(helix::InputSettingsManager::instance().get_debug_touches());
}

TEST_CASE_METHOD(TouchCallbacksFixture, "Touch page: the home-edit toggle drives the setting",
                 "[settings][touch_callbacks]") {
    set_toggle("row_home_edit_mode", false);
    CHECK_FALSE(helix::InputSettingsManager::instance().get_home_edit_mode_enabled());
    CHECK_FALSE(restart_prompt_up());

    set_toggle("row_home_edit_mode", true);
    CHECK(helix::InputSettingsManager::instance().get_home_edit_mode_enabled());
}

TEST_CASE_METHOD(TouchCallbacksFixture, "Touch page: the number pad dropdown drives the setting",
                 "[settings][touch_callbacks][keypad_layout]") {
    lv_obj_t* dropdown = part("row_keypad_layout", "dropdown");
    REQUIRE(lv_dropdown_get_selected(dropdown) ==
            static_cast<uint32_t>(helix::KeypadLayout::PHONE));

    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(helix::KeypadLayout::CALCULATOR));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(helix::InputSettingsManager::instance().get_keypad_layout() ==
          helix::KeypadLayout::CALCULATOR);
    CHECK_FALSE(restart_prompt_up());
}

TEST_CASE_METHOD(TouchCallbacksFixture, "Touch page: the scroll guard toggle asks for a restart",
                 "[settings][touch_callbacks]") {
    const bool before = helix::InputSettingsManager::instance().get_scroll_guard();

    set_toggle("row_scroll_guard", !before);

    CHECK(helix::InputSettingsManager::instance().get_scroll_guard() == !before);
    CHECK(restart_prompt_up());
}

TEST_CASE_METHOD(TouchCallbacksFixture,
                 "Touch page: the scroll-engage slider applies live without a restart",
                 "[settings][touch_callbacks]") {
    release_slider("row_scroll_limit", 15);

    CHECK(helix::InputSettingsManager::instance().get_scroll_limit() == 15);
    CHECK(slider_label("row_scroll_limit") == "15");
    CHECK_FALSE(restart_prompt_up());
}

TEST_CASE_METHOD(TouchCallbacksFixture,
                 "Touch page: the long-press slider applies live without a restart",
                 "[settings][touch_callbacks]") {
    release_slider("row_long_press_time", 700);

    CHECK(helix::InputSettingsManager::instance().get_long_press_time() == 700);
    CHECK(slider_label("row_long_press_time") == "700");
    CHECK_FALSE(restart_prompt_up());
}

TEST_CASE_METHOD(TouchCallbacksFixture,
                 "Touch page: the display-setting toggles reach DisplaySettingsManager",
                 "[settings][touch_callbacks]") {
    auto& display = DisplaySettingsManager::instance();

    set_toggle("row_page_scroll_buttons", true);
    CHECK(display.get_page_scroll_buttons());
    set_toggle("row_page_scroll_buttons", false);
    CHECK_FALSE(display.get_page_scroll_buttons());

    set_toggle("row_hide_keyboard_with_hardware", true);
    CHECK(display.get_hide_keyboard_with_hardware());
    set_toggle("row_hide_keyboard_with_hardware", false);
    CHECK_FALSE(display.get_hide_keyboard_with_hardware());
}

TEST_CASE_METHOD(TouchCallbacksFixture, "Touch page: the calibration row runs its callback",
                 "[settings][touch_callbacks]") {
    // An unregistered callback is skipped when the XML is built, so the row would
    // stay on screen and do nothing.
    lv_obj_t* row = lv_obj_find_by_name(root_, "row_touch_calibration");
    REQUIRE(row != nullptr);
    lv_event_cb_t cb = lv_xml_get_event_cb(nullptr, "on_touch_calibration_clicked");
    REQUIRE(cb != nullptr);

    bool attached = false;
    for (uint32_t i = 0; i < lv_obj_get_event_count(row); ++i) {
        attached = attached || lv_event_dsc_get_cb(lv_obj_get_event_dsc(row, i)) == cb;
    }
    CHECK(attached);
}
