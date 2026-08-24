// tests/unit/test_print_control_view.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_control_view.h"

#include <cstring>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::ui::compute_control_button_view;
using helix::ui::ControlButtonInputs;
using helix::ui::PendingAction;

namespace {

/// A printer actually running the job.
ControlButtonInputs printing() {
    return ControlButtonInputs{PrintJobState::PRINTING,
                               PrintState::Printing,
                               false,
                               PendingAction::None,
                               true,
                               true,
                               true};
}

/// A printer paused mid-job.
ControlButtonInputs paused() {
    return ControlButtonInputs{
        PrintJobState::PAUSED, PrintState::Paused, false, PendingAction::None, true, true, true};
}

/// This screen has committed to a print the printer has not accepted yet: a
/// host-side pre-start block is running in front of the job, so print_stats
/// still describes the PREVIOUS job.
ControlButtonInputs preparing_host_side() {
    return ControlButtonInputs{PrintJobState::COMPLETE,
                               PrintState::Preparing,
                               true,
                               PendingAction::None,
                               true,
                               true,
                               true};
}

/// The same user-visible moment on a printer that does its pre-print work
/// inside PRINT_START: Klipper already reports `printing`.
ControlButtonInputs preparing_firmware_side() {
    return ControlButtonInputs{PrintJobState::PRINTING,
                               PrintState::Preparing,
                               false,
                               PendingAction::None,
                               true,
                               true,
                               true};
}

} // namespace

TEST_CASE("control view: printing shows enabled Pause", "[print_control_view]") {
    auto v = compute_control_button_view(printing());
    REQUIRE(std::string(v.primary_label) == "Pause");
    REQUIRE(v.primary_enabled);
    REQUIRE(v.stop_enabled);
}

TEST_CASE("control view: paused shows enabled Resume (play icon)", "[print_control_view]") {
    auto v = compute_control_button_view(paused());
    REQUIRE(std::string(v.primary_label) == "Resume");
    REQUIRE(std::string(v.primary_icon) == "\xF3\xB0\x90\x8A"); // play
    REQUIRE(v.primary_enabled);
}

TEST_CASE("control view: idle disables both buttons", "[print_control_view]") {
    for (auto s : {PrintJobState::STANDBY, PrintJobState::COMPLETE, PrintJobState::CANCELLED,
                   PrintJobState::ERROR}) {
        ControlButtonInputs in;
        in.job_state = s;
        in.lifecycle = PrintState::Idle;
        in.pause_available = in.resume_available = in.cancel_available = true;
        auto v = compute_control_button_view(in);
        REQUIRE_FALSE(v.primary_enabled);
        REQUIRE_FALSE(v.stop_enabled);
        REQUIRE_FALSE(v.stop_retires_preparing);
    }
}

TEST_CASE("control view: pending Pausing -> hourglass, disabled, transitional label",
          "[print_control_view]") {
    auto in = printing();
    in.pending = PendingAction::Pausing;
    auto v = compute_control_button_view(in);
    REQUIRE(std::string(v.primary_icon) == "\xF3\xB0\x94\x9F"); // hourglass
    REQUIRE(std::string(v.primary_label) == "Pausing...");
    REQUIRE_FALSE(v.primary_enabled);
    REQUIRE(v.stop_enabled);
}

TEST_CASE("control view: pending Resuming -> hourglass + Resuming label", "[print_control_view]") {
    auto in = paused();
    in.pending = PendingAction::Resuming;
    auto v = compute_control_button_view(in);
    REQUIRE(std::string(v.primary_icon) == "\xF3\xB0\x94\x9F");
    REQUIRE(std::string(v.primary_label) == "Resuming...");
    REQUIRE_FALSE(v.primary_enabled);
}

// The user-visible consequence of a stuck pending action, stated as one
// before/after pair: while Resuming is pending the paused printer's primary
// button is BOTH mislabelled and un-tappable, and clearing the pending action is
// the entire difference. On the reporter's AD5X that window lasted 150s because
// nothing linked Klipper's `!!` rejection back to the pending state — they could
// not retry even after clearing the runout (bundle JX2FVRB9). See
// PrintControlButtons::notify_printer_error(), which performs this transition.
TEST_CASE("control view: clearing a pending Resume makes the button tappable again",
          "[print_control_view]") {
    auto stuck_in = paused();
    stuck_in.pending = PendingAction::Resuming;
    auto stuck = compute_control_button_view(stuck_in);
    REQUIRE(std::string(stuck.primary_label) == "Resuming...");
    REQUIRE_FALSE(stuck.primary_enabled);

    auto released = compute_control_button_view(paused());
    REQUIRE(std::string(released.primary_label) == "Resume");
    REQUIRE(released.primary_enabled);
}

TEST_CASE("control view: missing macro slot disables primary", "[print_control_view]") {
    auto a = printing();
    a.pause_available = false;
    REQUIRE_FALSE(compute_control_button_view(a).primary_enabled);

    auto b = paused();
    b.resume_available = false;
    REQUIRE_FALSE(compute_control_button_view(b).primary_enabled);

    auto c = printing();
    c.cancel_available = false;
    REQUIRE_FALSE(compute_control_button_view(c).stop_enabled);
}

// ============================================================================
// The preparing window (#798)
//
// Governing rule: affordance is a function of PrintState alone. It must never
// depend on whether pre-print work runs host-side, in front of the job, or
// firmware-side inside Klipper's PRINT_START. The two architectures report
// different job states for the same user-visible moment, so any rule keyed on
// job state alone gives the user different controls for the same situation.
// ============================================================================

TEST_CASE("control view: Cancel is enabled during a host-side preparing window",
          "[print_control_view][preparing]") {
    // Regression: keying stop_enabled on PRINTING||PAUSED disabled Cancel for
    // exactly the window in which the host-side cancel path is the only one that
    // works, making that path unreachable by touch.
    auto v = compute_control_button_view(preparing_host_side());
    REQUIRE(v.stop_enabled);
    REQUIRE(v.stop_retires_preparing);
}

TEST_CASE("control view: Cancel during host-side preparing needs no cancel macro",
          "[print_control_view][preparing]") {
    // Retiring a preparing job is host-side bookkeeping. Gating it on the
    // printer's CANCEL_PRINT macro would strand users whose printer has none.
    auto in = preparing_host_side();
    in.cancel_available = false;
    auto v = compute_control_button_view(in);
    REQUIRE(v.stop_enabled);
    REQUIRE(v.stop_retires_preparing);
}

TEST_CASE("control view: Cancel is enabled during a firmware-side preparing window",
          "[print_control_view][preparing]") {
    auto v = compute_control_button_view(preparing_firmware_side());
    REQUIRE(v.stop_enabled);
    // The printer holds the job, so this one routes through CANCEL_PRINT.
    REQUIRE_FALSE(v.stop_retires_preparing);
}

TEST_CASE("control view: Pause is disabled in BOTH preparing architectures",
          "[print_control_view][preparing]") {
    // The parity assertion. Before this rule, host-side preparing disabled Pause
    // only incidentally (job state was not PRINTING) while firmware-side left it
    // live for the whole window — pressing it sent PAUSE mid-PRINT_START.
    REQUIRE_FALSE(compute_control_button_view(preparing_host_side()).primary_enabled);
    REQUIRE_FALSE(compute_control_button_view(preparing_firmware_side()).primary_enabled);
}

TEST_CASE("control view: the two preparing architectures agree on every affordance",
          "[print_control_view][preparing]") {
    auto host = compute_control_button_view(preparing_host_side());
    auto firmware = compute_control_button_view(preparing_firmware_side());

    REQUIRE(host.primary_enabled == firmware.primary_enabled);
    REQUIRE(host.stop_enabled == firmware.stop_enabled);
    // Only the mechanism differs, and that is deliberately not user-visible.
    REQUIRE(host.stop_retires_preparing != firmware.stop_retires_preparing);
}

TEST_CASE("control view: Pause re-enables once preparation hands off to printing",
          "[print_control_view][preparing]") {
    REQUIRE_FALSE(compute_control_button_view(preparing_firmware_side()).primary_enabled);
    REQUIRE(compute_control_button_view(printing()).primary_enabled);
}

TEST_CASE("control view: a paused printer is never treated as preparing",
          "[print_control_view][preparing]") {
    // derive_print_state() lets PAUSED outrank a stale phase so a pause is never
    // masked. Pause/Resume must stay live through that.
    auto v = compute_control_button_view(paused());
    REQUIRE(v.primary_enabled);
    REQUIRE(std::string(v.primary_label) == "Resume");
}

TEST_CASE("control view: an externally started print in PRINT_START cannot retire anything",
          "[print_control_view][preparing]") {
    // Printer-edge arming: the lifecycle says Preparing but this screen never
    // committed, so there is no preparing job to retire and Cancel must route to
    // the printer.
    auto in = preparing_firmware_side();
    in.has_preparing_job = false;
    auto v = compute_control_button_view(in);
    REQUIRE_FALSE(v.stop_retires_preparing);
    REQUIRE(v.stop_enabled);
}
