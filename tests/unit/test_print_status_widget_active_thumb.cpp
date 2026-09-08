// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_active_thumb.cpp
 * @brief The active-print thumbnail write must escape the queue batch and stand
 *        down while the card is off the active screen.
 *
 * `print_thumbnail_path` is published from the main thread inside an
 * `UpdateQueue::process_pending()` batch, and `lv_image_set_src` cascades
 * `update_align` -> `lv_obj_update_layout` up to the page grid. The idle sibling
 * (`reset_print_card_to_idle`) already carries both defences after three field
 * crashes; these cases pin the same two properties on the active path:
 *
 *   1. nothing is written during the batch — the write lands on the next LVGL
 *      tick, via lv_async_call;
 *   2. once the card's subtree has been reparented onto lv_layer_top() — the
 *      shape populate_page's safe_clean_children() leaves behind — the write is
 *      skipped rather than relayouting a condemned tree (#1001).
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Real, distinct assets. Neither is benchy_thumbnail_white.png, which is both
// the XML's authored src and the no-thumbnail placeholder — using it would make
// "written" indistinguishable from "never touched".
constexpr const char* kThumbA = "A:assets/images/printer_200.png";
constexpr const char* kThumbB = "A:assets/images/printer_400.png";

lv_obj_t* make_print_status(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "panel_widget_print_status", nullptr));
}

std::string active_thumb_src(lv_obj_t* comp) {
    lv_obj_t* img = lv_obj_find_by_name(comp, "print_card_active_thumb");
    REQUIRE(img != nullptr);
    const void* src = lv_image_get_src(img);
    return src ? std::string(static_cast<const char*>(src)) : std::string();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "print_status active thumbnail write escapes the queue batch",
                 "[print_status][panel_widget][thumbnail]") {
    std::string during_batch;
    std::string after_tick;
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});

        lv_obj_t* comp = make_print_status(test_screen());
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        process_lvgl(30);

        get_printer_state().set_print_thumbnail("a.gcode", kThumbA);
        helix::ui::UpdateQueue::instance().drain();
        // Still inside what a real publish would call the batch: only the LVGL
        // timer tick below may apply the src.
        during_batch = active_thumb_src(comp);

        process_lvgl(30);
        after_tick = active_thumb_src(comp);
    }
    PrintStatusWidget::destroy_formatter_for_test();

    CHECK(during_batch != kThumbA);
    CHECK(after_tick == kThumbA);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status active thumbnail is skipped while the card is off-screen",
                 "[print_status][panel_widget][thumbnail]") {
    std::string on_screen;
    std::string after_reparent;
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});

        lv_obj_t* comp = make_print_status(test_screen());
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        process_lvgl(30);

        get_printer_state().set_print_thumbnail("a.gcode", kThumbA);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(30);
        on_screen = active_thumb_src(comp);

        // safe_clean_children() parks condemned subtrees on lv_layer_top() until
        // the next async tick — exactly where a queued thumbnail can still land.
        lv_obj_set_parent(comp, lv_layer_top());

        get_printer_state().set_print_thumbnail("b.gcode", kThumbB);
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(30);
        after_reparent = active_thumb_src(comp);

        // Put it back so the fixture tears the tree down the usual way.
        lv_obj_set_parent(comp, test_screen());
        process_lvgl(30);
    }
    PrintStatusWidget::destroy_formatter_for_test();

    CHECK(on_screen == kThumbA);
    CHECK(after_reparent == kThumbA);
}
