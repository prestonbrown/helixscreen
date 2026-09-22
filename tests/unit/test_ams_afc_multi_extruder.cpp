// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_afc_multi_extruder.cpp
 * @brief Parsing of the AFC.system WEBHOOK payload — NOT what the wire sends.
 *
 * Every test here tagged [webhook] feeds `AFC.system`, the object AFC publishes
 * from `_webhooks_status()` at `GET /printer/afc/status`. **HelixScreen does not
 * call that endpoint.** `AFC.py get_status()` — the status subscription we do
 * consume — publishes current_load / current_lane / next_lane / current_state /
 * spoolman / error_state / units / lanes and no `system` key, verified against
 * the add-on source on a live BoxTurtle (v1.1.0-4-g2921371).
 *
 * So these tests exercise a shape production never receives. They are kept
 * because `parse_afc_state()` still parses `system` and that parse is genuine on
 * the webhook surface — it carries per-extruder `lane_loaded`, `lanes` and
 * `tool_stn` distances a bare name array cannot — and because it stays
 * authoritative whenever it IS present. But nothing in this file is evidence
 * that a real printer reaches the code it covers.
 *
 * The counterpart is what matters: **test_afc_toolchanger_from_status.cpp**
 * drives the same behaviours from the flat top-level `AFC.extruders` array the
 * firmware actually sends. That file is where toolchanger coverage lives; when
 * `AFC_SELECT_TOOL` was dead on hardware (fixed in 6bcfb067b), every test in
 * THIS file was green. See prestonbrown/helixscreen#1200 and #1154 for the
 * fixture-fidelity class of bug.
 *
 * Test tags: [ams][afc][multi_extruder], plus [webhook] on the AFC.system tests
 * and [status] on the ones fed a real status frame.
 */

#include "ams_backend_afc.h"
#include "ams_types.h"

#include <algorithm>
#include <any>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
// ============================================================================
// Test helper for multi-extruder AFC parsing
// ============================================================================

/**
 * @brief Test helper exposing AFC internals for multi-extruder testing
 *
 * Extends AmsBackendAfc to provide access to extruder state and
 * the ability to feed mock status updates.
 */
class AmsBackendAfcMultiExtruderHelper : public AmsBackendAfc {
  public:
    AmsBackendAfcMultiExtruderHelper() : AmsBackendAfc(nullptr, nullptr) {}

    // Feed a Moonraker notify_status_update notification through the backend
    void feed_status_update(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    // Feed AFC global state update
    void feed_afc_state(const nlohmann::json& afc_data) {
        nlohmann::json params;
        params["AFC"] = afc_data;
        feed_status_update(params);
    }

    // Feed AFC_extruder update
    void feed_afc_extruder(const std::string& ext_name, const nlohmann::json& data) {
        nlohmann::json params;
        params["AFC_extruder " + ext_name] = data;
        feed_status_update(params);
    }

    // Initialize lanes and slots for testing
    void initialize_test_lanes_with_slots(int count) {
        system_info_.units.clear();
        std::vector<std::string> names;

        AmsUnit unit;
        unit.unit_index = 0;
        unit.name = "Box Turtle 1";
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
            slot.color_rgb = AMS_DEFAULT_SLOT_COLOR;
            unit.slots.push_back(slot);
        }

        system_info_.units.push_back(unit);
        system_info_.total_slots = count;

        // Initialize tool-to-slot mapping
        system_info_.tool_to_slot_map.clear();
        for (int i = 0; i < count; ++i) {
            system_info_.tool_to_slot_map.push_back(i);
        }

        slots_.initialize("Box Turtle 1", names);
    }

    // Set discovered lanes (delegates to base)
    void setup_discovered_lanes(const std::vector<std::string>& lanes,
                                const std::vector<std::string>& hubs) {
        set_discovered_lanes(lanes, hubs);
    }

    // Accessors for extruder state
    int get_num_extruders() const {
        return num_extruders_;
    }

    const std::vector<AfcExtruderInfo>& get_extruders() const {
        return extruders_;
    }

    // Access system_info for assertions
    const AmsSystemInfo& get_system_info_ref() const {
        return system_info_;
    }

    // Override execute_gcode to capture commands
    std::vector<std::string> captured_gcodes;

    AmsError execute_gcode(const std::string& gcode) override {
        captured_gcodes.push_back(gcode);
        return AmsErrorHelper::success();
    }

    bool has_gcode(const std::string& expected) const {
        return std::find(captured_gcodes.begin(), captured_gcodes.end(), expected) !=
               captured_gcodes.end();
    }

    bool has_gcode_starting_with(const std::string& prefix) const {
        for (const auto& gcode : captured_gcodes) {
            if (gcode.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }
};

// ============================================================================
// AfcExtruderInfo struct tests
// ============================================================================

TEST_CASE("AfcExtruderInfo default construction", "[ams][afc][multi_extruder]") {
    AfcExtruderInfo info{};

    CHECK(info.name.empty());
    CHECK(info.lane_loaded.empty());
    CHECK(info.available_lanes.empty());
}

TEST_CASE("AfcExtruderInfo construction with values", "[ams][afc][multi_extruder]") {
    AfcExtruderInfo info;
    info.name = "extruder";
    info.lane_loaded = "lane1";
    info.available_lanes = {"lane1", "lane2"};

    CHECK(info.name == "extruder");
    CHECK(info.lane_loaded == "lane1");
    CHECK(info.available_lanes.size() == 2);
    CHECK(info.available_lanes[0] == "lane1");
    CHECK(info.available_lanes[1] == "lane2");
}

// ============================================================================
// Single extruder (standard AFC, no toolchanger)
// ============================================================================

TEST_CASE("AFC single extruder: num_extruders defaults to 1", "[ams][afc][multi_extruder]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // No extruder data fed — default state
    CHECK(helper.get_num_extruders() == 1);
}

TEST_CASE("AFC.system webhook: explicit num_extruders=1 populates one extruder",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // AFC reports a single extruder explicitly (webhook shape — never on the wire)
    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 1},
          {"extruders",
           {{"extruder",
             {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    CHECK(helper.get_num_extruders() == 1);
    REQUIRE(helper.get_extruders().size() == 1);
    CHECK(helper.get_extruders()[0].name == "extruder");
    CHECK(helper.get_extruders()[0].lane_loaded == "lane1");
    CHECK(helper.get_extruders()[0].available_lanes.size() == 4);
}

TEST_CASE("AFC.system webhook: single extruder carries its whole lane list",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 1},
          {"extruders",
           {{"extruder",
             {{"lane_loaded", "lane2"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    REQUIRE(helper.get_extruders().size() == 1);
    const auto& ext = helper.get_extruders()[0];
    CHECK(ext.lane_loaded == "lane2");
    CHECK(ext.available_lanes == std::vector<std::string>{"lane1", "lane2", "lane3", "lane4"});
}

// ============================================================================
// Multi-extruder (toolchanger with AFC)
// ============================================================================

TEST_CASE("AFC.system webhook: num_extruders=2 with two extruder entries",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", ""}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    CHECK(helper.get_num_extruders() == 2);
    REQUIRE(helper.get_extruders().size() == 2);
}

TEST_CASE("AFC.system webhook: extruder entries have correct names and lanes",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", ""}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    // The webhook shape is a JSON OBJECT, so it has no inherent order and the
    // parse sorts the keys to make tool indices deterministic. The status shape
    // is an array in tool order and must NOT be sorted — that half is pinned by
    // "tool number indexes extruders_ positionally" in
    // test_afc_toolchanger_from_status.cpp.
    const auto& extruders = helper.get_extruders();

    // "extruder" sorts before "extruder1"
    CHECK(extruders[0].name == "extruder");
    CHECK(extruders[0].lane_loaded == "lane1");
    CHECK(extruders[0].available_lanes == std::vector<std::string>{"lane1", "lane2"});

    CHECK(extruders[1].name == "extruder1");
    CHECK(extruders[1].lane_loaded.empty());
    CHECK(extruders[1].available_lanes == std::vector<std::string>{"lane3", "lane4"});
}

TEST_CASE("AFC.system webhook: lane_loaded tracks which lane feeds each extruder",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Initially, extruder has lane1 loaded, extruder1 has lane4 loaded
    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", "lane4"}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    const auto& extruders = helper.get_extruders();
    CHECK(extruders[0].lane_loaded == "lane1");
    CHECK(extruders[1].lane_loaded == "lane4");
}

TEST_CASE("AFC.system webhook: lane_loaded can be empty (no filament loaded)",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", ""}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", ""}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    const auto& extruders = helper.get_extruders();
    CHECK(extruders[0].lane_loaded.empty());
    CHECK(extruders[1].lane_loaded.empty());
}

// ============================================================================
// Lane-to-extruder mapping
// ============================================================================

TEST_CASE("AFC.system webhook: each extruder tracks its available lanes",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(8);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder",
             {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2", "lane3", "lane4"}}}},
            {"extruder1",
             {{"lane_loaded", "lane5"}, {"lanes", {"lane5", "lane6", "lane7", "lane8"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    const auto& extruders = helper.get_extruders();
    REQUIRE(extruders.size() == 2);
    CHECK(extruders[0].available_lanes.size() == 4);
    CHECK(extruders[1].available_lanes.size() == 4);
    CHECK(extruders[0].available_lanes[0] == "lane1");
    CHECK(extruders[1].available_lanes[0] == "lane5");
}

// ============================================================================
// Per-extruder bowden length device action
// ============================================================================

TEST_CASE("AFC single extruder: single bowden_length action", "[ams][afc][multi_extruder]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // A real single-extruder machine (Box Turtle) names its hub, and the
    // generic bowden slider is HUB-keyed — it is only advertised where one is.
    helper.feed_afc_state(nlohmann::json{{"hubs", nlohmann::json::array({"Turtle_1"})}});

    // Single extruder — should get the standard single bowden_length action
    auto actions = helper.get_device_actions();

    // Count calibration-section bowden actions (not hub_bowden_length from config)
    int bowden_count = 0;
    for (const auto& action : actions) {
        if (action.id.find("bowden") != std::string::npos && action.section == "setup") {
            bowden_count++;
        }
    }
    CHECK(bowden_count == 1);
}

// The status-shaped twin of the next two cases lives in
// test_afc_toolchanger_from_status.cpp ("Per-extruder device actions
// materialise from the status payload alone"). Do not let this be the only
// coverage of the split — it proves the split works given a populated
// extruders_, not that a real printer ever populates it.
TEST_CASE("AFC.system webhook: per-extruder bowden_length actions",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // Set up 2 extruders
    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", ""}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    auto actions = helper.get_device_actions();

    // Should have per-extruder bowden actions instead of single one
    bool has_bowden_t0 = false;
    bool has_bowden_t1 = false;
    bool has_generic_bowden = false;

    for (const auto& action : actions) {
        if (action.id == "bowden_T0")
            has_bowden_t0 = true;
        if (action.id == "bowden_T1")
            has_bowden_t1 = true;
        if (action.id == "bowden_length")
            has_generic_bowden = true;
    }

    CHECK(has_bowden_t0);
    CHECK(has_bowden_t1);
    // Generic bowden should be replaced by per-extruder bowdens
    CHECK_FALSE(has_generic_bowden);
}

TEST_CASE("AFC.system webhook: bowden actions have correct labels",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", ""}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", ""}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    auto actions = helper.get_device_actions();

    for (const auto& action : actions) {
        if (action.id == "bowden_T0") {
            CHECK(action.label.find("T0") != std::string::npos);
            CHECK(action.section == "setup");
            CHECK(action.type == helix::printer::ActionType::SLIDER);
            CHECK(action.unit == "mm");
        }
        if (action.id == "bowden_T1") {
            CHECK(action.label.find("T1") != std::string::npos);
            CHECK(action.section == "setup");
            CHECK(action.type == helix::printer::ActionType::SLIDER);
            CHECK(action.unit == "mm");
        }
    }
}

// ============================================================================
// State update: extruder data updates on subsequent AFC state messages
// ============================================================================

TEST_CASE("AFC.system webhook: state updates replace extruder data",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // First update: lane1 loaded in extruder
    nlohmann::json afc_data1 = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", "lane1"}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", ""}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data1);

    CHECK(helper.get_extruders()[0].lane_loaded == "lane1");
    CHECK(helper.get_extruders()[1].lane_loaded.empty());

    // Second update: lane loaded changes
    nlohmann::json afc_data2 = {
        {"system",
         {{"num_extruders", 2},
          {"extruders",
           {{"extruder", {{"lane_loaded", "lane2"}, {"lanes", {"lane1", "lane2"}}}},
            {"extruder1", {{"lane_loaded", "lane3"}, {"lanes", {"lane3", "lane4"}}}}}}}}};
    helper.feed_afc_state(afc_data2);

    CHECK(helper.get_extruders()[0].lane_loaded == "lane2");
    CHECK(helper.get_extruders()[1].lane_loaded == "lane3");
}

// ============================================================================
// Edge cases
// ============================================================================

// Status shape: a real frame with neither `system` nor `extruders`. This is what
// a delta update looks like, and it must not disturb what discovery already
// found.
TEST_CASE("AFC status frame: neither system nor extruders leaves the defaults alone",
          "[ams][afc][multi_extruder][status]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {{"current_state", "Idle"}};
    helper.feed_afc_state(afc_data);

    // Should keep defaults
    CHECK(helper.get_num_extruders() == 1);
    CHECK(helper.get_extruders().empty());
}

TEST_CASE("AFC.system webhook: system with no extruders key is no-op",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // system exists but has no extruders
    nlohmann::json afc_data = {{"system", {{"num_extruders", 2}}}};
    helper.feed_afc_state(afc_data);

    // num_extruders should be updated but extruders_ stays empty
    CHECK(helper.get_num_extruders() == 2);
    CHECK(helper.get_extruders().empty());
}

TEST_CASE("AFC.system webhook: extruder with missing lanes array",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    // extruder entry missing "lanes" key
    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 1}, {"extruders", {{"extruder", {{"lane_loaded", "lane1"}}}}}}}};
    helper.feed_afc_state(afc_data);

    REQUIRE(helper.get_extruders().size() == 1);
    CHECK(helper.get_extruders()[0].name == "extruder");
    CHECK(helper.get_extruders()[0].lane_loaded == "lane1");
    CHECK(helper.get_extruders()[0].available_lanes.empty());
}

TEST_CASE("AFC.system webhook: extruder with null lane_loaded",
          "[ams][afc][multi_extruder][webhook]") {
    AmsBackendAfcMultiExtruderHelper helper;
    helper.initialize_test_lanes_with_slots(4);

    nlohmann::json afc_data = {
        {"system",
         {{"num_extruders", 1},
          {"extruders",
           {{"extruder", {{"lane_loaded", nullptr}, {"lanes", {"lane1", "lane2"}}}}}}}}};
    helper.feed_afc_state(afc_data);

    REQUIRE(helper.get_extruders().size() == 1);
    // null lane_loaded should result in empty string
    CHECK(helper.get_extruders()[0].lane_loaded.empty());
}
