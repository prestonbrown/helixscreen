// tests/unit/test_job_holds_machine.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `job_holds_machine` - the lifecycle-derived answer to "does a job own the
// toolhead right now?".
//
// The subject it replaces, `print_active`, is `PRINTING || PAUSED` read off the
// wire. That cannot see a job the app has committed to but the printer has not
// reported yet, so during a host-side pre-print block - the K2's forced bed mesh
// is the motivating case - `print_active` is 0 while the toolhead homes and
// probes. 21 XML bindings disable jog/motion/extrude on that subject, so all 21
// controls are live during exactly the window they exist to guard.
//
// These tests pin BOTH halves: the pure predicate over the lifecycle enum, and
// the published subject, driven through the same status/phase inputs the app
// gets. The host-side cases are the regression - each one asserts that
// job_holds_machine is 1 where print_active is 0.

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

// ============================================================================
// The pure predicate
// ============================================================================

TEST_CASE("job_holds_machine: true for every state in which a job owns the toolhead",
          "[core][print_state][job_holds_machine]") {
    REQUIRE(job_holds_machine(PrintState::Preparing));
    REQUIRE(job_holds_machine(PrintState::Printing));
    REQUIRE(job_holds_machine(PrintState::Paused));
}

TEST_CASE("job_holds_machine: false when no job owns the toolhead",
          "[core][print_state][job_holds_machine]") {
    REQUIRE_FALSE(job_holds_machine(PrintState::Idle));
    REQUIRE_FALSE(job_holds_machine(PrintState::Complete));
    REQUIRE_FALSE(job_holds_machine(PrintState::Cancelled));
    REQUIRE_FALSE(job_holds_machine(PrintState::Error));
}

TEST_CASE("job_holds_machine: Preparing is the whole point - it is what print_active cannot see",
          "[core][print_state][job_holds_machine]") {
    // If this ever flips, the predicate has collapsed back onto PRINTING||PAUSED
    // and every guard built on it silently loses the pre-print window.
    REQUIRE(job_holds_machine(PrintState::Preparing));
}

// ============================================================================
// The published subject
// ============================================================================

namespace {

struct JobHoldsMachineFixture : public LVGLTestFixture {
    JobHoldsMachineFixture() {
        state_.init_subjects(false);
    }

    /// set_print_start_state() defers, so drive the queue to quiescence rather
    /// than once - a handler running during a drain can queue more work.
    static void drain() {
        for (int pass = 0; pass < 8; ++pass) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    void report_job_state(const char* moonraker_state) {
        state_.update_from_status(json{{"print_stats", {{"state", moonraker_state}}}});
        drain();
    }

    void set_phase(PrintStartPhase phase) {
        state_.set_print_start_state(phase, "", 0);
        drain();
    }

    int holds() {
        return lv_subject_get_int(state_.get_job_holds_machine_subject());
    }

    int print_active() {
        return lv_subject_get_int(state_.get_print_active_subject());
    }

    PrinterState state_;
};

} // namespace

TEST_CASE_METHOD(JobHoldsMachineFixture, "job_holds_machine subject: 0 at rest",
                 "[core][printer_state][job_holds_machine]") {
    REQUIRE(holds() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: agrees with print_active "
                 "for a plain print with no pre-print phase",
                 "[core][printer_state][job_holds_machine]") {
    report_job_state("printing");
    REQUIRE(holds() == 1);
    REQUIRE(print_active() == 1);

    report_job_state("paused");
    REQUIRE(holds() == 1);
    REQUIRE(print_active() == 1);

    report_job_state("printing");
    REQUIRE(holds() == 1);

    report_job_state("complete");
    REQUIRE(holds() == 0);
    REQUIRE(print_active() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: 1 during a host-side pre-print block, where "
                 "print_active is 0",
                 "[core][printer_state][job_holds_machine]") {
    // The K2 shape: a forced bed mesh runs BEFORE the printer is handed the job,
    // so print_stats.state still reads standby while the toolhead is homing.
    report_job_state("standby");
    set_phase(PrintStartPhase::BED_MESH);

    REQUIRE(print_active() == 0); // the blind spot, unchanged
    REQUIRE(holds() == 1);        // the fix
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: 1 during a host-side block that follows a "
                 "finished job",
                 "[core][printer_state][job_holds_machine]") {
    // print_stats holds the PREVIOUS job's terminal state through the whole
    // host-side window. Nothing on the wire distinguishes this from idle.
    report_job_state("complete");
    set_phase(PrintStartPhase::HOMING);

    REQUIRE(print_active() == 0);
    REQUIRE(holds() == 1);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: stays 1 across the hand-off from a host-side "
                 "block into the real print",
                 "[core][printer_state][job_holds_machine]") {
    report_job_state("standby");
    set_phase(PrintStartPhase::BED_MESH);
    REQUIRE(holds() == 1);

    // Printer accepts the job while the phase is still live (firmware-side
    // PRINT_START continues past the hand-off).
    report_job_state("printing");
    REQUIRE(holds() == 1);

    set_phase(PrintStartPhase::IDLE);
    REQUIRE(holds() == 1); // now a plain print

    report_job_state("complete");
    REQUIRE(holds() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture,
                 "job_holds_machine subject: falls to 0 when a pre-print block is abandoned",
                 "[core][printer_state][job_holds_machine]") {
    // The failure mode that matters: a latched-true guard blocks motion for the
    // rest of the session. Assert the FALSE edge, not just the true one.
    report_job_state("standby");
    set_phase(PrintStartPhase::BED_MESH);
    REQUIRE(holds() == 1);

    set_phase(PrintStartPhase::IDLE);
    REQUIRE(holds() == 0);
}

TEST_CASE_METHOD(JobHoldsMachineFixture, "job_holds_machine subject: 0 for every terminal outcome",
                 "[core][printer_state][job_holds_machine]") {
    for (const char* terminal : {"complete", "cancelled", "error"}) {
        report_job_state("printing");
        REQUIRE(holds() == 1);
        report_job_state(terminal);
        REQUIRE(holds() == 0);
    }
}

// ============================================================================
// The XML seam
//
// Two ways this refactor fails silently:
//   1. The subject is registered under a different name than the XML asks for.
//      LVGL logs "No subject was found" and the binding is simply inert - the
//      control stays enabled and nothing crashes.
//   2. A later sweep "finishes the job" by putting the bindings back on
//      print_active, reopening the exact hole this closes.
// ============================================================================

#include "../test_fixtures.h"

#include <fstream>
#include <sstream>
#include <string>

TEST_CASE_METHOD(XMLTestFixture,
                 "job_holds_machine is registered in the XML scope under that exact name",
                 "[ui][xml][job_holds_machine]") {
    // Identity, not just non-null: a stale registry entry from another fixture
    // would still be non-null and would never move when this state publishes.
    lv_subject_t* from_xml = lv_xml_get_subject(nullptr, "job_holds_machine");
    REQUIRE(from_xml != nullptr);
    REQUIRE(from_xml == state().get_job_holds_machine_subject());
}

TEST_CASE_METHOD(XMLTestFixture, "the bypass tile is disabled during a host-side pre-print block",
                 "[ui][xml][job_holds_machine][bypass]") {
    REQUIRE(register_component("components/panel_widget_bypass"));
    lv_obj_t* tile = create_component("panel_widget_bypass");
    REQUIRE(tile != nullptr);

    REQUIRE_FALSE(lv_obj_has_state(tile, LV_STATE_DISABLED));

    // The K2 shape: print_stats still says standby while the toolhead is being
    // homed and probed by a host-side block. print_active stays 0 through all of
    // it, which is why this tile used to remain tappable.
    state().update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
    state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    for (int pass = 0; pass < 8; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }

    REQUIRE(lv_subject_get_int(state().get_print_active_subject()) == 0);
    REQUIRE(lv_obj_has_state(tile, LV_STATE_DISABLED));

    // And it comes back when the block is abandoned - a latched-disabled control
    // is the failure mode that would make this fix worse than the bug.
    state().set_print_start_state(helix::PrintStartPhase::IDLE, "", 0);
    for (int pass = 0; pass < 8; ++pass) {
        helix::ui::UpdateQueue::instance().drain();
    }
    REQUIRE_FALSE(lv_obj_has_state(tile, LV_STATE_DISABLED));
}

TEST_CASE("no toolhead-guarding XML binding is left on the raw print_active subject",
          "[ui][xml][job_holds_machine]") {
    // Pins the sweep itself. These four files hold every `disabled`-state
    // binding that exists to keep a control off the toolhead; all of them must
    // ask the lifecycle, not the wire. Run from the repo root (as the suite is).
    const char* files[] = {
        "ui_xml/controls_panel.xml",
        "ui_xml/micro/controls_panel.xml",
        "ui_xml/motion_panel.xml",
        "ui_xml/components/panel_widget_bypass.xml",
    };

    int derived_bindings = 0;
    for (const char* path : files) {
        std::ifstream in(path);
        INFO("reading " << path);
        REQUIRE(in.good());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string xml = ss.str();

        INFO(path << " still binds a disabled state to the raw print_active subject");
        REQUIRE(xml.find("<bind_state_if_eq subject=\"print_active\" state=\"disabled\"") ==
                std::string::npos);

        // The full form, including the direction. Counting only the subject name
        // would let ref_value="0" through - controls disabled when idle and live
        // while printing, the exact inversion - and the census would still read
        // 21. Only the bypass tile has its direction pinned behaviourally.
        size_t pos = 0;
        const std::string needle =
            "<bind_state_if_eq subject=\"job_holds_machine\" state=\"disabled\" ref_value=\"1\"/>";
        while ((pos = xml.find(needle, pos)) != std::string::npos) {
            ++derived_bindings;
            pos += needle.size();
        }

        // A control with a SECOND reason to be disabled cannot carry two binds:
        // each bind_state_* sets or clears the state, so the later one undoes
        // the earlier. Those fold the lifecycle guard into one compound cond=,
        // which still pins the direction (`job_holds_machine eq 1`) and still
        // counts - a silent drop is exactly as visible either way.
        pos = 0;
        const std::string compound = "<bind_state_if cond=\"job_holds_machine eq 1 or ";
        while ((pos = xml.find(compound, pos)) != std::string::npos) {
            const size_t end = xml.find("/>", pos);
            INFO(path << " has an unterminated bind_state_if");
            REQUIRE(end != std::string::npos);
            INFO(path << " folds job_holds_machine into a bind that is not the disabled state");
            REQUIRE(xml.substr(pos, end - pos).find("state=\"disabled\"") != std::string::npos);
            ++derived_bindings;
            pos = end;
        }
    }

    // The census count. If a binding is legitimately added or removed, update
    // this - the number existing is what makes a silent drop visible.
    REQUIRE(derived_bindings == 21);
}
