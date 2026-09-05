// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: exercises patches/lvgl_indev_delete_cancels_anim.patch, which has
//                 no HelixScreen header to include

/**
 * @file test_indev_delete_scroll_throw.cpp
 * @brief Deleting an input device must leave no animation pointing at it.
 *
 * A flick that ends with scroll momentum starts LVGL's scroll-throw animation,
 * whose var is the lv_indev_t itself and whose completed/deleted callbacks
 * write through that pointer (indev_scroll_throw_anim_reset in
 * lib/lvgl/src/indev/lv_indev.c zeroes pointer.scroll_throw_vect and nulls
 * scroll_throw_anim). lv_indev_delete() frees the device, so an animation that
 * outlives it writes into whatever the allocator hands out next — silently,
 * because those callbacks fire from lv_anim_delete_all()/lv_deinit() long after
 * the delete, and the corrupted block is usually someone else's.
 *
 * DisplayManager swaps input devices with lv_indev_delete() while the UI is
 * live (src/application/display_manager.cpp#DisplayManager), and lv_deinit()
 * deletes every remaining device at shutdown, so both paths depend on this.
 */

#include "lvgl_test_fixture.h"

#include "../catch_amalgamated.hpp"

namespace {

/// State the synthetic indev's read callback reports. File-scope because LVGL
/// retains the callback for as long as the indev lives.
struct ThrowIndevState {
    lv_point_t point{0, 0};
    lv_indev_state_t state{LV_INDEV_STATE_RELEASED};
};

ThrowIndevState g_throw_indev_state;

void throw_indev_read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    data->point = g_throw_indev_state.point;
    data->state = g_throw_indev_state.state;
}

/// Virtual-clock step between synthetic reads, fine enough that the drag below
/// registers as motion rather than one teleport.
constexpr int READ_STEP_MS = 10;

void send(lv_indev_t* indev, int x, int y, lv_indev_state_t state) {
    g_throw_indev_state.point = {x, y};
    g_throw_indev_state.state = state;
    lv_tick_inc(READ_STEP_MS);
    lv_indev_read(indev);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "lv_indev_delete leaves no animation targeting the device",
                 "[indev][lifetime][anim]") {
    // A container whose content is taller than itself, so a vertical drag
    // inside it actually scrolls and sets indev->pointer.scroll_obj — the one
    // condition indev_proc_release() checks before starting the throw.
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, 200, 200);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tall = lv_obj_create(container);
    lv_obj_set_size(tall, 180, 1000);
    lv_obj_update_layout(test_screen());

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, throw_indev_read_cb);

    // Flick upward inside the container. The steps clear LVGL's scroll limit
    // (LV_INDEV_DEF_SCROLL_LIMIT, 10px) several times over, and the last one
    // still carries motion when the finger lifts, which is what leaves
    // momentum behind.
    send(indev, 100, 180, LV_INDEV_STATE_PRESSED);
    for (int y = 160; y >= 40; y -= 20) {
        send(indev, 100, y, LV_INDEV_STATE_PRESSED);
    }
    send(indev, 100, 40, LV_INDEV_STATE_RELEASED);

    // Precondition, not the assertion under test: without a live throw
    // animation there is nothing for the delete to clean up and the check
    // below would pass having proven nothing.
    REQUIRE(lv_anim_get(indev, nullptr) != nullptr);

    // Kept only as a search key for lv_anim_get(), which compares var and
    // never dereferences it.
    void* const deleted_indev = indev;
    lv_indev_delete(indev);

    CHECK(lv_anim_get(deleted_indev, nullptr) == nullptr);
}
