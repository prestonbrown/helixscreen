// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * The top layer carries the theme's body font.
 *
 * LVGL strips every style off lv_layer_top() when it creates the display, so
 * text there that sets no font of its own falls back to LV_FONT_DEFAULT
 * instead of the breakpoint's body font the screens inherit. A dialog shown
 * on the top layer then renders its button captions in a different size from
 * every other dialog, and one moved there after layout (the crash report
 * dialog) keeps the label size it measured on the screen while drawing the
 * larger font, which clips the caption at 480x272.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

namespace {

const lv_font_t* font_of(lv_obj_t* obj) {
    return lv_obj_get_style_text_font(obj, LV_PART_MAIN);
}

/// A label measured on the active screen, then moved to the top layer: the
/// font it draws with and the height it measured must agree.
void check_label_moved_to_top_layer() {
    lv_obj_t* label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Send Report");
    lv_obj_update_layout(label);
    const lv_font_t* screen_font = font_of(label);
    const int32_t measured_h = lv_obj_get_height(label);

    lv_obj_set_parent(label, lv_layer_top());

    CHECK(font_of(lv_layer_top()) == font_of(lv_screen_active()));
    CHECK(font_of(label) == screen_font);
    CHECK(lv_font_get_line_height(font_of(label)) == measured_h);
    lv_obj_delete(label);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "text on the top layer uses the screen's body font",
                 "[theme][modal]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    SECTION("at the fixture's resolution") {
        check_label_moved_to_top_layer();
    }

    SECTION("after a theme init at another breakpoint") {
        const lv_font_t* before = font_of(lv_screen_active());
        {
            ScopedResolution res(disp, 480, 272);
            theme_manager_init(disp, false);
            // Precondition: the breakpoint really picked a different body font.
            REQUIRE(font_of(lv_screen_active()) != before);
            check_label_moved_to_top_layer();
        }
        theme_manager_init(disp, false);
        CHECK(font_of(lv_layer_top()) == before);
    }

    helix::ui::UpdateQueue::instance().drain();
}
