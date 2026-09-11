// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_environment_zone.h"
#include "ams_types.h"
#include "test_helpers/happy_hare_test_access.h"

#include <map>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;

namespace {

/// AFC has no per-gate heater lists, so it exercises the generic default.
class ZoneSourceHelper : public AmsBackendAfc {
  public:
    ZoneSourceHelper() : AmsBackendAfc(nullptr, nullptr) {}

    AmsSystemInfo& mutable_info() {
        return system_info_;
    }

    void set_dryer(int unit, DryerInfo d) {
        dryers_[unit] = d;
    }

    DryerInfo get_dryer_info(int unit = 0) const override {
        auto it = dryers_.find(unit);
        return it == dryers_.end() ? DryerInfo{} : it->second;
    }

    AmsError execute_gcode(const std::string& /*gcode*/) override {
        return AmsErrorHelper::success();
    }

  private:
    std::map<int, DryerInfo> dryers_;
};

/// A unit with @p slot_count lanes starting at @p first_global.
AmsUnit make_unit(int index, const std::string& name, int slot_count, int first_global) {
    AmsUnit u;
    u.unit_index = index;
    u.name = name;
    u.slot_count = slot_count;
    u.first_slot_global_index = first_global;
    for (int i = 0; i < slot_count; ++i) {
        SlotInfo s;
        s.slot_index = i;
        u.slots.push_back(s);
    }
    return u;
}

} // namespace

TEST_CASE("A unit-level sensor makes the whole unit one zone", "[ams][zones][source]") {
    ZoneSourceHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "AFC";
    info.units = {make_unit(0, "Box_Turtle_1", 4, 0)};
    info.units[0].environment =
        EnvironmentData{.temperature_c = 24.5f, .humidity_pct = 42.0f, .has_humidity = true};

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 1);
    CHECK(zones[0].gates == std::vector<int>{0, 1, 2, 3});
    CHECK(zones[0].unit_index == 0);
    CHECK(zones[0].env.humidity_pct == Catch::Approx(42.0f));
    CHECK_FALSE(zones[0].dryer.supported);
}

TEST_CASE("Per-lane sensors make one zone per lane", "[ams][zones][source]") {
    ZoneSourceHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "AFC";
    // Second unit so the global gate offset is actually exercised.
    info.units = {make_unit(0, "Box_Turtle_1", 2, 0), make_unit(1, "EMU", 2, 2)};
    for (auto& s : info.units[1].slots) {
        s.environment =
            EnvironmentData{.temperature_c = 23.0f, .humidity_pct = 31.0f, .has_humidity = true};
    }

    const auto zones = backend.get_environment_zones(1);

    REQUIRE(zones.size() == 2);
    // slot_index is unit-relative; the zone's gates are global.
    CHECK(zones[0].gates == std::vector<int>{2});
    CHECK(zones[1].gates == std::vector<int>{3});
    CHECK(zones[0].unit_index == 1);
}

TEST_CASE("A unit with neither sensor nor dryer yields no zone", "[ams][zones][source]") {
    ZoneSourceHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "AFC";
    info.units = {make_unit(0, "Box_Turtle_1", 4, 0)};

    const auto zones = backend.get_environment_zones();

    // Proves the walk ran rather than that a default-constructed vector came back:
    // the same call with a sensor added returns one zone.
    CHECK(zones.empty());
    info.units[0].environment = EnvironmentData{};
    CHECK(backend.get_environment_zones().size() == 1);
}

TEST_CASE("A dryer with no sensor still makes a zone", "[ams][zones][source]") {
    ZoneSourceHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "Happy Hare";
    info.units = {make_unit(0, "MMU", 4, 0)};
    DryerInfo d;
    d.supported = true;
    d.active = true;
    backend.set_dryer(0, d);

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 1);
    CHECK(zones[0].dryer.supported);
    CHECK(zones[0].state == ZoneDryingState::Active);
}

TEST_CASE("Scoping to a unit drops the others", "[ams][zones][source]") {
    ZoneSourceHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "AFC";
    info.units = {make_unit(0, "A", 2, 0), make_unit(1, "B", 2, 2)};
    info.units[0].environment = EnvironmentData{};
    info.units[1].environment = EnvironmentData{};

    CHECK(backend.get_environment_zones(-1).size() == 2);
    const auto scoped = backend.get_environment_zones(1);
    REQUIRE(scoped.size() == 1);
    CHECK(scoped[0].unit_index == 1);
    // Pins the unit-level branch's own global-offset conversion, distinct from the
    // per-slot branch's: unit 1 starts at global gate 2.
    CHECK(scoped[0].gates == std::vector<int>{2, 3});
}

TEST_CASE("A mixed unit skips slots with no sensor of their own", "[ams][zones][source]") {
    ZoneSourceHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "AFC";
    info.units = {make_unit(0, "Box_Turtle_1", 3, 0)};
    // Only the first and last slot carry a reading; the middle one stays bare.
    info.units[0].slots[0].environment =
        EnvironmentData{.temperature_c = 22.0f, .humidity_pct = 35.0f, .has_humidity = true};
    info.units[0].slots[2].environment =
        EnvironmentData{.temperature_c = 26.0f, .humidity_pct = 40.0f, .has_humidity = true};

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].gates == std::vector<int>{0});
    CHECK(zones[1].gates == std::vector<int>{2});
}

namespace helix {

/// Exposes the per-gate config lists the override consumes.
class HHZoneHelper : public AmsBackendHappyHare {
  public:
    HHZoneHelper() : AmsBackendHappyHare(nullptr, nullptr) {}

    AmsSystemInfo& mutable_info() {
        return system_info_;
    }
    void set_per_gate(std::vector<std::string> heaters, std::vector<std::string> sensors) {
        HappyHareTestAccess::filament_heaters(*this) = std::move(heaters);
        HappyHareTestAccess::environment_sensors(*this) = std::move(sensors);
    }
    void set_gate_states(std::vector<std::string> states) {
        HappyHareTestAccess::gate_drying_states(*this) = std::move(states);
    }
    void set_dryer(DryerInfo d) {
        HappyHareTestAccess::dryer_info(*this) = d;
    }
    void set_heater_temp(const std::string& heater, float temp_c) {
        HappyHareTestAccess::heater_temp(*this)[heater] = temp_c;
    }

    AmsError execute_gcode(const std::string& /*gcode*/) override {
        return AmsErrorHelper::success();
    }
};

} // namespace helix

TEST_CASE("The mixed rig collapses to three zones over two units", "[ams][zones][source]") {
    HHZoneHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "Happy Hare";
    info.units = {make_unit(0, "QuattroBox", 4, 0), make_unit(1, "EMU", 2, 4)};

    // One QuattroBox heater repeated across its four gates, then two bare EMU lanes.
    backend.set_per_gate({"heater_generic MMU_heater", "heater_generic MMU_heater",
                          "heater_generic MMU_heater", "heater_generic MMU_heater", "", ""},
                         {"temperature_sensor MMU_Chamber", "temperature_sensor MMU_Chamber",
                          "temperature_sensor MMU_Chamber", "temperature_sensor MMU_Chamber",
                          "temperature_sensor Lane_0", "temperature_sensor Lane_1"});

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 3);
    CHECK(zones[0].gates == std::vector<int>{0, 1, 2, 3});
    CHECK(zones[0].dryer.supported);
    CHECK(zones[0].unit_index == 0);
    CHECK(zones[1].gates == std::vector<int>{4});
    CHECK_FALSE(zones[1].dryer.supported);
    CHECK(zones[2].gates == std::vector<int>{5});
    // A set that splits on capability is not tab-able.
    CHECK(select_zone_presentation(zones) == ZonePresentation::List);
}

TEST_CASE("A capped Happy Hare rig reports a queued zone", "[ams][zones][source]") {
    HHZoneHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "Happy Hare";
    info.units = {make_unit(0, "MMU", 2, 0)};
    backend.set_per_gate({"heater_generic Box_A", "heater_generic Box_B"},
                         {"temperature_sensor Chamber_A", "temperature_sensor Chamber_B"});
    backend.set_gate_states({"active", "queued"});
    backend.set_heater_temp("heater_generic Box_A", 45.0f);
    backend.set_heater_temp("heater_generic Box_B", 30.0f);

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].state == ZoneDryingState::Active);
    CHECK(zones[1].state == ZoneDryingState::Queued);
    // Each box reports its own heater, not the global dryer's single reading.
    CHECK(zones[0].dryer.current_temp_c == Catch::Approx(45.0f));
    CHECK(zones[1].dryer.current_temp_c == Catch::Approx(30.0f));
    CHECK(zones[0].dryer.active);
    CHECK_FALSE(zones[1].dryer.active);
}

TEST_CASE("A scalar Happy Hare config still falls through to the generic default",
          "[ams][zones][source]") {
    HHZoneHelper backend;
    auto& info = backend.mutable_info();
    info.type_name = "Happy Hare";
    info.units = {make_unit(0, "MMU", 4, 0)};
    // Shared-enclosure form: no per-gate lists, so the override must defer to
    // AmsBackend::get_environment_zones() rather than collapse an empty pair of lists.
    info.units[0].environment =
        EnvironmentData{.temperature_c = 24.0f, .humidity_pct = 40.0f, .has_humidity = true};
    DryerInfo d;
    d.supported = true;
    backend.set_dryer(d);

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 1);
    CHECK(zones[0].gates == std::vector<int>{0, 1, 2, 3});
    CHECK(zones[0].dryer.supported);
}

TEST_CASE("The capped mock rig queues its second heated zone", "[ams][zones][source]") {
    AmsBackendMock backend;
    backend.set_multi_unit_mode(true);
    backend.set_environment_mode("capped");

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 2);
    CHECK(zones[0].dryer.supported);
    CHECK(zones[1].dryer.supported);
    CHECK(zones[0].state == ZoneDryingState::Active);
    CHECK(zones[1].state == ZoneDryingState::Queued);
    // The Active zone is the one actually running the rig's cycle, not just a label.
    CHECK(zones[0].dryer.active);
    CHECK_FALSE(zones[1].dryer.active);
    CHECK(zones[0].dryer.duration_min == 240);
    CHECK(zones[0].dryer.remaining_min == 184);
    // Two interchangeable heated zones are tab-able.
    CHECK(select_zone_presentation(zones) == ZonePresentation::Tabs);
}

TEST_CASE("The mixed mock rig splits on capability", "[ams][zones][source]") {
    AmsBackendMock backend;
    backend.set_multi_unit_mode(true);
    backend.set_environment_mode("mixed");

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 3);
    CHECK(zones[0].dryer.supported);
    CHECK_FALSE(zones[1].dryer.supported);
    CHECK_FALSE(zones[2].dryer.supported);
    CHECK(zones[0].unit_index == 0);
    CHECK(zones[1].unit_index == 1);
    CHECK(select_zone_presentation(zones) == ZonePresentation::List);
}

TEST_CASE("The emu mock rig is passive per lane", "[ams][zones][source]") {
    AmsBackendMock backend;
    backend.set_multi_unit_mode(true);
    backend.set_environment_mode("emu");

    const auto zones = backend.get_environment_zones();

    REQUIRE(zones.size() == 6);
    for (const auto& z : zones) {
        CHECK_FALSE(z.dryer.supported);
        CHECK(z.gates.size() == 1);
    }
    CHECK(select_zone_presentation(zones) == ZonePresentation::List);
}
