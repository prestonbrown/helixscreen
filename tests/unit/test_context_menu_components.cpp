// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_context_menu_components.cpp
 * @brief Every context menu resolves the card and header the C++ reaches for
 *
 * The menus are built from two shared components, and both the backdrop and the
 * card are inherited rather than spelled out per file. That makes a per-file
 * mistake silent: `ContextMenu` looks the card up by the name `menu_card_name()`
 * returns, and if a file's usage does not produce it, the menu still appears -
 * unpositioned, in the corner, with only a log line to say so. Exactly that
 * happened once while extracting the card component, when its view was missing
 * its `name` attribute and every card auto-named itself `context_menu_card_0`.
 *
 * Five of the eleven cannot be opened under the mock at all (no multi-tool, no
 * second printer, no Happy Hare selector), so live checks cover six at best.
 * This is what covers the rest.
 */

#include "ui_context_menu.h"

#include "../test_fixtures.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// Every context menu component, including the five with no reachable mock state.
constexpr const char* MENUS[] = {
    "print_status_configure_picker",
    "print_status_nozzle_tool_picker",
    "thermistor_configure_picker",
    "thermistor_sensor_picker",
    "fan_picker",
    "fan_stack_picker",
    "tool_switcher_picker",
    "printer_switch_menu",
    "ams_selector_menu",
    "spoolman_context_menu",
    "ams_context_menu",
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "context menus: every one resolves its card and close button",
                 "[xml][context-menu][sweep]") {
    for (const char* menu : MENUS) {
        INFO("component: " << menu);
        REQUIRE(register_component(menu));

        lv_obj_t* backdrop = create_component(menu, nullptr);
        REQUIRE(backdrop != nullptr);
        lv_obj_update_layout(backdrop);

        // The name ContextMenu::menu_card_name() defaults to. Without it the menu
        // is created but never positioned.
        lv_obj_t* card = lv_obj_find_by_name(backdrop, "context_menu");
        REQUIRE(card != nullptr);

        // The card must be a real box, not a collapsed one - a card that resolved
        // but measured 0x0 would satisfy a pointer check and still show nothing.
        CHECK(lv_obj_get_width(card) > 0);
        CHECK(lv_obj_get_height(card) > 0);

        // The shared dismiss affordance. Its absence is how a menu ends up with
        // backdrop-tap as its only exit, which is the complaint that started this.
        lv_obj_t* close = lv_obj_find_by_name(backdrop, "btn_close");
        REQUIRE(close != nullptr);
        CHECK_FALSE(lv_obj_has_flag(close, LV_OBJ_FLAG_HIDDEN));

        lv_obj_delete(backdrop);
    }
}

TEST_CASE_METHOD(XMLTestFixture,
                 "context menus: the close control is flush with the card's content",
                 "[xml][context-menu][sweep]") {
    // The title sits at the card's content-left with no padding of its own, so the
    // close control has to sit at the content-right for the header to read evenly.
    // ui_button carries 16px of internal padding, which is why the XML zeroes
    // pad_right; without that the glyph floats short of the edge.
    for (const char* menu : MENUS) {
        INFO("component: " << menu);
        REQUIRE(register_component(menu));

        lv_obj_t* backdrop = create_component(menu, nullptr);
        REQUIRE(backdrop != nullptr);
        lv_obj_update_layout(backdrop);

        lv_obj_t* card = lv_obj_find_by_name(backdrop, "context_menu");
        lv_obj_t* close = lv_obj_find_by_name(backdrop, "btn_close");
        REQUIRE(card != nullptr);
        REQUIRE(close != nullptr);

        lv_area_t card_area;
        lv_area_t close_area;
        lv_obj_get_coords(card, &card_area);
        lv_obj_get_coords(close, &close_area);

        const int32_t pad_right = lv_obj_get_style_pad_right(card, LV_PART_MAIN);
        const int32_t content_right = card_area.x2 - pad_right;

        INFO("card content right = " << content_right << ", close right = " << close_area.x2);
        // x2 is inclusive in LVGL, so an exact match is the flush case.
        CHECK(close_area.x2 == content_right);

        lv_obj_delete(backdrop);
    }
}
