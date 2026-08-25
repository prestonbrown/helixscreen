// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_moonraker_full_stack.cpp
 * @brief Integration tests for the Moonraker layer refactor
 *
 * Tests the full integration of the refactored Moonraker layer:
 * - Phase 2: Event emitter pattern (MoonrakerClient emits events)
 * - Phase 3: Domain logic in MoonrakerAPI
 * - Phase 4: MockPrinterState shared between mocks
 * - Phase 5: UI callers migrated to use MoonrakerAPI
 *
 * Test Categories:
 * 1. Print workflow with object exclusion (shared state synchronization)
 * 2. Temperature control cycle (API -> shared state -> client)
 * 3. Bed mesh access through API
 * 4. Event emission and handling
 * 5. Domain method parity (API vs Client)
 *
 * Tagged with [integration] for selective test runs:
 *   ./build/bin/helix-tests "[integration]"
 */

#include "ui_update_queue.h"

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_api_mock.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_events.h"
#include "../../include/printer_hardware.h"
#include "../../include/printer_state.h"
#include "../../lvgl/lvgl.h"
#include "../helix_test_fixture.h"
#include "../mocks/mock_printer_state.h"
#include "../ui_test_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
// ============================================================================
// Global LVGL Initialization (called once per test session)
// ============================================================================

namespace {
struct LVGLInitializerFullStack {
    LVGLInitializerFullStack() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};

static LVGLInitializerFullStack lvgl_init;
} // namespace

// ============================================================================
// Full Stack Test Fixture
// ============================================================================

/**
 * @brief Test fixture for full-stack integration tests
 *
 * Creates a complete mock environment with:
 * - Shared MockPrinterState
 * - MoonrakerClientMock with shared state
 * - MoonrakerAPIMock with shared state
 * - PrinterState for reactive data binding
 *
 * This fixture verifies that all layers work together correctly.
 */
class FullStackTestFixture {
  public:
    FullStackTestFixture()
        : client_(MoonrakerClientMock::PrinterType::VORON_24,
                  1000.0) // 1000x speedup for fast tests
    {
        // Create shared state
        shared_state_ = std::make_shared<MockPrinterState>();

        // Attach shared state to client mock
        client_.set_mock_state(shared_state_);

        // Initialize printer state for reactive data
        printer_state_.init_subjects(false);
        // execute_gcode() halted gate would otherwise reject all G-code: subjects
        // initialize to SHUTDOWN until production code observes a real state update.
        printer_state_.set_klippy_state_sync(helix::KlippyState::READY);

        // Create API mock BEFORE discovery so it can receive hardware callbacks
        api_ = std::make_unique<MoonrakerAPIMock>(client_, printer_state_);
        api_->set_mock_state(shared_state_);

        // Connect mock client (required for discovery)
        client_.connect("ws://mock/websocket", []() {}, []() {});

        // Run discovery to populate hardware lists (API receives hardware via callback)
        client_.discover_printer([]() {});
    }

    ~FullStackTestFixture() {
        // Drain while printer_state_ is still alive — discover_printer() in the
        // ctor leaves PrinterCapabilitiesState's deferred setters queued (#1166).
        helix::ui::UpdateQueue::instance().drain();

        client_.stop_temperature_simulation();
        client_.disconnect();
        api_.reset();
    }

  protected:
    std::shared_ptr<MockPrinterState> shared_state_;
    MoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPIMock> api_;
};

// ============================================================================
// Test Case 1: Print Workflow with Object Exclusion
// ============================================================================

TEST_CASE_METHOD(FullStackTestFixture, "Full stack: Print workflow with object exclusion",
                 "[connection][integration][exclude]") {
    SECTION("Excluded objects sync from client to API") {
        // 1. Verify initial state is clean
        REQUIRE(api_->get_excluded_objects_from_mock().empty());
        REQUIRE(client_.get_excluded_objects().empty());

        // 2. Exclude an object via G-code command (simulating Klipper command)
        client_.gcode_script("EXCLUDE_OBJECT NAME=Part_1");

        // 3. Verify it appears in BOTH client and API mock
        auto client_excluded = client_.get_excluded_objects();
        REQUIRE(client_excluded.size() == 1);
        REQUIRE(client_excluded.count("Part_1") == 1);

        auto api_excluded = api_->get_excluded_objects_from_mock();
        REQUIRE(api_excluded.size() == 1);
        REQUIRE(api_excluded.count("Part_1") == 1);
    }

    SECTION("Multiple object exclusions synchronize correctly") {
        // Exclude multiple objects
        client_.gcode_script("EXCLUDE_OBJECT NAME=Body");
        client_.gcode_script("EXCLUDE_OBJECT NAME=Support_1");
        client_.gcode_script("EXCLUDE_OBJECT NAME=Support_2");

        // Verify all appear in API mock
        auto excluded = api_->get_excluded_objects_from_mock();
        REQUIRE(excluded.size() == 3);
        REQUIRE(excluded.count("Body") == 1);
        REQUIRE(excluded.count("Support_1") == 1);
        REQUIRE(excluded.count("Support_2") == 1);
    }

    SECTION("Print start clears excluded objects") {
        // Add some excluded objects
        client_.gcode_script("EXCLUDE_OBJECT NAME=Part_1");
        client_.gcode_script("EXCLUDE_OBJECT NAME=Part_2");
        REQUIRE(api_->get_excluded_objects_from_mock().size() == 2);

        // Start a new print (this should clear excluded objects)
        client_.gcode_script("SDCARD_PRINT_FILE FILENAME=\"3DBenchy.gcode\"");

        // Verify excluded objects are cleared
        REQUIRE(api_->get_excluded_objects_from_mock().empty());
        REQUIRE(client_.get_excluded_objects().empty());
    }

    SECTION("Restart clears excluded objects") {
        // Add excluded object
        client_.gcode_script("EXCLUDE_OBJECT NAME=Part_1");
        REQUIRE(api_->get_excluded_objects_from_mock().size() == 1);

        // Issue RESTART command
        client_.gcode_script("RESTART");

        // Wait for async restart to process
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Verify excluded objects are cleared
        REQUIRE(api_->get_excluded_objects_from_mock().empty());
    }

    SECTION("Available objects set via shared state") {
        // Set available objects directly in shared state
        shared_state_->set_available_objects({"Body", "Brim", "Support_Tower"});

        // Verify accessible from API mock
        auto available = api_->get_available_objects_from_mock();
        REQUIRE(available.size() == 3);
        REQUIRE(available[0] == "Body");
        REQUIRE(available[1] == "Brim");
        REQUIRE(available[2] == "Support_Tower");
    }
}

// ============================================================================
// Test Case 2: Temperature Control Cycle
// ============================================================================

TEST_CASE_METHOD(FullStackTestFixture, "Full stack: Temperature control cycle",
                 "[connection][integration][temperature]") {
    SECTION("API set_temperature sends G-code command") {
        // set_temperature() validates, builds the heater G-code, and routes it through
        // execute_gcode() -> printer.gcode.script. Assert the command that reached the
        // transport, not merely that the call returned: a set_temperature that emitted
        // nothing (or the wrong heater) is exactly the silent failure worth catching.
        bool success_called = false;
        api_->set_temperature(
            "heater_bed", 60.0, [&success_called]() { success_called = true; },
            [](const MoonrakerError&) { FAIL("Temperature set should succeed"); });

        REQUIRE(success_called);
        REQUIRE(client_.last_send_method() == "printer.gcode.script");
        REQUIRE(client_.gcode_script_history() ==
                std::vector<std::string>{"SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60"});
    }

    SECTION("Shared state tracks temperature values") {
        // MockPrinterState has atomic temperature fields
        // Verify we can read and write them thread-safely
        double initial_extruder = shared_state_->extruder_temp;
        double initial_bed = shared_state_->bed_temp;

        // Initial temperatures should be room temperature
        REQUIRE(initial_extruder == Catch::Approx(25.0).margin(1.0));
        REQUIRE(initial_bed == Catch::Approx(25.0).margin(1.0));

        // Update temperatures directly (simulating what a simulation might do)
        shared_state_->extruder_temp = 200.0;
        shared_state_->bed_temp = 60.0;

        REQUIRE(shared_state_->extruder_temp == 200.0);
        REQUIRE(shared_state_->bed_temp == 60.0);
    }
}

// ============================================================================
// Test Case 3: Bed Mesh Access Through API
// ============================================================================

TEST_CASE_METHOD(FullStackTestFixture, "Full stack: Bed mesh access through API",
                 "[connection][integration][bedmesh]") {
    // MoonrakerAPI's constructor points the client's bed-mesh callback at
    // MoonrakerAdvancedAPI::update_bed_mesh(), so pushing a `bed_mesh` status object
    // through the client is the same path a live printer's subscription takes.
    // Values below are asserted against that payload, not against whatever the mock
    // happened to generate — a cross-check of has_bed_mesh() against
    // get_active_bed_mesh() can never fail, since both read probed_matrix.empty().
    const json bed_mesh_status = {
        {"profile_name", "default"},
        {"probed_matrix",
         json::array({json::array({0.01, 0.02, 0.03}), json::array({0.04, 0.05, 0.06})})},
        {"mesh_min", json::array({20.0, 20.0})},
        {"mesh_max", json::array({330.0, 330.0})},
        {"profiles", {{"default", json::object()}, {"adaptive", json::object()}}},
    };

    SECTION("The connect-time initial status already populates a mesh") {
        // The fixture's connect() dispatches an initial status containing bed_mesh,
        // exactly as a live subscription response does — so the API must already hold
        // a mesh before any test does anything. x_count/y_count are derived from the
        // matrix by update_bed_mesh(), so they are checked against it, not hardcoded.
        REQUIRE(api_->advanced().has_bed_mesh());

        const BedMeshProfile* mesh = api_->advanced().get_active_bed_mesh();
        REQUIRE(mesh != nullptr);
        REQUIRE_FALSE(mesh->probed_matrix.empty());
        REQUIRE(mesh->y_count == static_cast<int>(mesh->probed_matrix.size()));
        REQUIRE(mesh->x_count == static_cast<int>(mesh->probed_matrix[0].size()));
    }

    SECTION("A later bed_mesh status update replaces the active mesh") {
        client_.parse_bed_mesh(bed_mesh_status);

        REQUIRE(api_->advanced().has_bed_mesh());

        const BedMeshProfile* mesh = api_->advanced().get_active_bed_mesh();
        REQUIRE(mesh != nullptr);
        REQUIRE(mesh->name == "default");
        REQUIRE(mesh->y_count == 2);
        REQUIRE(mesh->x_count == 3);
        REQUIRE(mesh->probed_matrix.size() == 2);
        REQUIRE(mesh->probed_matrix[1][2] == Catch::Approx(0.06f));
        REQUIRE(mesh->mesh_min[0] == Catch::Approx(20.0f));
        REQUIRE(mesh->mesh_max[1] == Catch::Approx(330.0f));
    }

    SECTION("Profile names come from the status payload") {
        client_.parse_bed_mesh(bed_mesh_status);

        std::vector<std::string> api_profiles = api_->advanced().get_bed_mesh_profiles();
        std::sort(api_profiles.begin(), api_profiles.end());
        REQUIRE(api_profiles == std::vector<std::string>{"adaptive", "default"});
    }
}

// ============================================================================
// Test Case 4: Event Emission and Handling
// ============================================================================

/**
 * @brief Test helper that exposes protected emit_event() for unit testing
 *
 * MoonrakerClient::emit_event() is protected to prevent external code from
 * emitting fake events. This subclass exposes it for testing purposes.
 */
class TestableMoonrakerClientMock : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    // Expose protected method for testing
    void test_emit_event(MoonrakerEventType type, const std::string& message, bool is_error = false,
                         const std::string& details = "") {
        emit_event(type, message, is_error, details);
    }
};

class EventIntegrationFixture {
  public:
    EventIntegrationFixture() : client_(MoonrakerClientMock::PrinterType::VORON_24, 1000.0) {
        shared_state_ = std::make_shared<MockPrinterState>();
        client_.set_mock_state(shared_state_);
        printer_state_.init_subjects(false);
        client_.connect("ws://mock/websocket", []() {}, []() {});
        client_.discover_printer([]() {});
        api_ = std::make_unique<MoonrakerAPIMock>(client_, printer_state_);
        api_->set_mock_state(shared_state_);
    }

    ~EventIntegrationFixture() {
        // Drain while printer_state_ is still alive — discover_printer() in the
        // ctor leaves PrinterCapabilitiesState's deferred setters queued (#1166).
        helix::ui::UpdateQueue::instance().drain();

        client_.stop_temperature_simulation();
        client_.disconnect();
    }

    MoonrakerEventCallback create_capture_handler() {
        return [this](const MoonrakerEvent& event) {
            std::lock_guard<std::mutex> lock(mutex_);
            captured_events_.push_back(event);
            event_received_.store(true);
        };
    }

    size_t event_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return captured_events_.size();
    }

    std::vector<MoonrakerEvent> get_events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return captured_events_;
    }

    MoonrakerEvent get_last_event() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (captured_events_.empty()) {
            throw std::runtime_error("No events captured");
        }
        return captured_events_.back();
    }

    bool has_event() const {
        return event_received_.load();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        captured_events_.clear();
        event_received_.store(false);
    }

  protected:
    std::shared_ptr<MockPrinterState> shared_state_;
    TestableMoonrakerClientMock client_;
    PrinterState printer_state_;
    std::unique_ptr<MoonrakerAPIMock> api_;
    mutable std::mutex mutex_;
    std::vector<MoonrakerEvent> captured_events_;
    std::atomic<bool> event_received_{false};
};

TEST_CASE_METHOD(EventIntegrationFixture, "Full stack: Event emission and handling",
                 "[integration][state]") {
    SECTION("Registered handler receives events") {
        client_.register_event_handler(create_capture_handler());

        // Emit a test event
        client_.test_emit_event(MoonrakerEventType::CONNECTION_LOST, "Test connection lost", true);

        REQUIRE(has_event());
        REQUIRE(event_count() == 1);

        auto event = get_last_event();
        CHECK(event.type == MoonrakerEventType::CONNECTION_LOST);
        CHECK(event.message == "Test connection lost");
        CHECK(event.is_error == true);
    }

    SECTION("Multiple event types are captured correctly") {
        client_.register_event_handler(create_capture_handler());

        // Emit sequence of events
        client_.test_emit_event(MoonrakerEventType::CONNECTION_LOST, "Disconnected", true);
        client_.test_emit_event(MoonrakerEventType::RECONNECTING, "Attempting reconnect", false);
        client_.test_emit_event(MoonrakerEventType::RECONNECTED, "Connected", false);

        REQUIRE(event_count() == 3);

        auto events = get_events();
        CHECK(events[0].type == MoonrakerEventType::CONNECTION_LOST);
        CHECK(events[1].type == MoonrakerEventType::RECONNECTING);
        CHECK(events[2].type == MoonrakerEventType::RECONNECTED);
    }

    SECTION("Handler can be unregistered") {
        client_.register_event_handler(create_capture_handler());
        client_.test_emit_event(MoonrakerEventType::KLIPPY_READY, "Ready", false);
        REQUIRE(event_count() == 1);

        // Unregister by passing nullptr
        client_.register_event_handler(nullptr);
        reset();

        // Should not receive new events
        REQUIRE_NOTHROW(
            client_.test_emit_event(MoonrakerEventType::KLIPPY_DISCONNECTED, "Disconnected", true));
        CHECK(event_count() == 0);
    }

    SECTION("Event handler exceptions are caught") {
        // Register handler that throws
        client_.register_event_handler(
            [](const MoonrakerEvent&) { throw std::runtime_error("Handler threw exception"); });

        // Should not propagate exception
        REQUIRE_NOTHROW(
            client_.test_emit_event(MoonrakerEventType::RPC_ERROR, "Trigger exception", true));
    }
}

// ============================================================================
// Test Case 5: PrinterHardware Guessing
// ============================================================================

TEST_CASE_METHOD(FullStackTestFixture, "Full stack: PrinterHardware guessing",
                 "[integration][printer]") {
    // Create PrinterHardware from the API's discovered hardware
    PrinterHardware hw(api_->hardware().heaters(), api_->hardware().sensors(),
                       api_->hardware().fans(), api_->hardware().leds());

    SECTION("guess_bed_heater finds heater_bed") {
        std::string result = hw.guess_bed_heater();
        REQUIRE(result == "heater_bed"); // VORON_24 should have heater_bed
    }

    SECTION("guess_hotend_heater finds extruder") {
        std::string result = hw.guess_hotend_heater();
        REQUIRE(result == "extruder"); // VORON_24 should have extruder
    }

    SECTION("guess_bed_sensor finds bed sensor") {
        std::string result = hw.guess_bed_sensor();
        REQUIRE(result == "heater_bed"); // Bed sensor returns heater (has built-in sensor)
    }

    SECTION("guess_hotend_sensor finds hotend sensor") {
        std::string result = hw.guess_hotend_sensor();
        REQUIRE(result == "extruder"); // Hotend sensor returns heater
    }

    SECTION("guess_part_cooling_fan returns non-empty") {
        std::string result = hw.guess_part_cooling_fan();
        REQUIRE_FALSE(result.empty()); // VORON_24 should have a fan
    }
}

// ============================================================================
// Test Case 6: All Printer Types Integration
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Full stack: All printer types work correctly",
                 "[connection][integration][all_printers]") {
    PrinterState state;
    state.init_subjects(false);

    std::vector<MoonrakerClientMock::PrinterType> printer_types = {
        MoonrakerClientMock::PrinterType::VORON_24,
        MoonrakerClientMock::PrinterType::VORON_TRIDENT,
        MoonrakerClientMock::PrinterType::CREALITY_K1,
        MoonrakerClientMock::PrinterType::FLASHFORGE_AD5M,
        MoonrakerClientMock::PrinterType::GENERIC_COREXY,
        MoonrakerClientMock::PrinterType::GENERIC_BEDSLINGER,
        MoonrakerClientMock::PrinterType::MULTI_EXTRUDER,
    };

    for (auto printer_type : printer_types) {
        DYNAMIC_SECTION("Printer type " << static_cast<int>(printer_type)) {
            // Create full stack for this printer type
            auto shared_state = std::make_shared<MockPrinterState>();

            MoonrakerClientMock client(printer_type, 1000.0);
            client.set_mock_state(shared_state);

            // Create API BEFORE discovery so it can receive hardware callbacks
            MoonrakerAPIMock api(client, state);
            api.set_mock_state(shared_state);

            // Now connect and discover (API receives hardware via callback)
            client.connect("ws://mock/websocket", []() {}, []() {});
            client.discover_printer([]() {});

            // Verify basic operations work via PrinterHardware
            PrinterHardware hw(api.hardware().heaters(), api.hardware().sensors(),
                               api.hardware().fans(), api.hardware().leds());
            std::string bed = hw.guess_bed_heater();
            std::string hotend = hw.guess_hotend_heater();

            // All standard printer types should have bed and hotend
            REQUIRE_FALSE(bed.empty());
            REQUIRE_FALSE(hotend.empty());

            // Verify object exclusion works
            client.gcode_script("EXCLUDE_OBJECT NAME=Test_Part");
            auto excluded = api.get_excluded_objects_from_mock();
            REQUIRE(excluded.size() == 1);
            REQUIRE(excluded.count("Test_Part") == 1);

            // Cleanup
            client.stop_temperature_simulation();
            client.disconnect();
        }
    }
}

// ============================================================================
// Test Case 7: Concurrent Access to Shared State
// ============================================================================

TEST_CASE_METHOD(FullStackTestFixture, "Full stack: Concurrent access to shared state",
                 "[connection][integration][threading]") {
    SECTION("Concurrent excluded object operations are thread-safe") {
        std::atomic<bool> stop_flag{false};
        std::atomic<int> add_count{0};
        std::atomic<int> read_count{0};

        // Thread that adds excluded objects via client
        std::thread writer([this, &stop_flag, &add_count]() {
            int i = 0;
            while (!stop_flag.load()) {
                client_.gcode_script("EXCLUDE_OBJECT NAME=Part_" + std::to_string(i++));
                add_count++;
                std::this_thread::yield();
            }
        });

        // Thread that reads excluded objects via API
        std::thread reader([this, &stop_flag, &read_count]() {
            while (!stop_flag.load()) {
                auto excluded = api_->get_excluded_objects_from_mock();
                read_count++;
                std::this_thread::yield();
            }
        });

        // Run for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop_flag.store(true);

        writer.join();
        reader.join();

        // Verify operations completed without crash
        REQUIRE(add_count.load() > 0);
        REQUIRE(read_count.load() > 0);

        // Verify final state is consistent
        auto final_excluded = api_->get_excluded_objects_from_mock();
        auto client_excluded = client_.get_excluded_objects();
        REQUIRE(final_excluded.size() == client_excluded.size());
    }

    SECTION("Concurrent temperature updates are thread-safe") {
        std::atomic<bool> stop_flag{false};
        std::atomic<int> update_count{0};

        // Thread that updates temperatures
        std::thread updater([this, &stop_flag, &update_count]() {
            double temp = 50.0;
            while (!stop_flag.load()) {
                client_.set_extruder_target(temp);
                client_.set_bed_target(temp / 2);
                temp = (temp >= 250.0) ? 50.0 : temp + 10.0;
                update_count++;
                std::this_thread::yield();
            }
        });

        // Thread that reads temperatures via shared state
        std::thread reader([this, &stop_flag]() {
            while (!stop_flag.load()) {
                double ext_temp = shared_state_->extruder_temp;
                double bed_temp = shared_state_->bed_temp;
                // Values should be valid (non-NaN, reasonable range)
                REQUIRE(ext_temp >= 0.0);
                REQUIRE(ext_temp <= 500.0);
                REQUIRE(bed_temp >= 0.0);
                REQUIRE(bed_temp <= 200.0);
                std::this_thread::yield();
            }
        });

        // Run for a short time
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        stop_flag.store(true);

        updater.join();
        reader.join();

        REQUIRE(update_count.load() > 0);
    }
}

// ============================================================================
// Test Case 8: State Reset and Cleanup
// ============================================================================

TEST_CASE_METHOD(FullStackTestFixture, "Full stack: State reset and cleanup",
                 "[integration][connection]") {
    SECTION("MockPrinterState reset clears all state") {
        // Set up various state
        shared_state_->extruder_temp = 200.0;
        shared_state_->bed_temp = 60.0;
        shared_state_->print_state = 1;
        shared_state_->add_excluded_object("Part_1");
        shared_state_->set_available_objects({"Object_A"});
        shared_state_->set_current_filename("test.gcode");

        // Reset
        shared_state_->reset();

        // Verify all state is cleared
        REQUIRE(shared_state_->extruder_temp == 25.0);
        REQUIRE(shared_state_->bed_temp == 25.0);
        REQUIRE(shared_state_->print_state == 0);
        REQUIRE(shared_state_->get_excluded_objects().empty());
        REQUIRE(shared_state_->get_available_objects().empty());
        REQUIRE(shared_state_->get_current_filename().empty());
    }

    SECTION("State changes persist through API and client") {
        // Set state via shared state
        shared_state_->set_current_filename("persistent_file.gcode");
        shared_state_->set_available_objects({"Persistent_Object"});

        // Verify via API
        auto available = api_->get_available_objects_from_mock();
        REQUIRE(available.size() == 1);
        REQUIRE(available[0] == "Persistent_Object");

        // Verify filename via shared state (no API method for this)
        REQUIRE(shared_state_->get_current_filename() == "persistent_file.gcode");
    }
}

// ============================================================================
// Test Case 9: API Error Handling Integration
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture, "Full stack: API error callbacks work correctly",
                 "[connection][integration][errors]") {
    PrinterState state;
    state.init_subjects(false);

    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24, 1000.0);
    client.connect("ws://mock/websocket", []() {}, []() {});

    // Construct API before discovery so its hardware callbacks get fired
    MoonrakerAPIMock api(client, state);

    client.discover_printer([]() {});

    // Allow async discovery to populate hardware data
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    SECTION("Async API methods invoke exactly one callback path") {
        // get_excluded_objects() answers from printer.objects.query and treats a
        // response with no exclude_object key as "nothing excluded" — success with an
        // empty set, never on_error. A run where neither callback fires is the bug
        // this asserts against: the caller would wait forever on a reply that never came.
        bool success_called = false;
        bool error_called = false;
        std::set<std::string> excluded;

        api.advanced().get_excluded_objects(
            [&](const std::set<std::string>& objs) {
                excluded = objs;
                success_called = true;
            },
            [&error_called](const MoonrakerError&) { error_called = true; });

        REQUIRE(success_called);
        REQUIRE_FALSE(error_called);
        REQUIRE(excluded.empty());
    }

    SECTION("Sync API methods return the discovered hardware") {
        // Hardware guessing now uses PrinterHardware directly. VORON_24's discovery
        // seeds the canonical Klipper names, so these are exact, not merely non-empty.
        PrinterHardware hw(api.hardware().heaters(), api.hardware().sensors(),
                           api.hardware().fans(), api.hardware().leds());
        REQUIRE(hw.guess_bed_heater() == "heater_bed");
        REQUIRE(hw.guess_hotend_heater() == "extruder");
    }

    client.stop_temperature_simulation();
    client.disconnect();
}
