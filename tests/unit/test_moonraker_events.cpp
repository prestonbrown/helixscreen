// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_events.cpp
 * @brief Unit tests for MoonrakerClient event emission functionality
 *
 * Tests the Phase 2 event emitter pattern for decoupling transport-layer
 * events from the UI layer. The event system allows MoonrakerClient to
 * notify listeners about connection issues, errors, and state changes
 * without direct UI dependencies.
 *
 * Test Categories:
 * 1. Event handler registration and unregistration
 * 2. Event emission with correct type/message/is_error
 * 3. Sequential event emission
 * 4. Graceful handling of null/missing handlers
 * 5. Exception safety in event handlers
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "abort_manager.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"
#include "moonraker_events.h"
#include "moonraker_request.h"
#include "moonraker_request_tracker.h"
#include "rpc_error_policy.h"

#include <spdlog/fmt/fmt.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Test Access: AbortManager friend class for test-only state manipulation
// ============================================================================

namespace helix {

class AbortManagerTestAccess {
  public:
    static void reset(AbortManager& m) {
        m.cancel_all_timers();
        m.klippy_observer_ = {};
        m.cancel_state_observer_ = {};
        m.abort_state_.store(AbortManager::State::IDLE);
        m.escalation_level_.store(0);
        m.shutdown_recovery_in_progress_.store(false);
        m.kalico_status_ = AbortManager::KalicoStatus::UNKNOWN;
        m.commands_sent_ = 0;
        m.api_ = nullptr;
        m.printer_state_ = nullptr;
        if (m.subjects_initialized_) {
            lv_subject_set_int(&m.abort_state_subject_,
                               static_cast<int>(AbortManager::State::IDLE));
        }
    }

    static void on_heater_interrupt_error(AbortManager& m) {
        m.on_heater_interrupt_error();
    }

    static void set_shutdown_recovery(AbortManager& m) {
        m.abort_state_.store(AbortManager::State::SENT_ESTOP);
        m.shutdown_recovery_in_progress_.store(true);
    }

    static void on_probe_timeout(AbortManager& m) {
        m.on_probe_timeout();
    }
};

} // namespace helix

class MoonrakerRequestTrackerTestAccess {
  public:
    static void inject_request(MoonrakerRequestTracker& tracker, RequestId id,
                               PendingRequest request) {
        std::lock_guard<std::mutex> lock(tracker.requests_mutex_);
        tracker.pending_requests_[id] = std::move(request);
    }
};

// ============================================================================
// Test Helper: Testable Mock with Protected emit_event Access
// ============================================================================

/**
 * @brief Test helper that exposes protected emit_event() for unit testing
 *
 * MoonrakerClient::emit_event() is protected to prevent external code from
 * emitting fake events. This subclass exposes it for testing purposes.
 *
 * Connection- and Klippy-lifecycle events are NOT simulated here: those tests
 * drive MoonrakerClient::on_ws_open() / on_ws_message() directly, so the
 * emission decisions under test are production's, not the test helper's.
 */
class TestableMoonrakerClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    // Expose protected method for testing
    void test_emit_event(MoonrakerEventType type, const std::string& message, bool is_error = false,
                         const std::string& details = "") {
        emit_event(type, message, is_error, details);
    }
};

// ============================================================================
// Test Fixture
// ============================================================================

/**
 * @brief Test fixture for event emission tests
 *
 * Provides a testable mock client and event capture infrastructure.
 */
class EventTestFixture : public LVGLTestFixture {
  public:
    EventTestFixture()
        : client_(std::make_unique<TestableMoonrakerClient>(
              MoonrakerClientMock::PrinterType::VORON_24)) {
        // The real notify_klippy_* handlers write PrinterState's klippy subject
        // through UpdateQueue. LVGLTestFixture owns that queue's lifecycle and
        // drains it on teardown; init_subjects() gives the drained callback a
        // subject to land on instead of a half-built one.
        get_printer_state().init_subjects(false);
    }

    ~EventTestFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    /**
     * @brief Create an event handler that captures received events
     */
    MoonrakerEventCallback create_capture_handler() {
        return [this](const MoonrakerEvent& event) {
            std::lock_guard<std::mutex> lock(mutex_);
            captured_events_.push_back(event);
            event_received_.store(true);
        };
    }

    /**
     * @brief Get count of captured events (thread-safe)
     */
    size_t event_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return captured_events_.size();
    }

    /**
     * @brief Get a copy of captured events (thread-safe)
     */
    std::vector<MoonrakerEvent> get_events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return captured_events_;
    }

    /**
     * @brief Get the last captured event (thread-safe)
     * @throws std::runtime_error if no events captured
     */
    MoonrakerEvent get_last_event() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (captured_events_.empty()) {
            throw std::runtime_error("No events captured");
        }
        return captured_events_.back();
    }

    /**
     * @brief Check if any event was received
     */
    bool has_event() const {
        return event_received_.load();
    }

    /**
     * @brief Reset captured state for next test
     */
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        captured_events_.clear();
        event_received_.store(false);
    }

    std::unique_ptr<TestableMoonrakerClient> client_;

  private:
    mutable std::mutex mutex_;
    std::vector<MoonrakerEvent> captured_events_;
    std::atomic<bool> event_received_{false};
};

// ============================================================================
// Test Cases: Event Handler Registration
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient event handler can be registered",
                 "[state][integration][registration]") {
    SECTION("registered handler receives events") {
        client_->register_event_handler(create_capture_handler());

        // Emit a test event
        client_->test_emit_event(MoonrakerEventType::CONNECTION_LOST, "Test connection lost", true);

        REQUIRE(has_event());
        REQUIRE(event_count() == 1);

        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::CONNECTION_LOST);
        CHECK(event.message == "Test connection lost");
        CHECK(event.is_error == true);
    }

    SECTION("handler registration returns immediately") {
        // Should not block or throw
        auto start = std::chrono::steady_clock::now();
        client_->register_event_handler(create_capture_handler());
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();

        CHECK(elapsed < 100); // Registration should be fast
    }
}

// ============================================================================
// Test Cases: Event Content Verification
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient events contain correct fields",
                 "[state][integration][content]") {
    client_->register_event_handler(create_capture_handler());

    SECTION("error event has is_error=true") {
        client_->test_emit_event(MoonrakerEventType::RPC_ERROR, "Command failed", true,
                                 "printer.gcode.script");

        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::RPC_ERROR);
        CHECK(event.message == "Command failed");
        CHECK(event.details == "printer.gcode.script");
        CHECK(event.is_error == true);
    }

    SECTION("warning event has is_error=false") {
        client_->test_emit_event(MoonrakerEventType::RECONNECTING, "Attempting reconnect", false);

        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::RECONNECTING);
        CHECK(event.message == "Attempting reconnect");
        CHECK(event.is_error == false);
    }

    SECTION("all event types can be emitted") {
        // Test each event type
        std::vector<MoonrakerEventType> types = {
            MoonrakerEventType::CONNECTION_FAILED,   MoonrakerEventType::CONNECTION_LOST,
            MoonrakerEventType::RECONNECTING,        MoonrakerEventType::RECONNECTED,
            MoonrakerEventType::MESSAGE_OVERSIZED,   MoonrakerEventType::RPC_ERROR,
            MoonrakerEventType::KLIPPY_DISCONNECTED, MoonrakerEventType::KLIPPY_READY,
            MoonrakerEventType::DISCOVERY_FAILED,    MoonrakerEventType::REQUEST_TIMEOUT};

        for (auto type : types) {
            reset();
            client_->test_emit_event(type, "Test message", false);
            REQUIRE(event_count() == 1);
            CHECK(get_last_event().type == type);
        }
    }

    SECTION("empty details is valid") {
        client_->test_emit_event(MoonrakerEventType::KLIPPY_READY, "Ready", false, "");

        auto event = get_last_event();
        CHECK(event.details.empty());
    }

    SECTION("message with special characters is preserved") {
        std::string special_msg = "Error: \"quotes\" and 'apostrophes' & <xml> chars";
        client_->test_emit_event(MoonrakerEventType::RPC_ERROR, special_msg, true);

        auto event = get_last_event();
        CHECK(event.message == special_msg);
    }
}

// ============================================================================
// Test Cases: Sequential Event Emission
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient can emit multiple events sequentially",
                 "[state][integration][sequential]") {
    client_->register_event_handler(create_capture_handler());

    SECTION("events are received in order") {
        client_->test_emit_event(MoonrakerEventType::CONNECTION_LOST, "First", true);
        client_->test_emit_event(MoonrakerEventType::RECONNECTING, "Second", false);
        client_->test_emit_event(MoonrakerEventType::RECONNECTED, "Third", false);

        REQUIRE(event_count() == 3);

        auto events = get_events();
        CHECK(events[0].type == MoonrakerEventType::CONNECTION_LOST);
        CHECK(events[0].message == "First");
        CHECK(events[1].type == MoonrakerEventType::RECONNECTING);
        CHECK(events[1].message == "Second");
        CHECK(events[2].type == MoonrakerEventType::RECONNECTED);
        CHECK(events[2].message == "Third");
    }

    SECTION("rapid fire events all captured") {
        constexpr int NUM_EVENTS = 100;
        for (int i = 0; i < NUM_EVENTS; i++) {
            client_->test_emit_event(MoonrakerEventType::RPC_ERROR, "Event " + std::to_string(i),
                                     true);
        }

        REQUIRE(event_count() == NUM_EVENTS);

        // Verify sequence
        auto events = get_events();
        for (int i = 0; i < NUM_EVENTS; i++) {
            CHECK(events[i].message == "Event " + std::to_string(i));
        }
    }
}

// ============================================================================
// Test Cases: Null/Empty Handler Handling
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient handles null event handler gracefully",
                 "[state][integration][null_handler]") {
    SECTION("emit without registered handler does not crash") {
        // No handler registered - should log and continue
        REQUIRE_NOTHROW(
            client_->test_emit_event(MoonrakerEventType::CONNECTION_LOST, "No handler", true));
    }

    SECTION("unregistering handler with nullptr works") {
        // Register, then unregister
        client_->register_event_handler(create_capture_handler());
        client_->test_emit_event(MoonrakerEventType::RECONNECTING, "Before unregister", false);
        REQUIRE(event_count() == 1);

        // Unregister by passing nullptr
        client_->register_event_handler(nullptr);
        reset();

        // Should not crash, but no event captured
        REQUIRE_NOTHROW(
            client_->test_emit_event(MoonrakerEventType::RECONNECTED, "After unregister", false));
        CHECK(event_count() == 0);
    }

    SECTION("re-registering handler after nullptr works") {
        // Start with handler
        client_->register_event_handler(create_capture_handler());
        client_->test_emit_event(MoonrakerEventType::CONNECTION_LOST, "First", true);
        REQUIRE(event_count() == 1);

        // Unregister
        client_->register_event_handler(nullptr);
        reset();
        client_->test_emit_event(MoonrakerEventType::RECONNECTING, "Dropped", false);
        CHECK(event_count() == 0);

        // Re-register
        client_->register_event_handler(create_capture_handler());
        client_->test_emit_event(MoonrakerEventType::RECONNECTED, "Third", false);
        REQUIRE(event_count() == 1);
        CHECK(get_last_event().message == "Third");
    }
}

// ============================================================================
// Test Cases: Exception Safety in Handlers
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient catches exceptions from event handlers",
                 "[state][integration][exception_safety]") {
    SECTION("std::exception in handler is caught") {
        client_->register_event_handler(
            [](const MoonrakerEvent&) { throw std::runtime_error("Handler threw exception"); });

        // Should not propagate exception
        REQUIRE_NOTHROW(
            client_->test_emit_event(MoonrakerEventType::RPC_ERROR, "Trigger exception", true));
    }

    SECTION("exception does not prevent client operation") {
        std::atomic<int> call_count{0};

        // Handler that throws on first call, succeeds on second
        client_->register_event_handler([&call_count](const MoonrakerEvent&) {
            call_count++;
            if (call_count == 1) {
                throw std::runtime_error("First call throws");
            }
            // Second call succeeds
        });

        // First event - handler throws but client continues
        REQUIRE_NOTHROW(
            client_->test_emit_event(MoonrakerEventType::CONNECTION_LOST, "First", true));
        CHECK(call_count == 1);

        // Second event - handler succeeds
        REQUIRE_NOTHROW(client_->test_emit_event(MoonrakerEventType::RECONNECTED, "Second", false));
        CHECK(call_count == 2);
    }

    SECTION("client remains functional after handler exception") {
        // Register throwing handler
        client_->register_event_handler(
            [](const MoonrakerEvent&) { throw std::logic_error("Always throws"); });

        // Emit multiple events - all should be handled without crash
        for (int i = 0; i < 10; i++) {
            REQUIRE_NOTHROW(client_->test_emit_event(MoonrakerEventType::RPC_ERROR,
                                                     "Event " + std::to_string(i), true));
        }
    }
}

// ============================================================================
// Test Cases: Handler Replacement
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient replaces handler on re-registration",
                 "[state][integration][replacement]") {
    SECTION("new handler replaces old handler") {
        std::vector<std::string> handler1_events;
        std::vector<std::string> handler2_events;

        // Register first handler
        client_->register_event_handler([&handler1_events](const MoonrakerEvent& event) {
            handler1_events.push_back(event.message);
        });

        client_->test_emit_event(MoonrakerEventType::RECONNECTING, "To handler 1", false);
        CHECK(handler1_events.size() == 1);
        CHECK(handler2_events.size() == 0);

        // Register second handler (replaces first)
        client_->register_event_handler([&handler2_events](const MoonrakerEvent& event) {
            handler2_events.push_back(event.message);
        });

        client_->test_emit_event(MoonrakerEventType::RECONNECTED, "To handler 2", false);

        // First handler should not receive new event
        CHECK(handler1_events.size() == 1);
        CHECK(handler1_events[0] == "To handler 1");

        // Second handler should receive it
        CHECK(handler2_events.size() == 1);
        CHECK(handler2_events[0] == "To handler 2");
    }
}

// ============================================================================
// Test Cases: Thread Safety (Basic)
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient event emission is thread-safe",
                 "[state][integration][threadsafe][slow]") {
    SECTION("concurrent registration and emission") {
        std::atomic<int> received_count{0};
        std::atomic<bool> stop_flag{false};

        // Handler that counts events
        client_->register_event_handler(
            [&received_count](const MoonrakerEvent&) { received_count++; });

        // Thread that re-registers handler periodically
        std::thread register_thread([this, &stop_flag, &received_count]() {
            while (!stop_flag.load()) {
                client_->register_event_handler(
                    [&received_count](const MoonrakerEvent&) { received_count++; });
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        // Main thread emits events
        constexpr int NUM_EVENTS = 50;
        for (int i = 0; i < NUM_EVENTS; i++) {
            REQUIRE_NOTHROW(client_->test_emit_event(MoonrakerEventType::RPC_ERROR,
                                                     "Event " + std::to_string(i), true));
        }

        stop_flag.store(true);
        register_thread.join();

        // received_count only ever increments, so `>= 0` held even if every emit was
        // dropped. Exactly one handler is registered at a time (register_event_handler
        // replaces), so the count cannot exceed the number of emits either.
        int received = received_count.load();
        CHECK(received > 0);
        CHECK(received <= NUM_EVENTS);
    }
}

// ============================================================================
// Test Cases: Reconnection Event Behavior
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient RECONNECTED event behavior",
                 "[state][integration][reconnection]") {
    client_->register_event_handler(create_capture_handler());

    // on_ws_open() is the real WebSocket onopen body. Its whole reconnection rule is
    // the `was_connected_` latch it reads BEFORE setting: the first open of a process
    // is not a reconnection, every later one is. Calling it directly means the latch
    // decides, instead of the test passing itself a flag and asserting the flag back.
    SECTION("first connection does NOT emit RECONNECTED event") {
        client_->on_ws_open();

        CHECK(event_count() == 0);
        CHECK_FALSE(has_event());
    }

    SECTION("the SECOND open emits exactly one RECONNECTED event") {
        client_->on_ws_open(); // first connect — latches was_connected_
        REQUIRE(event_count() == 0);

        client_->on_ws_open(); // reconnect

        REQUIRE(event_count() == 1);
        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::RECONNECTED);
        CHECK(event.message == "Connection restored");
        CHECK(event.is_error == false);
    }

    SECTION("every subsequent reconnection emits its own event") {
        client_->on_ws_open(); // first connect
        client_->on_ws_open(); // reconnect 1
        client_->on_ws_open(); // reconnect 2

        REQUIRE(event_count() == 2);
        auto events = get_events();
        CHECK(events[0].type == MoonrakerEventType::RECONNECTED);
        CHECK(events[1].type == MoonrakerEventType::RECONNECTED);
    }
}

// ============================================================================
// Test Cases: Klippy State Event Behavior
// ============================================================================

namespace {

/// A Moonraker notification frame: no "id", so the client routes it to the
/// notification arm of on_ws_message() rather than the request tracker.
std::string klippy_notification(const char* method) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", json::array()}}.dump();
}

} // namespace

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient KLIPPY_READY event behavior",
                 "[state][integration][klippy]") {
    client_->register_event_handler(create_capture_handler());

    // Feed real Moonraker notification frames through the real onmessage body, so the
    // method-name dispatch, the event type, and the user-facing message text are all
    // production's. The disconnected message in particular is a fixed string the old
    // helper let the caller supply — which meant the test asserted its own argument.
    SECTION("notify_klippy_ready emits KLIPPY_READY") {
        client_->on_ws_message(klippy_notification("notify_klippy_ready"));

        REQUIRE(event_count() == 1);
        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::KLIPPY_READY);
        CHECK(event.message == "Klipper ready");
        CHECK(event.is_error == false);
    }

    SECTION("notify_klippy_disconnected emits KLIPPY_DISCONNECTED as an error") {
        client_->on_ws_message(klippy_notification("notify_klippy_disconnected"));

        REQUIRE(event_count() == 1);
        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::KLIPPY_DISCONNECTED);
        CHECK(event.is_error == true);
        CHECK(event.message.find("disconnected from Moonraker") != std::string::npos);
    }

    SECTION("notify_klippy_shutdown is distinct from a disconnect") {
        // Klipper is still talking to Moonraker, just halted — a different event type
        // and a different recovery path in the UI.
        client_->on_ws_message(klippy_notification("notify_klippy_shutdown"));

        REQUIRE(event_count() == 1);
        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::KLIPPY_SHUTDOWN);
        CHECK(event.is_error == true);
    }

    SECTION("disconnect then ready emits both, in order") {
        client_->on_ws_message(klippy_notification("notify_klippy_disconnected"));
        client_->on_ws_message(klippy_notification("notify_klippy_ready"));

        REQUIRE(event_count() == 2);
        auto events = get_events();
        CHECK(events[0].type == MoonrakerEventType::KLIPPY_DISCONNECTED);
        CHECK(events[0].is_error == true);
        CHECK(events[1].type == MoonrakerEventType::KLIPPY_READY);
        CHECK(events[1].is_error == false);
    }
}

// ============================================================================
// Test Cases: Shutdown Suppression
// ============================================================================
//
// MoonrakerClient::on_ws_message() hands the request tracker
// `AbortManager::instance().is_handling_shutdown()` as its suppress_error_toast
// predicate. The tracker feeds that to rpc_error_policy::decide() as
// RequestFacts::suppress_all, and only the resulting Decision decides whether an
// RPC_ERROR event reaches the UI. Driving route_response() with that same real
// predicate is what makes these assertions production's rather than the test's.
//
// The old helper here modelled the rule as `!is_silent && !suppress_toast`, which is
// not the shipped rule: a request carrying an error callback, or one on a method
// Klipper mirrors as `!!` (printer.gcode.script), is already reported by its owner and
// must NOT also raise the generic toast. So it used printer.gcode.script and asserted
// an event that production suppresses.

TEST_CASE("MoonrakerClient RPC_ERROR suppression follows AbortManager shutdown state",
          "[state][integration][shutdown][suppression]") {
    MoonrakerRequestTracker tracker;

    // No error_callback and not silent: nobody else reports this failure, so the
    // generic RPC_ERROR fallback is the only surface — the case where suppression
    // is actually observable.
    PendingRequest request;
    request.id = 4242;
    request.method = "printer.objects.query";
    request.timeout_ms = 60000;
    request.timestamp = std::chrono::steady_clock::now();
    request.intent = helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                           /*surfaces_errors=*/false};

    const json error_response = {
        {"jsonrpc", "2.0"},
        {"id", 4242},
        {"error", {{"code", -32601}, {"message", "Klippy not ready"}}},
    };

    std::vector<MoonrakerEvent> events;
    // Field order is {type, message, details, is_error} — see include/moonraker_events.h.
    auto emit = [&events](MoonrakerEventType type, const std::string& message, bool is_error,
                          const std::string& details) {
        events.push_back(MoonrakerEvent{type, message, details, is_error});
    };
    auto suppress = []() { return helix::AbortManager::instance().is_handling_shutdown(); };

    SECTION("RPC_ERROR is emitted when AbortManager is NOT handling shutdown") {
        helix::AbortManagerTestAccess::reset(helix::AbortManager::instance());
        REQUIRE(helix::AbortManager::instance().is_handling_shutdown() == false);

        MoonrakerRequestTrackerTestAccess::inject_request(tracker, 4242, request);
        REQUIRE(tracker.route_response(error_response, emit, suppress));

        REQUIRE(events.size() == 1);
        CHECK(events[0].type == MoonrakerEventType::RPC_ERROR);
        CHECK(events[0].is_error == true);
        CHECK(events[0].message.find("Klippy not ready") != std::string::npos);
        CHECK(events[0].details == "printer.objects.query");
    }

    SECTION("RPC_ERROR is suppressed while AbortManager is handling shutdown") {
        // After M112 the printer produces a burst of "Klippy not ready" rejections.
        // Toasting each one buries the recovery dialog the user actually needs.
        helix::AbortManagerTestAccess::reset(helix::AbortManager::instance());
        helix::AbortManagerTestAccess::set_shutdown_recovery(helix::AbortManager::instance());
        REQUIRE(helix::AbortManager::instance().is_handling_shutdown() == true);

        MoonrakerRequestTrackerTestAccess::inject_request(tracker, 4242, request);
        REQUIRE(tracker.route_response(error_response, emit, suppress));

        CHECK(events.empty());

        helix::AbortManagerTestAccess::reset(helix::AbortManager::instance());
    }
}

// ============================================================================
// Test Cases: Combined Connection and Klippy Events
// ============================================================================

TEST_CASE_METHOD(EventTestFixture, "MoonrakerClient combined connection flow events",
                 "[state][integration][combined]") {
    client_->register_event_handler(create_capture_handler());

    SECTION("full reconnection scenario: connection lost, reconnected, klippy ready") {
        client_->on_ws_open(); // first connect — no event
        client_->test_emit_event(MoonrakerEventType::CONNECTION_LOST, "WebSocket closed", true);
        client_->on_ws_open(); // reconnect
        client_->on_ws_message(klippy_notification("notify_klippy_ready"));

        REQUIRE(event_count() == 3);

        auto events = get_events();
        CHECK(events[0].type == MoonrakerEventType::CONNECTION_LOST);
        CHECK(events[0].is_error == true);
        CHECK(events[1].type == MoonrakerEventType::RECONNECTED);
        CHECK(events[1].is_error == false);
        CHECK(events[2].type == MoonrakerEventType::KLIPPY_READY);
        CHECK(events[2].is_error == false);
    }

    SECTION("klippy restart without connection loss") {
        // A RESTART G-code drops Klippy but not the WebSocket, so on_ws_open() never
        // runs and there must be no RECONNECTED event — the discriminator between
        // "printer firmware restarted" and "we lost the host".
        client_->on_ws_message(klippy_notification("notify_klippy_disconnected"));
        client_->on_ws_message(klippy_notification("notify_klippy_ready"));

        REQUIRE(event_count() == 2);

        auto events = get_events();
        CHECK(events[0].type == MoonrakerEventType::KLIPPY_DISCONNECTED);
        CHECK(events[1].type == MoonrakerEventType::KLIPPY_READY);

        for (const auto& evt : events) {
            CHECK(evt.type != MoonrakerEventType::RECONNECTED);
        }
    }
}
