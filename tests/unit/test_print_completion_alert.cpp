// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_completion_alert.h"

#include "../catch_amalgamated.hpp"

using helix::CompletionAlertMode;
using helix::CompletionAlertPlan;
using helix::plan_completion_alert;
using helix::PrintEnding;
using helix::should_turn_heaters_off;

namespace {

struct Row {
    PrintEnding ending;
    CompletionAlertMode mode;
    bool on_print_status;
    bool wake;
    bool modal;
    bool toast;
};

} // namespace

TEST_CASE("a running saver takes the place of the completion wake", "[print_completion_alert]") {
    constexpr auto C = PrintEnding::COMPLETE;
    constexpr auto X = PrintEnding::CANCELLED;
    constexpr auto E = PrintEnding::ERROR;
    constexpr auto kOff = CompletionAlertMode::OFF;
    constexpr auto kNote = CompletionAlertMode::NOTIFICATION;
    constexpr auto kAlert = CompletionAlertMode::ALERT;

    // Without a saver: an error always wakes with a modal; the print status panel needs nothing;
    // otherwise the mode decides.
    const Row awake[] = {
        {E, kOff, false, true, true, false},    {E, kNote, true, true, true, false},
        {E, kAlert, false, true, true, false},  {C, kOff, false, false, false, false},
        {C, kNote, false, true, false, true},   {C, kAlert, false, true, true, false},
        {X, kNote, false, true, false, true},   {X, kAlert, false, true, true, false},
        {C, kAlert, true, false, false, false}, {X, kNote, true, false, false, false},
    };
    for (const Row& r : awake) {
        CAPTURE(static_cast<int>(r.ending), static_cast<int>(r.mode), r.on_print_status);
        const CompletionAlertPlan plan =
            plan_completion_alert(r.ending, r.mode, r.on_print_status, false);
        CHECK(plan.wake == r.wake);
        CHECK(plan.modal == r.modal);
        CHECK(plan.toast == r.toast);
    }

    // With a saver running nothing wakes the display; the modal still waits behind the saver.
    const Row saver[] = {
        {E, kOff, false, false, true, false},   {E, kAlert, true, false, true, false},
        {C, kOff, false, false, false, false},  {C, kNote, false, false, false, false},
        {C, kAlert, false, false, true, false}, {X, kAlert, false, false, true, false},
        {X, kNote, false, false, false, false}, {C, kAlert, true, false, false, false},
    };
    for (const Row& r : saver) {
        CAPTURE(static_cast<int>(r.ending), static_cast<int>(r.mode), r.on_print_status);
        const CompletionAlertPlan plan =
            plan_completion_alert(r.ending, r.mode, r.on_print_status, true);
        CHECK(plan.wake == r.wake);
        CHECK(plan.modal == r.modal);
        CHECK(plan.toast == r.toast);
    }
}

TEST_CASE("only a print error the firmware leaves to the app turns the heaters off",
          "[print_completion_alert]") {
    CHECK(should_turn_heaters_off(PrintEnding::ERROR, false));
    // The firmware has already turned them off.
    CHECK_FALSE(should_turn_heaters_off(PrintEnding::ERROR, true));
    // A completed or cancelled print's own end gcode decides.
    const bool owned = GENERATE(false, true);
    CHECK_FALSE(should_turn_heaters_off(PrintEnding::COMPLETE, owned));
    CHECK_FALSE(should_turn_heaters_off(PrintEnding::CANCELLED, owned));
}
