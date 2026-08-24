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
 * ui_notification_error() is a logging-only stub in the test binary (see
 * tests/ui_test_utils.cpp), so toasts are observed via a spdlog ringbuffer
 * sink rather than the widget tree — which is exactly what makes the
 * "how many toasts fired" question answerable here.
 */

#include "ui_update_queue.h"

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/rpc_error_correlation.h"
#include "../../include/ui_error_reporting.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "gcode_error_router.h"
#include "recovery_modal_presenter.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// RAII spdlog capture: collects formatted log lines so the test can count the
/// toasts the user would actually have seen.
class LogCapture {
  public:
    LogCapture() : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(512)) {
        logger_ = spdlog::default_logger();
        prev_level_ = logger_->level();
        sink_->set_level(spdlog::level::trace);
        logger_->sinks().push_back(sink_);
        logger_->set_level(spdlog::level::trace);
    }

    ~LogCapture() {
        auto& sinks = logger_->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == sink_) {
                sinks.erase(it);
                break;
            }
        }
        logger_->set_level(prev_level_);
    }

    std::vector<std::string> lines() const {
        return sink_->last_formatted(512);
    }

    int count_containing(const std::string& needle) const {
        int n = 0;
        for (const auto& l : lines()) {
            if (l.find(needle) != std::string::npos)
                ++n;
        }
        return n;
    }

  private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
    std::shared_ptr<spdlog::logger> logger_;
    spdlog::level::level_enum prev_level_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "mock jog rejection surfaces exactly one readable toast",
                 "[api][movement][errors][mock_fidelity][e2e]") {
    rpc_error_correlation::clear_for_test();

    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    PrinterState state;
    state.init_subjects(false);
    state.set_klippy_state_sync(KlippyState::READY);
    lv_subject_set_int(state.get_print_state_enum_subject(),
                       static_cast<int>(PrintJobState::STANDBY));
    helix::PrinterStateTestAccess::set_sustained_idle_timeout_printing(state, false);
    mock.connect("ws://mock/websocket", []() {}, []() {});
    MoonrakerAPI api(mock, state);

    // Park near the X_MAX edge (350 on a Voron 2.4) so a normal-sized jog
    // overshoots — the exact real-world shape of this bug.
    mock.gcode_script("G28");
    mock.gcode_script("G0 X340");

    helix::ui::RecoveryModalPresenter presenter(nullptr);
    // Real router, real client: the ctor registers a notify_gcode_response
    // handler on the mock, so the `!!` broadcast arrives through production glue.
    helix::GcodeErrorRouter router(&api, &mock, presenter);

    std::string rendered_toast;
    {
        LogCapture log;

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

        // The router's toast arm calls ui_notification_error(lv_tr("Klipper Error"), ...),
        // which the test stub logs. It must never have fired.
        CHECK(log.count_containing("ui_notification_error: Klipper Error") == 0);
    }

    mock.stop_temperature_simulation();
    mock.disconnect();
    rpc_error_correlation::clear_for_test();
}
