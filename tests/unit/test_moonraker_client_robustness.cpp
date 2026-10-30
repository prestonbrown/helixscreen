// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_client.h"
#include "../../include/moonraker_error.h"
#include "../mocks/mock_websocket_server.h"
#include "hv/EventLoopThread.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
/**
 * MoonrakerClient robustness: a throwing notify callback cannot take down
 * dispatch_status_update, and a dial to an unroutable address (TEST-NET-1)
 * walks CONNECTING -> DISCONNECTED once its connect timeout expires.
 */

using namespace std::chrono;

namespace {

/// Connect timeout for dials to TEST-NET-1 (192.0.2.1), which never answers.
constexpr uint32_t kBlackHoleConnectTimeoutMs = 200;

} // namespace

// ============================================================================
// Test Fixture
// ============================================================================

class MoonrakerRobustnessFixture {
  public:
    MoonrakerRobustnessFixture() {
        // Start mock WebSocket server on a random port (port 0 = OS-assigned).
        // Must use random ports because leaked event loop threads from previous
        // test runs may still hold the old port open.
        server_ = std::make_unique<MockWebSocketServer>();
        server_->on_method("printer.info", [](const json&) {
            return json{{"state", "ready"}, {"hostname", "test-printer"}};
        });
        int port = server_->start(0);
        if (port <= 0) {
            throw std::runtime_error("Failed to start mock server");
        }

        // Create event loop and client
        loop_thread_ = std::make_shared<hv::EventLoopThread>();
        loop_thread_->start();

        client_ = std::make_unique<MoonrakerClient>(loop_thread_->loop());

        // Configure for testing
        client_->set_connection_timeout(2000);      // 2s timeout
        client_->set_default_request_timeout(2000); // 2s timeout
        client_->setReconnect(nullptr);             // Disable auto-reconnect
    }

    ~MoonrakerRobustnessFixture() {
        client_->disconnect();

        // Stop the event loop BEFORE destroying the client.  disconnect()
        // calls close() which may schedule an onclose callback on the event
        // loop thread.  Destroying the client while the thread is still
        // running races on the std::function callback members → SIGSEGV.
        //
        // Note: [eventloop] tests are excluded from macOS CI (kqueue hangs
        // in join() after WebSocket I/O), so stop+join is safe here.
        loop_thread_->stop();
        loop_thread_->join();

        client_.reset();
        server_->stop();
        server_.reset();
    }

    std::string server_url() const {
        return server_->url();
    }

    std::unique_ptr<MockWebSocketServer> server_;
    std::shared_ptr<hv::EventLoopThread> loop_thread_;
    std::unique_ptr<MoonrakerClient> client_;
};

// ============================================================================
// Subscription-restricted-null safety
// ============================================================================
//
// Background: Moonraker delivers JSON null for subscribed fields the underlying
// Klipper object lacks. nlohmann::json::value("k", default) and unguarded
// .get<T>() throw type_error.302 on null. dispatch_status_update is the
// initial-subscription apply path that runs synchronously on the main thread
// (Application::on_discovery_complete), where a throwing callback would unwind
// into main()'s top-level catch and exit 134. The dispatch path wraps each
// callback in try/catch so a single rogue parser cannot crash the app.

TEST_CASE("MoonrakerClient::dispatch_status_update absorbs throwing callbacks",
          "[connection][regression][subscription-null]") {
    // Build a client without connecting — dispatch_status_update is callable
    // standalone since it just walks the registered notify callbacks.
    MoonrakerClient client;

    int good_calls = 0;
    bool bad_call_attempted = false;

    // First callback throws (simulates a parser hitting JSON null)
    client.register_notify_update([&bad_call_attempted](const json& /*notif*/) {
        bad_call_attempted = true;
        throw nlohmann::json::type_error::create(302, "type must be number, but is null", nullptr);
    });
    // Second callback is well-behaved — must still fire after the first throws
    client.register_notify_update([&good_calls](const json& /*notif*/) { ++good_calls; });

    // dispatch_status_update wraps a status payload as notify_status_update
    // and walks the notify_callbacks_ map. Pre-fix: the throw escaped this
    // loop and propagated up. Post-fix: caught and logged, second cb fires.
    json status = {{"some_object", {{"some_field", nullptr}}}};
    REQUIRE_NOTHROW(client.dispatch_status_update(status));
    REQUIRE(bad_call_attempted);
    REQUIRE(good_calls == 1);
}

// ============================================================================
// Connection State Transitions
// ============================================================================

TEST_CASE_METHOD(MoonrakerRobustnessFixture, "MoonrakerClient state machine transitions correctly",
                 "[connection][edge][state][priority4][eventloop][slow]") {
    SECTION("State transitions during failed connection") {
        // The callback stays registered until the fixture destroys the client, and the
        // fixture destructor's disconnect() and the loop thread's onclose can both change
        // state after the body returns, so the log lives on the heap with the callback.
        struct StateLog {
            std::mutex mutex;
            std::vector<ConnectionState> states;
        };
        auto log = std::make_shared<StateLog>();
        auto& states = log->states;
        auto& states_mutex = log->mutex;

        client_->set_state_change_callback(
            [log](ConnectionState old_state, ConnectionState new_state) {
                std::lock_guard<std::mutex> lock(log->mutex);
                log->states.push_back(new_state);
            });

        client_->set_connection_timeout(kBlackHoleConnectTimeoutMs);
        client_->connect("ws://192.0.2.1:7125/websocket", []() {}, []() {});

        // 192.0.2.1 (TEST-NET-1) never answers, and on a loaded runner the
        // timeout can surface DISCONNECTED well after it is due, so this polls
        // for CONNECTING -> DISCONNECTED instead of sleeping a fixed time.
        auto reached_disconnected = [&]() {
            std::lock_guard<std::mutex> lock(states_mutex);
            return !states.empty() && states.back() == ConnectionState::DISCONNECTED;
        };
        auto deadline = steady_clock::now() + milliseconds(15000);
        while (steady_clock::now() < deadline && !reached_disconnected()) {
            std::this_thread::sleep_for(milliseconds(50));
        }

        std::lock_guard<std::mutex> lock(states_mutex);

        // Should see: CONNECTING -> ... -> DISCONNECTED
        REQUIRE(states.size() >= 2);
        CHECK(states[0] == ConnectionState::CONNECTING);
        CHECK(states[states.size() - 1] == ConnectionState::DISCONNECTED);
    }
}
