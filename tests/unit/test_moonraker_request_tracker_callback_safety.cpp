// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_request_tracker_callback_safety.cpp
 * @brief Regression tests asserting that throwing user callbacks do not unwind
 *        out of MoonrakerRequestTracker::route_response().
 *
 * Pre-#931 the success callback was wrapped but the error callback was not. A
 * throwing error_cb would propagate through libhv's onmessage handler, out of
 * Application::main_loop(), and exit the process with code 134 — appearing to
 * the watchdog as a crash and triggering the recovery dialog after
 * CRASH_LOOP_MAX_CRASHES boots.
 *
 * Both callbacks are now wrapped: any exception is logged and swallowed.
 */

#include "moonraker_request.h"
#include "moonraker_request_tracker.h"

#include "../catch_amalgamated.hpp"

#include <atomic>
#include <stdexcept>

using namespace helix;

class MoonrakerRequestTrackerTestAccess {
  public:
    static void inject_request(MoonrakerRequestTracker& tracker, RequestId id,
                               PendingRequest request) {
        std::lock_guard<std::mutex> lock(tracker.requests_mutex_);
        tracker.pending_requests_[id] = std::move(request);
    }
};

namespace {

/// No-op event sink — these tests don't care about the emitted events,
/// only whether the callback exception escapes route_response().
auto null_emit() {
    return [](MoonrakerEventType, const std::string&, bool, const std::string&) {};
}

PendingRequest make_request_with_throwing_error_cb(std::shared_ptr<std::atomic<int>> entered) {
    PendingRequest req;
    req.id = 0;
    req.method = "test.method";
    req.timeout_ms = 60000;
    req.timestamp = std::chrono::steady_clock::now();
    req.error_callback = [entered](const MoonrakerError&) {
        entered->fetch_add(1);
        throw std::runtime_error("test exception from error_callback");
    };
    return req;
}

PendingRequest make_request_with_throwing_success_cb(std::shared_ptr<std::atomic<int>> entered) {
    PendingRequest req;
    req.id = 0;
    req.method = "test.method";
    req.timeout_ms = 60000;
    req.timestamp = std::chrono::steady_clock::now();
    req.success_callback = [entered](const json&) {
        entered->fetch_add(1);
        throw std::runtime_error("test exception from success_callback");
    };
    return req;
}

} // namespace

TEST_CASE("route_response absorbs exceptions thrown from error_callback",
          "[moonraker][tracker][regression][crash-safety]") {
    MoonrakerRequestTracker tracker;
    auto entered = std::make_shared<std::atomic<int>>(0);

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, /*id=*/101, make_request_with_throwing_error_cb(entered));

    // Server-side RPC error response — triggers error_callback path.
    json error_response = {
        {"jsonrpc", "2.0"},
        {"id", 101},
        {"error", {{"code", -32600}, {"message", "test failure"}}},
    };

    REQUIRE_NOTHROW(tracker.route_response(error_response, null_emit()));
    REQUIRE(entered->load() == 1);
}

TEST_CASE("route_response absorbs exceptions thrown from success_callback",
          "[moonraker][tracker][regression][crash-safety]") {
    MoonrakerRequestTracker tracker;
    auto entered = std::make_shared<std::atomic<int>>(0);

    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, /*id=*/202, make_request_with_throwing_success_cb(entered));

    json success_response = {
        {"jsonrpc", "2.0"},
        {"id", 202},
        {"result", json::object()},
    };

    REQUIRE_NOTHROW(tracker.route_response(success_response, null_emit()));
    REQUIRE(entered->load() == 1);
}
