// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_spoolman_list_view.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// The only non-mirroring assertion from the deleted "constants are reasonable"
// case: the pool must outnumber the buffer rows above and below the viewport or
// virtualization has nothing left to show. Compile-time, so it costs nothing.
static_assert(SpoolmanListView::POOL_SIZE > SpoolmanListView::BUFFER_ROWS * 2,
              "pool must be larger than the buffer above + below the viewport");

// ============================================================================
// Unit Tests (LVGLTestFixture - minimal LVGL, no XML)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "SpoolmanListView - setup with null container",
                 "[spoolman_list_view]") {
    SpoolmanListView view;
    REQUIRE(view.setup(nullptr) == false);
    REQUIRE(view.is_initialized() == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "SpoolmanListView - setup with valid container",
                 "[spoolman_list_view]") {
    SpoolmanListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    REQUIRE(view.setup(container) == true);
    REQUIRE(view.container() == container);
}

TEST_CASE_METHOD(LVGLTestFixture, "SpoolmanListView - cleanup is safe to call twice",
                 "[spoolman_list_view]") {
    SpoolmanListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    view.setup(container);
    view.cleanup();
    view.cleanup(); // Should not crash
    REQUIRE(view.is_initialized() == false);
    REQUIRE(view.container() == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "SpoolmanListView - cleanup without setup",
                 "[spoolman_list_view]") {
    SpoolmanListView view;
    view.cleanup(); // Should not crash
    REQUIRE(view.is_initialized() == false);
}

// Deleted: four "should not crash" cases that asserted nothing AND could not
// reach anything. Under the plain LVGLTestFixture there is no XML registration,
// so lv_xml_create("spoolman_spool_row") fails, pool_ stays empty, and
// populate/update_visible/refresh_content/update_active_indicators all take the
// pool_.empty() early return (src/ui/ui_spoolman_list_view.cpp:295 and friends).
// The XML-backed cases below cover the same calls for real.

// ============================================================================
// Integration Tests (LVGLUITestFixture - full XML component registration)
// ============================================================================

static std::vector<SpoolInfo> make_test_spools(int count) {
    std::vector<SpoolInfo> spools;
    spools.reserve(count);
    for (int i = 0; i < count; i++) {
        SpoolInfo s;
        s.id = i + 1;
        s.vendor = "TestVendor";
        s.material = (i % 2 == 0) ? "PLA" : "PETG";
        s.filament_name = "Color " + std::to_string(i + 1);
        s.color_hex = "#808080";
        s.initial_weight_g = 1000.0;
        s.remaining_weight_g = 1000.0 - (i * 50.0);
        spools.push_back(s);
    }
    return spools;
}

TEST_CASE_METHOD(LVGLUITestFixture, "SpoolmanListView - populate creates pool rows",
                 "[spoolman_list_view][ui_integration]") {
    SpoolmanListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 600);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container);

    auto spools = make_test_spools(5);
    view.populate(spools, 1);
    process_lvgl(50);

    REQUIRE(view.is_initialized() == true);
}

TEST_CASE_METHOD(LVGLUITestFixture, "SpoolmanListView - populate with many spools",
                 "[spoolman_list_view][ui_integration]") {
    SpoolmanListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 600);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container);

    auto spools = make_test_spools(50);
    view.populate(spools, 1);
    process_lvgl(50);

    REQUIRE(view.is_initialized() == true);
    // Only POOL_SIZE rows should be created (not 50)
}

/// Find the visible pool row currently showing @p spool_id.
///
/// configure_row() stores the spool id in the row's user_data
/// (src/ui/ui_spoolman_list_view.cpp:174) — the same handle the XML click
/// callback reads — so this is the production lookup, not a test-only backdoor.
/// Returns nullptr when the id is outside the virtualized window.
static lv_obj_t* find_row_for_spool(lv_obj_t* container, int spool_id) {
    lv_obj_t* found = nullptr;
    const uint32_t count = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(container, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue; // recycled slot not currently showing anything
        }
        if (static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(child))) == spool_id) {
            REQUIRE(found == nullptr); // one visible row per spool, never two
            found = child;
        }
    }
    return found;
}

TEST_CASE_METHOD(LVGLUITestFixture, "SpoolmanListView - active indicator moves to the new spool",
                 "[spoolman_list_view][ui_integration]") {
    SpoolmanListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 600);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container);

    auto spools = make_test_spools(10);
    view.populate(spools, 1);
    process_lvgl(50);
    REQUIRE(view.is_initialized() == true);

    // Spool 1 owns the marker to begin with, so the assertions after the switch
    // also pin that the OLD row gets cleared rather than just the new one set.
    lv_obj_t* first_row = find_row_for_spool(container, 1);
    REQUIRE(first_row != nullptr);
    REQUIRE(lv_obj_has_state(first_row, LV_STATE_CHECKED));

    view.update_active_indicators(spools, 5);
    process_lvgl(50);

    lv_obj_t* active_row = find_row_for_spool(container, 5);
    REQUIRE(active_row != nullptr);
    lv_obj_t* active_marker = lv_obj_find_by_name(active_row, "active_indicator");
    REQUIRE(active_marker != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(active_marker, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_state(active_row, LV_STATE_CHECKED));

    for (int id : {1, 2, 3, 4, 6, 7, 8, 9, 10}) {
        INFO("spool id: " << id);
        lv_obj_t* row = find_row_for_spool(container, id);
        if (row == nullptr) {
            continue; // scrolled outside the virtualized window
        }
        lv_obj_t* marker = lv_obj_find_by_name(row, "active_indicator");
        REQUIRE(marker != nullptr);
        REQUIRE(lv_obj_has_flag(marker, LV_OBJ_FLAG_HIDDEN));
        REQUIRE_FALSE(lv_obj_has_state(row, LV_STATE_CHECKED));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "SpoolmanListView - refresh_content rewrites the row labels",
                 "[spoolman_list_view][ui_integration]") {
    SpoolmanListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 400, 600);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container);

    auto spools = make_test_spools(10);
    view.populate(spools, 1);
    process_lvgl(50);
    REQUIRE(view.is_initialized() == true);

    lv_obj_t* row = find_row_for_spool(container, 1);
    REQUIRE(row != nullptr);
    lv_obj_t* weight_label = lv_obj_find_by_name(row, "weight_text");
    REQUIRE(weight_label != nullptr);

    // Asserting the pre-refresh text is what makes the post-refresh assertion
    // mean something: it proves refresh_content changed the label rather than
    // the label having read "42g" all along.
    REQUIRE(std::string(lv_label_get_text(weight_label)) == "1000g");

    spools[0].remaining_weight_g = 42.0;
    view.refresh_content(spools, 1);
    process_lvgl(50);

    // "%.0fg" in configure_row (src/ui/ui_spoolman_list_view.cpp:211-215).
    // Rows are recycled in place, so the label pointer stays valid.
    REQUIRE(std::string(lv_label_get_text(weight_label)) == "42g");
}
