// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_formatter_lifecycle.cpp
 * @brief The Detailed card's shared formatter keeps its XML subjects resolvable.
 *
 * The first PrintStatusWidget builds `s_formatter_`; it stays up while widgets
 * come and go, and its StaticSubjectRegistry entry destroys it, after which the
 * next widget builds a replacement. Both formatters publish the same thirteen
 * subject names into helix-xml's process-wide scope, and ~DetailedFormatter
 * withdraws those names, so a replacement built before its predecessor is gone
 * would lose them. If they do not resolve, every bind_text on the Detailed card
 * silently binds nothing and the card renders blank for the rest of the session.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

TEST_CASE_METHOD(HelixTestFixture, "DetailedFormatter keeps its XML subjects across widgets",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    // The formatter prefixes "~" to a layer count guessed from the progress
    // fraction; the direct subject writes below skip the real-layer setter.
    PrinterPrintStateTestAccess::set_has_real_layer_data(
        PrinterStateTestAccess::get_print_state(ps), true);
    auto layer_text_after = [&ps](int current, int total) {
        lv_subject_t* layer_text = lv_xml_get_subject(nullptr, "print_status_layer_text");
        REQUIRE(layer_text != nullptr);
        lv_subject_set_int(ps.print_state().get_print_layer_current_subject(), current);
        lv_subject_set_int(ps.print_state().get_print_layer_total_subject(), total);
        UpdateQueue::instance().drain_until_empty();
        return std::string(lv_subject_get_string(layer_text));
    };

    { PrintStatusWidget first; }
    // The last widget is gone; the formatter is still publishing.
    CHECK(layer_text_after(3, 5) == "3 / 5");

    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        "PrintStatusWidgetDetailedFormatter", StaticSubjectRegistry::Scope::PrinterSwitch));
    CHECK(lv_xml_get_subject(nullptr, "print_status_layer_text") == nullptr);

    // The next widget builds a replacement that owns the names.
    PrintStatusWidget second;
    CHECK(layer_text_after(7, 9) == "7 / 9");
}
