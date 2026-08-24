// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "moonraker_types.h"
#include "power_device_state.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState tracks device state", "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    std::vector<PowerDevice> devices = {
        {"printer_psu", "gpio", "off", false},
        {"chamber_light", "klipper_device", "on", true},
    };
    state.set_devices(devices);

    REQUIRE(state.device_names().size() == 2);
    REQUIRE(state.is_locked_while_printing("chamber_light") == true);
    REQUIRE(state.is_locked_while_printing("printer_psu") == false);

    SubjectLifetime lt;
    auto* psu_subj = state.get_status_subject("printer_psu", lt);
    REQUIRE(psu_subj != nullptr);
    REQUIRE(lv_subject_get_int(psu_subj) == 0); // off

    auto* light_subj = state.get_status_subject("chamber_light", lt);
    REQUIRE(light_subj != nullptr);
    REQUIRE(lv_subject_get_int(light_subj) == 1); // on

    REQUIRE(state.get_status_subject("nonexistent", lt) == nullptr);

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState replaces devices on re-discovery",
                 "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    // Initial set
    state.set_devices({{"dev_a", "gpio", "on", false}});
    REQUIRE(state.device_names().size() == 1);

    // Replace with different set
    state.set_devices({{"dev_b", "gpio", "off", true}, {"dev_c", "klipper_device", "on", false}});
    REQUIRE(state.device_names().size() == 2);

    SubjectLifetime lt;
    REQUIRE(state.get_status_subject("dev_a", lt) == nullptr);
    REQUIRE(state.get_status_subject("dev_b", lt) != nullptr);
    REQUIRE(lv_subject_get_int(state.get_status_subject("dev_b", lt)) == 0); // off

    state.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState deinit clears all subjects",
                 "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    state.set_devices({{"psu", "gpio", "on", false}});
    REQUIRE(state.device_names().size() == 1);

    state.deinit_subjects();
    REQUIRE(state.device_names().empty());

    SubjectLifetime lt;
    REQUIRE(state.get_status_subject("psu", lt) == nullptr);
}

// ============================================================================
// locked_while_printing during a pre-print block
//
// A device flagged locked_while_printing is typically the printer PSU or a bound
// relay. The lock existed only for PRINTING/PAUSED, read off the wire — so during
// a HOST-side pre-start block, where print_stats still says standby, the toggle
// was live while the printer was homing and probing under power. Cutting the PSU
// mid-probe is as destructive as cutting it mid-layer.
//
// `effective` is 0=off, 1=on, 2=locked, and had NO coverage at all before this.
//
// Mutation check: revert the predicate to `state == PRINTING || state == PAUSED`
// and the Preparing cases below drop to 1 (unlocked). Re-point the observer back
// to print_state_enum and they fail too — the wire never moves during the block,
// so reevaluate_lock_states() would never run.
// ============================================================================

namespace {

void drain_queue() {
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

int effective_status(PowerDeviceState& st, const char* name) {
    SubjectLifetime lt;
    lv_subject_t* s = st.get_status_subject(name, lt);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState locks a device during a host-side pre-print",
                 "[power_device_state][preparing]") {
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    auto& state = PowerDeviceState::instance();
    // The print-state observer is registered once, guarded on
    // !subjects_initialized_, and this is a process singleton - so whichever test
    // called set_devices() first owns it. Tear down so it re-registers against
    // THIS PrinterState, or the lock never re-evaluates here.
    state.deinit_subjects();

    // locked_while_printing=true, currently on.
    state.set_devices({{"psu", "gpio", "on", true}});
    drain_queue();

    helix::test::set_wire_state(ps, helix::PrintJobState::STANDBY);
    ps.reset_print_start_state();
    drain_queue();
    REQUIRE(effective_status(state, "psu") == 1); // on, reachable

    // A host-side block: the wire still says standby.
    ps.set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    drain_queue();
    CHECK(effective_status(state, "psu") == 2); // locked

    // And it unlocks when the block is abandoned — a latched lock would strand
    // the user's PSU control for the rest of the session.
    ps.reset_print_start_state();
    drain_queue();
    CHECK(effective_status(state, "psu") == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState still locks while printing and paused",
                 "[power_device_state][preparing]") {
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    auto& state = PowerDeviceState::instance();
    // The print-state observer is registered once, guarded on
    // !subjects_initialized_, and this is a process singleton - so whichever test
    // called set_devices() first owns it. Tear down so it re-registers against
    // THIS PrinterState, or the lock never re-evaluates here.
    state.deinit_subjects();
    state.set_devices({{"psu", "gpio", "on", true}});
    drain_queue();

    helix::test::set_wire_state(ps, helix::PrintJobState::PRINTING);
    drain_queue();
    CHECK(effective_status(state, "psu") == 2);

    helix::test::set_wire_state(ps, helix::PrintJobState::PAUSED);
    drain_queue();
    CHECK(effective_status(state, "psu") == 2);

    helix::test::set_wire_state(ps, helix::PrintJobState::COMPLETE);
    drain_queue();
    CHECK(effective_status(state, "psu") == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState leaves unflagged devices reachable",
                 "[power_device_state][preparing]") {
    // The lock is opt-in per device: a chamber light must stay controllable
    // through a pre-print block.
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    auto& state = PowerDeviceState::instance();
    // The print-state observer is registered once, guarded on
    // !subjects_initialized_, and this is a process singleton - so whichever test
    // called set_devices() first owns it. Tear down so it re-registers against
    // THIS PrinterState, or the lock never re-evaluates here.
    state.deinit_subjects();
    state.set_devices({{"light", "gpio", "on", false}});
    drain_queue();

    ps.set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    drain_queue();
    CHECK(effective_status(state, "light") == 1);

    ps.reset_print_start_state();
    drain_queue();
}
