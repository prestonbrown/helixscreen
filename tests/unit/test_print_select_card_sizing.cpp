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
#include "../test_helpers/print_select_panel_test_access.h"

#include <filesystem>

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

/// Lay out (firing SIZE_CHANGED), run the re-populate it defers, then build
/// the cards that re-populate left to the prebuild.
void settle(PrintSelectPanel& panel) {
    lv_obj_update_layout(lv_screen_active());
    PrintSelectPanelFixture::drain();
    PrintSelectPanelTestAccess::finish_card_prebuild(panel);
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
    settle(*panel_);

    REQUIRE_FALSE(lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_height(banner) > 0);

    const FirstScreen shown = measure_first_screen(container);
    INFO("container content height " << lv_obj_get_content_height(container));
    // More than one row on screen, so the bottom row is not also the top one.
    REQUIRE(shown.cards > 4);
    CHECK(shown.overflow == 0);

    // Hiding the banner hands its height back; the grid regrows to fill it.
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    settle(*panel_);
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

// A local (mock) thumbnail is drawn from its prescaled .bin, sized to the card;
// the raw PNG would render at native size and be cropped to the card (#1208).
// A refresh re-applies metadata to a file that already has its .bin.
TEST_CASE_METHOD(PrintSelectPanelFixture,
                 "Print-select keeps a local thumbnail's prescaled image across a refresh",
                 "[print_select][card_sizing][thumbnail]") {
    PlantedGcode file("local_thumb_refresh.gcode");
    panel_->refresh_files(true);
    drain();

    const std::string png =
        std::filesystem::absolute("assets/images/benchy_thumbnail_white.png").string();
    REQUIRE(std::filesystem::exists(png));
    FileMetadata meta;
    meta.filename = file.name();
    meta.thumbnails.push_back(ThumbnailInfo{png, 300, 300});

    auto thumb = [&] {
        const PrintFileData* row = PrintSelectPanelTestAccess::find_file(*panel_, file.name());
        REQUIRE(row != nullptr);
        return row->thumbnail_path;
    };
    auto is_bin = [](const std::string& p) {
        return p.size() > 4 && p.compare(p.size() - 4, 4, ".bin") == 0;
    };

    PrintSelectPanelTestAccess::apply_metadata(*panel_, file.name(), meta);
    drain();
    const std::string first = thumb();
    REQUIRE(is_bin(first));

    PrintSelectPanelTestAccess::apply_metadata(*panel_, file.name(), meta);
    drain();
    CHECK(thumb() == first);
}
