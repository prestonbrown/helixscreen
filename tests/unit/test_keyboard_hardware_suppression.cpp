// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_keyboard_hardware_suppression.cpp
 * @brief The on-screen keyboard stays down while a hardware keyboard is attached
 *        and the user asked for that, and typing still reaches the focused field
 *        (prestonbrown/helixscreen#1572).
 */

#include "ui_keyboard_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "keyboard_layout_provider.h"
#include "lvgl/lvgl.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::DisplaySettingsManager;

namespace {

/// Owns the process-wide KeyboardManager and the two inputs of the policy for
/// one test, and puts all of them back afterwards.
struct ScopedKeyboardPolicy {
    lv_group_t* group = nullptr;
    lv_group_t* prior_default = nullptr;
    bool prior_hide = false;
    bool prior_present = false;

    explicit ScopedKeyboardPolicy(lv_obj_t* parent) {
        auto& settings = DisplaySettingsManager::instance();
        prior_hide = settings.get_hide_keyboard_with_hardware();
        prior_present = settings.hardware_keyboard_present();
        prior_default = lv_group_get_default();
        group = lv_group_create();
        lv_group_set_default(group);
        release_keyboard();
        KeyboardManager::instance().init(parent);
    }
    ~ScopedKeyboardPolicy() {
        release_keyboard();
        auto& settings = DisplaySettingsManager::instance();
        settings.set_hide_keyboard_with_hardware(prior_hide);
        settings.set_hardware_keyboard_present(prior_present);
        lv_group_set_default(prior_default);
        lv_group_delete(group);
    }

    static void release_keyboard() {
        KeyboardManager::instance().reset();
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// Focus a registered textarea the way a tap does, through the input group.
lv_obj_t* focus_new_textarea(lv_obj_t* parent, lv_group_t* group) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    KeyboardManager::instance().register_textarea(ta);
    lv_group_focus_obj(ta);
    REQUIRE(lv_group_get_focused(group) == ta);
    return ta;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The on-screen keyboard is suppressed only with the setting on and a keyboard "
                 "attached",
                 "[1572][keyboard]") {
    ScopedKeyboardPolicy policy(test_screen());
    auto& settings = DisplaySettingsManager::instance();

    struct Row {
        bool hide_setting;
        bool present;
        bool keyboard_shown;
    };
    const Row rows[] = {
        {false, false, true},
        {false, true, true},
        {true, false, true},
        {true, true, false},
    };
    for (const Row& row : rows) {
        INFO("hide setting " << row.hide_setting << ", keyboard attached " << row.present);
        settings.set_hide_keyboard_with_hardware(row.hide_setting);
        settings.set_hardware_keyboard_present(row.present);
        CHECK(settings.soft_keyboard_suppressed() == (row.hide_setting && row.present));

        lv_obj_t* ta = focus_new_textarea(test_screen(), policy.group);
        CHECK(KeyboardManager::instance().is_visible() == row.keyboard_shown);

        lv_group_focus_obj(nullptr);
        lv_obj_delete(ta);
        KeyboardManager::instance().hide();
        process_lvgl(400);
        REQUIRE_FALSE(KeyboardManager::instance().is_visible());
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "A suppressed on-screen keyboard leaves the field focused and typeable",
                 "[1572][keyboard]") {
    ScopedKeyboardPolicy policy(test_screen());
    auto& settings = DisplaySettingsManager::instance();
    settings.set_hide_keyboard_with_hardware(true);
    settings.set_hardware_keyboard_present(true);

    lv_obj_t* ta = focus_new_textarea(test_screen(), policy.group);
    REQUIRE_FALSE(KeyboardManager::instance().is_visible());
    CHECK(lv_obj_has_state(ta, LV_STATE_FOCUSED));

    // What a keypad indev in this group delivers for each key.
    lv_group_send_data(policy.group, 'o');
    lv_group_send_data(policy.group, 'k');
    CHECK(std::string(lv_textarea_get_text(ta)) == "ok");

    lv_group_focus_obj(nullptr);
    lv_obj_delete(ta);
}

TEST_CASE_METHOD(LVGLUITestFixture, "The hardware keyboard setting row shows only with a keyboard",
                 "[1572][keyboard][settings]") {
    auto& settings = DisplaySettingsManager::instance();
    const bool prior_present = settings.hardware_keyboard_present();

    lv_obj_t* overlay =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "settings_touch_overlay", nullptr));
    REQUIRE(overlay != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(overlay, "row_hide_keyboard_with_hardware");
    REQUIRE(row != nullptr);

    settings.set_hardware_keyboard_present(false);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    settings.set_hardware_keyboard_present(true);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    settings.set_hardware_keyboard_present(prior_present);
    lv_obj_delete(overlay);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Suppression turning on takes down a keyboard already raised for another field",
                 "[1572][keyboard]") {
    ScopedKeyboardPolicy policy(test_screen());
    auto& settings = DisplaySettingsManager::instance();
    settings.set_hide_keyboard_with_hardware(false);
    settings.set_hardware_keyboard_present(true);

    lv_obj_t* first = focus_new_textarea(test_screen(), policy.group);
    REQUIRE(KeyboardManager::instance().is_visible());
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(lv_keyboard_get_textarea(kb) == first);

    // A direct show() for another field, as a caller that raises the keyboard
    // itself does, with no defocus of the first field in between.
    settings.set_hide_keyboard_with_hardware(true);
    lv_obj_t* second = lv_textarea_create(test_screen());
    KeyboardManager::instance().show(second);
    process_lvgl(400);

    CHECK_FALSE(KeyboardManager::instance().is_visible());
    CHECK(lv_keyboard_get_textarea(kb) == nullptr);

    lv_group_focus_obj(nullptr);
    lv_obj_delete(second);
    lv_obj_delete(first);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Hiding a suppressed keyboard leaves the screen's layout alone",
                 "[1572][keyboard]") {
    ScopedKeyboardPolicy policy(test_screen());
    auto& settings = DisplaySettingsManager::instance();
    settings.set_hide_keyboard_with_hardware(true);
    settings.set_hardware_keyboard_present(true);

    // A screen child placed below the top on purpose, which an unsuppressed
    // hide() would slide back to y=0.
    lv_obj_t* placed = lv_obj_create(lv_screen_active());
    lv_obj_set_y(placed, 37);
    lv_obj_update_layout(placed);
    REQUIRE(lv_obj_get_y(placed) == 37);
    lv_obj_t* ta = focus_new_textarea(test_screen(), policy.group);
    REQUIRE_FALSE(KeyboardManager::instance().is_visible());

    KeyboardManager::instance().hide();
    process_lvgl(400);
    lv_obj_update_layout(placed);

    CHECK(lv_obj_get_y(placed) == 37);

    lv_group_focus_obj(nullptr);
    lv_obj_delete(ta);
    lv_obj_delete(placed);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The keyboard manager applies the no-bubble map when popovers are off",
                 "[keyboard][keyboard_popover]") {
    ScopedKeyboardPolicy policy(test_screen());
    lv_obj_t* kb = KeyboardManager::instance().get_instance();
    REQUIRE(kb != nullptr);
    lv_keyboard_set_popovers(kb, false);

    lv_obj_t* ta = focus_new_textarea(test_screen(), policy.group);
    KeyboardManager::instance().show(ta);

    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
    const lv_buttonmatrix_ctrl_t* ctrl = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);
    uint32_t buttons = 0;
    for (size_t i = 0; map[i][0] != '\0'; ++i) {
        if (std::string(map[i]) != "\n") {
            ++buttons;
        }
    }
    size_t popover_keys = 0;
    for (uint32_t i = 0; i < buttons; ++i) {
        INFO("button " << i);
        CHECK_FALSE(lv_buttonmatrix_has_button_ctrl(kb, i, LV_BUTTONMATRIX_CTRL_POPOVER));
        if (ctrl[i] & LV_BUTTONMATRIX_CTRL_POPOVER) {
            ++popover_keys;
        }
    }
    REQUIRE(popover_keys > 0);

    lv_group_focus_obj(nullptr);
    lv_obj_delete(ta);
}
