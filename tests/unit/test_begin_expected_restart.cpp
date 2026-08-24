// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// begin_expected_klippy_restart() is the initiation half of the klippy-restart
// contract; the klippy_state READY observer in ui_emergency_stop.cpp is the
// completion half. These pin the helper's own contract, then drive the real
// flows consolidated onto it (input-shaper save, bed-mesh save, PID save,
// z-offset apply-and-save), asserting per flow: exactly one direct INFO
// initiation toast with the flow's wording, NO ui_notification_* fire (a
// direct toast writes no notification-history row), the RPC dispatched
// through the mock with the right payload, and both suppression windows
// armed. The 15000 ms disconnect-modal duration is asserted as a literal so
// a silent change to the constant fails here.
//
// Mock stack follows test_ams_home_confirmation.cpp: MoonrakerClientMock +
// MoonrakerAPIMock over a stack PrinterState, RPC callbacks drained through
// UpdateQueueTestAccess. The api is published via set_moonraker_api() because
// the helper and BedMeshPanel reach it through the global accessor.

#include "ui_emergency_stop.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_input_shaper.h"
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/bed_mesh_panel_test_access.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../test_helpers/input_shaper_panel_test_access.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "z_offset_utils.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

namespace {

class ExpectedRestartFixture : public LVGLTestFixture {
  public:
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    MoonrakerAPIMock api{client, state};

    struct Shown {
        ToastSeverity severity;
        std::string message;
    };
    std::vector<Shown> toasts;
    std::vector<std::string> notifications;

    ExpectedRestartFixture() {
        state.init_subjects(false);
        set_moonraker_api(&api);
        helix::ui::set_test_toast_hook([this](ToastSeverity severity, const std::string& m) {
            toasts.push_back({severity, m});
        });
        auto note = [this](const std::string& m) { notifications.push_back(m); };
        helix::ui::set_test_notification_info_hook(note);
        helix::ui::set_test_notification_warning_hook(note);
        client.clear_gcode_script_history();
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
    }

    ~ExpectedRestartFixture() override {
        set_moonraker_api(nullptr);
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::set_test_notification_info_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
        settle();
    }

    // Drains until the queue stops producing work: the helper queues its
    // toast, and RPC callbacks hop the queue again before side effects land.
    void settle() {
        for (int i = 0; i < 4; i++) {
            helix::ui::UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        }
    }
};

} // namespace

TEST_CASE_METHOD(ExpectedRestartFixture,
                 "begin_expected_klippy_restart arms both windows and shows one INFO toast",
                 "[recovery][expectedrestart]") {
    helix::ui::begin_expected_klippy_restart("Saving config... Klipper will restart.");
    settle();

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    REQUIRE(api.suppress_disconnect_modal_calls() == 1);
    CHECK(api.last_suppress_disconnect_modal_ms() == 15000);

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Saving config... Klipper will restart.");
    CHECK(notifications.empty()); // direct toast - no history row
}

TEST_CASE_METHOD(ExpectedRestartFixture, "z-offset apply-and-save initiates the restart contract",
                 "[expectedrestart][zoffset]") {
    helix::zoffset::apply_and_save(
        &api, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE, [] {}, [](const std::string&) {});
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 2);
    CHECK(hist[0] == "Z_OFFSET_APPLY_PROBE");
    CHECK(hist[1] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Saving config... Klipper will restart.");
    CHECK(notifications.empty());
}

TEST_CASE_METHOD(ExpectedRestartFixture, "bed-mesh SAVE_CONFIG initiates the restart contract",
                 "[expectedrestart][bedmesh]") {
    BedMeshPanel panel;
    helix::ui::BedMeshPanelTestAccess::save_config(panel);
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Configuration saved - restarting");
    // The acceptance-time NOTIFY_INFO is gone: the initiation toast plus the
    // READY completion carry the information now, and neither writes a
    // history row.
    CHECK(notifications.empty());
}

TEST_CASE_METHOD(ExpectedRestartFixture, "input-shaper SAVE_CONFIG initiates the restart contract",
                 "[expectedrestart]") {
    InputShaperPanel panel;
    panel.set_api(&client, &api);
    helix::ui::InputShaperPanelTestAccess::save_configuration(panel);
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    // The mock's SAVE_CONFIG handler returns error-by-design (klippy
    // "restarts" before the reply arrives), so the panel's direct failure
    // toast also appears on this stack and can precede the queued initiation
    // toast - order is not the contract. The initiation toast itself must be
    // INFO - not the WARNING this flow used before the helper; an expected
    // restart is not a fault, and it pairs with the SUCCESS completion toast.
    auto initiation = std::find_if(toasts.begin(), toasts.end(), [](const Shown& t) {
        return t.message == "Saving config... Klipper will restart.";
    });
    REQUIRE(initiation != toasts.end());
    CHECK(initiation->severity == ToastSeverity::INFO);
    CHECK(std::count_if(toasts.begin(), toasts.end(), [](const Shown& t) {
              return t.message == "Saving config... Klipper will restart.";
          }) == 1);
    CHECK(notifications.empty());
}

TEST_CASE_METHOD(ExpectedRestartFixture, "PID SAVE_CONFIG initiates the restart contract",
                 "[expectedrestart]") {
    PIDCalibrationPanel panel;
    panel.set_api(&api);
    helix::ui::PIDCalibrationPanelTestAccess::send_save_config(panel);
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Saving config... Klipper will restart.");
    CHECK(notifications.empty());
}
