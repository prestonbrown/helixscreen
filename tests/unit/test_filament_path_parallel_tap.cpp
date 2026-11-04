// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_path_parallel_tap.cpp
 * @brief A real tap on a PARALLEL path canvas reaches the toolhead callback
 *        only on a drawn head glyph, and the slot callback on its tube.
 *
 * Taps go through LVGL's input pipeline (UITest::click_at), so the routing in
 * the canvas's click handler is what is under test, at points taken from the
 * geometry the canvas draws from.
 */

#include "ui_filament_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_types.h"
#include "lvgl/lvgl.h"
#include "settings_manager.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "src/ui/filament/ui_filament_path_plan.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::fpath;

namespace {

struct Taps {
    std::vector<int> toolheads;
    std::vector<int> slots;
};

void on_toolhead(int slot, void* user_data) {
    static_cast<Taps*>(user_data)->toolheads.push_back(slot);
}
void on_slot(int slot, void* user_data) {
    static_cast<Taps*>(user_data)->slots.push_back(slot);
}

/// Sets the toolhead style for one case and restores the previous one.
struct ScopedToolheadStyle {
    helix::ToolheadStyle previous;
    explicit ScopedToolheadStyle(helix::ToolheadStyle style)
        : previous(prepared().get_toolhead_style()) {
        helix::SettingsManager::instance().set_toolhead_style(style);
    }
    // The style lives in a subject; without one, set_toolhead_style() stores nothing.
    static helix::SettingsManager& prepared() {
        helix::SettingsManager::instance().init_subjects();
        return helix::SettingsManager::instance();
    }
    ~ScopedToolheadStyle() {
        helix::SettingsManager::instance().set_toolhead_style(previous);
    }
};

struct ParallelTapFixture : public LVGLTestFixture {
    ParallelTapFixture() {
        UITest::init(test_screen());
        canvas = ui_filament_path_canvas_create(test_screen());
        lv_obj_set_pos(canvas, 0, 0);
        lv_obj_set_size(canvas, 400, 400);
        ui_filament_path_canvas_set_slot_count(canvas, 4);
        ui_filament_path_canvas_set_slot_width(canvas, 100);
        ui_filament_path_canvas_set_slot_overlap(canvas, 0);
        ui_filament_path_canvas_set_topology(canvas,
                                             static_cast<int>(helix::PathTopology::PARALLEL));
        ui_filament_path_canvas_set_slot_callback(canvas, on_slot, &taps);
        lv_obj_update_layout(test_screen());
        process_lvgl(120);
    }
    ~ParallelTapFixture() override {
        lv_obj_delete(canvas);
        UITest::cleanup();
    }

    void tap(int32_t x, int32_t y) {
        REQUIRE(UITest::click_at(x, y));
        process_lvgl(20);
    }

    lv_obj_t* canvas = nullptr;
    Taps taps;
};

} // namespace

TEST_CASE_METHOD(ParallelTapFixture, "PARALLEL canvas: a head tap reaches the toolhead callback",
                 "[filament-path][hit_test][toolhead_tap]") {
    const auto style = GENERATE(
        helix::ToolheadStyle::DEFAULT, helix::ToolheadStyle::A4T, helix::ToolheadStyle::ANTHEAD,
        helix::ToolheadStyle::JABBERWOCKY, helix::ToolheadStyle::STEALTHBURNER,
        helix::ToolheadStyle::CREALITY_K1, helix::ToolheadStyle::CREALITY_K2);
    ScopedToolheadStyle scoped(style);
    CAPTURE(static_cast<int>(style));
    helix::ui::filament_path_canvas_set_toolhead_callback(canvas, on_toolhead, &taps);
    process_lvgl(60);

    const FilamentPathData* d = get_data(canvas);
    const BaseGeometry g = compute_base_geometry(canvas, d);
    const ParallelRows rows = parallel_rows(*d, g);
    const int32_t x = g.slot_x[1];
    const helix::ui::GlyphBounds head = toolhead_glyph_bounds(x, rows.toolhead_y, rows.tool_scale);
    CAPTURE(head.left, head.top, head.right, head.bottom, rows.sensor_y);
    REQUIRE(head.top > rows.sensor_y);

    SECTION("the glyph's middle") {
        tap(x, (head.top + head.bottom) / 2);
        CHECK(taps.toolheads == std::vector<int>{1});
        CHECK(taps.slots.empty());
    }
    SECTION("the glyph's top row") {
        tap(x, head.top);
        CHECK(taps.toolheads == std::vector<int>{1});
    }
    SECTION("the tube one pixel above the glyph is the lane's") {
        tap(x, head.top - 1);
        CHECK(taps.toolheads.empty());
        CHECK(taps.slots == std::vector<int>{1});
    }
    SECTION("the tube between the sensor and the glyph is the lane's") {
        tap(x, (rows.sensor_y + head.top) / 2);
        CHECK(taps.toolheads.empty());
        CHECK(taps.slots == std::vector<int>{1});
    }
    SECTION("the glyph's side edges") {
        tap(head.left, (head.top + head.bottom) / 2);
        tap(head.right, (head.top + head.bottom) / 2);
        CHECK(taps.toolheads == std::vector<int>{1, 1});
    }
    SECTION("the tube above the sensor reaches the slot callback") {
        tap(x, rows.sensor_y - 2);
        CHECK(taps.toolheads.empty());
        CHECK(taps.slots == std::vector<int>{1});
    }
}

TEST_CASE_METHOD(ParallelTapFixture,
                 "PARALLEL canvas: with no toolhead callback a head tap reaches the slot callback",
                 "[filament-path][hit_test][toolhead_tap]") {
    const FilamentPathData* d = get_data(canvas);
    const BaseGeometry g = compute_base_geometry(canvas, d);
    const ParallelRows rows = parallel_rows(*d, g);
    const helix::ui::GlyphBounds head =
        toolhead_glyph_bounds(g.slot_x[2], rows.toolhead_y, rows.tool_scale);
    tap(g.slot_x[2], (head.top + head.bottom) / 2);
    CHECK(taps.slots == std::vector<int>{2});
}
