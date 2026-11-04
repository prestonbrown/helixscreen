// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Controls "Calibration & Tools" grid refits its visible tiles to its own
// size, so no tile lands in a row the card clips. At the test display's
// 800x480 the card is wide: up to 4 tiles lay out 2x2, 5-6 as 3x2, 7 as 4x2.

#include "ui_panel_controls.h"

#include "../lvgl_ui_test_fixture.h"
#include "core/update_queue.h"

#include <set>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct GridShape {
    int visible = 0;
    int rows = 0;
    int max_columns = 0;
    bool all_inside = true;
    /// The rows of tiles and their gaps take up the grid's height, to within
    /// the pixel per row that whole-pixel tiles may leave over.
    bool fills_height = false;
};

class ControlsCalibGridFixture : public LVGLUITestFixture {
  public:
    ControlsCalibGridFixture() : panel(state(), nullptr) {}

    ~ControlsCalibGridFixture() override {
        if (panel_obj) {
            panel.on_deactivate(DeactivateReason::NavigateAway);
            lv_obj_delete(panel_obj);
            panel_obj = nullptr;
        }
        helix::ui::UpdateQueue::instance().drain();
        panel.deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    void build() {
        panel.init_subjects();
        panel_obj = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "controls_panel", nullptr));
        REQUIRE(panel_obj != nullptr);
        panel.setup(panel_obj, test_screen());
        settle();
    }

    void set(const char* name, int value) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        lv_subject_set_int(s, value);
    }

    void settle() {
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(test_screen());
        process_lvgl(10);
    }

    GridShape shape() {
        settle();
        lv_obj_t* grid = lv_obj_find_by_name(panel_obj, "calib_tools_grid");
        REQUIRE(grid != nullptr);
        lv_area_t g;
        lv_obj_get_coords(grid, &g);

        GridShape out;
        std::multiset<int32_t> row_tops;
        for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
            lv_obj_t* tile = lv_obj_get_child(grid, static_cast<int32_t>(i));
            if (lv_obj_has_flag(tile, LV_OBJ_FLAG_HIDDEN)) {
                continue;
            }
            lv_area_t t;
            lv_obj_get_coords(tile, &t);
            out.visible++;
            row_tops.insert(t.y1);
            if (t.y2 > g.y2 || t.x2 > g.x2) {
                out.all_inside = false;
            }
        }
        std::set<int32_t> rows(row_tops.begin(), row_tops.end());
        out.rows = static_cast<int>(rows.size());
        int32_t min_h = INT32_MAX;
        for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
            lv_obj_t* tile = lv_obj_get_child(grid, static_cast<int32_t>(i));
            if (!lv_obj_has_flag(tile, LV_OBJ_FLAG_HIDDEN)) {
                min_h = std::min(min_h, lv_obj_get_height(tile));
            }
        }
        const int32_t gap = lv_obj_get_style_pad_row(grid, LV_PART_MAIN);
        out.fills_height = out.rows > 0 && out.rows * min_h + (out.rows - 1) * gap >=
                                               lv_obj_get_content_height(grid) - out.rows;
        for (int32_t y : rows) {
            out.max_columns = std::max(out.max_columns, static_cast<int>(row_tops.count(y)));
        }
        return out;
    }

    lv_obj_t* grid() {
        lv_obj_t* g = lv_obj_find_by_name(panel_obj, "calib_tools_grid");
        REQUIRE(g != nullptr);
        return g;
    }

    /// Pin the grid to a box of its own, as a different card shape would give it.
    void resize_grid(int32_t w, int32_t h) {
        lv_obj_set_flex_grow(grid(), 0);
        lv_obj_set_size(grid(), w, h);
        settle();
        // The resize lays the grid out, which refits it, which lays it out once more.
        settle();
    }

    /// Whether the caption under the Bed Mesh glyph is shown.
    bool bed_mesh_label_shown() {
        lv_obj_t* label = find_label(grid(), "Bed Mesh");
        REQUIRE(label != nullptr);
        return !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN);
    }

    static lv_obj_t* find_label(lv_obj_t* obj, const char* text) {
        if (lv_obj_check_type(obj, &lv_label_class)) {
            const char* t = lv_label_get_text(obj);
            if (t && std::string(t) == text) {
                return obj;
            }
        }
        for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
            if (lv_obj_t* hit = find_label(lv_obj_get_child(obj, static_cast<int32_t>(i)), text)) {
                return hit;
            }
        }
        return nullptr;
    }

    /// Bed Mesh and Z Calibration are always shown; each flag adds one tile.
    /// Beta features stay off: no tile depends on them.
    void show(int qgl, int z_tilt, int pa, int screws, int tool_offsets) {
        set("show_beta_features", 0);
        set("printer_has_tool_offset_cal", tool_offsets);
        set("hide_manual_z_calibration", 0);
        set("printer_has_qgl", qgl);
        set("printer_has_z_tilt", z_tilt);
        set("printer_has_pa_cal", pa);
        set("printer_has_screws_tilt", screws);
    }

    ControlsPanel panel;
    lv_obj_t* panel_obj = nullptr;
};

} // namespace

TEST_CASE_METHOD(ControlsCalibGridFixture,
                 "Controls calibration grid picks columns from the visible tile count",
                 "[controls][calib-grid]") {
    build();
    SECTION("4 tiles lay out 2x2") {
        show(1, 0, 1, 0, 0);
        GridShape s = shape();
        CHECK(s.visible == 4);
        CHECK(s.rows == 2);
        CHECK(s.max_columns == 2);
        CHECK(s.all_inside);
    }
    SECTION("5 tiles lay out 3x2") {
        show(1, 0, 1, 1, 0);
        GridShape s = shape();
        CHECK(s.visible == 5);
        CHECK(s.rows == 2);
        CHECK(s.max_columns == 3);
        CHECK(s.all_inside);
    }
    SECTION("6 tiles lay out 3x2") {
        show(1, 0, 1, 1, 1);
        GridShape s = shape();
        CHECK(s.visible == 6);
        CHECK(s.rows == 2);
        CHECK(s.max_columns == 3);
        CHECK(s.all_inside);
        CHECK(s.fills_height);
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "calib_grid_labels")) == 1);
    }
    SECTION("7 tiles lay out 4x2") {
        show(1, 1, 1, 1, 1);
        GridShape s = shape();
        CHECK(s.visible == 7);
        CHECK(s.rows == 2);
        CHECK(s.max_columns == 4);
        CHECK(s.all_inside);
    }
    SECTION("a tile hidden again narrows the grid back to 2 columns") {
        show(1, 0, 1, 1, 1);
        REQUIRE(shape().max_columns == 3);
        show(1, 0, 1, 0, 0);
        GridShape s = shape();
        CHECK(s.visible == 4);
        CHECK(s.max_columns == 2);
    }
    SECTION("hiding manual Z calibration on a tool-offset printer counts as one fewer tile") {
        show(1, 0, 1, 1, 1);
        set("hide_manual_z_calibration", 1);
        GridShape s = shape();
        CHECK(s.visible == 5);
        CHECK(s.max_columns == 3);
        CHECK(s.rows == 2);
    }
    SECTION("a tall grid stacks six tiles 2x3") {
        show(1, 0, 1, 1, 1);
        resize_grid(220, 300);
        GridShape s = shape();
        CHECK(s.visible == 6);
        CHECK(s.max_columns == 2);
        CHECK(s.rows == 3);
        CHECK(s.all_inside);
        CHECK(s.fills_height);
        CHECK(bed_mesh_label_shown());
    }
    SECTION("a grid too small for captions drops them and still fits every tile") {
        show(1, 0, 1, 1, 1);
        resize_grid(120, 80);
        GridShape s = shape();
        CHECK(s.visible == 6);
        CHECK(s.all_inside);
        CHECK(s.rows * s.max_columns >= 6);
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "calib_grid_labels")) == 0);
        CHECK_FALSE(bed_mesh_label_shown());
    }
    SECTION("resizing the grid changes its shape") {
        show(1, 0, 1, 1, 1);
        GridShape wide = shape();
        REQUIRE(wide.max_columns == 3);
        REQUIRE(wide.rows == 2);
        resize_grid(220, 300);
        GridShape tall = shape();
        CHECK(tall.max_columns == 2);
        CHECK(tall.rows == 3);
        CHECK(tall.all_inside);
    }
    SECTION("a narrow grid with wide gaps keeps its tiles inside") {
        show(1, 0, 1, 1, 1);
        lv_obj_set_style_pad_column(grid(), 12, LV_PART_MAIN);
        lv_obj_set_style_pad_row(grid(), 12, LV_PART_MAIN);
        resize_grid(266, 241);
        GridShape s = shape();
        CHECK(s.visible == 6);
        CHECK(s.all_inside);
        CHECK(s.fills_height);
        CHECK(s.rows * s.max_columns >= 6);
    }
}

TEST_CASE_METHOD(ControlsCalibGridFixture,
                 "Controls Z Calibration steps aside for a full tool offset routine with beta off",
                 "[controls][calib-grid][tool_offsets]") {
    build();
    show(0, 0, 0, 0, 1);
    auto tile_hidden = [this](const char* name) {
        lv_obj_t* obj = lv_obj_find_by_name(panel_obj, name);
        REQUIRE(obj != nullptr);
        for (; obj && obj != grid(); obj = lv_obj_get_parent(obj)) {
            if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return true;
            }
        }
        return false;
    };

    settle();
    CHECK_FALSE(tile_hidden("btn_tool_offsets"));
    CHECK_FALSE(tile_hidden("btn_z_calibration"));

    set("hide_manual_z_calibration", 1);
    settle();
    CHECK(tile_hidden("btn_z_calibration"));
    CHECK_FALSE(tile_hidden("btn_tool_offsets"));

    set("printer_has_tool_offset_cal", 0);
    settle();
    CHECK_FALSE(tile_hidden("btn_z_calibration"));
    CHECK(tile_hidden("btn_tool_offsets"));
    set("hide_manual_z_calibration", 0);
}
