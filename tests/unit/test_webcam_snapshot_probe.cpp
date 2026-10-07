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

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A loopback port nothing is listening on. Asked of the kernel per fixture so
/// parallel test shards (and other sessions' runs) never contend for one number.
int free_loopback_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    const bool ok = fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
    if (fd >= 0)
        ::close(fd);
    return ok ? ntohs(addr.sin_port) : 0;
}

/// True once a TCP connect to the loopback port succeeds.
bool accepts_connections(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    const bool ok = fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (fd >= 0)
        ::close(fd);
    return ok;
}

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
        server_.setPort(port_);
        // One loop thread: every case sends one request at a time, and libhv's
        // HttpMessage caches its Date header in a process-wide buffer that two loop
        // threads write without a lock.
        server_.setThreadNum(1);
        started_ = port_ != 0 && server_.start() == 0;
        // start() is asynchronous: probe until the listener accepts.
        for (int i = 0; started_ && i < 500 && !accepts_connections(port_); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~ProbeServer() {
        server_.stop();
    }

    [[nodiscard]] bool started() const {
        return started_;
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

  private:
    const int port_ = free_loopback_port();
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
    REQUIRE(probe_snapshot_reachable(server.url("/slow.jpg")));
}

TEST_CASE("Snapshot probe accepts an endpoint that answers immediately",
          "[webcam][discovery][probe][1205]") {
    ProbeServer server;
    REQUIRE(server.started());

    REQUIRE(probe_snapshot_reachable(server.url("/fast.jpg")));
}

TEST_CASE("Snapshot probe rejects a reachable host that has no camera there",
          "[webcam][discovery][probe][1205]") {
    ProbeServer server;
    REQUIRE(server.started());

    // A 200 is the only acceptable answer — a 404 means the URL is stale even
    // though something is listening.
    REQUIRE_FALSE(probe_snapshot_reachable(server.url("/missing.jpg")));
}

TEST_CASE("Snapshot probe rejects a dead address without waiting out the response budget",
          "[webcam][discovery][probe][1205]") {
    const std::string dead =
        "http://127.0.0.1:" + std::to_string(free_loopback_port()) + "/frame.jpeg";

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
