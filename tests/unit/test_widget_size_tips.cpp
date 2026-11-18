// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_tips.cpp
 * @brief tips picks its text font from physical width, not colspan, and gives
 *        its title the height of the tile.
 *
 * `compact` is the widget's only branch (`on_size_changed` predicate at
 * tips_widget.cpp:165), so no isolation is needed beyond the two cases below
 * — each contradicts colspan against width_px so a span-reading
 * implementation fails instead of passing by coincidence. The predicate
 * restyles two objects: the named tip label (`status_text_label`, cached as
 * `tip_label_`) and the unnamed "Tip:" prefix label (first child of
 * `tip_container`). The help_circle icon (last child) uses
 * `theme_manager_get_font("icon_font_md")` unconditionally — not
 * size-dependent — so it is left alone per the task brief and not asserted
 * here.
 */

#include "ui_fonts.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "featured_tip.h"
#include "panel_widget_manager.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/tips_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace helix {
class TipsWidgetTestAccess {
  public:
    static void display(TipsWidget& w, const PrintingTip& tip) {
        w.set_displayed_tip(tip);
    }
};
} // namespace helix

namespace {
PrintingTip featured_line(const std::string& title) {
    featured_tip::FeaturedTip f;
    f.id = "t";
    f.title = title;
    f.source = "Print Handy";
    return featured_tip::to_printing_tip(f);
}

/// Height @p label needs for all of its text at its own font and laid-out width.
int32_t full_text_height(lv_obj_t* label, const char* text) {
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                     lv_obj_get_content_width(label), LV_TEXT_FLAG_NONE);
    return size.y;
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "a featured line drops the Tip label and shrinks until it fits",
                 "[widget_size][tips][featured_tip]") {
    // The "Tip:" label's hidden flag binds to a widget subject, as at startup.
    PanelWidgetManager::instance().init_widget_subjects();
    PanelWidgetHarness<TipsWidget> h(test_screen());
    lv_obj_t* tip_container = h.child("tip_container");
    REQUIRE(tip_container != nullptr);
    lv_obj_t* label = h.child("status_text_label");
    REQUIRE(label != nullptr);
    lv_obj_t* prefix = lv_obj_get_child(tip_container, 0);
    REQUIRE(prefix != label);

    // The one-row tile the 1024x600 home page gives tips, measured with ctl geom.
    const int tile_w = 446;
    const int tile_h = 108;
    REQUIRE(tile_w >= w_wide());
    h.resize(2, 1, tile_w, tile_h);

    SECTION("a long joke steps down a face and shows whole") {
        const std::string joke =
            "Looking for self intersections with the patience of a tax auditor. I would ask "
            "the vertices to explain, but they all point elsewhere.";
        TipsWidgetTestAccess::display(h.widget(), featured_line(joke));
        lv_obj_update_layout(h.root());

        CHECK(lv_obj_has_flag(prefix, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) !=
              theme_manager_get_font("font_heading"));
        // The row gives the line the tile's whole height...
        CHECK(lv_obj_get_style_pad_top(tip_container, LV_PART_MAIN) == 0);
        CHECK(lv_obj_get_style_pad_bottom(tip_container, LV_PART_MAIN) == 0);
        // ...and the text needs no more than that, so no ellipsis.
        CHECK(full_text_height(label, joke.c_str()) <= tile_h);
        CHECK(lv_obj_get_style_max_height(label, LV_PART_MAIN) == tile_h);
    }

    SECTION("a joke too long for every face ends in the small one") {
        TipsWidgetTestAccess::display(h.widget(), featured_line(std::string(60, 'x') + " " +
                                                                std::string(400, 'y')));
        CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
              theme_manager_get_font("font_small"));
    }

    SECTION("a short joke keeps the heading face") {
        TipsWidgetTestAccess::display(h.widget(), featured_line("These go to eleven."));
        lv_obj_update_layout(h.root());
        CHECK(lv_obj_has_flag(prefix, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
              theme_manager_get_font("font_heading"));
    }

    SECTION("an ordinary tip after it gets its label and face back") {
        TipsWidgetTestAccess::display(h.widget(), featured_line(std::string(200, 'x')));
        PrintingTip tip;
        tip.title = "Level the bed before the first layer.";
        TipsWidgetTestAccess::display(h.widget(), tip);
        lv_obj_update_layout(h.root());
        CHECK_FALSE(lv_obj_has_flag(prefix, LV_OBJ_FLAG_HIDDEN));
        CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
              theme_manager_get_font("font_heading"));
        CHECK(lv_obj_get_style_pad_top(tip_container, LV_PART_MAIN) ==
              theme_manager_get_spacing("space_sm"));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "tips text font follows width, not colspan",
                 "[widget_size][tips]") {
    require_font_tokens_distinct();

    PanelWidgetManager::instance().init_widget_subjects();
    PanelWidgetHarness<TipsWidget> h(test_screen());

    lv_obj_t* tip_container = h.child("tip_container");
    REQUIRE(tip_container != nullptr);

    lv_obj_t* label = h.child("status_text_label");
    REQUIRE(label != nullptr);

    lv_obj_t* prefix = lv_obj_get_child(tip_container, 0);
    REQUIRE(prefix != nullptr);
    // Sanity: the prefix must be a different object from the bound label —
    // otherwise both assertions below would trivially agree.
    REQUIRE(prefix != label);

    // Wide colspan (would be non-compact under the old span predicate),
    // narrow pixels: compact must win on pixels alone.
    h.resize(4, 4, w_wide() - 1, 100);
    REQUIRE(lv_obj_get_style_text_font(label, LV_PART_MAIN) == theme_manager_get_font("font_body"));
    REQUIRE(lv_obj_get_style_text_font(prefix, LV_PART_MAIN) ==
            theme_manager_get_font("font_body"));

    // Narrow colspan (would be compact under the old span predicate), wide
    // pixels: non-compact must win on pixels alone.
    h.resize(1, 1, w_wide(), 100);
    REQUIRE(lv_obj_get_style_text_font(label, LV_PART_MAIN) ==
            theme_manager_get_font("font_heading"));
    REQUIRE(lv_obj_get_style_text_font(prefix, LV_PART_MAIN) ==
            theme_manager_get_font("font_heading"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "a long tip title fills the tile's height and stops inside it",
                 "[widget_size][tips]") {
    PanelWidgetManager::instance().init_widget_subjects();
    PanelWidgetHarness<TipsWidget> h(test_screen());

    lv_obj_t* tip_container = h.child("tip_container");
    REQUIRE(tip_container != nullptr);
    lv_obj_t* label = h.child("status_text_label");
    REQUIRE(label != nullptr);

    std::string title;
    for (int i = 0; i < 40; ++i) {
        title += "calibrate ";
    }
    lv_label_set_text(label, title.c_str());

    const int tile_h = 120;
    h.resize(2, 2, w_wide() - 1, tile_h);
    lv_obj_update_layout(h.root());

    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    REQUIRE(font != nullptr);
    const int32_t line_h = lv_font_get_line_height(font);
    const int32_t pad_v = lv_obj_get_style_pad_top(tip_container, LV_PART_MAIN) +
                          lv_obj_get_style_pad_bottom(tip_container, LV_PART_MAIN);
    // More lines of title than the tile has room for.
    REQUIRE(line_h * 6 > tile_h);

    // The title wraps over the height the tile gives it, not just the row's
    // single line beside the icon...
    CHECK(lv_obj_get_height(label) > 2 * line_h);
    // ...and ends inside the tile's padding.
    CHECK(lv_obj_get_height(label) <= tile_h - pad_v);
    CHECK(lv_obj_get_height(tip_container) <= tile_h);
}
