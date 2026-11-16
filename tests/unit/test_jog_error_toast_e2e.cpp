// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// tests/unit/test_jog_error_toast_e2e.cpp

/**
 * @file test_jog_error_toast_e2e.cpp
 * @brief End-to-end lock-in for "one readable toast per rejected jog" in mock.
 *
 * A single Klipper rejection reaches the user through two transport-distinct
 * channels, and the app must render exactly ONE toast:
 *
 *   (a) the JSON-RPC error response to printer.gcode.script -> the caller's
 *       error_cb (MotionPanel's jog handler) surfaces "Jog failed: <reason>";
 *   (b) the broadcast `!! Move out of range: ...` line on the gcode response
 *       stream -> GcodeErrorRouter, which must SUPPRESS its own toast because
 *       (a) already reported the same root cause.
 *
 * Three mock defects broke that in mock runs while real hardware was fine:
 *   1. gcode_script() cleared the error latch per LINE, so a jog's trailing G90
 *      wiped the failing G0's message -> "An unknown error occurred.";
 *   2. the latch stored the `!!`-prefixed text, which the RPC message never
 *      carries -> the two channels could never string-match;
 *   3. MoonrakerClientMock::send_jsonrpc bypasses MoonrakerRequestTracker, so
 *      record_caller_handled() was never called -> no dedup.
 *
 * This test wires the REAL GcodeErrorRouter to the mock client (the ctor
 * self-registers a notify_gcode_response handler), so the `!!` broadcast and
 * the RPC error race through the real code in the real order — the broadcast
 * fires from inside gcode_script(), BEFORE the RPC error_cb records the
 * correlation. present_deferred_toast()'s 150ms re-check is what closes that
 * ordering gap; this test would catch its removal.
 *
 * Error notifications are counted through the error-notification hook
 * (tests/ui_test_utils.h) rather than the widget tree, which is what makes the
 * "how many toasts fired" question answerable here.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/rpc_error_correlation.h"
#include "../../include/ui_error_reporting.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "core/update_queue.h"
#include "gcode_error_router.h"
#include "recovery_modal_presenter.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "mock jog rejection surfaces exactly one readable toast",
                 "[api][movement][errors][mock_fidelity][e2e]") {
    rpc_error_correlation::clear_for_test();

    // The capture swaps the process-wide default logger, which the mock's
    // simulation thread reads without a lock on every log call. Installing it
    // before connect() starts that thread, and keeping it until the thread has
    // been joined below, orders the swap both ways. Exclusive: pushing a sink
    // onto the shared logger instead would race that thread's sink_it_.
    ExclusiveLogCapture log(512);

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(KlippyState::READY);
    lv_subject_set_int(state.print_state().get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::STANDBY));
    helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, false);
    mock.connect("ws://mock/websocket", []() {}, []() {});
    MoonrakerAPI api(mock, state);

    // Park near the kXMax edge (175 on the centre-origin Voron 2.4 persona)
    // so a normal-sized jog overshoots — the exact real-world shape of this bug.
    mock.gcode_script("G28");
    mock.gcode_script("G0 X170");

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    // Real router, real client: the ctor registers a notify_gcode_response
    // handler on the mock, so the `!!` broadcast arrives through production glue.
    helix::GcodeErrorRouter router(&api, &mock, presenter);

    std::vector<std::string> error_notifications;
    helix::ui::set_test_notification_error_hook(
        [&error_notifications](const std::string& m) { error_notifications.push_back(m); });

    std::string rendered_toast;
    {
        // The MotionPanel jog error handler, verbatim (ui_panel_motion.cpp).
        api.motion().move_relative(50.0, 0.0, 0.0, 6000.0, 600.0, nullptr,
                                   [&](const MoonrakerError& err) {
                                       rendered_toast = err.user_message();
                                       NOTIFY_ERROR("Jog failed: {}", rendered_toast);
                                   });

        // Let the router's bg_cb-queued notify reach the main thread, then run
        // past present_deferred_toast()'s 150ms re-check window.
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(400);
        helix::ui::UpdateQueue::instance().drain();

        // ---- (a) The caller's toast is readable ----
        INFO("rendered toast: " << rendered_toast);
        CHECK(rendered_toast.find("out of range") != std::string::npos);
        CHECK(rendered_toast.find("An unknown error occurred") == std::string::npos);
        CHECK(rendered_toast.rfind("!!", 0) != 0);
        CHECK(log.count_containing("[USER] Jog failed: Move out of range") == 1);
        CHECK(log.count_containing("An unknown error occurred") == 0);

        // ---- (b) The router's duplicate is suppressed ----
        // Either suppression arm is correct; which one fires depends on channel
        // ordering. What must NOT happen is the router rendering a second toast.
        const int suppressed = log.count_containing("Suppressing deferred `!!` toast") +
                               log.count_containing("Suppressing duplicate (RPC-handled)");
        INFO("suppression log lines: " << suppressed);
        CHECK(suppressed >= 1);

        // The caller's NOTIFY_ERROR is the only error notification: the router's
        // toast arm, ui_notification_error(lv_tr("Klipper Error"), ...), must
        // never have fired.
        CHECK(error_notifications == std::vector<std::string>{"Jog failed: " + rendered_toast});
    }
    helix::ui::set_test_notification_error_hook(nullptr);

    mock.stop_temperature_simulation();
    mock.disconnect();
    rpc_error_correlation::clear_for_test();
}
