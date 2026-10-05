// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_card_sizing.cpp
 * @brief Print-select cards are sized to the card container's own height
 *
 * Everything stacked above the card grid (the header, the "Recently Printed"
 * banner) takes height from it, so the rows sized for the first screen must end
 * inside the container rather than past its bottom edge.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct FirstScreen {
    int cards = 0;    ///< visible cards whose top lies inside the content area
    int overflow = 0; ///< of those, cards whose bottom passes the content area
};

FirstScreen measure_first_screen(lv_obj_t* container) {
    lv_obj_update_layout(container);
    lv_area_t content;
    lv_obj_get_content_coords(container, &content);

    FirstScreen out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* child = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_height(child) == 0) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        if (a.y1 < content.y1 || a.y1 > content.y2) {
            continue;
        }
        out.cards++;
        if (a.y2 > content.y2) {
            out.overflow++;
        }
    }
    return out;
}

/// Lay out (firing SIZE_CHANGED), then run the re-populate it defers.
void settle() {
    lv_obj_update_layout(lv_screen_active());
    PrintSelectPanelFixture::drain();
}

} // namespace

TEST_CASE_METHOD(PrintSelectPanelFixture,
                 "Print-select cards fit the container with the Recently Printed banner shown",
                 "[print_select][card_sizing]") {
    PlantedGcode file("card_sizing.gcode");
    panel_->refresh_files(true);
    drain();

    lv_obj_t* container = lv_obj_find_by_name(panel_obj_, "card_view_container");
    lv_obj_t* banner = lv_obj_find_by_name(panel_obj_, "context_banner");
    REQUIRE(container != nullptr);
    REQUIRE(banner != nullptr);

    panel_->set_sort_recent();
    settle();

    REQUIRE_FALSE(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_height(banner) > 0);

    const FirstScreen shown = measure_first_screen(container);
    INFO("container content height " << lv_obj_get_content_height(container));
    // More than one row on screen, so the bottom row is not also the top one.
    REQUIRE(shown.cards > 4);
    CHECK(shown.overflow == 0);

    // Hiding the banner hands its height back; the grid regrows to fill it.
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    settle();
    const int grown_h = lv_obj_get_content_height(container);
    const FirstScreen regrown = measure_first_screen(container);
    CHECK(regrown.overflow == 0);
    // Every on-screen card shares one height: none was left at the old size.
    int card_h = -1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* child = lv_obj_get_child(container, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_height(child) == 0 ||
            lv_obj_get_width(child) >= lv_obj_get_content_width(container)) {
            continue; // spacers span the full width
        }
        if (card_h < 0) {
            card_h = lv_obj_get_height(child);
        }
        CHECK(lv_obj_get_height(child) == card_h);
    }
    REQUIRE(card_h > 0);
    // The rows use the reclaimed height rather than leaving it empty.
    CHECK(card_h * 2 > grown_h / 2);
}
