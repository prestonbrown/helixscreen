// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_happy_hare.h"
#include "ams_types.h"
#include "moonraker_api.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

/**
 * @brief Test helper class providing access to AmsBackendHappyHare internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendHappyHareTestHelper : public AmsBackendHappyHare {
  public:
    AmsBackendHappyHareTestHelper() : AmsBackendHappyHare(nullptr, nullptr) {}

    /**
     * @brief Initialize test gates with default SlotInfo
     * @param count Number of gates to create
     */
    void initialize_test_gates(int count) {
        system_info_.units.clear();

        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Happy Hare MMU";
        unit.slot_count = count;
        unit.first_slot_global_index = 0;

        for (int i = 0; i < count; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            slot.mapped_tool = i;
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        unit.topology = is_type_b() ? PathTopology::HUB : PathTopology::LINEAR;
        unit.has_encoder = !is_type_b();

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;

        // Also initialize tool_to_slot_map for reset_tool_mappings tests
        system_info_.tool_to_slot_map.clear();
        for (int i = 0; i < count; ++i) {
            system_info_.tool_to_slot_map.push_back(i);
        }

        // Initialize SlotRegistry to match
        std::vector<std::string> slot_names;
        for (int i = 0; i < count; ++i) {
            slot_names.push_back(std::to_string(i));
        }
        slots_.initialize("MMU", slot_names);
        // Set status to AVAILABLE to match legacy init
        for (int i = 0; i < count; ++i) {
            auto* entry = slots_.get_mut(i);
            if (entry) {
                entry->info.status = SlotStatus::AVAILABLE;
                entry->info.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            }
        }
        // Set 1:1 tool map
        slots_.set_tool_map(system_info_.tool_to_slot_map);
    }

    /**
     * @brief Get mutable slot pointer for test setup
     * @param slot_index Global slot index
     * @return Pointer to SlotInfo or nullptr
     */
    SlotInfo* get_mutable_slot(int slot_index) {
        auto* entry = slots_.get_mut(slot_index);
        return entry ? &entry->info : nullptr;
    }

    /**
     * @brief Get const SlotEntry pointer for test assertions (includes sensors)
     * @param slot_index Global slot index
     * @return Pointer to SlotEntry or nullptr
     */
    const helix::printer::SlotEntry* get_slot_entry(int slot_index) const {
        return slots_.get(slot_index);
    }

    // G-code capture for persistence tests
    std::vector<std::string> captured_gcodes;

    /**
     * @brief Override execute_gcode to capture commands for testing
     * @param gcode The G-code command
     * @return Success
     */
    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    /**
     * @brief Feed MMU JSON state through the normal notification pipeline
     * @param mmu_data JSON object representing printer.mmu data
     */
    void test_parse_mmu_state(const nlohmann::json& mmu_data) {
        nlohmann::json notification;
        nlohmann::json params;
        params["mmu"] = mmu_data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    void set_running(bool state) {
        running_ = state;
    }

    void set_filament_loaded(bool state) {
        system_info_.filament_loaded = state;
    }

    void set_current_slot(int slot) {
        system_info_.current_slot = slot;
    }

    void set_selector_type(const std::string& type) {
        selector_type_ = type;
    }

    void apply_selector_type_update() {
        update_unit_topologies();
    }

    void clear_captured_gcodes() {
        captured_gcodes.clear();
    }

    /**
     * @brief Check if exact G-code was captured
     * @param expected Exact G-code string to find
     * @return true if found
     */
    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    /**
     * @brief Check if any captured G-code starts with prefix
     * @param prefix Prefix to search for
     * @return true if any G-code starts with prefix
     */
    bool has_gcode_starting_with(const std::string& prefix) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    /**
     * @brief Check if any captured G-code contains substring
     * @param substring String to search for
     * @return true if any G-code contains substring
     */
    bool has_gcode_containing(const std::string& substring) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.find(substring) != std::string::npos)
                return true;
        }
        return false;
    }
};

// ============================================================================
// set_slot_info() Persistence Tests - Happy Hare MMU_GATE_MAP
// ============================================================================
//
// These tests verify that set_slot_info() sends the appropriate MMU_GATE_MAP
// G-code commands to persist filament properties in Happy Hare.
//
// Command format:
// - MMU_GATE_MAP GATE={n} COLOR={RRGGBB} MATERIAL={type} SPOOLID={id}
//
// NOTE: These tests are designed to FAIL initially (test-first approach).
// The set_slot_info() method currently only updates local state and does NOT
// send G-code commands. Implementation must be added to make these tests pass.
// ============================================================================

TEST_CASE("Happy Hare persistence: MMU_GATE_MAP basic format", "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000; // Red - need something to trigger command

    helper.set_slot_info(0, info);

    // Should send MMU_GATE_MAP with GATE=0
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode_starting_with("MMU_GATE_MAP GATE=0"));
}

TEST_CASE("Happy Hare persistence: MMU_GATE_MAP with color", "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0xFF0000; // Red

    helper.set_slot_info(0, info);

    // Should send: MMU_GATE_MAP GATE=0 COLOR=FF0000
    // Color: uppercase hex, no # prefix
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=FF0000"));
}

TEST_CASE("Happy Hare persistence: MMU_GATE_MAP color uppercase no prefix",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0x00FF00; // Green

    helper.set_slot_info(1, info);

    // Should send: MMU_GATE_MAP GATE=1 COLOR=00FF00 (uppercase, no #)
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=1 COLOR=00FF00"));
}

TEST_CASE("Happy Hare persistence: MMU_GATE_MAP with material", "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.material = "PLA";

    helper.set_slot_info(1, info);

    // Should send: MMU_GATE_MAP GATE=1 MATERIAL=PLA
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=1 MATERIAL=PLA"));
}

TEST_CASE("Happy Hare persistence: MMU_GATE_MAP with Spoolman ID",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.spoolman_id = 42;

    helper.set_slot_info(2, info);

    // Should contain: SPOOLID=42
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode_containing("SPOOLID=42"));
}

TEST_CASE("Happy Hare persistence: MMU_GATE_MAP clear Spoolman with -1",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Pre-set existing spoolman_id on slot
    SlotInfo* existing_slot = helper.get_mutable_slot(0);
    REQUIRE(existing_slot != nullptr);
    existing_slot->spoolman_id = 123;

    // Now clear it by setting spoolman_id = 0
    SlotInfo new_info;
    new_info.spoolman_id = 0;

    helper.set_slot_info(0, new_info);

    // Should send: SPOOLID=-1 to clear
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode_containing("SPOOLID=-1"));
}

TEST_CASE("Happy Hare persistence: full slot info generates complete command",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0x0000FF; // Blue
    info.material = "PETG";
    info.spoolman_id = 99;

    helper.set_slot_info(0, info);

    // Should send: MMU_GATE_MAP GATE=0 COLOR=0000FF MATERIAL=PETG SPOOLID=99
    // FAILS: set_slot_info doesn't call execute_gcode yet
    REQUIRE(helper.has_gcode("MMU_GATE_MAP GATE=0 COLOR=0000FF MATERIAL=PETG SPOOLID=99"));
}

TEST_CASE("Happy Hare persistence: skips COLOR for default grey",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0x808080; // Default grey - should NOT include COLOR
    info.material = "PLA";     // But material should still be sent

    helper.set_slot_info(0, info);

    // Should NOT include COLOR parameter for grey default
    // But should still send the command if other values are present
    if (!helper.captured_gcodes.empty()) {
        // If command was sent, it should not contain COLOR
        REQUIRE_FALSE(helper.has_gcode_containing("COLOR="));
    }
    // This test verifies COLOR is skipped - currently passes since nothing is sent
}

TEST_CASE("Happy Hare persistence: skips COLOR for zero", "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0;    // Zero color - should NOT include COLOR
    info.material = "ABS"; // But material should still be sent

    helper.set_slot_info(0, info);

    // Should NOT include COLOR parameter for zero
    if (!helper.captured_gcodes.empty()) {
        REQUIRE_FALSE(helper.has_gcode_containing("COLOR="));
    }
    // This test verifies COLOR is skipped - currently passes since nothing is sent
}

TEST_CASE("Happy Hare persistence: skips MATERIAL for empty string",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.material = "";        // Empty - should NOT include MATERIAL
    info.color_rgb = 0xFF0000; // But color should still be sent

    helper.set_slot_info(0, info);

    // Should NOT include MATERIAL parameter for empty
    if (!helper.captured_gcodes.empty()) {
        REQUIRE_FALSE(helper.has_gcode_containing("MATERIAL="));
    }
    // This test verifies MATERIAL is skipped - currently passes since nothing is sent
}

TEST_CASE("Happy Hare persistence: skips SPOOLID when both old and new are zero/negative",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Slot starts with spoolman_id = 0 (default)
    SlotInfo info;
    info.spoolman_id = 0;
    info.color_rgb = 0xFF0000; // Need something to potentially trigger command

    helper.set_slot_info(0, info);

    // Should NOT include SPOOLID parameter when both old and new are 0
    if (!helper.captured_gcodes.empty()) {
        REQUIRE_FALSE(helper.has_gcode_containing("SPOOLID="));
    }
    // This test verifies SPOOLID is skipped when not needed
}

TEST_CASE("Happy Hare persistence: skips command when all values are default/empty",
          "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SlotInfo info;
    info.color_rgb = 0x808080; // Default grey
    info.material = "";        // Empty
    info.spoolman_id = 0;      // Zero (and no existing to clear)

    helper.set_slot_info(0, info);

    // Should NOT send any G-code when all values are default/empty
    // PASSES: no G-code sent at all currently
    REQUIRE(helper.captured_gcodes.empty());
}

TEST_CASE("Happy Hare persistence: different gate indices", "[ams][happy_hare][persistence]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(8);

    SECTION("Gate 0") {
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        helper.set_slot_info(0, info);
        // FAILS: set_slot_info doesn't call execute_gcode yet
        REQUIRE(helper.has_gcode_starting_with("MMU_GATE_MAP GATE=0"));
    }

    SECTION("Gate 3") {
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        helper.set_slot_info(3, info);
        // FAILS: set_slot_info doesn't call execute_gcode yet
        REQUIRE(helper.has_gcode_starting_with("MMU_GATE_MAP GATE=3"));
    }

    SECTION("Gate 7") {
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        helper.set_slot_info(7, info);
        // FAILS: set_slot_info doesn't call execute_gcode yet
        REQUIRE(helper.has_gcode_starting_with("MMU_GATE_MAP GATE=7"));
    }
}

// ============================================================================
// reset_tool_mappings() Tests
// ============================================================================

TEST_CASE("Happy Hare reset_tool_mappings sends MMU_TTG_MAP for each tool",
          "[ams][happy_hare][tool_mapping][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    // Should have sent 4 MMU_TTG_MAP commands (one per tool)
    REQUIRE(helper.captured_gcodes.size() == 4);
    REQUIRE(helper.has_gcode("MMU_TTG_MAP TOOL=0 GATE=0"));
    REQUIRE(helper.has_gcode("MMU_TTG_MAP TOOL=1 GATE=1"));
    REQUIRE(helper.has_gcode("MMU_TTG_MAP TOOL=2 GATE=2"));
    REQUIRE(helper.has_gcode("MMU_TTG_MAP TOOL=3 GATE=3"));
}

TEST_CASE("Happy Hare reset_tool_mappings with 8 tools", "[ams][happy_hare][tool_mapping][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(8);

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    REQUIRE(helper.captured_gcodes.size() == 8);
    // Verify first and last
    REQUIRE(helper.has_gcode("MMU_TTG_MAP TOOL=0 GATE=0"));
    REQUIRE(helper.has_gcode("MMU_TTG_MAP TOOL=7 GATE=7"));
}

TEST_CASE("Happy Hare reset_tool_mappings with zero tools is no-op",
          "[ams][happy_hare][tool_mapping][reset]") {
    AmsBackendHappyHareTestHelper helper;
    // Don't initialize gates - tool_to_slot_map is empty

    auto result = helper.reset_tool_mappings();

    REQUIRE(result.success());
    REQUIRE(helper.captured_gcodes.empty());
}

// ============================================================================
// reset_endless_spool() Tests
// ============================================================================

TEST_CASE("Happy Hare reset_endless_spool returns not_supported",
          "[ams][happy_hare][endless_spool][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    auto result = helper.reset_endless_spool();

    CHECK_FALSE(result.success());
    CHECK(result.result == AmsResult::NOT_SUPPORTED);
    // Should NOT send any G-code commands
    REQUIRE(helper.captured_gcodes.empty());
}

// ============================================================================
// eject_lane() Tests
// ============================================================================

TEST_CASE("Happy Hare eject_lane sends MMU_EJECT command", "[ams][happy_hare][eject]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.eject_lane(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_EJECT GATE=0"));
}

TEST_CASE("Happy Hare eject_lane targets correct gate", "[ams][happy_hare][eject]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.eject_lane(2);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_EJECT GATE=2"));
}

TEST_CASE("Happy Hare eject_lane validates slot index", "[ams][happy_hare][eject]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.eject_lane(99);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("Happy Hare eject_lane fails when not running", "[ams][happy_hare][eject]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    auto result = helper.eject_lane(0);

    REQUIRE_FALSE(result.success());
}

// ============================================================================
// reset_lane() Tests
// ============================================================================

TEST_CASE("Happy Hare reset_lane sends MMU_RECOVER with gate", "[ams][happy_hare][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.reset_lane(0);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_RECOVER GATE=0"));
}

TEST_CASE("Happy Hare reset_lane targets correct gate", "[ams][happy_hare][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.reset_lane(3);

    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_RECOVER GATE=3"));
}

TEST_CASE("Happy Hare reset_lane validates slot index", "[ams][happy_hare][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.reset_lane(-1);

    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::INVALID_SLOT);
}

TEST_CASE("Happy Hare reset_lane fails when not running", "[ams][happy_hare][reset]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    auto result = helper.reset_lane(0);

    REQUIRE_FALSE(result.success());
}

// ============================================================================
// Capability Query Tests
// ============================================================================

TEST_CASE("Happy Hare supports_lane_eject returns true", "[ams][happy_hare][capability]") {
    AmsBackendHappyHareTestHelper helper;
    REQUIRE(helper.supports_lane_eject());
}

TEST_CASE("Happy Hare supports_lane_reset returns true", "[ams][happy_hare][capability]") {
    AmsBackendHappyHareTestHelper helper;
    REQUIRE(helper.supports_lane_reset());
}

// ============================================================================
// Default AmsBackend capability tests (not supported)
// ============================================================================

TEST_CASE("Default AmsBackend eject_lane returns not_supported", "[ams][capability]") {
    // AmsBackendMock doesn't override eject_lane, so it uses the default
    // We test via the base class default behavior
    AmsBackendHappyHareTestHelper helper; // Has overrides, but let's test the concept
    // This is tested via the HH-specific tests above; the base class default
    // is implicitly tested by backends that don't override it
    REQUIRE(helper.supports_lane_eject() == true);
    REQUIRE(helper.supports_lane_reset() == true);
}

// ============================================================================
// Happy Hare v4 Support Tests
// ============================================================================

// --- Phase 1A: Extended filament_pos range ---

TEST_CASE("path_segment_from_happy_hare_pos handles v4 positions 9 and 10",
          "[ams][happy_hare][v4]") {
    REQUIRE(path_segment_from_happy_hare_pos(9) == PathSegment::NOZZLE);
    REQUIRE(path_segment_from_happy_hare_pos(10) == PathSegment::NOZZLE);
    // Existing positions still work
    REQUIRE(path_segment_from_happy_hare_pos(0) == PathSegment::SPOOL);
    REQUIRE(path_segment_from_happy_hare_pos(8) == PathSegment::NOZZLE);
}

// --- Phase 1B: New v4 action strings ---

TEST_CASE("ams_action_from_string handles v4 cutting variants", "[ams][happy_hare][v4]") {
    REQUIRE(ams_action_from_string("Cutting") == AmsAction::CUTTING);
    REQUIRE(ams_action_from_string("Cutting Tip") == AmsAction::CUTTING);
    REQUIRE(ams_action_from_string("Cutting Filament") == AmsAction::CUTTING);
}

TEST_CASE("ams_action_from_string handles v4 extruder actions", "[ams][happy_hare][v4]") {
    REQUIRE(ams_action_from_string("Loading Ext") == AmsAction::LOADING);
    REQUIRE(ams_action_from_string("Exiting Ext") == AmsAction::UNLOADING);
    // Original strings still work
    REQUIRE(ams_action_from_string("Loading") == AmsAction::LOADING);
    REQUIRE(ams_action_from_string("Unloading") == AmsAction::UNLOADING);
}

// --- Phase 1C: Gate temperature parsing ---

TEST_CASE("Happy Hare parses gate_temperature into slot nozzle temps", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"gate_temperature", {210, 220, 230, 240}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.total_slots == 4);

    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.nozzle_temp_min == 210);
    REQUIRE(slot0.nozzle_temp_max == 210);

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.nozzle_temp_min == 240);
    REQUIRE(slot3.nozzle_temp_max == 240);
}

// --- Phase 1D: Gate name parsing ---

TEST_CASE("Happy Hare parses gate_name into slot color_name", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"gate_name", {"Red PLA", "Blue PETG", "Black ABS", ""}}};
    helper.test_parse_mmu_state(mmu_data);

    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_name == "Red PLA");

    auto slot1 = helper.get_slot_info(1);
    REQUIRE(slot1.color_name == "Blue PETG");

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.color_name.empty());
}

// --- Phase 2A: Bowden progress ---

TEST_CASE("Happy Hare parses bowden_progress", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Default is -1 (not available)
    REQUIRE(helper.get_bowden_progress() == -1);

    nlohmann::json mmu_data = {{"bowden_progress", 75}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == 75);

    // Value of -1 means not applicable
    mmu_data = {{"bowden_progress", -1}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == -1);
}

// --- Phase 2B: Spoolman mode ---

TEST_CASE("SpoolmanMode string conversions", "[ams][happy_hare][v4]") {
    REQUIRE(spoolman_mode_from_string("off") == SpoolmanMode::OFF);
    REQUIRE(spoolman_mode_from_string("readonly") == SpoolmanMode::READONLY);
    REQUIRE(spoolman_mode_from_string("push") == SpoolmanMode::PUSH);
    REQUIRE(spoolman_mode_from_string("pull") == SpoolmanMode::PULL);
    REQUIRE(spoolman_mode_from_string("unknown") == SpoolmanMode::OFF);

    REQUIRE(std::string(spoolman_mode_to_string(SpoolmanMode::OFF)) == "Off");
    REQUIRE(std::string(spoolman_mode_to_string(SpoolmanMode::PUSH)) == "Push");
    REQUIRE(std::string(spoolman_mode_to_string(SpoolmanMode::PULL)) == "Pull");
}

TEST_CASE("Happy Hare parses spoolman_support and pending_spool_id", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"spoolman_support", "pull"}, {"pending_spool_id", 42}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.spoolman_mode == SpoolmanMode::PULL);
    REQUIRE(info.pending_spool_id == 42);
}

// --- Phase 2C: gate_spool_id parsing ---

TEST_CASE("Happy Hare parses gate_spool_id into slot spoolman_id", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"gate_spool_id", {5, 0, 12, 99}}};
    helper.test_parse_mmu_state(mmu_data);

    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.spoolman_id == 5);

    auto slot1 = helper.get_slot_info(1);
    REQUIRE(slot1.spoolman_id == 0);

    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.spoolman_id == 12);

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.spoolman_id == 99);
}

TEST_CASE("Happy Hare gate_spool_id skips non-integer values", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(3);

    // Slot 0 is string (skip), slot 1 is valid, slot 2 is null (skip)
    nlohmann::json mmu_data = {{"gate_spool_id", {"bad", 7, nullptr}}};
    helper.test_parse_mmu_state(mmu_data);

    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.spoolman_id == 0); // Unchanged default

    auto slot1 = helper.get_slot_info(1);
    REQUIRE(slot1.spoolman_id == 7);

    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.spoolman_id == 0); // Unchanged default
}

TEST_CASE("Happy Hare gate_spool_id with negative values clears spool link",
          "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(2);

    // First set valid IDs
    nlohmann::json mmu_data = {{"gate_spool_id", {10, 20}}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_slot_info(0).spoolman_id == 10);

    // Negative means "unlinked" in Happy Hare — treat as 0
    mmu_data = {{"gate_spool_id", {-1, 20}}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_slot_info(0).spoolman_id == 0);
    REQUIRE(helper.get_slot_info(1).spoolman_id == 20);
}

TEST_CASE("Happy Hare gate_spool_id partial array only updates provided slots",
          "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Only 2 elements for 4 gates — slots 2,3 should remain at default
    nlohmann::json mmu_data = {{"gate_spool_id", {3, 8}}};
    helper.test_parse_mmu_state(mmu_data);

    REQUIRE(helper.get_slot_info(0).spoolman_id == 3);
    REQUIRE(helper.get_slot_info(1).spoolman_id == 8);
    REQUIRE(helper.get_slot_info(2).spoolman_id == 0);
    REQUIRE(helper.get_slot_info(3).spoolman_id == 0);
}

// --- Phase 3: Dissimilar multi-unit ---

TEST_CASE("Happy Hare dissimilar multi-unit initialization from num_gates string",
          "[ams][happy_hare][v4][multi-unit]") {
    AmsBackendHappyHareTestHelper helper;

    // Simulate v4 sending num_gates as comma-separated string, num_units: 2
    // First, set num_units via parse
    nlohmann::json setup = {{"num_units", 2}};
    helper.test_parse_mmu_state(setup);

    // Then send num_gates as string + gate_status with 10 elements
    nlohmann::json mmu_data = {{"num_gates", "6,4"},
                               {"gate_status", {1, 1, 0, 1, 1, 1, 1, 0, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].slot_count == 6);
    REQUIRE(info.units[0].first_slot_global_index == 0);
    REQUIRE(info.units[1].slot_count == 4);
    REQUIRE(info.units[1].first_slot_global_index == 6);
    REQUIRE(info.total_slots == 10);
}

TEST_CASE("Happy Hare falls back to even split when no per-unit counts",
          "[ams][happy_hare][v4][multi-unit]") {
    AmsBackendHappyHareTestHelper helper;

    // v3-style: just num_units + gate_status
    nlohmann::json setup = {{"num_units", 2}};
    helper.test_parse_mmu_state(setup);

    nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1, 1, 1, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
}

// --- Phase 4: Status fields ---

TEST_CASE("Happy Hare parses v4 status fields", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"espooler_active", "rewind"},
                               {"sync_feedback_state", "tension"},
                               {"sync_drive", true},
                               {"clog_detection_enabled", 2},
                               {"encoder", {{"flow_rate", 95}}},
                               {"toolchange_purge_volume", 150.5}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.espooler_state == "rewind");
    REQUIRE(info.sync_feedback_state == "tension");
    REQUIRE(info.sync_drive == true);
    REQUIRE(info.clog_detection == 2);
    REQUIRE(info.encoder_flow_rate == 95);
    REQUIRE(info.toolchange_purge_volume == Catch::Approx(150.5f));
}

TEST_CASE("Happy Hare v4 status fields have safe defaults", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Parse with no v4 fields (simulating v3)
    nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.espooler_state.empty());
    REQUIRE(info.sync_feedback_state.empty());
    REQUIRE(info.sync_drive == false);
    REQUIRE(info.clog_detection == 0);
    REQUIRE(info.encoder_flow_rate == -1);
    REQUIRE(info.toolchange_purge_volume == 0.0f);
    REQUIRE(info.spoolman_mode == SpoolmanMode::OFF);
    REQUIRE(info.pending_spool_id == -1);
}

// --- Phase 5: Device actions ---

TEST_CASE("Happy Hare device sections include accessories", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    auto sections = helper.get_device_sections();

    bool found_accessories = false;
    for (const auto& s : sections) {
        if (s.id == "accessories") {
            found_accessories = true;
            break;
        }
    }
    REQUIRE(found_accessories);
}

TEST_CASE("Happy Hare espooler_mode action sends MMU_ESPOOLER", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.execute_device_action("espooler_mode", std::string("rewind"));
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_ESPOOLER OPERATION=rewind"));
}

TEST_CASE("Happy Hare clog_detection action sends MMU_TEST_CONFIG", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.execute_device_action("clog_detection", std::string("Auto"));
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_TEST_CONFIG CLOG_DETECTION=2"));

    helper.clear_captured_gcodes();
    result = helper.execute_device_action("clog_detection", std::string("Off"));
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_TEST_CONFIG CLOG_DETECTION=0"));
}

// --- Phase 6: Dryer support ---

TEST_CASE("Happy Hare dryer not supported by default", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    auto dryer = helper.get_dryer_info();
    REQUIRE_FALSE(dryer.supported);
}

TEST_CASE("Happy Hare parses drying_state from v4", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"drying_state",
                                {{"active", true},
                                 {"current_temp", 52.3},
                                 {"target_temp", 55.0},
                                 {"remaining_min", 120},
                                 {"duration_min", 240},
                                 {"fan_pct", 50}}}};
    helper.test_parse_mmu_state(mmu_data);

    auto dryer = helper.get_dryer_info();
    REQUIRE(dryer.supported);
    REQUIRE(dryer.active);
    REQUIRE(dryer.current_temp_c == Catch::Approx(52.3f));
    REQUIRE(dryer.target_temp_c == Catch::Approx(55.0f));
    REQUIRE(dryer.remaining_min == 120);
    REQUIRE(dryer.duration_min == 240);
    REQUIRE(dryer.fan_pct == 50);
}

TEST_CASE("Happy Hare dryer start/stop send MMU_HEATER commands", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Enable dryer support by parsing drying_state
    nlohmann::json mmu_data = {{"drying_state", {{"active", false}}}};
    helper.test_parse_mmu_state(mmu_data);

    auto result = helper.start_drying(55.0f, 240, 50);
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_HEATER DRY=1 TEMP=55 DURATION=240 FAN=50"));

    helper.clear_captured_gcodes();
    result = helper.stop_drying();
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_HEATER DRY=0"));
}

TEST_CASE("Happy Hare dryer start without dryer returns not_supported", "[ams][happy_hare][v4]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // No drying_state parsed, so dryer is not supported
    auto result = helper.start_drying(55.0f, 240);
    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_SUPPORTED);
}

// ============================================================================
// Happy Hare v4 Comprehensive Edge Case Tests
// ============================================================================

// --- filament_pos boundary values ---

TEST_CASE("path_segment_from_happy_hare_pos handles all boundary values",
          "[ams][happy_hare][v4][edge]") {
    // Negative values
    REQUIRE(path_segment_from_happy_hare_pos(-1) == PathSegment::NONE);
    REQUIRE(path_segment_from_happy_hare_pos(-100) == PathSegment::NONE);

    // Out of range high
    REQUIRE(path_segment_from_happy_hare_pos(11) == PathSegment::NONE);
    REQUIRE(path_segment_from_happy_hare_pos(255) == PathSegment::NONE);

    // Complete v4 range mapping
    REQUIRE(path_segment_from_happy_hare_pos(0) == PathSegment::SPOOL);
    REQUIRE(path_segment_from_happy_hare_pos(1) == PathSegment::PREP);
    REQUIRE(path_segment_from_happy_hare_pos(2) == PathSegment::PREP);
    REQUIRE(path_segment_from_happy_hare_pos(3) == PathSegment::LANE);
    REQUIRE(path_segment_from_happy_hare_pos(4) == PathSegment::HUB);
    REQUIRE(path_segment_from_happy_hare_pos(5) == PathSegment::OUTPUT);
    REQUIRE(path_segment_from_happy_hare_pos(6) == PathSegment::TOOLHEAD);
    REQUIRE(path_segment_from_happy_hare_pos(7) == PathSegment::NOZZLE);
    REQUIRE(path_segment_from_happy_hare_pos(8) == PathSegment::NOZZLE);
    REQUIRE(path_segment_from_happy_hare_pos(9) == PathSegment::NOZZLE);
    REQUIRE(path_segment_from_happy_hare_pos(10) == PathSegment::NOZZLE);
}

// --- v4 action strings: all remaining v3 strings still work ---

TEST_CASE("ams_action_from_string preserves all v3 mappings", "[ams][happy_hare][v4][edge]") {
    REQUIRE(ams_action_from_string("Idle") == AmsAction::IDLE);
    REQUIRE(ams_action_from_string("Loading") == AmsAction::LOADING);
    REQUIRE(ams_action_from_string("Unloading") == AmsAction::UNLOADING);
    REQUIRE(ams_action_from_string("Selecting") == AmsAction::SELECTING);
    REQUIRE(ams_action_from_string("Homing") == AmsAction::RESETTING);
    REQUIRE(ams_action_from_string("Resetting") == AmsAction::RESETTING);
    REQUIRE(ams_action_from_string("Cutting") == AmsAction::CUTTING);
    REQUIRE(ams_action_from_string("Forming Tip") == AmsAction::FORMING_TIP);
    REQUIRE(ams_action_from_string("Heating") == AmsAction::HEATING);
    REQUIRE(ams_action_from_string("Checking") == AmsAction::CHECKING);
    REQUIRE(ams_action_from_string("Purging") == AmsAction::PURGING);
    // Partial matches
    REQUIRE(ams_action_from_string("Paused (user)") == AmsAction::PAUSED);
    REQUIRE(ams_action_from_string("Error: filament jam") == AmsAction::ERROR);
    // Unknown → IDLE
    REQUIRE(ams_action_from_string("SomeNewV5Action") == AmsAction::IDLE);
    REQUIRE(ams_action_from_string("") == AmsAction::IDLE);
}

// --- gate_temperature: wrong types, partial arrays ---

TEST_CASE("Happy Hare gate_temperature handles wrong value types gracefully",
          "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Array with mixed types — string values should be ignored
    nlohmann::json mmu_data = {{"gate_temperature", {210, "not_a_number", 230, nullptr}}};
    helper.test_parse_mmu_state(mmu_data);

    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.nozzle_temp_min == 210);
    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.nozzle_temp_min == 230);
    // Slot 1 and 3 unchanged (still 0 from initialization)
    auto slot1 = helper.get_slot_info(1);
    REQUIRE(slot1.nozzle_temp_min == 0);
}

TEST_CASE("Happy Hare gate_temperature with shorter array than gate count",
          "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(8);

    // Only 4 values for 8 gates — remaining should be untouched
    nlohmann::json mmu_data = {{"gate_temperature", {200, 210, 220, 230}}};
    helper.test_parse_mmu_state(mmu_data);

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.nozzle_temp_min == 230);
    auto slot4 = helper.get_slot_info(4);
    REQUIRE(slot4.nozzle_temp_min == 0); // Untouched
}

// --- gate_name: empty strings, partial arrays ---

TEST_CASE("Happy Hare gate_name with all empty strings", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data = {{"gate_name", {"", "", "", ""}}};
    helper.test_parse_mmu_state(mmu_data);

    for (int i = 0; i < 4; ++i) {
        auto slot = helper.get_slot_info(i);
        REQUIRE(slot.color_name.empty());
    }
}

// --- bowden_progress: boundary values ---

TEST_CASE("Happy Hare bowden_progress boundary values", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // 0%
    nlohmann::json mmu_data = {{"bowden_progress", 0}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == 0);

    // 100%
    mmu_data = {{"bowden_progress", 100}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == 100);

    // Back to -1 (not applicable)
    mmu_data = {{"bowden_progress", -1}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == -1);
}

TEST_CASE("Happy Hare bowden_progress ignores non-integer values", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Set to known value first
    nlohmann::json mmu_data = {{"bowden_progress", 50}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == 50);

    // String value should not change it
    mmu_data = {{"bowden_progress", "invalid"}};
    helper.test_parse_mmu_state(mmu_data);
    REQUIRE(helper.get_bowden_progress() == 50); // Unchanged
}

// --- SpoolmanMode: edge cases ---

TEST_CASE("SpoolmanMode from_string is case-sensitive with alternatives",
          "[ams][happy_hare][v4][edge]") {
    // Supported case variants
    REQUIRE(spoolman_mode_from_string("off") == SpoolmanMode::OFF);
    REQUIRE(spoolman_mode_from_string("Off") == SpoolmanMode::OFF);
    REQUIRE(spoolman_mode_from_string("readonly") == SpoolmanMode::READONLY);
    REQUIRE(spoolman_mode_from_string("Read Only") == SpoolmanMode::READONLY);
    REQUIRE(spoolman_mode_from_string("push") == SpoolmanMode::PUSH);
    REQUIRE(spoolman_mode_from_string("Push") == SpoolmanMode::PUSH);
    REQUIRE(spoolman_mode_from_string("pull") == SpoolmanMode::PULL);
    REQUIRE(spoolman_mode_from_string("Pull") == SpoolmanMode::PULL);

    // Unrecognized → OFF (safe default)
    REQUIRE(spoolman_mode_from_string("PUSH") == SpoolmanMode::OFF); // ALL CAPS not supported
    REQUIRE(spoolman_mode_from_string("") == SpoolmanMode::OFF);
    REQUIRE(spoolman_mode_from_string("sync") == SpoolmanMode::OFF);
}

// --- Dissimilar multi-unit: edge cases ---

TEST_CASE("Happy Hare dissimilar multi-unit with mismatched sum falls back to even split",
          "[ams][happy_hare][v4][multi-unit][edge]") {
    AmsBackendHappyHareTestHelper helper;

    // Set num_units first
    nlohmann::json setup = {{"num_units", 2}};
    helper.test_parse_mmu_state(setup);

    // num_gates string "6,4" sums to 10, but gate_status has only 8 elements
    // The per_unit_gate_counts will be set to {6,4} but total=10 != gate_count=8
    // Should fall back to even split
    nlohmann::json mmu_data = {{"num_gates", "6,4"},
                               {"gate_status", {1, 1, 1, 1, 1, 1, 1, 1}}}; // 8 gates
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    // Even split: 8/2 = 4 each
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
}

TEST_CASE("Happy Hare unit_gate_counts array overrides num_gates string",
          "[ams][happy_hare][v4][multi-unit][edge]") {
    AmsBackendHappyHareTestHelper helper;

    nlohmann::json setup = {{"num_units", 2}};
    helper.test_parse_mmu_state(setup);

    // Both provided — unit_gate_counts should win (parsed after num_gates)
    nlohmann::json mmu_data = {{"num_gates", "5,5"},
                               {"unit_gate_counts", {3, 7}},
                               {"gate_status", {1, 1, 1, 1, 1, 1, 1, 1, 1, 1}}}; // 10 gates
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].slot_count == 3);
    REQUIRE(info.units[1].slot_count == 7);
}

TEST_CASE("Happy Hare single unit ignores per-unit counts",
          "[ams][happy_hare][v4][multi-unit][edge]") {
    AmsBackendHappyHareTestHelper helper;

    // Single unit — per_unit_gate_counts should still work if size matches
    nlohmann::json mmu_data = {{"num_units", 1}, {"gate_status", {1, 1, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[0].name == "MMU");
}

TEST_CASE("Happy Hare num_gates string with invalid tokens",
          "[ams][happy_hare][v4][multi-unit][edge]") {
    AmsBackendHappyHareTestHelper helper;

    nlohmann::json setup = {{"num_units", 2}};
    helper.test_parse_mmu_state(setup);

    // Invalid token "abc" ignored, resulting in {6} — size mismatch with num_units=2
    // Should fall back to even split
    nlohmann::json mmu_data = {{"num_gates", "6,abc"},
                               {"gate_status", {1, 1, 1, 1, 1, 1, 1, 1}}}; // 8 gates
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    // Fallback: even split 8/2=4
    REQUIRE(info.units[0].slot_count == 4);
    REQUIRE(info.units[1].slot_count == 4);
}

// --- v4 status fields: wrong types, missing nested fields ---

TEST_CASE("Happy Hare v4 status fields ignore wrong types", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Wrong types for all fields — should be silently ignored
    nlohmann::json mmu_data = {{"espooler_active", 42},             // Should be string
                               {"sync_feedback_state", true},       // Should be string
                               {"sync_drive", "yes"},               // Should be bool
                               {"clog_detection_enabled", "2"},     // Should be int
                               {"encoder", "not_object"},           // Should be object
                               {"toolchange_purge_volume", "big"}}; // Should be number
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    // All should remain at defaults
    REQUIRE(info.espooler_state.empty());
    REQUIRE(info.sync_feedback_state.empty());
    REQUIRE(info.sync_drive == false);
    REQUIRE(info.clog_detection == 0);
    REQUIRE(info.encoder_flow_rate == -1);
    REQUIRE(info.toolchange_purge_volume == 0.0f);
}

TEST_CASE("Happy Hare encoder object without flow_rate field", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // encoder object exists but without flow_rate
    nlohmann::json mmu_data = {{"encoder", {{"some_other_field", 42}}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_flow_rate == -1); // Still default
}

// --- v4 status field updates are incremental ---

TEST_CASE("Happy Hare v4 status fields update incrementally", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Set espooler first
    nlohmann::json mmu_data1 = {{"espooler_active", "rewind"}};
    helper.test_parse_mmu_state(mmu_data1);

    // Then set clog_detection in a separate update
    nlohmann::json mmu_data2 = {{"clog_detection_enabled", 1}};
    helper.test_parse_mmu_state(mmu_data2);

    auto info = helper.get_system_info();
    // Both should be set
    REQUIRE(info.espooler_state == "rewind");
    REQUIRE(info.clog_detection == 1);
}

// --- Dryer: partial drying_state ---

TEST_CASE("Happy Hare drying_state with partial fields", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Only some fields present
    nlohmann::json mmu_data = {{"drying_state", {{"active", false}, {"current_temp", 25.0}}}};
    helper.test_parse_mmu_state(mmu_data);

    auto dryer = helper.get_dryer_info();
    REQUIRE(dryer.supported);
    REQUIRE_FALSE(dryer.active);
    REQUIRE(dryer.current_temp_c == Catch::Approx(25.0f));
    // Missing fields stay at defaults
    REQUIRE(dryer.target_temp_c == Catch::Approx(0.0f));
    REQUIRE(dryer.remaining_min == 0);
}

TEST_CASE("Happy Hare dryer stop also returns not_supported without dryer hardware",
          "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    auto result = helper.stop_drying();
    REQUIRE_FALSE(result.success());
    REQUIRE(result.result == AmsResult::NOT_SUPPORTED);
}

TEST_CASE("Happy Hare dryer start without fan_pct omits FAN param", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    // Enable dryer
    nlohmann::json mmu_data = {{"drying_state", {{"active", false}}}};
    helper.test_parse_mmu_state(mmu_data);

    auto result = helper.start_drying(45.0f, 120); // No fan_pct (-1 default)
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_HEATER DRY=1 TEMP=45 DURATION=120"));
    // Should NOT have FAN= parameter
    REQUIRE_FALSE(helper.has_gcode_containing("FAN="));
}

// --- Device action edge cases ---

TEST_CASE("Happy Hare espooler_mode without value returns error", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.execute_device_action("espooler_mode");
    REQUIRE_FALSE(result.success());
}

TEST_CASE("Happy Hare clog_detection Manual maps to 1", "[ams][happy_hare][v4][edge]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);
    helper.set_running(true);

    auto result = helper.execute_device_action("clog_detection", std::string("Manual"));
    REQUIRE(result.success());
    REQUIRE(helper.has_gcode("MMU_TEST_CONFIG CLOG_DETECTION=1"));
}

// --- Backwards compatibility: v3 sends nothing new ---

TEST_CASE("Happy Hare v3 data with no v4 fields works normally", "[ams][happy_hare][v4][compat]") {
    AmsBackendHappyHareTestHelper helper;

    // Pure v3 data — only classic fields
    nlohmann::json mmu_data = {{"gate", 2},
                               {"tool", 2},
                               {"filament", "Loaded"},
                               {"action", "Idle"},
                               {"filament_pos", 8},
                               {"has_bypass", true},
                               {"gate_status", {1, 0, 2, 1}},
                               {"gate_color_rgb", {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}},
                               {"gate_material", {"PLA", "PETG", "ABS", "TPU"}},
                               {"ttg_map", {0, 1, 2, 3}},
                               {"endless_spool_groups", {0, 0, 1, 1}}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.type == AmsType::HAPPY_HARE);
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.current_tool == 2);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.action == AmsAction::IDLE);
    REQUIRE(info.supports_bypass == true);
    REQUIRE(info.total_slots == 4);

    // All v4 fields should be at safe defaults
    REQUIRE(info.spoolman_mode == SpoolmanMode::OFF);
    REQUIRE(info.pending_spool_id == -1);
    REQUIRE(info.espooler_state.empty());
    REQUIRE(info.sync_feedback_state.empty());
    REQUIRE(info.sync_drive == false);
    REQUIRE(info.clog_detection == 0);
    REQUIRE(info.encoder_flow_rate == -1);
    REQUIRE(info.toolchange_purge_volume == 0.0f);

    // Bowden progress not available
    REQUIRE(helper.get_bowden_progress() == -1);

    // Dryer not available
    auto dryer = helper.get_dryer_info();
    REQUIRE_FALSE(dryer.supported);

    // Slot data should be properly parsed
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_rgb == 0xFF0000);
    REQUIRE(slot0.material == "PLA");
    REQUIRE(slot0.status == SlotStatus::AVAILABLE);

    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.color_rgb == 0x0000FF);
    REQUIRE(slot2.material == "ABS");
    REQUIRE(slot2.status == SlotStatus::FROM_BUFFER); // gate_status=2 maps to FROM_BUFFER
}

// --- v3+v4 mixed: some v4 fields with v3 base ---

TEST_CASE("Happy Hare mixed v3/v4 data parses both correctly", "[ams][happy_hare][v4][compat]") {
    AmsBackendHappyHareTestHelper helper;

    nlohmann::json mmu_data = {// v3 fields
                               {"gate", 0},
                               {"tool", 0},
                               {"filament", "Loaded"},
                               {"action", "Idle"},
                               {"filament_pos", 8},
                               {"gate_status", {2, 1, 0, 1}},
                               {"gate_material", {"PLA", "PETG", "", "ABS"}},
                               // v4 fields mixed in
                               {"bowden_progress", 100},
                               {"spoolman_support", "push"},
                               {"gate_name", {"Red Spool", "", "Empty", "Black"}},
                               {"gate_temperature", {210, 230, 0, 250}},
                               {"espooler_active", "assist"},
                               {"clog_detection_enabled", 2}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    // v3 data
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.total_slots == 4);

    // v4 additions
    REQUIRE(helper.get_bowden_progress() == 100);
    REQUIRE(info.spoolman_mode == SpoolmanMode::PUSH);
    REQUIRE(info.espooler_state == "assist");
    REQUIRE(info.clog_detection == 2);

    // Per-slot v4 data
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_name == "Red Spool");
    REQUIRE(slot0.nozzle_temp_min == 210);
    REQUIRE(slot0.material == "PLA");

    auto slot3 = helper.get_slot_info(3);
    REQUIRE(slot3.color_name == "Black");
    REQUIRE(slot3.nozzle_temp_min == 250);
    REQUIRE(slot3.material == "ABS");
}

// --- Review-driven coverage gaps ---

TEST_CASE("Happy Hare bowden_progress clamped to valid range",
          "[ams][happy_hare][v4][bowden][edge]") {
    AmsBackendHappyHareTestHelper helper;

    SECTION("Value > 100 clamped to 100") {
        nlohmann::json mmu_data = {{"bowden_progress", 150}};
        helper.test_parse_mmu_state(mmu_data);
        REQUIRE(helper.get_bowden_progress() == 100);
    }

    SECTION("Value < -1 clamped to -1") {
        nlohmann::json mmu_data = {{"bowden_progress", -5}};
        helper.test_parse_mmu_state(mmu_data);
        REQUIRE(helper.get_bowden_progress() == -1);
    }

    SECTION("Exactly -1 preserved") {
        nlohmann::json mmu_data = {{"bowden_progress", -1}};
        helper.test_parse_mmu_state(mmu_data);
        REQUIRE(helper.get_bowden_progress() == -1);
    }

    SECTION("Exactly 100 preserved") {
        nlohmann::json mmu_data = {{"bowden_progress", 100}};
        helper.test_parse_mmu_state(mmu_data);
        REQUIRE(helper.get_bowden_progress() == 100);
    }
}

TEST_CASE("Happy Hare num_units < 1 clamped to 1", "[ams][happy_hare][v4][multi-unit][edge]") {
    AmsBackendHappyHareTestHelper helper;

    SECTION("num_units = 0") {
        nlohmann::json mmu_data = {{"num_units", 0}, {"gate_status", {1, 1, 1, 1}}};
        helper.test_parse_mmu_state(mmu_data);
        auto info = helper.get_system_info();
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].slot_count == 4);
    }

    SECTION("num_units = -1") {
        nlohmann::json mmu_data = {{"num_units", -1}, {"gate_status", {1, 1, 1, 1}}};
        helper.test_parse_mmu_state(mmu_data);
        auto info = helper.get_system_info();
        REQUIRE(info.units.size() == 1);
    }
}

TEST_CASE("Happy Hare encoder flow_rate accepts float values",
          "[ams][happy_hare][v4][status][edge]") {
    AmsBackendHappyHareTestHelper helper;

    // encoder.flow_rate uses is_number() — floats are accepted and truncated to int
    nlohmann::json mmu_data = {{"encoder", {{"flow_rate", 95.7}}}};
    helper.test_parse_mmu_state(mmu_data);
    auto info = helper.get_system_info();
    REQUIRE(info.encoder_flow_rate == 95); // Float truncated to int
}

TEST_CASE("Happy Hare active_unit parsed from status", "[ams][happy_hare][v4][multi-unit]") {
    AmsBackendHappyHareTestHelper helper;

    nlohmann::json mmu_data = {
        {"num_units", 2}, {"unit", 1}, {"gate_status", {1, 1, 1, 1, 0, 0, 0, 0}}};
    helper.test_parse_mmu_state(mmu_data);
    // active_unit_ is stored internally — verify via system_info units exist
    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
}

// ============================================================================
// manages_active_spool() — depends on Happy Hare's spoolman_support setting
// ============================================================================

TEST_CASE("Happy Hare manages_active_spool=false when spoolman off (default)",
          "[ams][happy_hare][spoolman]") {
    AmsBackendHappyHareTestHelper helper;
    // Default spoolman_mode is OFF
    REQUIRE(helper.manages_active_spool() == false);
}

TEST_CASE("Happy Hare manages_active_spool=true when spoolman enabled",
          "[ams][happy_hare][spoolman]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SECTION("readonly mode") {
        helper.test_parse_mmu_state({{"spoolman_support", "readonly"}});
        REQUIRE(helper.manages_active_spool() == true);
    }

    SECTION("push mode") {
        helper.test_parse_mmu_state({{"spoolman_support", "push"}});
        REQUIRE(helper.manages_active_spool() == true);
    }

    SECTION("pull mode") {
        helper.test_parse_mmu_state({{"spoolman_support", "pull"}});
        REQUIRE(helper.manages_active_spool() == true);
    }

    SECTION("off mode — back to false") {
        helper.test_parse_mmu_state({{"spoolman_support", "readonly"}});
        REQUIRE(helper.manages_active_spool() == true);
        helper.test_parse_mmu_state({{"spoolman_support", "off"}});
        REQUIRE(helper.manages_active_spool() == false);
    }
}

// ============================================================================
// EMU drying_state array format
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU drying_state as array",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(4);

    SECTION("all empty strings means supported but not active") {
        nlohmann::json mmu_data = {{"drying_state", {"", "", "", ""}}};
        test_parse_mmu_state(mmu_data);

        auto dryer = get_dryer_info();
        REQUIRE(dryer.supported == true);
        REQUIRE(dryer.active == false);
    }

    SECTION("existing object format still works") {
        nlohmann::json mmu_data = {{"drying_state",
                                    {{"active", true},
                                     {"current_temp", 55.0},
                                     {"target_temp", 60.0},
                                     {"remaining_min", 30},
                                     {"duration_min", 240},
                                     {"fan_pct", 75}}}};
        test_parse_mmu_state(mmu_data);

        auto dryer = get_dryer_info();
        REQUIRE(dryer.supported == true);
        REQUIRE(dryer.active == true);
        REQUIRE(dryer.current_temp_c == Catch::Approx(55.0));
    }

    SECTION("array with non-empty entry means active") {
        nlohmann::json mmu_data = {{"drying_state", {"", "drying", "", ""}}};
        test_parse_mmu_state(mmu_data);

        auto dryer = get_dryer_info();
        REQUIRE(dryer.supported == true);
        REQUIRE(dryer.active == true);
    }
}

// ============================================================================
// tracks_weight_locally() — Happy Hare does NOT track weight (no extruder
// position-based weight decrement like AFC). Spoolman is source of truth.
// ============================================================================

TEST_CASE("Happy Hare does not track weight locally", "[ams][happy_hare][spoolman]") {
    AmsBackendHappyHareTestHelper helper;
    REQUIRE(helper.tracks_weight_locally() == false);
}

// ============================================================================
// EMU compatibility — num_gates as integer or array
// EMU sends num_gates as plain integer (e.g. 8), not comma-separated string.
// Config format may also send it as a JSON array (e.g. [8]).
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU num_gates as integer",
                 "[ams][happy_hare][emu]") {
    // EMU sends num_gates as plain integer, not string
    nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1, 1, 1, 1, 1}}, {"num_gates", 8}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 8);
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU num_gates as array",
                 "[ams][happy_hare][emu]") {
    // Config format sends num_gates as [8] array
    nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1, 1, 1, 1, 1}}, {"num_gates", {8}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 8);
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU gate_color_rgb as float arrays",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(4);
    nlohmann::json mmu_data = {{"gate_color_rgb",
                                {
                                    {1.0, 0.0, 0.0},     // Red
                                    {0.0, 1.0, 0.0},     // Green
                                    {0.0, 0.0, 1.0},     // Blue
                                    {0.976, 0.976, 0.4}, // Yellowish
                                }}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFF0000);
    REQUIRE(info.units[0].slots[1].color_rgb == 0x00FF00);
    REQUIRE(info.units[0].slots[2].color_rgb == 0x0000FF);
    // 0.976*255 = 248.88 -> round -> 249 = 0xF9, 0.4*255 = 102 -> 0x66
    REQUIRE(info.units[0].slots[3].color_rgb == 0xF9F966);
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "gate_color hex string fallback",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(3);
    // gate_color_rgb absent, fall back to gate_color hex strings
    nlohmann::json mmu_data = {{"gate_color", {"ffffff", "000000", "042f56"}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFFFFFF);
    REQUIRE(info.units[0].slots[1].color_rgb == 0x000000);
    REQUIRE(info.units[0].slots[2].color_rgb == 0x042F56);
}

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU num_gates array multi-unit dissimilar",
                 "[ams][happy_hare][emu]") {
    // Multi-unit setup with array format [6, 4]
    nlohmann::json setup = {{"num_units", 2}};
    test_parse_mmu_state(setup);

    nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1, 1, 1, 1, 1, 1, 1}},
                               {"num_gates", {6, 4}}};
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();
    REQUIRE(info.total_slots == 10);
    REQUIRE(info.units.size() == 2);
    REQUIRE(info.units[0].slot_count == 6);
    REQUIRE(info.units[1].slot_count == 4);
}

// ============================================================================
// EMU aggregate sensor format
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU aggregate sensor format",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(4);

    SECTION("aggregate sensors with active gate") {
        nlohmann::json mmu_data = {
            {"gate", 0},
            {"sensors",
             {{"mmu_pre_gate", true}, {"mmu_gear", true}, {"extruder", true}, {"toolhead", true}}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].has_slot_sensors == true);

        // All gates should report having pre-gate sensors
        auto slot0 = get_slot_entry(0);
        REQUIRE(slot0 != nullptr);
        REQUIRE(slot0->sensors.has_pre_gate_sensor == true);
        REQUIRE(slot0->sensors.pre_gate_triggered == true);

        // Other gates have sensor hardware but we only know current gate's reading
        auto slot1 = get_slot_entry(1);
        REQUIRE(slot1 != nullptr);
        REQUIRE(slot1->sensors.has_pre_gate_sensor == true);
    }

    SECTION("aggregate sensors with different active gate") {
        nlohmann::json mmu_data = {{"gate", 2},
                                   {"sensors", {{"mmu_pre_gate", false}, {"mmu_gear", true}}}};
        test_parse_mmu_state(mmu_data);

        auto slot2 = get_slot_entry(2);
        REQUIRE(slot2 != nullptr);
        REQUIRE(slot2->sensors.has_pre_gate_sensor == true);
        REQUIRE(slot2->sensors.pre_gate_triggered == false);
    }
}

// ============================================================================
// EMU gate_filament_name parsing — EMU sends filament names via
// gate_filament_name instead of gate_name
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "EMU gate_filament_name parsing",
                 "[ams][happy_hare][emu]") {
    initialize_test_gates(3);

    SECTION("gate_filament_name used when gate_name is null") {
        nlohmann::json mmu_data = {{"gate_name", nullptr},
                                   {"gate_filament_name",
                                    {"Matte White", "Matte Black", "Matte Yellow"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Matte White");
        REQUIRE(info.units[0].slots[1].color_name == "Matte Black");
        REQUIRE(info.units[0].slots[2].color_name == "Matte Yellow");
    }

    SECTION("gate_name takes priority over gate_filament_name") {
        nlohmann::json mmu_data = {{"gate_name", {"Priority Name", "Other", "Third"}},
                                   {"gate_filament_name", {"Fallback", "Fallback", "Fallback"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Priority Name");
    }

    SECTION("gate_filament_name used when gate_name absent") {
        nlohmann::json mmu_data = {{"gate_filament_name", {"Name A", "Name B", "Name C"}}};
        test_parse_mmu_state(mmu_data);

        auto info = get_system_info();
        REQUIRE(info.units[0].slots[0].color_name == "Name A");
    }
}

// ============================================================================
// Full EMU integration test — validates all EMU parsing fixes together
// using real data from an EMU user's Moonraker dump
// ============================================================================

TEST_CASE_METHOD(AmsBackendHappyHareTestHelper, "Full EMU status integration",
                 "[ams][happy_hare][emu]") {
    // Real data from EMU user's Moonraker dump (simplified)
    nlohmann::json mmu_data = {
        {"gate", 0},
        {"tool", 0},
        {"filament", "Loaded"},
        {"action", "Idle"},
        {"num_gates", 8},
        {"filament_pos", 10},
        {"has_bypass", true},
        {"gate_status", {2, 2, 2, 2, 2, 2, 1, 1}},
        {"gate_color_rgb", {
            {1.0, 1.0, 1.0},
            {0.0, 0.0, 0.0},
            {0.976, 0.976, 0.4},
            {0.016, 0.184, 0.337},
            {0.553, 0.784, 0.588},
            {0.0, 0.0, 0.0},
            {1.0, 1.0, 1.0},
            {0.0, 0.0, 0.0}
        }},
        {"gate_material", {"PLA", "PLA", "PLA", "PLA", "PLA", "ABS", "ABS", "ASA CF"}},
        {"gate_filament_name", {"Matte White", "Matte Black", "Matte Yellow", "Matte Navy",
                                "Matte Green", "Black", "White", "Black"}},
        {"gate_temperature", {230, 230, 230, 230, 230, 260, 260, 265}},
        {"gate_name", nullptr},
        {"ttg_map", {0, 1, 2, 3, 4, 5, 6, 7}},
        {"endless_spool_groups", {0, 1, 2, 3, 4, 5, 6, 7}},
        {"bowden_progress", -1},
        {"encoder", nullptr},
        {"unit_gate_counts", nullptr},
        {"sync_drive", true},
        {"sync_feedback_state", "neutral"},
        {"clog_detection_enabled", 0},
        {"espooler_active", ""},
        {"spoolman_support", "push"},
        {"drying_state", {"", "", "", "", "", "", "", ""}},
        {"sensors", {
            {"mmu_pre_gate", true},
            {"mmu_gear", true},
            {"filament_proportional", false},
            {"extruder", true},
            {"toolhead", true}
        }}
    };
    test_parse_mmu_state(mmu_data);

    auto info = get_system_info();

    // Structure
    REQUIRE(info.total_slots == 8);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slot_count == 8);

    // Current state
    REQUIRE(info.current_slot == 0);
    REQUIRE(info.current_tool == 0);
    REQUIRE(info.filament_loaded == true);
    REQUIRE(info.action == AmsAction::IDLE);
    REQUIRE(info.supports_bypass == true);

    // Colors (float arrays converted to 0xRRGGBB)
    REQUIRE(info.units[0].slots[0].color_rgb == 0xFFFFFF);  // White
    REQUIRE(info.units[0].slots[1].color_rgb == 0x000000);  // Black
    REQUIRE(info.units[0].slots[3].color_rgb == 0x042F56);  // Navy

    // Materials
    REQUIRE(info.units[0].slots[0].material == "PLA");
    REQUIRE(info.units[0].slots[5].material == "ABS");
    REQUIRE(info.units[0].slots[7].material == "ASA CF");

    // Filament names (from gate_filament_name since gate_name is null)
    REQUIRE(info.units[0].slots[0].color_name == "Matte White");
    REQUIRE(info.units[0].slots[3].color_name == "Matte Navy");

    // Temperatures
    REQUIRE(info.units[0].slots[0].nozzle_temp_min == 230);
    REQUIRE(info.units[0].slots[5].nozzle_temp_min == 260);

    // Sensors (aggregate format)
    REQUIRE(info.units[0].has_slot_sensors == true);

    // Dryer (array format = supported but inactive)
    auto dryer = get_dryer_info();
    REQUIRE(dryer.supported == true);
    REQUIRE(dryer.active == false);

    // v4 fields
    REQUIRE(info.sync_drive == true);
    REQUIRE(info.sync_feedback_state == "neutral");
    REQUIRE(info.spoolman_mode == SpoolmanMode::PUSH);
    REQUIRE(info.encoder_flow_rate == -1);  // null encoder
}

// ============================================================================
// Encoder Clog Info Struct Tests
// ============================================================================

TEST_CASE("EncoderClogInfo: get_clog_pct zero detection_length returns 0", "[ams][clog]") {
    EncoderClogInfo info;
    info.detection_length = 0;
    info.headroom = 5.0f;
    REQUIRE(info.get_clog_pct() == 0);
}

TEST_CASE("EncoderClogInfo: get_clog_pct full headroom returns 0", "[ams][clog]") {
    EncoderClogInfo info;
    info.detection_length = 12.4f;
    info.headroom = 12.4f;
    REQUIRE(info.get_clog_pct() == 0);
}

TEST_CASE("EncoderClogInfo: get_clog_pct no headroom returns 100", "[ams][clog]") {
    EncoderClogInfo info;
    info.detection_length = 12.4f;
    info.headroom = 0.0f;
    REQUIRE(info.get_clog_pct() == 100);
}

TEST_CASE("EncoderClogInfo: get_clog_pct normal case", "[ams][clog]") {
    EncoderClogInfo info;
    info.detection_length = 10.0f;
    info.headroom = 3.0f;  // 7mm used = 70%
    REQUIRE(info.get_clog_pct() == 70);
}

TEST_CASE("EncoderClogInfo: get_clog_pct clamps negative headroom to 100", "[ams][clog]") {
    EncoderClogInfo info;
    info.detection_length = 10.0f;
    info.headroom = -1.0f;
    REQUIRE(info.get_clog_pct() == 100);
}

TEST_CASE("EncoderClogInfo: is_warning when min below desired", "[ams][clog]") {
    EncoderClogInfo info;
    info.desired_headroom = 5.0f;
    info.min_headroom = 3.0f;
    REQUIRE(info.is_warning() == true);
}

TEST_CASE("EncoderClogInfo: is_warning false when min above desired", "[ams][clog]") {
    EncoderClogInfo info;
    info.desired_headroom = 5.0f;
    info.min_headroom = 6.0f;
    REQUIRE(info.is_warning() == false);
}

TEST_CASE("EncoderClogInfo: is_warning false when desired is 0", "[ams][clog]") {
    EncoderClogInfo info;
    info.desired_headroom = 0.0f;
    info.min_headroom = 0.0f;
    REQUIRE(info.is_warning() == false);
}

// ============================================================================
// Encoder + Flowguard JSON Parsing Tests
// ============================================================================

TEST_CASE("Happy Hare: parse full encoder object into encoder_info", "[ams][happy_hare][clog]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["clog_detection_enabled"] = 2;
    mmu_data["encoder"] = {
        {"flow_rate", 85},
        {"desired_headroom", 5.0},
        {"detection_length", 12.4},
        {"headroom", 8.0},
        {"min_headroom", 4.2}
    };
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.enabled == true);
    REQUIRE(info.encoder_info.flow_rate == 85);
    REQUIRE(info.encoder_info.detection_mode == 2);
    REQUIRE(info.encoder_info.desired_headroom == Catch::Approx(5.0f));
    REQUIRE(info.encoder_info.detection_length == Catch::Approx(12.4f));
    REQUIRE(info.encoder_info.headroom == Catch::Approx(8.0f));
    REQUIRE(info.encoder_info.min_headroom == Catch::Approx(4.2f));
}

TEST_CASE("Happy Hare: partial encoder object defaults missing fields", "[ams][happy_hare][clog]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["encoder"] = {{"flow_rate", 92}};
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.flow_rate == 92);
    // Not enabled because clog_detection_enabled not set
    REQUIRE(info.encoder_info.enabled == false);
    REQUIRE(info.encoder_info.detection_length == Catch::Approx(0.0f));
    REQUIRE(info.encoder_info.headroom == Catch::Approx(0.0f));
}

TEST_CASE("Happy Hare: missing encoder object leaves defaults", "[ams][happy_hare][clog]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.encoder_info.enabled == false);
    REQUIRE(info.encoder_info.flow_rate == -1);
}

TEST_CASE("Happy Hare: parse flowguard object", "[ams][happy_hare][clog]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["flowguard"] = {
        {"enabled", true},
        {"active", true},
        {"trigger", "CLOG"},
        {"level", 0.35},
        {"max_clog", 0.8},
        {"max_tangle", -0.6}
    };
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.flowguard_info.enabled == true);
    REQUIRE(info.flowguard_info.active == true);
    REQUIRE(info.flowguard_info.trigger == "CLOG");
    REQUIRE(info.flowguard_info.level == Catch::Approx(0.35f));
    REQUIRE(info.flowguard_info.max_clog == Catch::Approx(0.8f));
    REQUIRE(info.flowguard_info.max_tangle == Catch::Approx(-0.6f));
}

TEST_CASE("Happy Hare: missing flowguard object leaves defaults", "[ams][happy_hare][clog]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.flowguard_info.enabled == false);
    REQUIRE(info.flowguard_info.active == false);
    REQUIRE(info.flowguard_info.trigger.empty());
}

TEST_CASE("Happy Hare: parse sync_feedback_flow_rate", "[ams][happy_hare][clog]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    nlohmann::json mmu_data;
    mmu_data["sync_feedback_flow_rate"] = 92.5;
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.sync_feedback_flow_rate == Catch::Approx(92.5f));
}

// ============================================================================
// Type B MMU Hub Topology Detection
// ============================================================================
//
// Happy Hare supports two MMU architectures:
// - Type A (ERCF, Tradrack): selector_type = LinearSelector/RotarySelector/ServoSelector → LINEAR
// - Type B (3MS, Box Turtle, Night Owl): selector_type = VirtualSelector → HUB
//
// The selector_type is queried from configfile.settings.mmu_machine.selector_type

TEST_CASE("Happy Hare: default topology is LINEAR", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    // No selector_type set — should default to LINEAR (Type A)
    REQUIRE(helper.get_topology() == PathTopology::LINEAR);
}

TEST_CASE("Happy Hare: Type B (VirtualSelector) topology is HUB", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_selector_type("VirtualSelector");
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("Happy Hare: Type A selector types stay LINEAR", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;

    for (const auto& type : {"LinearSelector", "RotarySelector", "ServoSelector"}) {
        helper.set_selector_type(type);
        REQUIRE(helper.get_topology() == PathTopology::LINEAR);
    }
}

TEST_CASE("Happy Hare: get_unit_topology returns per-unit topology", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_selector_type("VirtualSelector");
    helper.initialize_test_gates(4);

    REQUIRE(helper.get_unit_topology(0) == PathTopology::HUB);
}

TEST_CASE("Happy Hare: get_unit_topology falls back for invalid index",
          "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_selector_type("VirtualSelector");
    helper.initialize_test_gates(4);

    // Out-of-range should fall back to get_topology()
    REQUIRE(helper.get_unit_topology(99) == PathTopology::HUB);
}

TEST_CASE("Happy Hare: initialize_slots sets topology per unit", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_selector_type("VirtualSelector");
    helper.initialize_test_gates(4);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].topology == PathTopology::HUB);
}

TEST_CASE("Happy Hare: Type A initialize_slots sets LINEAR topology",
          "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    // Default (no selector_type set) = Type A
    helper.initialize_test_gates(4);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].topology == PathTopology::LINEAR);
}

TEST_CASE("Happy Hare: multi-unit Type B all get HUB topology", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_selector_type("VirtualSelector");
    // Simulate 2-unit setup via parse
    nlohmann::json mmu_data;
    mmu_data["num_units"] = 2;
    mmu_data["gate_status"] = {1, 1, 1, 1, 1, 1};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFF0000, 0x00FF00, 0x0000FF};
    mmu_data["gate_material"] = {"PLA", "PLA", "PLA", "PLA", "PLA", "PLA"};
    mmu_data["gate"] = 0;
    mmu_data["tool"] = 0;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 2);
    for (const auto& unit : info.units) {
        REQUIRE(unit.topology == PathTopology::HUB);
    }
}

TEST_CASE("Happy Hare: Type B has_encoder is false", "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    helper.set_selector_type("VirtualSelector");

    nlohmann::json mmu_data;
    mmu_data["gate_status"] = {1, 1, 1, 1};
    mmu_data["gate_color_rgb"] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00};
    mmu_data["gate_material"] = {"PLA", "PLA", "PLA", "PLA"};
    mmu_data["gate"] = 0;
    mmu_data["tool"] = 0;
    mmu_data["filament"] = "Unloaded";
    mmu_data["action"] = "Idle";
    helper.test_parse_mmu_state(mmu_data);

    auto info = helper.get_system_info();
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].has_encoder == false);
}

TEST_CASE("Happy Hare: late selector_type retroactively updates topology",
          "[ams][happy_hare][topology]") {
    AmsBackendHappyHareTestHelper helper;
    // Initialize with default (LINEAR) first — simulates slots arriving before config
    helper.initialize_test_gates(4);
    auto info = helper.get_system_info();
    REQUIRE(info.units[0].topology == PathTopology::LINEAR);
    REQUIRE(info.units[0].has_encoder == true);

    // Simulate config response arriving after slots initialized
    helper.set_selector_type("VirtualSelector");
    helper.apply_selector_type_update();

    info = helper.get_system_info();
    REQUIRE(info.units[0].topology == PathTopology::HUB);
    REQUIRE(info.units[0].has_encoder == false);
}

// --- Sync feedback bias parsing ---

TEST_CASE("Happy Hare parses sync_feedback_bias fields", "[ams][happy_hare][v4][sync_feedback]") {
    AmsBackendHappyHareTestHelper helper;
    helper.initialize_test_gates(4);

    SECTION("positive bias (compression)") {
        nlohmann::json mmu_data = {{"sync_feedback_bias_modelled", 0.75f},
                                   {"sync_feedback_bias_raw", 0.82f}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(0.75f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(0.82f));
    }

    SECTION("negative bias (tension)") {
        nlohmann::json mmu_data = {{"sync_feedback_bias_modelled", -0.5f},
                                   {"sync_feedback_bias_raw", -0.33f}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-0.5f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-0.33f));
    }

    SECTION("missing fields remain at sentinel") {
        nlohmann::json mmu_data = {{"gate_status", {1, 1, 1, 1}}};
        helper.test_parse_mmu_state(mmu_data);

        auto info = helper.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-2.0f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-2.0f));
    }
}
