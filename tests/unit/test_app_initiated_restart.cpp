// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A Klipper restart the app itself sends must read as a restart, never as a
// fault: no "Printer Firmware Disconnected" recovery dialog, no disconnect
// modal. KlipperConfigEditor's safe edits (every Save to Config that writes a
// section and restarts) are the shared path, so the editor arms that window.

#include "ui_emergency_stop.h"
#include "ui_toast_manager.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/emergency_stop_test_access.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../ui_test_utils.h"
#include "core/http_executor.h"
#include "core/update_queue.h"
#include "klipper_config_editor.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::system;

TEST_CASE_METHOD(LVGLTestFixture, "a config editor safe edit marks its restart as expected",
                 "[pa_tuner][expected_restart]") {
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
    REQUIRE_FALSE(estop.is_expected_restart());

    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({
        {"printer.cfg", "[printer]\nkinematics: corexy\n[extruder]\npressure_advance: 0.04\n"},
    });
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);
    client.set_simulation_speedup(1000.0);

    ToastManager::instance().init();
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook([&toasts](ToastSeverity, const std::string& message, uint32_t) {
        toasts.push_back(message);
    });
    // The hook captures a local: clear it on every exit, a failed REQUIRE included.
    struct HookReset {
        ~HookReset() {
            helix::ui::set_test_toast_hook(nullptr);
        }
    } hook_reset;

    KlipperConfigEditor editor;
    editor.set_restart_poll_interval(std::chrono::milliseconds(1));
    // The caller's own words announce the restart, not a generic line.
    editor.set_restart_announcement("Saving the extruder section... Klipper will restart.");
    bool succeeded = false;
    std::string error;
    editor.safe_multi_edit(
        api, "extruder", {{ConfigEdit::Type::ADD_KEY, "pressure_advance_model", "tanh"}},
        [&]() { succeeded = true; }, [&](const std::string& err) { error = err; },
        /*restart_timeout_ms=*/300);
    helix::ui::UpdateQueue::instance().drain_until_empty();

    // The monitor holds `api` and `editor` by reference: join it before return.
    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 15000));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(error.empty());
    REQUIRE(succeeded);

    // The restart went out under the expected-restart window: neither the
    // recovery dialog nor the disconnect modal reads it as a fault.
    CHECK(estop.is_recovery_suppressed());
    CHECK(api.suppress_disconnect_modal_calls() >= 1);
    process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(std::find(toasts.begin(), toasts.end(),
                    "Saving the extruder section... Klipper will restart.") != toasts.end());

    process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "an expected-restart announcement keeps a longer window already pending",
                 "[pa_tuner][expected_restart]") {
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_suppression(estop);

    // A flow's own longer window (PID migration) followed by the editor's
    // generic announcement of the restart that flow triggers.
    estop.suppress_recovery_dialog(helix::recovery_suppression::EXTRA);
    helix::ui::begin_expected_klippy_restart("Firmware restarting...", nullptr);

    lv_tick_inc(helix::recovery_suppression::LONG + 2000);
    CHECK(estop.is_recovery_suppressed());

    lv_tick_inc(helix::recovery_suppression::EXTRA - helix::recovery_suppression::LONG);
    CHECK_FALSE(estop.is_recovery_suppressed());

    helix::ui::UpdateQueue::instance().drain();
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
}

TEST_CASE_METHOD(LVGLTestFixture, "a config editor's revert restart is announced as expected too",
                 "[pa_tuner][expected_restart][pa_mutation]") {
    auto& estop = EmergencyStopOverlay::instance();
    EmergencyStopOverlayTestAccess::reset_suppression(estop);

    helix::PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);
    api.set_config_files({
        {"printer.cfg", "[printer]\nkinematics: corexy\n[extruder]\npressure_advance: 0.04\n"},
    });
    helix::MoonrakerClientTestAccess::force_connection_state(client,
                                                             helix::ConnectionState::CONNECTED);
    client.set_simulation_speedup(1000.0);
    // Klippy refuses the edited config, so the editor restores the backup and
    // restarts a second time.
    client.set_restart_outcome(MoonrakerClientMock::KlippyState::ERROR);

    KlipperConfigEditor editor;
    editor.set_restart_poll_interval(std::chrono::milliseconds(1));
    bool succeeded = false;
    std::string error;
    editor.safe_multi_edit(
        api, "extruder", {{ConfigEdit::Type::ADD_KEY, "pressure_advance_model", "tanh"}},
        [&]() { succeeded = true; }, [&](const std::string& err) { error = err; },
        /*restart_timeout_ms=*/300);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    REQUIRE(wait_until([]() { return helix::http::HttpExecutor::fast().inflight() == 0; }, 15000));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE_FALSE(succeeded);
    REQUIRE_FALSE(error.empty());

    // One announcement per restart: the edit's and the revert's.
    CHECK(api.suppress_disconnect_modal_calls() == 2);

    process_lvgl(1200);
    helix::ui::UpdateQueue::instance().drain();
    EmergencyStopOverlayTestAccess::reset_suppression(estop);
}
