// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A Firmware Restart or Restart Klipper requested while one is already in
// flight is refused: stacking a second restart on a Klipper that is still
// coming up can leave it halted on a busy serial port. The in-flight window
// ends when klippy reports READY or its backstop deadline passes.

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../ui_test_utils.h"
#include "moonraker_api_mock.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::KlippyState;
using helix::ui::UpdateQueue;

namespace {

/// Counts the restart RPCs instead of sending them. The firmware restart fails
/// over to a services restart that succeeds, so PrinterRecoveryService's chain
/// ends and the next test can start a recovery of its own.
class CountingRestartApi : public MoonrakerAPIMock {
  public:
    using MoonrakerAPIMock::MoonrakerAPIMock;

    int klipper_restarts = 0;
    int firmware_restarts = 0;
    /// When set, every restart the API is asked for fails with this error.
    std::optional<MoonrakerError> fail_with;

    void restart_klipper(SuccessCallback on_success, ErrorCallback on_error) override {
        ++klipper_restarts;
        if (fail_with) {
            on_error(*fail_with);
            return;
        }
        on_success();
    }
    void restart_firmware(SuccessCallback /*on_success*/, ErrorCallback on_error) override {
        ++firmware_restarts;
        on_error(MoonrakerError::connection_lost("printer.firmware_restart"));
    }
    void restart_service(const std::string& /*service*/, SuccessCallback on_success,
                         ErrorCallback on_error) override {
        if (fail_with) {
            on_error(*fail_with);
            return;
        }
        on_success();
    }
};

class RestartInFlightFixture : public XMLTestFixture {
  public:
    CountingRestartApi counting_api{client(), state()};

    struct Shown {
        ToastSeverity severity;
        std::string message;
    };
    std::vector<Shown> shown;

    RestartInFlightFixture() {
        ToastManager::instance().init();
        auto& estop = EmergencyStopOverlay::instance();
        estop.init(state(), &counting_api);
        estop.init_subjects();
        estop.create(); // subscribes the klippy_state observer

        // Burn the observer's initial-fire guard on a suppressed SHUTDOWN, so
        // the READY below is a real transition and no recovery dialog exists.
        estop.suppress_recovery_dialog(recovery_suppression::LONG);
        state().set_klippy_state_sync(KlippyState::SHUTDOWN);
        settle();
        EmergencyStopOverlayTestAccess::reset_suppression(estop);
        EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, false);

        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& message,
                                              uint32_t) { shown.push_back({severity, message}); });
    }

    ~RestartInFlightFixture() override {
        helix::ui::set_test_toast_hook(nullptr);
        auto& estop = EmergencyStopOverlay::instance();
        EmergencyStopOverlayTestAccess::reset_suppression(estop);
        EmergencyStopOverlayTestAccess::set_restart_in_progress(estop, false);
        estop.deinit_subjects();
        EmergencyStopOverlayTestAccess::reset_dependencies(estop);
        UpdateQueue::instance().drain();
    }

    void settle() {
        UpdateQueue::instance().drain_until_empty(16);
    }

    [[nodiscard]] bool toasted(const std::string& message) const {
        for (const auto& s : shown) {
            if (s.message == message) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE_METHOD(RestartInFlightFixture, "A restart in flight refuses a second restart",
                 "[recovery][restart_in_flight]") {
    auto& estop = EmergencyStopOverlay::instance();

    EmergencyStopOverlayTestAccess::firmware_restart(estop);
    settle();
    REQUIRE(counting_api.firmware_restarts == 1);

    SECTION("Restart Klipper while a firmware restart is in flight") {
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        settle();
        CHECK(counting_api.klipper_restarts == 0);
    }
    SECTION("A second Firmware Restart") {
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        CHECK(counting_api.firmware_restarts == 1);
    }
    CHECK(toasted("A restart is already in progress"));
}

TEST_CASE_METHOD(RestartInFlightFixture, "A restart is accepted again once klippy reports READY",
                 "[recovery][restart_in_flight]") {
    auto& estop = EmergencyStopOverlay::instance();

    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    REQUIRE(counting_api.klipper_restarts == 1);

    state().set_klippy_state_sync(KlippyState::READY);
    settle();

    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    CHECK(counting_api.klipper_restarts == 2);
    CHECK_FALSE(toasted("A restart is already in progress"));
}

TEST_CASE_METHOD(RestartInFlightFixture, "A restart that never reports READY stops refusing",
                 "[recovery][restart_in_flight]") {
    auto& estop = EmergencyStopOverlay::instance();

    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    REQUIRE(counting_api.klipper_restarts == 1);

    lv_tick_inc(recovery_suppression::kRestartFlagTimeout + 1000);

    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    CHECK(counting_api.klipper_restarts == 2);
}

TEST_CASE_METHOD(RestartInFlightFixture, "A restart that failed to send refuses nothing after it",
                 "[recovery][restart_in_flight]") {
    auto& estop = EmergencyStopOverlay::instance();
    // Moonraker answered and refused: the command never reached Klipper.
    counting_api.fail_with = MoonrakerError::json_rpc_error("printer.restart", "refused");

    SECTION("Restart Klipper") {
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        settle();
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        settle();
        CHECK(counting_api.klipper_restarts == 2);
    }
    SECTION("Firmware Restart, whose whole recovery chain failed") {
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        CHECK(counting_api.firmware_restarts == 2);
    }
    CHECK_FALSE(toasted("A restart is already in progress"));
}

TEST_CASE_METHOD(RestartInFlightFixture,
                 "A restart that came up into a shutdown accepts the Firmware Restart it needs",
                 "[recovery][restart_in_flight]") {
    auto& estop = EmergencyStopOverlay::instance();
    state().set_klippy_state_sync(KlippyState::READY);
    settle();

    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    REQUIRE(counting_api.klipper_restarts == 1);

    SECTION("Klipper started, then halted on an MCU shutdown") {
        state().set_klippy_state_sync(KlippyState::STARTUP);
        state().set_klippy_state_sync(KlippyState::SHUTDOWN);
        settle();
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        CHECK(counting_api.firmware_restarts == 1);
        CHECK_FALSE(toasted("A restart is already in progress"));
    }
    SECTION("the shutdown a restart passes on its way down is still in flight") {
        state().set_klippy_state_sync(KlippyState::SHUTDOWN);
        settle();
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        CHECK(counting_api.firmware_restarts == 0);
    }
}

TEST_CASE_METHOD(RestartInFlightFixture,
                 "A restart whose reply the restart's own disconnect dropped is still in flight",
                 "[recovery][restart_in_flight]") {
    // Klipper going down fails every pending request with connection_lost,
    // the restart's own reply among them, while the restart carries on.
    auto& estop = EmergencyStopOverlay::instance();
    counting_api.fail_with = MoonrakerError::connection_lost("printer.restart");

    SECTION("Restart Klipper") {
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        settle();
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        settle();
        CHECK(counting_api.klipper_restarts == 1);
    }
    SECTION("Firmware Restart") {
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        EmergencyStopOverlayTestAccess::firmware_restart(estop);
        settle();
        CHECK(counting_api.firmware_restarts == 1);
    }
    CHECK(toasted("A restart is already in progress"));
}

TEST_CASE_METHOD(RestartInFlightFixture,
                 "A startup seen by an expired restart does not end the next one at its shutdown",
                 "[recovery][restart_in_flight]") {
    auto& estop = EmergencyStopOverlay::instance();
    state().set_klippy_state_sync(KlippyState::READY);
    settle();

    // The first restart starts up, then runs out its backstop with no READY.
    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    state().set_klippy_state_sync(KlippyState::STARTUP);
    settle();
    lv_tick_inc(recovery_suppression::kRestartFlagTimeout + 1000);

    EmergencyStopOverlayTestAccess::restart_klipper(estop);
    settle();
    REQUIRE(counting_api.klipper_restarts == 2);

    // The second restart's way down: a SHUTDOWN with no STARTUP of its own.
    state().set_klippy_state_sync(KlippyState::SHUTDOWN);
    settle();
    EmergencyStopOverlayTestAccess::firmware_restart(estop);
    settle();
    CHECK(counting_api.firmware_restarts == 0);
}
