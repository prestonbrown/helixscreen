// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_teardown.cpp
 * @brief PrintStatusWidget's cached widgets must read nullptr once its tile tree
 *        is deleted raw (prestonbrown/helixscreen#1298)
 *
 * A raw lv_obj_delete() of the page frees the tile without calling detach(), so
 * the widget's observers, lifetime token and live_instances() entry all stay
 * valid. Every deferred path guards on widget_obj_ and the cached children, so
 * those pointers must track the tree's lifetime, not the owner's.
 *
 * ~PrintStatusWidget leaves the static DetailedFormatter alone, so each case
 * destroys it while the fixture's subjects are still alive, as the recycle
 * tests do.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = PrintStatusWidgetTestAccess;

namespace {

lv_obj_t* make_page(lv_obj_t* screen) {
    lv_obj_t* page = lv_obj_create(screen);
    lv_obj_set_size(page, 800, 600);
    return page;
}

lv_obj_t* make_print_status(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "panel_widget_print_status", nullptr));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status widget drops its cached tree when the page is deleted raw",
                 "[print_status][panel_widget][teardown][uaf][1298]") {
    bool root_null = false, thumb_null = false, active_null = false, row_null = false;
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});
        lv_obj_t* page = make_page(test_screen());
        lv_obj_t* comp = make_print_status(page);
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        // The active thumbnail exists only while a print holds the machine.
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        process_lvgl(10);
        process_lvgl(30);

        // Populated first, or the checks below pass for the wrong reason.
        REQUIRE(Access::widget_obj(widget) == comp);
        REQUIRE(Access::thumb(widget) != nullptr);
        REQUIRE(Access::active_thumb(widget) != nullptr);
        REQUIRE(Access::library_row_last(widget) != nullptr);

        lv_obj_delete(page);

        root_null = Access::widget_obj(widget) == nullptr;
        thumb_null = Access::thumb(widget) == nullptr;
        active_null = Access::active_thumb(widget) == nullptr;
        row_null = Access::library_row_last(widget) == nullptr;

        // The owner outlives its tree: the widget's destructor runs detach()
        // against the dead tile.
    }
    PrintStatusWidget::destroy_formatter_for_test();
    CHECK(root_null);
    CHECK(thumb_null);
    CHECK(active_null);
    CHECK(row_null);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status thumbnail and idle paths skip a tree that died under a live owner",
                 "[print_status][panel_widget][teardown][uaf][1298]") {
    bool root_null = false;
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});
        lv_obj_t* page = make_page(test_screen());
        lv_obj_t* comp = make_print_status(page);
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        // The active thumbnail exists only while a print holds the machine.
        helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
        process_lvgl(10);
        process_lvgl(30);
        REQUIRE(Access::active_thumb(widget) != nullptr);

        lv_obj_delete(page);
        root_null = Access::widget_obj(widget) == nullptr;

        // The observers are still subscribed: a new active-print thumbnail and
        // an idle reset both reach the widget after its tree is gone.
        lv_subject_copy_string(get_printer_state().print_state().get_print_thumbnail_path_subject(),
                               "A:assets/images/benchy_thumbnail_white.png");
        Access::reset_to_idle(widget);
        process_lvgl(30);
    }
    PrintStatusWidget::destroy_formatter_for_test();
    CHECK(root_null);
}
