// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_environment_zone.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

namespace {

/// Shared enclosure: Happy Hare's scalar form reaches the collapse as one name
/// repeated per gate, so both config shapes take the same path.
std::vector<std::string> repeated(const std::string& name, int count) {
    return std::vector<std::string>(static_cast<size_t>(count), name);
}

} // namespace

TEST_CASE("Gates sharing a heater and sensor collapse into one zone", "[ams][dryer][zones]") {
    // QuattroBox: one enclosure heater and one chamber BME over four gates.
    const auto zones = derive_environment_zones(repeated("heater_generic MMU_heater", 4),
                                                repeated("temperature_sensor MMU_Chamber", 4), 4);

    REQUIRE(zones.size() == 1);
    CHECK(zones[0].gates == std::vector<int>{0, 1, 2, 3});
    CHECK(zones[0].heater_name == "heater_generic MMU_heater");
    CHECK(zones[0].sensor_name == "temperature_sensor MMU_Chamber");
    CHECK(zones[0].dryer.supported);
}

TEST_CASE("Two QuattroBoxes are two heated zones", "[ams][dryer][zones]") {
    std::vector<std::string> heaters = repeated("heater_generic MMU_heater_1", 4);
    std::vector<std::string> sensors = repeated("temperature_sensor MMU_Chamber_1", 4);
    for (const auto& h : repeated("heater_generic MMU_heater_2", 4)) {
        heaters.push_back(h);
    }
    for (const auto& s : repeated("temperature_sensor MMU_Chamber_2", 4)) {
        sensors.push_back(s);
    }

    const auto zones = derive_environment_zones(heaters, sensors, 8);

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].gates == std::vector<int>{0, 1, 2, 3});
    CHECK(zones[1].gates == std::vector<int>{4, 5, 6, 7});
    CHECK(zones[0].dryer.supported);
    CHECK(zones[1].dryer.supported);
    // Distinct Klipper object names must not collapse across boxes.
    CHECK(zones[0].id != zones[1].id);
}

TEST_CASE("A passive lane reports environment without a dryer", "[ams][dryer][zones][emu]") {
    // Stock EMU: a sealed box and a BME per lane, no heaters anywhere.
    const auto zones = derive_environment_zones(
        {}, {"temperature_sensor Lane_0", "temperature_sensor Lane_1", "temperature_sensor Lane_2"},
        3);

    REQUIRE(zones.size() == 3);
    for (int i = 0; i < 3; ++i) {
        const auto& z = zones[static_cast<size_t>(i)];
        CHECK(z.gates == std::vector<int>{i});
        CHECK(z.sensor_name == "temperature_sensor Lane_" + std::to_string(i));
        CHECK(z.heater_name.empty());
        // The zone exists to be watched, not driven.
        CHECK_FALSE(z.dryer.supported);
    }
}

TEST_CASE("A mixed rig splits into heated and passive zones", "[ams][dryer][zones][mixed]") {
    // One QuattroBox (gates 0-3, shared heater + chamber sensor) beside a
    // two-lane EMU (gates 4-5, own sensor each, no heater).
    std::vector<std::string> heaters = repeated("heater_generic MMU_heater", 4);
    heaters.emplace_back(""); // gate 4, EMU
    heaters.emplace_back(""); // gate 5, EMU
    std::vector<std::string> sensors = repeated("temperature_sensor MMU_Chamber", 4);
    sensors.emplace_back("temperature_sensor Lane_0");
    sensors.emplace_back("temperature_sensor Lane_1");

    const auto zones = derive_environment_zones(heaters, sensors, 6);

    REQUIRE(zones.size() == 3);
    CHECK(zones[0].gates == std::vector<int>{0, 1, 2, 3});
    CHECK(zones[0].dryer.supported);
    CHECK(zones[1].gates == std::vector<int>{4});
    CHECK_FALSE(zones[1].dryer.supported);
    CHECK(zones[2].gates == std::vector<int>{5});
    CHECK_FALSE(zones[2].dryer.supported);
}

TEST_CASE("Zones come back ordered by their lowest gate", "[ams][dryer][zones]") {
    // Interleaved gates: the second box's lanes are not contiguous.
    const auto zones =
        derive_environment_zones({"h_b", "h_a", "h_b", "h_a"}, {"s_b", "s_a", "s_b", "s_a"}, 4);

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].gates == std::vector<int>{0, 2});
    CHECK(zones[1].gates == std::vector<int>{1, 3});
}

TEST_CASE("A gate with neither heater nor sensor forms no zone", "[ams][dryer][zones]") {
    // Gate 1 is a plain lane on a rig where only gate 0 sits in a box.
    const auto zones = derive_environment_zones({"heater_generic MMU_heater", ""},
                                                {"temperature_sensor MMU_Chamber", ""}, 2);

    REQUIRE(zones.size() == 1);
    CHECK(zones[0].gates == std::vector<int>{0});
}

TEST_CASE("Lists shorter than the gate count do not run off the end",
          "[ams][dryer][zones][bounds]") {
    // Happy Hare only requires one entry per configured lane; a rig that gained
    // a gate without gaining a sensor must not read past the list.
    const auto zones = derive_environment_zones({}, {"temperature_sensor Lane_0"}, 4);

    REQUIRE(zones.size() == 1);
    CHECK(zones[0].gates == std::vector<int>{0});
}

TEST_CASE("No environment hardware yields no zones", "[ams][dryer][zones]") {
    // A plain Box Turtle: four gates, nothing to monitor or drive.
    CHECK(derive_environment_zones({}, {}, 4).empty());
}

TEST_CASE("A zero gate count yields no zones", "[ams][dryer][zones][bounds]") {
    CHECK(derive_environment_zones({"h"}, {"s"}, 0).empty());
}

// ============================================================================
// Zone drying state
// ============================================================================

TEST_CASE("A zone with no gates in the cycle is idle", "[ams][dryer][zones][state]") {
    CHECK(fold_zone_drying_state({0, 1}, {"", "", "", ""}) == ZoneDryingState::Idle);
}

TEST_CASE("An active gate makes its zone active", "[ams][dryer][zones][state]") {
    CHECK(fold_zone_drying_state({2, 3}, {"", "", "active", ""}) == ZoneDryingState::Active);
}

TEST_CASE("A queued zone is queued, not active and not idle", "[ams][dryer][zones][state]") {
    // A backend capping simultaneous heaters leaves later zones waiting. Reporting
    // that as Active lies; reporting it as Idle makes the Start press look ignored.
    CHECK(fold_zone_drying_state({1}, {"active", "queued"}) == ZoneDryingState::Queued);
}

TEST_CASE("Another zone's activity does not leak in", "[ams][dryer][zones][state]") {
    CHECK(fold_zone_drying_state({2, 3}, {"active", "queued", "", ""}) == ZoneDryingState::Idle);
}

TEST_CASE("Active outranks queued within one zone", "[ams][dryer][zones][state]") {
    CHECK(fold_zone_drying_state({0, 1}, {"queued", "active"}) == ZoneDryingState::Active);
}

TEST_CASE("A finished zone is complete", "[ams][dryer][zones][state]") {
    CHECK(fold_zone_drying_state({0, 1}, {"complete", "complete"}) == ZoneDryingState::Complete);
}

TEST_CASE("Happy Hare spells cancelled with one L; both spellings are accepted",
          "[ams][dryer][zones][state]") {
    // The emitted constant is 'canceled'; Happy Hare's own header documents 'cancelled'.
    // Accepting both keeps an upstream correction from reading as Idle.
    CHECK(fold_zone_drying_state({0}, {"canceled"}) == ZoneDryingState::Cancelled);
    CHECK(fold_zone_drying_state({0}, {"cancelled"}) == ZoneDryingState::Cancelled);
}

TEST_CASE("A partly cancelled zone does not report as cleanly finished",
          "[ams][dryer][zones][state]") {
    CHECK(fold_zone_drying_state({0, 1}, {"complete", "canceled"}) == ZoneDryingState::Cancelled);
}

TEST_CASE("Gates past the end of the state list are idle", "[ams][dryer][zones][state][bounds]") {
    CHECK(fold_zone_drying_state({5, 6}, {"active"}) == ZoneDryingState::Idle);
    CHECK(fold_zone_drying_state({}, {"active"}) == ZoneDryingState::Idle);
}

TEST_CASE("An unrecognised state string is idle, not a crash",
          "[ams][dryer][zones][state][bounds]") {
    CHECK(fold_zone_drying_state({0}, {"drying-ish"}) == ZoneDryingState::Idle);
}

TEST_CASE("Nothing is queued behind a cycle that is not running", "[ams][dryer][zones][state]") {
    EnvironmentZone idle_a;
    idle_a.dryer.supported = true;
    idle_a.state = ZoneDryingState::Idle;
    EnvironmentZone idle_b;
    idle_b.dryer.supported = true;
    idle_b.state = ZoneDryingState::Idle;
    std::vector<EnvironmentZone> zones = {idle_a, idle_b};

    queue_zones_waiting_for_the_cap(zones);

    // Two heated zones with nothing running stay Idle; neither is waiting on a cycle.
    CHECK(zones[0].state == ZoneDryingState::Idle);
    CHECK(zones[1].state == ZoneDryingState::Idle);
}

// ============================================================================
// Zone selector strategy
// ============================================================================

namespace {

std::vector<EnvironmentZone> zones_with(std::initializer_list<bool> dryer_supported) {
    std::vector<EnvironmentZone> zs;
    int i = 0;
    for (bool sup : dryer_supported) {
        EnvironmentZone z;
        z.id = "z" + std::to_string(i);
        z.gates = {i++};
        z.dryer.supported = sup;
        zs.push_back(std::move(z));
    }
    return zs;
}

} // namespace

TEST_CASE("No zones and one zone both skip the selector", "[ams][dryer][zones][selector]") {
    CHECK(select_zone_presentation({}) == ZonePresentation::Single);
    CHECK(select_zone_presentation(zones_with({true})) == ZonePresentation::Single);
    CHECK(select_zone_presentation(zones_with({false})) == ZonePresentation::Single);
}

TEST_CASE("A few interchangeable zones get tabs", "[ams][dryer][zones][selector]") {
    CHECK(select_zone_presentation(zones_with({true, true})) == ZonePresentation::Tabs);
    CHECK(select_zone_presentation(zones_with({true, true, true, true})) == ZonePresentation::Tabs);
    // All-passive is still uniform: nothing to drive on any of them.
    CHECK(select_zone_presentation(zones_with({false, false, false})) == ZonePresentation::Tabs);
}

TEST_CASE("More zones than fit a tab strip get the list", "[ams][dryer][zones][selector]") {
    CHECK(select_zone_presentation(zones_with({true, true, true, true, true})) ==
          ZonePresentation::List);
}

TEST_CASE("Zones that differ in capability get the list even when few",
          "[ams][dryer][zones][selector]") {
    // A heated box beside a passive one: a tab would hide that one of them has no
    // controls at all until after the user picks it.
    CHECK(select_zone_presentation(zones_with({true, false})) == ZonePresentation::List);
    CHECK(select_zone_presentation(zones_with({true, false, false})) == ZonePresentation::List);
}

TEST_CASE("An unverified dryer defaults to stop-and-restart", "[ams][dryer][zones][capability]") {
    // Conservative by construction: a backend nobody has checked against hardware must
    // take the visible restart rather than silently discard an adjustment.
    const helix::DryerInfo fresh;
    CHECK_FALSE(fresh.supports_live_temp);
    CHECK_FALSE(fresh.supports_live_duration);
}

TEST_CASE("A zone folds its gates' drying states as it is derived", "[ams][dryer][zones]") {
    // One heated enclosure over gates 0-3, and a separate box on gate 4.
    std::vector<std::string> heaters = repeated("heater_generic Box_A", 4);
    heaters.push_back("heater_generic Box_B");
    std::vector<std::string> sensors = repeated("temperature_sensor Chamber_A", 4);
    sensors.push_back("temperature_sensor Chamber_B");

    // Box A is waiting on Box B's heater: a capped rig leaves it queued.
    const std::vector<std::string> per_gate_state = {"queued", "queued", "queued", "queued",
                                                     "active"};

    const auto zones = derive_environment_zones(heaters, sensors, 5, per_gate_state);

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].state == ZoneDryingState::Queued);
    CHECK(zones[1].state == ZoneDryingState::Active);
}

TEST_CASE("A zone derived without firmware states reads as idle", "[ams][dryer][zones]") {
    const auto heaters = repeated("heater_generic Box_A", 2);
    const auto sensors = repeated("temperature_sensor Chamber_A", 2);

    const auto idle_zones = derive_environment_zones(heaters, sensors, 2);
    REQUIRE(idle_zones.size() == 1);
    // Idle rather than a sentinel: a rig that reports no drying state is not drying.
    CHECK(idle_zones[0].state == ZoneDryingState::Idle);

    // Same gates, states supplied this time: proves derive_environment_zones actually
    // ran the fold rather than the field just keeping its default.
    const auto active_zones = derive_environment_zones(heaters, sensors, 2, {"active", "active"});
    REQUIRE(active_zones.size() == 1);
    CHECK(active_zones[0].state == ZoneDryingState::Active);
}
