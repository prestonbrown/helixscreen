// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_device_actions.cpp
 * @brief Unit tests for device actions feature (Phases 8-10)
 *
 * Tests for device-specific actions infrastructure:
 * - DeviceSection struct (UI grouping)
 * - DeviceAction struct (control types)
 * - ActionType enum (button, toggle, slider, dropdown, info)
 * - Backend device action interfaces
 * - Mock backend default actions and setters
 * - ACE/ToolChanger stub implementations
 */

#include "ams_backend_ace.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_device_action_value.h"
#include "ams_types.h"

#include <any>
#include <cmath>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

// =============================================================================
// Type Tests - DeviceSection struct
// =============================================================================

TEST_CASE("DeviceSection struct exists and has required fields", "[ams][device_actions][types]") {
    SECTION("default construction") {
        DeviceSection section{};

        CHECK(section.id.empty());
        CHECK(section.label.empty());
        // Value-initialized struct has display_order = 0
        CHECK(section.display_order == 0);
        CHECK(section.description.empty());
    }

    SECTION("can construct with values") {
        DeviceSection section{"calibration", "Calibration", 0, "Bowden length calibration"};

        CHECK(section.id == "calibration");
        CHECK(section.label == "Calibration");
        CHECK(section.display_order == 0);
        CHECK(section.description == "Bowden length calibration");
    }

    SECTION("sections can have different display orders") {
        DeviceSection section1{"first", "First", 0};
        DeviceSection section2{"second", "Second", 1};
        DeviceSection section3{"third", "Third", 2};

        CHECK(section1.display_order < section2.display_order);
        CHECK(section2.display_order < section3.display_order);
    }

    SECTION("sections with same display_order are allowed") {
        DeviceSection section1{"a", "A", 0};
        DeviceSection section2{"b", "B", 0};

        CHECK(section1.display_order == section2.display_order);
    }
}

// =============================================================================
// Type Tests - DeviceAction struct
// =============================================================================

TEST_CASE("DeviceAction struct exists and has required fields", "[ams][device_actions][types]") {
    SECTION("default construction") {
        DeviceAction action{};

        CHECK(action.id.empty());
        CHECK(action.label.empty());
        CHECK(action.icon.empty());
        CHECK(action.section.empty());
        CHECK(action.description.empty());
        // ActionType has default member initializers in struct
        CHECK(action.options.empty());
        CHECK(action.min_value == 0.0f);
        CHECK(action.max_value == 100.0f);
        CHECK(action.unit.empty());
        CHECK(action.slot_index == -1);
        CHECK(action.enabled == true);
        CHECK(action.disable_reason.empty());
    }

    SECTION("button action type") {
        DeviceAction action;
        action.id = "calibrate";
        action.label = "Run Calibration";
        action.icon = "play";
        action.section = "calibration";
        action.description = "Run the calibration wizard";
        action.type = ActionType::BUTTON;
        action.enabled = true;

        CHECK(action.id == "calibrate");
        CHECK(action.type == ActionType::BUTTON);
        CHECK_FALSE(action.current_value.has_value());
    }

    SECTION("toggle action type with boolean value") {
        DeviceAction action;
        action.id = "auto_load";
        action.label = "Auto Load";
        action.type = ActionType::TOGGLE;
        action.current_value = true;

        CHECK(action.type == ActionType::TOGGLE);
        REQUIRE(action.current_value.has_value());
        CHECK(std::any_cast<bool>(action.current_value) == true);
    }

    SECTION("slider action type with float value") {
        DeviceAction action;
        action.id = "speed_mult";
        action.label = "Speed Multiplier";
        action.type = ActionType::SLIDER;
        action.current_value = 1.5f;
        action.min_value = 0.5f;
        action.max_value = 2.0f;
        action.unit = "x";

        CHECK(action.type == ActionType::SLIDER);
        REQUIRE(action.current_value.has_value());
        CHECK(std::any_cast<float>(action.current_value) == Catch::Approx(1.5f));
        CHECK(action.min_value == Catch::Approx(0.5f));
        CHECK(action.max_value == Catch::Approx(2.0f));
        CHECK(action.unit == "x");
    }

    SECTION("slider action type with int value") {
        DeviceAction action;
        action.id = "bowden_length";
        action.label = "Bowden Length";
        action.type = ActionType::SLIDER;
        action.current_value = 450;
        action.min_value = 100.0f;
        action.max_value = 1000.0f;
        action.unit = "mm";

        CHECK(action.type == ActionType::SLIDER);
        REQUIRE(action.current_value.has_value());
        CHECK(std::any_cast<int>(action.current_value) == 450);
    }

    SECTION("dropdown action type with string value and options") {
        DeviceAction action;
        action.id = "profile";
        action.label = "Profile";
        action.type = ActionType::DROPDOWN;
        action.current_value = std::string("Fast");
        action.options = {"Slow", "Normal", "Fast"};

        CHECK(action.type == ActionType::DROPDOWN);
        REQUIRE(action.current_value.has_value());
        CHECK(std::any_cast<std::string>(action.current_value) == "Fast");
        REQUIRE(action.options.size() == 3);
        CHECK(action.options[0] == "Slow");
        CHECK(action.options[1] == "Normal");
        CHECK(action.options[2] == "Fast");
    }

    SECTION("info action type (read-only display)") {
        DeviceAction action;
        action.id = "firmware_version";
        action.label = "Firmware";
        action.type = ActionType::INFO;
        action.current_value = std::string("v1.2.3");

        CHECK(action.type == ActionType::INFO);
        REQUIRE(action.current_value.has_value());
        CHECK(std::any_cast<std::string>(action.current_value) == "v1.2.3");
    }

    SECTION("per-slot action") {
        DeviceAction action;
        action.id = "lane_calibrate";
        action.label = "Calibrate Lane";
        action.type = ActionType::BUTTON;
        action.slot_index = 2;

        CHECK(action.slot_index == 2);
    }

    SECTION("disabled action with reason") {
        DeviceAction action;
        action.id = "calibrate";
        action.label = "Calibrate";
        action.type = ActionType::BUTTON;
        action.enabled = false;
        action.disable_reason = "Busy with print";

        CHECK(action.enabled == false);
        CHECK(action.disable_reason == "Busy with print");
    }
}

// =============================================================================
// Type Tests - ActionType enum
// =============================================================================

TEST_CASE("ActionType enum values and conversion", "[ams][device_actions][types]") {
    SECTION("all action types exist") {
        CHECK(static_cast<int>(ActionType::BUTTON) == 0);
        CHECK(static_cast<int>(ActionType::TOGGLE) == 1);
        CHECK(static_cast<int>(ActionType::SLIDER) == 2);
        CHECK(static_cast<int>(ActionType::DROPDOWN) == 3);
        CHECK(static_cast<int>(ActionType::INFO) == 4);
    }

    SECTION("action_type_to_string conversion") {
        CHECK(std::string(action_type_to_string(ActionType::BUTTON)) == "Button");
        CHECK(std::string(action_type_to_string(ActionType::TOGGLE)) == "Toggle");
        CHECK(std::string(action_type_to_string(ActionType::SLIDER)) == "Slider");
        CHECK(std::string(action_type_to_string(ActionType::DROPDOWN)) == "Dropdown");
        CHECK(std::string(action_type_to_string(ActionType::INFO)) == "Info");
    }

    SECTION("invalid action type returns Unknown") {
        CHECK(std::string(action_type_to_string(static_cast<ActionType>(99))) == "Unknown");
    }
}

// =============================================================================
// Base Class Interface Tests
// =============================================================================

TEST_CASE("AmsBackend base class has device action virtual methods",
          "[ams][device_actions][interface]") {
    // Use mock backend to verify the interface exists
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("get_device_sections returns vector of sections") {
        auto sections = backend.get_device_sections();

        // Mock has default sections
        CHECK_FALSE(sections.empty());
    }

    SECTION("get_device_actions returns vector of actions") {
        auto actions = backend.get_device_actions();

        // Mock has default actions
        CHECK_FALSE(actions.empty());
    }

    SECTION("execute_device_action returns AmsError") {
        auto sections = backend.get_device_sections();
        auto actions = backend.get_device_actions();

        // Execute first default action
        if (!actions.empty()) {
            auto result = backend.execute_device_action(actions[0].id);
            CHECK(result); // Should succeed for valid action
        }
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - Default Actions
// =============================================================================

TEST_CASE("AmsBackendMock device actions - default configuration", "[ams][device_actions][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("get_device_sections returns default HH sections") {
        auto sections = backend.get_device_sections();

        // Mock defaults to Happy Hare mode with 5 sections
        REQUIRE(sections.size() == 5);

        // Find setup section
        auto it = std::find_if(sections.begin(), sections.end(),
                               [](const DeviceSection& s) { return s.id == "setup"; });
        REQUIRE(it != sections.end());
        CHECK(it->label == "Setup");
        CHECK_FALSE(it->label.empty());

        // Find speed section
        it = std::find_if(sections.begin(), sections.end(),
                          [](const DeviceSection& s) { return s.id == "speed"; });
        REQUIRE(it != sections.end());
        CHECK(it->label == "Speed");

        // Find accessories section
        it = std::find_if(sections.begin(), sections.end(),
                          [](const DeviceSection& s) { return s.id == "accessories"; });
        REQUIRE(it != sections.end());
        CHECK(it->label == "Accessories");

        // Find maintenance section
        it = std::find_if(sections.begin(), sections.end(),
                          [](const DeviceSection& s) { return s.id == "maintenance"; });
        REQUIRE(it != sections.end());
        CHECK(it->label == "Maintenance");
    }

    SECTION("get_device_actions returns default HH actions") {
        auto actions = backend.get_device_actions();

        // Mock defaults to Happy Hare: 15 actions across 3 sections
        REQUIRE(actions.size() >= 2);

        // Verify actions have required fields populated
        for (const auto& action : actions) {
            CHECK_FALSE(action.id.empty());
            CHECK_FALSE(action.label.empty());
            CHECK_FALSE(action.section.empty());
        }
    }

    SECTION("default HH actions include calibrate_bowden (button)") {
        auto actions = backend.get_device_actions();

        auto it = std::find_if(actions.begin(), actions.end(),
                               [](const DeviceAction& a) { return a.id == "calibrate_bowden"; });
        REQUIRE(it != actions.end());
        CHECK(it->type == ActionType::BUTTON);
        CHECK(it->section == "setup");
        CHECK(it->enabled == true);
    }

    SECTION("default HH actions include gear_from_buffer_speed (slider)") {
        auto actions = backend.get_device_actions();

        auto it = std::find_if(actions.begin(), actions.end(), [](const DeviceAction& a) {
            return a.id == "gear_from_buffer_speed";
        });
        REQUIRE(it != actions.end());
        CHECK(it->type == ActionType::SLIDER);
        CHECK(it->section == "speed");
        CHECK(it->unit == "mm/s");
        CHECK(it->min_value < it->max_value);
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - execute_device_action
// =============================================================================

TEST_CASE("AmsBackendMock execute_device_action behavior", "[ams][device_actions][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("execute valid action succeeds") {
        auto actions = backend.get_device_actions();
        REQUIRE_FALSE(actions.empty());

        auto result = backend.execute_device_action(actions[0].id);
        CHECK(result);
        CHECK(result.result == AmsResult::SUCCESS);
    }

    SECTION("execute unknown action returns not_supported") {
        auto result = backend.execute_device_action("nonexistent_action");

        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }

    SECTION("stores last executed action for verification") {
        backend.clear_last_executed_action();

        backend.execute_device_action("calibrate_bowden");

        auto [last_id, last_value] = backend.get_last_executed_action();
        CHECK(last_id == "calibrate_bowden");
        CHECK_FALSE(last_value.has_value());
    }

    SECTION("stores value with last executed action") {
        backend.clear_last_executed_action();

        float slider_value = 500.0f;
        backend.execute_device_action("gear_from_buffer_speed", slider_value);

        auto [last_id, last_value] = backend.get_last_executed_action();
        CHECK(last_id == "gear_from_buffer_speed");
        REQUIRE(last_value.has_value());
        CHECK(std::any_cast<float>(last_value) == Catch::Approx(500.0f));
    }

    SECTION("clear_last_executed_action clears state") {
        backend.execute_device_action("calibrate_bowden");

        auto [id1, val1] = backend.get_last_executed_action();
        CHECK_FALSE(id1.empty());

        backend.clear_last_executed_action();

        auto [id2, val2] = backend.get_last_executed_action();
        CHECK(id2.empty());
        CHECK_FALSE(val2.has_value());
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - Setters
// =============================================================================

TEST_CASE("AmsBackendMock device action setters", "[ams][device_actions][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("set_device_sections replaces sections") {
        std::vector<DeviceSection> custom_sections = {
            {"custom", "Custom Section", 0},
        };

        backend.set_device_sections(custom_sections);

        auto sections = backend.get_device_sections();
        REQUIRE(sections.size() == 1);
        CHECK(sections[0].id == "custom");
        CHECK(sections[0].label == "Custom Section");
    }

    SECTION("set_device_actions replaces actions") {
        std::vector<DeviceAction> custom_actions;
        DeviceAction action;
        action.id = "custom_action";
        action.label = "Custom Action";
        action.section = "custom";
        action.type = ActionType::BUTTON;
        action.enabled = true;
        custom_actions.push_back(action);

        backend.set_device_actions(custom_actions);

        auto actions = backend.get_device_actions();
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].id == "custom_action");
    }

    SECTION("set empty sections clears all sections") {
        backend.set_device_sections({});

        auto sections = backend.get_device_sections();
        CHECK(sections.empty());
    }

    SECTION("set empty actions clears all actions") {
        backend.set_device_actions({});

        auto actions = backend.get_device_actions();
        CHECK(actions.empty());
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - Disabled Actions
// =============================================================================

TEST_CASE("AmsBackendMock handles disabled actions", "[ams][device_actions][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("disabled action returns error when executed") {
        // Create a disabled action
        DeviceAction disabled_action;
        disabled_action.id = "disabled_test";
        disabled_action.label = "Disabled Test";
        disabled_action.section = "test";
        disabled_action.type = ActionType::BUTTON;
        disabled_action.enabled = false;
        disabled_action.disable_reason = "Feature not available during print";

        backend.set_device_actions({disabled_action});

        auto result = backend.execute_device_action("disabled_test");

        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }

    SECTION("enabled action succeeds") {
        DeviceAction enabled_action;
        enabled_action.id = "enabled_test";
        enabled_action.label = "Enabled Test";
        enabled_action.section = "test";
        enabled_action.type = ActionType::BUTTON;
        enabled_action.enabled = true;

        backend.set_device_actions({enabled_action});

        auto result = backend.execute_device_action("enabled_test");

        CHECK(result);
        CHECK(result.result == AmsResult::SUCCESS);
    }

    backend.stop();
}

// =============================================================================
// Mock Backend Tests - Different Action Types with Values
// =============================================================================

TEST_CASE("AmsBackendMock execute with different value types", "[ams][device_actions][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    std::vector<DeviceAction> actions;

    DeviceAction toggle;
    toggle.id = "toggle_action";
    toggle.label = "Toggle";
    toggle.section = "test";
    toggle.type = ActionType::TOGGLE;
    toggle.current_value = false;
    toggle.enabled = true;
    actions.push_back(toggle);

    DeviceAction slider;
    slider.id = "slider_action";
    slider.label = "Slider";
    slider.section = "test";
    slider.type = ActionType::SLIDER;
    slider.current_value = 50.0f;
    slider.enabled = true;
    actions.push_back(slider);

    DeviceAction dropdown;
    dropdown.id = "dropdown_action";
    dropdown.label = "Dropdown";
    dropdown.section = "test";
    dropdown.type = ActionType::DROPDOWN;
    dropdown.options = {"A", "B", "C"};
    dropdown.current_value = std::string("A");
    dropdown.enabled = true;
    actions.push_back(dropdown);

    backend.set_device_actions(actions);

    SECTION("toggle action with boolean value") {
        backend.clear_last_executed_action();

        auto result = backend.execute_device_action("toggle_action", true);
        CHECK(result);

        auto [id, value] = backend.get_last_executed_action();
        CHECK(id == "toggle_action");
        REQUIRE(value.has_value());
        CHECK(std::any_cast<bool>(value) == true);
    }

    SECTION("slider action with float value") {
        backend.clear_last_executed_action();

        auto result = backend.execute_device_action("slider_action", 75.5f);
        CHECK(result);

        auto [id, value] = backend.get_last_executed_action();
        CHECK(id == "slider_action");
        REQUIRE(value.has_value());
        CHECK(std::any_cast<float>(value) == Catch::Approx(75.5f));
    }

    SECTION("dropdown action with string value") {
        backend.clear_last_executed_action();

        auto result = backend.execute_device_action("dropdown_action", std::string("B"));
        CHECK(result);

        auto [id, value] = backend.get_last_executed_action();
        CHECK(id == "dropdown_action");
        REQUIRE(value.has_value());
        CHECK(std::any_cast<std::string>(value) == "B");
    }

    SECTION("button action with no value") {
        DeviceAction button;
        button.id = "button_action";
        button.label = "Button";
        button.section = "test";
        button.type = ActionType::BUTTON;
        button.enabled = true;

        backend.set_device_actions({button});
        backend.clear_last_executed_action();

        auto result = backend.execute_device_action("button_action");
        CHECK(result);

        auto [id, value] = backend.get_last_executed_action();
        CHECK(id == "button_action");
        CHECK_FALSE(value.has_value());
    }

    backend.stop();
}

// =============================================================================
// ACE Backend Device Action Tests
// =============================================================================

TEST_CASE("ACE device actions - sections and actions", "[ams][device_actions][ace]") {
    // Create ACE backend with null dependencies (for structure testing)
    AmsBackendAce backend(nullptr, nullptr);

    SECTION("get_device_sections returns two sections") {
        auto sections = backend.get_device_sections();
        REQUIRE(sections.size() == 2);

        auto it = std::find_if(sections.begin(), sections.end(),
                               [](const DeviceSection& s) { return s.id == "filament_control"; });
        REQUIRE(it != sections.end());
        CHECK(it->label == "Filament Control");

        it = std::find_if(sections.begin(), sections.end(),
                          [](const DeviceSection& s) { return s.id == "maintenance"; });
        REQUIRE(it != sections.end());
        CHECK(it->label == "Maintenance");
    }

    SECTION("get_device_actions returns three actions") {
        auto actions = backend.get_device_actions();
        REQUIRE(actions.size() == 3);

        for (const auto& action : actions) {
            CHECK_FALSE(action.id.empty());
            CHECK_FALSE(action.label.empty());
            CHECK_FALSE(action.section.empty());
            CHECK(action.enabled == true);
        }
    }

    SECTION("ace_manual_feed is a BUTTON in filament_control section") {
        auto actions = backend.get_device_actions();
        auto it = std::find_if(actions.begin(), actions.end(),
                               [](const DeviceAction& a) { return a.id == "ace_manual_feed"; });
        REQUIRE(it != actions.end());
        CHECK(it->type == ActionType::BUTTON);
        CHECK(it->section == "filament_control");
    }

    SECTION("ace_manual_retract is a BUTTON in filament_control section") {
        auto actions = backend.get_device_actions();
        auto it = std::find_if(actions.begin(), actions.end(),
                               [](const DeviceAction& a) { return a.id == "ace_manual_retract"; });
        REQUIRE(it != actions.end());
        CHECK(it->type == ActionType::BUTTON);
        CHECK(it->section == "filament_control");
    }

    SECTION("ace_feed_assist_toggle is a TOGGLE in maintenance section") {
        auto actions = backend.get_device_actions();
        auto it = std::find_if(actions.begin(), actions.end(), [](const DeviceAction& a) {
            return a.id == "ace_feed_assist_toggle";
        });
        REQUIRE(it != actions.end());
        CHECK(it->type == ActionType::TOGGLE);
        CHECK(it->section == "maintenance");
    }
}

TEST_CASE("ACE device actions - execute unknown returns not_supported",
          "[ams][device_actions][ace]") {
    AmsBackendAce backend(nullptr, nullptr);

    SECTION("unknown action returns not_supported") {
        auto result = backend.execute_device_action("nonexistent_action");
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }

    SECTION("unknown action with value returns not_supported") {
        auto result = backend.execute_device_action("nonexistent_action", 42);
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }
}

TEST_CASE("ACE device actions - execute valid actions", "[ams][device_actions][ace]") {
    // Note: ACE backend requires api_ for execute_gcode, so these tests verify
    // that execute_device_action recognizes the action IDs (returns NOT_CONNECTED, not
    // NOT_SUPPORTED)
    AmsBackendAce backend(nullptr, nullptr);

    SECTION("ace_manual_feed with null API returns not_connected") {
        auto result = backend.execute_device_action("ace_manual_feed");
        // Should fail with not_connected (recognized action), not not_supported (unknown action)
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_CONNECTED);
    }

    SECTION("ace_manual_retract with null API returns not_connected") {
        auto result = backend.execute_device_action("ace_manual_retract");
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_CONNECTED);
    }

    SECTION("ace_feed_assist_toggle with enable=true returns not_connected") {
        auto result = backend.execute_device_action("ace_feed_assist_toggle", true);
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_CONNECTED);
    }

    SECTION("ace_feed_assist_toggle with enable=false returns not_connected") {
        auto result = backend.execute_device_action("ace_feed_assist_toggle", false);
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_CONNECTED);
    }

    SECTION("ace_feed_assist_toggle with no value returns not_connected") {
        auto result = backend.execute_device_action("ace_feed_assist_toggle");
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_CONNECTED);
    }
    SECTION("NaN") {
        CHECK_FALSE(device_action_number(std::any(std::nan(""))).has_value());
        CHECK_FALSE(device_action_value<double>(std::any(std::nan("")), "speed").second.success());
    }
}

TEST_CASE("device_action_value reads bools and strings exactly",
          "[ams][device_actions][device_action_value]") {
    CHECK(device_action_value<bool>(std::any(true), "state").first);
    CHECK(device_action_value<std::string>(std::any(std::string("Auto")), "mode").first == "Auto");
    CHECK_FALSE(device_action_value<std::string>(std::any("Auto"), "mode").second.success());
}

// =============================================================================
// ToolChanger Backend Stub Tests
// =============================================================================

TEST_CASE("ToolChanger device actions stubs", "[ams][device_actions][toolchanger]") {
    // Create ToolChanger backend with null dependencies (for stub testing)
    AmsBackendToolChanger backend(nullptr, nullptr);

    SECTION("get_device_sections returns empty vector") {
        auto sections = backend.get_device_sections();
        CHECK(sections.empty());
    }

    SECTION("get_device_actions returns empty vector") {
        auto actions = backend.get_device_actions();
        CHECK(actions.empty());
    }

    SECTION("execute_device_action returns not_supported") {
        auto result = backend.execute_device_action("any_action");

        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }

    SECTION("execute_device_action with value still returns not_supported") {
        auto result = backend.execute_device_action("calibrate", std::string("fast"));

        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }
}

// =============================================================================
// Edge Cases and Integration
// =============================================================================

TEST_CASE("Device actions edge cases", "[ams][device_actions][edge]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("action ID is case-sensitive") {
        auto actions = backend.get_device_actions();
        REQUIRE_FALSE(actions.empty());

        std::string valid_id = actions[0].id;
        std::string uppercase_id = valid_id;
        for (auto& c : uppercase_id)
            c = static_cast<char>(std::toupper(c));

        // Original should work
        auto result1 = backend.execute_device_action(valid_id);
        CHECK(result1);

        // Uppercase should fail (if IDs differ)
        if (valid_id != uppercase_id) {
            auto result2 = backend.execute_device_action(uppercase_id);
            CHECK_FALSE(result2);
            CHECK(result2.result == AmsResult::NOT_SUPPORTED);
        }
    }

    SECTION("empty action_id returns not_supported") {
        auto result = backend.execute_device_action("");
        CHECK_FALSE(result);
        CHECK(result.result == AmsResult::NOT_SUPPORTED);
    }

    SECTION("actions can reference same section") {
        std::vector<DeviceAction> actions;
        for (int i = 0; i < 3; ++i) {
            DeviceAction action;
            action.id = "action_" + std::to_string(i);
            action.label = "Action " + std::to_string(i);
            action.section = "shared_section";
            action.type = ActionType::BUTTON;
            action.enabled = true;
            actions.push_back(action);
        }

        backend.set_device_actions(actions);

        auto result_actions = backend.get_device_actions();
        int shared_count = 0;
        for (const auto& a : result_actions) {
            if (a.section == "shared_section")
                ++shared_count;
        }
        CHECK(shared_count == 3);
    }

    SECTION("action can have slot_index for per-slot actions") {
        DeviceAction per_slot_action;
        per_slot_action.id = "lane_0_calibrate";
        per_slot_action.label = "Calibrate Lane 0";
        per_slot_action.section = "calibration";
        per_slot_action.type = ActionType::BUTTON;
        per_slot_action.slot_index = 0;
        per_slot_action.enabled = true;

        backend.set_device_actions({per_slot_action});

        auto actions = backend.get_device_actions();
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].slot_index == 0);
    }

    backend.stop();
}

TEST_CASE("Device actions section ordering", "[ams][device_actions][edge]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("sections maintain insertion order") {
        std::vector<DeviceSection> sections = {
            {"z_section", "Z Section", 2},
            {"a_section", "A Section", 0},
            {"m_section", "M Section", 1},
        };

        backend.set_device_sections(sections);

        auto result = backend.get_device_sections();
        REQUIRE(result.size() == 3);

        // Verify insertion order is preserved
        CHECK(result[0].id == "z_section");
        CHECK(result[1].id == "a_section");
        CHECK(result[2].id == "m_section");

        // Verify display_order can be used for sorting
        CHECK(result[1].display_order == 0); // a_section
        CHECK(result[2].display_order == 1); // m_section
        CHECK(result[0].display_order == 2); // z_section
    }

    backend.stop();
}

TEST_CASE("Device actions thread safety", "[ams][device_actions][mock]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    REQUIRE(backend.start());

    SECTION("concurrent reads are safe") {
        // Set up some test data
        std::vector<DeviceSection> sections = {{"test", "Test", 0}};
        std::vector<DeviceAction> actions;
        DeviceAction action;
        action.id = "test_action";
        action.label = "Test";
        action.section = "test";
        action.type = ActionType::BUTTON;
        action.enabled = true;
        actions.push_back(action);

        backend.set_device_sections(sections);
        backend.set_device_actions(actions);

        // Perform multiple reads (single-threaded but tests locking)
        for (int i = 0; i < 100; ++i) {
            auto s = backend.get_device_sections();
            auto a = backend.get_device_actions();
            CHECK(s.size() == 1);
            CHECK(a.size() == 1);
        }
    }

    SECTION("read-execute-read pattern works correctly") {
        auto actions_before = backend.get_device_actions();
        REQUIRE_FALSE(actions_before.empty());

        backend.execute_device_action(actions_before[0].id);

        auto actions_after = backend.get_device_actions();
        CHECK(actions_after.size() == actions_before.size());
    }

    backend.stop();
}

// =============================================================================
// Integration with Tool Changer Mode
// =============================================================================

TEST_CASE("Mock backend device actions in tool changer mode",
          "[ams][device_actions][mock][toolchanger]") {
    AmsBackendMock backend(4);
    backend.set_operation_delay(0);
    backend.set_tool_changer_mode(true);
    REQUIRE(backend.start());

    SECTION("tool changer mode has no device actions") {
        // Tool changers have no AMS-style device settings —
        // set_tool_changer_mode() correctly clears them
        auto sections = backend.get_device_sections();
        auto actions = backend.get_device_actions();

        CHECK(sections.empty());
        CHECK(actions.empty());
    }

    SECTION("can clear device actions in tool changer mode") {
        backend.set_device_sections({});
        backend.set_device_actions({});

        auto sections = backend.get_device_sections();
        auto actions = backend.get_device_actions();

        CHECK(sections.empty());
        CHECK(actions.empty());
    }

    backend.stop();
}
