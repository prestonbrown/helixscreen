// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_filename_observer.cpp
 * @brief PrintStatusPanel identity, driven through its REAL entry point (#1339).
 *
 * The other identity suites call PrintStatusPanel::set_filename() directly.
 * Production never does: the printer reports a name, print_filename_subject
 * changes, and consumers react. These cases enter where Moonraker does, through
 * update_from_status(), so the ordering between the identity and the filename
 * subject is on the path under test rather than assumed.
 *
 * They were written against the older arrangement, where the panel resolved a
 * rewritten path into its own override and a re-entrant set_filename() could
 * retire it again before it was used. That churn is gone - PrinterPrintState
 * decides the identity once, before print_filename_ is published - but the cases
 * are kept because they pin the behaviour that churn used to threaten.
 *
 * The panel is exercised WITHOUT ActivePrintMediaManager, and the thumbnail is
 * published by hand. That is the point: the invariant is "the panel agrees with
 * whatever identity the manager stamps", so the manager's own behaviour must not
 * be in the loop when asserting it.
 */

#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_status_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "printer_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using Access = PrintStatusPanelTestAccess;
using json = nlohmann::json;

namespace {

// Real assets, so lv_image_set_src resolves instead of logging a decoder miss,
// and distinguishable so "which print's image is on screen" is a real assertion.
constexpr const char* THUMB_A = "A:assets/images/filament_spool.png";
constexpr const char* THUMB_B = "A:assets/images/printer.png";

struct FilenameObserverFixture : public LVGLTestFixture {
    FilenameObserverFixture() {
        state_.init_subjects(false);
        panel_ = std::make_unique<PrintStatusPanel>(state_, nullptr);
        Access::set_thumbnail_widget(*panel_, lv_image_create(test_screen()));
    }

    ~FilenameObserverFixture() override {
        panel_.reset();
        state_.deinit_subjects();
    }

    PrintStatusPanel& panel() {
        return *panel_;
    }
    PrinterState& state() {
        return state_;
    }

    /// Drive the filename the way Moonraker does, then let the panel's deferred
    /// observer run. Nested queue_update calls are why drain_all, not drain.
    void report_filename(const std::string& filename) {
        json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        drain();
    }

    void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    PrinterState state_;
    std::unique_ptr<PrintStatusPanel> panel_;
};

} // namespace

TEST_CASE_METHOD(FilenameObserverFixture,
                 "Filename observer: a rewritten path reported first names the original",
                 "[print_status][thumbnail][1339]") {
    // Restart or reconnect while a prepared copy is already printing. Nothing
    // has installed an override, because this process never started the job.
    report_filename(".helix_temp/modified_1748_Widget.gcode");

    // Resolved, not overridden: no state is installed for a name the rule can
    // derive. The effective identity is what every consumer reads.
    CHECK(state().get_effective_print_filename() == "Widget.gcode");
    CHECK(Access::identity_override(panel()).empty());

    // The manager stamps its publishes with the resolved original; the panel must
    // already agree, or it drops every one of them with no retry.
    state().set_print_thumbnail("Widget.gcode", THUMB_B);
    drain();
    CHECK(Access::displayed_src(panel()) == THUMB_B);
    CHECK(Access::displayed_file(panel()) == "Widget.gcode");
}

TEST_CASE_METHOD(FilenameObserverFixture,
                 "Filename observer: a rewritten print after a plain one survives the churn",
                 "[print_status][thumbnail][1339]") {
    // The case set_filename()-entry tests cannot reach. Print A leaves a name in
    // current_print_filename_, so when print B arrives as a rewritten copy the
    // re-entrant set_filename(printA) retires the override that was just
    // installed for B. Something must put it back before `desired` is computed.
    report_filename("printA.gcode");
    state().set_print_thumbnail("printA.gcode", THUMB_A);
    drain();
    REQUIRE(Access::displayed_src(panel()) == THUMB_A);

    report_filename(".helix_temp/modified_1748_Widget.gcode");

    CHECK(state().get_effective_print_filename() == "Widget.gcode");

    state().set_print_thumbnail("Widget.gcode", THUMB_B);
    drain();
    // The symptom this whole issue is about: print A's image must not still be
    // the one on screen.
    CHECK(Access::displayed_src(panel()) == THUMB_B);
    CHECK(Access::displayed_file(panel()) == "Widget.gcode");
}

TEST_CASE_METHOD(FilenameObserverFixture,
                 "Filename observer: a plain print after a plain one keeps no override",
                 "[print_status][thumbnail][1339]") {
    // Control. Two ordinary prints must leave the override EMPTY - if the resolve
    // logic ever starts installing one for a name that needs no resolving, the
    // retirement check becomes load-bearing for every print instead of just the
    // rewritten ones, and the two cases above would pass for the wrong reason.
    report_filename("printA.gcode");
    state().set_print_thumbnail("printA.gcode", THUMB_A);
    drain();
    REQUIRE(Access::displayed_src(panel()) == THUMB_A);

    report_filename("printB.gcode");
    CHECK(Access::identity_override(panel()).empty());
    CHECK(state().get_effective_print_filename() == "printB.gcode");

    state().set_print_thumbnail("printB.gcode", THUMB_B);
    drain();
    CHECK(Access::displayed_src(panel()) == THUMB_B);
    CHECK(Access::displayed_file(panel()) == "printB.gcode");
}
