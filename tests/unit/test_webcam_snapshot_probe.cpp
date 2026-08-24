// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_webcam_snapshot_probe.cpp
 * @brief Unit tests for helix::probe_snapshot_reachable().
 *
 * Discovery guards against a stale ABSOLUTE webcam snapshot URL (an install-time
 * LAN IP since changed by DHCP) by probing it once at startup. The probe must
 * separate two answers that a single timeout conflates:
 *
 *   - "nothing is listening"      → reject, and reject fast
 *   - "listening, but slow"       → accept
 *
 * A go2rtc endpoint transcoding H.264 waits for the next keyframe before it can
 * emit a JPEG — measured at up to ~2.9s on a Pi 5. libhv clamps the connect phase
 * to MIN(connect_timeout, timeout), so the old single 2s budget covered both
 * phases and rejected that live camera exactly like a dead host
 * (prestonbrown/helixscreen#1205).
 *
 * These tests run against a real local HTTP server, not a mock: the behavior under
 * test lives entirely in libhv's timeout handling, which a mock would not exercise.
 */

#include "../../include/moonraker_discovery_sequence.h"
#include "hv/HttpServer.h"

#include <chrono>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Ports for the probe fixtures. High and fixed — the suite is single-process and
/// these are bound only for the lifetime of one TEST_CASE.
constexpr int PROBE_SERVER_PORT = 19731;
constexpr int DEAD_PORT = 19732; // deliberately never bound

/// Milliseconds the "slow camera" endpoint stalls before answering. Above the 2s
/// connect budget so it fails under the old single-timeout scheme, and comfortably
/// under the total budget so it must pass under the new one.
constexpr int SLOW_RESPONSE_MS = 2500;

/// Smallest bytes that look like a JPEG. Content is irrelevant to the probe (it
/// only reads the status code) but a realistic body keeps the fixture honest.
const std::string JPEG_BODY = "\xFF\xD8\xFF\xE0 fake jpeg payload \xFF\xD9";

/// A libhv HTTP server that serves the probe fixtures for the life of the object.
class ProbeServer {
  public:
    ProbeServer() {
        service_.GET("/fast.jpg", [](HttpRequest*, HttpResponse* resp) {
            resp->content_type = APPLICATION_OCTET_STREAM;
            resp->body = JPEG_BODY;
            return 200;
        });
        service_.GET("/slow.jpg", [](HttpRequest*, HttpResponse* resp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(SLOW_RESPONSE_MS));
            resp->content_type = APPLICATION_OCTET_STREAM;
            resp->body = JPEG_BODY;
            return 200;
        });
        service_.GET("/missing.jpg", [](HttpRequest*, HttpResponse* resp) {
            resp->body = "no such camera";
            return 404;
        });

        server_.registerHttpService(&service_);
        server_.setPort(PROBE_SERVER_PORT);
        // Each stalled handler occupies a worker for SLOW_RESPONSE_MS; give the
        // server enough threads that one slow request cannot starve the others.
        server_.setThreadNum(4);
        started_ = server_.start() == 0;
        // start() is asynchronous — let the listener come up before probing.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ~ProbeServer() {
        server_.stop();
    }

    [[nodiscard]] bool started() const {
        return started_;
    }

    static std::string url(const std::string& path) {
        return "http://127.0.0.1:" + std::to_string(PROBE_SERVER_PORT) + path;
    }

  private:
    hv::HttpService service_;
    hv::HttpServer server_;
    bool started_ = false;
};

/// Run fn and report how long it took, in milliseconds.
template <typename Fn> long long time_ms(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

} // namespace

TEST_CASE("Snapshot probe accepts an endpoint slower than the connect budget",
          "[webcam][discovery][probe][1205][slow]") {
    ProbeServer server;
    REQUIRE(server.started());

    // The regression for #1205: a live go2rtc camera that needs ~2.5s to produce
    // its first keyframe must be kept, not discarded as unreachable.
    REQUIRE(probe_snapshot_reachable(ProbeServer::url("/slow.jpg")));
}

TEST_CASE("Snapshot probe accepts an endpoint that answers immediately",
          "[webcam][discovery][probe][1205]") {
    ProbeServer server;
    REQUIRE(server.started());

    REQUIRE(probe_snapshot_reachable(ProbeServer::url("/fast.jpg")));
}

TEST_CASE("Snapshot probe rejects a reachable host that has no camera there",
          "[webcam][discovery][probe][1205]") {
    ProbeServer server;
    REQUIRE(server.started());

    // A 200 is the only acceptable answer — a 404 means the URL is stale even
    // though something is listening.
    REQUIRE_FALSE(probe_snapshot_reachable(ProbeServer::url("/missing.jpg")));
}

TEST_CASE("Snapshot probe rejects a dead address without waiting out the response budget",
          "[webcam][discovery][probe][1205]") {
    const std::string dead = "http://127.0.0.1:" + std::to_string(DEAD_PORT) + "/frame.jpeg";

    bool reachable = true;
    const long long elapsed_ms = time_ms([&] { reachable = probe_snapshot_reachable(dead); });

    REQUIRE_FALSE(reachable);
    // The point of a separate connect budget: raising the response timeout must not
    // make the stale-address case — the reason this probe exists — any slower. A
    // refused connection returns immediately; the assertion guards against a future
    // change that folds the two budgets back together.
    REQUIRE(elapsed_ms < SNAPSHOT_PROBE_TOTAL_TIMEOUT_SEC * 1000);
}

TEST_CASE("Snapshot probe allows a live endpoint more time than a dead one",
          "[webcam][discovery][probe][1205]") {
    // The two budgets must stay distinct. Collapsing them (as the pre-#1205 code
    // did, with a single 2s total) is what rejected the reporter's camera.
    REQUIRE(SNAPSHOT_PROBE_TOTAL_TIMEOUT_SEC > SNAPSHOT_PROBE_CONNECT_TIMEOUT_SEC);
}
