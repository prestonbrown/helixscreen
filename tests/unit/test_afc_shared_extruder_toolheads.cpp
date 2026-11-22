// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_shared_extruder_toolheads.cpp
 * @brief Units sharing one extruder draw one toolhead, whatever the section is called.
 *
 * The reporter's Snapmaker U1 has four extruders and five AFC units. Two units
 * feed each of two of them:
 *
 *   Box_Turtle Turtle_1     hub      -> e0
 *   Claymore HTLF_claymore_1 hub     -> e0
 *   ViViD Vivid_1           hub      -> e3
 *   EMU EMU_1               hub      -> e3
 *   Toolchanger Tools       parallel -> e1, e2
 *
 * The overview drew SIX toolhead nodes. The cross-unit merge in
 * compute_system_tool_layout() keyed on AmsUnit::hub_tool_label, an integer
 * parsed out of the extruder name by tool_number_for_extruder(), which only
 * accepts `extruder` / `extruder<N>`. These sections are named `e0`..`e3`, so
 * every unit came back with hub_tool_label == -1 and none of them merged.
 *
 * Two separable things were wrong, and they need separate tests because they
 * have different inputs:
 *
 *   COUNT    Two units feed one nozzle when they name the same extruder. That
 *            is string identity and needs nothing but the status frame, so it
 *            must hold with no configfile at all.
 *   IDENTITY Badging that nozzle `E<n>` needs the section name resolved to a
 *            Klipper extruder name, which only configfile.settings carries
 *            (`[AFC_extruder e0] extruder_name: extruder`). Without it the
 *            nodes still merge, they just fall back to `T` lane aliases.
 *
 * Fixture is the reporter's own capture (debug bundle, v0.99.113).
 * Related: #1229, which established that toolhead nodes carry extruder
 * identity `E<n>` and that `T` means an AFC lane alias and nothing else.
 */

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/log_capture.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string fixture_dir() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/";
    }
    return "tests/fixtures/";
}

nlohmann::json load_fixture(const std::string& name) {
    const std::string path = fixture_dir() + name;
    std::ifstream f(path);
    INFO("fixture missing or unreadable: " << path);
    REQUIRE(f.is_open());
    nlohmann::json j;
    f >> j;
    return j;
}

/// Distinct extruders named by the capture's own lane objects — ground truth
/// for how many toolheads the machine has, derived rather than hardcoded.
std::set<std::string> distinct_lane_extruders(const nlohmann::json& status) {
    std::set<std::string> names;
    for (auto& item : status.items()) {
        const bool is_lane =
            item.key().rfind("AFC_lane ", 0) == 0 || item.key().rfind("AFC_stepper ", 0) == 0;
        if (!is_lane || !item.value().contains("extruder") ||
            !item.value()["extruder"].is_string()) {
            continue;
        }
        const auto name = item.value()["extruder"].get<std::string>();
        if (!name.empty()) {
            names.insert(name);
        }
    }
    return names;
}

/// The reporter's `[AFC_extruder eN] extruder_name:` lines, keyed lowercase the
/// way Klipper publishes configfile section suffixes.
std::unordered_map<std::string, std::string> u1_configfile_extruder_names() {
    return {
        {"e0", "extruder"},
        {"e1", "extruder1"},
        {"e2", "extruder2"},
        {"e3", "extruder3"},
    };
}

} // namespace

namespace helix {

// Not in the anonymous namespace: AmsBackendAfc befriends this by name.
class AfcSharedExtruderHelper : public AmsBackendAfc {
  public:
    AfcSharedExtruderHelper() : AmsBackendAfc(nullptr, nullptr) {}

    /// `AFC.lanes` / `AFC.hubs` are what the discovery pass converges on for
    /// this machine (its lanes arrive as a mix of AFC_lane and AFC_stepper
    /// objects); taking them from the capture keeps the test off that path.
    void discover_from(const nlohmann::json& status) {
        std::vector<std::string> lanes;
        std::vector<std::string> hubs;
        for (const auto& lane : status["AFC"]["lanes"]) {
            lanes.push_back(lane.get<std::string>());
        }
        for (const auto& hub : status["AFC"]["hubs"]) {
            hubs.push_back(hub.get<std::string>());
        }
        set_discovered_lanes(lanes, hubs);
        discovered_lanes_ = lanes;
    }

    /// What start() does before the first status frame arrives: the slots
    /// exist, in discovery order, before any unit object regroups them.
    void seed_slots_from_discovery() {
        std::lock_guard<std::mutex> lock(mutex_);
        AfcTestAccess::initialize_slots(*this, discovered_lanes_);
    }

    void feed(const nlohmann::json& status) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({status, 0.0});
        handle_status_update(notification);
    }

    /// The first frame creates the slots, so per-lane data parsed during it
    /// lands before they exist. Feed until it has settled.
    void feed_until_settled(const nlohmann::json& status, int frames = 3) {
        for (int i = 0; i < frames; ++i) {
            feed(status);
        }
    }

    /// Stand in for `[AFC_extruder <section>] extruder_name:` in
    /// configfile.settings. client_ is null here, so
    /// query_afc_configfile_topology() never runs.
    void seed_extruder_klipper_names(std::unordered_map<std::string, std::string> names) {
        std::lock_guard<std::mutex> lock(mutex_);
        AfcTestAccess::extruder_klipper_names(*this) = std::move(names);
        AfcTestAccess::configfile_answered(*this) = true; // stands in for the query having landed
        AfcTestAccess::extruder_tool_index_warned(*this).clear();
    }

  private:
    std::vector<std::string> discovered_lanes_;
};
} // namespace helix

TEST_CASE("AFC shared extruders: two units on one extruder draw one toolhead",
          "[ams][afc][toolchanger][topology][shared_extruder]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    // Ground truth from the capture, not from this test's assumptions.
    const auto extruders = distinct_lane_extruders(status);
    INFO("lane-named extruders: " << extruders.size());
    REQUIRE(extruders.size() == 4);
    REQUIRE(status["AFC"]["extruders"].size() == 4);
    REQUIRE(status["AFC"]["units"].size() == 5);

    AfcSharedExtruderHelper afc;
    afc.discover_from(status);
    afc.feed_until_settled(status);

    const AmsSystemInfo info = afc.get_system_info();
    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);

    for (size_t i = 0; i < info.units.size() && i < layout.units.size(); ++i) {
        spdlog::warn("  unit '{}' topology={} tool_count={} first_physical={}", info.units[i].name,
                     path_topology_to_string(afc.get_unit_topology(static_cast<int>(i))),
                     layout.units[i].tool_count, layout.units[i].first_physical_tool);
    }

    INFO("drew " << layout.total_physical_tools << " toolhead nodes for " << extruders.size()
                 << " extruders");
    CHECK(layout.total_physical_tools == static_cast<int>(extruders.size()));
}

// The first frame of a connection names AFC.current_load before the unit
// objects regroup the slots, so a tool read from that lane's slot index then
// names whatever lane lands on that index after the regroup. The mounted
// extruder still says which head is on the carriage, and the frames that follow
// carry no current_load to correct it.
TEST_CASE("AFC shared extruders: the mounted head survives the first frame's slot regroup",
          "[ams][afc][2158][toolchanger][shared_extruder]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];
    REQUIRE(status["AFC_extruder e3"]["on_shuttle"].get<bool>());
    REQUIRE(status["AFC"]["current_load"] == "lane10");

    AfcSharedExtruderHelper afc;
    afc.discover_from(status);
    afc.seed_slots_from_discovery();
    afc.feed(status);
    afc.feed(nlohmann::json::object());

    const AmsSystemInfo info = afc.get_system_info();
    const std::vector<int> mapping = afc.get_tool_mapping();
    INFO("current_tool=" << info.current_tool << " current_slot=" << info.current_slot);
    REQUIRE(info.current_tool >= 0);
    REQUIRE(info.current_tool < static_cast<int>(mapping.size()));
    const SlotInfo* slot = info.get_slot_global(mapping[static_cast<size_t>(info.current_tool)]);
    REQUIRE(slot != nullptr);
    INFO("the active tool prints through another head than the one on the carriage");
    CHECK(slot->extruder_name == "e3");
    INFO("the active tool runs a lane other than the one seated on the carriage");
    CHECK(mapping[static_cast<size_t>(info.current_tool)] == info.current_slot);
}

TEST_CASE("AFC shared extruders: merging needs no configfile",
          "[ams][afc][toolchanger][topology][shared_extruder]") {
    // The count is a property of the status frame alone. A machine whose
    // configfile query failed, or that answered before it landed, must still
    // draw one node per extruder — the names are equal strings either way.
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    AfcSharedExtruderHelper afc; // deliberately NOT seeded
    afc.discover_from(status);
    afc.feed_until_settled(status);

    const AmsSystemInfo info = afc.get_system_info();
    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);

    CHECK(layout.total_physical_tools == 4);

    // Unresolvable section names mean no extruder identity, so badges fall back
    // to AFC lane aliases. That is the documented #1229 contract, not a defect.
    CHECK_FALSE(ams_draw::layout_has_extruder_identity(layout));
    const auto labels = ams_draw::compute_tool_badge_labels(layout, info, -1, -1);
    CHECK(labels.prefix == 'T');
}

TEST_CASE("AFC shared extruders: configfile extruder_name gives toolheads E<n> identity",
          "[ams][afc][toolchanger][topology][shared_extruder]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    AfcSharedExtruderHelper afc;
    afc.seed_extruder_klipper_names(u1_configfile_extruder_names());
    afc.discover_from(status);
    afc.feed_until_settled(status);

    const AmsSystemInfo info = afc.get_system_info();
    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);

    REQUIRE(layout.total_physical_tools == 4);

    // Every nozzle knows which Klipper extruder it is, so badges are E0..E3.
    REQUIRE(ams_draw::layout_has_extruder_identity(layout));

    std::set<std::string> resolved(layout.physical_to_extruder_name.begin(),
                                   layout.physical_to_extruder_name.end());
    CHECK(resolved == std::set<std::string>{"extruder", "extruder1", "extruder2", "extruder3"});

    const auto labels = ams_draw::compute_tool_badge_labels(layout, info, -1, -1);
    CHECK(labels.prefix == 'E');
    std::vector<int> numbers = labels.numbers;
    std::sort(numbers.begin(), numbers.end());
    CHECK(numbers == std::vector<int>{1, 2, 3, 4}); // 1-based extruder identity
}

// ============================================================================
// The unresolvable-extruder warning must not fire before configfile answers
// ============================================================================
//
// A section named `e0` is REQUIRED to carry `extruder_name` — AFC refuses to
// start otherwise (AFC_extruder.py:384) — so any machine that boots already
// has the option the warning told the reporter to add. It fired only because
// the configfile query lands milliseconds after the first status frames.

TEST_CASE("AFC extruder warning: silent until configfile has actually answered",
          "[ams][afc][extruder_tool_index][shared_extruder]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    LogCapture log;
    AfcSharedExtruderHelper afc; // configfile has NOT answered
    afc.discover_from(status);
    afc.feed_until_settled(status);

    INFO("a pending configfile query is not evidence that the config is wrong");
    CHECK(log.count_containing("Cannot determine a tool number") == 0);
}

TEST_CASE("AFC extruder warning: fires once configfile answers and still has nothing",
          "[ams][afc][extruder_tool_index][shared_extruder]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    LogCapture log;
    AfcSharedExtruderHelper afc;
    // Query landed and carried no extruder_name for any section — now the
    // config really is missing it, and saying so is correct.
    afc.seed_extruder_klipper_names({});
    afc.discover_from(status);
    afc.feed_until_settled(status);

    INFO("gate must not silence a config that genuinely lacks extruder_name");
    CHECK(log.count_containing("Cannot determine a tool number") > 0);
}

TEST_CASE("AFC extruder warning: an unparseable extruder_name names the value it read",
          "[ams][afc][extruder_tool_index][shared_extruder]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    LogCapture log;
    AfcSharedExtruderHelper afc;
    // AFC accepts any value containing "extruder", so this is a config it will
    // happily start with and we still cannot number. Telling the user to set
    // the option they already set would be useless; quote it back instead.
    afc.seed_extruder_klipper_names({{"e0", "my_extruder_left"}});
    afc.discover_from(status);
    afc.feed_until_settled(status);

    CHECK(log.count_containing("its extruder_name is 'my_extruder_left'") > 0);
}

TEST_CASE("AFC toolhead sensor: each unit reads its own extruders' pins",
          "[ams][afc][configfile][toolhead_sensor]") {
    auto fixture = load_fixture("afc_u1_shared_extruders.json");
    const auto& status = fixture["status"];

    AfcSharedExtruderHelper afc;
    afc.discover_from(status);
    afc.feed_until_settled(status);
    int state_events = 0;
    afc.set_event_callback([&](const std::string& event, const std::string&) {
        state_events += event == AmsBackend::EVENT_STATE_CHANGED;
    });

    auto sensor_of = [&](const std::string& unit_name) {
        for (const auto& unit : afc.get_system_info().units) {
            if (unit.display_name == unit_name)
                return unit.has_toolhead_sensor;
        }
        FAIL("no unit " << unit_name);
        return false;
    };
    const std::vector<std::string> units = {"Turtle_1", "HTLF_claymore_1", "Vivid_1", "EMU_1",
                                            "Tools"};
    // Unknown until configfile answers.
    for (const auto& name : units) {
        CAPTURE(name);
        REQUIRE(sensor_of(name));
    }

    auto section = [](const char* klipper_name, nlohmann::json pins) {
        pins["extruder_name"] = klipper_name;
        return pins;
    };
    nlohmann::json settings = {
        {"afc_extruder e0", section("extruder", {{"pin_tool_start", "Turtle_1:PA1"}})},
        {"afc_extruder e1", section("extruder1", {{"pin_tool_start", "buffer"}})},
        {"afc_extruder e2", section("extruder2", {{"pin_tool_end", "PB2"}})},
    };

    SECTION("units on an unfitted extruder report none, the rest keep theirs") {
        settings["afc_extruder e3"] =
            section("extruder3", {{"pin_tool_start", "none"}, {"pin_tool_end", ""}});
        AfcTestAccess::apply_configfile_topology(
            afc, AmsBackendAfc::parse_configfile_topology(
                     {{"result", {{"status", {{"configfile", {{"settings", settings}}}}}}}}));
        CHECK(sensor_of("Turtle_1"));
        CHECK(sensor_of("HTLF_claymore_1"));
        CHECK_FALSE(sensor_of("Vivid_1"));
        CHECK_FALSE(sensor_of("EMU_1"));
        // Tools feeds e1 (borrowed from the buffer) and e2 (fitted).
        CHECK(sensor_of("Tools"));
        CHECK(state_events == 1);
    }
    SECTION("a unit whose extruders the config does not describe keeps the default") {
        // e3 has no section, and the sections that exist disagree.
        AfcTestAccess::apply_configfile_topology(
            afc, AmsBackendAfc::parse_configfile_topology(
                     {{"result", {{"status", {{"configfile", {{"settings", settings}}}}}}}}));
        CHECK(sensor_of("Vivid_1"));
        CHECK(sensor_of("EMU_1"));
        CHECK(state_events == 0);
    }
}

TEST_CASE("AFC toolhead sensor: the per-unit rule", "[ams][afc][toolhead_sensor]") {
    const std::unordered_map<std::string, bool> mixed = {{"e0", true}, {"e1", false}};
    const std::unordered_map<std::string, bool> none = {{"e0", false}, {"e1", false}};
    const std::unordered_map<std::string, bool> all = {{"e0", true}, {"e1", true}};

    CHECK(AmsBackendAfc::unit_toolhead_sensor_fitted({"e0"}, mixed));
    CHECK_FALSE(AmsBackendAfc::unit_toolhead_sensor_fitted({"e1"}, mixed));
    CHECK(AmsBackendAfc::unit_toolhead_sensor_fitted({"e1", "e0"}, mixed));
    // Unknown extruders: the sections' common answer, else the default.
    CHECK(AmsBackendAfc::unit_toolhead_sensor_fitted({}, mixed));
    CHECK(AmsBackendAfc::unit_toolhead_sensor_fitted({"e9"}, mixed));
    CHECK_FALSE(AmsBackendAfc::unit_toolhead_sensor_fitted({}, none));
    CHECK_FALSE(AmsBackendAfc::unit_toolhead_sensor_fitted({"e1", "e9"}, none));
    CHECK(AmsBackendAfc::unit_toolhead_sensor_fitted({"e9"}, all));
    CHECK(AmsBackendAfc::unit_toolhead_sensor_fitted({}, {}));
}
