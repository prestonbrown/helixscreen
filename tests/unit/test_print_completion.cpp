// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_completion.cpp
 * @brief Unit tests for print completion notification system
 *
 * Tests the print completion observer that:
 * - Skips the first callback (initial state, not a real transition)
 * - Detects PRINTING/PAUSED -> COMPLETE/CANCELLED/ERROR transitions
 * - Does NOT trigger on startup when printer is mid-print
 *
 * The key fix being tested: has_received_first_update flag that prevents
 * false notifications on observer registration.
 *
 * TEST-FIRST: Documents expected behavior for the fix.
 */

#include "ui_format_utils.h"

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "print_completion.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include "../catch_amalgamated.hpp"

using namespace helix;
using json = nlohmann::json;

// ============================================================================
// Test Fixture
// ============================================================================

class PrintCompletionTestFixture {
  public:
    PrintCompletionTestFixture() {
        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a headless display for testing
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(disp);
            });
            display_created_ = true;
        }

        // Reset PrinterState for test isolation
        PrinterStateTestAccess::reset(state_);

        // Initialize subjects (without XML registration in tests)
        state_.init_subjects(false);

        // Reset tracking state
        completion_callback_count_ = 0;
        last_completion_state_ = PrintJobState::STANDBY;
    }

    ~PrintCompletionTestFixture() {
        // Remove observer if registered
        if (observer_ != nullptr) {
            lv_observer_remove(observer_);
            observer_ = nullptr;
        }
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    // Helper to update print state via status JSON
    void set_print_state(const std::string& state_str) {
        json status = {{"print_stats", {{"state", state_str}}}};
        state_.update_from_status(status);
    }

    // Get current print state enum
    PrintJobState get_print_state_enum() {
        return static_cast<PrintJobState>(
            lv_subject_get_int(state_.get_print_state_enum_subject()));
    }

    // Register a test observer that mimics print_completion behavior
    // This observer tracks state transitions and counts "completion" events
    void register_completion_observer() {
        // Initialize prev_state to current state (same as real implementation)
        prev_print_state_ = get_print_state_enum();
        has_received_first_update_ = false;

        observer_ = lv_subject_add_observer(
            state_.get_print_state_enum_subject(),
            [](lv_observer_t* observer, lv_subject_t* subject) {
                auto* self =
                    static_cast<PrintCompletionTestFixture*>(lv_observer_get_user_data(observer));
                self->on_state_changed(subject);
            },
            this);
    }

    // Get number of completion callbacks triggered
    int get_completion_callback_count() const {
        return completion_callback_count_;
    }

    // Get the last state that triggered a completion
    PrintJobState get_last_completion_state() const {
        return last_completion_state_;
    }

  private:
    void on_state_changed(lv_subject_t* subject) {
        auto current = static_cast<PrintJobState>(lv_subject_get_int(subject));

        spdlog::debug("[TestObserver] State change: {} -> {} (first_update={})",
                      static_cast<int>(prev_print_state_), static_cast<int>(current),
                      has_received_first_update_);

        // KEY FIX: Skip the very first callback (initial registration)
        // This is the behavior we're testing
        if (!has_received_first_update_) {
            has_received_first_update_ = true;
            prev_print_state_ = current;
            spdlog::debug("[TestObserver] Skipping first update (initial registration)");
            return;
        }

        // Check for transitions to terminal states (from active print states)
        bool was_active = (prev_print_state_ == PrintJobState::PRINTING ||
                           prev_print_state_ == PrintJobState::PAUSED);
        bool is_terminal = (current == PrintJobState::COMPLETE ||
                            current == PrintJobState::CANCELLED || current == PrintJobState::ERROR);

        if (was_active && is_terminal) {
            completion_callback_count_++;
            last_completion_state_ = current;
            spdlog::debug("[TestObserver] Completion detected! count={}",
                          completion_callback_count_);
        }

        prev_print_state_ = current;
    }

    PrinterState state_;
    static lv_display_t* display_;
    static bool display_created_;

    lv_observer_t* observer_ = nullptr;
    PrintJobState prev_print_state_ = PrintJobState::STANDBY;
    bool has_received_first_update_ = false;

    int completion_callback_count_ = 0;
    PrintJobState last_completion_state_ = PrintJobState::STANDBY;
};

lv_display_t* PrintCompletionTestFixture::display_ = nullptr;
bool PrintCompletionTestFixture::display_created_ = false;

// ============================================================================
// First Callback Skipping Tests
// ============================================================================

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: first callback is skipped on observer registration",
                 "[print_completion][first_update]") {
    // Start in standby state
    set_print_state("standby");
    REQUIRE(get_print_state_enum() == PrintJobState::STANDBY);

    // Register observer - LVGL fires callback immediately with current value
    register_completion_observer();

    // The initial callback should be skipped (no completion notification)
    REQUIRE(get_completion_callback_count() == 0);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: first callback skipped even when printer is printing",
                 "[print_completion][first_update]") {
    // Printer is already printing when we connect
    set_print_state("printing");
    REQUIRE(get_print_state_enum() == PrintJobState::PRINTING);

    // Register observer
    register_completion_observer();

    // The initial callback should be skipped
    REQUIRE(get_completion_callback_count() == 0);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: first callback skipped when printer shows complete",
                 "[print_completion][first_update]") {
    // Edge case: printer was already in COMPLETE state when we connect
    // (e.g., previous print finished while screen was off)
    set_print_state("complete");
    REQUIRE(get_print_state_enum() == PrintJobState::COMPLETE);

    // Register observer
    register_completion_observer();

    // Should NOT trigger completion (no transition, just initial state)
    REQUIRE(get_completion_callback_count() == 0);
}

// ============================================================================
// Normal Completion Flow Tests
// ============================================================================

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: PRINTING to COMPLETE triggers notification",
                 "[print_completion][transition]") {
    // Start in standby
    set_print_state("standby");
    register_completion_observer();
    REQUIRE(get_completion_callback_count() == 0);

    // Start printing
    set_print_state("printing");
    REQUIRE(get_completion_callback_count() == 0); // Not a terminal state

    // Complete the print
    set_print_state("complete");
    REQUIRE(get_completion_callback_count() == 1);
    REQUIRE(get_last_completion_state() == PrintJobState::COMPLETE);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: PRINTING to CANCELLED triggers notification",
                 "[print_completion][transition]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    REQUIRE(get_completion_callback_count() == 0);

    set_print_state("cancelled");
    REQUIRE(get_completion_callback_count() == 1);
    REQUIRE(get_last_completion_state() == PrintJobState::CANCELLED);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: PRINTING to ERROR triggers notification",
                 "[print_completion][transition]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    REQUIRE(get_completion_callback_count() == 0);

    set_print_state("error");
    REQUIRE(get_completion_callback_count() == 1);
    REQUIRE(get_last_completion_state() == PrintJobState::ERROR);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: PAUSED to COMPLETE triggers notification",
                 "[print_completion][transition]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    set_print_state("paused");
    REQUIRE(get_completion_callback_count() == 0); // Pause is not terminal

    set_print_state("complete");
    REQUIRE(get_completion_callback_count() == 1);
    REQUIRE(get_last_completion_state() == PrintJobState::COMPLETE);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: PAUSED to CANCELLED triggers notification",
                 "[print_completion][transition]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    set_print_state("paused");
    set_print_state("cancelled");

    REQUIRE(get_completion_callback_count() == 1);
    REQUIRE(get_last_completion_state() == PrintJobState::CANCELLED);
}

// ============================================================================
// Startup Scenarios (Mid-Print Connection)
// ============================================================================

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: connecting mid-print does NOT trigger on first PRINTING update",
                 "[print_completion][startup]") {
    // This is the key bug scenario:
    // 1. HelixScreen starts while printer is already printing
    // 2. First status update shows PRINTING
    // 3. This should NOT trigger a completion notification

    // Printer is in STANDBY when we start (before connection)
    set_print_state("standby");
    register_completion_observer();

    // First update after connection shows printer is printing
    // This could be misinterpreted as a transition TO printing,
    // but it's actually just initial state discovery
    set_print_state("printing");

    // Should NOT trigger completion (not a terminal state anyway)
    REQUIRE(get_completion_callback_count() == 0);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: connecting when printer just completed does NOT trigger",
                 "[print_completion][startup]") {
    // Scenario: HelixScreen connects right after print completed
    // First update shows COMPLETE, but we shouldn't notify

    set_print_state("standby");
    register_completion_observer();

    // Immediately receive COMPLETE status (no PRINTING seen first)
    set_print_state("complete");

    // Should NOT trigger - we never saw it printing
    // (was_active is false because prev was STANDBY)
    REQUIRE(get_completion_callback_count() == 0);
}

// ============================================================================
// Multiple Print Cycle Tests
// ============================================================================

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: multiple print completions each trigger notification",
                 "[print_completion][multi]") {
    set_print_state("standby");
    register_completion_observer();

    // First print
    set_print_state("printing");
    set_print_state("complete");
    REQUIRE(get_completion_callback_count() == 1);

    // Back to standby
    set_print_state("standby");

    // Second print
    set_print_state("printing");
    set_print_state("complete");
    REQUIRE(get_completion_callback_count() == 2);

    // Third print - cancelled this time
    set_print_state("standby");
    set_print_state("printing");
    set_print_state("cancelled");
    REQUIRE(get_completion_callback_count() == 3);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: pause-resume-complete only triggers once",
                 "[print_completion][multi]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    set_print_state("paused");   // No notification
    set_print_state("printing"); // Resume - no notification
    set_print_state("paused");   // Pause again - no notification
    set_print_state("complete"); // Finally complete

    // Only ONE completion notification for the entire print
    REQUIRE(get_completion_callback_count() == 1);
}

// ============================================================================
// Non-Terminal State Transitions (Should NOT Trigger)
// ============================================================================

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: STANDBY to PRINTING does NOT trigger",
                 "[print_completion][no_trigger]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");

    // Starting a print is not a completion
    REQUIRE(get_completion_callback_count() == 0);
}

TEST_CASE_METHOD(PrintCompletionTestFixture, "PrintCompletion: PRINTING to PAUSED does NOT trigger",
                 "[print_completion][no_trigger]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    set_print_state("paused");

    // Pausing is not a completion
    REQUIRE(get_completion_callback_count() == 0);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: COMPLETE to STANDBY does NOT trigger",
                 "[print_completion][no_trigger]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    set_print_state("complete");
    REQUIRE(get_completion_callback_count() == 1);

    // Going back to standby after completion should NOT trigger again
    set_print_state("standby");
    REQUIRE(get_completion_callback_count() == 1); // Still 1
}

TEST_CASE_METHOD(PrintCompletionTestFixture, "PrintCompletion: ERROR to STANDBY does NOT trigger",
                 "[print_completion][no_trigger]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");
    set_print_state("error");
    REQUIRE(get_completion_callback_count() == 1);

    // Recovery from error should NOT trigger
    set_print_state("standby");
    REQUIRE(get_completion_callback_count() == 1); // Still 1
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE_METHOD(PrintCompletionTestFixture, "PrintCompletion: same state update does NOT trigger",
                 "[print_completion][edge_case]") {
    set_print_state("standby");
    register_completion_observer();

    set_print_state("printing");

    // Duplicate updates (Moonraker might send same state multiple times)
    set_print_state("printing");
    set_print_state("printing");

    REQUIRE(get_completion_callback_count() == 0);

    set_print_state("complete");
    REQUIRE(get_completion_callback_count() == 1);
}

TEST_CASE_METHOD(PrintCompletionTestFixture,
                 "PrintCompletion: rapid state changes handled correctly",
                 "[print_completion][edge_case]") {
    set_print_state("standby");
    register_completion_observer();

    // Rapid transitions (edge case where updates arrive quickly)
    set_print_state("printing");
    set_print_state("paused");
    set_print_state("printing");
    set_print_state("paused");
    set_print_state("cancelled");

    // Only the final terminal transition should count
    REQUIRE(get_completion_callback_count() == 1);
    REQUIRE(get_last_completion_state() == PrintJobState::CANCELLED);
}

// ============================================================================
// Completion stats formatting
// ============================================================================

TEST_CASE("build_completion_stats renders layers through the canonical formatter",
          "[print_completion][format]") {
    // The completion modal used to hand-roll snprintf("%d %s", n, lv_tr("layers")),
    // which renders "0 layers" where the shared helper returns UNAVAILABLE and
    // "1 layers" where it returns the singular. It also used a second, separate
    // translation key for the same word. Asserting equality with the helper
    // states the contract directly and survives translation.
    SECTION("zero layers matches the helper's unavailable form") {
        auto stats = helix::build_completion_stats(60, 0, 0, 0);
        REQUIRE(stats.layers == helix::ui::format_layer_count(0));
    }

    SECTION("a single layer is not pluralised") {
        auto stats = helix::build_completion_stats(60, 0, 1, 0);
        REQUIRE(stats.layers == helix::ui::format_layer_count(1));
    }

    SECTION("many layers match the helper") {
        auto stats = helix::build_completion_stats(60, 0, 250, 0);
        REQUIRE(stats.layers == helix::ui::format_layer_count(250));
    }
}

TEST_CASE("build_completion_stats omits optional fields when unknown",
          "[print_completion][format]") {
    SECTION("no estimate yields an empty estimate string") {
        auto stats = helix::build_completion_stats(60, 0, 10, 500);
        REQUIRE(stats.estimate.empty());
    }

    SECTION("no filament yields an empty filament string") {
        auto stats = helix::build_completion_stats(60, 120, 10, 0);
        REQUIRE(stats.filament.empty());
    }

    SECTION("both present yields both populated") {
        auto stats = helix::build_completion_stats(60, 120, 10, 500);
        REQUIRE_FALSE(stats.estimate.empty());
        REQUIRE_FALSE(stats.filament.empty());
        REQUIRE_FALSE(stats.duration.empty());
    }
}

// ============================================================================
// What happens when a start dies before it prints
// ============================================================================

TEST_CASE("decide_preparing_exit_action distinguishes cancel from failure",
          "[print_completion][preparing]") {
    using helix::decide_preparing_exit_action;
    using helix::PreparingExit;

    SECTION("a user cancel reads as cancelled, not as a failure") {
        auto a = decide_preparing_exit_action(PreparingExit::Cancelled);
        REQUIRE(a.notify_cancelled);
        REQUIRE_FALSE(a.notify_failure);
    }

    SECTION("a failed start reads as a failure") {
        auto a = decide_preparing_exit_action(PreparingExit::Failed);
        REQUIRE(a.notify_failure);
        REQUIRE_FALSE(a.notify_cancelled);
    }

    SECTION("a timeout is a failure, not silence") {
        // A start can die on a printer that never reaches state=printing at all,
        // so nothing downstream would ever report it.
        auto a = decide_preparing_exit_action(PreparingExit::TimedOut);
        REQUIRE(a.notify_failure);
    }
}

TEST_CASE("decide_preparing_exit_action only cools down when no print is running",
          "[print_completion][preparing]") {
    using helix::decide_preparing_exit_action;
    using helix::PreparingExit;

    SECTION("cancel, failure and timeout drop the heaters") {
        // A pre-start block heats to print temperature. If no print follows,
        // leaving a bed at 105C is the worst outcome - it was observed on a K2
        // after cancelling mid-mesh.
        REQUIRE(decide_preparing_exit_action(PreparingExit::Cancelled).cool_down);
        REQUIRE(decide_preparing_exit_action(PreparingExit::Failed).cool_down);
        REQUIRE(decide_preparing_exit_action(PreparingExit::TimedOut).cool_down);
    }

    SECTION("a confirmed start must not touch the heaters") {
        auto a = decide_preparing_exit_action(PreparingExit::Confirmed);
        REQUIRE_FALSE(a.cool_down);
        REQUIRE_FALSE(a.notify_failure);
        REQUIRE_FALSE(a.notify_cancelled);
    }

    SECTION("a superseded start must not touch the heaters") {
        // Someone else's print is now running and owns the printer. Cooling
        // down here would sabotage it.
        auto a = decide_preparing_exit_action(PreparingExit::Superseded);
        REQUIRE_FALSE(a.cool_down);
        REQUIRE_FALSE(a.notify_failure);
        REQUIRE_FALSE(a.notify_cancelled);
    }
}

// ============================================================================
// When a print ending should notify
// ============================================================================

TEST_CASE("should_notify_print_ended fires only when an active print reaches a terminal state",
          "[print_completion][lifecycle]") {
    using helix::should_notify_print_ended;

    SECTION("printing or paused into a terminal state notifies") {
        REQUIRE(should_notify_print_ended(PrintState::Printing, PrintState::Complete,
                                          PrintOutcome::COMPLETE));
        REQUIRE(should_notify_print_ended(PrintState::Printing, PrintState::Cancelled,
                                          PrintOutcome::CANCELLED));
        REQUIRE(should_notify_print_ended(PrintState::Printing, PrintState::Error,
                                          PrintOutcome::ERROR));
        REQUIRE(should_notify_print_ended(PrintState::Paused, PrintState::Cancelled,
                                          PrintOutcome::CANCELLED));
    }

    SECTION("a print that dies inside PRINT_START notifies") {
        // The lifecycle never reaches Printing here. A live pre-print phase
        // outranks the job state, so a firmware-side PRINT_START sits at
        // Preparing while print_stats already says printing; when Klipper faults
        // (probe triggered prior to movement, heater fault) or the user cancels
        // during the K2's ~19-minute mesh, the phase clears and the lifecycle
        // goes straight Preparing -> terminal. Requiring prev==Printing meant
        // NOTHING reported those - not this observer, and not the preparing-exit
        // observer either, because the preparing job was already retired as
        // Confirmed the moment the printer reported printing.
        //
        // The outcome is what makes it safe: it is recorded on the job-state
        // transition into a terminal value.
        REQUIRE(should_notify_print_ended(PrintState::Preparing, PrintState::Error,
                                          PrintOutcome::ERROR));
        REQUIRE(should_notify_print_ended(PrintState::Preparing, PrintState::Cancelled,
                                          PrintOutcome::CANCELLED));
        REQUIRE(should_notify_print_ended(PrintState::Preparing, PrintState::Complete,
                                          PrintOutcome::COMPLETE));
    }

    SECTION("Preparing into a STALE terminal state does not notify") {
        // The stale-badge case this whole branch exists to stop. During a
        // host-side pre-start block print_stats still holds the PREVIOUS job's
        // terminal value, so clearing the phase derives Preparing -> Complete
        // from data about a print that ended minutes ago. begin_preparing()
        // clears print_outcome to NONE, which is the only thing distinguishing
        // it from a real ending - without this arm, abandoning a host-side block
        // would announce "Print Complete!" for the previous job.
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Preparing, PrintState::Complete,
                                                PrintOutcome::NONE));
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Preparing, PrintState::Cancelled,
                                                PrintOutcome::NONE));
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Preparing, PrintState::Error,
                                                PrintOutcome::NONE));
    }

    SECTION("booting straight into a terminal state does not notify") {
        // The app can start while print_stats already reads complete. The
        // previous state is Idle there, so nothing actually ended on our watch.
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Idle, PrintState::Complete,
                                                PrintOutcome::COMPLETE));
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Idle, PrintState::Cancelled,
                                                PrintOutcome::CANCELLED));
    }

    SECTION("non-terminal destinations never notify") {
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Printing, PrintState::Paused,
                                                PrintOutcome::NONE));
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Paused, PrintState::Printing,
                                                PrintOutcome::NONE));
        REQUIRE_FALSE(
            should_notify_print_ended(PrintState::Printing, PrintState::Idle, PrintOutcome::NONE));
    }

    SECTION("a terminal state settling to Idle does not re-notify") {
        // Moonraker reports standby after a terminal state; that must not
        // produce a second notification for the same print.
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Complete, PrintState::Idle,
                                                PrintOutcome::COMPLETE));
        REQUIRE_FALSE(should_notify_print_ended(PrintState::Cancelled, PrintState::Idle,
                                                PrintOutcome::CANCELLED));
    }
}
