// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The numeric keypad's digit order follows the Number Pad Layout setting. Phone
// puts 1-2-3 on top and backspace bottom-left, and on an integer-only field the
// cell the dot would occupy holds a confirm key. Calculator is 7-8-9 on top, dot
// bottom-left, and never shows the in-pad confirm.

#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "input_settings_manager.h"
#include "input_settings_test_helpers.h"
#include "settings_manager.h"

#include <array>

#include "../catch_amalgamated.hpp"

using helix::InputSettingsManager;
using helix::KeypadLayout;

namespace {

struct KeypadLayoutFixture : LVGLUITestFixture {
    float confirmed_ = -1.0f;
    int confirms_ = 0;

    KeypadLayoutFixture() {
        SettingsManager::instance().init_subjects();
        helix_test::reset_input_settings_to_defaults();
        std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
        for (auto& p : panels) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels.data());
        ui_keypad_init(lv_screen_active());
    }

    ~KeypadLayoutFixture() override {
        NavigationManager::instance().shutdown();
        helix::ui::destroy_static_panels();
        helix::ui::UpdateQueue::instance().drain();
        helix_test::reset_input_settings_to_defaults();
    }

    lv_obj_t* show(bool allow_decimal) {
        ui_keypad_config_t config = {};
        config.min_value = 0;
        config.max_value = 300;
        config.allow_decimal = allow_decimal;
        config.callback = [](float value, void* ud) {
            auto* self = static_cast<KeypadLayoutFixture*>(ud);
            self->confirmed_ = value;
            ++self->confirms_;
        };
        config.user_data = this;
        ui_keypad_show(&config);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_t* keypad = lv_obj_find_by_name(lv_screen_active(), "keypad_panel");
        REQUIRE(keypad != nullptr);
        lv_obj_update_layout(keypad);
        return keypad;
    }

    static lv_obj_t* key(lv_obj_t* keypad, const char* name) {
        lv_obj_t* k = lv_obj_find_by_name(keypad, name);
        REQUIRE(k != nullptr);
        return k;
    }

    static int32_t y_of(lv_obj_t* keypad, const char* name) {
        lv_area_t a;
        lv_obj_get_coords(key(keypad, name), &a);
        return a.y1;
    }

    static int32_t x_of(lv_obj_t* keypad, const char* name) {
        lv_area_t a;
        lv_obj_get_coords(key(keypad, name), &a);
        return a.x1;
    }

    static int32_t height_of(lv_obj_t* keypad, const char* name) {
        return lv_obj_get_height(key(keypad, name));
    }

    static bool shown(lv_obj_t* keypad, const char* name) {
        return !lv_obj_has_flag(key(keypad, name), LV_OBJ_FLAG_HIDDEN);
    }

    void press(lv_obj_t* keypad, const char* name) {
        lv_obj_send_event(key(keypad, name), LV_EVENT_CLICKED, nullptr);
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(KeypadLayoutFixture, "Keypad layout defaults to phone",
                 "[keypad][keypad_layout][input_settings]") {
    CHECK(InputSettingsManager::instance().get_keypad_layout() == KeypadLayout::PHONE);
}

TEST_CASE_METHOD(KeypadLayoutFixture, "Phone layout: 1-2-3 on top, backspace bottom-left",
                 "[keypad][keypad_layout]") {
    InputSettingsManager::instance().set_keypad_layout(KeypadLayout::PHONE);
    lv_obj_t* keypad = show(true);

    CHECK(y_of(keypad, "btn_1") < y_of(keypad, "btn_4"));
    CHECK(y_of(keypad, "btn_4") < y_of(keypad, "btn_7"));
    CHECK(y_of(keypad, "btn_7") < y_of(keypad, "btn_0"));
    CHECK(x_of(keypad, "btn_backspace") < x_of(keypad, "btn_0"));
    CHECK(x_of(keypad, "btn_0") < x_of(keypad, "btn_dot"));
    // Every row is the same height, the bottom one included.
    CHECK(height_of(keypad, "btn_1") == height_of(keypad, "btn_0"));
}

TEST_CASE_METHOD(KeypadLayoutFixture, "Calculator layout: 7-8-9 on top, dot bottom-left",
                 "[keypad][keypad_layout]") {
    InputSettingsManager::instance().set_keypad_layout(KeypadLayout::CALCULATOR);
    lv_obj_t* keypad = show(true);

    CHECK(y_of(keypad, "btn_7") < y_of(keypad, "btn_4"));
    CHECK(y_of(keypad, "btn_4") < y_of(keypad, "btn_1"));
    CHECK(y_of(keypad, "btn_1") < y_of(keypad, "btn_0"));
    CHECK(x_of(keypad, "btn_dot") < x_of(keypad, "btn_0"));
    CHECK(x_of(keypad, "btn_0") < x_of(keypad, "btn_backspace"));
    CHECK(height_of(keypad, "btn_7") == height_of(keypad, "btn_0"));
}

TEST_CASE_METHOD(KeypadLayoutFixture, "The in-pad confirm key takes the dot's free cell",
                 "[keypad][keypad_layout]") {
    SECTION("phone, integer field: confirm shown in place of the dot") {
        InputSettingsManager::instance().set_keypad_layout(KeypadLayout::PHONE);
        lv_obj_t* keypad = show(false);
        CHECK(shown(keypad, "btn_confirm"));
        CHECK_FALSE(shown(keypad, "btn_dot"));
    }
    SECTION("phone, decimal field: the dot keeps its cell") {
        InputSettingsManager::instance().set_keypad_layout(KeypadLayout::PHONE);
        lv_obj_t* keypad = show(true);
        CHECK_FALSE(shown(keypad, "btn_confirm"));
        CHECK(shown(keypad, "btn_dot"));
    }
    SECTION("calculator, integer field: the cell stays blank") {
        InputSettingsManager::instance().set_keypad_layout(KeypadLayout::CALCULATOR);
        lv_obj_t* keypad = show(false);
        CHECK_FALSE(shown(keypad, "btn_confirm"));
        CHECK_FALSE(shown(keypad, "btn_dot"));
    }
}

TEST_CASE_METHOD(KeypadLayoutFixture, "The in-pad confirm key confirms the entered value",
                 "[keypad][keypad_layout]") {
    InputSettingsManager::instance().set_keypad_layout(KeypadLayout::PHONE);
    lv_obj_t* keypad = show(false);

    press(keypad, "btn_2");
    press(keypad, "btn_1");
    press(keypad, "btn_0");
    press(keypad, "btn_confirm");

    CHECK(confirms_ == 1);
    CHECK(confirmed_ == 210.0f);
    CHECK_FALSE(ui_keypad_is_visible());
}

TEST_CASE_METHOD(KeypadLayoutFixture, "Changing the setting re-lays out an already built keypad",
                 "[keypad][keypad_layout]") {
    InputSettingsManager::instance().set_keypad_layout(KeypadLayout::PHONE);
    lv_obj_t* keypad = show(false);
    REQUIRE(y_of(keypad, "btn_1") < y_of(keypad, "btn_7"));
    ui_keypad_hide();
    helix::ui::UpdateQueue::instance().drain();

    InputSettingsManager::instance().set_keypad_layout(KeypadLayout::CALCULATOR);
    REQUIRE(show(false) == keypad);

    CHECK(y_of(keypad, "btn_7") < y_of(keypad, "btn_1"));
    CHECK(x_of(keypad, "btn_0") < x_of(keypad, "btn_backspace"));
    CHECK_FALSE(shown(keypad, "btn_confirm"));
}
