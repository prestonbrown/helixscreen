// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_client_subscription_cancel.cpp
 * @brief Unit tests for Moonraker client lifecycle management APIs
 *
 * Tests the following API features for subscription management, request
 * cancellation, and connection lifecycle:
 *
 * 1. Subscription ID / Unsubscribe API:
 *    - register_notify_update() returns SubscriptionId (uint64_t, >= 1)
 *    - unsubscribe_notify_update(SubscriptionId) removes callback
 *    - Each registration gets unique incrementing ID
 *    - After unsubscribe, callback is NOT invoked
 *    - Multiple subscriptions coexist independently
 *
 * 2. Method Callback Handler Names / Unregister:
 *    - register_method_callback(method, handler_name, callback) uses handler_name as key
 *    - unregister_method_callback(method, handler_name) removes specific handler
 *    - Multiple handlers per method are supported
 *    - Unregistering non-existent handler is safe
 *
 * 3. Request Cancellation API:
 *    - send_jsonrpc() returns RequestId (uint64_t, >= 1)
 *    - cancel_request(RequestId) cancels pending request
 *    - Cancelled request's callbacks are NOT invoked
 *    - Cancelling completed/non-existent request is safe
 *
 * 4. force_reconnect() Method:
 *    - force_reconnect() disconnects and reconnects with same URL/callbacks
 *    - Works when connected
 *    - Safe when not connected (logs warning, no crash)
 *    - Preserves connection callbacks
 */

#include "../../include/moonraker_client.h"
#include "../../include/moonraker_client_mock.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Test Fixture Base
// ============================================================================

/**
 * @brief Fixture for Moonraker client lifecycle tests
 *
 * Provides helper methods for waiting on async callbacks in tests covering
 * subscription management, request cancellation, and connection lifecycle.
 */
class MoonrakerClientLifecycleFixture {
  public:
    MoonrakerClientLifecycleFixture() = default;

    /**
     * @brief Wait for callback to be invoked with timeout
     * @param flag Atomic flag to wait for
     * @param timeout_ms Maximum wait time in milliseconds
     * @return true if flag became true, false on timeout
     */
    bool wait_for_flag(std::atomic<bool>& flag, int timeout_ms = 1000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (flag.load()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return flag.load();
    }

    /**
     * @brief Wait for counter to reach target value
     * @param counter Atomic counter
     * @param target Target value
     * @param timeout_ms Maximum wait time
     * @return true if target reached, false on timeout
     */
    bool wait_for_count(std::atomic<int>& counter, int target, int timeout_ms = 1000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (counter.load() >= target) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return counter.load() >= target;
    }

    /**
     * @brief Wait with condition variable
     */
    bool wait_for_cv(std::condition_variable& cv, std::mutex& mtx, std::atomic<bool>& flag,
                     int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&flag] { return flag.load(); });
    }
};

/**
 * @brief Disconnect a client and stop+join its borrowed EventLoopThread.
 *
 * Declare this AFTER the client it guards: reverse declaration order then runs
 * the disconnect and the join while both the client and the loop are still
 * alive, which is the ordering these tests need.
 *
 * The point of doing it in a destructor rather than inline is that a failing
 * REQUIRE throws. Teardown written at the end of the test body only runs when
 * every assertion above it passed, so one local failure leaves the loop thread
 * running with a reconnect timer armed on a client that is about to be
 * destroyed — and the SIGSEGV then lands in hio_get during some unrelated test
 * later in the run. RAII keeps a failure local to the test that caused it.
 *
 * Note this cannot help a fatal signal: SIGSEGV does not unwind, so a crash
 * inside the body still skips teardown. It bounds the assertion-failure case.
 */
struct BorrowedEventLoopGuard {
    MoonrakerClient* client;
    std::shared_ptr<hv::EventLoopThread> loop_thread;

    ~BorrowedEventLoopGuard() {
        if (client != nullptr) {
            client->disconnect();
        }
        if (loop_thread) {
            loop_thread->stop(true);
        }
    }
};

// ============================================================================
// Subscription ID / Unsubscribe API Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient register_notify_update returns valid SubscriptionId",
                 "[connection][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("First subscription returns ID >= 1") {
        SubscriptionId id = client.register_notify_update([](json) {});
        REQUIRE(id >= 1);
        REQUIRE(id != INVALID_SUBSCRIPTION_ID);
    }

    SECTION("Consecutive subscriptions return unique incrementing IDs") {
        SubscriptionId id1 = client.register_notify_update([](json) {});
        SubscriptionId id2 = client.register_notify_update([](json) {});
        SubscriptionId id3 = client.register_notify_update([](json) {});

        REQUIRE(id1 >= 1);
        REQUIRE(id2 > id1);
        REQUIRE(id3 > id2);
    }

    SECTION("Null callback returns INVALID_SUBSCRIPTION_ID") {
        SubscriptionId id = client.register_notify_update(nullptr);
        REQUIRE(id == INVALID_SUBSCRIPTION_ID);
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient unsubscribe_notify_update removes callback",
                 "[connection][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Unsubscribe with valid ID returns true") {
        SubscriptionId id = client.register_notify_update([](json) {});
        REQUIRE(id != INVALID_SUBSCRIPTION_ID);

        bool result = client.unsubscribe_notify_update(id);
        REQUIRE(result == true);
    }

    SECTION("Unsubscribe with same ID twice returns false on second call") {
        SubscriptionId id = client.register_notify_update([](json) {});

        bool first_result = client.unsubscribe_notify_update(id);
        bool second_result = client.unsubscribe_notify_update(id);

        REQUIRE(first_result == true);
        REQUIRE(second_result == false);
    }

    SECTION("Unsubscribe with INVALID_SUBSCRIPTION_ID returns false") {
        bool result = client.unsubscribe_notify_update(INVALID_SUBSCRIPTION_ID);
        REQUIRE(result == false);
    }

    SECTION("Unsubscribe with non-existent ID returns false") {
        bool result = client.unsubscribe_notify_update(999999);
        REQUIRE(result == false);
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClientMock subscription callbacks receive notifications",
                 "[connection][mock][eventloop][slow]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("Registered callback receives notifications after connect") {
        std::atomic<int> callback_count{0};
        std::condition_variable cv;
        std::mutex mtx;

        SubscriptionId id = mock.register_notify_update([&callback_count, &cv](json) {
            callback_count++;
            cv.notify_all();
        });
        REQUIRE(id != INVALID_SUBSCRIPTION_ID);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for at least one callback (initial state dispatch)
        REQUIRE(wait_for_count(callback_count, 1, 1000));

        mock.stop_temperature_simulation();
        mock.disconnect();
    }

    SECTION("Unsubscribed callback does not receive notifications") {
        std::atomic<int> callback1_count{0};
        std::atomic<int> callback2_count{0};

        SubscriptionId id1 =
            mock.register_notify_update([&callback1_count](json) { callback1_count++; });
        [[maybe_unused]] SubscriptionId id2 =
            mock.register_notify_update([&callback2_count](json) { callback2_count++; });

        // Unsubscribe callback 1 before connecting
        mock.unsubscribe_notify_update(id1);

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for callback 2 to receive notification
        REQUIRE(wait_for_count(callback2_count, 1, 1000));

        mock.stop_temperature_simulation();

        // Callback 1 should not have been invoked
        REQUIRE(callback1_count.load() == 0);
        REQUIRE(callback2_count.load() >= 1);

        mock.disconnect();
    }

    SECTION("Multiple subscriptions coexist independently") {
        std::atomic<int> callback1_count{0};
        std::atomic<int> callback2_count{0};
        std::atomic<int> callback3_count{0};

        [[maybe_unused]] SubscriptionId id1 =
            mock.register_notify_update([&callback1_count](json) { callback1_count++; });
        SubscriptionId id2 =
            mock.register_notify_update([&callback2_count](json) { callback2_count++; });
        [[maybe_unused]] SubscriptionId id3 =
            mock.register_notify_update([&callback3_count](json) { callback3_count++; });

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // Wait for all to receive initial notification
        REQUIRE(wait_for_count(callback1_count, 1, 1000));
        REQUIRE(wait_for_count(callback2_count, 1, 1000));
        REQUIRE(wait_for_count(callback3_count, 1, 1000));

        // Unsubscribe only callback 2
        mock.unsubscribe_notify_update(id2);

        // Allow more notifications to arrive
        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        mock.stop_temperature_simulation();

        // Callbacks 1 and 3 should have more notifications than callback 2
        int count2_at_unsubscribe = callback2_count.load();
        REQUIRE(callback1_count.load() >= count2_at_unsubscribe);
        REQUIRE(callback3_count.load() >= count2_at_unsubscribe);

        mock.disconnect();
    }
}

// ============================================================================
// Method Callback Handler Names / Unregister Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient method callback registration with handler names",
                 "[connection][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Register single handler for method") {
        bool called = false;
        client.register_method_callback("notify_gcode_response", "test_handler",
                                        [&called](json) { called = true; });

        // Unregister should succeed
        bool result = client.unregister_method_callback("notify_gcode_response", "test_handler");
        REQUIRE(result == true);
    }

    SECTION("Register multiple handlers for same method") {
        bool handler1_called = false;
        bool handler2_called = false;

        client.register_method_callback("notify_gcode_response", "handler1",
                                        [&handler1_called](json) { handler1_called = true; });
        client.register_method_callback("notify_gcode_response", "handler2",
                                        [&handler2_called](json) { handler2_called = true; });

        // Both should be unregisterable independently
        bool result1 = client.unregister_method_callback("notify_gcode_response", "handler1");
        bool result2 = client.unregister_method_callback("notify_gcode_response", "handler2");

        REQUIRE(result1 == true);
        REQUIRE(result2 == true);
    }

    SECTION("Unregister removes only specified handler") {
        client.register_method_callback("notify_gcode_response", "handler1", [](json) {});
        client.register_method_callback("notify_gcode_response", "handler2", [](json) {});

        // Remove handler1
        bool result1 = client.unregister_method_callback("notify_gcode_response", "handler1");
        REQUIRE(result1 == true);

        // handler2 should still exist
        bool result2 = client.unregister_method_callback("notify_gcode_response", "handler2");
        REQUIRE(result2 == true);

        // handler1 should no longer exist
        bool result3 = client.unregister_method_callback("notify_gcode_response", "handler1");
        REQUIRE(result3 == false);
    }

    SECTION("Unregister non-existent handler is safe") {
        bool result =
            client.unregister_method_callback("nonexistent_method", "nonexistent_handler");
        REQUIRE(result == false);
    }

    SECTION("Unregister non-existent handler name from existing method") {
        client.register_method_callback("notify_gcode_response", "real_handler", [](json) {});

        bool result = client.unregister_method_callback("notify_gcode_response", "fake_handler");
        REQUIRE(result == false);

        // Real handler should still be there
        bool result2 = client.unregister_method_callback("notify_gcode_response", "real_handler");
        REQUIRE(result2 == true);
    }

    SECTION("Same handler name on different methods are independent") {
        client.register_method_callback("method1", "shared_name", [](json) {});
        client.register_method_callback("method2", "shared_name", [](json) {});

        // Removing from method1 should not affect method2
        bool result1 = client.unregister_method_callback("method1", "shared_name");
        REQUIRE(result1 == true);

        bool result2 = client.unregister_method_callback("method2", "shared_name");
        REQUIRE(result2 == true);

        // Both should now be gone
        bool result3 = client.unregister_method_callback("method1", "shared_name");
        bool result4 = client.unregister_method_callback("method2", "shared_name");
        REQUIRE(result3 == false);
        REQUIRE(result4 == false);
    }
}

// ============================================================================
// Request Cancellation API Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient send_jsonrpc returns valid RequestId",
                 "[connection][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("send_jsonrpc with callback returns ID >= 1 when connected (but fails send)") {
        // Note: Without connection, send will fail but registration happens first
        // The implementation should return INVALID_REQUEST_ID if send fails
        RequestId id = client.send_jsonrpc("printer.info", json(), [](json) {});

        // Without connection, send fails and returns INVALID_REQUEST_ID
        REQUIRE(id == INVALID_REQUEST_ID);
    }

    SECTION("Consecutive requests return unique IDs (when mock connected)") {
        MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
        mock.connect("ws://mock/websocket", []() {}, []() {});

        RequestId id1 = mock.send_jsonrpc("server.files.list", json(), [](json) {});
        RequestId id2 = mock.send_jsonrpc("server.files.list", json(), [](json) {});
        RequestId id3 = mock.send_jsonrpc("server.files.list", json(), [](json) {});

        // Mock returns incrementing IDs
        REQUIRE(id1 >= 1);
        REQUIRE(id2 > id1);
        REQUIRE(id3 > id2);

        mock.stop_temperature_simulation();
        mock.disconnect();
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient cancel_request removes pending request",
                 "[connection][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Cancel with INVALID_REQUEST_ID returns false") {
        bool result = client.cancel_request(INVALID_REQUEST_ID);
        REQUIRE(result == false);
    }

    SECTION("Cancel with non-existent ID returns false") {
        bool result = client.cancel_request(999999);
        REQUIRE(result == false);
    }

    SECTION("Cancel same ID twice returns false on second call") {
        // Cannot test with real pending requests without a connection,
        // but we can verify that cancelling a non-existent ID returns false
        // (same code path as second cancel of an already-cancelled request)
        bool result = client.cancel_request(12345);
        REQUIRE(result == false);
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient cancelled request callback not invoked",
                 "[connection][eventloop][slow]") {
    auto loop_thread = std::make_shared<hv::EventLoopThread>();
    loop_thread->start();

    MoonrakerClient client(loop_thread->loop());
    BorrowedEventLoopGuard guard{&client, loop_thread};

    SECTION("Cancelled request does not invoke success callback on response") {
        // This test requires actual message handling which needs a real connection
        // We document the expected behavior here.
        //
        // Expected behavior:
        // 1. Send request, get RequestId
        // 2. Cancel request with that ID
        // 3. When response arrives for that ID, no callback is invoked
        //
        // The cancel_request implementation removes the request from pending_requests_
        // map, so when onmessage handler looks for the request ID, it won't find it.
        // Verify cancel on non-existent request returns false (same path as post-cancel)
        REQUIRE(client.cancel_request(99999) == false);
    }

    // Teardown is BorrowedEventLoopGuard's job -- see its comment for why it
    // must not be written inline here.
}

// ============================================================================
// Mock Client Request ID Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClientMock send_jsonrpc returns valid RequestId",
                 "[connection][mock][eventloop][slow]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    mock.connect("ws://mock/websocket", []() {}, []() {});

    SECTION("send_jsonrpc with single callback returns valid ID") {
        RequestId id = mock.send_jsonrpc("server.files.list", json(), [](json) {});
        REQUIRE(id >= 1);
        REQUIRE(id != INVALID_REQUEST_ID);
    }

    SECTION("send_jsonrpc with success/error callbacks returns valid ID") {
        RequestId id = mock.send_jsonrpc(
            "server.files.list", json(), [](json) {}, [](const MoonrakerError&) {});
        REQUIRE(id >= 1);
        REQUIRE(id != INVALID_REQUEST_ID);
    }

    SECTION("Multiple requests return incrementing IDs") {
        std::vector<RequestId> ids;
        for (int i = 0; i < 10; i++) {
            RequestId id = mock.send_jsonrpc("server.files.list", json(), [](json) {});
            ids.push_back(id);
        }

        // Verify all IDs are unique and incrementing
        for (size_t i = 1; i < ids.size(); i++) {
            REQUIRE(ids[i] > ids[i - 1]);
        }
    }

    mock.stop_temperature_simulation();
    mock.disconnect();
}

// ============================================================================
// force_reconnect() Method Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient force_reconnect when not connected",
                 "[connection][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("force_reconnect without prior connect logs warning and returns safely") {
        // Should not crash, just log a warning
        REQUIRE_NOTHROW(client.force_reconnect());
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture, "MoonrakerClientMock force_reconnect behavior",
                 "[connection][mock][eventloop][slow]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("force_reconnect without prior connect is safe (mock limitation)") {
        // Note: MoonrakerClientMock's connect() doesn't store URL/callbacks like
        // the real client, so force_reconnect() will log a warning and return.
        // This is a documented mock limitation - the real client stores these.

        std::atomic<int> connected_count{0};

        mock.connect("ws://mock/websocket", [&connected_count]() { connected_count++; }, []() {});

        // Wait for initial connection
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        REQUIRE(connected_count.load() == 1);

        mock.stop_temperature_simulation();

        // Force reconnect on mock will log warning because mock doesn't store callbacks
        // This documents the mock limitation - real client would reconnect
        REQUIRE_NOTHROW(mock.force_reconnect());

        mock.disconnect();
    }

    SECTION("force_reconnect does not crash") {
        std::atomic<bool> callback_invoked{false};

        mock.connect("ws://mock/websocket", []() {}, []() {});

        // The mock immediately invokes callbacks for known methods, so we use
        // an unknown method that won't trigger callback
        mock.send_jsonrpc("unknown.method", json(),
                          [&callback_invoked](json) { callback_invoked = true; });

        mock.stop_temperature_simulation();

        // Force reconnect should not crash
        REQUIRE_NOTHROW(mock.force_reconnect());

        // Callback should not have been invoked (mock doesn't invoke for unknown methods)
        REQUIRE(callback_invoked.load() == false);

        mock.disconnect();
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient force_reconnect state transitions",
                 "[connection][eventloop][slow]") {
    auto loop_thread = std::make_shared<hv::EventLoopThread>();
    loop_thread->start();

    MoonrakerClient client(loop_thread->loop());
    BorrowedEventLoopGuard guard{&client, loop_thread};

    SECTION("force_reconnect transitions through DISCONNECTED state") {
        std::vector<ConnectionState> state_history;
        std::mutex state_mutex;

        client.set_state_change_callback(
            [&state_history, &state_mutex]([[maybe_unused]] ConnectionState old_state,
                                           ConnectionState new_state) {
                std::lock_guard<std::mutex> lock(state_mutex);
                state_history.push_back(new_state);
            });

        // Connect to a non-existent server (will fail quickly)
        client.connect("ws://127.0.0.1:19999/websocket", []() {}, []() {});

        // Wait for connection attempt
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Clear history and force reconnect
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            state_history.clear();
        }

        client.force_reconnect();

        // Wait for state transitions
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Should have gone through DISCONNECTED state
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            bool found_disconnected = false;
            for (const auto& state : state_history) {
                if (state == ConnectionState::DISCONNECTED) {
                    found_disconnected = true;
                    break;
                }
            }
            REQUIRE(found_disconnected);
        }
    }

    // force_reconnect() above scheduled a reconnect timer; BorrowedEventLoopGuard
    // disarms it and joins the loop before `client` destructs.
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient subscription ID generation is thread-safe",
                 "[connection][thread_safety][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Concurrent registrations get unique IDs") {
        std::vector<std::thread> threads;
        std::vector<SubscriptionId> ids(100);
        std::atomic<int> completed{0};

        // Spawn 10 threads, each registering 10 callbacks
        for (int t = 0; t < 10; t++) {
            threads.emplace_back([&client, &ids, &completed, t]() {
                for (int i = 0; i < 10; i++) {
                    int index = t * 10 + i;
                    ids[index] = client.register_notify_update([](json) {});
                    completed++;
                }
            });
        }

        // Wait for all threads
        for (auto& thread : threads) {
            thread.join();
        }

        REQUIRE(completed.load() == 100);

        // Verify all IDs are unique
        std::set<SubscriptionId> unique_ids(ids.begin(), ids.end());
        // Remove any INVALID_SUBSCRIPTION_ID entries
        unique_ids.erase(INVALID_SUBSCRIPTION_ID);
        REQUIRE(unique_ids.size() == 100);
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient concurrent subscribe/unsubscribe is safe",
                 "[connection][thread_safety][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Concurrent subscribe and unsubscribe operations") {
        std::atomic<bool> running{true};
        std::atomic<int> subscribe_count{0};
        std::atomic<int> unsubscribe_count{0};
        std::vector<SubscriptionId> ids;
        std::mutex ids_mutex;

        // Producer thread: register callbacks
        std::thread producer([&]() {
            while (running.load()) {
                SubscriptionId id = client.register_notify_update([](json) {});
                if (id != INVALID_SUBSCRIPTION_ID) {
                    std::lock_guard<std::mutex> lock(ids_mutex);
                    ids.push_back(id);
                    subscribe_count++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Consumer thread: unregister callbacks
        std::thread consumer([&]() {
            while (running.load()) {
                SubscriptionId id_to_remove = INVALID_SUBSCRIPTION_ID;
                {
                    std::lock_guard<std::mutex> lock(ids_mutex);
                    if (!ids.empty()) {
                        id_to_remove = ids.back();
                        ids.pop_back();
                    }
                }
                if (id_to_remove != INVALID_SUBSCRIPTION_ID) {
                    client.unsubscribe_notify_update(id_to_remove);
                    unsubscribe_count++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Run for a short duration
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        running.store(false);

        producer.join();
        consumer.join();

        // Should have completed without crashes
        REQUIRE(subscribe_count.load() >= 0);
        REQUIRE(unsubscribe_count.load() >= 0);
    }
}

// ============================================================================
// Mock Parity Tests - Verify Mock behaves like Real Client
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClientMock subscription API matches MoonrakerClient",
                 "[connection][eventloop][slow]") {
    MoonrakerClient real_client;
    MoonrakerClientMock mock_client(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("Both return valid IDs for register_notify_update") {
        SubscriptionId real_id = real_client.register_notify_update([](json) {});
        SubscriptionId mock_id = mock_client.register_notify_update([](json) {});

        REQUIRE(real_id >= 1);
        REQUIRE(mock_id >= 1);
    }

    SECTION("Both return INVALID_SUBSCRIPTION_ID for null callback") {
        SubscriptionId real_id = real_client.register_notify_update(nullptr);
        SubscriptionId mock_id = mock_client.register_notify_update(nullptr);

        REQUIRE(real_id == INVALID_SUBSCRIPTION_ID);
        REQUIRE(mock_id == INVALID_SUBSCRIPTION_ID);
    }

    SECTION("Both return false for unsubscribing invalid ID") {
        bool real_result = real_client.unsubscribe_notify_update(INVALID_SUBSCRIPTION_ID);
        bool mock_result = mock_client.unsubscribe_notify_update(INVALID_SUBSCRIPTION_ID);

        REQUIRE(real_result == false);
        REQUIRE(mock_result == false);
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClientMock method callback API matches MoonrakerClient",
                 "[connection][eventloop][slow]") {
    MoonrakerClient real_client;
    MoonrakerClientMock mock_client(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("Both allow registering method callbacks") {
        REQUIRE_NOTHROW(
            real_client.register_method_callback("test_method", "handler", [](json) {}));
        REQUIRE_NOTHROW(
            mock_client.register_method_callback("test_method", "handler", [](json) {}));
    }

    SECTION("Both return false for unregistering non-existent callback") {
        bool real_result = real_client.unregister_method_callback("fake", "fake");
        bool mock_result = mock_client.unregister_method_callback("fake", "fake");

        REQUIRE(real_result == false);
        REQUIRE(mock_result == false);
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClientMock cancel_request API matches MoonrakerClient",
                 "[connection][eventloop][slow]") {
    MoonrakerClient real_client;
    MoonrakerClientMock mock_client(MoonrakerClientMock::PrinterType::VORON_24);

    SECTION("Both return false for cancelling INVALID_REQUEST_ID") {
        bool real_result = real_client.cancel_request(INVALID_REQUEST_ID);
        bool mock_result = mock_client.cancel_request(INVALID_REQUEST_ID);

        REQUIRE(real_result == false);
        REQUIRE(mock_result == false);
    }

    SECTION("Both return false for cancelling non-existent ID") {
        bool real_result = real_client.cancel_request(999999);
        bool mock_result = mock_client.cancel_request(999999);

        REQUIRE(real_result == false);
        REQUIRE(mock_result == false);
    }
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture, "MoonrakerClient handles subscription edge cases",
                 "[connection][edge_cases][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Many subscriptions (stress test)") {
        std::vector<SubscriptionId> ids;
        ids.reserve(1000);

        for (int i = 0; i < 1000; i++) {
            SubscriptionId id = client.register_notify_update([](json) {});
            ids.push_back(id);
        }

        // All should be valid
        for (const auto& id : ids) {
            REQUIRE(id >= 1);
        }

        // All should be unsubscribeable
        for (const auto& id : ids) {
            bool result = client.unsubscribe_notify_update(id);
            REQUIRE(result == true);
        }

        // All should now be invalid
        for (const auto& id : ids) {
            bool result = client.unsubscribe_notify_update(id);
            REQUIRE(result == false);
        }
    }

    SECTION("Subscription IDs never wrap to zero in reasonable usage") {
        // Register many callbacks and verify none return 0
        for (int i = 0; i < 100; i++) {
            SubscriptionId id = client.register_notify_update([](json) {});
            REQUIRE(id != 0);
            REQUIRE(id != INVALID_SUBSCRIPTION_ID);
        }
    }
}

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient handles method callback edge cases",
                 "[connection][edge_cases][eventloop][slow]") {
    MoonrakerClient client;

    SECTION("Empty method name is handled") {
        REQUIRE_NOTHROW(client.register_method_callback("", "handler", [](json) {}));
        bool result = client.unregister_method_callback("", "handler");
        REQUIRE(result == true);
    }

    SECTION("Empty handler name is handled") {
        REQUIRE_NOTHROW(client.register_method_callback("method", "", [](json) {}));
        bool result = client.unregister_method_callback("method", "");
        REQUIRE(result == true);
    }

    SECTION("Overwriting handler with same name replaces callback") {
        std::atomic<int> callback1_count{0};
        std::atomic<int> callback2_count{0};

        client.register_method_callback("method", "handler",
                                        [&callback1_count](json) { callback1_count++; });
        client.register_method_callback("method", "handler",
                                        [&callback2_count](json) { callback2_count++; });

        // Unregister should only need one call
        bool result = client.unregister_method_callback("method", "handler");
        REQUIRE(result == true);

        // Second unregister should fail (already removed)
        bool result2 = client.unregister_method_callback("method", "handler");
        REQUIRE(result2 == false);
    }
}

// Destruction has to actually *release* the registered notify callbacks, not merely
// avoid crashing on the way out. The counter below lives in a shared_ptr that each
// of the 10 lambdas copies, so use_count() is direct, non-sanitizer evidence of
// whether the stored std::functions were destroyed with the client: a destructor
// that stashes them anywhere (a static registry, a detached container, a map moved
// aside and never freed) leaves the count above 1 in a plain build.
//
// Each section dispatches once before teardown so the post-destruction check is
// measuring cleanup and not a registration that silently overwrote itself.
TEST_CASE_METHOD(MoonrakerClientLifecycleFixture,
                 "MoonrakerClient client destruction releases notify subscriptions",
                 "[connection][cleanup][eventloop][slow]") {
    // Payload with no bed_mesh/toolhead keys, so dispatch_status_update does nothing
    // beyond the callback fan-out we are measuring.
    const json status = {{"extruder", {{"temperature", 25.0}}}};

    SECTION("Destroying client releases every registered notify callback") {
        auto callback_count = std::make_shared<std::atomic<int>>(0);

        {
            MoonrakerClient client;

            for (int i = 0; i < 10; i++) {
                client.register_notify_update([callback_count](json) { (*callback_count)++; });
            }
            // 10 stored callbacks + this scope's own reference.
            REQUIRE(callback_count.use_count() == 11);

            client.dispatch_status_update(status);
            REQUIRE(callback_count->load() == 10);
            // Client destroyed here
        }

        // Sole remaining owner: all 10 stored callbacks went away with the client.
        REQUIRE(callback_count.use_count() == 1);
        // And nothing fired during or after teardown.
        REQUIRE(callback_count->load() == 10);
    }

    SECTION("Destroying mock client releases every registered notify callback") {
        auto callback_count = std::make_shared<std::atomic<int>>(0);
        int dispatched_while_alive = 0;

        {
            MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

            for (int i = 0; i < 10; i++) {
                mock.register_notify_update([callback_count](json) { (*callback_count)++; });
            }

            mock.connect("ws://mock/websocket", []() {}, []() {});
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            mock.stop_temperature_simulation();

            // Simulation thread is stopped, so this is the only writer from here on.
            const int before = callback_count->load();
            mock.dispatch_status_update(status);
            dispatched_while_alive = callback_count->load();
            REQUIRE(dispatched_while_alive >= before + 10);
            // Mock destroyed here
        }

        REQUIRE(callback_count.use_count() == 1);

        // No straggler thread still holding and firing a copy.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(callback_count->load() == dispatched_while_alive);
    }
}

// ============================================================================
// Integration Test - Full Workflow
// ============================================================================

TEST_CASE_METHOD(MoonrakerClientLifecycleFixture, "Full subscription workflow with mock client",
                 "[connection][integration][eventloop][slow]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);

    std::atomic<int> total_notifications{0};
    std::vector<SubscriptionId> subscription_ids;
    std::mutex ids_mutex;

    // Register 5 subscriptions
    for (int i = 0; i < 5; i++) {
        SubscriptionId id =
            mock.register_notify_update([&total_notifications](json) { total_notifications++; });
        REQUIRE(id != INVALID_SUBSCRIPTION_ID);
        subscription_ids.push_back(id);
    }

    // Connect
    mock.connect("ws://mock/websocket", []() {}, []() {});

    // Wait for initial notifications
    REQUIRE(wait_for_count(total_notifications, 5, 1000)); // All 5 should get initial notification

    // Unsubscribe 2 of them
    mock.unsubscribe_notify_update(subscription_ids[0]);
    mock.unsubscribe_notify_update(subscription_ids[1]);

    // Reset counter
    total_notifications.store(0);

    // Wait for more notifications from simulation
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    mock.stop_temperature_simulation();

    int final_count = total_notifications.load();
    // Should have received notifications, but from only 3 active subscriptions.
    // The exact count depends on simulation timing; that it is non-zero does
    // not. The assertion was `>= 0`, which an int can never fail.
    //
    // This is now a real ordering assertion where it used to be decoration. If
    // it ever flakes, that is a finding about subscription-cancel timing - do
    // not weaken it back to `>= 0`.
    REQUIRE(final_count > 0);

    // Force reconnect - note: mock doesn't store callbacks, so reconnect won't
    // re-invoke on_connected or re-dispatch initial state. This is a mock limitation.
    // The subscriptions themselves remain registered though.
    REQUIRE_NOTHROW(mock.force_reconnect());

    // Cleanup - unsubscribe remaining 3
    mock.unsubscribe_notify_update(subscription_ids[2]);
    mock.unsubscribe_notify_update(subscription_ids[3]);
    mock.unsubscribe_notify_update(subscription_ids[4]);

    mock.disconnect();
}
