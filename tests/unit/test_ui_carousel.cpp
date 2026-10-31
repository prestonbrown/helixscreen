// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_carousel.cpp
 * @brief Unit tests for ui_carousel XML widget
 *
 * Tests carousel creation, state management, and tile management.
 */

#include "ui_carousel.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/widget_in_tree.h"

#include <vector>

#include "../catch_amalgamated.hpp"

// Needed to access lv_timer_t internals for direct callback invocation in tests
#include "lib/lvgl/src/misc/lv_timer_private.h"

// ============================================================================
// Task 2: Basic state and creation tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_get_state returns nullptr for nullptr",
                 "[carousel]") {
    REQUIRE(ui_carousel_get_state(nullptr) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_get_state returns nullptr for non-carousel object",
                 "[carousel]") {
    lv_obj_t* plain = lv_obj_create(test_screen());
    REQUIRE(plain != nullptr);
    REQUIRE(ui_carousel_get_state(plain) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "ui_carousel_get_state returns nullptr for object with wrong magic",
                 "[carousel]") {
    // Object with arbitrary user data should not be treated as carousel
    lv_obj_t* obj = lv_obj_create(test_screen());
    int dummy = 42;
    lv_obj_set_user_data(obj, &dummy);
    REQUIRE(ui_carousel_get_state(obj) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Carousel creation produces valid state with correct defaults",
                 "[carousel]") {
    // Manually create a carousel by allocating state (simulating what create callback does)
    lv_obj_t* container = lv_obj_create(test_screen());
    CarouselState* state = new CarouselState();
    state->scroll_container = lv_obj_create(container);
    state->indicator_row = lv_obj_create(container);
    lv_obj_set_user_data(container, state);

    CarouselState* retrieved = ui_carousel_get_state(container);
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->magic == CarouselState::kMagic);
    REQUIRE(retrieved->scroll_container != nullptr);
    REQUIRE(retrieved->indicator_row != nullptr);
    REQUIRE(retrieved->real_tiles.empty());
    REQUIRE(retrieved->current_page == 0);
    REQUIRE(retrieved->wrap == true);
    REQUIRE(retrieved->show_indicators == true);
    REQUIRE(retrieved->auto_scroll_ms == 0);
    REQUIRE(retrieved->auto_timer == nullptr);
    REQUIRE(retrieved->page_subject == nullptr);

    // Clean up manually since we didn't use the XML create path
    delete state;
    lv_obj_set_user_data(container, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Empty carousel has page_count=0 and current_page=0",
                 "[carousel]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    CarouselState* state = new CarouselState();
    state->scroll_container = lv_obj_create(container);
    lv_obj_set_user_data(container, state);

    REQUIRE(ui_carousel_get_page_count(container) == 0);
    REQUIRE(ui_carousel_get_current_page(container) == 0);

    delete state;
    lv_obj_set_user_data(container, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "get_page_count and get_current_page return 0 for non-carousel",
                 "[carousel]") {
    lv_obj_t* plain = lv_obj_create(test_screen());
    REQUIRE(ui_carousel_get_page_count(plain) == 0);
    REQUIRE(ui_carousel_get_current_page(plain) == 0);
}

// ============================================================================
// Task 3: Tile management tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_add_item creates tiles and increments page count",
                 "[carousel]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    CarouselState* state = new CarouselState();
    state->scroll_container = lv_obj_create(container);
    lv_obj_set_user_data(container, state);

    // Add first item
    lv_obj_t* item1 = lv_obj_create(test_screen());
    ui_carousel_add_item(container, item1);
    REQUIRE(ui_carousel_get_page_count(container) == 1);
    REQUIRE(state->real_tiles.size() == 1);

    // Verify item was reparented into the tile
    lv_obj_t* tile1 = state->real_tiles[0];
    REQUIRE(tile1 != nullptr);
    REQUIRE(lv_obj_get_parent(item1) == tile1);
    REQUIRE(lv_obj_get_parent(tile1) == state->scroll_container);

    delete state;
    lv_obj_set_user_data(container, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "Adding multiple items tracks correct count", "[carousel]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    CarouselState* state = new CarouselState();
    state->scroll_container = lv_obj_create(container);
    lv_obj_set_user_data(container, state);

    // Add three items
    lv_obj_t* items[3];
    for (int i = 0; i < 3; i++) {
        items[i] = lv_obj_create(test_screen());
        ui_carousel_add_item(container, items[i]);
    }

    REQUIRE(ui_carousel_get_page_count(container) == 3);
    REQUIRE(state->real_tiles.size() == 3);

    // Verify each item is in its own tile, and all tiles are in the scroll container
    for (int i = 0; i < 3; i++) {
        lv_obj_t* tile = state->real_tiles[i];
        REQUIRE(tile != nullptr);
        REQUIRE(lv_obj_get_parent(items[i]) == tile);
        REQUIRE(lv_obj_get_parent(tile) == state->scroll_container);
    }

    // Each tile should be distinct
    REQUIRE(state->real_tiles[0] != state->real_tiles[1]);
    REQUIRE(state->real_tiles[1] != state->real_tiles[2]);

    delete state;
    lv_obj_set_user_data(container, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_add_item ignores null carousel or item",
                 "[carousel]") {
    // Should not crash with nullptr carousel
    lv_obj_t* item = lv_obj_create(test_screen());
    ui_carousel_add_item(nullptr, item);

    // Should not crash with nullptr item
    lv_obj_t* container = lv_obj_create(test_screen());
    CarouselState* state = new CarouselState();
    state->scroll_container = lv_obj_create(container);
    lv_obj_set_user_data(container, state);

    ui_carousel_add_item(container, nullptr);
    REQUIRE(ui_carousel_get_page_count(container) == 0);

    delete state;
    lv_obj_set_user_data(container, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_add_item ignores non-carousel object",
                 "[carousel]") {
    lv_obj_t* plain = lv_obj_create(test_screen());
    lv_obj_t* item = lv_obj_create(test_screen());

    // Should not crash when called on a non-carousel object
    ui_carousel_add_item(plain, item);
    REQUIRE(ui_carousel_get_page_count(plain) == 0);
}

// ============================================================================
// Task 4: Page navigation tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Carousel page navigation", "[carousel]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    CarouselState* cs = new CarouselState();
    cs->wrap = false;
    cs->scroll_container = lv_obj_create(container);
    lv_obj_set_size(cs->scroll_container, 400, 280);
    lv_obj_set_user_data(container, cs);

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(container, lv_obj_create(test_screen()));
    }

    SECTION("goto_page sets current page") {
        ui_carousel_goto_page(container, 1, false);
        REQUIRE(ui_carousel_get_current_page(container) == 1);
        ui_carousel_goto_page(container, 2, false);
        REQUIRE(ui_carousel_get_current_page(container) == 2);
    }

    SECTION("goto_page clamps when wrap=false") {
        ui_carousel_goto_page(container, -1, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
        ui_carousel_goto_page(container, 99, false);
        REQUIRE(ui_carousel_get_current_page(container) == 2);
    }

    SECTION("goto_page wraps when wrap=true") {
        cs->wrap = true;
        ui_carousel_goto_page(container, 3, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
        ui_carousel_goto_page(container, -1, false);
        REQUIRE(ui_carousel_get_current_page(container) == 2);
    }

    delete cs;
    lv_obj_set_user_data(container, nullptr);
}

// ============================================================================
// Task 5: Indicator dot tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Carousel indicator dots", "[carousel]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    CarouselState* cs = new CarouselState();
    cs->wrap = false;
    cs->scroll_container = lv_obj_create(container);
    cs->indicator_row = lv_obj_create(container);
    lv_obj_set_flex_flow(cs->indicator_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_user_data(container, cs);

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(container, lv_obj_create(test_screen()));
    }

    SECTION("indicator row has correct number of dots") {
        REQUIRE(lv_obj_get_child_count(cs->indicator_row) == 3);
    }

    SECTION("first dot is active by default") {
        lv_obj_t* first_dot = lv_obj_get_child(cs->indicator_row, 0);
        REQUIRE(lv_obj_get_style_bg_opa(first_dot, LV_PART_MAIN) == LV_OPA_COVER);
    }

    SECTION("navigating updates active dot") {
        ui_carousel_goto_page(container, 1, false);
        lv_obj_t* dot0 = lv_obj_get_child(cs->indicator_row, 0);
        lv_obj_t* dot1 = lv_obj_get_child(cs->indicator_row, 1);
        REQUIRE(lv_obj_get_style_bg_opa(dot0, LV_PART_MAIN) < LV_OPA_COVER);
        REQUIRE(lv_obj_get_style_bg_opa(dot1, LV_PART_MAIN) == LV_OPA_COVER);
    }

    delete cs;
    lv_obj_set_user_data(container, nullptr);
}

// ============================================================================
// Task 6: Wrap-around behavior tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Carousel wrap-around behavior", "[carousel][wrap]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    CarouselState* cs = new CarouselState();
    cs->scroll_container = lv_obj_create(container);
    lv_obj_set_size(cs->scroll_container, 400, 280);
    cs->indicator_row = lv_obj_create(container);
    lv_obj_set_flex_flow(cs->indicator_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_user_data(container, cs);

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(container, lv_obj_create(test_screen()));
    }

    SECTION("wrap=true: forward past end wraps to start") {
        cs->wrap = true;
        ui_carousel_goto_page(container, 3, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
    }

    SECTION("wrap=true: backward past start wraps to end") {
        cs->wrap = true;
        ui_carousel_goto_page(container, -1, false);
        REQUIRE(ui_carousel_get_current_page(container) == 2);
    }

    SECTION("wrap=true: large positive index wraps correctly") {
        cs->wrap = true;
        ui_carousel_goto_page(container, 7, false); // 7 % 3 = 1
        REQUIRE(ui_carousel_get_current_page(container) == 1);
    }

    SECTION("wrap=true: large negative index wraps correctly") {
        cs->wrap = true;
        ui_carousel_goto_page(container, -4, false); // (-4 % 3 + 3) % 3 = 2
        REQUIRE(ui_carousel_get_current_page(container) == 2);
    }

    SECTION("wrap=false: clamps at end") {
        cs->wrap = false;
        ui_carousel_goto_page(container, 99, false);
        REQUIRE(ui_carousel_get_current_page(container) == 2);
    }

    SECTION("wrap=false: clamps at start") {
        cs->wrap = false;
        ui_carousel_goto_page(container, -5, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
    }

    delete cs;
    lv_obj_set_user_data(container, nullptr);
}

// ============================================================================
// Task 7: Auto-advance timer tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Carousel auto-advance timer", "[carousel][timer]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    CarouselState* cs = new CarouselState();
    cs->wrap = true;
    cs->auto_scroll_ms = 1000;
    cs->scroll_container = lv_obj_create(container);
    lv_obj_set_size(cs->scroll_container, 400, 280);
    cs->indicator_row = lv_obj_create(container);
    lv_obj_set_flex_flow(cs->indicator_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_user_data(container, cs);

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(container, lv_obj_create(test_screen()));
    }

    SECTION("start creates timer") {
        ui_carousel_start_auto_advance(container);
        REQUIRE(cs->auto_timer != nullptr);
        ui_carousel_stop_auto_advance(container);
    }

    SECTION("stop deletes timer") {
        ui_carousel_start_auto_advance(container);
        REQUIRE(cs->auto_timer != nullptr);
        ui_carousel_stop_auto_advance(container);
        REQUIRE(cs->auto_timer == nullptr);
    }

    SECTION("zero interval is no-op") {
        cs->auto_scroll_ms = 0;
        ui_carousel_start_auto_advance(container);
        REQUIRE(cs->auto_timer == nullptr);
    }

    SECTION("timer callback advances page") {
        ui_carousel_start_auto_advance(container);
        REQUIRE(cs->current_page == 0);
        // Manually invoke the timer callback to simulate a timer fire,
        // since lv_timer_handler_safe() skips repeating timers in tests
        REQUIRE(cs->auto_timer != nullptr);
        REQUIRE(cs->auto_timer->timer_cb != nullptr);
        cs->auto_timer->timer_cb(cs->auto_timer);
        REQUIRE(cs->current_page == 1);
        cs->auto_timer->timer_cb(cs->auto_timer);
        REQUIRE(cs->current_page == 2);
        ui_carousel_stop_auto_advance(container);
    }

    SECTION("timer skips advance when user is touching") {
        ui_carousel_start_auto_advance(container);
        cs->user_touching = true;
        REQUIRE(cs->auto_timer->timer_cb != nullptr);
        cs->auto_timer->timer_cb(cs->auto_timer);
        REQUIRE(cs->current_page == 0); // Should not advance
        cs->user_touching = false;
        cs->auto_timer->timer_cb(cs->auto_timer);
        REQUIRE(cs->current_page == 1); // Should advance now
        ui_carousel_stop_auto_advance(container);
    }

    delete cs;
    lv_obj_set_user_data(container, nullptr);
}

// ============================================================================
// Task 10: Edge case tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "Carousel edge cases", "[carousel][edge]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 300);
    CarouselState* cs = new CarouselState();
    cs->scroll_container = lv_obj_create(container);
    lv_obj_set_size(cs->scroll_container, 400, 280);
    cs->indicator_row = lv_obj_create(container);
    lv_obj_set_flex_flow(cs->indicator_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_user_data(container, cs);

    SECTION("empty carousel: goto_page is safe") {
        ui_carousel_goto_page(container, 0, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
        REQUIRE(ui_carousel_get_page_count(container) == 0);
    }

    SECTION("single item carousel") {
        ui_carousel_add_item(container, lv_obj_create(test_screen()));
        REQUIRE(ui_carousel_get_page_count(container) == 1);

        // Clamp should keep at 0
        cs->wrap = false;
        ui_carousel_goto_page(container, 1, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
    }

    SECTION("single item with wrap: goto_page wraps to 0") {
        ui_carousel_add_item(container, lv_obj_create(test_screen()));
        cs->wrap = true;
        ui_carousel_goto_page(container, 1, false);
        REQUIRE(ui_carousel_get_current_page(container) == 0);
    }

    SECTION("rebuild_indicators with no indicator_row is safe") {
        CarouselState* cs2 = new CarouselState();
        cs2->scroll_container = lv_obj_create(container);
        cs2->indicator_row = nullptr;
        lv_obj_t* cont2 = lv_obj_create(test_screen());
        lv_obj_set_user_data(cont2, cs2);

        ui_carousel_add_item(cont2, lv_obj_create(test_screen()));
        // Should not crash
        ui_carousel_rebuild_indicators(cont2);
        REQUIRE(ui_carousel_get_page_count(cont2) == 1);

        delete cs2;
        lv_obj_set_user_data(cont2, nullptr);
    }

    SECTION("auto-advance with zero interval is no-op") {
        cs->auto_scroll_ms = 0;
        ui_carousel_start_auto_advance(container);
        REQUIRE(cs->auto_timer == nullptr);
    }

    delete cs;
    lv_obj_set_user_data(container, nullptr);
}

// ============================================================================
// ui_carousel_create_obj tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_create_obj creates valid carousel",
                 "[carousel][create_obj]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    REQUIRE(carousel != nullptr);

    CarouselState* state = ui_carousel_get_state(carousel);
    REQUIRE(state != nullptr);
    REQUIRE(state->magic == CarouselState::kMagic);
    REQUIRE(state->scroll_container != nullptr);
    REQUIRE(state->indicator_row != nullptr);
    REQUIRE(state->real_tiles.empty());
    REQUIRE(state->current_page == 0);
    REQUIRE(state->real_page_count == -1);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_create_obj returns nullptr for null parent",
                 "[carousel][create_obj]") {
    REQUIRE(ui_carousel_create_obj(nullptr) == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui_carousel_create_obj carousel supports add_item and goto_page",
                 "[carousel][create_obj]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    REQUIRE(carousel != nullptr);

    // Add items and navigate
    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }
    REQUIRE(ui_carousel_get_page_count(carousel) == 3);

    CarouselState* state = ui_carousel_get_state(carousel);
    state->wrap = false;
    ui_carousel_goto_page(carousel, 2, false);
    REQUIRE(ui_carousel_get_current_page(carousel) == 2);
}

// ============================================================================
// set_real_page_count tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "set_real_page_count limits indicator dots",
                 "[carousel][real_page_count]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);

    // Add 5 tiles
    for (int i = 0; i < 5; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 5);

    // Limit indicator dots to 3
    ui_carousel_set_real_page_count(carousel, 3);
    REQUIRE(state->real_page_count == 3);
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 3);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_real_page_count clamps goto_page",
                 "[carousel][real_page_count]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);
    state->wrap = false;

    for (int i = 0; i < 5; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }

    ui_carousel_set_real_page_count(carousel, 3);

    // Should clamp to page 2 (0-based, count=3)
    ui_carousel_goto_page(carousel, 4, false);
    REQUIRE(ui_carousel_get_current_page(carousel) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_real_page_count -1 reverts to tile count",
                 "[carousel][real_page_count]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);

    for (int i = 0; i < 4; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }

    ui_carousel_set_real_page_count(carousel, 2);
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 2);

    ui_carousel_set_real_page_count(carousel, -1);
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 4);
}

TEST_CASE_METHOD(LVGLTestFixture, "set_real_page_count on non-carousel is safe",
                 "[carousel][real_page_count]") {
    // user_data that is not a carousel's state is left alone.
    CarouselState foreign;
    foreign.magic = 0;
    lv_obj_t* plain = lv_obj_create(test_screen());
    lv_obj_set_user_data(plain, &foreign);

    ui_carousel_set_real_page_count(plain, 3);
    REQUIRE(foreign.real_page_count == -1);

    lv_obj_set_user_data(plain, nullptr);
}

// ============================================================================
// remove_item tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "remove_item removes tile and adjusts state",
                 "[carousel][remove_item]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }
    REQUIRE(ui_carousel_get_page_count(carousel) == 3);

    // Remove middle item
    ui_carousel_remove_item(carousel, 1);
    REQUIRE(ui_carousel_get_page_count(carousel) == 2);
    REQUIRE(state->real_tiles.size() == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "remove_item adjusts current_page when removing last page",
                 "[carousel][remove_item]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);
    state->wrap = false;

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }

    // Navigate to last page
    ui_carousel_goto_page(carousel, 2, false);
    REQUIRE(state->current_page == 2);

    // Remove last page — current_page should adjust to new last
    ui_carousel_remove_item(carousel, 2);
    REQUIRE(state->current_page == 1);
    REQUIRE(ui_carousel_get_page_count(carousel) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "remove_item updates page_subject", "[carousel][remove_item]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);
    state->wrap = false;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);
    state->page_subject = &subject;

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }

    ui_carousel_goto_page(carousel, 2, false);
    REQUIRE(lv_subject_get_int(&subject) == 2);

    // Remove last page — subject should update
    ui_carousel_remove_item(carousel, 2);
    REQUIRE(lv_subject_get_int(&subject) == 1);

    lv_subject_deinit(&subject);
    state->page_subject = nullptr;
}

TEST_CASE_METHOD(LVGLTestFixture, "remove_item rebuilds indicators", "[carousel][remove_item]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);

    for (int i = 0; i < 3; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    }
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 3);

    ui_carousel_remove_item(carousel, 0);
    REQUIRE(lv_obj_get_child_count(state->indicator_row) == 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "remove_item with out-of-range index is safe",
                 "[carousel][remove_item]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());

    ui_carousel_add_item(carousel, lv_obj_create(test_screen()));

    // Should not crash
    ui_carousel_remove_item(carousel, -1);
    ui_carousel_remove_item(carousel, 5);
    REQUIRE(ui_carousel_get_page_count(carousel) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "remove_item on non-carousel is safe",
                 "[carousel][remove_item]") {
    // user_data that is not a carousel's state is left alone.
    CarouselState foreign;
    foreign.magic = 0;
    lv_obj_t* plain = lv_obj_create(test_screen());
    lv_obj_t* tile = lv_obj_create(plain);
    foreign.real_tiles.push_back(tile);
    lv_obj_set_user_data(plain, &foreign);

    ui_carousel_remove_item(plain, 0);
    REQUIRE(foreign.real_tiles.size() == 1);
    REQUIRE(helix::test::widget_in_tree(tile));

    lv_obj_set_user_data(plain, nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "remove_item removing all items leaves empty carousel",
                 "[carousel][remove_item]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());
    CarouselState* state = ui_carousel_get_state(carousel);

    ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
    ui_carousel_remove_item(carousel, 0);
    REQUIRE(ui_carousel_get_page_count(carousel) == 0);
    REQUIRE(state->current_page == 0);
}

// ============================================================================
// set_bubble_events tests
// ============================================================================

namespace {

int g_released_count = 0;

void count_released(lv_event_t* /*e*/) {
    ++g_released_count;
}

/// True when a RELEASED sent to @p page reaches the object count_released is
/// registered on.
bool reaches_counter(lv_obj_t* page) {
    const int before = g_released_count;
    lv_obj_send_event(page, LV_EVENT_RELEASED, nullptr);
    return g_released_count == before + 1;
}

lv_obj_t* add_bubbling_page(lv_obj_t* carousel, lv_obj_t* screen) {
    lv_obj_t* page = lv_obj_create(screen);
    lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);
    ui_carousel_add_item(carousel, page);
    return page;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a bubbling carousel routes page events to its parent at every page count",
                 "[carousel][bubble]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_add_event_cb(parent, count_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_t* carousel = ui_carousel_create_obj(parent);
    helix::ui::carousel_set_bubble_events(carousel, true);

    lv_obj_t* first = add_bubbling_page(carousel, test_screen());
    ui_carousel_set_real_page_count(carousel, 1);
    CHECK(reaches_counter(first));

    // A second page switches the tiles to capturing input for the swipe; the
    // setting holds through that switch.
    lv_obj_t* second = add_bubbling_page(carousel, test_screen());
    ui_carousel_set_real_page_count(carousel, 2);
    CHECK(reaches_counter(first));
    CHECK(reaches_counter(second));

    // A page added after the count was set bubbles from add_item alone, before
    // any count update; then a removal rebuilds too.
    lv_obj_t* third = add_bubbling_page(carousel, test_screen());
    CHECK(reaches_counter(third));
    ui_carousel_set_real_page_count(carousel, 3);
    CHECK(reaches_counter(third));
    ui_carousel_remove_item(carousel, 2);
    CHECK(reaches_counter(first));
    CHECK(reaches_counter(second));
}

TEST_CASE_METHOD(LVGLTestFixture, "a default multi-page carousel keeps page events to itself",
                 "[carousel][bubble]") {
    lv_obj_t* carousel = ui_carousel_create_obj(test_screen());

    lv_obj_t* first = add_bubbling_page(carousel, test_screen());
    add_bubbling_page(carousel, test_screen());
    ui_carousel_set_real_page_count(carousel, 2);
    // Counted on the scroll container, the tile's own parent: the carousel
    // object never bubbles by default, so a count further up could not tell
    // whether the tile kept the event.
    lv_obj_add_event_cb(ui_carousel_get_state(carousel)->scroll_container, count_released,
                        LV_EVENT_RELEASED, nullptr);
    CHECK_FALSE(reaches_counter(first));
}

// ============================================================================
// Swipe policy tests
// ============================================================================

namespace {

lv_obj_t* carousel_with_pages(lv_obj_t* screen, int pages) {
    lv_obj_t* carousel = ui_carousel_create_obj(screen);
    for (int i = 0; i < pages; i++) {
        ui_carousel_add_item(carousel, lv_obj_create(screen));
    }
    return carousel;
}

/// Swipeable: the scroll container scrolls, horizontally.
bool swipe_live(lv_obj_t* carousel) {
    lv_obj_t* scroll = ui_carousel_get_state(carousel)->scroll_container;
    return lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE) &&
           lv_obj_get_scroll_dir(scroll) == LV_DIR_HOR;
}

/// Not swipeable: the scroll container neither scrolls nor has a direction.
bool swipe_off(lv_obj_t* carousel) {
    lv_obj_t* scroll = ui_carousel_get_state(carousel)->scroll_container;
    return !lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE) &&
           lv_obj_get_scroll_dir(scroll) == LV_DIR_NONE;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "the swipe policy decides swiping at one page and at two",
                 "[carousel][swipe]") {
    using helix::ui::CarouselSwipe;
    struct Row {
        CarouselSwipe policy;
        int pages;
        bool swipes;
    };
    const Row rows[] = {
        {CarouselSwipe::Auto, 1, false},
        {CarouselSwipe::Auto, 2, true},
        {CarouselSwipe::Disabled, 1, false},
        {CarouselSwipe::Disabled, 2, false},
    };
    for (const Row& row : rows) {
        CAPTURE(static_cast<int>(row.policy), row.pages);
        lv_obj_t* carousel = carousel_with_pages(test_screen(), row.pages);
        helix::ui::carousel_set_swipe(carousel, row.policy);
        CHECK(swipe_live(carousel) == row.swipes);
        CHECK(swipe_off(carousel) == !row.swipes);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "changing the swipe or bubbling leaves the indicator dots in place",
                 "[carousel][swipe]") {
    using helix::ui::CarouselSwipe;
    lv_obj_t* carousel = carousel_with_pages(test_screen(), 2);
    CarouselState* state = ui_carousel_get_state(carousel);

    auto dots = [state]() {
        std::vector<lv_obj_t*> children;
        for (uint32_t i = 0; i < lv_obj_get_child_count(state->indicator_row); i++) {
            children.push_back(lv_obj_get_child(state->indicator_row, static_cast<int32_t>(i)));
        }
        return children;
    };
    const std::vector<lv_obj_t*> original = dots();
    REQUIRE(original.size() == 2);

    // These run inside input dispatch whenever an edit gesture locks or
    // releases the swipe, so they write flags and nothing else.
    helix::ui::carousel_set_swipe(carousel, CarouselSwipe::Disabled);
    REQUIRE(swipe_off(carousel));
    CHECK(dots() == original);

    helix::ui::carousel_set_swipe(carousel, CarouselSwipe::Auto);
    REQUIRE(swipe_live(carousel));
    CHECK(dots() == original);

    helix::ui::carousel_set_bubble_events(carousel, true);
    REQUIRE(lv_obj_has_flag(state->real_tiles[0], LV_OBJ_FLAG_EVENT_BUBBLE));
    CHECK(dots() == original);
}

TEST_CASE_METHOD(LVGLTestFixture, "the swipe policy holds through page-count changes",
                 "[carousel][swipe]") {
    using helix::ui::CarouselSwipe;

    SECTION("Auto, the default, follows the reachable tiles") {
        lv_obj_t* carousel = carousel_with_pages(test_screen(), 0);
        CHECK(swipe_off(carousel));
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
        CHECK(swipe_off(carousel));
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
        CHECK(swipe_live(carousel));
        // A tile past the page count stays swipeable while it is reachable: the
        // home panel's next-page slot rides on one.
        ui_carousel_set_real_page_count(carousel, 1);
        CHECK(swipe_live(carousel));
        helix::ui::carousel_set_trailing_tiles_reachable(carousel, false);
        CHECK(swipe_off(carousel));
    }

    SECTION("Disabled keeps a carousel still past one page") {
        lv_obj_t* carousel = carousel_with_pages(test_screen(), 1);
        helix::ui::carousel_set_swipe(carousel, CarouselSwipe::Disabled);
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
        CHECK(swipe_off(carousel));
        ui_carousel_set_real_page_count(carousel, 2);
        CHECK(swipe_off(carousel));
        ui_carousel_add_item(carousel, lv_obj_create(test_screen()));
        ui_carousel_set_real_page_count(carousel, -1);
        ui_carousel_remove_item(carousel, 0);
        CHECK(swipe_off(carousel));
    }
}

// ============================================================================
// Page tracking across programmatic and settled scrolls
// ============================================================================

namespace {

void record_published(lv_observer_t* observer, lv_subject_t* subject) {
    auto* published = static_cast<std::vector<int>*>(lv_observer_get_user_data(observer));
    published->push_back(lv_subject_get_int(subject));
}

/// A four-tile carousel filling the test screen, with a page subject that
/// records every value published to it.
struct RecordedCarousel {
    lv_obj_t* carousel = nullptr;
    CarouselState* state = nullptr;
    lv_subject_t subject{};
    std::vector<int> published;
    int32_t page_w = 0;

    explicit RecordedCarousel(lv_obj_t* screen) {
        carousel = ui_carousel_create_obj(screen);
        state = ui_carousel_get_state(carousel);
        REQUIRE(state != nullptr);
        state->wrap = false;
        for (int i = 0; i < 4; i++) {
            ui_carousel_add_item(carousel, lv_obj_create(screen));
        }
        lv_obj_update_layout(carousel);
        page_w = lv_obj_get_content_width(state->scroll_container);
        REQUIRE(page_w > 0);

        lv_subject_init_int(&subject, 0);
        state->page_subject = &subject;
        lv_subject_add_observer(&subject, record_published, &published);
        published.clear(); // adding the observer reports the current value once
    }

    ~RecordedCarousel() {
        state->page_subject = nullptr;
        lv_subject_deinit(&subject);
    }

    RecordedCarousel(const RecordedCarousel&) = delete;
    RecordedCarousel& operator=(const RecordedCarousel&) = delete;

    int32_t scroll_x() const {
        return lv_obj_get_scroll_x(state->scroll_container);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "an animated goto replacing one in flight publishes only the pages it set",
                 "[carousel][goto]") {
    RecordedCarousel rc(test_screen());

    ui_carousel_goto_page(rc.carousel, 3, true);
    for (int step = 0; step < 100 && rc.scroll_x() < rc.page_w; ++step) {
        process_lvgl(5);
    }
    // In flight over page 1: the offset the replaced animation reports belongs
    // to neither the page it was heading for nor the page the next goto sets.
    REQUIRE(rc.scroll_x() >= rc.page_w);
    REQUIRE(rc.scroll_x() < 2 * rc.page_w);

    ui_carousel_goto_page(rc.carousel, 0, true);
    process_lvgl(600); // the second animation runs out and sends its own SCROLL_END

    CHECK(rc.published == std::vector<int>{3, 0});
    CHECK(ui_carousel_get_current_page(rc.carousel) == 0);
    CHECK(rc.scroll_x() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a swipe that interrupts an animated goto still settles the page",
                 "[carousel][goto]") {
    RecordedCarousel rc(test_screen());

    ui_carousel_goto_page(rc.carousel, 3, true);
    for (int step = 0; step < 100 && rc.scroll_x() < rc.page_w; ++step) {
        process_lvgl(5);
    }
    REQUIRE(rc.scroll_x() >= rc.page_w); // the animation is in flight

    // A drag taking over the scroll container: LVGL's indev stops the running
    // scroll animation, then the swipe moves the offset and ends with its own
    // SCROLL_END. Neither passes through a carousel goto.
    lv_obj_stop_scroll_anim(rc.state->scroll_container);
    lv_obj_scroll_to_x(rc.state->scroll_container, 2 * rc.page_w, LV_ANIM_OFF);

    CHECK(ui_carousel_get_current_page(rc.carousel) == 2);
    REQUIRE_FALSE(rc.published.empty());
    CHECK(rc.published.back() == 2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "a scroll settling beside a page boundary reports the nearest page",
                 "[carousel][scroll_end]") {
    RecordedCarousel rc(test_screen());
    lv_obj_t* scroll = rc.state->scroll_container;

    // A few px short of page 2 is page 2...
    lv_obj_scroll_to_x(scroll, 2 * rc.page_w - 3, LV_ANIM_OFF);
    REQUIRE(rc.scroll_x() == 2 * rc.page_w - 3);
    CHECK(ui_carousel_get_current_page(rc.carousel) == 2);
    CHECK(lv_subject_get_int(&rc.subject) == 2);

    // ...and a few px past page 1 is page 1.
    lv_obj_scroll_to_x(scroll, rc.page_w + 3, LV_ANIM_OFF);
    REQUIRE(rc.scroll_x() == rc.page_w + 3);
    CHECK(ui_carousel_get_current_page(rc.carousel) == 1);
    CHECK(lv_subject_get_int(&rc.subject) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "goto_tile reaches the tile past the last page while goto_page stops at it",
                 "[carousel][goto]") {
    RecordedCarousel rc(test_screen());
    ui_carousel_set_real_page_count(rc.carousel, 3); // the fourth tile is past the last page

    SECTION("without wrap, each clamps to its own count") {
        helix::ui::carousel_goto_tile(rc.carousel, 3, false);
        CHECK(ui_carousel_get_current_page(rc.carousel) == 3);
        CHECK(lv_subject_get_int(&rc.subject) == 3);
        CHECK(rc.scroll_x() == 3 * rc.page_w);

        ui_carousel_goto_page(rc.carousel, 3, false);
        CHECK(ui_carousel_get_current_page(rc.carousel) == 2);
        CHECK(lv_subject_get_int(&rc.subject) == 2);
        CHECK(rc.scroll_x() == 2 * rc.page_w);

        helix::ui::carousel_goto_tile(rc.carousel, 9, false);
        CHECK(ui_carousel_get_current_page(rc.carousel) == 3);
    }

    SECTION("with wrap, each wraps over its own count") {
        rc.state->wrap = true;
        helix::ui::carousel_goto_tile(rc.carousel, 5, false); // 5 wraps over 4 tiles
        CHECK(ui_carousel_get_current_page(rc.carousel) == 1);
        ui_carousel_goto_page(rc.carousel, 5, false); // 5 wraps over 3 pages
        CHECK(ui_carousel_get_current_page(rc.carousel) == 2);
        helix::ui::carousel_goto_tile(rc.carousel, -1, false);
        CHECK(ui_carousel_get_current_page(rc.carousel) == 3);
    }
}

TEST_CASE_METHOD(
    LVGLTestFixture,
    "a tile past the last page out of reach takes no goto and leaves the swipe no room",
    "[carousel][goto]") {
    RecordedCarousel rc(test_screen());
    ui_carousel_set_real_page_count(rc.carousel, 3); // the fourth tile is past the last page
    lv_obj_t* scroll = rc.state->scroll_container;

    helix::ui::carousel_set_trailing_tiles_reachable(rc.carousel, false);
    helix::ui::carousel_goto_tile(rc.carousel, 3, false);
    CHECK(ui_carousel_get_current_page(rc.carousel) == 2);
    CHECK(lv_subject_get_int(&rc.subject) == 2);
    CHECK(rc.scroll_x() == 2 * rc.page_w);
    // On the last page, a swipe toward the next tile has nothing left to scroll.
    lv_obj_update_layout(rc.carousel);
    CHECK(lv_obj_get_scroll_right(scroll) <= 0);

    // Back in reach, the tile is a goto away again.
    helix::ui::carousel_set_trailing_tiles_reachable(rc.carousel, true);
    lv_obj_update_layout(rc.carousel);
    CHECK(lv_obj_get_scroll_right(scroll) == rc.page_w);
    helix::ui::carousel_goto_tile(rc.carousel, 3, false);
    CHECK(ui_carousel_get_current_page(rc.carousel) == 3);
    CHECK(rc.scroll_x() == 3 * rc.page_w);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "taking the tiles past the last page out of reach moves no page and stops no "
                 "slide",
                 "[carousel][goto]") {
    RecordedCarousel rc(test_screen());
    ui_carousel_set_real_page_count(rc.carousel, 3); // the fourth tile is past the last page
    lv_obj_t* scroll = rc.state->scroll_container;

    // On the tile past the last page, a goto starts the slide back to a page,
    // and the tiles past the pages go out of reach under it.
    helix::ui::carousel_goto_tile(rc.carousel, 3, false);
    ui_carousel_goto_page(rc.carousel, 1, /*animate=*/true);
    REQUIRE(lv_anim_get(scroll, nullptr) != nullptr);
    rc.published.clear();

    helix::ui::carousel_set_trailing_tiles_reachable(rc.carousel, false);

    // Flags only: the tile hides, the page stands with nothing published, and
    // the slide runs on to its page.
    CHECK(lv_obj_has_flag(rc.state->real_tiles[3], LV_OBJ_FLAG_HIDDEN));
    CHECK(ui_carousel_get_current_page(rc.carousel) == 1);
    CHECK(rc.published.empty());
    CHECK(lv_anim_get(scroll, nullptr) != nullptr);
    for (int step = 0; step < 100 && lv_anim_get(scroll, nullptr) != nullptr; ++step) {
        process_lvgl(20);
    }
    REQUIRE(lv_anim_get(scroll, nullptr) == nullptr);
    CHECK(ui_carousel_get_current_page(rc.carousel) == 1);
    CHECK(rc.scroll_x() == rc.page_w);
}
