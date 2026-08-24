// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_rpc_error_correlation_normalization.cpp
 * @brief Cross-channel dedup must compare both sides in the SAME normalization.
 *
 * One Klipper rejection reaches the UI on two transport-distinct channels:
 *
 *   (a) the JSON-RPC error response to printer.gcode.script -> routed by
 *       MoonrakerRequestTracker, which calls record_caller_handled() with the
 *       Klipper-supplied `error.message` when the caller owns the error UI;
 *   (b) the broadcast `!! <text>` line on the gcode-response stream -> routed
 *       by GcodeErrorRouter, which asks was_recently_handled() before toasting.
 *
 * Channel (b) runs its text through GcodeErrorRouter::clean_error_text(), which
 * REWRITES some messages ("Must home axis first" -> "Must home axes first").
 * Channel (a) records the raw text. The dedup match is exact-string by design
 * (see include/rpc_error_correlation.h -- substring would mask unrelated errors
 * sharing a phrase), so any message the cleaner rewrites can never match its RPC
 * twin, and the user sees TWO toasts for one rejection on real hardware.
 *
 * These tests drive the REAL tracker and the REAL router -- not the mock's
 * inline gcode_script() dispatch -- so they exercise the production comparison.
 *
 * ui_notification_error() is a logging-only stub in the test binary (see
 * tests/ui_test_utils.cpp), so the router's toast is observed through a spdlog
 * ringbuffer sink.
 */

#include "ui_update_queue.h"

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/printer_state.h"
#include "../../include/rpc_error_correlation.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/gcode_error_router_test_access.h"
#include "gcode_error_router.h"
#include "moonraker_request.h"
#include "moonraker_request_tracker.h"
#include "recovery_modal_presenter.h"

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

/// Friend-class test accessor (L065 / test_code_lint.bats). Definition is kept
/// byte-identical to the one in test_moonraker_request_tracker_silent.cpp --
/// the friend declaration in moonraker_request_tracker.h names this exact
/// global-scope class, so it cannot live in an anonymous namespace.
class MoonrakerRequestTrackerTestAccess {
  public:
    static void inject_request(MoonrakerRequestTracker& tracker, RequestId id,
                               PendingRequest request) {
        std::lock_guard<std::mutex> lock(tracker.requests_mutex_);
        tracker.pending_requests_[id] = std::move(request);
    }
};

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

/// A PendingRequest shaped like a jog: the caller raises its own error toast, so
/// the tracker treats it as caller-handles-UI and records the correlation.
PendingRequest make_caller_handled_request(std::shared_ptr<std::atomic<bool>> error_cb_fired) {
    PendingRequest req;
    req.id = 0; // caller supplies the map key
    req.method = "printer.gcode.script";
    req.timeout_ms = 5000;
    // A jog error handler renders a real toast, so it owns the report and the
    // `!!` copy of the same rejection must dedup against it.
    req.intent = helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                       /*surfaces_errors=*/true};
    req.timestamp = std::chrono::steady_clock::now();
    req.error_callback = [error_cb_fired](const MoonrakerError&) { error_cb_fired->store(true); };
    return req;
}

nlohmann::json make_error_response(uint64_t id, const std::string& message) {
    return nlohmann::json{{"id", id}, {"error", {{"code", -32000}, {"message", message}}}};
}

struct ChannelResult {
    int router_toasts = 0;
    int suppressions = 0;
    bool caller_cb_fired = false;
    /// Generic "Printer command '...' failed" RPC_ERROR events the tracker
    /// emitted — the OTHER user-visible surface. Counted so a test can pin that
    /// exactly one surface speaks per rejection, not zero and not two.
    int generic_toasts = 0;
};

/// Fixture wrapper: run_both_channels() needs LVGLTestFixture::process_lvgl()
/// to pump present_deferred_toast()'s 150ms re-check timer.
class CorrelationFixture : public LVGLTestFixture {
  protected:
    /// Drives one Klipper rejection through BOTH production channels in the
    /// order real hardware produces them for a gcode.script rejection, and
    /// reports how many router toasts fired.
    ///
    /// @param klipper_message the exact text Klipper puts in the RPC error AND
    ///        on the `!!` broadcast line -- one rejection, one payload, two
    ///        channels.
    ChannelResult run_both_channels(const std::string& klipper_message) {
        rpc_error_correlation::clear_for_test();

        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        PrinterState state;
        state.init_subjects(false);
        MoonrakerAPI api(mock, state);
        helix::ui::RecoveryModalPresenter presenter(nullptr);
        helix::GcodeErrorRouter router(&api, &mock, presenter);

        ChannelResult out;
        {
            LogCapture log;

            // ---- Channel (a): the REAL request tracker routes the RPC error ----
            MoonrakerRequestTracker tracker;
            auto err_fired = std::make_shared<std::atomic<bool>>(false);
            MoonrakerRequestTrackerTestAccess::inject_request(
                tracker, /*id=*/700, make_caller_handled_request(err_fired));
            tracker.route_response(
                make_error_response(700, klipper_message),
                [](MoonrakerEventType, const std::string&, bool, const std::string&) {}, nullptr);
            out.caller_cb_fired = err_fired->load();

            // ---- Channel (b): the REAL router handles the `!!` broadcast ----
            GcodeErrorRouterTestAccess::process_line(router, "!! " + klipper_message);

            // Run past present_deferred_toast()'s 150ms re-check window.
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(400);
            helix::ui::UpdateQueue::instance().drain();

            out.router_toasts = log.count_containing("ui_notification_error: Klipper Error");
            out.suppressions = log.count_containing("Suppressing deferred `!!` toast") +
                               log.count_containing("Suppressing duplicate (RPC-handled)");
        }

        mock.stop_temperature_simulation();
        rpc_error_correlation::clear_for_test();
        return out;
    }

    /// Same two channels, but with a caller-declared intent supplied by the test
    /// and BOTH user-visible surfaces counted. Used to pin the core invariant:
    /// one Klipper rejection produces exactly one report, never zero, never two.
    ChannelResult run_both_channels_with_intent(const std::string& klipper_message,
                                                helix::rpc_error_policy::CallerIntent intent,
                                                bool with_error_cb) {
        rpc_error_correlation::clear_for_test();

        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        PrinterState state;
        state.init_subjects(false);
        MoonrakerAPI api(mock, state);
        helix::ui::RecoveryModalPresenter presenter(nullptr);
        helix::GcodeErrorRouter router(&api, &mock, presenter);

        ChannelResult out;
        {
            LogCapture log;

            MoonrakerRequestTracker tracker;
            PendingRequest req;
            req.id = 0;
            req.method = "printer.gcode.script";
            req.timeout_ms = 5000;
            req.intent = intent;
            req.timestamp = std::chrono::steady_clock::now();
            auto err_fired = std::make_shared<std::atomic<bool>>(false);
            if (with_error_cb) {
                req.error_callback = [err_fired](const MoonrakerError&) { err_fired->store(true); };
            }
            MoonrakerRequestTrackerTestAccess::inject_request(tracker, /*id=*/701, std::move(req));

            int generic = 0;
            tracker.route_response(
                make_error_response(701, klipper_message),
                [&generic](MoonrakerEventType t, const std::string&, bool, const std::string&) {
                    if (t == MoonrakerEventType::RPC_ERROR) {
                        ++generic;
                    }
                },
                nullptr);
            out.caller_cb_fired = err_fired->load();
            out.generic_toasts = generic;

            GcodeErrorRouterTestAccess::process_line(router, "!! " + klipper_message);
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(400);
            helix::ui::UpdateQueue::instance().drain();

            out.router_toasts = log.count_containing("ui_notification_error: Klipper Error");
            out.suppressions = log.count_containing("Suppressing deferred `!!` toast") +
                               log.count_containing("Suppressing duplicate (RPC-handled)");
        }

        mock.stop_temperature_simulation();
        rpc_error_correlation::clear_for_test();
        return out;
    }
};

} // namespace

TEST_CASE_METHOD(CorrelationFixture,
                 "cross-channel dedup survives clean_error_text rewriting the message",
                 "[moonraker][tracker][dedup][errors]") {
    // clean_error_text() rewrites this one: "Must home axis first" (Klipper's
    // wording, sent identically on both channels) becomes "Must home axes
    // first" on the router side only. The RPC side records Klipper's wording,
    // so the exact-string dedup misses and BOTH toasts fire.
    const std::string REWRITTEN = "Must home axis first";

    auto r = run_both_channels(REWRITTEN);

    INFO("router toasts: " << r.router_toasts << " suppressions: " << r.suppressions);
    CHECK(r.caller_cb_fired);
    // The caller's error_cb owns the UI for this rejection. The router must not
    // render a second toast on top of it.
    CHECK(r.router_toasts == 0);
    CHECK(r.suppressions >= 1);
}

TEST_CASE_METHOD(CorrelationFixture, "cross-channel dedup still works for un-rewritten messages",
                 "[moonraker][tracker][dedup][errors]") {
    // Regression guard: clean_error_text() leaves this text alone, so the two
    // channels already match today. The normalization fix must not break it.
    auto r = run_both_channels("Move out of range: X=400.000000");

    INFO("router toasts: " << r.router_toasts << " suppressions: " << r.suppressions);
    CHECK(r.caller_cb_fired);
    CHECK(r.router_toasts == 0);
    CHECK(r.suppressions >= 1);
}

TEST_CASE_METHOD(CorrelationFixture, "cross-channel dedup is unaffected by toast-length truncation",
                 "[moonraker][tracker][dedup][errors]") {
    // truncate_for_toast() clips at 80 chars, but it runs at PRESENTATION time
    // on a copy -- the compared text (ErrorEvent::detail / DeferredCtx::clean)
    // is never truncated. This >80-char message must therefore dedup on the
    // untruncated text. Pins that truncation stays out of the compared path.
    const std::string LONG = "Move out of range: X=400.000000 Y=400.000000 Z=400.000000 exceeds "
                             "the configured axis maximum for this printer";
    REQUIRE(LONG.size() > 80);

    auto r = run_both_channels(LONG);

    INFO("router toasts: " << r.router_toasts << " suppressions: " << r.suppressions);
    CHECK(r.caller_cb_fired);
    CHECK(r.router_toasts == 0);
    CHECK(r.suppressions >= 1);
}

// ============================================================================
// The invariant, stated directly: ONE Klipper rejection produces exactly ONE
// user-visible report. Zero is the silent-failure bug; two is the key69 bug.
// Both surfaces are counted here, so neither can regress unnoticed.
// ============================================================================

TEST_CASE_METHOD(CorrelationFixture, "a gcode rejection nobody claims reports exactly once",
                 "[moonraker][tracker][dedup][errors]") {
    // The dominant macro-send shape: on_error == nullptr, not silent. Klipper
    // broadcasts `!!` for the same rejection, so the router is a strictly better
    // surface than the generic "Printer command '...' failed" fallback — but if
    // BOTH speak the user gets two toasts for one failure.
    auto r = run_both_channels_with_intent(
        "Must home axis first",
        helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                              /*surfaces_errors=*/false},
        /*with_error_cb=*/false);

    INFO("generic: " << r.generic_toasts << " router: " << r.router_toasts);
    CHECK(r.generic_toasts + r.router_toasts == 1);
}

TEST_CASE_METHOD(CorrelationFixture, "a silent log-only caller still reports exactly once",
                 "[moonraker][tracker][dedup][errors]") {
    // AmsSubscriptionBackend's shape: silent, error_cb logs only. The generic
    // fallback is opted out, so the `!!` router must be the one that speaks.
    auto r =
        run_both_channels_with_intent("AFC_UNLOAD: lane not loaded",
                                      helix::rpc_error_policy::CallerIntent{/*silent=*/true,
                                                                            /*surfaces_errors=*/
                                                                            false},
                                      /*with_error_cb=*/true);

    INFO("generic: " << r.generic_toasts << " router: " << r.router_toasts);
    CHECK(r.generic_toasts == 0);
    CHECK(r.router_toasts == 1);
}

TEST_CASE_METHOD(CorrelationFixture, "a caller that surfaces errors reports exactly once",
                 "[moonraker][tracker][dedup][errors]") {
    // The caller's own contextual toast is the single report; both the generic
    // fallback and the `!!` copy must stay quiet (key69).
    auto r = run_both_channels_with_intent(
        "The value 'chamber' is not valid for HEATER",
        helix::rpc_error_policy::CallerIntent{/*silent=*/false, /*surfaces_errors=*/true},
        /*with_error_cb=*/true);

    INFO("generic: " << r.generic_toasts << " router: " << r.router_toasts);
    CHECK(r.generic_toasts == 0);
    CHECK(r.router_toasts == 0);
    CHECK(r.caller_cb_fired);
}
