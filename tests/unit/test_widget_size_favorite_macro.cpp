// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_favorite_macro.cpp
 * @brief favorite_macro draws its badge, glyph and name at the rung its box
 *        earns, not the one its span suggests.
 *
 * Each case passes a span that contradicts the pixels, so a tile still reading
 * colspan/rowspan fails rather than passing by coincidence.
 */

#include "ui_tile_rung.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "favorite_macro_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture, "favorite_macro badge/icon/name follow pixels, not spans",
                 "[widget_size][favorite_macro]") {
    require_font_tokens_distinct();

    PanelWidgetHarness<FavoriteMacroWidget> h(test_screen(), "favorite_macro:1");
    lv_obj_t* badge = h.child("fav_macro_badge");
    REQUIRE(badge != nullptr);
    lv_obj_t* icon = h.child("fav_macro_icon");
    REQUIRE(icon != nullptr);
    lv_obj_t* name = h.child("fav_macro_name");
    REQUIRE(name != nullptr);
    lv_subject_t* rung = lv_xml_get_subject(nullptr, "favorite_macro:1_tile_icon");
    REQUIRE(rung != nullptr);

    auto check_faces = [&] {
        const int r = lv_subject_get_int(rung);
        const ui::TileFace icon_face = ui::tile_rung_face(ui::TileLadder::Icon, r);
        CHECK(lv_obj_get_style_text_font(icon, LV_PART_MAIN) == icon_face.font);
        CHECK(lv_obj_get_width(badge) == ui::tile_disc_edge(icon_face));
        CHECK(lv_obj_get_style_text_font(name, LV_PART_MAIN) ==
              ui::tile_rung_face(ui::TileLadder::Label, r).font);
        return r;
    };

    // A big span over a small box, then a small span over a big box.
    h.resize(8, 8, 60, 60);
    const int small = check_faces();
    h.resize(1, 1, 400, 400);
    const int large = check_faces();
    CHECK(large > small);
}
