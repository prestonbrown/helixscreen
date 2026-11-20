// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toast_markup.cpp
 * @brief What a shown toast looks like in the widget tree: the stack it sits
 * in on lv_layer_top() and the text its labels carry.
 */

#include "ui_toast_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/toast_manager_test_access.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "theme_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct ToastMarkupFixture : LVGLUITestFixture {
    ToastMarkupFixture() {
        animations_were_enabled_ =
            helix::DisplaySettingsManager::instance().get_animations_enabled();
        helix::DisplaySettingsManager::instance().set_animations_enabled(false);
        ToastManagerTestAccess::clear(ToastManager::instance());
        ToastManager::instance().init();
    }
    ~ToastMarkupFixture() override {
        ToastManager::instance().deinit_subjects();
        helix::DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static lv_obj_t* shown(const char* name) {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        return lv_obj_find_by_name(lv_layer_top(), name);
    }

    static std::string label_text(const char* name) {
        lv_obj_t* label = lv_obj_find_by_name(lv_layer_top(), name);
        REQUIRE(label != nullptr);
        return lv_label_get_text(label);
    }

    bool animations_were_enabled_ = true;
};

} // namespace

TEST_CASE_METHOD(ToastMarkupFixture,
                 "Toast markup: toasts stack in a transparent column that takes no input",
                 "[toast][xml]") {
    ToastManager::instance().show(ToastSeverity::INFO, "first", 0);
    lv_obj_t* toast = shown("toast_root");
    REQUIRE(toast != nullptr);

    lv_obj_t* stack = lv_obj_get_parent(toast);
    REQUIRE(stack != nullptr);
    CHECK(lv_obj_get_parent(stack) == lv_layer_top());
    CHECK(lv_obj_get_style_flex_flow(stack, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_get_style_bg_opa(stack, LV_PART_MAIN) == LV_OPA_TRANSP);
    CHECK(lv_obj_get_style_border_width(stack, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_pad_top(stack, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_pad_left(stack, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_pad_row(stack, LV_PART_MAIN) == theme_manager_get_spacing("space_sm"));
    CHECK_FALSE(lv_obj_is_clickable(stack));
    CHECK_FALSE(lv_obj_is_scrollable(stack));

    ToastManager::instance().show(ToastSeverity::WARNING, "second", 0);
    shown("toast_root");
    CHECK(lv_obj_get_child_count(stack) == 2);
}

TEST_CASE_METHOD(ToastMarkupFixture, "Toast markup: message and detail show their text verbatim",
                 "[toast][xml]") {
    // Printer and plugin text can start with '#'; it must not be read as a const.
    ToastManager::instance().show_with_detail(ToastSeverity::ERROR, "#2 extruder jammed",
                                              "#check the gear", 0);
    REQUIRE(shown("toast_root") != nullptr);
    CHECK(label_text("toast_message") == "#2 extruder jammed");
    CHECK(label_text("toast_detail") == "#check the gear");
    CHECK_FALSE(lv_obj_is_hidden(lv_obj_find_by_name(lv_layer_top(), "toast_detail")));
}

TEST_CASE_METHOD(ToastMarkupFixture, "Toast markup: a toast without detail hides the detail line",
                 "[toast][xml]") {
    ToastManager::instance().show(ToastSeverity::SUCCESS, "Saved", 0);
    REQUIRE(shown("toast_root") != nullptr);
    CHECK(label_text("toast_message") == "Saved");
    CHECK(lv_obj_is_hidden(lv_obj_find_by_name(lv_layer_top(), "toast_detail")));
    CHECK(lv_obj_is_hidden(lv_obj_find_by_name(lv_layer_top(), "toast_action_btn")));
}
