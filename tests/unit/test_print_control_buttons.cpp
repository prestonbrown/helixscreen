// Copyright (C) 2025-2026 356C LLC
// tests/unit/test_print_control_buttons.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_control_buttons_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "print_control_buttons.h"
#include "printer_state.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

struct ControlButtonsFixture : LVGLTestFixture {
    ControlButtonsFixture() {
        // Tear the controller down FIRST: its observer is registered on the
        // print_state_enum subject. PrinterStateTestAccess::reset() deinits that
        // subject (freeing its observers in LVGL); resetting the controller
        // observer afterwards would lv_observer_remove() a freed observer (UAF
        // across sequential test cases — the singleton persists).
        helix::ui::PrintControlButtonsTestAccess::reset();

        // Fresh PrinterState subjects so print_state_enum is valid to observe.
        helix::PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);

        helix::ui::PrintControlButtons::instance().init_subjects();
    }
};

int read_int(const char* name) {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name));
}
std::string read_str(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}
void set_print_state(helix::PrintJobState s) {
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(s));
    // observe<int> defers via queue_update — drain so the controller reacts.
    helix::ui::UpdateQueue::instance().drain_until_empty();
}

} // namespace

TEST_CASE_METHOD(ControlButtonsFixture, "controller registers global subjects",
                 "[print_control][slow]") {
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_primary_icon") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_primary_label") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_primary_enabled") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_control_stop_enabled") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "print_pending_action") != nullptr);
}

TEST_CASE_METHOD(ControlButtonsFixture, "primary label tracks print state",
                 "[print_control][slow]") {
    set_print_state(helix::PrintJobState::PRINTING);
    REQUIRE(read_str("print_control_primary_label") == "Pause");
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(read_str("print_control_primary_label") == "Resume");
    set_print_state(helix::PrintJobState::STANDBY);
    REQUIRE(read_str("print_control_primary_label") == "Pause");
    REQUIRE(read_int("print_control_primary_enabled") == 0);
    REQUIRE(read_int("print_control_stop_enabled") == 0);
}

// Enable state depends on StandardMacros slot availability. In the unit-test
// harness no printer discovery runs, so the Pause/Resume/Cancel slots are
// EMPTY (kFallbackMacros has "" for all three, and init() is never called).
// The pure enable logic (active && pending==None && slot_ok) is fully covered
// by Task 1's [print_control_view] tests; here we assert the controller wires
// the slot-availability gate through correctly: with empty slots, the primary
// button is disabled even while PRINTING/PAUSED.
TEST_CASE_METHOD(ControlButtonsFixture, "primary disabled when macro slots empty",
                 "[print_control][slow]") {
    set_print_state(helix::PrintJobState::PRINTING);
    REQUIRE(read_int("print_control_primary_enabled") == 0);
    REQUIRE(read_int("print_control_stop_enabled") == 0);
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(read_int("print_control_primary_enabled") == 0);
}

// Optimistic pending action drives the icon/label to the "...ing" form and
// publishes the pending_action subject, independent of macro availability.
TEST_CASE_METHOD(ControlButtonsFixture, "pending action publishes hourglass state",
                 "[print_control][slow]") {
    set_print_state(helix::PrintJobState::PRINTING);
    helix::ui::PrintControlButtonsTestAccess::set_pending(helix::ui::PendingAction::Pausing);
    REQUIRE(read_int("print_pending_action") ==
            static_cast<int>(helix::ui::PendingAction::Pausing));
    REQUIRE(read_str("print_control_primary_label") == "Pausing...");

    // A real state change is authoritative and clears the optimistic pending.
    set_print_state(helix::PrintJobState::PAUSED);
    REQUIRE(read_int("print_pending_action") == static_cast<int>(helix::ui::PendingAction::None));
    REQUIRE(read_str("print_control_primary_label") == "Resume");
}

// While a pending action is in flight the primary button is disabled (you cannot
// re-trigger Pause while a Pause is mid-flight); the real state arrival clears it.
TEST_CASE_METHOD(ControlButtonsFixture, "pending action set then cleared on state arrival",
                 "[print_control][slow]") {
    using helix::ui::PendingAction;
    using helix::ui::PrintControlButtonsTestAccess;

    set_print_state(helix::PrintJobState::PRINTING);
    PrintControlButtonsTestAccess::set_pending(PendingAction::Pausing);
    REQUIRE(read_int("print_pending_action") == static_cast<int>(PendingAction::Pausing));
    REQUIRE(read_str("print_control_primary_label") == "Pausing...");
    REQUIRE(read_int("print_control_primary_enabled") == 0); // disabled in flight

    set_print_state(helix::PrintJobState::PAUSED); // real state arrives -> observer clears pending
    REQUIRE(read_int("print_pending_action") == static_cast<int>(PendingAction::None));
    REQUIRE(read_str("print_control_primary_label") == "Resume");
}

// Klipper rejects a RESUME by broadcasting `!!` and returning normally — the
// gcode.script RPC still answers `ok`, so dispatch_prepared_resume's error path
// never runs and the optimistic spinner is left to the 150s backstop. For 150s
// the button reads "Resuming..." AND is disabled, so the user cannot retry even
// after clearing the runout (bundle JX2FVRB9: RESUME at 07:33:29 rejected for
// filament-out, pending finally cleared at 07:35:59).
//
// Mutation check: make notify_printer_error() a no-op and the first REQUIRE in
// "klipper error releases a pending resume" fails with pending == Resuming.
TEST_CASE_METHOD(ControlButtonsFixture, "klipper error releases a pending resume",
                 "[print_control][slow]") {
    using helix::ui::PendingAction;
    using helix::ui::PrintControlButtons;
    using helix::ui::PrintControlButtonsTestAccess;

    set_print_state(helix::PrintJobState::PAUSED);
    PrintControlButtonsTestAccess::set_pending(PendingAction::Resuming);
    REQUIRE(read_str("print_control_primary_label") == "Resuming...");
    REQUIRE(read_int("print_control_primary_enabled") == 0);

    // The `!!` line Klipper actually emitted on the reporter's AD5X.
    PrintControlButtons::instance().notify_printer_error(
        "\"head_switch_sensor\" has detected that the filament has run out, "
        "please load it and press RESUME");

    REQUIRE(PrintControlButtonsTestAccess::pending() == PendingAction::None);
    REQUIRE(read_int("print_pending_action") == static_cast<int>(PendingAction::None));
    // Back to the actionable label. primary_enabled is NOT asserted here: this
    // harness runs no printer discovery, so the Resume macro slot is empty and
    // the enable term is pinned to 0 regardless (see "primary disabled when
    // macro slots empty"). The pending->enabled half lives in
    // [print_control_view], where the slot availability is an input.
    REQUIRE(read_str("print_control_primary_label") == "Resume");
}

TEST_CASE_METHOD(ControlButtonsFixture, "klipper error also releases a pending pause",
                 "[print_control][slow]") {
    using helix::ui::PendingAction;
    using helix::ui::PrintControlButtons;
    using helix::ui::PrintControlButtonsTestAccess;

    set_print_state(helix::PrintJobState::PRINTING);
    PrintControlButtonsTestAccess::set_pending(PendingAction::Pausing);
    REQUIRE(read_str("print_control_primary_label") == "Pausing...");

    PrintControlButtons::instance().notify_printer_error("Move out of range");

    REQUIRE(PrintControlButtonsTestAccess::pending() == PendingAction::None);
    REQUIRE(read_str("print_control_primary_label") == "Pause");
}

// Klipper broadcasts `!!` for plenty of things that have nothing to do with the
// print-control buttons. With nothing pending there is no state to disturb.
TEST_CASE_METHOD(ControlButtonsFixture, "klipper error with nothing pending is inert",
                 "[print_control][slow]") {
    using helix::ui::PendingAction;
    using helix::ui::PrintControlButtons;
    using helix::ui::PrintControlButtonsTestAccess;

    set_print_state(helix::PrintJobState::PRINTING);
    REQUIRE(PrintControlButtonsTestAccess::pending() == PendingAction::None);

    PrintControlButtons::instance().notify_printer_error("Heater extruder not heating at expected "
                                                         "rate");

    REQUIRE(PrintControlButtonsTestAccess::pending() == PendingAction::None);
    REQUIRE(read_str("print_control_primary_label") == "Pause");
}

// Regression test for the nightly [slow] crash (segfault in lv_observer_remove
// during ~PrintControlButtons at process exit, plus mid-run
// "malloc(): unaligned tcache chunk detected" aborts).
//
// PrintControlButtons is a process-lifetime singleton that observes the GLOBAL
// print_state_enum subject, which is owned by PrinterState and gets torn down
// per-test (PrinterStateTestAccess::reset) and at process exit. If the singleton
// is left observing a subject that is then deinited, lv_subject_deinit frees the
// observer node and the singleton's ObserverGuard is left with a dangling
// lv_observer_t*; the next teardown that walks it calls lv_observer_remove() on
// freed memory.
//
// The fix: reset_all() (run at every test boundary, while subjects are still
// alive) detaches the controller's observer. This test asserts that invariant.
// Deliberately does NOT use ControlButtonsFixture — it drives the lifecycle by
// hand so the assertion targets reset_all() directly. Pre-fix, has_observer()
// stays true after reset_all() and the REQUIRE_FALSE fails (and the follow-on
// deinit_subjects() walks freed memory).
TEST_CASE_METHOD(LVGLTestFixture,
                 "reset_all detaches PrintControlButtons observer before subject teardown",
                 "[print_control][slow]") {
    using helix::ui::PrintControlButtons;
    using helix::ui::PrintControlButtonsTestAccess;

    // Stand up fresh PrinterState subjects and the controller observing them,
    // mirroring what a ControlButtonsFixture-based test leaves behind.
    helix::PrinterStateTestAccess::reset(get_printer_state());
    get_printer_state().init_subjects(false);
    PrintControlButtons::instance().init_subjects();
    REQUIRE(PrintControlButtonsTestAccess::has_observer()); // precondition: observer is live

    // reset_all() must detach the controller while print_state_enum is alive.
    HelixTestFixture::reset_all();
    REQUIRE_FALSE(PrintControlButtonsTestAccess::has_observer());

    // With the observer detached, tearing the subject down (what a later test or
    // process exit does) no longer walks a dangling lv_observer_t*. Pre-fix this
    // is where the SIGSEGV / heap corruption happened.
    REQUIRE_NOTHROW(get_printer_state().deinit_subjects());
}
