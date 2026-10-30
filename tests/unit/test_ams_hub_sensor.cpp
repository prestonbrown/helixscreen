// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "moonraker_api.h"
#include "test_helpers/afc_test_access.h"

#include "../catch_amalgamated.hpp"

namespace helix {

/**
 * @brief Test helper for hub sensor tests, extending AmsBackendAfc
 *
 * Reuses the same pattern as AmsBackendAfcTestHelper in test_ams_backend_afc.cpp.
 */
class HubSensorTestHelper : public AmsBackendAfc {
  public:
    HubSensorTestHelper() : AmsBackendAfc(nullptr, nullptr) {}

    // Lane/hub setup
    void initialize_test_lanes_with_slots(int count) {
        system_info_.units.clear();
        std::vector<std::string> names;

        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Turtle_1";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;

        for (int i = 0; i < count; ++i) {
            std::string name = "lane" + std::to_string(i + 1);
            names.push_back(name);

            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = kAmsDefaultSlotColor;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;
        AfcTestAccess::slots(*this).initialize("Turtle_1", names);
    }

    void set_discovered_hubs(const std::vector<std::string>& hubs) {
        AfcTestAccess::hub_names(*this) = hubs;
    }

    void set_hub_sensor(const std::string& hub_name, bool state) {
        AfcTestAccess::hub_sensors(*this)[hub_name] = state;
    }

    bool get_hub_sensor(const std::string& hub_name) const {
        auto it = AfcTestAccess::hub_sensors(*this).find(hub_name);
        return it != AfcTestAccess::hub_sensors(*this).end() && it->second;
    }

    const std::unordered_map<std::string, bool>& get_hub_sensors() const {
        return AfcTestAccess::hub_sensors(*this);
    }

    // Multi-unit setup
    void
    setup_multi_unit(const std::unordered_map<std::string, std::vector<std::string>>& unit_map) {
        AfcTestAccess::unit_lane_map(*this) = unit_map;
        AfcTestAccess::reorganize_slots(*this);
    }

    // Feed status updates
    void feed_status_update(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    void feed_afc_hub(const std::string& hub_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_hub " + hub_name] = data;
        feed_status_update(params);
    }

    AmsSystemInfo get_test_system_info() const {
        return system_info_;
    }

    PathSegment test_compute_filament_segment() const {
        return AfcTestAccess::compute_filament_segment_unlocked(*this);
    }

    void initialize_slots_from_discovery() {
        if (!AfcTestAccess::discovered_lane_names(*this).empty() &&
            !AfcTestAccess::slots(*this).is_initialized()) {
            AfcTestAccess::initialize_slots(*this, AfcTestAccess::discovered_lane_names(*this));
        }
    }

    // Override execute_gcode (required by base class, not used in these tests)
    AmsError execute_gcode(const std::string& /*gcode*/) override {
        return AmsErrorHelper::success();
    }
};
} // namespace helix

// ============================================================================
// AmsUnit defaults
// ============================================================================

TEST_CASE("AmsUnit hub sensor fields default to false", "[ams][hub_sensor]") {
    helix::AmsUnit unit;
    REQUIRE(unit.has_hub_sensor == false);
    REQUIRE(unit.hub_sensor_triggered == false);
}

// ============================================================================
// AFC per-unit hub sensor — single unit
// ============================================================================

TEST_CASE("AFC single-unit: apply_hub stores per-hub state", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_discovered_hubs({"Turtle_1"});

    // Feed hub sensor triggered
    helper.feed_afc_hub("Turtle_1", {{"state", true}});

    REQUIRE(helper.get_hub_sensor("Turtle_1") == true);

    // Feed hub sensor cleared
    helper.feed_afc_hub("Turtle_1", {{"state", false}});

    REQUIRE(helper.get_hub_sensor("Turtle_1") == false);
}

TEST_CASE("AFC single-unit: hub sensor updates AmsUnit", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_discovered_hubs({"Turtle_1"});

    // Unit name matches hub name "Turtle_1"
    helper.feed_afc_hub("Turtle_1", {{"state", true}});

    auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].has_hub_sensor == true);
    REQUIRE(info.units[0].hub_sensor_triggered == true);

    // Clear it
    helper.feed_afc_hub("Turtle_1", {{"state", false}});

    info = helper.get_test_system_info();
    REQUIRE(info.units[0].hub_sensor_triggered == false);
}

TEST_CASE("AFC single-unit: hub sensor triggers OUTPUT segment", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;
    helper.initialize_test_lanes_with_slots(4);
    helper.set_discovered_hubs({"Turtle_1"});

    helper.feed_afc_hub("Turtle_1", {{"state", true}});

    REQUIRE(helper.test_compute_filament_segment() == helix::PathSegment::OUTPUT);
}

// ============================================================================
// AFC per-unit hub sensor — multi-unit
// ============================================================================

TEST_CASE("AFC multi-unit: per-unit hub sensor population after reorganize",
          "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;

    // Set up 8 lanes across 2 units
    std::vector<std::string> all_lanes;
    for (int i = 1; i <= 8; ++i) {
        all_lanes.push_back("lane" + std::to_string(i));
    }
    helper.initialize_test_lanes_with_slots(8);
    helper.set_discovered_hubs({"Turtle_1", "Turtle_2"});

    // Set per-hub states before reorganize
    helper.set_hub_sensor("Turtle_1", true);
    helper.set_hub_sensor("Turtle_2", false);

    // Reorganize into 2 units
    std::unordered_map<std::string, std::vector<std::string>> unit_map;
    unit_map["Turtle_1"] = {"lane1", "lane2", "lane3", "lane4"};
    unit_map["Turtle_2"] = {"lane5", "lane6", "lane7", "lane8"};
    helper.setup_multi_unit(unit_map);

    auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 2);

    // Turtle_1 should have hub sensor triggered
    REQUIRE(info.units[0].name == "Turtle_1");
    REQUIRE(info.units[0].has_hub_sensor == true);
    REQUIRE(info.units[0].hub_sensor_triggered == true);

    // Turtle_2 should have hub sensor not triggered
    REQUIRE(info.units[1].name == "Turtle_2");
    REQUIRE(info.units[1].has_hub_sensor == true);
    REQUIRE(info.units[1].hub_sensor_triggered == false);
}

TEST_CASE("AFC multi-unit: real-time hub update on correct unit", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;

    std::vector<std::string> all_lanes;
    for (int i = 1; i <= 8; ++i) {
        all_lanes.push_back("lane" + std::to_string(i));
    }
    helper.initialize_test_lanes_with_slots(8);
    helper.set_discovered_hubs({"Turtle_1", "Turtle_2"});

    // Reorganize first (both hubs off)
    std::unordered_map<std::string, std::vector<std::string>> unit_map;
    unit_map["Turtle_1"] = {"lane1", "lane2", "lane3", "lane4"};
    unit_map["Turtle_2"] = {"lane5", "lane6", "lane7", "lane8"};
    helper.setup_multi_unit(unit_map);

    // Now feed a real-time update for Turtle_2
    helper.feed_afc_hub("Turtle_2", {{"state", true}});

    auto info = helper.get_test_system_info();

    // Turtle_1 should still be off
    REQUIRE(info.units[0].hub_sensor_triggered == false);

    // Turtle_2 should now be triggered
    REQUIRE(info.units[1].hub_sensor_triggered == true);
}

TEST_CASE("AFC multi-unit: any hub triggered returns OUTPUT segment", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;
    helper.initialize_test_lanes_with_slots(8);
    helper.set_discovered_hubs({"Turtle_1", "Turtle_2"});

    // Only Turtle_2 triggered
    helper.feed_afc_hub("Turtle_2", {{"state", true}});

    REQUIRE(helper.test_compute_filament_segment() == helix::PathSegment::OUTPUT);
}

TEST_CASE("AFC multi-unit: no hub triggered returns NONE", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;
    helper.initialize_test_lanes_with_slots(8);
    helper.set_discovered_hubs({"Turtle_1", "Turtle_2"});

    // Both hubs off
    helper.feed_afc_hub("Turtle_1", {{"state", false}});
    helper.feed_afc_hub("Turtle_2", {{"state", false}});

    // No lane sensors, no toolhead sensors — should be NONE
    REQUIRE(helper.test_compute_filament_segment() == helix::PathSegment::NONE);
}

// ============================================================================
// AFC initialize_slots sets has_hub_sensor
// ============================================================================

TEST_CASE("AFC initialize_slots sets has_hub_sensor on unit", "[ams][hub_sensor][afc]") {
    helix::HubSensorTestHelper helper;

    // Use the real initialize_slots flow via set_discovered_lanes + initialize
    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.set_discovered_lanes(lanes, hubs);
    helper.initialize_slots_from_discovery();

    auto info = helper.get_test_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].has_hub_sensor == true);
    // hub_sensor_triggered defaults to false until sensor data arrives
    REQUIRE(info.units[0].hub_sensor_triggered == false);
}
