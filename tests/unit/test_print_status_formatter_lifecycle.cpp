// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_formatter_lifecycle.cpp
 * @brief The Detailed card's formatter singleton must survive being re-created.
 *
 * `s_formatter_` is refcounted by PrintStatusWidget's ctor/dtor. Removing the
 * last print-status widget from the dashboard drops the count to zero; adding
 * one back builds a second formatter. Both formatters publish the same thirteen
 * subject names into helix-xml's process-wide scope, and ~DetailedFormatter
 * withdraws those names — so the order in which the replacement is built and the
 * predecessor is destroyed decides whether the names still resolve afterwards.
 * If they do not, every bind_text on the Detailed card silently binds nothing
 * and the card renders blank for the rest of the session.
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

TEST_CASE_METHOD(HelixTestFixture, "Re-created DetailedFormatter keeps its XML subjects resolvable",
                 "[print_status][formatter]") {
    PrintStatusWidget::destroy_formatter_for_test();

    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);

    {
        PrintStatusWidget first;
        REQUIRE(lv_xml_get_subject(nullptr, "print_status_layer_text") != nullptr);
    }
    // Refcount is back to zero; the formatter is parked, not destroyed.

    PrintStatusWidget second;

    lv_subject_t* layer_text = lv_xml_get_subject(nullptr, "print_status_layer_text");
    REQUIRE(layer_text != nullptr);

    // The shared formatter prefixes "~" to a layer count it had to guess from
    // the progress fraction. Writing the subjects directly below bypasses the
    // setter that records real layer data, so state the precondition here
    // rather than assert whichever marker happens to fall out.
    PrinterPrintStateTestAccess::set_has_real_layer_data(
        PrinterStateTestAccess::get_print_state(ps), true);

    // ...and it is the live formatter's subject, not an orphaned record.
    lv_subject_set_int(ps.print_state().get_print_layer_current_subject(), 7);
    lv_subject_set_int(ps.print_state().get_print_layer_total_subject(), 9);
    UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
    CHECK(std::string(lv_subject_get_string(layer_text)) == "7 / 9");
}
