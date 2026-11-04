// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_path_canvas_ceilings.cpp
 * @brief A path widget setter given a value past its ceiling stores the ceiling.
 *
 * The widgets index fixed-size arrays and segment tables with these values, so
 * an over-range one that is stored as given reads past them at draw time.
 */

#include "ui_endless_spool_arrows.h"
#include "ui_filament_path_canvas.h"
#include "ui_spool_canvas.h"
#include "ui_system_path_canvas.h"

#include "../lvgl_test_fixture.h"
#include "ams_types.h"
#include "display_settings_manager.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "src/ui/filament/ui_system_path_plan.h"

#include <vector>

#include "../catch_amalgamated.hpp"

using helix::kAmsMaxSlots;
using helix::kPathSegmentCount;
using helix::ui::fpath::FilamentPathData;
using helix::ui::fpath::get_data;
using helix::ui::syspath::system_path_data;
using helix::ui::syspath::SystemPathData;

namespace {

constexpr int kLastSegment = kPathSegmentCount - 1;

/// Pixels with any opacity in a snapshot of @p obj.
int drawn_pixels(lv_obj_t* obj) {
    lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(snap != nullptr);
    int drawn = 0;
    for (uint32_t y = 0; y < snap->header.h; ++y) {
        const uint8_t* row = snap->data + y * snap->header.stride;
        for (uint32_t x = 0; x < snap->header.w; ++x) {
            if (row[x * 4 + 3] != 0) {
                ++drawn;
            }
        }
    }
    lv_draw_buf_destroy(snap);
    return drawn;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "FilamentPath: setters past the ceiling store the ceiling",
                 "[filament_path][canvas_ceiling]") {
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    FilamentPathData* data = get_data(path);
    REQUIRE(data != nullptr);

    SECTION("filament segment") {
        ui_filament_path_canvas_set_filament_segment(path, kPathSegmentCount + 3);
        CHECK(data->filament_segment == kLastSegment);
    }
    SECTION("error segment") {
        ui_filament_path_canvas_set_error_segment(path, kPathSegmentCount + 3);
        CHECK(data->error_segment == kLastSegment);
    }
    SECTION("animation progress") {
        ui_filament_path_canvas_set_anim_progress(path, 250);
        CHECK(data->anim.progress == 100);
    }
    SECTION("fixed hub lanes") {
        ui_filament_path_canvas_set_fixed_hub(path, kAmsMaxSlots + 5, 20);
        CHECK(data->fixed_hub_lanes == FilamentPathData::kMaxSlots);
    }
    SECTION("an animated segment's target") {
        ui_filament_path_canvas_animate_segment(path, 0, kPathSegmentCount + 3);
        CHECK(data->filament_segment == kLastSegment);
    }
    SECTION("an animated segment's origin") {
        // With animations off the transition jumps to its end and keeps no origin.
        auto& display = helix::DisplaySettingsManager::instance();
        const bool animations = display.get_animations_enabled();
        display.set_animations_enabled(true);
        ui_filament_path_canvas_animate_segment(path, kPathSegmentCount + 3, 0);
        CHECK(data->anim.prev_segment == kLastSegment);
        display.set_animations_enabled(animations);
    }
    lv_obj_delete(path);
}

TEST_CASE_METHOD(LVGLTestFixture, "SystemPath: setters past the ceiling store the ceiling",
                 "[system_path][canvas_ceiling]") {
    lv_obj_t* canvas = ui_system_path_canvas_create(test_screen());
    REQUIRE(canvas != nullptr);
    const SystemPathData* data = system_path_data(canvas);
    REQUIRE(data != nullptr);

    SECTION("unit count") {
        ui_system_path_canvas_set_unit_count(canvas, SystemPathData::kMaxUnits + 4);
        CHECK(data->unit_count == SystemPathData::kMaxUnits);
    }
    SECTION("total tools") {
        ui_system_path_canvas_set_total_tools(canvas, SystemPathData::kMaxTools + 4);
        CHECK(data->total_tools == SystemPathData::kMaxTools);
    }
    lv_obj_delete(canvas);
}

TEST_CASE_METHOD(LVGLTestFixture, "SpoolCanvas: a fill level above full stores full",
                 "[spool_canvas][canvas_ceiling]") {
    lv_obj_t* canvas = ui_spool_canvas_create(test_screen(), 64);
    REQUIRE(canvas != nullptr);
    ui_spool_canvas_set_fill_level(canvas, 1.75f);
    CHECK(ui_spool_canvas_get_fill_level(canvas) == 1.0f);
    lv_obj_delete(canvas);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "EndlessSpoolArrows: a slot count past the ceiling draws no lane beyond it",
                 "[endless_spool][canvas_ceiling]") {
    constexpr int kSlotWidth = 30;
    constexpr int kCount = kAmsMaxSlots + 1;
    lv_obj_t* arrows = ui_endless_spool_arrows_create(test_screen());
    REQUIRE(arrows != nullptr);
    lv_obj_set_pos(arrows, 0, 0);
    lv_obj_set_size(arrows, kCount * kSlotWidth, 60);
    ui_endless_spool_arrows_set_slot_count(arrows, kCount);
    ui_endless_spool_arrows_set_slot_width(arrows, kSlotWidth);
    ui_endless_spool_arrows_set_slot_overlap(arrows, 0);

    std::vector<int> edges(kCount, -1);
    SECTION("a backup between the last two lanes under the ceiling is drawn") {
        edges[kAmsMaxSlots - 2] = kAmsMaxSlots - 1;
        ui_endless_spool_arrows_set_groups(arrows,
                                           helix::printer::endless_spool_config_from_edges(edges));
        lv_obj_update_layout(test_screen());
        CHECK(drawn_pixels(arrows) > 0);
    }
    SECTION("a backup into the lane past the ceiling is not") {
        edges[kAmsMaxSlots - 1] = kAmsMaxSlots;
        ui_endless_spool_arrows_set_groups(arrows,
                                           helix::printer::endless_spool_config_from_edges(edges));
        lv_obj_update_layout(test_screen());
        CHECK(drawn_pixels(arrows) == 0);
    }
    lv_obj_delete(arrows);
}
