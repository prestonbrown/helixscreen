// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toast_second_action.cpp
 * @brief show_with_action()'s optional second button: shown with its own label,
 * and a tap on each button runs that button's callback alone.
 */

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/toast_manager_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "display_settings_manager.h"

#include <cstdint>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

int g_first = 0;
int g_second = 0;
void* g_second_data = nullptr;

void first_cb(void*) {
    ++g_first;
}
void second_cb(void* data) {
    ++g_second;
    g_second_data = data;
}

void* data_7() {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(7));
}

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

struct SecondActionFixture : LVGLUITestFixture {
    SecondActionFixture() {
        animations_were_enabled_ =
            helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        g_first = 0;
        g_second = 0;
        g_second_data = nullptr;
        ToastManagerTestAccess::clear(ToastManager::instance());
        ToastManager::instance().init();
    }
    ~SecondActionFixture() override {
        ToastManager::instance().deinit_subjects();
        ToastManagerTestAccess::clear(ToastManager::instance());
        helix::DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
        drain();
    }
    bool animations_were_enabled_ = true;
};

std::string label_of(lv_obj_t* btn) {
    REQUIRE(btn != nullptr);
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class)) {
            return lv_label_get_text(child);
        }
    }
    return {};
}

} // namespace

TEST_CASE_METHOD(SecondActionFixture, "an action toast's second button runs its own callback",
                 "[toast][second-action]") {
    auto& tm = ToastManager::instance();
    tm.show_with_action(ToastSeverity::SUCCESS, "Lane 2: eSUN PLA", "Undo", first_cb, nullptr, 0,
                        nullptr, ToastAction{"Print Label", second_cb, data_7()});
    drain();
    REQUIRE(tm.is_visible());

    // Both buttons sit on a row under the text, which keeps the toast's width
    // for a one-line headline; the side button hides.
    lv_obj_t* side = lv_obj_find_by_name(lv_layer_top(), "toast_action_btn");
    lv_obj_t* first = lv_obj_find_by_name(lv_layer_top(), "toast_action_below_btn");
    lv_obj_t* second = lv_obj_find_by_name(lv_layer_top(), "toast_action2_btn");
    lv_obj_t* row = lv_obj_find_by_name(lv_layer_top(), "toast_actions_below");
    REQUIRE(second != nullptr);
    REQUIRE(row != nullptr);
    CHECK(lv_obj_has_flag(side, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    CHECK(label_of(first) == "Undo");
    CHECK(label_of(second) == "Print Label");
    lv_obj_t* headline = lv_obj_find_by_name(lv_layer_top(), "toast_message");
    REQUIRE(headline != nullptr);
    CHECK(lv_label_get_long_mode(headline) == LV_LABEL_LONG_MODE_DOTS);

    lv_obj_send_event(second, LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(g_second == 1);
    CHECK(g_second_data == data_7());
    CHECK(g_first == 0);
    CHECK_FALSE(tm.is_visible());

    // The row's main button runs the main action.
    tm.show_with_action(ToastSeverity::SUCCESS, "Lane 3: Sunlu PETG", "Undo", first_cb, nullptr, 0,
                        nullptr, ToastAction{"Print Label", second_cb, data_7()});
    drain();
    lv_obj_send_event(lv_obj_find_by_name(lv_layer_top(), "toast_action_below_btn"),
                      LV_EVENT_CLICKED, nullptr);
    drain();
    CHECK(g_first == 1);
    CHECK(g_second == 1);
}

TEST_CASE_METHOD(SecondActionFixture, "an action toast without a second action shows one button",
                 "[toast][second-action]") {
    auto& tm = ToastManager::instance();
    tm.show_with_action(ToastSeverity::SUCCESS, "Saved", "Undo", first_cb, nullptr, 0);
    drain();
    lv_obj_t* row = lv_obj_find_by_name(lv_layer_top(), "toast_actions_below");
    REQUIRE(row != nullptr);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(lv_layer_top(), "toast_action_btn"),
                                LV_OBJ_FLAG_HIDDEN));

    lv_obj_send_event(lv_obj_find_by_name(lv_layer_top(), "toast_action_btn"), LV_EVENT_CLICKED,
                      nullptr);
    drain();
    CHECK(g_first == 1);
    CHECK(g_second == 0);
}
