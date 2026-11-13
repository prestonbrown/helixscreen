// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_responsive_grid.cpp
 * @brief Where <responsive_grid> (lib/helix-xml) places its children on this
 *        app's display config. The column rule itself is tested in helix-xml's
 *        own suite, which by its rules asserts no placed geometry.
 */

#include "../lvgl_ui_test_fixture.h"
// TEST_MIRROR_OK: the widget under test is lib/helix-xml's, included below; this file pins its
// placement on the app's display config
#include "helix-xml/src/xml/parsers/lv_xml_responsive_grid_parser.h"

#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct Geom {
    int32_t x, y, w, h;
};

struct Fit {
    int32_t cols, col_w;
    bool operator==(const Fit& o) const {
        return cols == o.cols && col_w == o.col_w;
    }
};

Fit fit_of(lv_obj_t* grid) {
    const lv_xml_grid_fit_t f = lv_xml_responsive_grid_get_fit(grid);
    return {f.cols, f.col_w};
}

Geom geom_in(lv_obj_t* grid, lv_obj_t* child) {
    return {lv_obj_get_x(child) - lv_obj_get_style_pad_left(grid, LV_PART_MAIN),
            lv_obj_get_y(child) - lv_obj_get_style_pad_top(grid, LV_PART_MAIN),
            lv_obj_get_width(child), lv_obj_get_height(child)};
}

lv_obj_t* make_grid(lv_obj_t* parent, std::vector<const char*> attrs) {
    attrs.push_back(nullptr);
    attrs.push_back(nullptr);
    auto* grid = static_cast<lv_obj_t*>(lv_xml_create(parent, "responsive_grid", attrs.data()));
    REQUIRE(grid != nullptr);
    return grid;
}

lv_obj_t* add_item(lv_obj_t* grid, int32_t h) {
    lv_obj_t* item = lv_obj_create(grid);
    lv_obj_set_size(item, 40, h); // the grid overrides the width
    return item;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "responsive_grid places children in fitted columns",
                 "[responsive_grid]") {
    lv_obj_t* grid = make_grid(test_screen(), {"width", "500", "min_col_width", "150",
                                               "style_pad_column", "10", "style_pad_row", "6"});
    std::vector<lv_obj_t*> items;
    for (int32_t h : {30, 50, 40, 20, 25}) {
        items.push_back(add_item(grid, h));
    }
    lv_obj_update_layout(test_screen());

    // Three columns of 160: row 1 is as tall as its tallest item (50).
    CHECK(geom_in(grid, items[0]).x == 0);
    CHECK(geom_in(grid, items[1]).x == 170);
    CHECK(geom_in(grid, items[2]).x == 340);
    for (lv_obj_t* item : items) {
        CHECK(lv_obj_get_width(item) == 160);
    }
    CHECK(geom_in(grid, items[0]).y == 0);
    CHECK(geom_in(grid, items[2]).y == 0);
    // The short last row keeps the column width and starts at the left.
    CHECK(geom_in(grid, items[3]).x == 0);
    CHECK(geom_in(grid, items[3]).y == 56);
    CHECK(geom_in(grid, items[4]).x == 170);
    // Content height: 50 + 6 + 25.
    CHECK(lv_obj_get_content_height(grid) == 81);
    CHECK(fit_of(grid) == Fit{3, 160});

    SECTION("a hidden child leaves no hole") {
        lv_obj_set_hidden(items[1], true);
        lv_obj_update_layout(test_screen());
        CHECK(geom_in(grid, items[2]).x == 170);
        CHECK(geom_in(grid, items[3]).x == 340);
        CHECK(geom_in(grid, items[4]).x == 0);
        CHECK(geom_in(grid, items[4]).y == 46); // row 1 is now 40 tall
    }
    SECTION("narrowing the grid reflows it") {
        lv_obj_set_width(grid, 320);
        lv_obj_update_layout(test_screen());
        CHECK(fit_of(grid) == Fit{2, 155});
        CHECK(geom_in(grid, items[2]).x == 0);
        CHECK(geom_in(grid, items[2]).y == 56);
        CHECK(lv_obj_get_width(items[4]) == 155);
    }
    SECTION("a child the grid stops placing gets its own width back") {
        lv_obj_set_ignore_layout(items[0], true);
        lv_obj_update_layout(test_screen());
        CHECK(lv_obj_get_width(items[0]) == 40);
        CHECK(geom_in(grid, items[1]).x == 0);
    }
    SECTION("a deleted child closes up") {
        lv_obj_delete(items[0]);
        lv_obj_update_layout(test_screen());
        CHECK(geom_in(grid, items[1]).x == 0);
        CHECK(geom_in(grid, items[3]).x == 340);
        CHECK(geom_in(grid, items[3]).y == 0);
        CHECK(geom_in(grid, items[4]).x == 0);
        CHECK(geom_in(grid, items[4]).y == 56);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "responsive_grid fit_items shares the row among few items",
                 "[responsive_grid]") {
    lv_obj_t* grid = make_grid(test_screen(), {"width", "500", "min_col_width", "100", "fit_items",
                                               "true", "style_pad_column", "10"});
    lv_obj_t* a = add_item(grid, 30);
    lv_obj_t* b = add_item(grid, 30);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_width(a) == 245);
    CHECK(geom_in(grid, b).x == 255);

    SECTION("max_cols caps a long list") {
        lv_obj_t* capped = make_grid(test_screen(), {"width", "500", "min_col_width", "100",
                                                     "max_cols", "2", "style_pad_column", "10"});
        for (int i = 0; i < 5; ++i) {
            add_item(capped, 30);
        }
        lv_obj_update_layout(test_screen());
        CHECK(fit_of(capped) == Fit{2, 245});
    }
}
