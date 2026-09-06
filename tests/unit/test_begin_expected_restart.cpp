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
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/pid_calibration_panel_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "http_executor.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "save_config_restart.h"
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
        // The ERROR hook matters as much as the other two here: BedMeshPanel
        // reports through NOTIFY_ERROR while InputShaperPanel calls ToastManager
        // directly. Leaving it uninstalled made a bed-mesh failure invisible to
        // this fixture, which is why the #1359 toast survived a test suite that
        // drove the exact flow that produced it.
        helix::ui::set_test_notification_error_hook(note);
        client.clear_gcode_script_history();
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
    }

    ~ExpectedRestartFixture() override {
        set_moonraker_api(nullptr);
        helix::ui::set_test_toast_hook(nullptr);
        helix::ui::set_test_notification_info_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
        helix::ui::set_test_notification_error_hook(nullptr);
        EmergencyStopOverlayTestAccess::reset_suppression(EmergencyStopOverlay::instance());
        settle();
    }

    /// No panel may surface the rpc failure that SAVE_CONFIG's own restart
    /// causes. Klipper never acks the command, so that error arrives on EVERY
    /// save including the ones that worked; a panel that renders it is telling
    /// the user a successful save failed (prestonbrown/helixscreen#1359).
    bool no_failure_reported() const {
        const auto is_failure = [](const std::string& m) {
            return m == "Failed to save configuration";
        };
        return std::none_of(toasts.begin(), toasts.end(),
                            [&](const Shown& t) { return is_failure(t.message); }) &&
               std::none_of(notifications.begin(), notifications.end(), is_failure);
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

TEST_CASE_METHOD(ExpectedRestartFixture,
                 "the recovery window outlasts a real config-write restart",
                 "[recovery][expectedrestart]") {
    // A K2 Plus takes ~17s from SAVE_CONFIG to klippy READY. A window that
    // expires first puts a "Printer Shutdown" dialog on screen in the middle of
    // a Save that succeeded, then dismisses it a couple of seconds later when
    // klippy reports ready.
    //
    // lv_tick_inc() rather than process_lvgl(): is_recovery_suppressed() only
    // reads the tick, so no timer needs to come due and 17s of virtual time
    // costs nothing here.
    helix::ui::begin_expected_klippy_restart("Saving config... Klipper will restart.");
    settle();
    REQUIRE(EmergencyStopOverlay::instance().is_recovery_suppressed());

    lv_tick_inc(17000);
    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());

    // ...and still lets go, so a host that goes down and stays down is reported.
    lv_tick_inc(4000);
    CHECK_FALSE(EmergencyStopOverlay::instance().is_recovery_suppressed());
}

TEST_CASE_METHOD(ExpectedRestartFixture, "z-offset apply-and-save initiates the restart contract",
                 "[expectedrestart][zoffset][1359]") {
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::apply_and_save(
        &api, save_watch, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE, [] {},
        [](const std::string&) {});
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 2);
    CHECK(hist[0] == "Z_OFFSET_APPLY_PROBE");
    CHECK(hist[1] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    CHECK(no_failure_reported()); // the dropped rpc must never reach the user (#1359)
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Saving config... Klipper will restart.");
    CHECK(notifications.empty());
}

TEST_CASE_METHOD(ExpectedRestartFixture,
                 "z-offset apply-and-save survives the rpc its own restart drops",
                 "[expectedrestart][zoffset][1359]") {
    // The mock fails SAVE_CONFIG's rpc by design: Klipper drops the reply when
    // it restarts. Reporting that as a failure is #1359. Treating it as success
    // before Klipper is back is the opposite bug - nothing is known yet - and
    // here it also skipped whatever the caller does on a real save.
    //
    // SaveConfigWatch observes the process-wide PrinterState, not this fixture's
    // local one, so that one has to exist. init_subjects() is idempotent.
    get_printer_state().init_subjects(false);
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);

    bool saved = false;
    std::string error;
    helix::ui::SaveConfigWatch save_watch;
    helix::zoffset::apply_and_save(
        &api, save_watch, helix::ZOffsetCalibrationStrategy::PROBE_CALIBRATE, [&] { saved = true; },
        [&](const std::string& m) { error = m; });
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 2);
    CHECK(hist[0] == "Z_OFFSET_APPLY_PROBE");
    CHECK(hist[1] == "SAVE_CONFIG");
    CHECK(no_failure_reported());
    CHECK(error.empty());
    CHECK_FALSE(saved); // nothing is known until Klipper is back

    // Klipper comes back. THAT is what says the save worked.
    get_printer_state().set_klippy_state_sync(helix::KlippyState::STARTUP);
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    settle();

    CHECK(error.empty());
    CHECK(saved);
}

TEST_CASE_METHOD(ExpectedRestartFixture, "bed-mesh SAVE_CONFIG initiates the restart contract",
                 "[expectedrestart][bedmesh][1359]") {
    BedMeshPanel panel;
    helix::ui::BedMeshPanelTestAccess::save_config(panel);
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    CHECK(no_failure_reported()); // the dropped rpc must never reach the user (#1359)
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Configuration saved - restarting");
    // The acceptance-time NOTIFY_INFO is gone: the initiation toast plus the
    // READY completion carry the information now, and neither writes a
    // history row.
    CHECK(notifications.empty());
}

TEST_CASE_METHOD(ExpectedRestartFixture, "input-shaper SAVE_CONFIG initiates the restart contract",
                 "[expectedrestart][1359]") {
    InputShaperPanel panel;
    panel.set_api(&client, &api);
    // The merged panel refuses to SAVE_CONFIG with nothing selected (the
    // chip-selection feature on devel), so seed one calibrated axis or the
    // save returns early and no restart is initiated. Connected mock plus a
    // short monitor timeout: the health monitor treats "still connected" as
    // a fast restart and finishes instead of waiting out the 30s default
    // while holding references into this stack frame.
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);
    ::InputShaperPanelTestAccess::set_save_restart_timeout_ms(panel, 1200);
    InputShaperResult seeded;
    seeded.axis = 'X';
    seeded.shaper_type = "mzv";
    seeded.shaper_freq = 45.0f;
    ::InputShaperPanelTestAccess::seed_axis(panel, 'X', seeded, {{"MZV", 45.0f, {}}}, 0);
    api.set_config_files({
        {"printer.cfg", "[include conf.d/*.cfg]\n[printer]\nkinematics: corexy\n"},
        {"conf.d/options.cfg",
         "[input_shaper]\nshaper_freq_x: 47.4\nshaper_type_x: mzv\nshaper_freq_y: 35.0\n"},
    });
    helix::ui::InputShaperPanelTestAccess::save_configuration(panel);
    // The save chain runs on HttpExecutor::fast(); join it before reading the
    // script history, then drain the queue hops (the toast).
    wait_until([] { return helix::http::HttpExecutor::fast().inflight() == 0; }, 5000);
    settle();

    // The merged save chain (chip-selection flow) uploads the edited config
    // through the file API rather than scripting SAVE_CONFIG, so the script
    // history's equivalent is the uploaded options.cfg.
    const auto uploaded = api.get_uploaded_config("conf.d/options.cfg");
    REQUIRE(uploaded.has_value());
    // The seeded X frequency was 47.4; the seeded selection says 45, so the
    // uploaded file proves the save chain ran and wrote the chip's pick.
    CHECK(uploaded->find("shaper_freq_x: 45") != std::string::npos);
    CHECK(uploaded->find("shaper_freq_x: 47.4") == std::string::npos);

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);

    // SAVE_CONFIG's rpc is ALWAYS dropped by the restart it triggers, so the
    // mock fails it by design. That must not reach the user: reporting it cost
    // every successful save a red "Failed to save configuration"
    // (prestonbrown/helixscreen#1359). This assertion is the regression -- it
    // was previously a find_if that hunted for the initiation toast AMONG the
    // failure toast, which tolerated exactly the bug.
    CHECK(no_failure_reported());

    // The initiation toast must be INFO - not the WARNING this flow used before
    // the helper; an expected restart is not a fault, and it pairs with the
    // SUCCESS completion toast.
    //
    // Only the FIRST toast is contract. Whether the completion toast lands
    // inside this test's wait depends on how fast the health monitor sees the
    // mock back, so a trailing SUCCESS is allowed rather than counted; the
    // #1359 regression is held by no_failure_reported() above.
    REQUIRE_FALSE(toasts.empty());
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Saving config... Klipper will restart.");
    for (size_t i = 1; i < toasts.size(); ++i) {
        INFO("trailing toast: " << toasts[i].message);
        CHECK(toasts[i].severity == ToastSeverity::SUCCESS);
    }
    CHECK(notifications.empty());
}

TEST_CASE_METHOD(ExpectedRestartFixture, "PID SAVE_CONFIG initiates the restart contract",
                 "[expectedrestart][1359]") {
    PIDCalibrationPanel panel;
    panel.set_api(&api);
    helix::ui::PIDCalibrationPanelTestAccess::send_save_config(panel);
    settle();

    const auto& hist = client.gcode_script_history();
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == "SAVE_CONFIG");

    CHECK(EmergencyStopOverlay::instance().is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() == 1);
    CHECK(no_failure_reported()); // the dropped rpc must never reach the user (#1359)
    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0].severity == ToastSeverity::INFO);
    CHECK(toasts[0].message == "Saving config... Klipper will restart.");
    CHECK(notifications.empty());
}
