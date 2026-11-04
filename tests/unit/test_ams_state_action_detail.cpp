// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_action_detail.cpp
 * @brief Tests for AmsState::ams_action_detail subject derivation
 *
 * Verifies the priority logic that derives the user-visible AMS status
 * label from the combined view of AmsState and PrinterState:
 *   1. backend operation_detail (non-empty)
 *   2. ams_action != IDLE → action string
 *   3. PrintJobState::PRINTING → "Printing now"
 *   4. PrintJobState::PAUSED  → "Paused"
 *   5. otherwise              → "Idle"
 *
 * Also verifies the observer wired to the print state subject so the
 * label refreshes without waiting for the next sync_from_backend().
 */

#include "../lvgl_test_fixture.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

// Helper: read the current ams_action_detail subject string.
std::string detail_text() {
    return std::string(lv_subject_get_string(AmsState::instance().get_ams_action_detail_subject()));
}

// Helper: set print state via Moonraker-side subject + drain queue.
void set_print_state(PrintJobState state) {
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(state));
    helix::ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState::ams_action_detail priority",
                 "[ams][ams_state][action_detail]") {
    auto& ams = AmsState::instance();
    auto& printer = get_printer_state();

    // PrinterState MUST be initialized before AmsState: AmsState's
    // install_print_state_observer() binds to PrinterState's print_state_enum
    // subject, which must already be live. This mirrors the production init
    // order in subject_initializer.cpp (PrinterState first, then AmsState).
    printer.init_subjects(false);
    ams.init_subjects(false);

    // Start in a known clean baseline.
    set_print_state(PrintJobState::STANDBY);
    ams.set_action(AmsAction::IDLE);
    ams.set_action_detail("");

    SECTION("action != IDLE wins regardless of print state (printing)") {
        set_print_state(PrintJobState::PRINTING);
        ams.set_action(AmsAction::LOADING);
        // The observer recomputes when the action changes.
        helix::ui::UpdateQueue::instance().drain();

        CHECK(detail_text() == "Loading");
    }

    SECTION("backend operation_detail wins when set (printing)") {
        set_print_state(PrintJobState::PRINTING);
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("Waiting for slot 2");
        helix::ui::UpdateQueue::instance().drain();

        CHECK(detail_text() == "Waiting for slot 2");
    }

    SECTION("IDLE + empty detail + PRINTING -> Printing now") {
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::PRINTING);

        CHECK(detail_text() == "Printing now");
    }

    SECTION("IDLE + empty detail + PAUSED -> Paused") {
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::PAUSED);

        CHECK(detail_text() == "Paused");
    }

    SECTION("IDLE + empty detail + STANDBY -> Idle") {
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::STANDBY);

        CHECK(detail_text() == "Idle");
    }

    SECTION("Observer flips label when print state changes mid-session") {
        // Start idle + standby: label should read "Idle".
        ams.set_action(AmsAction::IDLE);
        ams.set_action_detail("");
        set_print_state(PrintJobState::STANDBY);
        REQUIRE(detail_text() == "Idle");

        // Transition to PRINTING — observer should rerun derivation
        // without anyone calling sync_from_backend() or set_action()/set_action_detail().
        set_print_state(PrintJobState::PRINTING);
        CHECK(detail_text() == "Printing now");

        // Pause it.
        set_print_state(PrintJobState::PAUSED);
        CHECK(detail_text() == "Paused");

        // Back to standby.
        set_print_state(PrintJobState::STANDBY);
        CHECK(detail_text() == "Idle");
    }

    // Restore clean state for any subsequent tests in the suite.
    set_print_state(PrintJobState::STANDBY);
    ams.set_action(AmsAction::IDLE);
    ams.set_action_detail("");
}
