// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_render_theme.cpp
 * @brief The renderers' color lookup answers what the UI theme does, across a switch.
 */

#include "../test_fixtures.h"
#include "lvgl/lvgl.h"
#include "render_theme.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(XMLTestFixture,
                 "render_theme: renderers see the theme's colors across a dark-mode switch",
                 "[render_theme][theme]") {
    const char* const tokens[] = {"card_bg", "text",    "text_muted",    "elevated_bg",
                                  "info",    "warning", "filament_metal"};
    for (const char* token : tokens) {
        INFO(token);
        CHECK(lv_color_eq(helix::render_theme::color(token), theme_manager_get_color(token)));
    }

    const lv_color_t card_before = helix::render_theme::color("card_bg");
    theme_manager_toggle_dark_mode();
    const lv_color_t card_after = helix::render_theme::color("card_bg");
    CHECK_FALSE(lv_color_eq(card_before, card_after));
    for (const char* token : tokens) {
        INFO(token);
        CHECK(lv_color_eq(helix::render_theme::color(token), theme_manager_get_color(token)));
    }

    theme_manager_toggle_dark_mode();
    CHECK(lv_color_eq(helix::render_theme::color("card_bg"), card_before));
}
