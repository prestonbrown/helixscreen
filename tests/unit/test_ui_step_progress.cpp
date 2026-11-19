// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_step_progress.cpp
 * @brief Rendering tests for the step-progress widget's connector lines.
 *
 * The vertical connector lines between step circles are absolutely-positioned
 * IGNORE_LAYOUT rects sized from a layout pass. When the widget is created while
 * an ancestor is hidden, the layout is degenerate (0-size), and the connectors
 * must not be skipped permanently. These tests pin the two contracts:
 *   1. connectors exist with a positive extent when created while visible, and
 *   2. connectors are (re)created + sized once the widget becomes visible, even
 *      if it was built while hidden — driven by an LV_EVENT_SIZE_CHANGED reflow.
 */

#include "ui_step_progress.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/lvgl.h"

#include <array>
#include <cstdlib>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

// A 4-step vertical model → 3 connectors between the circles.
std::array<ui_step_t, 4> make_vertical_steps() {
    return {{
        {"Heat nozzle", helix::StepState::Pending},
        {"Feed filament", helix::StepState::Pending},
        {"Purge", helix::StepState::Pending},
        {"Clean", helix::StepState::Pending},
    }};
}

lv_obj_t* make_sized_parent(lv_obj_t* screen) {
    lv_obj_t* parent = lv_obj_create(screen);
    lv_obj_set_size(parent, 300, 400);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    return parent;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "step progress creates vertical connectors when visible",
                 "[ui][step]") {
    lv_obj_t* parent = make_sized_parent(test_screen());

    auto steps = make_vertical_steps();
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/false, nullptr);
    REQUIRE(widget != nullptr);

    lv_obj_update_layout(parent);

    for (int i = 0; i < static_cast<int>(steps.size()) - 1; i++) {
        lv_obj_t* conn = ui_step_progress_test_get_connector(widget, i);
        INFO("connector " << i);
        REQUIRE(conn != nullptr);
        CHECK(lv_obj_get_height(conn) > 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "step progress recomputes connectors on reflow (SIZE_CHANGED)",
                 "[ui][step]") {
    // Connectors are absolutely-positioned rects sized from the layout. When the
    // widget reflows — a becoming-visible container or a growing live-temp label
    // shifting the circles — the connectors must be recomputed, not frozen at
    // their create-time geometry. A horizontal bar makes this deterministic: the
    // step items flex-grow to fill the width, so circle spacing (and therefore
    // connector width) tracks the container width directly.
    lv_obj_t* parent = make_sized_parent(test_screen());
    lv_obj_set_width(parent, 600);

    auto steps = make_vertical_steps(); // labels/states reused; horizontal layout
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/true, nullptr);
    REQUIRE(widget != nullptr);
    lv_obj_update_layout(parent);

    lv_obj_t* conn0_wide = ui_step_progress_test_get_connector(widget, 0);
    REQUIRE(conn0_wide != nullptr);
    const int32_t wide_w = lv_obj_get_width(conn0_wide);
    CHECK(wide_w > 0);

    // Halve the available width and drive a reflow, as an unhide/resize would.
    lv_obj_set_width(parent, 300);
    lv_obj_update_layout(parent);
    lv_obj_send_event(widget, LV_EVENT_SIZE_CHANGED, nullptr);
    lv_obj_update_layout(parent);

    lv_obj_t* conn0_narrow = ui_step_progress_test_get_connector(widget, 0);
    REQUIRE(conn0_narrow != nullptr);
    const int32_t narrow_w = lv_obj_get_width(conn0_narrow);

    INFO("wide_w=" << wide_w << " narrow_w=" << narrow_w);
    // A recomputed connector must shrink with the container; a frozen one keeps
    // its create-time width and overshoots the narrower layout.
    CHECK(narrow_w < wide_w);
    CHECK(narrow_w > 0);
}

TEST_CASE("step progress caption: the active step, else the last done, else the first",
          "[ui][step][step_caption]") {
    using helix::StepState;
    const StepState active[] = {StepState::Completed, StepState::Active, StepState::Pending};
    CHECK(ui_step_progress_caption_step(active, 3) == 1);
    const StepState done[] = {StepState::Completed, StepState::Completed, StepState::Completed};
    CHECK(ui_step_progress_caption_step(done, 3) == 2);
    const StepState fresh[] = {StepState::Pending, StepState::Pending, StepState::Pending};
    CHECK(ui_step_progress_caption_step(fresh, 3) == 0);
    // Steps completed out of order, nothing active: the last one done.
    const StepState partial[] = {StepState::Completed, StepState::Pending, StepState::Completed};
    CHECK(ui_step_progress_caption_step(partial, 3) == 2);
    CHECK(ui_step_progress_caption_step(nullptr, 0) == -1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "step progress current-label-only mode: one centred caption, no per-step labels",
                 "[ui][step][step_caption]") {
    lv_obj_t* parent = make_sized_parent(test_screen());
    lv_obj_set_width(parent, 264);
    auto steps = make_vertical_steps();
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/true, nullptr);
    REQUIRE(widget != nullptr);
    ui_step_progress_set_current_label_only(widget, true);
    ui_step_progress_set_current(widget, 2);
    lv_obj_update_layout(parent);

    // Every step's own label is hidden; the caption names the current step.
    for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
        if (lv_obj_get_child_count(item) < 2)
            continue; // a connector
        CHECK(lv_obj_is_hidden(lv_obj_get_child(item, 1)));
    }
    lv_obj_t* caption = ui_step_progress_get_caption(widget);
    REQUIRE(caption != nullptr);
    CHECK(std::string(lv_label_get_text(caption)) == "Purge");
    lv_area_t ca, wa;
    lv_obj_get_coords(caption, &ca);
    lv_obj_get_coords(widget, &wa);
    CHECK(ca.y1 >= wa.y1);
    CHECK(ca.y2 <= wa.y2);
    CHECK(std::abs((ca.x1 + ca.x2) / 2 - (wa.x1 + wa.x2) / 2) <= 1);
    // The caption stands under the row of dots, not over it.
    for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
        if (item == caption || lv_obj_get_child_count(item) < 2)
            continue;
        lv_area_t ia;
        lv_obj_get_coords(lv_obj_get_child(item, 0), &ia);
        CHECK(ca.y1 > ia.y2);
    }

    // A relabel of the current step reaches the caption; another step's does not.
    ui_step_progress_set_label(widget, 2, "Purge 40 mm");
    CHECK(std::string(lv_label_get_text(caption)) == "Purge 40 mm");
    ui_step_progress_set_label(widget, 0, "Heat nozzle 220");
    CHECK(std::string(lv_label_get_text(caption)) == "Purge 40 mm");

    ui_step_progress_set_current(widget, 3);
    CHECK(std::string(lv_label_get_text(caption)) == "Clean");
}

TEST_CASE_METHOD(LVGLTestFixture, "step progress horizontal: the row of dots is centred",
                 "[ui][step][step_caption]") {
    const char* names[] = {"Heat nozzle", "Cut & retract", "Feed filament", "Purge"};
    for (int n = 2; n <= 4; ++n) {
        for (bool caption_only : {true, false}) {
            CAPTURE(n, caption_only);
            lv_obj_t* parent = make_sized_parent(test_screen());
            lv_obj_set_width(parent, 264);
            std::vector<ui_step_t> steps;
            for (int i = 0; i < n; ++i)
                steps.push_back({names[i], helix::StepState::Pending});
            lv_obj_t* widget =
                ui_step_progress_create(parent, steps.data(), n, /*horizontal=*/true, nullptr);
            REQUIRE(widget != nullptr);
            if (caption_only)
                ui_step_progress_set_current_label_only(widget, true);
            ui_step_progress_set_current(widget, 1);
            lv_obj_update_layout(parent);

            std::vector<lv_area_t> circles;
            for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
                lv_obj_t* item = lv_obj_get_child(widget, (int32_t)i);
                if (lv_obj_get_child_count(item) < 2)
                    continue; // a connector or the caption
                lv_area_t a;
                lv_obj_get_coords(lv_obj_get_child(lv_obj_get_child(item, 0), 0), &a);
                circles.push_back(a);
            }
            REQUIRE(circles.size() == static_cast<size_t>(n));
            lv_area_t wa;
            lv_obj_get_coords(widget, &wa);
            const int32_t left_gap = circles.front().x1 - wa.x1;
            const int32_t right_gap = wa.x2 - circles.back().x2;
            CAPTURE(wa.x1, wa.x2, circles.front().x1, circles.back().x2);
            CHECK(left_gap > 0);
            CHECK(std::abs(left_gap - right_gap) <= 1);
            // The connectors still join neighbouring dots.
            for (int i = 0; i + 1 < n; ++i) {
                lv_obj_t* conn = ui_step_progress_test_get_connector(widget, i);
                REQUIRE(conn != nullptr);
                lv_area_t ca;
                lv_obj_get_coords(conn, &ca);
                CHECK(std::abs(ca.x1 - circles[i].x2) <= 1);
                CHECK(std::abs(ca.x2 - circles[i + 1].x1) <= 1);
            }
            lv_obj_delete(parent);
        }
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "step progress: a list taller than its scrolling parent follows the current step",
                 "[ui][step]") {
    // The AMS sidebar's stepper container: a short box that scrolls, holding a
    // content-sized bar of many steps (a six-tool run is 19).
    lv_obj_t* parent = make_sized_parent(test_screen());
    lv_obj_set_height(parent, 120);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    std::vector<std::string> names;
    std::vector<ui_step_t> steps;
    for (int i = 0; i < 19; ++i) {
        names.push_back("Step " + std::to_string(i + 1));
    }
    for (const auto& n : names) {
        steps.push_back({n.c_str(), helix::StepState::Pending});
    }
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/false, nullptr);
    REQUIRE(widget != nullptr);
    lv_obj_update_layout(parent);
    REQUIRE(lv_obj_get_scroll_bottom(parent) > 0); // the bar overflows its parent

    // One scroller: a drag on the bar moves the parent, never rows inside the
    // bar's own box, which would open a gap below the last row.
    CHECK_FALSE(lv_obj_is_scrollable(widget));

    ui_step_progress_set_current(widget, 15);
    process_lvgl(1000);
    CHECK(lv_obj_get_scroll_y(parent) > 0);
    // Step 16 is on screen.
    lv_obj_t* current = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(widget); ++i) {
        lv_obj_t* item = lv_obj_get_child(widget, static_cast<int32_t>(i));
        if (lv_obj_get_child_count(item) >= 2 &&
            std::string(lv_label_get_text(lv_obj_get_child(item, 1))) == "Step 16") {
            current = item;
        }
    }
    REQUIRE(current != nullptr);
    lv_area_t row;
    lv_area_t box;
    lv_obj_get_coords(current, &row);
    lv_obj_get_coords(parent, &box);
    CHECK(row.y1 >= box.y1);
    CHECK(row.y2 <= box.y2);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "step progress horizontal: a row wider than its column scrolls itself",
                 "[ui][step]") {
    // The sidebar's stepper container scrolls only vertically, so a horizontal
    // row of dots has no other scroller.
    lv_obj_t* parent = make_sized_parent(test_screen());
    auto steps = make_vertical_steps();
    lv_obj_t* widget = ui_step_progress_create(parent, steps.data(), static_cast<int>(steps.size()),
                                               /*horizontal=*/true, nullptr);
    REQUIRE(widget != nullptr);
    CHECK(lv_obj_is_scrollable(widget));
}
