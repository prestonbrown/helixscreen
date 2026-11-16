// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_chamber_air_state.cpp
 * @brief Live chamber air readings: what each status frame changes, what a frame
 *        that says nothing keeps, and when the accessory's presence edges fire.
 */

#include "../helix_test_fixture.h"
#include "chamber_air_modes.h"
#include "chamber_air_state.h"
#include "printer_discovery.h"

#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ChamberAirState;
using helix::PrinterDiscovery;
using nlohmann::json;
namespace air = helix::chamber_air;
using air::AirMode;

namespace {

PrinterDiscovery hw_with(std::initializer_list<const char*> names) {
    json objects = json::array();
    for (const char* n : names) {
        objects.push_back(n);
    }
    PrinterDiscovery hw;
    hw.parse_objects(objects);
    return hw;
}

PrinterDiscovery u1() {
    return hw_with({"purifier", "fm175xx_reader", "tool", "extruder"});
}

air::Reading frame(const PrinterDiscovery& hw, json fields) {
    return air::read_status(hw, json{{"purifier", std::move(fields)}});
}

struct Fixture : HelixTestFixture {
    ChamberAirState state;
    std::vector<std::pair<bool, bool>> edges;
    Fixture() {
        state.init_subjects(false);
        state.set_presence_edge_handler(
            [this](bool present, bool at_startup) { edges.emplace_back(present, at_startup); });
    }
    ~Fixture() override {
        state.deinit_subjects();
    }
    int available() {
        return lv_subject_get_int(state.available_subject());
    }
    int mode() {
        return lv_subject_get_int(state.mode_subject());
    }
};

} // namespace

TEST_CASE_METHOD(Fixture, "chamber air state: a fitted accessory publishes its mode",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = u1();
    state.set_hardware(hw);
    REQUIRE(available() == 0);
    state.apply(frame(hw, {{"power_detected", true}, {"mode", 3}}));
    CHECK(available() == 1);
    CHECK(mode() == static_cast<int>(AirMode::Recirculate));
    CHECK(state.mode() == AirMode::Recirculate);
}

TEST_CASE_METHOD(Fixture, "chamber air state: a frame that says nothing keeps every reading",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = u1();
    state.set_hardware(hw);
    state.apply(frame(hw, {{"power_detected", true},
                           {"mode", 1},
                           {"inner_fan_work_time", 3600.0},
                           {"exhaust_fan", {{"speed", 0.6}, {"speed_threshold", 0.4}}},
                           {"inner_fan", {{"speed", 0.0}, {"speed_threshold", 0.3}}}}));
    REQUIRE(available() == 1);
    state.apply(air::read_status(hw, json{{"toolhead", json::object()}}));
    CHECK(available() == 1);
    CHECK(mode() == static_cast<int>(AirMode::Exhaust));
    CHECK(state.filter_seconds() == Catch::Approx(3600.0));
    CHECK(lv_subject_get_int(state.exhaust_pct_subject()) == 60);
    CHECK(state.threshold_pct(air::FanRole::Exhaust) == 40);
    CHECK(state.threshold_pct(air::FanRole::Recirculate) == 30);
}

TEST_CASE_METHOD(Fixture, "chamber air state: removal hides control and fires an edge",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = u1();
    state.set_hardware(hw);
    state.apply(frame(hw, {{"power_detected", true}, {"mode", 1}}));
    state.apply(frame(hw, {{"power_detected", false}, {"mode", 0}}));
    CHECK(available() == 0);
    // No mode is shown while nothing can be controlled.
    CHECK(mode() == -1);
    REQUIRE(edges.size() == 2);
    CHECK(edges[0] == std::make_pair(true, true));
    CHECK(edges[1] == std::make_pair(false, false));
}

TEST_CASE_METHOD(Fixture, "chamber air state: the first reading is a startup edge, not a change",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = u1();
    state.set_hardware(hw);
    state.apply(frame(hw, {{"power_detected", false}}));
    REQUIRE(edges.size() == 1);
    CHECK(edges[0] == std::make_pair(false, true));
    // A repeat of the same reading is no edge.
    state.apply(frame(hw, {{"power_detected", false}}));
    CHECK(edges.size() == 1);
    state.apply(frame(hw, {{"power_detected", true}}));
    REQUIRE(edges.size() == 2);
    CHECK(edges[1] == std::make_pair(true, false));
}

TEST_CASE_METHOD(Fixture, "chamber air state: a new printer forgets the last one's readings",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = u1();
    state.set_hardware(hw);
    state.apply(frame(hw, {{"power_detected", true}, {"mode", 1}}));
    REQUIRE(available() == 1);
    state.set_hardware(hw);
    CHECK(available() == 0);
    CHECK(mode() == -1);
    CHECK_FALSE(state.filter_seconds().has_value());
}

TEST_CASE_METHOD(Fixture, "chamber air state: a printer without a provider offers nothing",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = hw_with({"extruder", "purifier"});
    state.set_hardware(hw);
    air::Reading r;
    r.present = true;
    r.mode = AirMode::Exhaust;
    state.apply(r);
    CHECK(available() == 0);
    CHECK(edges.empty());
}

TEST_CASE_METHOD(Fixture, "chamber air state: an always-present provider is available at once",
                 "[chamber_air_state]") {
    const PrinterDiscovery hw = hw_with({"extruder", "gcode_macro AIR_CIRCULATION_STOP"});
    state.set_hardware(hw);
    CHECK(available() == 1);
    CHECK(mode() == -1);
    CHECK(edges.empty());
}
