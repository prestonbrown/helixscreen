// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The busy overlay, the loading pill and the lock screen are XML components
// created on lv_layer_top(). Their text takes the same font and color it had
// when C++ built them, and showing and hiding them leaves the layer as it was.

#include "ui_busy_overlay.h"
#include "ui_lock_screen.h"
#include "ui_nav_transitions.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"
#include "theme_manager.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

lv_obj_t* find_on_top_layer(const char* name) {
    return lv_obj_find_by_name(lv_layer_top(), name);
}

const char* label_text(const char* name) {
    lv_obj_t* label = find_on_top_layer(name);
    return label ? lv_label_get_text(label) : nullptr;
}

bool same_color(lv_color_t a, lv_color_t b) {
    return lv_color_to_u32(a) == lv_color_to_u32(b);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Busy overlay label shows the overlay's text",
                 "[ui_busy_overlay][top_layer]") {
    BusyOverlay::hide();
    process_lvgl(20);

    SECTION("shown at once") {
        BusyOverlay::show("Preparing...", 0);
        REQUIRE(label_text("busy_overlay_label") != nullptr);
        CHECK(std::strcmp(label_text("busy_overlay_label"), "Preparing...") == 0);

        BusyOverlay::set_progress("Downloading", 45.0f);
        CHECK(std::strcmp(label_text("busy_overlay_label"), "Downloading... 45%") == 0);

        BusyOverlay::show("Second", 0);
        CHECK(std::strcmp(label_text("busy_overlay_label"), "Second") == 0);
    }

    SECTION("progress set during the grace period appears with the overlay") {
        BusyOverlay::show("Preparing...", 30);
        BusyOverlay::set_progress("Uploading", 12.0f);
        CHECK(find_on_top_layer("busy_overlay_label") == nullptr);
        REQUIRE(advance_until([] { return BusyOverlay::is_visible(); }, 200));
        REQUIRE(label_text("busy_overlay_label") != nullptr);
        CHECK(std::strcmp(label_text("busy_overlay_label"), "Uploading... 12%") == 0);
    }

    BusyOverlay::hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Busy overlay keeps its spinner and small text font",
                 "[ui_busy_overlay][top_layer]") {
    BusyOverlay::hide();
    process_lvgl(20);
    BusyOverlay::show("Please wait...", 0);

    CHECK(find_on_top_layer("busy_overlay_spinner") != nullptr);
    lv_obj_t* label = find_on_top_layer("busy_overlay_label");
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) == theme_manager_get_font("font_small"));
    CHECK(same_color(lv_obj_get_style_text_color(label, LV_PART_MAIN),
                     theme_manager_get_color("text")));

    BusyOverlay::hide();
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Busy overlay show/hide cycles leave the top layer as it was",
                 "[ui_busy_overlay][top_layer]") {
    BusyOverlay::hide();
    process_lvgl(20);
    const uint32_t before = lv_obj_get_child_count(lv_layer_top());

    for (int i = 0; i < 5; ++i) {
        BusyOverlay::show("Cycle", 0);
        REQUIRE(BusyOverlay::is_visible());
        BusyOverlay::hide();
        process_lvgl(20);
    }

    CHECK(lv_obj_get_child_count(lv_layer_top()) == before);
    CHECK(find_on_top_layer("busy_overlay_label") == nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Loading pill is a white label in the top layer's base font",
                 "[navigation][loading_pill][top_layer]") {
    const uint32_t before = lv_obj_get_child_count(lv_layer_top());

    lv_obj_t* pill = helix::nav::make_loading_scrim();
    REQUIRE(pill != nullptr);
    CHECK(lv_obj_get_parent(pill) == lv_layer_top());
    CHECK_FALSE(lv_obj_is_scrollable(pill));
    CHECK(lv_obj_get_style_border_width(pill, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_style_bg_opa(pill, LV_PART_MAIN) == LV_OPA_80);

    lv_obj_t* label = lv_obj_find_by_name(pill, "loading_scrim_label");
    REQUIRE(label != nullptr);
    CHECK(std::strcmp(lv_label_get_text(label), lv_tr("Loading...")) == 0);
    CHECK(same_color(lv_obj_get_style_text_color(label, LV_PART_MAIN), lv_color_white()));
    CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
          lv_obj_get_style_text_font(lv_layer_top(), LV_PART_MAIN));

    helix::ui::safe_delete_deferred(pill);
    process_lvgl(20);
    CHECK(lv_obj_get_child_count(lv_layer_top()) == before);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Lock screen component is the full-screen top-layer root",
                 "[lock_screen][top_layer]") {
    auto& lock = helix::ui::LockScreenOverlay::instance();
    lock.hide();
    process_lvgl(20);
    const uint32_t before = lv_obj_get_child_count(lv_layer_top());

    lock.show();
    REQUIRE(lock.is_visible());
    REQUIRE(lv_obj_get_child_count(lv_layer_top()) == before + 1);
    lv_obj_t* root = lv_obj_get_child(lv_layer_top(), -1);
    lv_obj_update_layout(root);
    CHECK(lv_obj_is_clickable(root));
    CHECK(lv_obj_get_width(root) == lv_obj_get_width(lv_layer_top()));
    CHECK(lv_obj_get_height(root) == lv_obj_get_height(lv_layer_top()));
    CHECK(lv_obj_find_by_name(root, "lock_keypad") != nullptr);

    lock.hide();
    process_lvgl(20);
    for (int i = 0; i < 3; ++i) {
        lock.show();
        lock.hide();
        process_lvgl(20);
    }
    CHECK(lv_obj_get_child_count(lv_layer_top()) == before);
    helix::ui::UpdateQueue::instance().drain();
}
