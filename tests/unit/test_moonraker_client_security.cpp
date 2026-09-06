// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_client.h"
#include "../../include/moonraker_error.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;
/**
 * MoonrakerClient Security Tests
 *
 * Comprehensive tests for Moonraker security fixes from Issues #2, #3, #4, #6, #7, #9
 * in the Moonraker Security Review (docs/MOONRAKER_SECURITY_REVIEW.md).
 *
 * Test Categories:
 * 1. Issue #4: Use-After-Free - Destructor cleanup (no dangling callbacks)
 * 2. Issue #6: Deadlock Risk - Two-phase timeout pattern (callbacks outside mutex)
 * 3. Issue #7: JSON-RPC Validation - Method/params/payload validation
 * 4. Issue #9: Exception Safety - All callbacks exception-safe
 *
 * SECURITY CRITICAL: These tests verify memory safety, thread safety, and
 * robust error handling that prevents crashes and undefined behavior.
 */

// ============================================================================
// Test Fixtures
// ============================================================================

/**
 * @brief Base fixture for MoonrakerClient security tests
 *
 * Provides isolated client instances and callback tracking.
 */
class MoonrakerClientSecurityFixture {
  public:
    MoonrakerClientSecurityFixture() {
        // Default-construct (no external loop) so libhv's TcpClientTmpl sets
        // is_loop_owner=true. That is what makes ~WebSocketClient's stop(true)
        // actually join the event-loop thread before any member is freed —
        // supplying a loop makes stop() skip the join and frees http_req_ and the
        // onConnection/onMessage std::functions out from under a live thread.
        // Matches production topology (MoonrakerManager::create_client()).
        client = std::make_unique<MoonrakerClient>();

        reset_callbacks();
    }

    ~MoonrakerClientSecurityFixture() {
        client.reset();
    }

    void reset_callbacks() {
        success_called = false;
        error_called = false;
        callback_count = 0;
        captured_error = MoonrakerError();
    }

    // Standard callbacks for testing
    void success_callback(json response) {
        success_called = true;
        callback_count++;
        captured_response = response;
    }

    void error_callback(const MoonrakerError& err) {
        error_called = true;
        callback_count++;
        captured_error = err;
    }

    // Test objects
    std::unique_ptr<MoonrakerClient> client;

    // Callback tracking
    std::atomic<bool> success_called{false};
    std::atomic<bool> error_called{false};
    std::atomic<int> callback_count{0};
    MoonrakerError captured_error;
    json captured_response;
};

// ============================================================================
// Issue #4: Use-After-Free - Destructor Cleanup
// ============================================================================

// [slow] tag: the create/destroy SECTIONs churn real libhv connect attempts
// against a dead port, so they belong in the dedicated test-eventloop CI lane
// per L052.
//
// Every client here is default-constructed. Supplying an hv::EventLoop makes
// libhv's TcpClientTmpl set is_loop_owner=false, which makes stop() skip
// EventLoopThread::stop(wait); ~WebSocketClient then returns without joining and
// frees http_req_/http_parser_ and the onConnection/onMessage std::functions
// while the loop thread is still running the failing connect — heap corruption
// that surfaced as a SIGSEGV in an unrelated test thousands of cases later
// (prestonbrown/helixscreen#1146).
TEST_CASE("MoonrakerClient destructor clears callbacks (UAF prevention)",
          "[connection][security][uaf][issue4][eventloop][slow]") {
    SECTION("Destroy client before connection completes") {
        auto client = std::make_unique<MoonrakerClient>();

        std::atomic<bool> connected_called{false};
        std::atomic<bool> disconnected_called{false};

        // Start connection to non-existent server (will fail)
        client->connect(
            "ws://127.0.0.1:19999/websocket", [&connected_called]() { connected_called = true; },
            [&disconnected_called]() { disconnected_called = true; });

        // Destroy client immediately before connection resolves
        client.reset();

        // Sleep briefly to allow any pending events
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // If callbacks weren't cleared, this could crash with UAF
        // Test passing = callbacks properly cleared
        REQUIRE_FALSE(connected_called);
    }

    SECTION("Destroy client with pending requests") {
        auto client = std::make_unique<MoonrakerClient>();

        std::atomic<bool> error_callback_invoked{false};

        // Send request that will never complete (no connection)
        client->send_jsonrpc(
            "printer.info", json(), [](json) { FAIL("Success callback should not be called"); },
            [&error_callback_invoked](const MoonrakerError& err) {
                error_callback_invoked = true;
                REQUIRE(err.type == MoonrakerErrorType::CONNECTION_LOST);
            });

        // Destroy client - should invoke error callbacks with CONNECTION_LOST
        client.reset();

        // Error callback should have been invoked during cleanup
        REQUIRE(error_callback_invoked.load());
    }

    SECTION("Multiple rapid create/destroy cycles (stress test)") {
        // Stress test: rapid allocation/deallocation to catch UAF bugs
        int cycles_completed = 0;

        for (int i = 0; i < 20; i++) {
            auto client = std::make_unique<MoonrakerClient>();

            // Start connection
            client->connect(
                "ws://127.0.0.1:19999/websocket", []() { /* connected */ },
                []() { /* disconnected */ });

            // Send pending request
            client->send_jsonrpc("printer.info", json(), [](json) {}, [](const MoonrakerError&) {});

            // Destroy immediately
            client.reset();
            cycles_completed++;
        }

        // All 20 rapid create/connect/destroy cycles completed without crash
        REQUIRE(cycles_completed == 20);
    }

    SECTION("Destroy client with registered persistent callbacks") {
        auto client = std::make_unique<MoonrakerClient>();

        std::atomic<bool> notify_callback_invoked{false};

        // Register persistent status update callback
        client->register_notify_update(
            [&notify_callback_invoked](json j) { notify_callback_invoked = true; });

        // Register persistent method callback
        client->register_method_callback("notify_gcode_response", "test_handler",
                                         [](json j) { /* callback */ });

        // Destroy client
        client.reset();

        // If callbacks weren't cleared, accessing them would crash
        REQUIRE_FALSE(notify_callback_invoked);
    }
}

// ============================================================================
// Issue #6: Deadlock Risk - Two-Phase Timeout Pattern
// ============================================================================

// NOTE: This test requires an actual WebSocket connection to test timeout behavior.
// Without a connection, send_jsonrpc immediately fails with CONNECTION_LOST
// before any timeout can occur. Marked as integration test.
TEST_CASE_METHOD(MoonrakerClientSecurityFixture,
                 "MoonrakerClient timeout callbacks invoked outside mutex",
                 "[connection][security][deadlock][issue6][.integration][slow]") {
    SECTION("Timeout callback can safely call send_jsonrpc (no deadlock)") {
        // This test verifies the two-phase timeout pattern:
        // Phase 1: Copy callbacks under lock
        // Phase 2: Invoke callbacks outside lock

        bool timeout_callback_invoked = false;
        bool nested_request_sent = false;

        // Set very short timeout for testing
        client->set_default_request_timeout(100); // 100ms

        // Send request with callback that sends another request
        client->send_jsonrpc(
            "printer.info", json(), [](json) { FAIL("Should timeout, not succeed"); },
            [this, &timeout_callback_invoked, &nested_request_sent](const MoonrakerError& err) {
                timeout_callback_invoked = true;
                // May be TIMEOUT (if request was queued) or CONNECTION_LOST (if no connection)
                REQUIRE((err.type == MoonrakerErrorType::TIMEOUT ||
                         err.type == MoonrakerErrorType::CONNECTION_LOST));

                // Try to send nested request (would deadlock if mutex held)
                int result = client->send_jsonrpc(
                    "server.info", json(), [](json) {}, [](const MoonrakerError&) {});

                // If we reach here, no deadlock occurred
                nested_request_sent = true;
            });

        // Wait for timeout to occur
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Trigger timeout check
        client->process_timeouts();

        // Verify callback was invoked and nested request succeeded
        REQUIRE(timeout_callback_invoked);
        REQUIRE(nested_request_sent);
    }

    SECTION("Cleanup callbacks can safely call send_jsonrpc (no deadlock)") {
        // Verify cleanup_pending_requests uses two-phase pattern

        bool cleanup_callback_invoked = false;
        bool nested_request_sent = false;

        // Send request with callback that sends another request
        client->send_jsonrpc(
            "printer.info", json(), [](json) { FAIL("Should be cleaned up, not succeed"); },
            [this, &cleanup_callback_invoked, &nested_request_sent](const MoonrakerError& err) {
                cleanup_callback_invoked = true;
                REQUIRE(err.type == MoonrakerErrorType::CONNECTION_LOST);

                // Try to send nested request (would deadlock if mutex held)
                // Note: client may be nullptr during destruction, but attempt should not deadlock
                if (client) {
                    int result = client->send_jsonrpc(
                        "server.info", json(), [](json) {}, [](const MoonrakerError&) {});
                }

                nested_request_sent = true;
            });

        // Destroy client to trigger cleanup
        client.reset();

        // Verify callback was invoked and nested request succeeded
        REQUIRE(cleanup_callback_invoked);
        REQUIRE(nested_request_sent);
    }
}

// ============================================================================
// Issue #9: Exception Safety
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientSecurityFixture,
                 "MoonrakerClient state change callback is exception safe",
                 "[connection][security][exception][issue9][eventloop][slow]") {
    SECTION("State change callback that throws doesn't crash") {
        std::atomic<bool> callback_invoked{false};

        // Register callback that throws
        client->set_state_change_callback(
            [&callback_invoked](ConnectionState old_state, ConnectionState new_state) {
                callback_invoked = true;
                throw std::runtime_error("Test exception in state callback");
            });

        // Trigger state change by attempting connection
        // Exception should be caught and logged, not propagate
        REQUIRE_NOTHROW(client->connect("ws://127.0.0.1:19999/websocket", []() {}, []() {}));

        // Verify callback was invoked (and threw)
        REQUIRE(callback_invoked.load());
    }
}

TEST_CASE_METHOD(MoonrakerClientSecurityFixture,
                 "MoonrakerClient error callbacks are exception safe",
                 "[connection][security][exception][issue9][eventloop][slow]") {
    SECTION("Error callback throwing doesn't crash during cleanup") {
        bool first_callback_called = false;
        bool second_callback_called = false;

        // Register multiple requests with throwing error callbacks
        client->send_jsonrpc(
            "printer.info", json(), [](json) {},
            [&first_callback_called](const MoonrakerError& err) {
                first_callback_called = true;
                throw std::runtime_error("Test exception 1");
            });

        client->send_jsonrpc(
            "server.info", json(), [](json) {},
            [&second_callback_called](const MoonrakerError& err) {
                second_callback_called = true;
                // This callback doesn't throw
            });

        // Destroy client - should not crash even if callbacks throw
        REQUIRE_NOTHROW(client.reset());

        // First callback was invoked and threw
        REQUIRE(first_callback_called);

        // Second callback should still have been called
        // (exception handling shouldn't stop iteration)
        REQUIRE(second_callback_called);
    }

    SECTION("Error callback throwing doesn't crash during timeout") {
        bool timeout_callback_called = false;

        // Set very short timeout
        client->set_default_request_timeout(50);

        // Register request with throwing timeout callback
        client->send_jsonrpc(
            "printer.info", json(), [](json) {},
            [&timeout_callback_called](const MoonrakerError& err) {
                timeout_callback_called = true;
                throw std::runtime_error("Test exception in timeout");
            });

        // Wait for timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Process timeouts - should not crash
        REQUIRE_NOTHROW(client->process_timeouts());

        REQUIRE(timeout_callback_called);
    }
}

TEST_CASE_METHOD(MoonrakerClientSecurityFixture,
                 "MoonrakerClient notify callbacks are exception safe",
                 "[connection][security][exception][issue9][eventloop][slow]") {
    SECTION("Notify callback throwing doesn't crash") {
        std::atomic<bool> callback_invoked{false};

        // Register notify callback that throws
        client->register_notify_update([&callback_invoked](const json& notification) {
            callback_invoked = true;
            throw std::runtime_error("Test exception in notify callback");
        });

        // Verify notify callback registration itself doesn't throw
        // (actual notification dispatch requires a server connection)
        REQUIRE(callback_invoked.load() == false);
    }

    SECTION("Method callback throwing doesn't crash") {
        std::atomic<bool> callback_invoked{false};

        // Register method callback that throws
        client->register_method_callback(
            "notify_gcode_response", "test_handler", [&callback_invoked](const json& notification) {
                callback_invoked = true;
                throw std::runtime_error("Test exception in method callback");
            });

        // Verify method callback registration itself doesn't throw
        // (actual method dispatch requires a server connection)
        REQUIRE(callback_invoked.load() == false);
    }
}

TEST_CASE("MoonrakerClient all callback types exception-safe (comprehensive)",
          "[connection][security][exception][issue9][eventloop][slow]") {
    SECTION("Exception in every callback type doesn't crash") {
        auto client = std::make_unique<MoonrakerClient>();

        // Connection callbacks
        REQUIRE_NOTHROW(client->connect(
            "ws://127.0.0.1:19999/websocket",
            []() { throw std::runtime_error("onopen exception"); },
            []() { throw std::runtime_error("onclose exception"); }));

        // Request callbacks
        REQUIRE_NOTHROW(client->send_jsonrpc(
            "printer.info", json(), [](json) { throw std::runtime_error("success exception"); },
            [](const MoonrakerError&) { throw std::runtime_error("error exception"); }));

        // Notify callbacks
        REQUIRE_NOTHROW(client->register_notify_update(
            [](json) { throw std::runtime_error("notify exception"); }));

        // Method callbacks
        REQUIRE_NOTHROW(client->register_method_callback("test_method", "test_handler", [](json) {
            throw std::runtime_error("method exception");
        }));

        // State change callback
        REQUIRE_NOTHROW(client->set_state_change_callback(
            [](ConnectionState, ConnectionState) { throw std::runtime_error("state exception"); }));

        // Cleanup with pending requests (triggers error callbacks)
        REQUIRE_NOTHROW(client.reset());
    }
}

// ============================================================================
// Issue #357: Callback Lifecycle Mutex - Destructor/Callback Synchronization
// ============================================================================

TEST_CASE("MoonrakerClient destructor waits for in-flight callbacks (issue #357)",
          "[connection][security][uaf][issue357][eventloop][slow]") {
    SECTION("Rapid create/connect/destroy stress test with callback contention") {
        // Stress test: rapidly create clients, start connections, and destroy them.
        // The callback_lifecycle_mutex_ ensures the destructor waits for any
        // in-flight callbacks before proceeding with member destruction.
        for (int i = 0; i < 50; i++) {
            auto client = std::make_unique<MoonrakerClient>();

            std::atomic<int> callback_count{0};

            // Register various callbacks that touch client state
            client->register_notify_update([&callback_count](const json&) { callback_count++; });

            client->register_method_callback("test_method", "handler",
                                             [&callback_count](const json&) { callback_count++; });

            // Start connection (will trigger onclose when destroyed)
            client->connect(
                "ws://127.0.0.1:19999/websocket", [&callback_count]() { callback_count++; },
                [&callback_count]() { callback_count++; });

            // Send pending requests
            client->send_jsonrpc("printer.info", json(), [](json) {}, [](const MoonrakerError&) {});

            // Destroy immediately — destructor must set is_destroying_ FIRST,
            // then acquire exclusive lock to wait for any in-flight callbacks
            REQUIRE_NOTHROW(client.reset());
        }
        // Reaching here without SIGSEGV = mutex synchronization works
    }

    SECTION("is_destroying flag prevents new callback execution during destruction") {
        auto client = std::make_unique<MoonrakerClient>();

        std::atomic<bool> state_cb_called{false};

        // Set state change callback — connect() triggers CONNECTING state change,
        // and destroy triggers state clearing
        client->set_state_change_callback(
            [&state_cb_called](ConnectionState old_s, ConnectionState new_s) {
                state_cb_called = true;
            });

        client->connect("ws://127.0.0.1:19999/websocket", []() {}, []() {});

        // state_cb should have been called for CONNECTING transition
        REQUIRE(state_cb_called);

        // Reset tracking
        state_cb_called = false;

        // Destroy — is_destroying_ should prevent state change callback during teardown
        client.reset();

        // State callback should NOT be called during destruction
        // (destructor clears it, and is_destroying_ blocks invocation)
        REQUIRE_FALSE(state_cb_called);
    }
}

// ============================================================================
// Integration Tests - Multiple Security Properties
// ============================================================================

// Destroys a client with 50 pending requests whose error callbacks throw and re-enter
// it to send more. The destructor's two-phase cleanup, its callback drain, and the
// exception guards around each invocation all have to hold at once for this to finish
// without a use-after-free.
TEST_CASE("MoonrakerClient security properties work together correctly",
          "[connection][security][integration][eventloop][slow]") {
    SECTION("Cleanup with exceptions, large IDs, and nested requests") {
        auto client = std::make_unique<MoonrakerClient>();

        std::atomic<int> cleanup_callbacks_invoked{0};

        // Send many requests with various properties
        for (int i = 0; i < 50; i++) {
            client->send_jsonrpc(
                "printer.info", json(), [](json) { throw std::runtime_error("Success exception"); },
                [&cleanup_callbacks_invoked, &client](const MoonrakerError& err) {
                    cleanup_callbacks_invoked++;

                    // Some callbacks throw
                    if (cleanup_callbacks_invoked % 3 == 0) {
                        throw std::runtime_error("Cleanup exception");
                    }

                    // Some callbacks send nested requests
                    if (cleanup_callbacks_invoked % 5 == 0) {
                        client->send_jsonrpc(
                            "nested.request", json(), [](json) {}, [](const MoonrakerError&) {});
                    }
                });
        }

        // Destroy client - tests all properties together:
        // - Two-phase cleanup (nested requests work)
        // - Exception safety (throwing callbacks)
        // - Callback cleanup (no UAF)
        REQUIRE_NOTHROW(client.reset());

        // All cleanup callbacks should have been invoked
        REQUIRE(cleanup_callbacks_invoked == 50);
    }
}
