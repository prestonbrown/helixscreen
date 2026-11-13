// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_widget_edit_exit.cpp
 * @brief Leaving home edit mode must not re-arm a Print Last row that history
 *        disabled during the session.
 *
 * Edit mode takes CLICKABLE from every object on the page and gives it back to
 * exactly those objects on exit. The Print Last rows' clickability follows the
 * print history, so a job that disappeared mid-session leaves a row the restore
 * would wrongly re-arm; the widget's on_edit_mode_exited() re-applies it.
 */

#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = PrintStatusWidgetTestAccess;

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status: leaving edit mode re-applies Print Last availability",
                 "[print_status][panel_widget][grid_edit]") {
    bool clickable_after_restore = false;
    bool clickable_after_hook = true;
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});
        lv_obj_t* page = lv_obj_create(test_screen());
        lv_obj_set_size(page, 800, 600);
        lv_obj_t* comp =
            static_cast<lv_obj_t*>(lv_xml_create(page, "panel_widget_print_status", nullptr));
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        process_lvgl(30);
        lv_obj_t* row = Access::library_row_last(widget);
        REQUIRE(row != nullptr);

        // A job existed when edit mode began, so the row was armed and the
        // session disarmed it. The test environment has no history, so the job
        // is gone by the time the session ends.
        lv_obj_set_clickable(row, true);
        helix::ui::disable_widget_clicks_recursive(page);
        REQUIRE_FALSE(lv_obj_is_clickable(row));

        helix::ui::enable_widget_clicks_recursive(page);
        clickable_after_restore = lv_obj_is_clickable(row);
        widget.on_edit_mode_exited();
        clickable_after_hook = lv_obj_is_clickable(row);

        widget.detach();
        lv_obj_delete(page);
    }
    PrintStatusWidget::destroy_formatter_for_test();
    REQUIRE(clickable_after_restore); // the restore alone re-arms it
    CHECK_FALSE(clickable_after_hook);
}
