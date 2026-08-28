// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_types.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

// ============================================================================
// Helper: Build a multi-unit AmsSystemInfo for direct struct tests
// ============================================================================

/**
 * @brief Create a test AmsSystemInfo with the given unit configuration
 * @param slots_per_unit Vector of slot counts, one per unit (e.g., {4, 4} for 2x4)
 * @return Populated AmsSystemInfo
 */
static AmsSystemInfo make_multi_unit_info(const std::vector<int>& slots_per_unit) {
    AmsSystemInfo info;
    info.type = AmsType::AFC;

    int global_offset = 0;
    for (int u = 0; u < static_cast<int>(slots_per_unit.size()); ++u) {
        AmsUnit unit;
        unit.unit_index = u;
        unit.name = "Box Turtle " + std::to_string(u + 1);
        unit.slot_count = slots_per_unit[u];
        unit.first_slot_global_index = global_offset;

        for (int s = 0; s < slots_per_unit[u]; ++s) {
            SlotInfo slot;
            slot.slot_index = s;
            slot.global_index = global_offset + s;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = global_offset + s;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        info.units.push_back(unit);
        global_offset += slots_per_unit[u];
    }

    info.total_slots = global_offset;
    return info;
}

// ============================================================================
// Test helpers for AFC and Happy Hare backends
// ============================================================================

/**
 * @brief Test helper for AFC multi-unit parsing
 *
 * Exposes parse_afc_state and initialize_slots for testing multi-unit
 * initialization paths that currently create only 1 unit.
 */
class AmsBackendAfcMultiUnitHelper : public AmsBackendAfc {
  public:
    AmsBackendAfcMultiUnitHelper() : AmsBackendAfc(nullptr, nullptr) {}

    /// Expose parse_afc_state for testing
    void test_parse_afc_state(const nlohmann::json& data) {
        // Build the full notification format and route through handle_status_update
        nlohmann::json notification;
        nlohmann::json params;
        params["AFC"] = data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// Expose initialize_slots for testing
    void test_initialize_slots(const std::vector<std::string>& names) {
        initialize_slots(names);
    }

    /// Access system_info for assertions (const)
    const AmsSystemInfo& get_system_info_ref() const {
        return system_info_;
    }

    /// Access system_info for test setup (mutable)
    AmsSystemInfo& get_mutable_system_info() {
        return system_info_;
    }

    /// Get slot count from registry
    int get_slot_count() const {
        return slots_.slot_count();
    }

    /// Get slot name from registry
    std::string get_slot_name(int index) const {
        return slots_.name_of(index);
    }

    /// Check if lanes have been initialized
    bool are_lanes_initialized() const {
        return slots_.is_initialized();
    }

    /// Set discovered lanes (delegates to base)
    void setup_discovered_lanes(const std::vector<std::string>& lanes,
                                const std::vector<std::string>& hubs) {
        set_discovered_lanes(lanes, hubs);
    }

    /// Override execute_gcode to prevent real API calls
    AmsError execute_gcode(const std::string& /*gcode*/) override {
        return AmsErrorHelper::success();
    }

  private:
    // Grant friend access (base class already has friend for AmsBackendAfcTestHelper,
    // but we need access through this separate helper class)
    // Access is provided through the protected members via inheritance
};

/**
 * @brief Test helper for Happy Hare multi-unit parsing
 *
 * Exposes parse_mmu_state for testing multi-unit initialization paths.
 */
class AmsBackendHHMultiUnitHelper : public AmsBackendHappyHare {
  public:
    AmsBackendHHMultiUnitHelper() : AmsBackendHappyHare(nullptr, nullptr) {}

    /// Feed MMU state through the normal notification pipeline
    void test_parse_mmu_state(const nlohmann::json& data) {
        nlohmann::json notification;
        nlohmann::json params;
        params["mmu"] = data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    /// Get system info snapshot for assertions (builds from registry when initialized)
    AmsSystemInfo get_system_info_snapshot() const {
        return get_system_info();
    }

    /// Override execute_gcode to prevent real API calls
    AmsError execute_gcode(const std::string& /*gcode*/) override {
        return AmsErrorHelper::success();
    }
};

// ============================================================================
// Section 1: AmsSystemInfo multi-unit helpers (ams_types.h)
// ============================================================================

TEST_CASE("AmsSystemInfo is_multi_unit returns false for empty units", "[ams][multi-unit]") {
    AmsSystemInfo info;
    REQUIRE_FALSE(info.is_multi_unit());
}

TEST_CASE("AmsSystemInfo is_multi_unit returns false for single unit", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4});
    REQUIRE_FALSE(info.is_multi_unit());
}

TEST_CASE("AmsSystemInfo is_multi_unit returns true for two units", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});
    REQUIRE(info.is_multi_unit());
}

TEST_CASE("AmsSystemInfo is_multi_unit returns true for three units", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4, 4});
    REQUIRE(info.is_multi_unit());
}

TEST_CASE("AmsSystemInfo unit_count returns correct count", "[ams][multi-unit]") {
    SECTION("empty") {
        AmsSystemInfo info;
        REQUIRE(info.unit_count() == 0);
    }

    SECTION("single unit") {
        auto info = make_multi_unit_info({4});
        REQUIRE(info.unit_count() == 1);
    }

    SECTION("two units") {
        auto info = make_multi_unit_info({4, 4});
        REQUIRE(info.unit_count() == 2);
    }

    SECTION("three units with different sizes") {
        auto info = make_multi_unit_info({4, 6, 2});
        REQUIRE(info.unit_count() == 3);
    }
}

TEST_CASE("AmsSystemInfo get_unit_for_slot maps slots to correct unit", "[ams][multi-unit]") {
    // 2 units, 4 slots each: unit 0 has globals 0-3, unit 1 has globals 4-7
    auto info = make_multi_unit_info({4, 4});

    SECTION("first slot of first unit") {
        const auto* unit = info.get_unit_for_slot(0);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 0);
        REQUIRE(unit->name == "Box Turtle 1");
    }

    SECTION("last slot of first unit") {
        const auto* unit = info.get_unit_for_slot(3);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 0);
    }

    SECTION("first slot of second unit") {
        const auto* unit = info.get_unit_for_slot(4);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 1);
        REQUIRE(unit->name == "Box Turtle 2");
    }

    SECTION("last slot of second unit") {
        const auto* unit = info.get_unit_for_slot(7);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 1);
    }

    SECTION("middle slot of first unit") {
        const auto* unit = info.get_unit_for_slot(2);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 0);
    }

    SECTION("middle slot of second unit") {
        const auto* unit = info.get_unit_for_slot(5);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 1);
    }
}

TEST_CASE("AmsSystemInfo get_unit_for_slot returns nullptr for out-of-range", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    SECTION("negative index") {
        REQUIRE(info.get_unit_for_slot(-1) == nullptr);
    }

    SECTION("index equal to total slots") {
        REQUIRE(info.get_unit_for_slot(8) == nullptr);
    }

    SECTION("large out-of-range index") {
        REQUIRE(info.get_unit_for_slot(100) == nullptr);
    }

    SECTION("negative large index") {
        REQUIRE(info.get_unit_for_slot(-99) == nullptr);
    }
}

TEST_CASE("AmsSystemInfo get_unit_for_slot with asymmetric units", "[ams][multi-unit]") {
    // Unit 0 has 4 slots (globals 0-3), unit 1 has 6 slots (globals 4-9)
    auto info = make_multi_unit_info({4, 6});

    SECTION("last slot of smaller unit") {
        const auto* unit = info.get_unit_for_slot(3);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 0);
    }

    SECTION("first slot of larger unit") {
        const auto* unit = info.get_unit_for_slot(4);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 1);
    }

    SECTION("last slot of larger unit") {
        const auto* unit = info.get_unit_for_slot(9);
        REQUIRE(unit != nullptr);
        REQUIRE(unit->unit_index == 1);
    }

    SECTION("one past end") {
        REQUIRE(info.get_unit_for_slot(10) == nullptr);
    }
}

TEST_CASE("AmsSystemInfo get_unit returns correct unit by index", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4, 2});

    SECTION("valid indices") {
        const auto* u0 = info.get_unit(0);
        REQUIRE(u0 != nullptr);
        REQUIRE(u0->name == "Box Turtle 1");
        REQUIRE(u0->slot_count == 4);

        const auto* u1 = info.get_unit(1);
        REQUIRE(u1 != nullptr);
        REQUIRE(u1->name == "Box Turtle 2");
        REQUIRE(u1->slot_count == 4);

        const auto* u2 = info.get_unit(2);
        REQUIRE(u2 != nullptr);
        REQUIRE(u2->name == "Box Turtle 3");
        REQUIRE(u2->slot_count == 2);
    }

    SECTION("out of range returns nullptr") {
        REQUIRE(info.get_unit(-1) == nullptr);
        REQUIRE(info.get_unit(3) == nullptr);
        REQUIRE(info.get_unit(100) == nullptr);
    }
}

TEST_CASE("AmsSystemInfo get_active_unit_index returns correct unit", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    SECTION("no active slot") {
        info.current_slot = -1;
        REQUIRE(info.get_active_unit_index() == -1);
    }

    SECTION("slot in first unit") {
        info.current_slot = 2;
        REQUIRE(info.get_active_unit_index() == 0);
    }

    SECTION("slot in second unit") {
        info.current_slot = 5;
        REQUIRE(info.get_active_unit_index() == 1);
    }

    SECTION("first slot of second unit") {
        info.current_slot = 4;
        REQUIRE(info.get_active_unit_index() == 1);
    }

    SECTION("last slot of first unit") {
        info.current_slot = 3;
        REQUIRE(info.get_active_unit_index() == 0);
    }

    SECTION("out of range slot returns -1") {
        info.current_slot = 99;
        REQUIRE(info.get_active_unit_index() == -1);
    }
}

TEST_CASE("AmsSystemInfo get_slot_global works across units", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    SECTION("slot in first unit has correct local index") {
        const auto* slot = info.get_slot_global(2);
        REQUIRE(slot != nullptr);
        REQUIRE(slot->slot_index == 2);
        REQUIRE(slot->global_index == 2);
    }

    SECTION("slot in second unit has correct local index") {
        const auto* slot = info.get_slot_global(5);
        REQUIRE(slot != nullptr);
        REQUIRE(slot->slot_index == 1); // Local index within unit 1
        REQUIRE(slot->global_index == 5);
    }

    SECTION("first slot of second unit") {
        const auto* slot = info.get_slot_global(4);
        REQUIRE(slot != nullptr);
        REQUIRE(slot->slot_index == 0); // First slot in unit 1
        REQUIRE(slot->global_index == 4);
    }

    SECTION("out of range returns nullptr") {
        REQUIRE(info.get_slot_global(-1) == nullptr);
        REQUIRE(info.get_slot_global(8) == nullptr);
    }
}

TEST_CASE("AmsSystemInfo total_slots matches sum across units", "[ams][multi-unit]") {
    SECTION("equal-sized units") {
        auto info = make_multi_unit_info({4, 4});
        REQUIRE(info.total_slots == 8);
    }

    SECTION("asymmetric units") {
        auto info = make_multi_unit_info({4, 6});
        REQUIRE(info.total_slots == 10);
    }

    SECTION("three units") {
        auto info = make_multi_unit_info({4, 4, 2});
        REQUIRE(info.total_slots == 10);
    }

    SECTION("single unit") {
        auto info = make_multi_unit_info({8});
        REQUIRE(info.total_slots == 8);
    }
}

// ============================================================================
// Section 2: AFC Backend multi-unit parsing
// ============================================================================
//
// NOTE: These tests define the EXPECTED behavior for multi-unit AFC support.
// The AFC backend's initialize_slots() currently always creates 1 unit.
// Tests for multi-unit will FAIL initially - this is TDD.
// ============================================================================

TEST_CASE("AFC single-unit backward compatibility", "[ams][multi-unit][afc]") {
    // Single unit with 4 lanes -- this is the existing behavior and should pass
    AmsBackendAfcMultiUnitHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    helper.test_initialize_slots(lanes);

    const auto& info = helper.get_system_info_ref();

    SECTION("creates exactly one unit") {
        REQUIRE(info.units.size() == 1);
    }

    SECTION("unit has correct slot count") {
        REQUIRE(info.units[0].slot_count == 4);
    }

    SECTION("total_slots is correct") {
        REQUIRE(info.total_slots == 4);
    }

    SECTION("first_slot_global_index is 0") {
        REQUIRE(info.units[0].first_slot_global_index == 0);
    }

    SECTION("slots have correct global indices") {
        for (int i = 0; i < 4; ++i) {
            const auto* slot = info.get_slot_global(i);
            REQUIRE(slot != nullptr);
            REQUIRE(slot->global_index == i);
            REQUIRE(slot->slot_index == i);
        }
    }

    SECTION("is_multi_unit is false") {
        REQUIRE_FALSE(info.is_multi_unit());
    }
}

TEST_CASE("AFC multi-unit: units array with 2 units creates 2 AmsUnit entries",
          "[ams][multi-unit][afc]") {
    // When AFC reports a units array with multiple units, the backend should
    // create separate AmsUnit entries for each.
    //
    // Expected AFC JSON structure for multi-unit:
    // {
    //   "units": [
    //     { "name": "Turtle_1", "lanes": ["lane1", "lane2", "lane3", "lane4"] },
    //     { "name": "Turtle_2", "lanes": ["lane5", "lane6", "lane7", "lane8"] }
    //   ]
    // }
    AmsBackendAfcMultiUnitHelper helper;

    // Set up discovered lanes (all 8 lanes across 2 units)
    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3", "lane4",
                                          "lane5", "lane6", "lane7", "lane8"};
    std::vector<std::string> hubs = {"Turtle_1", "Turtle_2"};
    helper.setup_discovered_lanes(all_lanes, hubs);

    // Feed AFC state with units array describing multi-unit topology
    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {{{"name", "Turtle_1"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}},
         {{"name", "Turtle_2"}, {"lanes", {"lane5", "lane6", "lane7", "lane8"}}}});
    helper.test_parse_afc_state(afc_state);

    const auto& info = helper.get_system_info_ref();

    // Two AmsUnit entries should be created
    REQUIRE(info.units.size() == 2);

    SECTION("first unit has correct properties") {
        REQUIRE(info.units[0].unit_index == 0);
        REQUIRE(info.units[0].slot_count == 4);
        REQUIRE(info.units[0].first_slot_global_index == 0);
        REQUIRE(info.units[0].name.find("Turtle_1") != std::string::npos);
    }

    SECTION("second unit has correct properties") {
        REQUIRE(info.units[1].unit_index == 1);
        REQUIRE(info.units[1].slot_count == 4);
        REQUIRE(info.units[1].first_slot_global_index == 4);
        REQUIRE(info.units[1].name.find("Turtle_2") != std::string::npos);
    }

    SECTION("total_slots is sum of all units") {
        REQUIRE(info.total_slots == 8);
    }

    SECTION("global indices are correct across units") {
        // Unit 0 slots: global 0-3
        for (int i = 0; i < 4; ++i) {
            const auto* slot = info.get_slot_global(i);
            REQUIRE(slot != nullptr);
            REQUIRE(slot->global_index == i);
            REQUIRE(slot->slot_index == i); // Local within unit 0
        }

        // Unit 1 slots: global 4-7, local 0-3
        for (int i = 4; i < 8; ++i) {
            const auto* slot = info.get_slot_global(i);
            REQUIRE(slot != nullptr);
            REQUIRE(slot->global_index == i);
            REQUIRE(slot->slot_index == i - 4); // Local within unit 1
        }
    }

    SECTION("is_multi_unit is true") {
        REQUIRE(info.is_multi_unit());
    }
}

TEST_CASE("AFC multi-unit: asymmetric unit sizes", "[ams][multi-unit][afc]") {
    // Unit 1 has 4 lanes, unit 2 has 6 lanes (different Box Turtle models)
    AmsBackendAfcMultiUnitHelper helper;

    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3", "lane4", "lane5",
                                          "lane6", "lane7", "lane8", "lane9", "lane10"};
    std::vector<std::string> hubs = {"Turtle_1", "Turtle_2"};
    helper.setup_discovered_lanes(all_lanes, hubs);

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {{{"name", "Turtle_1"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}},
         {{"name", "Turtle_2"},
          {"lanes", {"lane5", "lane6", "lane7", "lane8", "lane9", "lane10"}}}});
    helper.test_parse_afc_state(afc_state);

    const auto& info = helper.get_system_info_ref();

    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 6);
    REQUIRE(info.units[1].first_slot_global_index == 4);
    REQUIRE(info.total_slots == 10);

    // Last slot of unit 1: global index 9, local index 5
    const auto* last_slot = info.get_slot_global(9);
    REQUIRE(last_slot != nullptr);
    REQUIRE(last_slot->slot_index == 5);
    REQUIRE(last_slot->global_index == 9);
}

TEST_CASE("AFC multi-unit: three units", "[ams][multi-unit][afc]") {
    AmsBackendAfcMultiUnitHelper helper;

    std::vector<std::string> all_lanes = {"lane1", "lane2", "lane3", "lane4",  "lane5",  "lane6",
                                          "lane7", "lane8", "lane9", "lane10", "lane11", "lane12"};
    std::vector<std::string> hubs = {"Turtle_1", "Turtle_2", "Turtle_3"};
    helper.setup_discovered_lanes(all_lanes, hubs);

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {{{"name", "Turtle_1"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}},
         {{"name", "Turtle_2"}, {"lanes", {"lane5", "lane6", "lane7", "lane8"}}},
         {{"name", "Turtle_3"}, {"lanes", {"lane9", "lane10", "lane11", "lane12"}}}});
    helper.test_parse_afc_state(afc_state);

    const auto& info = helper.get_system_info_ref();

    REQUIRE(info.units.size() == 3);
    REQUIRE(info.total_slots == 12);
    REQUIRE(info.units[0].first_slot_global_index == 0);
    REQUIRE(info.units[1].first_slot_global_index == 4);
    REQUIRE(info.units[2].first_slot_global_index == 8);
    REQUIRE(info.is_multi_unit());
}

TEST_CASE("AFC multi-unit: single unit in units array is backward compatible",
          "[ams][multi-unit][afc]") {
    // When AFC reports units array with just 1 unit, should behave same as legacy
    AmsBackendAfcMultiUnitHelper helper;

    std::vector<std::string> lanes = {"lane1", "lane2", "lane3", "lane4"};
    std::vector<std::string> hubs = {"Turtle_1"};
    helper.setup_discovered_lanes(lanes, hubs);

    nlohmann::json afc_state;
    afc_state["units"] = nlohmann::json::array(
        {{{"name", "Turtle_1"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}}});
    helper.test_parse_afc_state(afc_state);

    const auto& info = helper.get_system_info_ref();

    REQUIRE(info.units.size() == 1);
    REQUIRE(info.total_slots == 4);
    REQUIRE_FALSE(info.is_multi_unit());
}

// ============================================================================
// Section 3: Happy Hare Backend multi-unit support
// ============================================================================
//
// Happy Hare can report multi-unit configurations via:
// - num_units: integer number of physical units
// - num_gates: comma-separated gate counts per unit (e.g., "4,4")
//
// Currently the backend always creates 1 unit. These tests define the
// expected multi-unit behavior and will FAIL initially (TDD).
// ============================================================================

TEST_CASE("Happy Hare single-unit backward compatibility", "[ams][multi-unit][happy-hare]") {
    // Standard single-unit Happy Hare with integer num_gates
    AmsBackendHHMultiUnitHelper helper;

    // Feed initial MMU state with gate arrays (triggers initialize_slots)
    nlohmann::json mmu_data;
    mmu_data["gate_status"] = {1, 1, 0, -1};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00};
    mmu_data["gate_material"] = {"PLA", "PETG", "ABS", "PLA"};
    mmu_data["gate"] = -1;
    mmu_data["tool"] = -1;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info_snapshot();

    SECTION("creates exactly one unit") {
        REQUIRE(info.units.size() == 1);
    }

    SECTION("unit has correct slot count") {
        REQUIRE(info.units[0].slot_count == 4);
    }

    SECTION("total_slots is correct") {
        REQUIRE(info.total_slots == 4);
    }

    SECTION("is_multi_unit is false") {
        REQUIRE_FALSE(info.is_multi_unit());
    }

    SECTION("slots have correct data") {
        const auto* slot0 = info.get_slot_global(0);
        REQUIRE(slot0 != nullptr);
        REQUIRE(slot0->status == SlotStatus::AVAILABLE);
        REQUIRE(slot0->color_rgb == 0xFF0000);
        REQUIRE(slot0->material == "PLA");
    }
}

TEST_CASE("Happy Hare multi-unit: num_units with comma-separated num_gates",
          "[ams][multi-unit][happy-hare]") {
    // When Happy Hare reports multiple units, it uses:
    // - num_units: 2
    // - num_gates: "4,4" (comma-separated gates per unit)
    //
    // Gate arrays (gate_status, gate_color_rgb, etc.) contain ALL gates
    // concatenated across units.
    AmsBackendHHMultiUnitHelper helper;

    nlohmann::json mmu_data;
    // 2 units, 4 gates each = 8 total gates
    mmu_data["num_units"] = 2;
    mmu_data["num_gates"] = "4,4";
    mmu_data["gate_status"] = {1, 1, 0, -1, 1, 1, 1, 0};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
                                  0xFF00FF, 0x00FFFF, 0xFFA500, 0x800080};
    mmu_data["gate_material"] = {"PLA", "PETG", "ABS", "PLA", "TPU", "PLA", "PETG", "ABS"};
    mmu_data["gate"] = -1;
    mmu_data["tool"] = -1;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info_snapshot();

    SECTION("creates two units") {
        REQUIRE(info.units.size() == 2);
    }

    SECTION("first unit has 4 slots starting at global 0") {
        REQUIRE(info.units[0].unit_index == 0);
        REQUIRE(info.units[0].slot_count == 4);
        REQUIRE(info.units[0].first_slot_global_index == 0);
    }

    SECTION("second unit has 4 slots starting at global 4") {
        REQUIRE(info.units[1].unit_index == 1);
        REQUIRE(info.units[1].slot_count == 4);
        REQUIRE(info.units[1].first_slot_global_index == 4);
    }

    SECTION("total_slots is 8") {
        REQUIRE(info.total_slots == 8);
    }

    SECTION("is_multi_unit is true") {
        REQUIRE(info.is_multi_unit());
    }

    SECTION("slots have correct global indices") {
        for (int i = 0; i < 8; ++i) {
            const auto* slot = info.get_slot_global(i);
            REQUIRE(slot != nullptr);
            REQUIRE(slot->global_index == i);
        }
    }

    SECTION("slot data is correctly distributed across units") {
        // Unit 0, slot 0: PLA, red
        const auto* s0 = info.get_slot_global(0);
        REQUIRE(s0 != nullptr);
        REQUIRE(s0->color_rgb == 0xFF0000);
        REQUIRE(s0->material == "PLA");

        // Unit 1, slot 0 (global 4): TPU, magenta
        const auto* s4 = info.get_slot_global(4);
        REQUIRE(s4 != nullptr);
        REQUIRE(s4->color_rgb == 0xFF00FF);
        REQUIRE(s4->material == "TPU");
        REQUIRE(s4->slot_index == 0); // Local index within unit 1
    }
}

TEST_CASE("Happy Hare multi-unit: uneven gate division", "[ams][multi-unit][happy-hare]") {
    // 10 gates across 3 units: 3+3+4 (last unit gets remainder)
    AmsBackendHHMultiUnitHelper helper;

    nlohmann::json mmu_data;
    mmu_data["num_units"] = 3;

    // Build arrays for 10 total gates
    mmu_data["gate_status"] = {1, 1, 0, -1, 1, 1, 1, 0, 1, -1};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF,
                                  0x00FFFF, 0xFFA500, 0x800080, 0xFFFFFF, 0x000000};
    mmu_data["gate_material"] = {"PLA", "PETG", "ABS", "PLA", "TPU",
                                 "PLA", "PETG", "ABS", "PLA", "PETG"};
    mmu_data["gate"] = -1;
    mmu_data["tool"] = -1;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info_snapshot();

    REQUIRE(info.units.size() == 3);
    REQUIRE(info.units[0].slot_count == 3);
    REQUIRE(info.units[1].slot_count == 3);
    REQUIRE(info.units[2].slot_count == 4); // Last unit gets remainder
    REQUIRE(info.units[0].first_slot_global_index == 0);
    REQUIRE(info.units[1].first_slot_global_index == 3);
    REQUIRE(info.units[2].first_slot_global_index == 6);
    REQUIRE(info.total_slots == 10);
}

TEST_CASE("Happy Hare multi-unit: integer num_gates creates single unit",
          "[ams][multi-unit][happy-hare]") {
    // When num_gates is a plain integer (not comma-separated), it means
    // single unit with that many gates. This is backward compatible.
    AmsBackendHHMultiUnitHelper helper;

    nlohmann::json mmu_data;
    mmu_data["num_gates"] = 4; // Integer, not string
    mmu_data["gate_status"] = {1, 1, 0, -1};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00};
    mmu_data["gate_material"] = {"PLA", "PETG", "ABS", "PLA"};
    mmu_data["gate"] = -1;
    mmu_data["tool"] = -1;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info_snapshot();

    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.total_slots == 4);
    REQUIRE_FALSE(info.is_multi_unit());
}

TEST_CASE("Happy Hare multi-unit: three units", "[ams][multi-unit][happy-hare]") {
    AmsBackendHHMultiUnitHelper helper;

    nlohmann::json mmu_data;
    mmu_data["num_units"] = 3;
    mmu_data["num_gates"] = "4,4,4";

    // Build 12-gate arrays
    mmu_data["gate_status"] = {1, 1, 0, -1, 1, 1, 1, 0, 1, -1, 0, 1};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF,
                                  0xFFA500, 0x800080, 0xFFFFFF, 0x000000, 0x123456, 0xABCDEF};
    mmu_data["gate_material"] = {"PLA",  "PETG", "ABS", "PLA",  "TPU", "PLA",
                                 "PETG", "ABS",  "PLA", "PETG", "ABS", "TPU"};
    mmu_data["gate"] = -1;
    mmu_data["tool"] = -1;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info_snapshot();

    REQUIRE(info.units.size() == 3);
    REQUIRE(info.total_slots == 12);
    REQUIRE(info.units[0].first_slot_global_index == 0);
    REQUIRE(info.units[1].first_slot_global_index == 4);
    REQUIRE(info.units[2].first_slot_global_index == 8);
    REQUIRE(info.is_multi_unit());
}

// ============================================================================
// Global-to-Local Slot Index Mapping
// ============================================================================

// The conversion AmsOverviewPanel's detail view performs on a slot tap: the
// widget carries a GLOBAL slot index, the detail view is indexed locally. Both
// halves of the answer are asked of shipped code — get_unit_position_for_slot()
// for "does this unit own the slot" and AmsUnit::first_slot_global_index for the
// origin — so a change to either layout rule is felt here rather than in a copy.
//
// One deliberate difference from the panel: handle_detail_slot_tap() bounds the
// local index on detail_slot_count_, the number of slot WIDGETS the detail view
// actually built, not on unit.slot_count. The two agree in the only state that
// function is reachable in (the detail view is built for detail_unit_index_),
// and the widget count is not a property of AmsSystemInfo, so it is not
// modelled here.
static int global_to_local_slot(const AmsSystemInfo& info, int unit_index, int global_slot_index) {
    if (info.get_unit_position_for_slot(global_slot_index) != unit_index) {
        return -1;
    }
    const AmsUnit* unit = info.get_unit_for_slot(global_slot_index);
    return unit ? global_slot_index - unit->first_slot_global_index : -1;
}

TEST_CASE("Global-to-local slot mapping for first unit", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    // Slot 0-3 map to local 0-3 in unit 0
    REQUIRE(global_to_local_slot(info, 0, 0) == 0);
    REQUIRE(global_to_local_slot(info, 0, 1) == 1);
    REQUIRE(global_to_local_slot(info, 0, 2) == 2);
    REQUIRE(global_to_local_slot(info, 0, 3) == 3);
}

TEST_CASE("Global-to-local slot mapping for second unit", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    // Slot 4-7 map to local 0-3 in unit 1
    REQUIRE(global_to_local_slot(info, 1, 4) == 0);
    REQUIRE(global_to_local_slot(info, 1, 5) == 1);
    REQUIRE(global_to_local_slot(info, 1, 6) == 2);
    REQUIRE(global_to_local_slot(info, 1, 7) == 3);
}

TEST_CASE("Global-to-local slot mapping rejects wrong unit", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    // Slot 4 belongs to unit 1 — asking for it from unit 0 should fail
    REQUIRE(global_to_local_slot(info, 0, 4) == -1);
    // Slot 0 belongs to unit 0 — asking for it from unit 1 should fail
    REQUIRE(global_to_local_slot(info, 1, 0) == -1);
}

TEST_CASE("Global-to-local slot mapping with asymmetric units", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 8, 2});

    // Unit 0: global 0-3, Unit 1: global 4-11, Unit 2: global 12-13
    REQUIRE(global_to_local_slot(info, 0, 3) == 3);
    REQUIRE(global_to_local_slot(info, 1, 4) == 0);
    REQUIRE(global_to_local_slot(info, 1, 11) == 7);
    REQUIRE(global_to_local_slot(info, 2, 12) == 0);
    REQUIRE(global_to_local_slot(info, 2, 13) == 1);

    // Out of range
    REQUIRE(global_to_local_slot(info, 2, 14) == -1);
    REQUIRE(global_to_local_slot(info, 1, 12) == -1);
}

TEST_CASE("Global-to-local slot mapping with invalid unit index", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    REQUIRE(global_to_local_slot(info, -1, 0) == -1);
    REQUIRE(global_to_local_slot(info, 2, 0) == -1);
    REQUIRE(global_to_local_slot(info, 99, 0) == -1);
}

TEST_CASE("Global-to-local slot mapping with negative global index", "[ams][multi-unit]") {
    auto info = make_multi_unit_info({4, 4});

    REQUIRE(global_to_local_slot(info, 0, -1) == -1);
    REQUIRE(global_to_local_slot(info, 1, -1) == -1);
}
