// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_print_lifecycle_state.cpp
 * @brief Unit tests for PrintLifecycleState — pure-logic state machine (no LVGL)
 *
 * Tests state transitions, race condition guards, preparing phase,
 * gcode_loaded lifecycle, and viewer visibility.
 */

#include "print_lifecycle_state.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Test Access — friend class declared in PrintLifecycleState
// ============================================================================

class PrintLifecycleStateTestAccess {
  public:
    static void set_state(PrintLifecycleState& s, PrintState state) {
        s.current_state_ = state;
    }
    static void set_progress(PrintLifecycleState& s, int p) {
        s.current_progress_ = p;
    }
    static void set_layers(PrintLifecycleState& s, int cur, int total) {
        s.current_layer_ = cur;
        s.total_layers_ = total;
    }
    static void set_elapsed(PrintLifecycleState& s, int secs) {
        s.elapsed_seconds_ = secs;
    }
    static void set_remaining(PrintLifecycleState& s, int secs) {
        s.remaining_seconds_ = secs;
    }
    static void set_gcode_loaded(PrintLifecycleState& s, bool loaded) {
        s.gcode_loaded_ = loaded;
    }
};

using TA = PrintLifecycleStateTestAccess;

// ============================================================================
// Race Condition Tests — known bugs where stale zero values arrive late
// ============================================================================

TEST_CASE("Progress=0 before Complete does not reset display", "[lifecycle][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_progress(sm, 75);

    // Zero progress while still Printing is valid (e.g. Moonraker reset)
    REQUIRE(sm.on_progress_changed(0) == true);
    REQUIRE(sm.progress() == 0);

    // Transition to Complete freezes at 100
    auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(result.state_changed == true);
    REQUIRE(sm.progress() == 100);

    // Late zero after Complete is guarded
    REQUIRE(sm.on_progress_changed(0) == false);
    REQUIRE(sm.progress() == 100);
}

TEST_CASE("Layer=0 before Complete does not reset display", "[lifecycle][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_layers(sm, 50, 100);

    // Transition to Complete snaps layer to total
    auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(result.state_changed == true);
    REQUIRE(sm.current_layer() == 100);

    // Late layer=0 after Complete is guarded
    REQUIRE(sm.on_layer_changed(0, 100, true) == false);
    REQUIRE(sm.current_layer() == 100);
}

TEST_CASE("Duration=0 after Complete does not reset elapsed", "[lifecycle][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_elapsed(sm, 3600);

    // Transition to Complete via on_job_state_changed
    auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(result.state_changed == true);
    REQUIRE(sm.elapsed_seconds() == 3600);

    // Late duration=0 after Complete is guarded
    REQUIRE(sm.on_duration_changed(0, PrintOutcome::NONE) == false);
    REQUIRE(sm.elapsed_seconds() == 3600);
}

TEST_CASE("Progress=0 after Complete is guarded", "[lifecycle][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_progress(sm, 75);

    sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);

    REQUIRE(sm.on_progress_changed(0) == false);
    REQUIRE(sm.progress() == 100);
}

TEST_CASE("Progress=0 after Cancelled is guarded", "[lifecycle][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_progress(sm, 42);

    sm.on_job_state_changed(PrintJobState::CANCELLED, PrintOutcome::CANCELLED);

    REQUIRE(sm.on_progress_changed(0) == false);
    REQUIRE(sm.progress() == 42);
}

TEST_CASE("Progress=0 after Error is guarded", "[lifecycle][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_progress(sm, 60);

    sm.on_job_state_changed(PrintJobState::ERROR, PrintOutcome::ERROR);

    REQUIRE(sm.on_progress_changed(0) == false);
    REQUIRE(sm.progress() == 60);
}

TEST_CASE("Data updates rejected in Idle state", "[lifecycle][race]") {
    // After Complete→Idle, Moonraker sends zeroed values in the same batch.
    // These must be rejected so the frozen display persists.
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_progress(sm, 75);
    TA::set_layers(sm, 50, 100);
    TA::set_elapsed(sm, 3600);

    // Complete freezes values
    sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(sm.progress() == 100);
    REQUIRE(sm.current_layer() == 100);

    // Transition to Idle
    sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
    REQUIRE(sm.state() == PrintState::Idle);

    // Moonraker's zeroed values arrive while in Idle — must be rejected
    REQUIRE(sm.on_progress_changed(0) == false);
    REQUIRE(sm.progress() == 100);

    REQUIRE(sm.on_layer_changed(0, 100, true) == false);
    REQUIRE(sm.current_layer() == 100);

    REQUIRE(sm.on_duration_changed(0, PrintOutcome::NONE) == false);
    REQUIRE(sm.elapsed_seconds() == 3600);

    REQUIRE(sm.on_time_left_changed(0, PrintOutcome::NONE) == false);
}

// ============================================================================
// State Transition Tests
// ============================================================================

TEST_CASE("STANDBY transitions to Idle", "[lifecycle][state]") {
    PrintLifecycleState sm;
    // Start from Printing so we actually see a state change
    TA::set_state(sm, PrintState::Printing);

    auto result = sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
    REQUIRE(result.state_changed == true);
    REQUIRE(result.new_state == PrintState::Idle);
}

TEST_CASE("PRINTING transitions to Printing", "[lifecycle][state]") {
    PrintLifecycleState sm;
    auto result = sm.on_job_state_changed(PrintJobState::PRINTING, PrintOutcome::NONE);
    REQUIRE(result.state_changed == true);
    REQUIRE(result.new_state == PrintState::Printing);
}

TEST_CASE("PAUSED transitions to Paused", "[lifecycle][state]") {
    PrintLifecycleState sm;
    auto result = sm.on_job_state_changed(PrintJobState::PAUSED, PrintOutcome::NONE);
    REQUIRE(result.state_changed == true);
    REQUIRE(result.new_state == PrintState::Paused);
}

TEST_CASE("COMPLETE transitions to Complete", "[lifecycle][state]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);

    auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(result.state_changed == true);
    REQUIRE(result.new_state == PrintState::Complete);
    REQUIRE(result.should_freeze_complete == true);
}

TEST_CASE("CANCELLED transitions to Cancelled", "[lifecycle][state]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);

    auto result = sm.on_job_state_changed(PrintJobState::CANCELLED, PrintOutcome::CANCELLED);
    REQUIRE(result.state_changed == true);
    REQUIRE(result.new_state == PrintState::Cancelled);
    REQUIRE(result.should_animate_cancelled == true);
}

TEST_CASE("ERROR transitions to Error", "[lifecycle][state]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);

    auto result = sm.on_job_state_changed(PrintJobState::ERROR, PrintOutcome::ERROR);
    REQUIRE(result.state_changed == true);
    REQUIRE(result.new_state == PrintState::Error);
    REQUIRE(result.should_animate_error == true);
}

TEST_CASE("Same state does not trigger change", "[lifecycle][state]") {
    PrintLifecycleState sm;
    // First transition to Printing
    auto r1 = sm.on_job_state_changed(PrintJobState::PRINTING, PrintOutcome::NONE);
    REQUIRE(r1.state_changed == true);

    // Same state again
    auto r2 = sm.on_job_state_changed(PrintJobState::PRINTING, PrintOutcome::NONE);
    REQUIRE(r2.state_changed == false);
}

TEST_CASE("Complete sets progress=100, remaining=0, freezes elapsed", "[lifecycle][state]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_progress(sm, 75);
    TA::set_elapsed(sm, 3600);
    TA::set_remaining(sm, 600);
    TA::set_layers(sm, 50, 100);

    auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(result.state_changed == true);

    REQUIRE(sm.progress() == 100);
    REQUIRE(sm.remaining_seconds() == 0);
    REQUIRE(sm.elapsed_seconds() == 3600);
    REQUIRE(sm.current_layer() == 100);
}

TEST_CASE("New print (Idle->Printing) sets should_reset_progress_bar=true", "[lifecycle][state]") {
    PrintLifecycleState sm;
    // Default state is Idle
    auto result = sm.on_job_state_changed(PrintJobState::PRINTING, PrintOutcome::NONE);
    REQUIRE(result.should_reset_progress_bar == true);
}

TEST_CASE("Resume (Paused->Printing) sets should_reset_progress_bar=false", "[lifecycle][state]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Paused);

    auto result = sm.on_job_state_changed(PrintJobState::PRINTING, PrintOutcome::NONE);
    REQUIRE(result.should_reset_progress_bar == false);
}

// ============================================================================
// Preparing State Tests
// ============================================================================

TEST_CASE("Phase != 0 transitions to Preparing", "[lifecycle][preparing]") {
    PrintLifecycleState sm;
    bool changed = sm.on_start_phase_changed(1, PrintJobState::PRINTING);
    REQUIRE(changed == true);
    REQUIRE(sm.state() == PrintState::Preparing);
}

TEST_CASE("Phase == 0 restores to actual Moonraker state", "[lifecycle][preparing]") {
    PrintLifecycleState sm;

    SECTION("Restores to Printing") {
        TA::set_state(sm, PrintState::Preparing);
        bool changed = sm.on_start_phase_changed(0, PrintJobState::PRINTING);
        REQUIRE(changed == true);
        REQUIRE(sm.state() == PrintState::Printing);
    }

    SECTION("Restores to Paused") {
        TA::set_state(sm, PrintState::Preparing);
        bool changed = sm.on_start_phase_changed(0, PrintJobState::PAUSED);
        REQUIRE(changed == true);
        REQUIRE(sm.state() == PrintState::Paused);
    }

    SECTION("Restores to Idle for STANDBY") {
        TA::set_state(sm, PrintState::Preparing);
        bool changed = sm.on_start_phase_changed(0, PrintJobState::STANDBY);
        REQUIRE(changed == true);
        REQUIRE(sm.state() == PrintState::Idle);
    }
}

TEST_CASE("Duration updates ignored during Preparing", "[lifecycle][preparing]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Preparing);

    // on_duration_changed returns false during Preparing (preprint observer owns display)
    bool accepted = sm.on_duration_changed(120, PrintOutcome::NONE);
    REQUIRE(accepted == false);
}

// ============================================================================
// print_ended & gcode_loaded Tests
// ============================================================================

TEST_CASE("Complete/Cancelled/Error do NOT trigger print_ended", "[lifecycle][state]") {
    // Resources (thumbnail, gcode, viewer) persist through terminal states.
    // Cleanup only happens on the subsequent Idle transition.
    SECTION("Complete does not trigger print_ended") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
        REQUIRE(result.print_ended == false);
    }

    SECTION("Cancelled does not trigger print_ended") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        auto result = sm.on_job_state_changed(PrintJobState::CANCELLED, PrintOutcome::CANCELLED);
        REQUIRE(result.print_ended == false);
    }

    SECTION("Error does not trigger print_ended") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        auto result = sm.on_job_state_changed(PrintJobState::ERROR, PrintOutcome::ERROR);
        REQUIRE(result.print_ended == false);
    }
}

TEST_CASE("Idle transition always triggers print_ended", "[lifecycle][state]") {
    // print_ended fires on any transition to Idle — that's when resources get cleaned up.
    SECTION("Complete -> Idle triggers print_ended") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Complete);
        auto result = sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
        REQUIRE(result.state_changed == true);
        REQUIRE(result.print_ended == true);
    }

    SECTION("Printing -> Idle triggers print_ended") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        auto result = sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
        REQUIRE(result.state_changed == true);
        REQUIRE(result.print_ended == true);
    }

    SECTION("Cancelled -> Idle triggers print_ended") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Cancelled);
        auto result = sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
        REQUIRE(result.state_changed == true);
        REQUIRE(result.print_ended == true);
    }
}

TEST_CASE("gcode_loaded preserved on all terminal states", "[lifecycle][state]") {
    SECTION("Complete preserves gcode_loaded") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        TA::set_gcode_loaded(sm, true);

        auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
        REQUIRE(sm.gcode_loaded() == true);
    }

    SECTION("Cancel preserves gcode_loaded") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        TA::set_gcode_loaded(sm, true);

        auto result = sm.on_job_state_changed(PrintJobState::CANCELLED, PrintOutcome::CANCELLED);
        REQUIRE(sm.gcode_loaded() == true);
    }

    SECTION("Error preserves gcode_loaded") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        TA::set_gcode_loaded(sm, true);

        auto result = sm.on_job_state_changed(PrintJobState::ERROR, PrintOutcome::ERROR);
        REQUIRE(sm.gcode_loaded() == true);
    }

    SECTION("Idle from non-active clears gcode_loaded") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Complete);
        TA::set_gcode_loaded(sm, true);

        auto result = sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
        REQUIRE(sm.gcode_loaded() == false);
    }
}

TEST_CASE("want_viewer true for all non-Idle states", "[lifecycle][viewer]") {
    PrintLifecycleState sm;

    SECTION("Preparing: want_viewer=true") {
        TA::set_state(sm, PrintState::Preparing);
        REQUIRE(sm.want_viewer() == true);
    }

    SECTION("Printing: want_viewer=true") {
        TA::set_state(sm, PrintState::Printing);
        REQUIRE(sm.want_viewer() == true);
    }

    SECTION("Paused: want_viewer=true") {
        TA::set_state(sm, PrintState::Paused);
        REQUIRE(sm.want_viewer() == true);
    }

    SECTION("Complete: want_viewer=true") {
        TA::set_state(sm, PrintState::Complete);
        REQUIRE(sm.want_viewer() == true);
    }

    SECTION("Cancelled: want_viewer=true") {
        TA::set_state(sm, PrintState::Cancelled);
        REQUIRE(sm.want_viewer() == true);
    }

    SECTION("Error: want_viewer=true") {
        TA::set_state(sm, PrintState::Error);
        REQUIRE(sm.want_viewer() == true);
    }

    SECTION("Idle: want_viewer=false") {
        TA::set_state(sm, PrintState::Idle);
        REQUIRE(sm.want_viewer() == false);
    }
}

TEST_CASE("should_show_viewer includes Preparing and Complete", "[lifecycle][viewer]") {
    // The on_job_state_changed result's should_show_viewer uses a broader set
    // than want_viewer(): it includes Preparing, Printing, Paused, Complete.

    SECTION("Complete with gcode_loaded shows viewer in result") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        TA::set_gcode_loaded(sm, true);

        auto result = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
        REQUIRE(result.should_show_viewer == true);
    }

    SECTION("Cancelled with gcode_loaded keeps viewer visible") {
        PrintLifecycleState sm;
        TA::set_state(sm, PrintState::Printing);
        TA::set_gcode_loaded(sm, true);

        auto result = sm.on_job_state_changed(PrintJobState::CANCELLED, PrintOutcome::CANCELLED);
        // gcode_loaded preserved on terminal states, viewer stays visible
        REQUIRE(result.should_show_viewer == true);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("Progress clamped to 0-100", "[lifecycle]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);

    sm.on_progress_changed(150);
    REQUIRE(sm.progress() == 100);

    sm.on_progress_changed(-5);
    REQUIRE(sm.progress() == 0);
}

TEST_CASE("Multiple Complete transitions are idempotent", "[lifecycle]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);

    auto r1 = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(r1.state_changed == true);

    auto r2 = sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(r2.state_changed == false);
}

TEST_CASE("Temperature/speed/flow always accepted", "[lifecycle]") {
    PrintLifecycleState sm;

    SECTION("Accepted during Complete") {
        TA::set_state(sm, PrintState::Complete);

        sm.on_temperature_changed(200, 210, 60, 65);
        REQUIRE(sm.nozzle_current() == 200);
        REQUIRE(sm.nozzle_target() == 210);
        REQUIRE(sm.bed_current() == 60);
        REQUIRE(sm.bed_target() == 65);

        sm.on_speed_changed(150);
        REQUIRE(sm.speed_percent() == 150);

        sm.on_flow_changed(95);
        REQUIRE(sm.flow_percent() == 95);
    }

    SECTION("Accepted during Idle") {
        TA::set_state(sm, PrintState::Idle);

        sm.on_temperature_changed(25, 0, 22, 0);
        REQUIRE(sm.nozzle_current() == 25);
        REQUIRE(sm.bed_current() == 22);

        sm.on_speed_changed(100);
        REQUIRE(sm.speed_percent() == 100);

        sm.on_flow_changed(100);
        REQUIRE(sm.flow_percent() == 100);
    }
}

// ============================================================================
// map_current_layer_to_viewer — layer rescaling used by both the live update
// path and the terminal→Idle re-freeze path in PrintStatusPanel.
//
// Regression: the from_terminal_to_idle block in ui_panel_print_status.cpp
// previously re-froze only the progress bar. On print completion Moonraker
// sends zeroed layer/progress subjects in the same batch as STANDBY, and the
// gcode viewer's direct observer zeroed its display before the lifecycle
// guard could reject the update. The viewer-layer mapping lives here so it
// can be exercised without instantiating the full panel; the panel calls
// this helper in both the live and re-freeze paths so they cannot drift.
// ============================================================================

TEST_CASE("map_current_layer_to_viewer rescales by total_layers", "[lifecycle][viewer]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_layers(sm, 60, 240);

    // 60/240 of 2912 = 728
    REQUIRE(sm.map_current_layer_to_viewer(2912) == 728);
}

TEST_CASE("map_current_layer_to_viewer returns current_layer when total unknown",
          "[lifecycle][viewer]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_layers(sm, 42, 0);

    REQUIRE(sm.map_current_layer_to_viewer(2912) == 42);
}

TEST_CASE("map_current_layer_to_viewer returns current_layer when viewer has no layers",
          "[lifecycle][viewer]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_layers(sm, 42, 100);

    REQUIRE(sm.map_current_layer_to_viewer(0) == 42);
}

TEST_CASE("map_current_layer_to_viewer stays frozen after Complete→Idle",
          "[lifecycle][viewer][race]") {
    // This is the direct regression coverage for the bug where the panel's
    // from_terminal_to_idle re-freeze path forgot to update the gcode viewer.
    // After Moonraker sends zeroed subjects on STANDBY, the lifecycle guard
    // keeps current_layer_ at its terminal value, so the mapping still yields
    // the final printed layer — not 0.
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_layers(sm, 50, 100);

    // Complete snaps current_layer to total_layers
    sm.on_job_state_changed(PrintJobState::COMPLETE, PrintOutcome::COMPLETE);
    REQUIRE(sm.current_layer() == 100);

    // Transition to Idle, then Moonraker's zeroed layer arrives
    sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
    REQUIRE(sm.on_layer_changed(0, 100, true) == false);
    REQUIRE(sm.current_layer() == 100);

    // Mapping must still yield the final viewer layer, not 0
    REQUIRE(sm.map_current_layer_to_viewer(2912) == 2912);
}

TEST_CASE("map_current_layer_to_viewer stays frozen after Cancelled→Idle",
          "[lifecycle][viewer][race]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_layers(sm, 37, 100);

    // Cancel mid-print — lifecycle preserves current_layer (unlike Complete
    // which snaps to total). Frozen value should map into viewer space.
    sm.on_job_state_changed(PrintJobState::CANCELLED, PrintOutcome::CANCELLED);
    REQUIRE(sm.current_layer() == 37);

    // Transition to Idle with zeroed layer from Moonraker
    sm.on_job_state_changed(PrintJobState::STANDBY, PrintOutcome::NONE);
    REQUIRE(sm.on_layer_changed(0, 100, true) == false);
    REQUIRE(sm.current_layer() == 37);

    // 37/100 of 2900 = 1073
    REQUIRE(sm.map_current_layer_to_viewer(2900) == 1073);
}

TEST_CASE("Duration ignored when outcome != NONE", "[lifecycle][guard]") {
    PrintLifecycleState sm;
    TA::set_state(sm, PrintState::Printing);
    TA::set_elapsed(sm, 500);

    REQUIRE(sm.on_duration_changed(100, PrintOutcome::COMPLETE) == false);
    REQUIRE(sm.elapsed_seconds() == 500);
}

// ============================================================================
// derive_print_state - the single job-state -> UI-state mapping
// ============================================================================

TEST_CASE("derive_print_state maps every job state", "[lifecycle][state]") {
    // The mapping from Moonraker's PrintJobState to the UI-level PrintState was
    // reimplemented inside PrintLifecycleState::on_job_state_changed while three
    // other components re-derived their own answers from the raw enum. This is
    // the single definition they all key off.
    const int no_phase = 0;

    REQUIRE(derive_print_state(PrintJobState::STANDBY, no_phase) == PrintState::Idle);
    REQUIRE(derive_print_state(PrintJobState::PRINTING, no_phase) == PrintState::Printing);
    REQUIRE(derive_print_state(PrintJobState::PAUSED, no_phase) == PrintState::Paused);
    REQUIRE(derive_print_state(PrintJobState::COMPLETE, no_phase) == PrintState::Complete);
    REQUIRE(derive_print_state(PrintJobState::CANCELLED, no_phase) == PrintState::Cancelled);
    REQUIRE(derive_print_state(PrintJobState::ERROR, no_phase) == PrintState::Error);
}

TEST_CASE("derive_print_state reports Preparing whenever a pre-print phase is live",
          "[lifecycle][state][preparing]") {
    // A live pre-print phase outranks the job state. This is what makes
    // Preparing reachable before the printer reports PRINTING at all: a
    // host-side pre-start block runs while print_stats still holds the previous
    // job's terminal state.
    SECTION("phase outranks a terminal job state") {
        REQUIRE(derive_print_state(PrintJobState::COMPLETE, 1) == PrintState::Preparing);
        REQUIRE(derive_print_state(PrintJobState::CANCELLED, 1) == PrintState::Preparing);
    }

    SECTION("phase outranks standby") {
        REQUIRE(derive_print_state(PrintJobState::STANDBY, 1) == PrintState::Preparing);
    }

    SECTION("phase outranks printing, which is the in-PRINT_START case") {
        REQUIRE(derive_print_state(PrintJobState::PRINTING, 3) == PrintState::Preparing);
    }

    SECTION("a paused job is not masked by a stale phase") {
        // Pause is a user-visible state that must win: a phase left set while the
        // printer is paused would hide the pause from every consumer.
        REQUIRE(derive_print_state(PrintJobState::PAUSED, 1) == PrintState::Paused);
    }
}

TEST_CASE("derive_print_state agrees with the state machine it replaces", "[lifecycle][state]") {
    // Guards against the mapping drifting from PrintLifecycleState's own
    // transition handling, which is the failure this consolidation exists to
    // prevent.
    const PrintJobState all[] = {PrintJobState::STANDBY,   PrintJobState::PRINTING,
                                 PrintJobState::PAUSED,    PrintJobState::COMPLETE,
                                 PrintJobState::CANCELLED, PrintJobState::ERROR};
    for (PrintJobState js : all) {
        PrintLifecycleState sm;
        sm.on_job_state_changed(js, PrintOutcome::NONE);
        REQUIRE(sm.state() == derive_print_state(js, 0));
    }
}
