// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_client.h"
#include "hv/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace std::chrono;

/**
 * Moonraker Connection Retry Tests
 *
 * These tests verify the connection retry mechanism works correctly
 * after bug fixes for instant retry failures and timeout issues.
 *
 * Critical bugs tested:
 * 1. Connection timeout not being applied (was using 15s instead of configured 5s)
 * 2. Second connection attempt failing instantly (at same millisecond)
 * 3. Auto-reconnect spam issues
 * 4. State management during retries
 *
 * Test strategy: Use invalid IP addresses (RFC 5737 TEST-NET-1) to trigger
 * timeouts without network-dependent failures.
 */

// Test constants
static constexpr uint32_t TEST_TIMEOUT_MS = 1000; // 1 second for fast tests
static constexpr const char* INVALID_URL = "ws://192.0.2.1:7125/websocket"; // RFC 5737 TEST-NET-1

/**
 * Test fixture for connection retry scenarios
 * Uses EventLoopThread to run the event loop in a separate thread
 */
class MoonrakerConnectionRetryFixture {
  public:
    MoonrakerConnectionRetryFixture() {
        loop_thread_ = std::make_shared<hv::EventLoopThread>();
        loop_thread_->start();

        client_ = std::make_unique<MoonrakerClient>(loop_thread_->loop());

        // Configure short timeouts for faster testing
        client_->set_connection_timeout(TEST_TIMEOUT_MS);
        client_->set_default_request_timeout(TEST_TIMEOUT_MS);

        // Disable auto-reconnect for manual retry testing
        client_->setReconnect(nullptr);
    }

    ~MoonrakerConnectionRetryFixture() {
        // disconnect() posts a close event to the libhv loop. The Channel
        // must remain alive until either the event is dispatched or the
        // loop is stopped — deleting client_ first races with the loop
        // thread running Channel::close() on freed memory (heap-use-after-
        // free observed under ASAN nightly).
        if (client_) {
            client_->disconnect();
        }
        loop_thread_->stop(true);
        client_.reset();
    }

    std::shared_ptr<hv::EventLoopThread> loop_thread_;
    std::unique_ptr<MoonrakerClient> client_;
};

TEST_CASE("Moonraker connection retries work correctly", "[connection][eventloop][slow]") {
    SECTION("First connection attempt respects timeout") {
        // Everything the disconnect callback touches is declared BEFORE the
        // fixture, so it outlives it. ~MoonrakerConnectionRetryFixture calls
        // disconnect() and then pumps the libhv loop to completion, which
        // dispatches one last on_ws_close on the loop thread. A local declared
        // after the fixture is already destroyed by the time that callback
        // runs — ASan caught exactly that as a stack-use-after-scope on
        // `disconnect_count` below.
        std::atomic<bool> disconnected{false};
        MoonrakerConnectionRetryFixture fixture;
        auto start = steady_clock::now();

        fixture.client_->connect(
            INVALID_URL,
            []() {
                // Should never connect to invalid address
                FAIL("Connection succeeded to invalid address");
            },
            [&]() { disconnected = true; });

        // Wait for disconnect callback (with timeout)
        for (int i = 0; i < 30 && !disconnected; i++) {
            std::this_thread::sleep_for(milliseconds(100));
        }

        auto duration = duration_cast<milliseconds>(steady_clock::now() - start).count();

        REQUIRE(disconnected);
        CHECK(duration >= TEST_TIMEOUT_MS - 500); // Within 500ms tolerance
        CHECK(duration < TEST_TIMEOUT_MS * 2);    // Should not exceed 2x configured value
    }

    SECTION("Second connection attempt also respects timeout (not instant failure)") {
        std::atomic<int> attempt{0};
        std::mutex durations_mutex;
        std::vector<milliseconds> durations;
        std::atomic<bool> all_done{false};
        // do_attempt is re-entered from the disconnect callback, so it too must
        // outlive the fixture. Declared here, assigned once the fixture exists.
        std::function<void()> do_attempt;
        MoonrakerConnectionRetryFixture fixture;

        do_attempt = [&]() {
            int current_attempt = ++attempt;
            auto start = steady_clock::now();

            fixture.client_->connect(
                INVALID_URL,
                [&]() {
                    FAIL("Connection succeeded to invalid address on attempt " << current_attempt);
                },
                [&, start, current_attempt]() {
                    auto duration = duration_cast<milliseconds>(steady_clock::now() - start);
                    {
                        std::lock_guard<std::mutex> lock(durations_mutex);
                        durations.push_back(duration);
                    }

                    if (current_attempt < 3) {
                        // Small delay before next attempt
                        std::this_thread::sleep_for(milliseconds(100));
                        do_attempt();
                    } else {
                        all_done = true;
                    }
                });
        };

        do_attempt();

        // Wait for all attempts to complete
        for (int i = 0; i < 100 && !all_done; i++) {
            std::this_thread::sleep_for(milliseconds(100));
        }

        std::lock_guard<std::mutex> lock(durations_mutex);
        REQUIRE(durations.size() == 3);

        for (size_t i = 0; i < durations.size(); i++) {
            INFO("Attempt " << (i + 1) << " took " << durations[i].count() << "ms");
            CHECK(durations[i].count() >= 100); // Should NOT fail instantly
            CHECK(durations[i].count() < TEST_TIMEOUT_MS * 2);
        }
    }

    SECTION("Multiple rapid retries all work correctly") {
        constexpr int NUM_RETRIES = 5;
        std::atomic<int> attempt{0};
        std::mutex durations_mutex;
        std::vector<milliseconds> durations;
        std::atomic<bool> all_done{false};
        std::function<void()> do_attempt;
        MoonrakerConnectionRetryFixture fixture;

        do_attempt = [&]() {
            int current_attempt = ++attempt;
            auto start = steady_clock::now();

            fixture.client_->connect(
                INVALID_URL,
                [&]() {
                    FAIL("Connection succeeded to invalid address on attempt " << current_attempt);
                },
                [&, start, current_attempt]() {
                    auto duration = duration_cast<milliseconds>(steady_clock::now() - start);
                    {
                        std::lock_guard<std::mutex> lock(durations_mutex);
                        durations.push_back(duration);
                    }

                    if (current_attempt < NUM_RETRIES) {
                        // Immediate retry (no delay) - tests the bug fix
                        do_attempt();
                    } else {
                        all_done = true;
                    }
                });
        };

        do_attempt();

        // Wait for all attempts to complete
        for (int i = 0; i < 150 && !all_done; i++) {
            std::this_thread::sleep_for(milliseconds(100));
        }

        std::lock_guard<std::mutex> lock(durations_mutex);
        REQUIRE(durations.size() == NUM_RETRIES);

        // All attempts should take approximately the timeout duration
        for (size_t i = 0; i < durations.size(); i++) {
            INFO("Attempt " << (i + 1) << " took " << durations[i].count() << "ms");
            CHECK(durations[i].count() >= 100); // Should NOT fail instantly
        }
    }

    SECTION("Auto-reconnect stays disabled between retries") {
        std::atomic<int> disconnect_count{0};
        MoonrakerConnectionRetryFixture fixture;

        fixture.client_->connect(
            INVALID_URL, []() { FAIL("Connection succeeded to invalid address"); },
            [&]() { disconnect_count++; });

        // Wait for first disconnect, then wait more to ensure no auto-reconnect
        for (int i = 0; i < 10 && disconnect_count == 0; i++) {
            std::this_thread::sleep_for(milliseconds(100));
        }

        // Wait additional time to detect any auto-reconnect
        std::this_thread::sleep_for(milliseconds(1000));

        // Should only disconnect once (no auto-reconnect)
        CHECK(disconnect_count == 1);
    }

    SECTION("Connection state transitions correctly during retries") {
        std::mutex states_mutex;
        std::vector<ConnectionState> states;
        std::atomic<bool> all_done{false};
        std::atomic<int> attempt{0};
        std::function<void()> do_attempt;
        MoonrakerConnectionRetryFixture fixture;

        fixture.client_->set_state_change_callback([&](ConnectionState, ConnectionState new_state) {
            std::lock_guard<std::mutex> lock(states_mutex);
            states.push_back(new_state);
        });

        do_attempt = [&]() {
            int current_attempt = ++attempt;
            fixture.client_->connect(
                INVALID_URL, []() {},
                [&, current_attempt]() {
                    if (current_attempt < 2) {
                        std::this_thread::sleep_for(milliseconds(100));
                        do_attempt();
                    } else {
                        all_done = true;
                    }
                });
        };

        do_attempt();

        // Wait for all attempts to complete
        for (int i = 0; i < 50 && !all_done; i++) {
            std::this_thread::sleep_for(milliseconds(100));
        }

        // Should see: CONNECTING -> DISCONNECTED -> CONNECTING -> DISCONNECTED
        std::lock_guard<std::mutex> lock(states_mutex);
        REQUIRE(states.size() >= 4);
        CHECK(states[0] == ConnectionState::CONNECTING);
        CHECK(states[1] == ConnectionState::DISCONNECTED);
        CHECK(states[2] == ConnectionState::CONNECTING);
        CHECK(states[3] == ConnectionState::DISCONNECTED);
    }

    SECTION("Disconnect is idempotent") {
        MoonrakerConnectionRetryFixture fixture;

        // Call disconnect multiple times without connecting
        REQUIRE_NOTHROW(fixture.client_->disconnect());
        REQUIRE_NOTHROW(fixture.client_->disconnect());
        REQUIRE_NOTHROW(fixture.client_->disconnect());

        CHECK(fixture.client_->get_connection_state() == ConnectionState::DISCONNECTED);
    }

    SECTION("Disconnect during connection attempt cleans up properly") {
        std::atomic<bool> connected{false};
        std::atomic<bool> disconnected{false};
        MoonrakerConnectionRetryFixture fixture;

        fixture.client_->connect(
            INVALID_URL, [&]() { connected = true; }, [&]() { disconnected = true; });

        // Wait a bit then disconnect
        std::this_thread::sleep_for(milliseconds(200));
        fixture.client_->disconnect();

        // Wait to see if disconnect callback fires
        std::this_thread::sleep_for(milliseconds(300));

        CHECK_FALSE(connected);
        CHECK(fixture.client_->get_connection_state() == ConnectionState::DISCONNECTED);
    }
}
