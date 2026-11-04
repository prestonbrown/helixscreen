// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
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
    helix::ui::UpdateQueue::instance().drain_until_empty();
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
    ps.print_state().reset_print_start_state();
    drain_queue();
    REQUIRE(effective_status(state, "psu") == 1); // on, reachable

    // A host-side block: the wire still says standby.
    ps.print_state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    drain_queue();
    CHECK(effective_status(state, "psu") == 2); // locked

    // And it unlocks when the block is abandoned — a latched lock would strand
    // the user's PSU control for the rest of the session.
    ps.print_state().reset_print_start_state();
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

    ps.print_state().set_print_start_state(helix::PrintStartPhase::BED_MESH, "", 0);
    drain_queue();
    CHECK(effective_status(state, "light") == 1);

    ps.print_state().reset_print_start_state();
    drain_queue();
}

TEST_CASE_METHOD(LVGLTestFixture, "PowerDeviceState teardown detaches its print-lifecycle observer",
                 "[power_device_state][lifetime]") {
    // The print-lifecycle subject outlives this teardown, so an observer left
    // attached to it keeps calling back into a torn-down state and its context
    // is never freed. Repeated teardown/rediscovery must not accumulate them.
    auto& ps = get_printer_state();
    ps.init_subjects(false);
    auto& state = PowerDeviceState::instance();
    state.deinit_subjects();

    lv_subject_t* lifecycle = ps.print_state().get_print_lifecycle_subject();
    REQUIRE(lifecycle != nullptr);
    const uint32_t baseline = lv_ll_get_len(&lifecycle->subs_ll);

    for (int cycle = 0; cycle < 3; ++cycle) {
        state.set_devices({{"psu", "gpio", "on", true}});
        drain_queue();
        REQUIRE(lv_ll_get_len(&lifecycle->subs_ll) == baseline + 1);

        state.deinit_subjects();
        CHECK(lv_ll_get_len(&lifecycle->subs_ll) == baseline);
    }
}

// ============================================================================
// Rediscovery after a Klipper restart
//
// notify_klippy_ready re-runs discovery, which calls set_devices() with the
// same list. Tiles subscribe to a device's subject once, at attach, and the
// home panel does not rebuild when its widget list is unchanged, so a device
// still present must keep the subject those tiles watch.
// ============================================================================

namespace {

class RediscoveryFixture : public LVGLTestFixture {
  public:
    RediscoveryFixture() {
        PowerDeviceState::instance().deinit_subjects();
        PowerDeviceState::instance().subscribe(api_);
    }
    ~RediscoveryFixture() override {
        PowerDeviceState::instance().unsubscribe(api_);
        drain_queue();
    }

    /// A notify_power_changed frame for @p device, delivered the way the
    /// WebSocket delivers it, then the deferred update run.
    void notify(const char* device, const char* status) {
        client_.dispatch_method_callback(
            "notify_power_changed",
            {{"method", "notify_power_changed"},
             {"params", nlohmann::json::array({{{"device", device}, {"status", status}}})}});
        drain_queue();
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPIMock api_{client_, get_printer_state()};
};

/// Records every value an observer on a subject is told.
struct SeenValues {
    std::vector<int> values;
    static void cb(lv_observer_t* obs, lv_subject_t* subj) {
        static_cast<SeenValues*>(lv_observer_get_user_data(obs))
            ->values.push_back(lv_subject_get_int(subj));
    }
};

} // namespace

TEST_CASE_METHOD(RediscoveryFixture,
                 "PowerDeviceState: an observer keeps seeing changes after rediscovery",
                 "[power_device_state][rediscovery]") {
    auto& state = PowerDeviceState::instance();
    const std::vector<PowerDevice> devices = {{"printer_psu", "gpio", "off", false},
                                              {"chamber_light", "gpio", "on", false}};
    state.set_devices(devices);

    SubjectLifetime lt;
    lv_subject_t* subj = state.get_status_subject("printer_psu", lt);
    REQUIRE(subj != nullptr);
    SeenValues seen;
    lv_subject_add_observer(subj, SeenValues::cb, &seen);
    REQUIRE(seen.values == std::vector<int>{0});

    state.set_devices(devices);

    REQUIRE(lt);
    CHECK(*lt);
    SubjectLifetime again;
    CHECK(state.get_status_subject("printer_psu", again) == subj);

    notify("printer_psu", "on");
    CHECK(seen.values == std::vector<int>{0, 1});
    notify("printer_psu", "off");
    CHECK(seen.values == std::vector<int>{0, 1, 0});
}

TEST_CASE_METHOD(RediscoveryFixture,
                 "PowerDeviceState: rediscovery publishes a changed status in place",
                 "[power_device_state][rediscovery]") {
    auto& state = PowerDeviceState::instance();
    state.set_devices({{"printer_psu", "gpio", "off", false}});

    SubjectLifetime lt;
    lv_subject_t* subj = state.get_status_subject("printer_psu", lt);
    REQUIRE(subj != nullptr);
    SeenValues seen;
    lv_subject_add_observer(subj, SeenValues::cb, &seen);

    // The relay was switched on while Klipper was down.
    state.set_devices({{"printer_psu", "gpio", "on", false}});

    CHECK(seen.values == std::vector<int>{0, 1});
}

TEST_CASE_METHOD(RediscoveryFixture,
                 "PowerDeviceState: rediscovery drops a vanished device and keeps the rest",
                 "[power_device_state][rediscovery]") {
    auto& state = PowerDeviceState::instance();
    state.set_devices(
        {{"printer_psu", "gpio", "off", false}, {"chamber_light", "gpio", "on", false}});

    SubjectLifetime psu_lt;
    lv_subject_t* psu = state.get_status_subject("printer_psu", psu_lt);
    SubjectLifetime light_lt;
    REQUIRE(state.get_status_subject("chamber_light", light_lt) != nullptr);
    REQUIRE(light_lt);

    state.set_devices({{"printer_psu", "gpio", "off", false}});

    // The vanished device's observers learn it is gone before its subject dies.
    CHECK_FALSE(*light_lt);
    SubjectLifetime probe;
    CHECK(state.get_status_subject("chamber_light", probe) == nullptr);
    CHECK(state.device_names() == std::vector<std::string>{"printer_psu"});

    CHECK(*psu_lt);
    CHECK(state.get_status_subject("printer_psu", probe) == psu);
    notify("chamber_light", "off"); // an update for the dropped device is ignored
    notify("printer_psu", "on");
    CHECK(lv_subject_get_int(psu) == 1);
}

TEST_CASE_METHOD(RediscoveryFixture,
                 "PowerDeviceState: rediscovery adds a new device beside the kept ones",
                 "[power_device_state][rediscovery]") {
    auto& state = PowerDeviceState::instance();
    state.set_devices({{"printer_psu", "gpio", "off", false}});

    SubjectLifetime psu_lt;
    lv_subject_t* psu = state.get_status_subject("printer_psu", psu_lt);
    REQUIRE(psu != nullptr);

    state.set_devices(
        {{"printer_psu", "gpio", "off", false}, {"chamber_heater", "gpio", "on", true}});

    SubjectLifetime probe;
    CHECK(state.get_status_subject("printer_psu", probe) == psu);
    CHECK(*psu_lt);
    lv_subject_t* heater = state.get_status_subject("chamber_heater", probe);
    REQUIRE(heater != nullptr);
    CHECK(lv_subject_get_int(heater) == 1);
    CHECK(state.is_locked_while_printing("chamber_heater"));

    notify("chamber_heater", "off");
    CHECK(lv_subject_get_int(heater) == 0);
}
