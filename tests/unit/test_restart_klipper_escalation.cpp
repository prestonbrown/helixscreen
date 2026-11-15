// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_emergency_stop.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "core/update_queue.h"
#include "moonraker_error.h"

#include "../catch_amalgamated.hpp"

namespace {

MoonrakerError restart_error(int code, const char* message) {
    MoonrakerError err;
    err.type = MoonrakerErrorType::JSON_RPC_ERROR;
    err.code = code;
    err.message = message;
    err.method = "printer.restart";
    return err;
}

} // namespace

// Restart Klipper asks Klippy to restart itself, so with the Klippy host gone
// it can only fail; the recovery chain restarts the host from outside.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Restart Klipper escalates to the recovery chain when Klippy is not connected",
                 "[recovery][restart_klipper]") {
    MockPrinter printer;
    auto& estop = EmergencyStopOverlay::instance();
    estop.init(printer.state, &printer.api);
    EmergencyStopOverlayTestAccess::reset_suppression(estop);

    SECTION("Moonraker's Klippy-disconnected answer starts the recovery chain") {
        printer.client.fail_next("printer.restart", restart_error(503, "Klippy Disconnected"));
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(printer.client.call_count("printer.restart") == 1);
        CHECK(printer.client.call_count("printer.firmware_restart") == 1);
    }

    SECTION("Klipper's own refusal is reported, not escalated") {
        printer.client.fail_next("printer.restart", restart_error(400, "Must home axis first"));
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(printer.client.call_count("printer.restart") == 1);
        CHECK(printer.client.call_count("printer.firmware_restart") == 0);
    }

    EmergencyStopOverlayTestAccess::reset_suppression(estop);
}
