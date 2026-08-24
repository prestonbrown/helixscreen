// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contextual E-Stop must be reachable from the moment the machine starts
// moving - which is BEFORE Moonraker reports a print.
//
// A host-side pre-start block (the K2's forced bed mesh) homes and probes while
// print_stats.state still reads standby, or the PREVIOUS job's terminal state.
// EmergencyStopOverlay used to cover that by hand-ORing the raw job state with
// "print_start_phase != 0" and watching two subjects to catch both halves. That
// is job_holds_machine(print_lifecycle) spelled out, so it now asks the
// lifecycle once, through one observer.
//
// Nothing about the button's visibility should change - that is the point, and
// it is why this file exists: nothing previously asserted estop_visible == 1
// during a pre-print block, so the de-duplication had no safety net.

#include "ui_emergency_stop.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "moonraker_api.h"
#include "printer_state.h"

#include <lvgl.h>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

namespace {

struct EstopVisibilityFixture : public LVGLUITestFixture {
    EstopVisibilityFixture() {
        auto& estop = EmergencyStopOverlay::instance();
        estop.init(state(), api());
        estop.create();
    }

    static void drain() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    void report_job_state(const char* wire) {
        state().update_from_status(json{{"print_stats", {{"state", wire}}}});
        drain();
    }

    void set_phase(helix::PrintStartPhase phase) {
        state().set_print_start_state(phase, "", 0);
        drain();
    }

    int visible() {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "estop_visible");
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
};

} // namespace

TEST_CASE_METHOD(EstopVisibilityFixture, "estop is hidden when nothing owns the machine",
                 "[recovery][estop][preparing]") {
    report_job_state("standby");
    CHECK(visible() == 0);
}

TEST_CASE_METHOD(EstopVisibilityFixture, "estop is visible while printing and while paused",
                 "[recovery][estop][preparing]") {
    report_job_state("printing");
    CHECK(visible() == 1);

    report_job_state("paused");
    CHECK(visible() == 1);
}

TEST_CASE_METHOD(EstopVisibilityFixture,
                 "estop is visible during a HOST-side pre-print block, where the wire says standby",
                 "[recovery][estop][preparing]") {
    // The case the second observer existed for. The toolhead is homing and
    // probing; print_stats has not mentioned a job yet.
    report_job_state("standby");
    set_phase(helix::PrintStartPhase::BED_MESH);
    CHECK(visible() == 1);
}

TEST_CASE_METHOD(EstopVisibilityFixture,
                 "estop is visible during a pre-print block that follows a finished job",
                 "[recovery][estop][preparing]") {
    // print_stats holds the PREVIOUS job's terminal state for the whole window,
    // so a predicate keyed on "not terminal" would hide the button here.
    report_job_state("complete");
    set_phase(helix::PrintStartPhase::HOMING);
    CHECK(visible() == 1);
}

TEST_CASE_METHOD(EstopVisibilityFixture, "estop hides again once the block is abandoned",
                 "[recovery][estop][preparing]") {
    // A latched-visible E-Stop is less harmful than a latched-hidden one, but it
    // still means the button never goes away on an idle printer.
    report_job_state("standby");
    set_phase(helix::PrintStartPhase::BED_MESH);
    REQUIRE(visible() == 1);

    set_phase(helix::PrintStartPhase::IDLE);
    CHECK(visible() == 0);
}

TEST_CASE_METHOD(EstopVisibilityFixture,
                 "estop stays visible across the hand-off from block into print",
                 "[recovery][estop][preparing]") {
    // The button must not blink off at the moment the printer accepts the job.
    report_job_state("standby");
    set_phase(helix::PrintStartPhase::BED_MESH);
    REQUIRE(visible() == 1);

    report_job_state("printing");
    CHECK(visible() == 1);

    set_phase(helix::PrintStartPhase::IDLE);
    CHECK(visible() == 1);

    report_job_state("complete");
    CHECK(visible() == 0);
}
