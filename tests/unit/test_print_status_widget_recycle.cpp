// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_recycle.cpp
 * @brief Regression: PrintStatusWidget must re-establish its imperative print-card
 *        flex layout (thumbnail row vs column) on attach.
 *
 * Same recycled-instance class as #1109. The row layout (1x2 / 3x2) is applied
 * only imperatively in on_size_changed, gated by `if (use_column == is_column_)
 * return;`. A fresh XML component defaults to column flow. Because is_column_
 * starts (and, on a recycled instance, persists) matching the non-column sizes,
 * on_size_changed early-returns and the row layout was never established on the
 * fresh component — leaving the card stuck in the default column arrangement.
 *
 * apply_card_layout() is now also called from attach(), so the card matches the
 * persisted is_column_ regardless of the early-return.
 *
 * Teardown note: ~PrintStatusWidget intentionally does NOT reset the static
 * DetailedFormatter (its ObserverGuards would dangle the helix-xml scope
 * subjects). Under LVGLUITestFixture the per-instance PrinterState subjects are
 * freed before the base fixture's reset_all runs, so we destroy the formatter
 * here — while those subjects are still alive — via destroy_formatter_for_test(),
 * matching the layout_gate tests. The flex-flow value is captured and asserted
 * AFTER that cleanup so a future regression fails cleanly instead of segfaulting
 * the whole suite in teardown.
 */

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

lv_obj_t* make_print_status(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "panel_widget_print_status", nullptr));
}

lv_flex_flow_t card_flow(lv_obj_t* comp) {
    lv_obj_t* layout = lv_obj_find_by_name(comp, "print_card_layout");
    REQUIRE(layout != nullptr);
    return lv_obj_get_style_flex_flow(layout, LV_PART_MAIN);
}

// Tri-state so a missing child is distinguishable from a cleared flag after the
// deferred-assert dance below: -1 = child not found, 0 = not scrollable, 1 = scrollable.
int scrollable_state(lv_obj_t* comp, const char* name) {
    lv_obj_t* child = lv_obj_find_by_name(comp, name);
    if (child == nullptr) {
        return -1;
    }
    return lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE) ? 1 : 0;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "print_status card keeps row layout after attach at 3x2",
                 "[print_status][panel_widget][1109]") {
    lv_flex_flow_t flow{};
    {
        // Construct the widget first so its subjects + formatter are registered
        // before the component XML is parsed (helix-xml skips bindings whose
        // subject is absent at parse time).
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});

        lv_obj_t* comp = make_print_status(test_screen());
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());

        // Wide band -> row layout. is_column_ starts false, so on_size_changed
        // early-returns; attach() must have already applied the row layout.
        widget.on_size_changed(3, 2, W_WIDE, 400);
        process_lvgl(30);
        flow = card_flow(comp);
    }
    PrintStatusWidget::destroy_formatter_for_test();
    REQUIRE(flow == LV_FLEX_FLOW_ROW);
}

/**
 * The idle containers hold the benchy placeholder thumbnail and were never meant
 * to scroll, but they carried no `scrollable` attribute, so they inherited LVGL's
 * scrollable-ON default. That made them qualify for a page-scroll gutter, and on
 * an 800x480 K-Touch (where /display/page_scroll_buttons defaults on) the gutter
 * chevrons drew straight over the thumbnail. Assert the runtime flag rather than
 * the XML text: the flag is what the gutter injector actually tests.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "print_status idle containers are not scrollable",
                 "[print_status][panel_widget][scroll]") {
    int idle = -2;
    int idle_compact = -2;
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});

        lv_obj_t* comp = make_print_status(test_screen());
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        process_lvgl(30);

        idle = scrollable_state(comp, "print_card_idle");
        idle_compact = scrollable_state(comp, "print_card_idle_compact");
    }
    PrintStatusWidget::destroy_formatter_for_test();
    REQUIRE(idle == 0);
    REQUIRE(idle_compact == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "print_status card layout survives instance recycle",
                 "[print_status][panel_widget][1109]") {
    lv_flex_flow_t flow_first{};
    lv_flex_flow_t flow_recycled{};
    lv_flex_flow_t flow_transitioned{};
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});

        // First placement at the wide band -> row layout, is_column_ stays false.
        lv_obj_t* comp1 = make_print_status(test_screen());
        REQUIRE(comp1 != nullptr);
        widget.attach(comp1, test_screen());
        widget.on_size_changed(3, 2, W_WIDE, 400);
        process_lvgl(30);
        flow_first = card_flow(comp1);

        // Recycle: destroy the old component, re-attach the SAME widget to a fresh
        // one at the SAME wide band. on_size_changed sees use_column == is_column_
        // (both false) and early-returns; without an attach()-time apply the fresh
        // component would keep its default COLUMN flow. This is the core repro.
        lv_obj_delete(comp1);
        lv_obj_t* comp2 = make_print_status(test_screen());
        REQUIRE(comp2 != nullptr);
        widget.attach(comp2, test_screen());
        widget.on_size_changed(3, 2, W_WIDE, 400);
        process_lvgl(30);
        flow_recycled = card_flow(comp2);

        // A recycle that transitions to a normal-band, tall-enough size applies column.
        lv_obj_delete(comp2);
        lv_obj_t* comp3 = make_print_status(test_screen());
        REQUIRE(comp3 != nullptr);
        widget.attach(comp3, test_screen());
        widget.on_size_changed(2, 2, W_NORMAL, H_TALL);
        process_lvgl(30);
        flow_transitioned = card_flow(comp3);
    }
    PrintStatusWidget::destroy_formatter_for_test();
    REQUIRE(flow_first == LV_FLEX_FLOW_ROW);
    REQUIRE(flow_recycled == LV_FLEX_FLOW_ROW);
    REQUIRE(flow_transitioned == LV_FLEX_FLOW_COLUMN);
}
