// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_path_theme.cpp
 * @brief A live filament_path_canvas follows a theme or dark-mode switch.
 *
 * The canvas paints its tubes into a cached child canvas, so a switch must both
 * re-read the theme colors and repaint that cache; neither happens through a
 * setter.
 */

#include "ui_filament_path_canvas.h"

#include "../test_fixtures.h"
#include "filament_tube_stroker.h"
#include "lvgl/lvgl.h"
#include "src/ui/filament/ui_filament_path_internal.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(XMLTestFixture, "FilamentPath: a dark-mode switch repaints in the new colors",
                 "[filament_path][theme]") {
    lv_obj_t* path = ui_filament_path_canvas_create(test_screen());
    REQUIRE(path != nullptr);
    lv_obj_set_size(path, 400, 200);
    lv_obj_update_layout(test_screen());
    process_lvgl(100);

    auto* data = helix::ui::fpath::get_data(path);
    REQUIRE(data != nullptr);
    const int painted = data->layers.render_count;
    REQUIRE(painted >= 1);
    const lv_color_t old_accent = data->theme.color_accent;
    const lv_color_t old_idle = data->theme.color_idle;
    REQUIRE(lv_color_eq(old_accent, helix::ui::tube_accent()));

    theme_manager_toggle_dark_mode();
    const bool dark = theme_manager_is_dark_mode();
    const lv_color_t new_idle =
        theme_manager_get_color(dark ? "filament_idle_dark" : "filament_idle_light");
    REQUIRE_FALSE(lv_color_eq(old_accent, helix::ui::tube_accent()));
    REQUIRE_FALSE(lv_color_eq(old_idle, new_idle));
    // The switch invalidated the screen; draw that frame, then let the
    // repaint it scheduled run.
    lv_refr_now(nullptr);
    process_lvgl(100);

    CHECK(lv_color_eq(data->theme.color_accent, helix::ui::tube_accent()));
    CHECK(lv_color_eq(data->theme.color_idle, new_idle));
    CHECK(lv_color_eq(data->theme.color_text, theme_manager_get_color("text")));
    // The cached tubes are repainted once, with nothing but the switch to prompt it.
    CHECK(data->layers.render_count == painted + 1);

    // A frame with no switch neither re-reads nor repaints.
    lv_obj_invalidate(path);
    lv_refr_now(nullptr);
    process_lvgl(100);
    CHECK(data->layers.render_count == painted + 1);

    theme_manager_toggle_dark_mode();
    process_lvgl(20);
    lv_obj_delete(path);
    process_lvgl(10);
}
