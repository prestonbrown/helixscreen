// tests/unit/test_print_status_lifecycle_seam.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam between PrintStatusPanel's private PrintLifecycleState and the
// published `print_lifecycle` subject.
//
// Before these, NOTHING exercised that seam: the 40 cases in
// test_print_lifecycle_state.cpp all drive the class in isolation, so they would
// keep passing against a panel that had stopped using it entirely. These pin the
// panel's observable behaviour so a change to where its state comes from is
// visible instead of silent.
//
// Two jobs:
//   1. Pin the Complete-screen freeze, which must survive the refactor. The
//      panel's latches are the reason a finished print still reads its final
//      numbers after Moonraker zeroes the underlying subjects on STANDBY.
//   2. Pin the agreement between the panel and the published subject. One of
//      these is expected to FAIL until 0b lands - that is the point.

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

namespace {

struct PanelLifecycleFixture : public LVGLTestFixture {
    PanelLifecycleFixture() {
        // Own the PrinterState: the panel registers observers against whatever
        // reference it is handed, and register_xml=false keeps these subjects
        // out of the process-wide XML registry, which outlives this frame.
        state_.init_subjects(false);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        // Load-bearing: PrintStatusPanel::on_print_start_phase_changed() returns
        // early unless subjects_initialized_ is set (ui_panel_print_status.cpp
        // :3044). Without this the phase axis never reaches the panel at all and
        // every agreement test below fails for a reason that has nothing to do
        // with the code under test.
        panel_->init_subjects();
        drain();
    }

    ~PanelLifecycleFixture() override {
        panel_.reset();
        helix::ui::UpdateQueue::instance().drain();
    }

    PrinterState& state() {
        return state_;
    }
    PrintStatusPanel& panel() {
        return *panel_;
    }

    /// Drain until quiescent, not once.
    ///
    /// A single drain() is NOT enough here: the panel's observers are
    /// observe_int_sync, so a handler that runs during a drain can queue further
    /// work, and that work is still pending when drain() returns. A one-shot
    /// drain left the panel one transition behind the published subject and made
    /// every agreement case below look like a production defect.
    static void drain() {
        for (int pass = 0; pass < 8; ++pass) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    /// Drive the printer's reported job state, the way Moonraker would.
    void report_job_state(const char* moonraker_state) {
        state_.update_from_status(json{{"print_stats", {{"state", moonraker_state}}}});
        drain();
    }

    /// Raise or clear a pre-print phase.
    void set_phase(helix::PrintStartPhase phase) {
        state_.set_print_start_state(phase, "", 0);
        drain();
    }

    /// What the app-wide authority says.
    PrintState published() const {
        return static_cast<PrintState>(
            lv_subject_get_int(const_cast<PrinterState&>(state_).get_print_lifecycle_subject()));
    }

    /// What the panel believes.
    PrintState panel_state() const {
        return panel_->get_state();
    }

    PrinterState state_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

// ============================================================================
// Agreement
// ============================================================================

TEST_CASE_METHOD(PanelLifecycleFixture, "panel and published lifecycle agree on a plain print",
                 "[print_status][lifecycle_seam]") {
    report_job_state("printing");
    REQUIRE(published() == PrintState::Printing);
    REQUIRE(panel_state() == published());

    report_job_state("paused");
    REQUIRE(published() == PrintState::Paused);
    REQUIRE(panel_state() == published());
}

TEST_CASE_METHOD(PanelLifecycleFixture,
                 "panel and published lifecycle agree during a host-side preparing window",
                 "[print_status][lifecycle_seam]") {
    // The printer has not accepted the job yet, so print_stats still reads a
    // terminal state while a pre-print phase runs in front of the job.
    report_job_state("complete");
    set_phase(helix::PrintStartPhase::BED_MESH);

    REQUIRE(published() == PrintState::Preparing);
    REQUIRE(panel_state() == published());
}

TEST_CASE_METHOD(PanelLifecycleFixture,
                 "panel and published lifecycle agree during a firmware-side PRINT_START",
                 "[print_status][lifecycle_seam]") {
    // The case derive_print_state() exists for: Klipper already reports
    // `printing` because the pre-print work lives inside PRINT_START, and a live
    // phase outranks the job state.
    //
    // PrintLifecycleState::on_job_state_changed() derives with a hard-coded
    // start_phase=0, so the panel moves Preparing -> Printing here while the
    // published subject correctly stays Preparing. They then disagree for the
    // whole remainder of PRINT_START.
    set_phase(helix::PrintStartPhase::HOMING);
    REQUIRE(published() == PrintState::Preparing);

    report_job_state("printing");

    REQUIRE(published() == PrintState::Preparing);
    REQUIRE(panel_state() == published());
}

TEST_CASE_METHOD(PanelLifecycleFixture, "clearing the phase hands off to the reported job state",
                 "[print_status][lifecycle_seam]") {
    set_phase(helix::PrintStartPhase::HOMING);
    report_job_state("printing");

    set_phase(helix::PrintStartPhase::IDLE);

    REQUIRE(published() == PrintState::Printing);
    REQUIRE(panel_state() == published());
}

// ============================================================================
// The Complete freeze - must survive 0b unchanged
// ============================================================================

TEST_CASE_METHOD(PanelLifecycleFixture, "a completed print keeps its numbers when Moonraker zeroes",
                 "[print_status][lifecycle_seam][freeze]") {
    // This is what the panel's latches buy, and why deleting them would be a
    // regression rather than a simplification. Moonraker reports zeroed
    // progress/layers the moment a finished print settles to STANDBY; the
    // Complete screen must keep showing what the print actually did.
    state_.update_from_status(json{{"print_stats",
                                    {{"state", "printing"},
                                     {"print_duration", 600.0},
                                     {"info", {{"current_layer", 240}, {"total_layer", 240}}}}},
                                   {"display_status", {{"progress", 0.73}}}});
    drain();
    REQUIRE(panel_state() == PrintState::Printing);
    REQUIRE(panel_->get_progress() == 73);

    state_.update_from_status(json{{"print_stats", {{"state", "complete"}}}});
    drain();
    REQUIRE(panel_state() == PrintState::Complete);

    // Moonraker now zeroes everything as the job settles.
    state_.update_from_status(json{{"print_stats",
                                    {{"state", "standby"},
                                     {"print_duration", 0.0},
                                     {"info", {{"current_layer", 0}, {"total_layer", 0}}}}},
                                   {"display_status", {{"progress", 0.0}}}});
    drain();

    // The freeze, asserted positively on the value it actually protects.
    //
    // An earlier version asserted only `panel_state() != PrintState::Printing`,
    // which is a tautology: derive_print_state(STANDBY, 0) is Idle under every
    // possible implementation, so six of the seven enum values satisfied it and
    // deleting every latch in the panel left it green.
    //
    // The lifecycle DOES go Complete -> Idle here, by design - that is the
    // `print_ended` edge (print_lifecycle_state.cpp), which is what releases the
    // thumbnail, gcode and viewer. The Complete SCREEN outlives it via
    // print_outcome, not via this enum. What must not move is the latched
    // progress. Note the expected value is 100, not the 73 last reported:
    // should_freeze_complete forces the terminal values on the ->Complete edge,
    // which is why a finished print reads 100% / 240 of 240 / 0s rather than
    // whatever the final sample happened to catch. Moonraker has since reported
    // 0 and the panel must still say 100.
    REQUIRE(panel_state() == PrintState::Idle);
    REQUIRE(panel_->get_progress() == 100);
}

// ============================================================================
// The per-job resets — a side effect, not a state value
//
// Every other test in this file asserts which STATE the panel holds. That is
// exactly what missed this: for a print started in-app the panel is already
// Preparing when Moonraker reports printing, so on_job_state_changed() derives
// Preparing == current, returns state_changed=false and bails before the resets.
// Panel and published lifecycle still AGREE the whole way, so an agreement
// assertion passes while every side effect behind that early return is skipped.
// ============================================================================

TEST_CASE_METHOD(PanelLifecycleFixture,
                 "a print started in-app still clears the previous print's end-overlay dismissal",
                 "[print_status][lifecycle_seam][resets]") {
    lv_subject_t* dismissed = lv_xml_get_subject(nullptr, "end_overlay_dismissed");
    REQUIRE(dismissed != nullptr);

    // Print A finishes and the user dismisses its end overlay.
    report_job_state("printing");
    report_job_state("complete");
    lv_subject_set_int(dismissed, 1);
    drain();
    REQUIRE(lv_subject_get_int(dismissed) == 1);

    // Print B is started FROM THE PANEL: the phase is raised before the printer
    // reports anything, which is what makes the job-state edge invisible.
    set_phase(helix::PrintStartPhase::INITIALIZING);
    REQUIRE(panel_state() == PrintState::Preparing);

    report_job_state("printing");
    REQUIRE(panel_state() == PrintState::Preparing); // phase still outranks

    // PRINT_START ends. This is the only edge that sees print B at all.
    set_phase(helix::PrintStartPhase::IDLE);
    REQUIRE(panel_state() == PrintState::Printing);

    // Print B must be able to show its own outcome.
    REQUIRE(lv_subject_get_int(dismissed) == 0);
}

TEST_CASE_METHOD(PanelLifecycleFixture, "a terminal state does not reset to Idle on its own",
                 "[print_status][lifecycle_seam][freeze]") {
    // Complete must persist so the badge and Reprint button stay reachable; it
    // is the arrival of a NEW print that clears it, not the printer settling.
    report_job_state("printing");
    report_job_state("complete");
    REQUIRE(panel_state() == PrintState::Complete);

    // A redundant re-report of the same terminal state changes nothing.
    report_job_state("complete");
    REQUIRE(panel_state() == PrintState::Complete);
}
