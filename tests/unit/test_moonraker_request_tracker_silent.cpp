// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_request_tracker_silent.cpp
 * @brief Regression tests for MoonrakerRequestTracker::check_timeouts() silent-mode behavior.
 *
 * Context: exclude_object and other long-running RPCs opt into `silent=true` so the
 * global REQUEST_TIMEOUT event (which surfaces a user-facing toast) is suppressed when
 * the request eventually times out. The error callback still fires — the caller handles
 * its own error UX. Non-silent requests must retain the existing toast behavior.
 */

#include "moonraker_request.h"
#include "moonraker_request_tracker.h"
#include "rpc_error_correlation.h"

#include <atomic>
#include <thread>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

/// Friend-class test accessor (L065 / test_code_lint.bats): keeps production
/// headers free of `_for_testing` methods.
class MoonrakerRequestTrackerTestAccess {
  public:
    static void inject_request(MoonrakerRequestTracker& tracker, RequestId id,
                               PendingRequest request) {
        std::lock_guard<std::mutex> lock(tracker.requests_mutex_);
        tracker.pending_requests_[id] = std::move(request);
    }
};

namespace {

/// Capture harness passed as the emit_event lambda — records any events emitted
/// during check_timeouts() so tests can assert on what (if anything) was surfaced.
struct EventCapture {
    struct Record {
        MoonrakerEventType type;
        std::string message;
        bool is_error;
        std::string details;
    };
    std::vector<Record> records;

    auto as_lambda() {
        return [this](MoonrakerEventType t, const std::string& m, bool e, const std::string& d) {
            records.push_back({t, m, e, d});
        };
    }
};

/// Build a PendingRequest that is already timed out (timestamp in the past).
/// @param method RPC method name for the fake request
/// @param silent whether the request opted into silent mode
/// @param error_cb_fired out-param atomic set to true when the error callback is invoked
PendingRequest make_timed_out_request(const std::string& method, bool silent,
                                      std::shared_ptr<std::atomic<bool>> error_cb_fired) {
    PendingRequest req;
    req.id = 0; // caller supplies the map key
    req.method = method;
    req.timeout_ms = 1; // 1ms so is_timed_out() returns true immediately
    // Timeout suppression keys off intent.silent alone; surfaces_errors is
    // irrelevant to check_timeouts() and stays at its default.
    req.intent.silent = silent;
    // Timestamp in the past — is_timed_out() compares elapsed vs timeout_ms.
    req.timestamp = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    req.error_callback = [error_cb_fired](const MoonrakerError&) { error_cb_fired->store(true); };
    return req;
}

} // namespace

TEST_CASE("check_timeouts suppresses REQUEST_TIMEOUT event for silent requests",
          "[moonraker][tracker][regression]") {
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto err_fired = std::make_shared<std::atomic<bool>>(false);

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker,
        /*id=*/42, make_timed_out_request("printer.gcode.script", /*silent=*/true, err_fired));

    tracker.check_timeouts(capture.as_lambda());

    SECTION("no REQUEST_TIMEOUT event reaches the UI layer") {
        for (const auto& r : capture.records) {
            REQUIRE(r.type != MoonrakerEventType::REQUEST_TIMEOUT);
        }
        // Stronger: in practice nothing at all should be emitted for a silent timeout.
        REQUIRE(capture.records.empty());
    }

    SECTION("error callback still fires so the caller can react") {
        REQUIRE(err_fired->load() == true);
    }
}

TEST_CASE("check_timeouts still emits REQUEST_TIMEOUT for non-silent requests",
          "[moonraker][tracker][regression]") {
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto err_fired = std::make_shared<std::atomic<bool>>(false);

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker,
        /*id=*/7, make_timed_out_request("printer.objects.query", /*silent=*/false, err_fired));

    tracker.check_timeouts(capture.as_lambda());

    SECTION("REQUEST_TIMEOUT event is surfaced to the UI layer") {
        bool saw_timeout_event = false;
        for (const auto& r : capture.records) {
            if (r.type == MoonrakerEventType::REQUEST_TIMEOUT) {
                saw_timeout_event = true;
                REQUIRE(r.details == "printer.objects.query");
            }
        }
        REQUIRE(saw_timeout_event);
    }

    SECTION("error callback also fires") {
        REQUIRE(err_fired->load() == true);
    }
}

// ============================================================================
// route_response() error dedup: a request whose caller declared it surfaces the
// error itself is handling its own error UI. It must NOT also emit the generic
// "Printer command failed" toast, and it must record correlation so the `!!`
// broadcast for the same error dedups. Without this, a single Klipper rejection
// produced THREE stacked toasts (K2 chamber, #key69).
//
// `silent` alone is a weaker claim and must NOT record: it means "no automatic
// toast from us", not "the user has been told".
// ============================================================================

namespace {

/// @param method also decides whether Klipper mirrors the failure as a `!!` line
///        (printer.gcode.script does; nothing else does), which the policy weighs
///        alongside @p intent — see helix::rpc_error_policy::RequestFacts.
/// @param intent what the caller declared about who reports the error.
/// @param with_error_cb whether an error callback is actually installed. Kept
///        separate from @p intent so a test can pin that the DECLARED intent, not
///        the presence of a callback, is what drives the policy.
PendingRequest make_error_request(const std::string& method,
                                  helix::rpc_error_policy::CallerIntent intent, bool with_error_cb,
                                  std::shared_ptr<std::atomic<bool>> error_cb_fired) {
    PendingRequest req;
    req.method = method;
    req.intent = intent;
    req.timeout_ms = 100000;
    req.timestamp = std::chrono::steady_clock::now();
    if (with_error_cb) {
        req.error_callback = [error_cb_fired](const MoonrakerError&) {
            error_cb_fired->store(true);
        };
    }
    return req;
}

nlohmann::json make_error_response(uint64_t id, const std::string& message) {
    return nlohmann::json{{"id", id}, {"error", {{"code", -32000}, {"message", message}}}};
}

} // namespace

TEST_CASE("route_response suppresses generic RPC_ERROR toast when the caller surfaces errors",
          "[moonraker][tracker][dedup]") {
    helix::rpc_error_correlation::clear_for_test();
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto err_fired = std::make_shared<std::atomic<bool>>(false);
    const std::string msg = "The value 'chamber' is not valid for HEATER";

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, /*id=*/100,
        make_error_request("printer.gcode.script",
                           helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                                 /*surfaces_errors=*/true},
                           /*with_error_cb=*/true, err_fired));

    tracker.route_response(make_error_response(100, msg), capture.as_lambda(), nullptr);

    SECTION("no generic RPC_ERROR event is emitted — the caller owns the UI") {
        for (const auto& r : capture.records) {
            REQUIRE(r.type != MoonrakerEventType::RPC_ERROR);
        }
    }
    SECTION("the caller's error_cb still fires") {
        REQUIRE(err_fired->load() == true);
    }
    SECTION("correlation is recorded so the !! broadcast dedups") {
        REQUIRE(helix::rpc_error_correlation::was_recently_handled(msg));
    }
}

TEST_CASE("route_response leaves the `!!` router free for a merely-silent caller",
          "[moonraker][tracker][dedup]") {
    // AmsSubscriptionBackend's macro sends are silent and their error_cb only
    // writes to the log. Recording correlation here would suppress the `!!`
    // broadcast, which is the ONLY surface that would have told the user an AFC
    // / Happy Hare / CFS macro failed.
    helix::rpc_error_correlation::clear_for_test();
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto err_fired = std::make_shared<std::atomic<bool>>(false);
    const std::string msg = "AFC_UNLOAD: lane not loaded";

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, /*id=*/102,
        make_error_request("printer.gcode.script",
                           helix::rpc_error_policy::CallerIntent{/*silent=*/true,
                                                                 /*surfaces_errors=*/false},
                           /*with_error_cb=*/true, err_fired));

    tracker.route_response(make_error_response(102, msg), capture.as_lambda(), nullptr);

    SECTION("the log-only error_cb still fires") {
        REQUIRE(err_fired->load() == true);
    }
    SECTION("no generic RPC_ERROR toast — the caller opted out of automatic error UI") {
        for (const auto& r : capture.records) {
            REQUIRE(r.type != MoonrakerEventType::RPC_ERROR);
        }
    }
    SECTION("nothing is recorded, so the `!!` broadcast still reaches the user") {
        REQUIRE_FALSE(helix::rpc_error_correlation::was_recently_handled(msg));
    }
}

TEST_CASE("route_response still emits RPC_ERROR when nobody claims a plain RPC error",
          "[moonraker][tracker][dedup]") {
    // A non-gcode method has no `!!` broadcast behind it, so the tracker's
    // generic fallback is the user's only possible signal.
    helix::rpc_error_correlation::clear_for_test();
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto unused = std::make_shared<std::atomic<bool>>(false);

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, /*id=*/101,
        make_error_request("printer.objects.query",
                           helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                                 /*surfaces_errors=*/false},
                           /*with_error_cb=*/false, unused));

    tracker.route_response(make_error_response(101, "Some failure"), capture.as_lambda(), nullptr);

    bool saw_rpc_error = false;
    for (const auto& r : capture.records) {
        if (r.type == MoonrakerEventType::RPC_ERROR) {
            saw_rpc_error = true;
        }
    }
    REQUIRE(saw_rpc_error); // unhandled error must still surface a fallback toast
    // The fallback IS the report, so the correlation window records it — that is
    // what keeps a same-text `!!` line from stacking a second toast on top.
    REQUIRE(helix::rpc_error_correlation::was_recently_handled("Some failure"));
}

TEST_CASE("route_response leaves an unclaimed gcode error to the `!!` router",
          "[moonraker][tracker][dedup]") {
    // The dominant macro-send shape: on_error == nullptr on printer.gcode.script.
    // Klipper mirrors the rejection as `!!`, and GcodeErrorRouter is a strictly
    // richer surface than "Printer command '...' failed" — it classifies
    // severity and can escalate to a recovery modal. The generic fallback must
    // stay out of its way, and must not record (which would mute the router).
    helix::rpc_error_correlation::clear_for_test();
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto unused = std::make_shared<std::atomic<bool>>(false);
    const std::string msg = "Must home axis first";

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, /*id=*/103,
        make_error_request("printer.gcode.script",
                           helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                                 /*surfaces_errors=*/false},
                           /*with_error_cb=*/false, unused));

    tracker.route_response(make_error_response(103, msg), capture.as_lambda(), nullptr);

    for (const auto& r : capture.records) {
        REQUIRE(r.type != MoonrakerEventType::RPC_ERROR);
    }
    REQUIRE_FALSE(helix::rpc_error_correlation::was_recently_handled(msg));
}

TEST_CASE("check_timeouts handles a mix of silent and non-silent timeouts in one sweep",
          "[moonraker][tracker]") {
    MoonrakerRequestTracker tracker;
    EventCapture capture;
    auto silent_fired = std::make_shared<std::atomic<bool>>(false);
    auto loud_fired = std::make_shared<std::atomic<bool>>(false);

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker,
        /*id=*/1, make_timed_out_request("silent.op", /*silent=*/true, silent_fired));
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker,
        /*id=*/2, make_timed_out_request("loud.op", /*silent=*/false, loud_fired));

    tracker.check_timeouts(capture.as_lambda());

    // Both error callbacks fire
    REQUIRE(silent_fired->load() == true);
    REQUIRE(loud_fired->load() == true);

    // Exactly one REQUEST_TIMEOUT event — from the loud one only
    int timeout_events = 0;
    for (const auto& r : capture.records) {
        if (r.type == MoonrakerEventType::REQUEST_TIMEOUT) {
            ++timeout_events;
            REQUIRE(r.details == "loud.op");
        }
    }
    REQUIRE(timeout_events == 1);
}
