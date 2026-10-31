// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/sensors_forget.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "moonraker_client.h"
#include "pause_descriptor.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "temperature_sensor_manager.h"

#include <optional>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::test::SensorsForget;

using namespace helix;
using namespace helix::ui;
using Catch::Approx;

namespace {

/// The process-wide PrinterState, reset and with its subjects initialised
/// without XML registration.
PrinterState& fresh_state() {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);
    return state;
}

} // namespace

// ============================================================================
// Singleton Behavior Tests
// ============================================================================

TEST_CASE("PrinterState: Singleton returns same instance", "[core][state][singleton]") {
    lv_init_safe();

    PrinterState& instance1 = get_printer_state();
    PrinterState& instance2 = get_printer_state();

    // Should be the exact same object (same memory address)
    REQUIRE(&instance1 == &instance2);
}

TEST_CASE("PrinterState: Singleton persists modifications", "[core][state][singleton]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    state.init_subjects();

    // Modify a value through one reference
    state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Connected");
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    // Read it back through another reference
    PrinterState& state2 = get_printer_state();
    REQUIRE(lv_subject_get_int(state2.network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::CONNECTED));
}

TEST_CASE("PrinterState: Singleton subjects have consistent addresses",
          "[core][state][singleton]") {
    lv_init_safe();

    PrinterState& state1 = get_printer_state();
    state1.init_subjects();

    lv_subject_t* subject1 = state1.network_state().get_printer_connection_state_subject();

    PrinterState& state2 = get_printer_state();
    lv_subject_t* subject2 = state2.network_state().get_printer_connection_state_subject();

    // Subject pointers must be identical (not just equal values)
    REQUIRE(subject1 == subject2);
}

// ============================================================================
// Observer Pattern Tests
// ============================================================================

TEST_CASE("PrinterState: Observer fires when printer connection state changes",
          "[core][state][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state); // Allow re-initialization after lv_init()
    state.init_subjects(false);           // Skip XML registration

    // Register observer
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer = lv_subject_add_observer(
        state.network_state().get_printer_connection_state_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added (fires immediately with current value)
    REQUIRE(user_data[0] == 1); // Callback fired immediately with initial value (0)
    REQUIRE(user_data[1] == 0); // Initial value is DISCONNECTED (0)

    // Change state - should trigger observer again
    state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTING),
                                       "Connecting...");
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(user_data[0] == 2); // Callback fired again with new value
    REQUIRE(user_data[1] == static_cast<int>(ConnectionState::CONNECTING));

    // Change again
    state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Connected");
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(user_data[0] == 3); // Callback fired three times total (initial + 2 changes)
    REQUIRE(user_data[1] == static_cast<int>(ConnectionState::CONNECTED));

    lv_observer_remove(observer);
}

TEST_CASE("PrinterState: Observer fires when network status changes",
          "[state][observer][network]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state); // Allow re-initialization after lv_init()
    state.init_subjects(false);           // Skip XML registration

    int callback_count = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count_ptr)++;
    };

    lv_observer_t* observer = lv_subject_add_observer(
        state.network_state().get_network_status_subject(), observer_cb, &callback_count);

    // LVGL auto-notifies observers when first added (fires immediately with current value)
    // Note: init_subjects() initializes network_status to CONNECTED (2) as mock mode default
    REQUIRE(callback_count == 1); // Callback fired immediately with initial value

    // Change network status to a DIFFERENT value - should trigger observer again
    state.network_state().set_network_status(static_cast<int>(NetworkStatus::DISCONNECTED));

    REQUIRE(callback_count == 2); // Callback fired again with new value
    REQUIRE(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
            static_cast<int>(NetworkStatus::DISCONNECTED));

    lv_observer_remove(observer);
}

TEST_CASE("PrinterState: Multiple observers on same subject all fire", "[state][observer]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state); // Allow re-initialization after lv_init()
    state.init_subjects(false);           // Skip XML registration

    int count1 = 0, count2 = 0, count3 = 0;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count_ptr)++;
    };

    // Register three observers on printer connection state
    lv_observer_t* observer1 = lv_subject_add_observer(
        state.network_state().get_printer_connection_state_subject(), observer_cb, &count1);
    lv_observer_t* observer2 = lv_subject_add_observer(
        state.network_state().get_printer_connection_state_subject(), observer_cb, &count2);
    lv_observer_t* observer3 = lv_subject_add_observer(
        state.network_state().get_printer_connection_state_subject(), observer_cb, &count3);

    // LVGL auto-notifies observers when first added (each fires immediately with current value)
    REQUIRE(count1 == 1); // First observer fired immediately
    REQUIRE(count2 == 1); // Second observer fired immediately
    REQUIRE(count3 == 1); // Third observer fired immediately

    // Single state change should fire all three again
    state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Connected");
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(count1 == 2); // All observers fired again
    REQUIRE(count2 == 2);
    REQUIRE(count3 == 2);

    lv_observer_remove(observer1);
    lv_observer_remove(observer2);
    lv_observer_remove(observer3);
}

// ============================================================================
// Initialization Tests
// ============================================================================

TEST_CASE("PrinterState: Initialization sets default values", "[state][init]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state); // Reset singleton state from previous tests
    state.init_subjects();

    // Temperature subjects should be initialized to 0
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_target_subject()) ==
            0);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_bed_temp_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_bed_target_subject()) == 0);

    // Print progress should be 0
    REQUIRE(lv_subject_get_int(state.print_state().get_print_progress_subject()) == 0);

    // Print state should be "standby"
    const char* print_state = lv_subject_get_string(state.print_state().get_print_state_subject());
    REQUIRE(std::string(print_state) == "standby");

    // Position should be 0
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 0);
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 0);

    // Speed/flow factors should be 100%
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 100);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 100);

    // Fan speed should be 0
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 0);

    // Printer connection state should be DISCONNECTED
    REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::DISCONNECTED));

    // Network status is initialized to CONNECTED (mock mode default)
    // In production, actual network status comes from EthernetManager/WiFiManager
    REQUIRE(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
            static_cast<int>(NetworkStatus::CONNECTED));
}

// ============================================================================
// Temperature Update Tests
// ============================================================================
// Note: Subjects store temperatures in decidegrees (temp * 10) for 0.1°C resolution.

TEST_CASE("PrinterState: temperatures from status are stored in decidegrees",
          "[state][temp][edge]") {
    using Getter = lv_subject_t* (PrinterTemperatureState::*)();
    struct Row {
        const char* label;
        const char* object;
        const char* field;
        double value;
        Getter subject;
        int expected;
    };
    const Row rows[] = {
        {"extruder temperature", "extruder", "temperature", 205.3,
         &PrinterTemperatureState::get_active_extruder_temp_subject, 2053},
        {"extruder target", "extruder", "target", 210.0,
         &PrinterTemperatureState::get_active_extruder_target_subject, 2100},
        {"bed temperature", "heater_bed", "temperature", 60.5,
         &PrinterTemperatureState::get_bed_temp_subject, 605},
        {"bed target", "heater_bed", "target", 60.0,
         &PrinterTemperatureState::get_bed_target_subject, 600},
        // A tenth either side of a whole degree is kept, not rounded away.
        {"205.4", "extruder", "temperature", 205.4,
         &PrinterTemperatureState::get_active_extruder_temp_subject, 2054},
        {"205.6", "extruder", "temperature", 205.6,
         &PrinterTemperatureState::get_active_extruder_temp_subject, 2056},
        {"210.0", "extruder", "temperature", 210.0,
         &PrinterTemperatureState::get_active_extruder_temp_subject, 2100},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.label) {
            PrinterState& state = fresh_state();
            state.update_from_status(json{{row.object, {{row.field, row.value}}}});
            CHECK(lv_subject_get_int((state.temperature_state().*row.subject)()) == row.expected);
        }
    }
}

// ============================================================================
// Print Progress Tests
// ============================================================================

TEST_CASE("PrinterState: Update print progress from notification", "[state][progress]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    json notification = {{"method", "notify_status_update"},
                         {"params", {{{"virtual_sdcard", {{"progress", 0.45}}}}, 1234567890.0}}};

    state.update_from_status(notification["params"][0]);

    REQUIRE(lv_subject_get_int(state.print_state().get_print_progress_subject()) == 45);
}

TEST_CASE("PrinterState: Update print state and filename", "[state][progress]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    json notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"print_stats", {{"state", "printing"}, {"filename", "benchy.gcode"}}}}, 1234567890.0}}};

    state.update_from_status(notification["params"][0]);

    const char* print_state = lv_subject_get_string(state.print_state().get_print_state_subject());
    REQUIRE(std::string(print_state) == "printing");

    const char* filename = lv_subject_get_string(state.print_state().get_print_filename_subject());
    REQUIRE(std::string(filename) == "benchy.gcode");
}

TEST_CASE("PrinterState: print_stats messages decode a firmware error envelope",
          "[state][pause][firmware_error_envelope]") {
    PrinterState& state = fresh_state();

    json status = {
        {"print_stats",
         {{"state", "error"},
          {"message",
           R"({"coded": "0003-0522-0002-0008", "msg":"Lost communication with MCU 'e0'"})"},
          {"exception",
           {{"id", 522},
            {"code", 8},
            {"message", R"(Halted: {"code":"key9001","msg":"MCU shutdown"})"}}}}}};
    state.update_from_status(status);

    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_print_message_subject())) ==
            "Lost communication with MCU 'e0'");
    REQUIRE(state.print_state().get_print_exception_message() == "Halted: MCU shutdown");

    // Plain prose is untouched.
    state.update_from_status(json{{"print_stats", {{"message", "Runout Detected"}}}});
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_print_message_subject())) ==
            "Runout Detected");
}

TEST_CASE("pause_descriptor::read: silence, an empty descriptor and a populated one",
          "[state][pause]") {
    using helix::pause_descriptor::read;
    CHECK_FALSE(read(json{{"state", "paused"}}).has_value());
    CHECK_FALSE(read(json{{"exception", nullptr}}).has_value());

    auto cleared = read(json{{"exception", json::object()}});
    REQUIRE(cleared.has_value());
    CHECK(cleared->id == -1);
    CHECK(cleared->code == -1);
    CHECK(cleared->message.empty());
    CHECK_FALSE(helix::pause_descriptor::frame_carries_cause(json{{"exception", json::object()}}));

    const json populated = {
        {"exception", {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}};
    auto cause = read(populated);
    REQUIRE(cause.has_value());
    CHECK(cause->id == 532);
    CHECK(cause->code == 1);
    CHECK(cause->message == "detected dirty bed");
    CHECK(helix::pause_descriptor::frame_carries_cause(populated));

    auto partial = read(json{{"exception", {{"id", "x"}, {"message", 7}}}});
    REQUIRE(partial.has_value());
    CHECK(partial->id == -1);
    CHECK(partial->message.empty());
}

TEST_CASE("PrinterState: parses print_stats.exception structured object",
          "[state][progress][pause]") {
    PrinterState& state = fresh_state();

    SECTION("Populated exception object sets all three fields") {
        // Hardware-verified runout shape from a physical Snapmaker U1 (#991).
        json status = {{"print_stats",
                        {{"state", "paused"},
                         {"exception",
                          {{"id", 523},
                           {"index", 1},
                           {"code", 0},
                           {"message", "e1_filament runout"},
                           {"level", 2}}}}}};
        state.update_from_status(status);

        REQUIRE(state.print_state().get_print_exception_id() == 523);
        REQUIRE(state.print_state().get_print_exception_code() == 0);
        REQUIRE(state.print_state().get_print_exception_message() == "e1_filament runout");
    }

    SECTION("Empty exception object resets fields to cleared state") {
        // First populate, then clear with an empty object (user pause / cleared).
        json populate = {
            {"print_stats",
             {{"exception", {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}}}};
        state.update_from_status(populate);
        REQUIRE(state.print_state().get_print_exception_id() == 532);

        json clear = {{"print_stats", {{"exception", json::object()}}}};
        state.update_from_status(clear);

        REQUIRE(state.print_state().get_print_exception_id() == -1);
        REQUIRE(state.print_state().get_print_exception_code() == -1);
        REQUIRE(state.print_state().get_print_exception_message().empty());
    }

    SECTION("Absent exception key leaves fields unchanged") {
        json populate = {
            {"print_stats",
             {{"exception", {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}}}};
        state.update_from_status(populate);

        // A subsequent update with no exception key must not disturb the values.
        json no_exc = {{"print_stats", {{"state", "paused"}}}};
        state.update_from_status(no_exc);

        REQUIRE(state.print_state().get_print_exception_id() == 532);
        REQUIRE(state.print_state().get_print_exception_code() == 1);
        REQUIRE(state.print_state().get_print_exception_message() == "detected dirty bed");
    }

    SECTION("Null exception (subscription null) leaves fields unchanged") {
        json populate = {
            {"print_stats",
             {{"exception", {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}}}}}};
        state.update_from_status(populate);

        json null_exc = {{"print_stats", {{"exception", nullptr}}}};
        state.update_from_status(null_exc);

        REQUIRE(state.print_state().get_print_exception_id() == 532);
        REQUIRE(state.print_state().get_print_exception_code() == 1);
        REQUIRE(state.print_state().get_print_exception_message() == "detected dirty bed");
    }
}

TEST_CASE("PrinterState: a pause takes no exception latched earlier in the print",
          "[state][progress][pause]") {
    PrinterState& state = fresh_state();
    state.update_from_status(json{{"print_stats", {{"state", "printing"}}}});
    const json dirty_bed = {{"id", 532}, {"code", 1}, {"message", "detected dirty bed"}};

    SECTION("published while printing, then a later pause without one: a plain pause") {
        state.update_from_status(json{{"print_stats", {{"exception", dirty_bed}}}});
        state.update_from_status(json{{"print_stats", {{"print_duration", 12.0}}}});
        state.update_from_status(json{{"print_stats", {{"print_duration", 13.0}}}});
        state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});
        CHECK(state.print_state().get_print_exception_id() == -1);
        CHECK(state.print_state().get_print_exception_code() == -1);
    }
    SECTION("carried in the pause's own frame") {
        state.update_from_status(
            json{{"print_stats", {{"state", "paused"}, {"exception", dirty_bed}}}});
        CHECK(state.print_state().get_print_exception_id() == 532);
    }
    SECTION("published in the frame just before the pause") {
        state.update_from_status(json{{"print_stats", {{"exception", dirty_bed}}}});
        state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});
        CHECK(state.print_state().get_print_exception_id() == 532);
    }
    SECTION("arriving after the pause") {
        state.update_from_status(json{{"print_stats", {{"state", "paused"}}}});
        state.update_from_status(json{{"print_stats", {{"exception", dirty_bed}}}});
        CHECK(state.print_state().get_print_exception_id() == 532);
    }
    SECTION("a resume clears it") {
        state.update_from_status(
            json{{"print_stats", {{"state", "paused"}, {"exception", dirty_bed}}}});
        state.update_from_status(json{{"print_stats", {{"state", "printing"}}}});
        CHECK(state.print_state().get_print_exception_id() == -1);
    }
}

TEST_CASE("PrinterState: Progress percentage edge cases", "[state][progress][edge]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    SECTION("0% progress") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"virtual_sdcard", {{"progress", 0.0}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(lv_subject_get_int(state.print_state().get_print_progress_subject()) == 0);
    }

    SECTION("100% progress") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"virtual_sdcard", {{"progress", 1.0}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(lv_subject_get_int(state.print_state().get_print_progress_subject()) == 100);
    }

    SECTION("67.3% progress -> 67%") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"virtual_sdcard", {{"progress", 0.673}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(lv_subject_get_int(state.print_state().get_print_progress_subject()) == 67);
    }
}

// ============================================================================
// Motion/Position Tests
// ============================================================================

TEST_CASE("PrinterState: Update toolhead position", "[state][motion]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    json notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"toolhead", {{"position", {125.5, 87.3, 45.2, 1234.5}}, {"homed_axes", "xyz"}}}},
          1234567890.0}}};

    state.update_from_status(notification["params"][0]);

    // Positions are stored as centimillimeters (×100) for 0.01mm precision
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 12550); // 125.5mm
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 8730);  // 87.3mm
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 4520);  // 45.2mm

    const char* homed = lv_subject_get_string(state.motion_state().get_homed_axes_subject());
    REQUIRE(std::string(homed) == "xyz");
}

TEST_CASE("PrinterState: Homed axes variations", "[state][motion]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    for (const char* axes : {"xy", "", "z", "xyz"}) {
        INFO("homed_axes '" << axes << "'");
        state.update_from_status(json{{"toolhead", {{"homed_axes", axes}}}});
        CHECK(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
              axes);
    }
}

// ============================================================================
// Speed/Flow Factor Tests
// ============================================================================

TEST_CASE("PrinterState: Update speed and flow factors", "[state][speed]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    json notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"gcode_move", {{"speed_factor", 1.25}, {"extrude_factor", 0.95}}}}, 1234567890.0}}};

    state.update_from_status(notification["params"][0]);

    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 125);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 95);
}

TEST_CASE("PrinterState: Update fan speed", "[state][fan]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    json notification = {{"method", "notify_status_update"},
                         {"params", {{{"fan", {{"speed", 0.75}}}}, 1234567890.0}}};

    state.update_from_status(notification["params"][0]);

    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 75);
}

// ============================================================================
// Connection State Tests
// ============================================================================

TEST_CASE("PrinterState: Set printer connection state", "[state][connection]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Connected");
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::CONNECTED));

    const char* message =
        lv_subject_get_string(state.network_state().get_printer_connection_message_subject());
    REQUIRE(std::string(message) == "Connected");
}

TEST_CASE("PrinterState: Connection state transitions", "[state][connection]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    SECTION("Disconnected -> Connecting") {
        state.set_printer_connection_state(static_cast<int>(ConnectionState::DISCONNECTED),
                                           "Disconnected");
        state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTING),
                                           "Connecting...");
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
                static_cast<int>(ConnectionState::CONNECTING));
    }

    SECTION("Connecting -> Connected") {
        state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTING),
                                           "Connecting...");
        state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Ready");
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
                static_cast<int>(ConnectionState::CONNECTED));
    }

    SECTION("Connected -> Reconnecting") {
        state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Ready");
        state.set_printer_connection_state(static_cast<int>(ConnectionState::RECONNECTING),
                                           "Reconnecting...");
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
                static_cast<int>(ConnectionState::RECONNECTING));
    }

    SECTION("Failed connection") {
        state.set_printer_connection_state(static_cast<int>(ConnectionState::FAILED),
                                           "Connection failed");
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());
        REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
                static_cast<int>(ConnectionState::FAILED));
    }
}

// ============================================================================
// Derived connection state: nav_buttons_enabled, was_ever_connected
// ============================================================================

TEST_CASE("PrinterState: nav_buttons_enabled derivation",
          "[characterization][network][nav_buttons]") {
    PrinterState& state = fresh_state();

    // Defaults: disconnected, "Disconnected", nav buttons off.
    REQUIRE(std::string(lv_subject_get_string(
                state.network_state().get_printer_connection_message_subject())) == "Disconnected");
    REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);

    SECTION("connected AND klippy ready -> nav_buttons_enabled = 1") {
        // Set connected state
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");
        // Set klippy ready
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 1);
    }

    SECTION("disconnected AND klippy ready -> nav_buttons_enabled = 0") {
        // Set disconnected state
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::DISCONNECTED), "Disconnected");
        // Set klippy ready
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connected AND klippy error -> nav_buttons_enabled = 0") {
        // Set connected state
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");
        // Set klippy error
        state.set_klippy_state_sync(KlippyState::ERROR);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("disconnected AND klippy error -> nav_buttons_enabled = 0") {
        // Set disconnected state
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::DISCONNECTED), "Disconnected");
        // Set klippy error
        state.set_klippy_state_sync(KlippyState::ERROR);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connecting state -> nav_buttons_enabled = 0") {
        // Set connecting state (not fully connected)
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTING), "Connecting...");
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("reconnecting state -> nav_buttons_enabled = 0") {
        // Set reconnecting state (not fully connected)
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::RECONNECTING), "Reconnecting...");
        state.set_klippy_state_sync(KlippyState::READY);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connected AND klippy startup -> nav_buttons_enabled = 0") {
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");
        state.set_klippy_state_sync(KlippyState::STARTUP);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }

    SECTION("connected AND klippy shutdown -> nav_buttons_enabled = 0") {
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");
        state.set_klippy_state_sync(KlippyState::SHUTDOWN);

        REQUIRE(lv_subject_get_int(state.network_state().get_nav_buttons_enabled_subject()) == 0);
    }
}

TEST_CASE("PrinterState: observer fires when nav_buttons_enabled changes",
          "[characterization][network][observer]") {
    PrinterState& state = fresh_state();

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1};

    lv_observer_t* observer = lv_subject_add_observer(
        state.network_state().get_nav_buttons_enabled_subject(), observer_cb, user_data);

    // Initial notification
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Starts disabled

    // Enable nav buttons by connecting with klippy ready
    state.network_state().set_printer_connection_state_internal(
        static_cast<int>(ConnectionState::CONNECTED), "Ready");
    state.set_klippy_state_sync(KlippyState::READY);

    REQUIRE(user_data[0] == 2);
    REQUIRE(user_data[1] == 1);

    // Disable by setting klippy to error
    state.set_klippy_state_sync(KlippyState::ERROR);

    REQUIRE(user_data[0] == 3);
    REQUIRE(user_data[1] == 0);

    lv_observer_remove(observer);
}

TEST_CASE("PrinterState: was_ever_connected flag behavior",
          "[characterization][network][was_connected]") {
    PrinterState& state = fresh_state();

    // PrinterStateTestAccess::reset() clears the flag, so each SECTION starts false.
    REQUIRE_FALSE(state.network_state().was_ever_connected());

    SECTION("becomes true when connection state becomes CONNECTED") {
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");

        REQUIRE(state.network_state().was_ever_connected() == true);
    }

    SECTION("stays true even after disconnection") {
        // First connect
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");
        REQUIRE(state.network_state().was_ever_connected() == true);

        // Then disconnect
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::DISCONNECTED), "Disconnected");
        REQUIRE(state.network_state().was_ever_connected() == true);
    }

    SECTION("stays true through reconnection cycle") {
        // Connect
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTED), "Ready");
        REQUIRE(state.network_state().was_ever_connected() == true);

        // Reconnecting
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::RECONNECTING), "Reconnecting...");
        REQUIRE(state.network_state().was_ever_connected() == true);

        // Disconnect (failed)
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::FAILED), "Connection failed");
        REQUIRE(state.network_state().was_ever_connected() == true);
    }

    SECTION("CONNECTING state alone does not set the flag") {
        state.network_state().set_printer_connection_state_internal(
            static_cast<int>(ConnectionState::CONNECTING), "Connecting...");

        REQUIRE_FALSE(state.network_state().was_ever_connected());
    }
}

// ============================================================================
// Network Status Tests
// ============================================================================

TEST_CASE("PrinterState: Network status initialization", "[state][network]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state); // Reset singleton state from previous tests
    state.init_subjects();

    // Network status is initialized to CONNECTED (mock mode default)
    // In production, actual network status comes from EthernetManager/WiFiManager
    REQUIRE(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
            static_cast<int>(NetworkStatus::CONNECTED));
}

TEST_CASE("PrinterState: set_network_status publishes every status", "[state][network]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    state.init_subjects();

    for (auto status :
         {NetworkStatus::DISCONNECTED, NetworkStatus::CONNECTING, NetworkStatus::CONNECTED}) {
        INFO(static_cast<int>(status));
        state.network_state().set_network_status(static_cast<int>(status));
        CHECK(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
              static_cast<int>(status));
    }

    // Repeating the current status is a no-op that leaves it in place.
    state.network_state().set_network_status(static_cast<int>(NetworkStatus::CONNECTED));
    CHECK(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
          static_cast<int>(NetworkStatus::CONNECTED));
}

TEST_CASE("PrinterState: Printer and network status are independent", "[state][integration]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    state.init_subjects();

    // Set printer connected but network disconnected
    state.set_printer_connection_state(static_cast<int>(ConnectionState::CONNECTED), "Connected");
    state.network_state().set_network_status(static_cast<int>(NetworkStatus::DISCONNECTED));
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::CONNECTED));
    REQUIRE(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
            static_cast<int>(NetworkStatus::DISCONNECTED));

    // Set network connected but printer disconnected
    state.set_printer_connection_state(static_cast<int>(ConnectionState::DISCONNECTED),
                                       "Disconnected");
    state.network_state().set_network_status(static_cast<int>(NetworkStatus::CONNECTED));
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(state.network_state().get_printer_connection_state_subject()) ==
            static_cast<int>(ConnectionState::DISCONNECTED));
    REQUIRE(lv_subject_get_int(state.network_state().get_network_status_subject()) ==
            static_cast<int>(NetworkStatus::CONNECTED));
}

// ============================================================================
// Invalid/Malformed Data Tests
// ============================================================================
// These tests verify update_from_status handles edge cases gracefully.

TEST_CASE("PrinterState: Empty status object is handled", "[state][error]") {
    PrinterState& state = fresh_state();

    // Empty JSON should not crash
    json empty_status = json::object();
    state.update_from_status(empty_status);

    // Values should remain at defaults
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) == 0);
}

TEST_CASE("PrinterState: Partial status updates work", "[state][error]") {
    PrinterState& state = fresh_state();

    SECTION("Only extruder temp, no target") {
        json status = {{"extruder", {{"temperature", 205.0}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) ==
                2050);
        REQUIRE(
            lv_subject_get_int(state.temperature_state().get_active_extruder_target_subject()) ==
            0); // unchanged
    }

    SECTION("Only bed target, no temp") {
        json status = {{"heater_bed", {{"target", 60.0}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.temperature_state().get_bed_temp_subject()) ==
                0); // unchanged
        REQUIRE(lv_subject_get_int(state.temperature_state().get_bed_target_subject()) == 600);
    }

    SECTION("Unknown fields are ignored") {
        json status = {{"unknown_sensor", {{"value", 123.0}}},
                       {"extruder", {{"temperature", 100.0}}}};
        state.update_from_status(status);
        REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) ==
                1000);
    }
}

// ============================================================================
// Comprehensive State Update Tests
// ============================================================================

TEST_CASE("PrinterState: Complete printing state update", "[state][integration]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    json notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"extruder", {{"temperature", 210.5}, {"target", 210.0}}},
           {"heater_bed", {{"temperature", 60.2}, {"target", 60.0}}},
           {"virtual_sdcard", {{"progress", 0.67}}},
           {"print_stats", {{"state", "printing"}, {"filename", "model.gcode"}}},
           {"toolhead", {{"position", {125.0, 87.0, 45.0, 1234.0}}, {"homed_axes", "xyz"}}},
           {"gcode_move", {{"speed_factor", 1.0}, {"extrude_factor", 1.0}}},
           {"fan", {{"speed", 0.5}}}},
          1234567890.0}}};

    state.update_from_status(notification["params"][0]);

    // Verify all values updated correctly (temps stored as decidegrees)
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) ==
            2105);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_target_subject()) ==
            2100);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_bed_temp_subject()) == 602);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_bed_target_subject()) == 600);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_progress_subject()) == 67);
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_print_state_subject())) ==
            "printing");
    REQUIRE(std::string(lv_subject_get_string(state.print_state().get_print_filename_subject())) ==
            "model.gcode");
    // Positions are stored as centimillimeters (×100) for 0.01mm precision
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_x_subject()) == 12500); // 125.0mm
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_y_subject()) == 8700);  // 87.0mm
    REQUIRE(lv_subject_get_int(state.motion_state().get_position_z_subject()) == 4500);  // 45.0mm
    REQUIRE(std::string(lv_subject_get_string(state.motion_state().get_homed_axes_subject())) ==
            "xyz");
    REQUIRE(lv_subject_get_int(state.motion_state().get_speed_factor_subject()) == 100);
    REQUIRE(lv_subject_get_int(state.motion_state().get_flow_factor_subject()) == 100);
    REQUIRE(lv_subject_get_int(state.fan_state().get_fan_speed_subject()) == 50);
}

// ============================================================================
// PrintJobState Enum Tests
// ============================================================================

TEST_CASE("PrintJobState: parse_print_job_state parses Moonraker strings", "[state][enum]") {
    SECTION("Parses all standard Moonraker states") {
        REQUIRE(parse_print_job_state("standby") == PrintJobState::STANDBY);
        REQUIRE(parse_print_job_state("printing") == PrintJobState::PRINTING);
        REQUIRE(parse_print_job_state("paused") == PrintJobState::PAUSED);
        REQUIRE(parse_print_job_state("complete") == PrintJobState::COMPLETE);
        REQUIRE(parse_print_job_state("cancelled") == PrintJobState::CANCELLED);
        REQUIRE(parse_print_job_state("error") == PrintJobState::ERROR);
    }

    SECTION("Unknown strings default to STANDBY") {
        REQUIRE(parse_print_job_state("unknown") == PrintJobState::STANDBY);
        REQUIRE(parse_print_job_state("") == PrintJobState::STANDBY);
        REQUIRE(parse_print_job_state("PRINTING") == PrintJobState::STANDBY); // Case sensitive
    }

    SECTION("Handles null input") {
        REQUIRE(parse_print_job_state(nullptr) == PrintJobState::STANDBY);
    }
}

TEST_CASE("PrintJobState: print_job_state_to_string converts to display strings", "[state][enum]") {
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::STANDBY)) == "Standby");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::PRINTING)) == "Printing");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::PAUSED)) == "Paused");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::COMPLETE)) == "Complete");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::CANCELLED)) == "Canceled");
    REQUIRE(std::string(print_job_state_to_string(PrintJobState::ERROR)) == "Error");
}

TEST_CASE("PrintJobState: Enum values match expected integers", "[state][enum]") {
    // These values are documented and must not change for backward compatibility
    REQUIRE(static_cast<int>(PrintJobState::STANDBY) == 0);
    REQUIRE(static_cast<int>(PrintJobState::PRINTING) == 1);
    REQUIRE(static_cast<int>(PrintJobState::PAUSED) == 2);
    REQUIRE(static_cast<int>(PrintJobState::COMPLETE) == 3);
    REQUIRE(static_cast<int>(PrintJobState::CANCELLED) == 4);
    REQUIRE(static_cast<int>(PrintJobState::ERROR) == 5);
}

TEST_CASE_METHOD(HelixTestFixture,
                 "PrinterState: Print state enum subject updates from notification",
                 "[state][enum]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    // Reset to known state first
    json standby_notification = {{"method", "notify_status_update"},
                                 {"params", {{{"print_stats", {{"state", "standby"}}}}, 0.0}}};
    state.update_from_status(standby_notification["params"][0]);

    SECTION("Updates to PRINTING from notification") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "printing"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);

        REQUIRE(state.print_state().get_print_job_state() == PrintJobState::PRINTING);
        REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
                static_cast<int>(PrintJobState::PRINTING));
    }

    SECTION("Updates to PAUSED from notification") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "paused"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);

        REQUIRE(state.print_state().get_print_job_state() == PrintJobState::PAUSED);
    }

    SECTION("Both string and enum subjects update together") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "complete"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);

        // String subject should have the raw string
        REQUIRE(std::string(lv_subject_get_string(state.print_state().get_print_state_subject())) ==
                "complete");
        // Enum subject should have the parsed enum value
        REQUIRE(state.print_state().get_print_job_state() == PrintJobState::COMPLETE);
    }
}

TEST_CASE("PrinterState: can_start_new_print logic", "[state][enum]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    SECTION("Can start from STANDBY") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "standby"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(state.print_state().can_start_new_print() == true);
    }

    SECTION("Can start from COMPLETE") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "complete"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(state.print_state().can_start_new_print() == true);
    }

    SECTION("Can start from CANCELLED") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "cancelled"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(state.print_state().can_start_new_print() == true);
    }

    SECTION("Can start from ERROR") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "error"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(state.print_state().can_start_new_print() == true);
    }

    SECTION("Cannot start from PRINTING") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "printing"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(state.print_state().can_start_new_print() == false);
    }

    SECTION("Cannot start from PAUSED") {
        json notification = {{"method", "notify_status_update"},
                             {"params", {{{"print_stats", {{"state", "paused"}}}}, 0.0}}};
        state.update_from_status(notification["params"][0]);
        REQUIRE(state.print_state().can_start_new_print() == false);
    }
}

TEST_CASE("PrinterState: Enum subject value reflects all state transitions", "[state][enum]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    state.init_subjects();

    // Test all state transitions are reflected in the enum subject

    // STANDBY
    json notification = {{"method", "notify_status_update"},
                         {"params", {{{"print_stats", {{"state", "standby"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::STANDBY));

    // PRINTING
    notification = {{"method", "notify_status_update"},
                    {"params", {{{"print_stats", {{"state", "printing"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::PRINTING));

    // PAUSED
    notification = {{"method", "notify_status_update"},
                    {"params", {{{"print_stats", {{"state", "paused"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::PAUSED));

    // COMPLETE
    notification = {{"method", "notify_status_update"},
                    {"params", {{{"print_stats", {{"state", "complete"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::COMPLETE));

    // CANCELLED
    notification = {{"method", "notify_status_update"},
                    {"params", {{{"print_stats", {{"state", "cancelled"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::CANCELLED));

    // ERROR
    notification = {{"method", "notify_status_update"},
                    {"params", {{{"print_stats", {{"state", "error"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.print_state().get_print_state_enum_subject()) ==
            static_cast<int>(PrintJobState::ERROR));
}

// ============================================================================
// KlippyState Tests
// ============================================================================

TEST_CASE("PrinterState: Klippy state initialization defaults to SHUTDOWN", "[state][klippy]") {
    PrinterState& state = fresh_state();

    // Default should be SHUTDOWN (2) - assume disconnected until confirmed ready
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::SHUTDOWN));
}

TEST_CASE("PrinterState: set_klippy_state_sync changes subject value", "[state][klippy]") {
    PrinterState& state = fresh_state();

    // Default should be SHUTDOWN (assume disconnected until confirmed)
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::SHUTDOWN));

    // Call set_klippy_state_sync (direct call, no async)
    state.set_klippy_state_sync(KlippyState::SHUTDOWN);

    // Subject should now be SHUTDOWN
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::SHUTDOWN));

    // Test other states
    state.set_klippy_state_sync(KlippyState::STARTUP);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::STARTUP));

    state.set_klippy_state_sync(KlippyState::ERROR);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::ERROR));

    state.set_klippy_state_sync(KlippyState::READY);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::READY));
}

TEST_CASE("PrinterState: Observer fires when klippy state changes", "[state][klippy][observer]") {
    PrinterState& state = fresh_state();

    // Register observer
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer = lv_subject_add_observer(
        state.network_state().get_klippy_state_subject(), observer_cb, user_data);

    // LVGL auto-notifies observers when first added (fires immediately with current value)
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == static_cast<int>(KlippyState::SHUTDOWN));

    // Change state via sync call (direct, no async)
    // Subject starts as SHUTDOWN, so set a different value to trigger notification
    state.set_klippy_state_sync(KlippyState::READY);

    REQUIRE(user_data[0] == 2);
    REQUIRE(user_data[1] == static_cast<int>(KlippyState::READY));

    // Change again
    state.set_klippy_state_sync(KlippyState::ERROR);

    REQUIRE(user_data[0] == 3);
    REQUIRE(user_data[1] == static_cast<int>(KlippyState::ERROR));

    lv_observer_remove(observer);
}

TEST_CASE("PrinterState: Update klippy state from webhooks notification",
          "[state][klippy][webhooks]") {
    PrinterState& state = fresh_state();

    // Test "startup" state (RESTART in progress)
    nlohmann::json notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"webhooks", {{"state", "startup"}, {"state_message", "Klipper restart"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::STARTUP));

    // Test "ready" state (restart complete)
    notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"webhooks", {{"state", "ready"}, {"state_message", "Printer is ready"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::READY));

    // Test "shutdown" state (M112 emergency stop)
    notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"webhooks", {{"state", "shutdown"}, {"state_message", "Emergency stop"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::SHUTDOWN));

    // Test "error" state (Klipper error)
    notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"webhooks", {{"state", "error"}, {"state_message", "Check klippy.log"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::ERROR));
}

TEST_CASE("PrinterState: Unknown webhooks state defaults to READY", "[state][klippy][webhooks]") {
    PrinterState& state = fresh_state();

    // Set to a known state first, then verify unknown doesn't change it
    state.set_klippy_state_sync(KlippyState::READY);
    nlohmann::json notification = {{"method", "notify_status_update"},
                                   {"params", {{{"webhooks", {{"state", "unknown_state"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);
    // Unknown state should remain READY (no change from previous value)
    REQUIRE(lv_subject_get_int(state.network_state().get_klippy_state_subject()) ==
            static_cast<int>(KlippyState::READY));
}

// ============================================================================
// Kinematics / Bed Moves Tests
// ============================================================================

TEST_CASE("PrinterState: set_kinematics decides whether the bed moves on Z",
          "[state][kinematics]") {
    // CoreXY machines (without QGL) carry Z on the bed; cartesian, delta and
    // CoreXZ carry it on the gantry or effector. Each row starts from the
    // opposite answer so the setter has to move the subject.
    struct Row {
        const char* kinematics;
        int bed_moves;
    };
    const Row rows[] = {
        {"corexy", 1},
        {"hybrid_corexy", 1}, // contains "corexy"
        {"cartesian", 0},
        {"delta", 0},
        {"corexz", 0}, // Voron Switchwire: gantry Z
        {"limited_cartesian", 0},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.kinematics) {
            PrinterState& state = fresh_state();
            lv_subject_t* bed_moves = state.capabilities_state().subject(Capability::BedMoves);
            // Gantry moves until a kinematics says otherwise.
            REQUIRE(lv_subject_get_int(bed_moves) == 0);
            state.set_kinematics(row.bed_moves ? "cartesian" : "corexy");
            REQUIRE(lv_subject_get_int(bed_moves) == 1 - row.bed_moves);
            state.set_kinematics(row.kinematics);
            CHECK(lv_subject_get_int(bed_moves) == row.bed_moves);
        }
    }
}

TEST_CASE("PrinterState: set_kinematics gives deltas no individual XYZ homing",
          "[state][kinematics]") {
    PrinterState& state = fresh_state();

    lv_subject_t* subject = state.capabilities_state().subject(Capability::HasIndividualXyzHoming);

    for (const char* delta : {"delta", "rotary_delta"}) {
        INFO(delta);
        state.set_kinematics("cartesian");
        REQUIRE(lv_subject_get_int(subject) == 1);
        state.set_kinematics(delta);
        CHECK(lv_subject_get_int(subject) == 0);
    }

    for (const char* other : {"cartesian", "corexy", "hybrid_corexy", "corexz"}) {
        INFO(other);
        state.set_kinematics("delta");
        REQUIRE(lv_subject_get_int(subject) == 0);
        state.set_kinematics(other);
        CHECK(lv_subject_get_int(subject) == 1);
    }
}

TEST_CASE("PrinterState: set_kinematics gates belt compare on true CoreXY", "[state][kinematics]") {
    PrinterState& state = fresh_state();

    REQUIRE(lv_subject_get_int(
                state.capabilities_state().subject(Capability::SupportsBeltCompare)) == 0);

    SECTION("corexz never arms it") {
        state.set_kinematics("corexz");
        REQUIRE(lv_subject_get_int(
                    state.capabilities_state().subject(Capability::SupportsBeltCompare)) == 0);
    }

    SECTION("corexy arms it and cartesian clears it") {
        state.set_kinematics("corexy");
        REQUIRE(lv_subject_get_int(
                    state.capabilities_state().subject(Capability::SupportsBeltCompare)) == 1);
        state.set_kinematics("cartesian");
        REQUIRE(lv_subject_get_int(
                    state.capabilities_state().subject(Capability::SupportsBeltCompare)) == 0);
    }

    SECTION("limited_corexy arms it") {
        state.set_kinematics("limited_corexy");
        REQUIRE(lv_subject_get_int(
                    state.capabilities_state().subject(Capability::SupportsBeltCompare)) == 1);
    }
}

namespace {
std::string firmware_name_subject_text() {
    lv_subject_t* s = lv_xml_get_subject(nullptr, "klipper_firmware_name");
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}
} // namespace

TEST_CASE("PrinterState: firmware name reads Klipper before any discovery",
          "[state][hardware][firmware]") {
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    REQUIRE(firmware_name_subject_text() == "Klipper");
}

TEST_CASE("PrinterState::set_hardware publishes the firmware name", "[state][hardware][firmware]") {
    SensorsForget forget;
    lv_init_safe();
    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(true);

    SECTION("Kalico") {
        PrinterDiscovery hw;
        hw.set_is_kalico(true);
        state.set_hardware(std::move(hw));
        REQUIRE(firmware_name_subject_text() == "Kalico");
    }

    SECTION("Klipper") {
        PrinterDiscovery hw;
        state.set_hardware(std::move(hw));
        REQUIRE(firmware_name_subject_text() == "Klipper");
    }
}

TEST_CASE("PrinterState: set_kinematics bumps the hardware generation",
          "[state][kinematics][hardware]") {
    PrinterState& state = fresh_state();

    lv_subject_t* gen = state.capabilities_state().subject(Capability::HardwareGeneration);
    const int before = lv_subject_get_int(gen);
    state.set_kinematics("corexy");
    REQUIRE(lv_subject_get_int(gen) != before);
}

TEST_CASE("PrinterState: Update kinematics from toolhead notification", "[state][kinematics][ui]") {
    PrinterState& state = fresh_state();

    // Send notification with toolhead kinematics
    nlohmann::json notification = {
        {"method", "notify_status_update"},
        {"params", {{{"toolhead", {{"kinematics", "cartesian"}, {"homed_axes", "xyz"}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);

    // Cartesian = gantry moves on Z
    REQUIRE(lv_subject_get_int(state.capabilities_state().subject(Capability::BedMoves)) == 0);
}

TEST_CASE("PrinterState: Kinematics update from cartesian notification",
          "[state][kinematics][ui]") {
    PrinterState& state = fresh_state();

    // First set to corexy (bed moves)
    state.set_kinematics("corexy");
    REQUIRE(lv_subject_get_int(state.capabilities_state().subject(Capability::BedMoves)) == 1);

    // Update to cartesian via notification
    nlohmann::json notification = {
        {"method", "notify_status_update"},
        {"params",
         {{{"toolhead", {{"kinematics", "cartesian"}, {"position", {0.0, 0.0, 0.0, 0.0}}}}}, 0.0}}};
    state.update_from_status(notification["params"][0]);

    // Should now be gantry-moves
    REQUIRE(lv_subject_get_int(state.capabilities_state().subject(Capability::BedMoves)) == 0);
}

TEST_CASE("PrinterState: Observer fires when bed_moves changes", "[state][kinematics][observer]") {
    PrinterState& state = fresh_state();

    // Register observer
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subject) {
        int* count_ptr = static_cast<int*>(lv_observer_get_user_data(observer));
        int* value_ptr = count_ptr + 1;

        (*count_ptr)++;
        *value_ptr = lv_subject_get_int(subject);
    };

    int user_data[2] = {0, -1}; // [callback_count, last_value]

    lv_observer_t* observer = lv_subject_add_observer(
        state.capabilities_state().subject(Capability::BedMoves), observer_cb, user_data);

    // LVGL auto-notifies observers when first added
    REQUIRE(user_data[0] == 1);
    REQUIRE(user_data[1] == 0); // Default: gantry moves

    // Change to corexy (bed moves)
    state.set_kinematics("corexy");
    REQUIRE(user_data[0] == 2);
    REQUIRE(user_data[1] == 1); // Now bed moves

    // Change to cartesian (gantry moves)
    state.set_kinematics("cartesian");
    REQUIRE(user_data[0] == 3);
    REQUIRE(user_data[1] == 0); // Back to gantry moves

    lv_observer_remove(observer);
}

// ============================================================================
// PrintOutcome Tests (set_print_outcome method - FAILING until implemented)
// ============================================================================

TEST_CASE("PrinterState: set_print_outcome updates subject", "[state][print_outcome]") {
    PrinterState& state = fresh_state();

    // Initial state should be NONE
    auto initial = static_cast<PrintOutcome>(
        lv_subject_get_int(state.print_state().get_print_outcome_subject()));
    REQUIRE(initial == PrintOutcome::NONE);

    // Set to CANCELLED - THIS SHOULD FAIL TO COMPILE (method doesn't exist yet)
    state.print_state().set_print_outcome(PrintOutcome::CANCELLED);
    UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

    auto after = static_cast<PrintOutcome>(
        lv_subject_get_int(state.print_state().get_print_outcome_subject()));
    REQUIRE(after == PrintOutcome::CANCELLED);
}

// ============================================================================
// Active Extruder / toolhead.extruder Parsing Tests
// ============================================================================

TEST_CASE("PrinterState: toolhead.extruder updates active extruder subjects",
          "[state][temp][active-extruder]") {
    PrinterState& state = fresh_state();

    // Set up two extruders
    state.temperature_state().init_extruders({"extruder", "extruder1"});

    // Set initial temperatures for both extruders
    json status1 = {{"extruder", {{"temperature", 200.0}, {"target", 210.0}}},
                    {"extruder1", {{"temperature", 150.0}, {"target", 160.0}}}};
    state.update_from_status(status1);

    // Active extruder defaults to "extruder" — verify those are the active values
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) ==
            2000);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_target_subject()) ==
            2100);

    // Now switch active extruder via toolhead.extruder
    json status2 = {{"toolhead", {{"extruder", "extruder1"}}}};
    state.update_from_status(status2);

    // Active subjects should now reflect extruder1's values
    REQUIRE(state.temperature_state().active_extruder_name() == "extruder1");
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) ==
            1500);
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_target_subject()) ==
            1600);
}

TEST_CASE("PrinterState: toolhead.extruder with unknown name keeps previous active",
          "[state][temp][active-extruder]") {
    PrinterState& state = fresh_state();

    state.temperature_state().init_extruders({"extruder"});

    json status = {{"extruder", {{"temperature", 205.0}, {"target", 210.0}}}};
    state.update_from_status(status);

    // Try to set unknown extruder — should be ignored
    json status2 = {{"toolhead", {{"extruder", "extruder_bogus"}}}};
    state.update_from_status(status2);

    // Active extruder should still be "extruder"
    REQUIRE(state.temperature_state().active_extruder_name() == "extruder");
    REQUIRE(lv_subject_get_int(state.temperature_state().get_active_extruder_temp_subject()) ==
            2050);
}

TEST_CASE("PrinterState: get_active_extruder_*_subject returns valid subjects",
          "[state][temp][active-extruder]") {
    PrinterState& state = fresh_state();

    // Active extruder subjects should be valid (non-null)
    REQUIRE(state.temperature_state().get_active_extruder_temp_subject() != nullptr);
    REQUIRE(state.temperature_state().get_active_extruder_target_subject() != nullptr);
}

// ============================================================================
// set_hardware() — chamber name propagation (regression: use-after-move)
// ============================================================================

TEST_CASE("PrinterState::set_hardware propagates auto-detected chamber sensor name",
          "[state][hardware][chamber][regression]") {
    PrinterState& state = fresh_state();

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_chamber_sensor_assignment("auto");
    settings.set_chamber_heater_assignment("auto");

    PrinterDiscovery hw;
    nlohmann::json objects = {"temperature_sensor chamber", "extruder", "heater_bed"};
    hw.parse_objects(objects);
    REQUIRE(hw.chamber_sensor_name() == "temperature_sensor chamber");

    state.set_hardware(std::move(hw));

    // Regression: `hardware` was read after std::move into discovery_, leaving these empty.
    REQUIRE(state.temperature_state().chamber_sensor_name() == "temperature_sensor chamber");
    REQUIRE(state.temperature_state().chamber_heater_name().empty());
}

TEST_CASE("PrinterState::set_hardware gives the chamber to its heater",
          "[state][hardware][chamber]") {
    SensorsForget forget;
    PrinterState& state = fresh_state();

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_chamber_sensor_assignment("auto");
    settings.set_chamber_heater_assignment("auto");

    PrinterDiscovery hw;
    nlohmann::json objects = {"heater_generic chamber", "temperature_sensor chamber", "extruder",
                              "heater_bed"};
    hw.parse_objects(objects);

    state.set_hardware(std::move(hw));

    REQUIRE(state.temperature_state().chamber_heater_name() == "heater_generic chamber");
    REQUIRE(state.temperature_state().chamber_sensor_name().empty());
}

TEST_CASE("PrinterState::set_hardware respects 'none' override for chamber",
          "[state][hardware][chamber]") {
    SensorsForget forget;
    PrinterState& state = fresh_state();

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_chamber_sensor_assignment("none");
    settings.set_chamber_heater_assignment("none");

    PrinterDiscovery hw;
    nlohmann::json objects = {"temperature_sensor chamber", "heater_generic chamber", "extruder",
                              "heater_bed"};
    hw.parse_objects(objects);

    state.set_hardware(std::move(hw));

    REQUIRE(state.temperature_state().chamber_sensor_name().empty());
    REQUIRE(state.temperature_state().chamber_heater_name().empty());

    settings.set_chamber_sensor_assignment("auto");
    settings.set_chamber_heater_assignment("auto");
}

// ============================================================================
// set_hardware(): a chamber heater assignment counts only while Klipper reports it
// ============================================================================

namespace {

/// Puts both chamber assignments back to "auto" however the case exits: the
/// SettingsManager singleton outlives every test in the shard.
struct ChamberAssignmentsRestore {
    ~ChamberAssignmentsRestore() {
        auto& settings = helix::SettingsManager::instance();
        settings.set_chamber_heater_assignment("auto");
        settings.set_chamber_sensor_assignment("auto");
    }
};

/// The name a model preset seeds for its family's chamber heater before the
/// wizard runs, and so before anything has checked the printer has one.
constexpr const char* kPresetChamberHeater = "heater_generic chamber_heater";

PrinterDiscovery discovered_objects(std::initializer_list<const char*> objects) {
    PrinterDiscovery hw;
    nlohmann::json list = nlohmann::json::array();
    for (const char* object : objects) {
        list.push_back(object);
    }
    hw.parse_objects(list);
    return hw;
}

/// The PrinterState singleton as a fresh session finds it: the assignments are
/// loaded and no discovery has landed yet.
PrinterState& state_before_discovery(const char* heater_assignment,
                                     const char* sensor_assignment = "auto") {
    PrinterState& state = fresh_state();

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();
    settings.set_chamber_sensor_assignment(sensor_assignment);
    settings.set_chamber_heater_assignment(heater_assignment);
    return state;
}

int has_chamber_heater(PrinterState& state) {
    return lv_subject_get_int(state.capabilities_state().subject(Capability::HasChamberHeater));
}

int has_chamber_sensor(PrinterState& state) {
    return lv_subject_get_int(PrinterStateTestAccess::has_chamber_sensor_subject(state));
}

/// A sensor's role, or nothing when the manager is not tracking that object.
std::optional<helix::sensors::TemperatureSensorRole>
role_of(helix::sensors::TemperatureSensorManager& sensors, const std::string& klipper_name) {
    for (const auto& sensor : sensors.get_sensors_sorted()) {
        if (sensor.klipper_name == klipper_name) {
            return std::optional<helix::sensors::TemperatureSensorRole>(sensor.role);
        }
    }
    return std::optional<helix::sensors::TemperatureSensorRole>{};
}

/// Every object an Elegoo Centauri Carbon on COSMOS reports apart from its gcode
/// macros: one chamber sensor, and neither a chamber heater nor a chamber-named fan.
PrinterDiscovery centauri_carbon_objects() {
    return discovered_objects({"gcode",
                               "webhooks",
                               "configfile",
                               "mcu",
                               "mcu bed",
                               "mcu hotend",
                               "gcode_button extruder_board_detect",
                               "gcode_move",
                               "print_stats",
                               "virtual_sdcard",
                               "pause_resume",
                               "display_status",
                               "exclude_object",
                               "idle_timeout",
                               "stepper_enable",
                               "tmc2209 stepper_x",
                               "tmc2209 stepper_y",
                               "tmc2209 stepper_z",
                               "tmc2209 extruder",
                               "heaters",
                               "heater_fan extruder",
                               "fan",
                               "heater_bed",
                               "bed_mesh",
                               "probe",
                               "load_cell_probe",
                               "filament_switch_sensor filament_sensor",
                               "led case",
                               "led hotend",
                               "temperature_sensor chamber",
                               "fan_generic aux_fan",
                               "fan_generic case_fan",
                               "temperature_sensor mcu_toolhead",
                               "temperature_sensor mcu_bed",
                               "temperature_host mainboard",
                               "temperature_fan mainboard",
                               "screws_tilt_adjust",
                               "telemetry",
                               "motion_report",
                               "query_endstops",
                               "system_stats",
                               "manual_probe",
                               "toolhead",
                               "extruder"});
}

} // namespace

TEST_CASE(
    "PrinterState::set_hardware: a preset chamber heater the printer lacks yields discovery's",
    "[state][hardware][chamber]") {
    ChamberAssignmentsRestore restore;
    PrinterState& state = state_before_discovery(kPresetChamberHeater);

    SECTION("a chamber-named temperature_fan drives the chamber") {
        state.set_hardware(
            discovered_objects({"temperature_fan chamber_fan", "temperature_sensor chamber_temp",
                                "extruder", "heater_bed"}));

        // The chamber did resolve: the fan this printer does have drives it,
        // and supplies the reading, so no probe holds the sensor role.
        CHECK(state.temperature_state().chamber_heater_name() == "temperature_fan chamber_fan");
        CHECK(state.temperature_state().chamber_sensor_name().empty());
        CHECK(has_chamber_heater(state) == 1);
    }
    SECTION("neither a heater nor a chamber fan leaves no chamber heater") {
        state.set_hardware(
            discovered_objects({"temperature_sensor chamber_temp", "extruder", "heater_bed"}));

        REQUIRE(state.temperature_state().chamber_sensor_name() ==
                "temperature_sensor chamber_temp");
        CHECK(state.temperature_state().chamber_heater_name().empty());
        CHECK(has_chamber_heater(state) == 0);
    }
}

TEST_CASE("PrinterState::set_hardware: a named chamber heater the printer reports is its heater",
          "[state][hardware][chamber]") {
    ChamberAssignmentsRestore restore;
    SensorsForget forget;

    SECTION("the preset's heater on a printer that has it") {
        PrinterState& state = state_before_discovery(kPresetChamberHeater);
        state.set_hardware(discovered_objects(
            {kPresetChamberHeater, "temperature_fan chamber_fan", "extruder", "heater_bed"}));

        CHECK(state.temperature_state().chamber_heater_name() == kPresetChamberHeater);
        CHECK(has_chamber_heater(state) == 1);
    }
    SECTION("a heater no chamber keyword names") {
        PrinterState& state = state_before_discovery("heater_generic ptc_heater");
        auto hw = discovered_objects({"heater_generic ptc_heater", "extruder", "heater_bed"});
        REQUIRE_FALSE(hw.has_chamber_heater());
        state.set_hardware(std::move(hw));

        CHECK(state.temperature_state().chamber_heater_name() == "heater_generic ptc_heater");
        CHECK(has_chamber_heater(state) == 1);
    }
}

TEST_CASE("PrinterState::set_hardware: an unreported chamber heater name falls back to discovery",
          "[state][hardware][chamber]") {
    ChamberAssignmentsRestore restore;
    SensorsForget forget;
    PrinterState& state = state_before_discovery(kPresetChamberHeater);

    state.set_hardware(discovered_objects({"heater_generic chamber", "extruder", "heater_bed"}));

    CHECK(state.temperature_state().chamber_heater_name() == "heater_generic chamber");
    CHECK(has_chamber_heater(state) == 1);
}

TEST_CASE("PrinterState: chamber heater presence follows each discovery, never the assignment",
          "[state][hardware][chamber]") {
    SensorsForget forget;
    ChamberAssignmentsRestore restore;
    PrinterState& state = state_before_discovery(kPresetChamberHeater);
    lv_subject_t* presence = state.capabilities_state().subject(Capability::HasChamberHeater);

    // Every value the capability takes from here on, including ones set and
    // replaced inside a single call.
    std::vector<int> seen;
    lv_observer_t* observer = lv_subject_add_observer(
        presence,
        [](lv_observer_t* obs, lv_subject_t* subject) {
            static_cast<std::vector<int>*>(lv_observer_get_user_data(obs))
                ->push_back(lv_subject_get_int(subject));
        },
        &seen);
    struct ObserverRemove {
        lv_observer_t* observer;
        ~ObserverRemove() {
            lv_observer_remove(observer);
        }
    } remove_observer{observer};

    // The assignment is loaded and discovery has not landed: no chamber heater.
    CHECK(state.temperature_state().chamber_heater_name().empty());
    CHECK(lv_subject_get_int(presence) == 0);

    // Discovery lands on a printer with neither the heater nor a chamber fan, and
    // the capability never shows.
    state.set_hardware(
        discovered_objects({"temperature_sensor chamber_temp", "extruder", "heater_bed"}));
    CHECK(lv_subject_get_int(presence) == 0);
    // The observer fired, so an absent 1 means one was never set.
    REQUIRE_FALSE(seen.empty());
    CHECK(std::find(seen.begin(), seen.end(), 1) == seen.end());

    // A later discovery reporting only a chamber-named temperature_fan: the fan
    // drives the chamber.
    state.set_hardware(
        discovered_objects({"temperature_fan chamber_fan", "extruder", "heater_bed"}));
    CHECK(state.temperature_state().chamber_heater_name() == "temperature_fan chamber_fan");
    CHECK(lv_subject_get_int(presence) == 1);

    // A later discovery that reports the heater makes it the chamber heater.
    state.set_hardware(discovered_objects({kPresetChamberHeater, "extruder", "heater_bed"}));
    CHECK(state.temperature_state().chamber_heater_name() == kPresetChamberHeater);
    CHECK(lv_subject_get_int(presence) == 1);

    // One that stops reporting it takes it away again.
    state.set_hardware(
        discovered_objects({"temperature_sensor chamber_temp", "extruder", "heater_bed"}));
    CHECK(state.temperature_state().chamber_heater_name().empty());
    CHECK(lv_subject_get_int(presence) == 0);
}

// ============================================================================
// set_hardware(): a chamber sensor assignment counts only while Klipper reports it
// ============================================================================

namespace {

/// A chamber sensor name an older model preset seeded, still saved on a printer
/// whose configuration names its chamber sensor differently.
constexpr const char* kStaleChamberSensor = "temperature_sensor box";

} // namespace

TEST_CASE("PrinterState::set_hardware: a chamber sensor the printer does not report yields "
          "discovery's",
          "[state][hardware][chamber]") {
    SensorsForget forget;
    ChamberAssignmentsRestore restore;
    PrinterState& state = state_before_discovery("auto", kStaleChamberSensor);

    SECTION("the chamber reads the sensor the printer has") {
        state.set_hardware(centauri_carbon_objects());

        REQUIRE(state.temperature_state().chamber_sensor_name() == "temperature_sensor chamber");
        CHECK(state.temperature_state().chamber_heater_name().empty());
        CHECK(has_chamber_sensor(state) == 1);

        json status = {{"temperature_sensor chamber", {{"temperature", 30.87}}}};
        state.update_from_status(status);
        // Decidegrees, truncated.
        CHECK(lv_subject_get_int(state.temperature_state().get_chamber_temp_subject()) == 308);
    }
    SECTION("a printer with no chamber sensor has none") {
        state.set_hardware(discovered_objects(
            {"temperature_sensor mcu_toolhead", "temperature_sensor mcu_bed",
             "temperature_host mainboard", "temperature_fan mainboard", "extruder", "heater_bed"}));

        CHECK(state.temperature_state().chamber_sensor_name().empty());
        CHECK(has_chamber_sensor(state) == 0);
    }
}

TEST_CASE("PrinterState::set_hardware: a named chamber sensor the printer reports is its sensor",
          "[state][hardware][chamber]") {
    ChamberAssignmentsRestore restore;
    PrinterState& state = state_before_discovery("auto", "temperature_sensor external_bme");

    auto hw = discovered_objects({"temperature_sensor external_bme", "temperature_sensor chamber",
                                  "extruder", "heater_bed"});
    REQUIRE(hw.chamber_sensor_name() == "temperature_sensor chamber");
    state.set_hardware(std::move(hw));

    CHECK(state.temperature_state().chamber_sensor_name() == "temperature_sensor external_bme");
    CHECK(has_chamber_sensor(state) == 1);
}

TEST_CASE("PrinterState::set_hardware: a named chamber sensor counts from the discovery that "
          "reports it",
          "[state][hardware][chamber]") {
    ChamberAssignmentsRestore restore;
    SensorsForget forget;
    PrinterState& state = state_before_discovery("auto", "temperature_sensor external_bme");

    // Discovery lands without the named sensor: discovery's pick stands.
    state.set_hardware(
        discovered_objects({"temperature_sensor chamber", "extruder", "heater_bed"}));
    CHECK(state.temperature_state().chamber_sensor_name() == "temperature_sensor chamber");
    CHECK(has_chamber_sensor(state) == 1);

    // A later discovery that reports it makes the saved name the chamber sensor.
    state.set_hardware(
        discovered_objects({"temperature_sensor external_bme", "temperature_sensor chamber",
                            "extruder", "heater_bed"}));
    CHECK(state.temperature_state().chamber_sensor_name() == "temperature_sensor external_bme");
    CHECK(has_chamber_sensor(state) == 1);

    // One that reports neither leaves the printer no chamber sensor.
    state.set_hardware(discovered_objects({"extruder", "heater_bed"}));
    CHECK(state.temperature_state().chamber_sensor_name().empty());
    CHECK(has_chamber_sensor(state) == 0);
}

TEST_CASE("PrinterState::set_hardware: a stale chamber sensor name leaves the reported sensor in "
          "the sensor list's chamber role",
          "[state][hardware][chamber]") {
    using helix::sensors::TemperatureSensorManager;
    using helix::sensors::TemperatureSensorRole;

    ChamberAssignmentsRestore restore;
    SensorsForget forget;
    PrinterState& state = state_before_discovery("auto", kStaleChamberSensor);

    auto& sensors = TemperatureSensorManager::instance();
    sensors.init_subjects();

    // Hardware discovery hands the manager the printer's sensors before discovery
    // completes and PrinterState resolves the chamber.
    PrinterDiscovery hw = centauri_carbon_objects();
    sensors.discover(hw);
    REQUIRE(role_of(sensors, "temperature_sensor chamber") == TemperatureSensorRole::CHAMBER);

    state.set_hardware(std::move(hw));

    REQUIRE(state.temperature_state().chamber_sensor_name() == "temperature_sensor chamber");
    CHECK(role_of(sensors, "temperature_sensor chamber") == TemperatureSensorRole::CHAMBER);
}

TEST_CASE("PrinterState::set_hardware: a stale chamber sensor override leaves the chamber "
          "to its heater",
          "[state][hardware][chamber]") {
    using helix::sensors::TemperatureSensorManager;
    using helix::sensors::TemperatureSensorRole;

    ChamberAssignmentsRestore restore;
    SensorsForget forget;
    PrinterState& state = state_before_discovery("auto", "temperature_sensor chamber_temp");

    auto& sensors = TemperatureSensorManager::instance();
    sensors.init_subjects();

    // The override outlived the hardware it named: this printer reports no chamber
    // temperature_sensor, only a chamber-named temperature_fan.
    PrinterDiscovery hw =
        discovered_objects({"heater_generic chamber_heater", "temperature_fan chamber_exhaust_fans",
                            "temperature_sensor mcu_temp", "extruder", "heater_bed"});
    sensors.discover(hw);
    REQUIRE(role_of(sensors, "temperature_fan chamber_exhaust_fans") ==
            TemperatureSensorRole::CHAMBER);

    state.set_hardware(std::move(hw));

    CHECK(state.temperature_state().chamber_sensor_name().empty());
    CHECK(has_chamber_sensor(state) == 0);
    // Released from the chamber role, the fan is listed as the sensor it is
    // rather than suppressed as a chamber readout it does not supply.
    CHECK(role_of(sensors, "temperature_fan chamber_exhaust_fans") !=
          TemperatureSensorRole::CHAMBER);
}

TEST_CASE("PrinterState::set_hardware: auto leaves a chamber-named fan its own sensor role",
          "[state][hardware][chamber]") {
    using helix::sensors::TemperatureSensorManager;
    using helix::sensors::TemperatureSensorRole;

    ChamberAssignmentsRestore restore;
    SensorsForget forget;
    PrinterState& state = state_before_discovery("auto", "auto");

    auto& sensors = TemperatureSensorManager::instance();
    sensors.init_subjects();

    PrinterDiscovery hw =
        discovered_objects({"heater_generic chamber_heater", "temperature_fan chamber_exhaust_fans",
                            "temperature_sensor mcu_temp", "extruder", "heater_bed"});
    sensors.discover(hw);
    REQUIRE(role_of(sensors, "temperature_fan chamber_exhaust_fans") ==
            TemperatureSensorRole::CHAMBER);

    state.set_hardware(std::move(hw));

    CHECK(state.temperature_state().chamber_sensor_name().empty());
    CHECK(has_chamber_sensor(state) == 0);
    // Released from the chamber role, the fan is listed as the sensor it is
    // rather than suppressed as a chamber readout it does not supply.
    CHECK(role_of(sensors, "temperature_fan chamber_exhaust_fans") !=
          TemperatureSensorRole::CHAMBER);
}

TEST_CASE("PrinterState::set_hardware: a chamber sensor override the printer reports still wins "
          "over a discovered chamber sensor",
          "[state][hardware][chamber]") {
    using helix::sensors::TemperatureSensorRole;

    ChamberAssignmentsRestore restore;
    SensorsForget forget;
    PrinterState& state = state_before_discovery("auto", "temperature_sensor external_bme");

    auto& sensors = helix::sensors::TemperatureSensorManager::instance();
    sensors.init_subjects();
    PrinterDiscovery hw =
        discovered_objects({"temperature_fan chamber_exhaust_fans",
                            "temperature_sensor external_bme", "extruder", "heater_bed"});
    sensors.discover(hw);
    // Discovery classified the fan CHAMBER on its own; the reported override must
    // take the role from it.
    REQUIRE(role_of(sensors, "temperature_fan chamber_exhaust_fans") ==
            TemperatureSensorRole::CHAMBER);

    state.set_hardware(std::move(hw));

    CHECK(state.temperature_state().chamber_sensor_name() == "temperature_sensor external_bme");
    CHECK(has_chamber_sensor(state) == 1);
    CHECK(role_of(sensors, "temperature_sensor external_bme") == TemperatureSensorRole::CHAMBER);
    CHECK(role_of(sensors, "temperature_fan chamber_exhaust_fans") !=
          TemperatureSensorRole::CHAMBER);
}

// ============================================================================
// Subscription-restricted-null safety
// ============================================================================
//
// Background: Moonraker delivers JSON null for subscribed fields the underlying
// Klipper object lacks (e.g. Snapmaker U1's filament_motion_sensor lacks
// detection_count). nlohmann::json::value("k", default) and unguarded .get<T>()
// throw type_error.302 on null. An uncaught throw inside a subscription
// handler unwinds out of run() into main()'s top-level catch, exiting 134
// and triggering a watchdog crash loop (#filament_motion_sensor, f75b961d8).
//
// These tests inject null for every field a parser reads and assert no
// exception escapes — converting "the next narrow-subscription rollout
// crashes Snapmaker users" into a CI failure.

TEST_CASE("parse_status_notification: only a notify_status_update yields a frame",
          "[state][regression][subscription-null]") {
    const auto params = nlohmann::json::array({nlohmann::json::object()});
    const nlohmann::json rejected[] = {
        {{"method", nullptr}, {"params", params}},
        {{"params", params}},
        {{"method", 42}, {"params", params}},
        {{"method", "notify_gcode_response"}, {"params", params}},
        {{"method", "notify_status_update"}, {"params", nlohmann::json::array()}},
        {{"method", "notify_status_update"}},
    };
    for (const auto& notification : rejected) {
        CAPTURE(notification.dump());
        CHECK_FALSE(parse_status_notification(notification));
    }
}

TEST_CASE("parse_status_notification: reads eventtime and the replay marker",
          "[state][klippy][freshness]") {
    const nlohmann::json status = {{"webhooks", {{"state", "ready"}}}};

    const nlohmann::json live_frame = {{"method", "notify_status_update"},
                                       {"params", nlohmann::json::array({status, 42.5})}};
    auto live = parse_status_notification(live_frame);
    REQUIRE(live);
    CHECK(*live->status == status);
    CHECK(live->eventtime == 42.5);
    CHECK_FALSE(live->from_cached_snapshot);

    const nlohmann::json replay_frame = {{"method", "notify_status_update"},
                                         {"params", nlohmann::json::array({status})},
                                         {kCachedSnapshotMarker, true}};
    auto replay = parse_status_notification(replay_frame);
    REQUIRE(replay);
    CHECK(replay->eventtime == 0.0);
    CHECK(replay->from_cached_snapshot);

    const nlohmann::json untimed_frame = {{"method", "notify_status_update"},
                                          {"params", nlohmann::json::array({status, "x"})}};
    auto untimed = parse_status_notification(untimed_frame);
    REQUIRE(untimed);
    CHECK(untimed->eventtime == 0.0);
}

TEST_CASE("make_status_notification round-trips through parse_status_notification",
          "[state][klippy][freshness]") {
    const nlohmann::json status = {{"print_stats", {{"state", "cancelled"}}},
                                   {"display_status", {{"message", "Print starting..."}}}};

    for (bool cached : {false, true}) {
        for (bool whole : {false, true}) {
            CAPTURE(cached, whole);
            const nlohmann::json notification = make_status_notification(status, cached, whole);
            auto frame = parse_status_notification(notification);
            REQUIRE(frame);
            CHECK(*frame->status == status);
            CHECK(frame->eventtime == 0.0);
            CHECK(frame->from_cached_snapshot == cached);
            CHECK(frame->whole_objects == whole);
            // A marker is present only when set: live frames carry neither key.
            CHECK(notification.contains(kCachedSnapshotMarker) == cached);
            CHECK(notification.contains(kWholeObjectsMarker) == whole);
        }
    }
}

TEST_CASE("PrinterPrintState: update_from_status handles null print_stats fields",
          "[state][regression][subscription-null]") {
    lv_init_safe();
    auto& state = get_printer_state();

    // Field-restricted subscription delivers print_stats with state and filename
    // present but null — exactly what Moonraker sends when the underlying
    // Klipper object lacks the field. Pre-fix: get<std::string>() on null
    // threw type_error.302. Post-fix: skip silently.
    nlohmann::json status = {
        {"print_stats",
         {
             {"state", nullptr},
             {"filename", nullptr},
             {"print_duration", nullptr},
             {"total_duration", nullptr},
             {"filament_used", nullptr},
         }},
    };
    REQUIRE_NOTHROW(state.update_from_status(status));

    // Wrong types should also be tolerated (Moonraker may send unexpected
    // types during firmware restart races).
    nlohmann::json wrong_types = {
        {"print_stats",
         {
             {"state", 42},
             {"filename", true},
         }},
    };
    REQUIRE_NOTHROW(state.update_from_status(wrong_types));
}

TEST_CASE("PrinterState tracks pause_resume.is_paused", "[printer-state][pause]") {
    PrinterState& state = fresh_state();

    SECTION("defaults to false before any status update") {
        REQUIRE_FALSE(state.is_paused());
    }

    SECTION("becomes true when Klipper reports is_paused:true") {
        json status = {{"pause_resume", {{"is_paused", true}}}};
        state.update_from_status(status);
        REQUIRE(state.is_paused());
    }

    SECTION("becomes false when Klipper reports is_paused:false") {
        // First set to true
        json status_true = {{"pause_resume", {{"is_paused", true}}}};
        state.update_from_status(status_true);
        REQUIRE(state.is_paused());

        // Then clear it
        json status_false = {{"pause_resume", {{"is_paused", false}}}};
        state.update_from_status(status_false);
        REQUIRE_FALSE(state.is_paused());
    }

    SECTION("ignores pause_resume update with missing is_paused field") {
        json status = {{"pause_resume", {{"other_field", 42}}}};
        state.update_from_status(status);
        REQUIRE_FALSE(state.is_paused()); // unchanged from default
    }

    SECTION("ignores pause_resume update with non-boolean is_paused") {
        json status = {{"pause_resume", {{"is_paused", 1}}}};
        state.update_from_status(status);
        REQUIRE_FALSE(state.is_paused()); // unchanged: integer, not boolean
    }
}
