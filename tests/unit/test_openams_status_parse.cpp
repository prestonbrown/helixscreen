// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/openams_status_json.h"
#include "openams_status_parse.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace openams_json;

namespace {

openams::ManagerStatus parse(const json& manager) {
    return openams::parse_manager_status(manager);
}

} // namespace

TEST_CASE("OpenAMS status parse flattens units, slots and groups into global slots",
          "[openams][status_parse]") {
    json m = manager(json::array({lane("loaded", "T1", 2)}));
    m["units"].push_back(json{{"id", "2"},
                              {"topology", "linear"},
                              {"lane", "fps"},
                              {"slots", json::array({slot(9, 0, true)})}});
    m["groups"].push_back(json{{"name", "T3"}, {"slots", {9, 42}}});
    const auto s = parse(m);
    REQUIRE(s.units.size() == 2);
    CHECK(s.total_slots == 5);
    CHECK(s.units[1].first_slot_global_index == 4);
    CHECK(s.units[1].topology == PathTopology::LINEAR);
    CHECK(s.topology == PathTopology::MIXED);
    CHECK_FALSE(s.units[0].has_toolhead_sensor);
    CHECK(s.units[0].slots[0].status == SlotStatus::EMPTY);
    CHECK(s.units[0].slots[1].status == SlotStatus::AVAILABLE);
    // The lane's current slot is loaded even when the slot does not say so.
    CHECK(s.units[0].slots[2].status == SlotStatus::LOADED);
    CHECK(s.remote_slot_ids == std::vector<int>{0, 1, 2, 3, 9});
    CHECK(s.unit_oams_idx == std::vector<int>{1, 2});
    CHECK(s.slot_groups == std::vector<std::string>{"T0", "T0", "T1", "T2", "T3"});
    // A member id no unit publishes is dropped.
    CHECK(s.groups[3].slots == std::vector<int>{4});
    CHECK(s.current_slot == 2);
    CHECK(s.current_tool == 1);
    CHECK(s.filament_loaded);
}

TEST_CASE("OpenAMS status parse never folds two loaded lanes into one current slot",
          "[openams][status_parse]") {
    json m = manager(json::array({lane("loaded", "T1", 2), lane("unloading", "T2", 3)}));
    auto s = parse(m);
    CHECK(s.filament_loaded);
    CHECK(s.current_slot == -1);
    CHECK(s.current_tool == -1);
    CHECK(s.reported_action == AmsAction::UNLOADING);

    m["lanes"][0]["state"] = "error";
    CHECK(parse(m).reported_action == AmsAction::ERROR);
}

TEST_CASE("OpenAMS status parse keeps only commands that are one G-code word, and draws an "
          "unknown topology as a hub",
          "[openams][status_parse]") {
    json commands = all_commands();
    commands["load"] = "OPENAMS_LOAD\nG28";
    commands["reset"] = "";
    json m = manager(json::array({lane("unloaded")}), commands);
    m["units"][0]["topology"] = "carousel";
    const auto s = parse(m);
    CHECK(s.commands.count("load") == 0);
    CHECK(s.commands.count("reset") == 0);
    CHECK(s.commands.at("unload") == "OPENAMS_UNLOAD");
    CHECK(s.unsafe_commands.size() == 1);
    CHECK(s.units[0].topology == PathTopology::HUB);
}

TEST_CASE("OpenAMS status parse puts each lane's FPS pressure on its units",
          "[openams][status_parse]") {
    const auto s = parse(shared_manager(2));
    REQUIRE(s.units[1].buffer_health.has_value());
    const auto& fps = *s.units[1].buffer_health;
    CHECK((fps.fps_reported && fps.compression_only && fps.filament_loaded));
    CHECK(fps.fps_value == Catch::Approx(0.79f));
    CHECK(fps.fps_set_point == Catch::Approx(0.5f));
    CHECK_FALSE(fps.fault_detection_enabled);

    CHECK_FALSE(parse(shared_manager(-1)).units[0].buffer_health->filament_loaded);
    CHECK_FALSE(parse(manager()).units[0].buffer_health.has_value());
}

TEST_CASE("OpenAMS status parse keeps a unit's stop and pause faults", "[openams][status_parse]") {
    const json info = fault_entry("c", "t", json::array(), "info");
    const auto s = parse(faulted_manager(
        json::array({fault_entry("c", "t", {"clear_fault"}), info}),
        json::array({fault_entry("c", "t", json::array({"clear_errors"}), "pause", 2)})));
    REQUIRE(s.faults[0].size() == 1);
    CHECK((s.faults[0][0].clearable && s.faults[0][0].bay == -1));
    REQUIRE(s.faults[1].size() == 1);
    CHECK((!s.faults[1][0].clearable && s.faults[1][0].bay == 2));
    CHECK(s.reported_action == AmsAction::ERROR);
    CHECK(parse(faulted_manager(json::array({info}))).reported_action == AmsAction::IDLE);
}

TEST_CASE("OpenAMS status parse reads each unit's environment and dryer",
          "[openams][status_parse][dryer]") {
    json ams2 = dryer_device(false);
    ams2["environment"] = {{"temp_c", 30.0}};
    const auto s = parse(dryer_manager(-1, dryer_device(true), ams2));
    REQUIRE(s.units[0].environment.has_value());
    CHECK(s.units[0].environment->temperature_c == Catch::Approx(26.1f));
    CHECK(s.units[0].environment->humidity_pct == Catch::Approx(29.0f));
    CHECK(s.units[0].environment->has_humidity);
    CHECK_FALSE(s.units[1].environment->has_humidity);
    CHECK(s.dryers[0].info.max_duration_min == 10080);
    CHECK_FALSE(s.dryers[0].requires_unloaded);
    CHECK(s.dryers[1].requires_unloaded);
    CHECK_FALSE(parse(manager()).units[0].environment.has_value());

    json no_capability = dryer_device(true);
    no_capability["capabilities"]["dryer"] = false;
    const auto none =
        parse(dryer_manager(-1, no_capability, dryer_device(false, "off", 0, 0, false)));
    CHECK_FALSE(none.dryers[0].offered);
    CHECK_FALSE(none.dryers[1].info.supported);
}

TEST_CASE("OpenAMS status parse maps the unit's dryer state", "[openams][status_parse][dryer]") {
    auto dryer = [](const json& device) { return parse(dryer_manager(-1, device)).dryers[0].info; };
    const auto idle = dryer(dryer_device(true));
    CHECK_FALSE(idle.active);
    CHECK(idle.target_temp_c == Catch::Approx(0.0f));

    const auto heating = dryer(dryer_device(true, "heating", 60.0, 5400));
    CHECK(heating.active);
    CHECK(heating.target_temp_c == Catch::Approx(60.0f));
    CHECK(heating.remaining_min == 90);
    CHECK(heating.current_temp_c == Catch::Approx(51.5f));
    CHECK(heating.fan_pct == 60);
    // A state this build does not know still counts as a running cycle.
    CHECK(dryer(dryer_device(true, "holding", 60.0, 61)).remaining_min == 2);
    CHECK_FALSE(dryer(dryer_device(true, "fault", 60.0, 100)).active);

    json no_probe = dryer_device(true, "heating", 60.0, 60);
    no_probe.erase("telemetry");
    CHECK(dryer(no_probe).current_temp_c == Catch::Approx(26.1f));
}
