// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_lazy_active_branch.cpp
 * @brief The print_status card builds its active views only while a print holds the
 *        machine, and the widget rebinds them each time they are built.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/print_status_widget_test_access.h"
#include "app_globals.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = PrintStatusWidgetTestAccess;

namespace {

constexpr const char* kThumbA = "A:assets/images/printer_200.png";

lv_obj_t* make_print_status(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "panel_widget_print_status", nullptr));
}

std::string image_src(lv_obj_t* img) {
    const void* src = img ? lv_image_get_src(img) : nullptr;
    return src ? std::string(static_cast<const char*>(src)) : std::string();
}

void set_print(PrintJobState s) {
    helix::test::set_wire_state(get_printer_state(), s);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status builds its active views for a print and drops them after",
                 "[print_status][panel_widget][lazy_branch]") {
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});
        lv_obj_t* comp = make_print_status(test_screen());
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        widget.on_size_changed(2, 2, 400, 400);
        process_lvgl(30);

        // Idle: the active views are not built, and nothing points into them.
        CHECK(lv_obj_find_by_name(comp, "print_card_printing") == nullptr);
        CHECK(Access::active_thumb(widget) == nullptr);

        // A thumbnail known before the print starts reaches the active view once built.
        get_printer_state().print_state().set_print_thumbnail("a.gcode", kThumbA);
        set_print(PrintJobState::PRINTING);
        process_lvgl(30);
        lv_obj_t* active_thumb = lv_obj_find_by_name(comp, "print_card_active_thumb");
        REQUIRE(active_thumb != nullptr);
        CHECK(Access::active_thumb(widget) == active_thumb);
        CHECK(image_src(active_thumb) == kThumbA);
        CHECK(lv_obj_find_by_name(comp, "detailed_progress_arc") != nullptr);

        // The print ends: the active views go, and late updates find nothing to touch.
        set_print(PrintJobState::COMPLETE);
        set_print(PrintJobState::STANDBY);
        process_lvgl(30);
        CHECK(lv_obj_find_by_name(comp, "print_card_printing") == nullptr);
        CHECK(Access::active_thumb(widget) == nullptr);
        get_printer_state().print_state().set_print_thumbnail("b.gcode", kThumbA);
        process_lvgl(30);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status rebinds a rebuilt active view when its layout changes mid-print",
                 "[print_status][panel_widget][lazy_branch]") {
    {
        PrintStatusWidget widget;
        widget.set_config({{"layout_style", "library"}});
        lv_obj_t* comp = make_print_status(test_screen());
        REQUIRE(comp != nullptr);
        widget.attach(comp, test_screen());
        widget.on_size_changed(2, 2, 400, 400);
        get_printer_state().print_state().set_print_thumbnail("a.gcode", kThumbA);
        set_print(PrintJobState::PRINTING);
        process_lvgl(30);
        lv_obj_t* first = lv_obj_find_by_name(comp, "print_card_active_thumb");
        REQUIRE(first != nullptr);

        // Library to detailed while printing rebuilds the active views.
        widget.set_config({{"layout_style", "detailed"}});
        widget.on_size_changed(2, 2, 401, 400);
        process_lvgl(30);
        REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) == 4);
        lv_obj_t* rebuilt = lv_obj_find_by_name(comp, "print_card_active_thumb");
        REQUIRE(rebuilt != nullptr);
        CHECK(Access::active_thumb(widget) == rebuilt);
        CHECK(image_src(rebuilt) == kThumbA);

        set_print(PrintJobState::STANDBY);
        process_lvgl(30);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}

// The view subject is shared, so one card's layout change rebuilds every card's active
// views; a card asked again for the view it already shows must bind the rebuilt ones.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "print_status rebinds active views another card's change rebuilt",
                 "[print_status][panel_widget][lazy_branch]") {
    {
        PrintStatusWidget a;
        PrintStatusWidget b;
        a.set_config({{"layout_style", "library"}});
        b.set_config({{"layout_style", "library"}});
        lv_obj_t* comp_a = make_print_status(test_screen());
        lv_obj_t* comp_b = make_print_status(test_screen());
        REQUIRE(comp_a != nullptr);
        REQUIRE(comp_b != nullptr);
        a.attach(comp_a, test_screen());
        b.attach(comp_b, test_screen());
        a.on_size_changed(2, 2, 400, 400);
        b.on_size_changed(2, 2, 400, 400);
        get_printer_state().print_state().set_print_thumbnail("a.gcode", kThumbA);
        set_print(PrintJobState::PRINTING);
        process_lvgl(30);
        REQUIRE(Access::active_thumb(a) != nullptr);

        // b goes to detailed and back: a's active views are built twice over.
        b.set_config({{"layout_style", "detailed"}});
        b.on_size_changed(2, 2, 401, 400);
        process_lvgl(30);
        b.set_config({{"layout_style", "library"}});
        b.on_size_changed(2, 2, 400, 400);
        process_lvgl(30);
        REQUIRE(lv_subject_get_int(PrintStatusWidget::view_subject_for_test()) == 3);

        a.on_size_changed(2, 2, 401, 400);
        process_lvgl(30);
        lv_obj_t* current = lv_obj_find_by_name(comp_a, "print_card_active_thumb");
        REQUIRE(current != nullptr);
        CHECK(Access::active_thumb(a) == current);
        CHECK(image_src(current) == kThumbA);

        set_print(PrintJobState::STANDBY);
        process_lvgl(30);
    }
    PrintStatusWidget::destroy_formatter_for_test();
}
