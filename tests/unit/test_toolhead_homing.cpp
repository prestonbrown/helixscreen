// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "core/update_queue.h"
#include "lvgl_test_fixture.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "toolhead_homing.h"

#include <chrono>
#include <optional>

#include "../catch_amalgamated.hpp"

TEST_CASE("toolhead_is_homed reads the live homed_axes subject", "[homing][toolhead]") {
    LVGLTestFixture fixture;
    helix::PrinterState ps;
    ps.init_subjects(true);

    SECTION("empty means not homed") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "");
        CHECK_FALSE(helix::toolhead_is_homed(ps));
    }
    SECTION("xyz means homed") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "xyz");
        CHECK(helix::toolhead_is_homed(ps));
    }
    SECTION("partial homing is NOT homed") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "xy");
        CHECK_FALSE(helix::toolhead_is_homed(ps));
    }
    SECTION("z-only is NOT homed") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "z");
        CHECK_FALSE(helix::toolhead_is_homed(ps));
    }
}

TEST_CASE("axis_is_homed reads each axis independently", "[homing][toolhead]") {
    LVGLTestFixture fixture;
    helix::PrinterState ps;
    ps.init_subjects(false); // false = skip XML registration (test default)

    SECTION("xy homed leaves z unhomed") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "xy");
        CHECK(helix::axis_is_homed(ps, helix::Axis::X));
        CHECK(helix::axis_is_homed(ps, helix::Axis::Y));
        CHECK_FALSE(helix::axis_is_homed(ps, helix::Axis::Z));
        CHECK_FALSE(helix::toolhead_is_homed(ps));
    }
    SECTION("empty homed_axes means nothing is homed") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "");
        CHECK_FALSE(helix::axis_is_homed(ps, helix::Axis::X));
        CHECK_FALSE(helix::axis_is_homed(ps, helix::Axis::Y));
        CHECK_FALSE(helix::axis_is_homed(ps, helix::Axis::Z));
    }
    SECTION("xyz homed reports all three axes") {
        lv_subject_copy_string(ps.motion_state().get_homed_axes_subject(), "xyz");
        CHECK(helix::axis_is_homed(ps, helix::Axis::X));
        CHECK(helix::axis_is_homed(ps, helix::Axis::Y));
        CHECK(helix::axis_is_homed(ps, helix::Axis::Z));
    }
}

TEST_CASE("free ensure_homed_then runs then() synchronously when homed", "[homing][toolhead]") {
    LVGLTestFixture fixture;
    // ensure_homed_then() has no PrinterState parameter — it reads the
    // process-wide singleton via get_printer_state(), same as every caller
    // it replaces. Drive that instance directly rather than a disconnected
    // local PrinterState (which owns its own, unrelated subject storage).
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    lv_subject_copy_string(state.motion_state().get_homed_axes_subject(), "xyz");

    helix::AsyncLifetimeGuard guard;
    bool ran = false;
    bool error_ran = false;
    helix::ensure_homed_then(
        nullptr, guard, [&ran] { ran = true; },
        [&error_ran](const MoonrakerError&) { error_ran = true; });
    CHECK(ran);
    CHECK_FALSE(error_ran);
}

TEST_CASE("free ensure_homed_then does not run then() synchronously when not homed",
          "[homing][toolhead]") {
    LVGLTestFixture fixture;
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    lv_subject_copy_string(state.motion_state().get_homed_axes_subject(), "");

    helix::AsyncLifetimeGuard guard;
    bool ran = false;
    // api == nullptr: there is nothing to send G28 with. Must not crash on
    // a null dereference, and then() must not fire synchronously — only a
    // real G28 completion (which cannot happen without an API) may run it.
    helix::ensure_homed_then(nullptr, guard, [&ran] { ran = true; }, nullptr);
    CHECK_FALSE(ran);
}

TEST_CASE("ensure_homed_then with HomingPolicy::Always homes an already-homed toolhead",
          "[homing][toolhead]") {
    LVGLTestFixture fixture;
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    lv_subject_copy_string(state.motion_state().get_homed_axes_subject(), "xyz");

    helix::AsyncLifetimeGuard guard;
    bool ran = false;
    std::optional<MoonrakerErrorType> error;
    // With no API the G28 cannot be sent, so the error path proves the policy
    // asked for one instead of skipping straight to then().
    helix::ensure_homed_then(
        nullptr, guard, [&ran] { ran = true; },
        [&error](const MoonrakerError& err) { error = err.type; }, helix::HomingPolicy::Always);
    CHECK_FALSE(ran);
    REQUIRE(error);
    CHECK(*error == MoonrakerErrorType::CONNECTION_LOST);
}

TEST_CASE("ensure_homed_then's continuation is not refused as someone else's busy",
          "[homing][toolhead][busy_guard]") {
    LVGLTestFixture fixture;
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    state.set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    lv_subject_copy_string(state.motion_state().get_homed_axes_subject(), "");

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    client.connect("ws://mock/websocket", []() {}, []() {});
    MoonrakerAPI api{client, state};

    helix::AsyncLifetimeGuard guard;
    bool ran = false;
    bool refused_as_external = true;
    helix::ensure_homed_then(
        &api, guard,
        [&] {
            ran = true;
            refused_as_external = state.is_external_blocking_operation_active();
        },
        nullptr);
    // The G28 is seen holding idle_timeout "Printing", and the homing macro is
    // still moving the bed past SETTLE when G28 acks. Ownership needs the send
    // to precede the episode, so the episode is observed now and allowed to
    // settle.
    lv_subject_set_int(state.calibration_state().get_idle_timeout_printing_subject(), 1);
    state.calibration_state().idle_timeout_busy().set_printing(true);
    helix::BusyClock::advance_for_test(helix::IdleTimeoutBusy::kSettle +
                                       std::chrono::milliseconds(100));
    REQUIRE(state.is_blocking_operation_active());
    helix::ui::UpdateQueue::instance().drain();

    REQUIRE(ran);
    CHECK_FALSE(refused_as_external);

    PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, false);
    client.stop_temperature_simulation();
    client.disconnect();
}

TEST_CASE("ensure_homed_then's G28 ack adopts no episode that begins after it",
          "[homing][toolhead][busy_guard]") {
    LVGLTestFixture fixture;
    helix::PrinterState& state = get_printer_state();
    state.init_subjects(false);
    state.set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    lv_subject_copy_string(state.motion_state().get_homed_axes_subject(), "");

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    client.connect("ws://mock/websocket", []() {}, []() {});
    MoonrakerAPI api{client, state};

    helix::AsyncLifetimeGuard guard;
    bool ran = false;
    helix::ensure_homed_then(&api, guard, [&] { ran = true; }, nullptr);
    // A homing pass longer than the grace window, then the ack.
    helix::BusyClock::advance_for_test(helix::BusyEpisodeOwner::kGraceWindow +
                                       std::chrono::milliseconds(100));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ran);

    // An operation someone else starts just after the ack is not the app's.
    const auto external = helix::BusyEpisodeOwner::clock::now() + std::chrono::milliseconds(100);
    CHECK_FALSE(state.app_motion_activity().owns_busy_episode(external,
                                                              external + std::chrono::seconds(1)));

    client.stop_temperature_simulation();
    client.disconnect();
}
