// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_initial_connect_failure.cpp
 * @brief A socket that never opens must eventually say so.
 *
 * Context (debug bundle XRK8KPTF, K2 Plus, v0.99.98): the configured Moonraker
 * host was stale, so the WebSocket never opened once. The client retried every
 * ~3.1s for the whole session and told the user nothing — the only indication
 * was a disconnected icon that does not name the host it is failing to reach.
 *
 * The existing escalation could not fire, for two independent reasons:
 *   1. start_health_timer() is called only from on_ws_open(), so a socket that
 *      never opens never starts the timer that owns the stall check.
 *   2. That check requires state RECONNECTING, but the never-connected close
 *      path sets DISCONNECTED.
 *
 * Both are conditioned on having connected once. This pins the initial-connect
 * case: after initial_connect_failure_timeout elapses with no socket ever
 * opened, emit CONNECTION_FAILED (which MoonrakerManager routes to an error
 * modal) and settle in FAILED (which PrinterStatusIcon renders red).
 */

#include "../../include/moonraker_client.h"
#include "../../include/moonraker_error.h"
#include "hv/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Closed port on loopback: connect() fails fast with RST, so the reconnect
// chain cycles quickly instead of waiting on a SYN timeout.
constexpr const char* DEAD_URL = "ws://127.0.0.1:19998/websocket";

struct LoopClient {
    LoopClient() : loop_(std::make_shared<hv::EventLoopThread>()) {
        loop_->start();
        client_ = std::make_unique<MoonrakerClient>(loop_->loop());
    }
    ~LoopClient() {
        // JOIN before destroying the client (#1146).
        loop_->stop();
        loop_->join();
        client_.reset();
    }

    std::shared_ptr<hv::EventLoopThread> loop_;
    std::unique_ptr<MoonrakerClient> client_;
};

} // namespace

TEST_CASE("A never-opened WebSocket escalates to CONNECTION_FAILED",
          "[moonraker][client][regression][eventloop][slow]") {
    LoopClient c;

    std::mutex m;
    std::vector<MoonrakerEvent> events;
    c.client_->register_event_handler([&](const MoonrakerEvent& e) {
        std::lock_guard<std::mutex> lk(m);
        events.push_back(e);
    });

    // Production default is 60s; shorten so the test does not sit through it.
    c.client_->set_initial_connect_failure_timeout(200);

    c.client_->connect(DEAD_URL, []() {}, []() {});

    // Poll rather than sleep a fixed span: the reconnect cadence is libhv's.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool saw_failure = false;
    std::string failure_message;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(m);
            for (const auto& e : events) {
                if (e.type == MoonrakerEventType::CONNECTION_FAILED) {
                    saw_failure = true;
                    failure_message = e.message;
                    break;
                }
            }
        }
        if (saw_failure) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    REQUIRE(saw_failure);

    // The message must name the endpoint we could not reach. Bundle XRK8KPTF's
    // reporter had a stale host in settings and no surface anywhere told them
    // which address the app was dialing.
    CHECK(failure_message.find("19998") != std::string::npos);

    // DEAD_URL is loopback, i.e. the printer HelixScreen is running on. Telling
    // that user to "check that the printer is powered on and that this address
    // is correct" is advice they cannot act on — the screen in their hand proves
    // the printer is up, and 127.0.0.1 is not wrong. AD5X bundles TAU4PW4H /
    // 865DXBQ7 are two boots of exactly this, with Moonraker simply not running.
    CHECK(failure_message.find("this printer") != std::string::npos);
    CHECK(failure_message.find("address is correct") == std::string::npos);

    // FAILED (not DISCONNECTED) is what PrinterStatusIcon renders as an error.
    CHECK(c.client_->get_connection_state() == ConnectionState::FAILED);

    // Emitted once, not once per ~3s retry — the modal must not re-open forever.
    {
        std::lock_guard<std::mutex> lk(m);
        int failures = 0;
        for (const auto& e : events) {
            if (e.type == MoonrakerEventType::CONNECTION_FAILED) {
                ++failures;
            }
        }
        CHECK(failures == 1);
    }

    c.client_->disconnect();
}

TEST_CASE("A reachable-then-lost connection is not treated as an initial failure",
          "[moonraker][client][regression][eventloop][slow]") {
    // Guard against over-firing: with no connect() ever issued there is no
    // attempt in flight, so nothing may escalate. This pins that the escalation
    // is anchored on a real connect attempt rather than on mere elapsed time.
    LoopClient c;

    std::mutex m;
    std::atomic<int> failures{0};
    c.client_->register_event_handler([&](const MoonrakerEvent& e) {
        if (e.type == MoonrakerEventType::CONNECTION_FAILED) {
            failures.fetch_add(1);
        }
    });

    c.client_->set_initial_connect_failure_timeout(50);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CHECK(failures.load() == 0);
    CHECK(c.client_->get_connection_state() == ConnectionState::DISCONNECTED);
}
