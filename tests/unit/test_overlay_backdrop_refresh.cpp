// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_backdrop_refresh.cpp
 * @brief The overlay backdrop is a frozen bitmap, and it has to be re-takeable
 *
 * push_overlay() darkens an lv_snapshot_take() of the whole screen and parks it
 * in front of everything, so every pixel outside the overlay stops tracking the
 * widgets beneath it. The navigation bar is the part of that snapshot the user
 * can still see, so a setting that adds or removes a navbar element while its
 * own toggle sits inside an overlay changed nothing visible until the stack
 * popped — the widget un-hid instantly, the photo of it did not.
 *
 * refresh_overlay_backdrop() re-takes the shot. The contract that matters is
 * WHAT the new snapshot contains: the live base content, and none of the
 * overlays or the outgoing backdrop that are stacked on top of it by the time
 * the refresh runs. Get that wrong and the backdrop turns into a photograph of
 * itself.
 */

#include "ui_nav_manager.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "lvgl/lvgl.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr lv_color_t BASE_COLOR = LV_COLOR_MAKE(0xFF, 0x00, 0x00);    // red
constexpr lv_color_t OVERLAY_COLOR = LV_COLOR_MAKE(0x00, 0xFF, 0x00); // green

/// Full-screen opaque child in a known flat color.
lv_obj_t* make_flat_layer(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    return obj;
}

/// Center pixel of the image a backdrop widget is displaying.
/// The backdrop's source is the lv_draw_buf_t built by create_darkened_backdrop.
struct Rgb {
    uint8_t r, g, b;
};

Rgb backdrop_center_pixel(lv_obj_t* backdrop) {
    REQUIRE(backdrop != nullptr);
    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(backdrop));
    REQUIRE(buf != nullptr);
    REQUIRE(buf->header.cf == LV_COLOR_FORMAT_ARGB8888);

    uint32_t x = buf->header.w / 2;
    uint32_t y = buf->header.h / 2;
    const auto* row = static_cast<const uint8_t*>(buf->data) + y * buf->header.stride;
    const uint8_t* px = row + x * 4; // BGRA byte order in LVGL's ARGB8888
    return Rgb{px[2], px[1], px[0]};
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Backdrop refresh re-snapshots the live base content",
                 "[navigation][backdrop][refresh]") {
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();

    lv_obj_t* base = make_flat_layer(screen, BASE_COLOR);
    process_lvgl(20);

    // First overlay: snapshot the screen (all red), then hide the base and cover
    // it with a green overlay — exactly what push_overlay() does in that order.
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    lv_obj_add_flag(base, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* overlay = make_flat_layer(screen, OVERLAY_COLOR);
    lv_obj_move_foreground(overlay);
    NavigationManagerTestAccess::set_panel_stack(nav, {base, overlay});
    process_lvgl(20);

    Rgb before = backdrop_center_pixel(backdrop);
    CHECK(before.r > before.g); // the snapshot is of the red base

    SECTION("the replacement captures the base, not the overlay stacked over it") {
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);

        lv_obj_t* refreshed = NavigationManagerTestAccess::overlay_backdrop(nav);
        REQUIRE(refreshed != nullptr);
        REQUIRE(refreshed != backdrop); // a NEW snapshot, not the stale one

        Rgb after = backdrop_center_pixel(refreshed);
        // Green here means the overlay was left visible during the snapshot and
        // the backdrop is now a photo of the thing it is supposed to sit behind.
        CHECK(after.g < after.r);
        // Red means the hidden base panel was un-hidden for the shot. Without
        // that, the snapshot is of an empty screen and this channel collapses.
        CHECK(after.r > 0x40);
    }

    SECTION("the overlay and base end up in the visibility state they started in") {
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);

        CHECK(lv_obj_has_flag(base, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("the replacement stays below the overlay it backs") {
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);

        lv_obj_t* refreshed = NavigationManagerTestAccess::overlay_backdrop(nav);
        REQUIRE(refreshed != nullptr);
        CHECK(lv_obj_get_index(refreshed) < lv_obj_get_index(overlay));
    }

    NavigationManagerTestAccess::set_panel_stack(nav, {});
}

TEST_CASE_METHOD(LVGLTestFixture, "Backdrop refresh is a no-op with no backdrop live",
                 "[navigation][backdrop][refresh]") {
    auto& nav = NavigationManager::instance();
    REQUIRE(NavigationManagerTestAccess::overlay_backdrop(nav) == nullptr);

    NavigationManagerTestAccess::refresh_overlay_backdrop(nav);

    // No overlay is open, so there is nothing to re-photograph — refreshing must
    // not conjure a backdrop that would then dim the whole screen.
    CHECK(NavigationManagerTestAccess::overlay_backdrop(nav) == nullptr);
}
