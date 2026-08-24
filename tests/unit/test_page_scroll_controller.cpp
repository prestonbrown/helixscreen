// SPDX-License-Identifier: GPL-3.0-or-later
#include "../lvgl_ui_test_fixture.h"
#include "display_settings_manager.h"
#include "page_scroll_controller.h"
#include "page_scroll_math.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {
// Build a column-flex scrollable container with `n` fixed-height rows.
lv_obj_t* make_scroll_container(lv_obj_t* parent, int n, int32_t h) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, 400, 200);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_create(c);
        lv_obj_set_size(row, lv_pct(100), h);
    }
    lv_obj_update_layout(c);
    return c;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "PageScrollController attaches gutter to overflowing container",
                 "[page_scroll_buttons][ui]") {
    lv_obj_t* c = make_scroll_container(test_screen(), 20, 60); // 1200px >> 200px
    REQUIRE(lv_obj_get_scroll_bottom(c) > 0);

    PageScrollController ctl;
    REQUIRE(ctl.attach(c));
    CHECK(ctl.gutter() != nullptr);
    CHECK(ctl.up_button() != nullptr);
    CHECK(ctl.down_button() != nullptr);

    lv_obj_update_layout(c);
    // Content reflowed narrower than the container's inner width.
    CHECK(lv_obj_get_content_width(c) < 400);
    // Native scrollbar suppressed while the gutter is active.
    CHECK(lv_obj_get_scrollbar_mode(c) == LV_SCROLLBAR_MODE_OFF);
}

TEST_CASE_METHOD(LVGLUITestFixture, "PageScrollController disables up at top and pages down",
                 "[page_scroll_buttons][ui]") {
    lv_obj_t* c = make_scroll_container(test_screen(), 20, 60);
    // page_down() animates when the Animations setting is on; force it off so the
    // scroll offset is deterministic immediately after the call.
    helix::DisplaySettingsManager::instance().init_subjects();
    helix::DisplaySettingsManager::instance().set_animations_enabled(false);

    PageScrollController ctl;
    REQUIRE(ctl.attach(c));

    // At top: up disabled, down enabled.
    CHECK(lv_obj_has_state(ctl.up_button(), LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(ctl.down_button(), LV_STATE_DISABLED));

    int32_t before = lv_obj_get_scroll_y(c);
    ctl.page_down(); // LV_ANIM_OFF here since Animations is disabled above
    lv_obj_update_layout(c);
    CHECK(lv_obj_get_scroll_y(c) > before);

    // Scroll to the very bottom: down disabled.
    lv_obj_scroll_to_y(c, lv_obj_get_scroll_bottom(c) + lv_obj_get_scroll_y(c), LV_ANIM_OFF);
    ctl.refresh_reach_state();
    CHECK(lv_obj_has_state(ctl.down_button(), LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(ctl.up_button(), LV_STATE_DISABLED));
}

TEST_CASE_METHOD(LVGLUITestFixture, "PageScrollController clamps the last page to the content end",
                 "[page_scroll_buttons][ui]") {
    helix::DisplaySettingsManager::instance().init_subjects();
    helix::DisplaySettingsManager::instance().set_animations_enabled(false);

    lv_obj_t* c = make_scroll_container(test_screen(), 1, 10);
    lv_obj_t* row = lv_obj_get_child(c, 0);

    PageScrollController ctl;
    REQUIRE(ctl.attach(c));
    lv_obj_update_layout(c);

    // Overflow by far less than one page step, so a full-page delta would overshoot
    // the content end and leave dead space below the last row.
    constexpr int32_t OVERFLOW_PX = 30;
    lv_obj_set_height(row, lv_obj_get_content_height(c) + OVERFLOW_PX);
    lv_obj_update_layout(c);
    REQUIRE(lv_obj_get_scroll_bottom(c) == OVERFLOW_PX);
    REQUIRE(page_scroll_step(lv_obj_get_content_height(c)) > OVERFLOW_PX);

    ctl.page_down();
    lv_obj_update_layout(c);
    CHECK(lv_obj_get_scroll_y(c) == OVERFLOW_PX); // stopped exactly at the content end
    CHECK(lv_obj_get_scroll_bottom(c) == 0);      // no empty space scrolled past the content

    // A press against the already-reached end must not move it further.
    ctl.page_down();
    lv_obj_update_layout(c);
    CHECK(lv_obj_get_scroll_y(c) == OVERFLOW_PX);

    // Symmetric on the way back up: the partial last page lands on the content start.
    ctl.page_up();
    lv_obj_update_layout(c);
    CHECK(lv_obj_get_scroll_y(c) == 0);
    CHECK(lv_obj_get_scroll_top(c) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "PageScrollController hides gutter when content fits",
                 "[page_scroll_buttons][ui]") {
    lv_obj_t* c = make_scroll_container(test_screen(), 1, 40); // 40px << 200px, no overflow
    // LVGL doesn't clamp lv_obj_get_scroll_bottom() to 0 when content is smaller
    // than the viewport — it goes negative (see lv_obj_scroll_by_bounded's own
    // `if (scroll_max < 0) scroll_max = 0;` clamp). <=0 is the real "no room
    // below" invariant, matching page_scroll_math.h's documented contract.
    REQUIRE(lv_obj_get_scroll_bottom(c) <= 0);

    PageScrollController ctl;
    REQUIRE(ctl.attach(c));
    CHECK(lv_obj_has_flag(ctl.gutter(), LV_OBJ_FLAG_HIDDEN));
    // No reserved padding while hidden — content keeps full width.
    CHECK(lv_obj_get_style_pad_right(c, LV_PART_MAIN) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "PageScrollController restores container on detach",
                 "[page_scroll_buttons][ui]") {
    lv_obj_t* c = make_scroll_container(test_screen(), 20, 60);
    lv_scrollbar_mode_t original = lv_obj_get_scrollbar_mode(c);

    PageScrollController ctl;
    REQUIRE(ctl.attach(c));
    ctl.detach();
    process_lvgl(20); // flush lv_obj_delete_async

    CHECK(ctl.gutter() == nullptr);
    CHECK(lv_obj_get_style_pad_right(c, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_scrollbar_mode(c) == original);
}

TEST_CASE_METHOD(LVGLUITestFixture, "PageScrollController survives container deletion",
                 "[page_scroll_buttons][ui]") {
    lv_obj_t* c = make_scroll_container(test_screen(), 20, 60);

    // ctl must outlive the container deletion below so its destructor runs
    // (safely, since container_ is already null) after the container is gone.
    PageScrollController ctl;
    REQUIRE(ctl.attach(c));

    bool deleted_fired = false;
    ctl.set_on_container_deleted([&deleted_fired]() { deleted_fired = true; });

    // LV_EVENT_DELETE fires synchronously within lv_obj_delete, which invokes
    // container_event_cb -> on_container_deleted() before this call returns.
    lv_obj_delete(c);
    process_lvgl(20); // flush any async gutter deletion from LVGL's own teardown

    CHECK_FALSE(ctl.alive());
    CHECK(ctl.gutter() == nullptr);
    CHECK(deleted_fired);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PageScrollController survives gutter destruction via container repopulation",
                 "[page_scroll_buttons][ui]") {
    lv_obj_t* c = make_scroll_container(test_screen(), 20, 60);

    PageScrollController ctl;
    REQUIRE(ctl.attach(c));
    REQUIRE(ctl.gutter() != nullptr);
    // Container mutated by attach: native scrollbar suppressed while the gutter owns
    // the right strip.
    REQUIRE(lv_obj_get_scrollbar_mode(c) == LV_SCROLLBAR_MODE_OFF);

    bool pruned = false;
    ctl.set_on_container_deleted([&pruned]() { pruned = true; });

    // Repopulate: wipe the container's children (the gutter among them) while the
    // container object itself survives. This is the #1123 pattern — the gutter is a
    // child of the container, so lv_obj_clean() destroys it, but the container's own
    // LV_EVENT_DELETE never fires. Without a gutter-death subscription the controller
    // keeps a dangling gutter_ and the next scroll drives refresh_reach_state() into a
    // use-after-free (SIGBUS in lv_obj_get_screen while invalidating the freed gutter).
    lv_obj_clean(c);
    process_lvgl(20); // flush any async deletions

    // The controller must have noticed its gutter died and detached cleanly.
    CHECK(ctl.gutter() == nullptr); // no dangling gutter pointer
    CHECK(ctl.up_button() == nullptr);
    CHECK(ctl.down_button() == nullptr);
    CHECK_FALSE(ctl.alive()); // owner notified to prune + allow re-inject
    CHECK(pruned);
    // Surviving container restored to its pre-attach state.
    CHECK(lv_obj_get_style_pad_right(c, LV_PART_MAIN) == 0);
    CHECK(lv_obj_get_scrollbar_mode(c) != LV_SCROLLBAR_MODE_OFF);

    // Smoking gun: a scroll event after repopulation must not touch freed memory.
    lv_obj_send_event(c, LV_EVENT_SCROLL, nullptr);
    process_lvgl(20);
    CHECK(ctl.gutter() == nullptr);
}
