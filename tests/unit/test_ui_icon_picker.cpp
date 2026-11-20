// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_icon_picker.h"

#include "../lvgl_test_fixture.h"

#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

const char* const kIcons[] = {"fan", "flash", "radiator"};

int outlined(lv_obj_t* grid) {
    int hit = -1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
        if (lv_obj_get_style_border_width(lv_obj_get_child(grid, i), LV_PART_MAIN) > 0) {
            REQUIRE(hit == -1);
            hit = static_cast<int>(i);
        }
    }
    return hit;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "icon grid: one cell per icon, selected one outlined",
                 "[icon_picker]") {
    lv_obj_t* grid = lv_obj_create(test_screen());
    populate_icon_grid(grid, kIcons, 3, "flash", [](const char*) {});
    CHECK(lv_obj_get_child_count(grid) == 3);
    CHECK(outlined(grid) == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "icon grid: each cell is a fixed square holding its glyph",
                 "[icon_picker]") {
    lv_obj_t* grid = lv_obj_create(test_screen());
    populate_icon_grid(grid, kIcons, 3, "", [](const char*) {});
    lv_obj_update_layout(grid);
    for (uint32_t i = 0; i < 3; ++i) {
        lv_obj_t* cell = lv_obj_get_child(grid, i);
        CHECK(lv_obj_get_width(cell) == 36);
        CHECK(lv_obj_get_height(cell) == 36);
        CHECK(lv_obj_is_clickable(cell));
        CHECK_FALSE(lv_obj_is_scrollable(cell));
        CHECK(lv_obj_get_style_bg_opa(cell, LV_PART_MAIN) == 0);
        REQUIRE(lv_obj_get_child_count(cell) == 1);
        lv_obj_t* glyph = lv_obj_get_child(cell, 0);
        CHECK(std::string(lv_label_get_text(glyph)) == icon::lookup_codepoint(kIcons[i]));
        CHECK(lv_obj_get_style_text_font(glyph, LV_PART_MAIN) == &mdi_icons_24);
        CHECK_FALSE(lv_obj_is_clickable(glyph));
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "icon grid: a tap reports the icon name and refresh moves the outline",
                 "[icon_picker]") {
    lv_obj_t* grid = lv_obj_create(test_screen());
    std::vector<std::string> picked;
    populate_icon_grid(grid, kIcons, 3, "fan", [&](const char* n) { picked.push_back(n); });
    CHECK(outlined(grid) == 0);

    lv_obj_send_event(lv_obj_get_child(grid, 2), LV_EVENT_CLICKED, nullptr);
    REQUIRE(picked.size() == 1);
    CHECK(picked[0] == "radiator");

    refresh_icon_grid(grid, "radiator");
    CHECK(outlined(grid) == 2);
    refresh_icon_grid(grid, "not-an-icon");
    CHECK(outlined(grid) == -1);
}

TEST_CASE_METHOD(LVGLTestFixture, "icon grid: deleting the grid frees the cells cleanly",
                 "[icon_picker]") {
    lv_obj_t* grid = lv_obj_create(test_screen());
    // Every cell holds a copy of the callback; the token lives as long as any copy does.
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> alive = token;
    populate_icon_grid(grid, kIcons, 3, "", [token](const char*) {});
    token.reset();
    REQUIRE_FALSE(alive.expired());

    lv_obj_delete(grid);
    REQUIRE(alive.expired());
}

TEST_CASE_METHOD(LVGLTestFixture, "icon grid: cells are freed after a modal disarms the tree",
                 "[icon_picker][lifetime]") {
    // A closing modal nulls user_data on every object in its dialog so nothing
    // dispatches into a freed owner during the exit animation. The cells must
    // still free what they own when the tree is deleted afterwards.
    lv_obj_t* grid = lv_obj_create(test_screen());
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> alive = token;
    populate_icon_grid(grid, kIcons, 3, "", [token](const char*) {});
    token.reset();
    REQUIRE_FALSE(alive.expired());

    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
        lv_obj_set_user_data(lv_obj_get_child(grid, i), nullptr);
    }
    lv_obj_delete(grid);
    CHECK(alive.expired());
}
