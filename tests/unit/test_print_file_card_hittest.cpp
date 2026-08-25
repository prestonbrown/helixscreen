// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEST_MIRROR_OK: builds the shipped ui_xml/components/print_file_card.xml through
//                 XMLTestFixture::create_component() and hit-tests the real widget tree
//                 with lv_indev_search_obj(). ../test_fixtures.h pulls in
//                 include/printer_state.h, include/moonraker_api.h, include/theme_manager.h.
// Hit-testing for print_file_card: the whole card face must resolve to card_root,
// which owns the CLICKED/LONG_PRESSED handlers (ui_print_select_card_view.cpp).
// Decorative children (metadata overlay and its rows) must not absorb the press.
// Regression: prestonbrown/helixscreen#1101

#include "../test_fixtures.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// Resolve the object LVGL's input layer would deliver a press at (x, y) to.
lv_obj_t* hit(lv_obj_t* screen, int32_t x, int32_t y) {
    lv_point_t p{x, y};
    return lv_indev_search_obj(screen, &p);
}

// Centre point of a widget in absolute screen coords.
lv_point_t centre_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return lv_point_t{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

std::string name_of(lv_obj_t* obj) {
    if (obj == nullptr)
        return "<null>";
    const char* n = lv_obj_get_name(obj);
    return n != nullptr ? n : "<unnamed>";
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "print_file_card: metadata area routes clicks to card_root",
                 "[xml][print_file_card][hittest]") {
    REQUIRE(register_component("print_file_card"));

    const char* attrs[] = {
        "filename", "some_long_model_name.gcode", "print_time", "1h 20m", "filament_weight", "24g",
        nullptr};
    lv_obj_t* card = create_component("print_file_card", attrs);
    REQUIRE(card != nullptr);

    // Layout must settle before coordinates mean anything.
    lv_obj_update_layout(card);

    lv_obj_t* overlay = lv_obj_find_by_name(card, "metadata_overlay");
    lv_obj_t* filename = lv_obj_find_by_name(card, "filename_label");
    lv_obj_t* row = lv_obj_find_by_name(card, "metadata_row");
    REQUIRE(overlay != nullptr);
    REQUIRE(filename != nullptr);
    REQUIRE(row != nullptr);

    SECTION("thumbnail area works today (control)") {
        lv_area_t c;
        lv_obj_get_coords(card, &c);
        // A point near the top of the card, well above the metadata overlay.
        lv_obj_t* got = hit(test_screen(), (c.x1 + c.x2) / 2, c.y1 + 10);
        INFO("hit near card top resolved to: " << name_of(got));
        CHECK(got == card);
    }

    SECTION("tap on the filename text reaches card_root") {
        lv_point_t p = centre_of(filename);
        lv_obj_t* got = hit(test_screen(), p.x, p.y);
        INFO("hit on filename_label resolved to: " << name_of(got));
        CHECK(got == card);
    }

    SECTION("tap on the metadata row (time/filament) reaches card_root") {
        lv_point_t p = centre_of(row);
        lv_obj_t* got = hit(test_screen(), p.x, p.y);
        INFO("hit on metadata_row resolved to: " << name_of(got));
        CHECK(got == card);
    }

    SECTION("tap anywhere on the overlay reaches card_root") {
        lv_point_t p = centre_of(overlay);
        lv_obj_t* got = hit(test_screen(), p.x, p.y);
        INFO("hit on metadata_overlay resolved to: " << name_of(got));
        CHECK(got == card);
    }
}

// The metadata row and its two groups are content-sized. They were percentage-sized
// inside a content-sized parent, which makes each depend on the other: the parent
// asks the child for its height, the child answers with a fraction of the parent,
// and both settle at zero. The labels then laid out past the bottom of the card and
// the print time and filament weight were invisible at every breakpoint.
// The hit-test case above does not cover this — routing still resolves correctly
// when the row is a few pixels tall.
// Regression: prestonbrown/helixscreen#1208
TEST_CASE_METHOD(XMLTestFixture, "print_file_card: metadata row keeps a real height",
                 "[xml][print_file_card][layout][1208]") {
    REQUIRE(register_component("print_file_card"));

    const char* attrs[] = {
        "filename", "some_long_model_name.gcode", "print_time", "1h 20m", "filament_weight", "24g",
        nullptr};
    lv_obj_t* card = create_component("print_file_card", attrs);
    REQUIRE(card != nullptr);
    lv_obj_update_layout(card);

    lv_obj_t* row = lv_obj_find_by_name(card, "metadata_row");
    lv_obj_t* time_label = lv_obj_find_by_name(card, "time_label");
    lv_obj_t* filament_label = lv_obj_find_by_name(card, "filament_label");
    REQUIRE(row != nullptr);
    REQUIRE(time_label != nullptr);
    REQUIRE(filament_label != nullptr);

    SECTION("the row is tall enough to hold its text") {
        const int32_t row_h = lv_obj_get_height(row);
        const int32_t text_h = lv_obj_get_height(time_label);
        INFO("metadata_row height=" << row_h << " time_label height=" << text_h);
        CHECK(text_h > 0);
        CHECK(row_h >= text_h);
    }

    SECTION("both labels have height") {
        INFO("time_label height=" << lv_obj_get_height(time_label) << " filament_label height="
                                  << lv_obj_get_height(filament_label));
        CHECK(lv_obj_get_height(time_label) > 0);
        CHECK(lv_obj_get_height(filament_label) > 0);
    }

    SECTION("the labels sit inside the card, not past its bottom edge") {
        lv_area_t card_area;
        lv_area_t time_area;
        lv_area_t filament_area;
        lv_obj_get_coords(card, &card_area);
        lv_obj_get_coords(time_label, &time_area);
        lv_obj_get_coords(filament_label, &filament_area);

        INFO("card bottom=" << card_area.y2 << " time bottom=" << time_area.y2
                            << " filament bottom=" << filament_area.y2);
        CHECK(time_area.y2 <= card_area.y2);
        CHECK(filament_area.y2 <= card_area.y2);
    }
}
