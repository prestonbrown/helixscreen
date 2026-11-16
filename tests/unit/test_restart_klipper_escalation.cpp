// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_emergency_stop.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../test_helpers/mock_printer.h"
#include "moonraker_error.h"

#include <optional>

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

/// Answers each restart at once, so a recovery chain the overlay starts also
/// ends inside the test: firmware_restart fails as it does with Klippy gone,
/// and the chain's service restart succeeds.
class EscalationApi : public MoonrakerAPIMock {
  public:
    using MoonrakerAPIMock::MoonrakerAPIMock;

    std::optional<MoonrakerError> klipper_restart_error;
    int klipper_restarts = 0;
    int firmware_restarts = 0;

    void restart_klipper(SuccessCallback on_success, ErrorCallback on_error) override {
        ++klipper_restarts;
        if (klipper_restart_error) {
            on_error(*klipper_restart_error);
            return;
        }
        on_success();
    }
    void restart_firmware(SuccessCallback /*on_success*/, ErrorCallback on_error) override {
        ++firmware_restarts;
        on_error(MoonrakerError::connection_lost("printer.firmware_restart"));
    }
    void restart_service(const std::string& /*service*/, SuccessCallback on_success,
                         ErrorCallback /*on_error*/) override {
        on_success();
    }
};

} // namespace

// Restart Klipper asks Klippy to restart itself, so with the Klippy host gone
// it can only fail; the recovery chain restarts the host from outside.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "Restart Klipper escalates to the recovery chain when Klippy is not connected",
                 "[recovery][restart_klipper]") {
    MockPrinter printer;
    EscalationApi api{printer.client, printer.state};
    auto& estop = EmergencyStopOverlay::instance();
    estop.init(printer.state, &api);

    SECTION("Moonraker's Klippy-disconnected answer starts the recovery chain") {
        api.klipper_restart_error = restart_error(503, "Klippy Disconnected");
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(api.klipper_restarts == 1);
        CHECK(api.firmware_restarts == 1);
    }

    SECTION("Klipper's own refusal is reported, not escalated") {
        api.klipper_restart_error = restart_error(400, "Must home axis first");
        EmergencyStopOverlayTestAccess::restart_klipper(estop);
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(api.klipper_restarts == 1);
        CHECK(api.firmware_restarts == 0);
    }
}
