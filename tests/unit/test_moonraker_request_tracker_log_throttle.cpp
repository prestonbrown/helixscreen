// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_request_tracker_log_throttle.cpp
 * @brief Regression tests for the periodic pending-count line in check_timeouts().
 *
 * Context: check_timeouts() throttles its pending-count line on signature change
 * (count + oldest method + warn level). That alone does not quiet a queue that is
 * draining normally — every count decrement is a fresh signature, so one healthy
 * poll burst emits a line per in-flight request. On a Spoolman printer this ran to
 * 1275 of the 2000 lines in a debug bundle's ring buffer, capping the bundle's
 * reach at ~3.5 h of near-nothing (bundle 3Q2GB74K).
 *
 * The debug line now additionally requires the oldest request to have aged past
 * PENDING_LOG_MIN_AGE_MS. The warn path (a request stuck past 30 s — the #909
 * leading indicator) is deliberately unaffected.
 */

#include "../test_helpers/log_capture.h"
#include "../test_helpers/moonraker_request_tracker_test_access.h"
#include "moonraker_request.h"
#include "moonraker_request_tracker.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A request that is still in flight (nowhere near its timeout) but has been
/// pending for `age`. Generous timeout so check_timeouts() counts it as pending
/// rather than timing it out.
PendingRequest make_pending_request(const std::string& method, std::chrono::milliseconds age,
                                    uint32_t timeout_ms = 120000) {
    PendingRequest req;
    req.id = 0; // caller supplies the map key
    req.method = method;
    req.timeout_ms = timeout_ms;
    req.intent.silent = false;
    req.timestamp = std::chrono::steady_clock::now() - age;
    return req;
}

/// check_timeouts() takes an emit_event sink; these tests assert on logs, not events.
auto ignore_events() {
    return [](const MoonrakerEvent&) {};
}

/// Matches only the periodic pending-count line, not the tracker's other
/// "[Request Tracker] ..." output (cancellations, timeouts).
constexpr const char* PENDING_LINE = "pending request(s); oldest";

} // namespace

TEST_CASE("check_timeouts stays silent for a queue that is draining normally",
          "[moonraker][tracker][logging][regression]") {
    MoonrakerRequestTracker tracker;
    LogCapture logs;

    // A healthy Spoolman poll burst: three requests, all tens of milliseconds old,
    // draining one at a time. Each decrement is a distinct signature, so the
    // signature throttle alone would emit three lines.
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 1, make_pending_request("server.spoolman.proxy", std::chrono::milliseconds(46)));
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 2, make_pending_request("server.spoolman.proxy", std::chrono::milliseconds(52)));
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 3, make_pending_request("server.spoolman.proxy", std::chrono::milliseconds(58)));

    tracker.check_timeouts(ignore_events());
    REQUIRE(tracker.cancel(3));
    tracker.check_timeouts(ignore_events());
    REQUIRE(tracker.cancel(2));
    tracker.check_timeouts(ignore_events());

    REQUIRE(logs.count_containing(PENDING_LINE) == 0);
}

TEST_CASE("check_timeouts logs once the oldest pending request ages past the floor",
          "[moonraker][tracker][logging][regression]") {
    MoonrakerRequestTracker tracker;
    LogCapture logs;

    // Past PENDING_LOG_MIN_AGE_MS but well short of the 30 s warn escalation:
    // the queue is not draining promptly and that is worth exactly one line.
    const auto age =
        std::chrono::milliseconds(MoonrakerRequestTracker::PENDING_LOG_MIN_AGE_MS + 500);
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 1, make_pending_request("printer.objects.query", age));

    tracker.check_timeouts(ignore_events());
    REQUIRE(logs.count_containing(PENDING_LINE) == 1);

    // Signature unchanged on the next tick — the existing throttle still applies,
    // so an aging request does not turn into a per-tick stream.
    tracker.check_timeouts(ignore_events());
    REQUIRE(logs.count_containing(PENDING_LINE) == 1);
}

TEST_CASE("check_timeouts still escalates a stuck request to warn",
          "[moonraker][tracker][logging][regression][909]") {
    MoonrakerRequestTracker tracker;
    LogCapture logs;

    // The #909 leading indicator must survive the new age floor.
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 1, make_pending_request("printer.objects.query", std::chrono::seconds(45)));

    tracker.check_timeouts(ignore_events());

    REQUIRE(logs.count_containing(PENDING_LINE) == 1);
    REQUIRE(logs.count_containing("warning") == 1);
}

TEST_CASE("check_timeouts does not warn on a blocking G-code script that is merely slow",
          "[moonraker][tracker][logging][regression]") {
    MoonrakerRequestTracker tracker;
    LogCapture logs;

    // printer.gcode.script does not return until Klipper finishes the macro. An
    // AFC toolchange over a 2 m bowden is legitimately over a minute, and at the
    // generic 30 s threshold it made the tracker the loudest thing in an AFC
    // user's log — 77 warnings in one debug bundle, every one of them healthy.
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 1, make_pending_request("printer.gcode.script", std::chrono::seconds(90)));

    tracker.check_timeouts(ignore_events());

    REQUIRE(logs.count_containing(PENDING_LINE) == 1);
    REQUIRE(logs.count_containing("warning") == 0);
}

TEST_CASE("check_timeouts still warns on a G-code script past its own longer threshold",
          "[moonraker][tracker][logging][regression]") {
    MoonrakerRequestTracker tracker;
    LogCapture logs;

    // The exemption raises the bar, it does not remove it: past five minutes a
    // script really has stopped and #909's leading indicator has to still fire.
    const auto age =
        std::chrono::milliseconds(MoonrakerRequestTracker::PENDING_WARN_AGE_GCODE_MS + 1000);
    MoonrakerRequestTrackerTestAccess::inject_request(
        tracker, 1, make_pending_request("printer.gcode.script", age, /*timeout_ms=*/900000));

    tracker.check_timeouts(ignore_events());

    REQUIRE(logs.count_containing(PENDING_LINE) == 1);
    REQUIRE(logs.count_containing("warning") == 1);
}
