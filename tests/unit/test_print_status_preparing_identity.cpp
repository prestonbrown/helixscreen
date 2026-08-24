// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_preparing_identity.cpp
 * @brief PrintStatusPanel tracks WHICH print it is showing (#1339).
 *
 * Two independent holes, both in the panel's own copy of the print identity:
 *
 * 1. adac6f7eb gave ActivePrintMediaManager a preparing-epoch observer and gave
 *    the panel none, so between commit and confirmation the panel's `desired`
 *    still named the PREVIOUS print. ensure_preview_current() therefore compared
 *    the viewer against a print that had already finished, found no mismatch,
 *    and the clear_gcode that 921200ab1 added never fired in the window it was
 *    written for.
 *
 * 2. Nothing retired thumbnail_source_filename_ when a print ended. A print
 *    started outside the app opens no preparing epoch, so the panel resolved it
 *    through the previous print's name indefinitely.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "printer_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = PrintStatusPanelTestAccess;

namespace {

struct PreparingIdentityFixture : public LVGLTestFixture {
    PreparingIdentityFixture() {
        // Own the PrinterState: the panel's observers bind to whatever reference
        // they are handed, and the epoch subject is what these cases drive.
        state_.init_subjects(false);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        Access::set_thumbnail_widget(*panel_, lv_image_create(test_screen()));
    }

    ~PreparingIdentityFixture() override {
        panel_.reset();
        state_.deinit_subjects();
    }

    PrintStatusPanel& panel() {
        return *panel_;
    }
    PrinterState& state() {
        return state_;
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    PrinterState state_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(PreparingIdentityFixture,
                 "Panel adopts the preparing job's identity at commit, not at confirmation",
                 "[print_status][preparing_identity][1339]") {
    // Print A is running and on screen.
    Access::set_filename(panel(), "printA.gcode");
    drain();
    REQUIRE(Access::thumbnail_source(panel()).empty());

    // Print B is committed. Moonraker still reports print A - that lag is the
    // whole reason the identity is recorded at commit.
    state().begin_preparing(PrintJobRef{"printB.gcode", "", ""});
    drain();

    REQUIRE(Access::thumbnail_source(panel()) == "printB.gcode");
}

TEST_CASE_METHOD(PreparingIdentityFixture,
                 "An abandoned preparing job releases the panel's identity",
                 "[print_status][preparing_identity][1339]") {
    state().begin_preparing(PrintJobRef{"never_ran.gcode", "", ""});
    drain();
    REQUIRE(Access::thumbnail_source(panel()) == "never_ran.gcode");

    // Superseded, not Confirmed: the printer did not take this job. Leaving the
    // source set would resolve the NEXT print through a job that never ran.
    state().retire_preparing(PreparingExit::Superseded);
    drain();

    REQUIRE(Access::thumbnail_source(panel()).empty());
}

TEST_CASE_METHOD(PreparingIdentityFixture, "A confirmed preparing job keeps the panel's identity",
                 "[print_status][preparing_identity][1339]") {
    // Confirmed means the printer took OUR job, so the source still describes
    // what is printing - print_stats may report a rewritten temp name for it.
    state().begin_preparing(PrintJobRef{"mine.gcode", "", ""});
    state().retire_preparing(PreparingExit::Confirmed);
    drain();

    REQUIRE(Access::thumbnail_source(panel()) == "mine.gcode");

    Access::set_filename(panel(), ".helix_temp/modified_1748_mine.gcode");
    drain();

    REQUIRE(Access::thumbnail_source(panel()) == "mine.gcode");
}

TEST_CASE_METHOD(PreparingIdentityFixture,
                 "A print started outside the app retires the panel's stale identity",
                 "[print_status][preparing_identity][1339]") {
    // Print A started FROM the app records the identity and keeps it (Confirmed).
    state().begin_preparing(PrintJobRef{"printA.gcode", "", ""});
    state().retire_preparing(PreparingExit::Confirmed);
    drain();
    Access::set_filename(panel(), "printA.gcode");
    drain();
    REQUIRE(Access::thumbnail_source(panel()) == "printA.gcode");

    // Print B started from Mainsail: no preparing epoch, nothing re-points the
    // identity. Before the fix the panel resolved B through printA for the
    // whole job and never reloaded a thing.
    Access::set_filename(panel(), "printB.gcode");
    drain();

    REQUIRE(Access::thumbnail_source(panel()).empty());
    REQUIRE(Access::current_print_filename(panel()) == "printB.gcode");
}

TEST_CASE_METHOD(PreparingIdentityFixture, "Reprinting the same file keeps the panel's identity",
                 "[print_status][preparing_identity][1339]") {
    // Retirement must not fire on a reprint, where the identity still describes
    // exactly what is printing.
    state().begin_preparing(PrintJobRef{"repeat.gcode", "", ""});
    state().retire_preparing(PreparingExit::Confirmed);
    drain();
    Access::set_filename(panel(), "repeat.gcode");
    drain();

    Access::set_filename(panel(), "repeat.gcode");
    drain();

    REQUIRE(Access::thumbnail_source(panel()) == "repeat.gcode");
}
