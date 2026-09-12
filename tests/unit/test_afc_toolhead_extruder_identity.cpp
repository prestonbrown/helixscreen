// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_afc_toolhead_extruder_identity.cpp
 * @brief Toolhead badges name the extruder, not an AFC lane alias (#1229 defect 2).
 *
 * Three numbering systems collide on the reporter's machine:
 *
 *   - Klipper extruder objects   extruder, extruder1 … extruder5
 *   - Klipper `tool T<n>` objects
 *   - AFC per-lane `map` aliases T0, T1, T2, T3, T4, T6 …
 *
 * Every "T" badge drawn on a TOOLHEAD node was the third of those — a per-lane
 * alias — which reads as a tool number. On this capture the lane feeding
 * `extruder5` is mapped `T0`, so the sixth toolhead was badged "T0" while the
 * machine's first extruder sat two nodes to its left. `T` now means AFC lane
 * alias and only that; toolhead nodes carry extruder identity, `E<n>`, taken
 * from the extruder NAME.
 *
 * Physical node order is not ascending by extruder — it follows the unit order
 * and, inside a PARALLEL unit, alphabetical extruder names:
 *
 *   HTLF_1  (mixed,    first_physical=0) -> extruder, extruder1, extruder2
 *   Tools   (parallel, first_physical=3) -> extruder4, extruder5
 *   AMS_1   (hub,      first_physical=5) -> extruder3
 */

#include "ams_backend_afc.h"
#include "ams_types.h"
#include "display_numbering.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <optional>
#include <string>
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

/// Harness copied from test_afc_toolhead_node_count.cpp — same capture, same
/// settling requirement.
class AfcLayoutHelper : public AmsBackendAfc {
  public:
    AfcLayoutHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void discover(const nlohmann::json& object_list) {
        std::vector<std::string> lanes;
        std::vector<std::string> hubs;
        for (const auto& entry : object_list) {
            const std::string name = entry.get<std::string>();
            if (name.rfind("AFC_lane ", 0) == 0) {
                lanes.push_back(name.substr(9));
            } else if (name.rfind("AFC_hub ", 0) == 0) {
                hubs.push_back(name.substr(8));
            }
        }
        set_discovered_lanes(lanes, hubs);
    }

    void feed(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    /// The first frame creates the slots, so per-lane data parsed during it
    /// lands before they exist. Feed until it has settled.
    void feed_until_settled(const nlohmann::json& params, int frames = 3) {
        for (int i = 0; i < frames; ++i) {
            feed(params);
        }
    }
};

/// The capture's own answer for a lane's AFC `map` alias ("T0" -> 0).
std::optional<int> fixture_lane_alias(const nlohmann::json& status, const std::string& extruder) {
    for (auto& item : status.items()) {
        if (item.key().rfind("AFC_lane ", 0) != 0) {
            continue;
        }
        const auto& lane = item.value();
        if (!lane.contains("extruder") || !lane["extruder"].is_string() ||
            lane["extruder"].get<std::string>() != extruder) {
            continue;
        }
        if (lane.contains("map") && lane["map"].is_string()) {
            const std::string map = lane["map"].get<std::string>();
            if (map.size() > 1 && map[0] == 'T') {
                return std::stoi(map.substr(1));
            }
        }
    }
    return std::nullopt;
}

/// Global slot index of the (single) lane feeding @p extruder, or -1.
int global_slot_for_extruder(const AmsSystemInfo& info, const std::string& extruder) {
    for (const auto& unit : info.units) {
        for (size_t s = 0; s < unit.slots.size(); ++s) {
            if (unit.slots[s].extruder_name == extruder) {
                return unit.first_slot_global_index + static_cast<int>(s);
            }
        }
    }
    return -1;
}

/// Lanes whose extruder name actually landed. Zero here means the per-lane data
/// was parsed before the slots existed and every assertion below is vacuous.
int lanes_with_extruder_names(const AmsSystemInfo& info) {
    int n = 0;
    for (const auto& unit : info.units) {
        for (const auto& slot : unit.slots) {
            if (!slot.extruder_name.empty()) {
                ++n;
            }
        }
    }
    return n;
}

} // namespace

TEST_CASE("extruder name -> tool number is the only naming rule",
          "[ams][afc][1229][toolchanger][badges]") {
    CHECK(helix::tool_number_for_extruder("extruder") == 0);
    CHECK(helix::tool_number_for_extruder("extruder5") == 5);
    CHECK(helix::tool_number_for_extruder("extruder10") == 10);

    // Not extruders, and emphatically not tool 0 — the catch-all that let a
    // missing name masquerade as "E0".
    CHECK_FALSE(helix::tool_number_for_extruder("").has_value());
    CHECK_FALSE(helix::tool_number_for_extruder("foo").has_value());
    CHECK_FALSE(helix::tool_number_for_extruder("extruderX").has_value());
}

TEST_CASE("AFC toolchanger: toolhead nodes carry extruder identity, not lane aliases",
          "[ams][afc][1229][toolchanger][badges]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcLayoutHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();

    // PRECONDITION. A single status frame leaves every slot blank, which would
    // make the name assertions below pass against nothing at all. The capture
    // declares an extruder on every one of its lanes.
    int lane_objects = 0;
    for (auto& item : fixture["status"].items()) {
        if (item.key().rfind("AFC_lane ", 0) == 0 && item.value().contains("extruder")) {
            ++lane_objects;
        }
    }
    REQUIRE(lane_objects == 10);
    INFO("per-lane extruder names never reached the slots — assertions would be vacuous");
    REQUIRE(lanes_with_extruder_names(info) == lane_objects);

    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);

    spdlog::warn("=== #1229 toolhead extruder identity ===");
    for (int p = 0; p < layout.total_physical_tools; ++p) {
        spdlog::warn("  physical {} -> extruder '{}' (legacy T label {})", p,
                     p < static_cast<int>(layout.physical_to_extruder_name.size())
                         ? layout.physical_to_extruder_name[p]
                         : std::string("<none>"),
                     layout.physical_to_virtual_label[p]);
    }

    const std::vector<std::string> expected = {"extruder",  "extruder1", "extruder2",
                                               "extruder4", "extruder5", "extruder3"};
    CHECK(layout.physical_to_extruder_name == expected);
    CHECK(ams_draw::layout_has_extruder_identity(layout));

    const auto badges = ams_draw::compute_tool_badge_labels(layout, info, -1, -1);
    CHECK(badges.prefix == 'E');
    REQUIRE(badges.numbers.size() == expected.size());
    for (size_t p = 0; p < expected.size(); ++p) {
        INFO("physical node " << p << " is " << expected[p]);
        CHECK(badges.numbers[p] ==
              helix::ui::lane_number(helix::tool_number_for_extruder(expected[p]).value()));
    }
}

TEST_CASE("AFC toolchanger: the extruder5 toolhead is E5, never the lane's T0 alias",
          "[ams][afc][1229][toolchanger][badges]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcLayoutHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();
    REQUIRE(lanes_with_extruder_names(info) > 0); // precondition, see above

    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);
    REQUIRE(ams_draw::layout_has_extruder_identity(layout));

    // Physical node 4 is extruder5. Derived from the layout, not asserted at it.
    int node_e5 = -1;
    int node_e4 = -1;
    for (int p = 0; p < static_cast<int>(layout.physical_to_extruder_name.size()); ++p) {
        if (layout.physical_to_extruder_name[p] == "extruder5") {
            node_e5 = p;
        }
        if (layout.physical_to_extruder_name[p] == "extruder4") {
            node_e4 = p;
        }
    }
    REQUIRE(node_e5 == 4);
    REQUIRE(node_e4 == 3);

    // The capture's own alias for those lanes — the numbers that used to be
    // painted on these toolheads.
    const auto alias_e5 = fixture_lane_alias(fixture["status"], "extruder5");
    const auto alias_e4 = fixture_lane_alias(fixture["status"], "extruder4");
    REQUIRE(alias_e5.has_value());
    REQUIRE(alias_e4.has_value());
    REQUIRE(*alias_e5 == 0); // the collision: extruder5's lane is mapped T0
    REQUIRE(*alias_e4 == 2);

    // The legacy T labels still carry those aliases at the same indices, which
    // is precisely what made the badge unreadable.
    REQUIRE(layout.physical_to_virtual_label[node_e5] == *alias_e5);
    REQUIRE(layout.physical_to_virtual_label[node_e4] == *alias_e4);

    const auto badges = ams_draw::compute_tool_badge_labels(layout, info, -1, -1);
    REQUIRE(badges.numbers.size() > static_cast<size_t>(node_e5));

    INFO("toolhead " << node_e5 << " drives extruder5 but its lane is mapped T" << *alias_e5);
    CHECK(badges.numbers[node_e5] == 6);         // 1-based extruder5 (index 5)
    CHECK(badges.numbers[node_e5] != *alias_e5); // THE defect: 6, never 0
    CHECK(badges.numbers[node_e4] == 5);         // 1-based extruder4 (index 4)
    CHECK(badges.numbers[node_e4] != *alias_e4);
    CHECK(badges.prefix == 'E');
}

TEST_CASE("AFC toolchanger: an active lane's alias is never written onto a toolhead",
          "[ams][afc][1229][toolchanger][badges]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcLayoutHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();
    REQUIRE(lanes_with_extruder_names(info) > 0);

    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);
    REQUIRE(ams_draw::layout_has_extruder_identity(layout));

    // Make the extruder5 lane the active one. Its alias (T0) disagrees with its
    // extruder identity (E5), so the legacy "stamp the active slot's mapped_tool
    // onto the active node" step would visibly corrupt the badge.
    const int active_slot = global_slot_for_extruder(info, "extruder5");
    REQUIRE(active_slot >= 0);
    const SlotInfo* slot = info.get_slot_global(active_slot);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->mapped_tool == 0);

    const int active_node = 4; // extruder5's physical node
    REQUIRE(layout.physical_to_extruder_name[active_node] == "extruder5");

    const auto badges = ams_draw::compute_tool_badge_labels(layout, info, active_slot, active_node);
    INFO("active lane alias T" << slot->mapped_tool << " must not overwrite the E5 badge");
    CHECK(badges.numbers[active_node] == 6); // 1-based extruder5 (index 5)
    CHECK(badges.prefix == 'E');

    // The legacy path keeps the substitution — it is long-standing behaviour on
    // backends with no extruder names, and this pins that it is still applied.
    ams_draw::SystemToolLayout legacy = layout;
    legacy.physical_to_extruder_name.assign(legacy.physical_to_extruder_name.size(), std::string());
    const auto legacy_badges =
        ams_draw::compute_tool_badge_labels(legacy, info, active_slot, active_node);
    CHECK(legacy_badges.prefix == 'T');
    CHECK(legacy_badges.numbers[active_node] == slot->mapped_tool);
}

TEST_CASE("AFC toolchanger: partial extruder identity falls back to T labels",
          "[ams][afc][1229][toolchanger][badges]") {
    auto fixture = load_fixture("afc_toolchanger_multiunit.json");

    AfcLayoutHelper afc;
    afc.discover(fixture["object_list"]);
    afc.feed_until_settled(fixture["status"]);

    const AmsSystemInfo info = afc.get_system_info();
    REQUIRE(lanes_with_extruder_names(info) > 0);

    const auto layout = ams_draw::compute_system_tool_layout(info, &afc);
    REQUIRE(ams_draw::layout_has_extruder_identity(layout));

    SECTION("one unknown name disables identity for the whole layout") {
        for (size_t blanked = 0; blanked < layout.physical_to_extruder_name.size(); ++blanked) {
            ams_draw::SystemToolLayout partial = layout;
            partial.physical_to_extruder_name[blanked].clear();

            INFO("blanked physical node " << blanked);
            CHECK_FALSE(ams_draw::layout_has_extruder_identity(partial));

            const auto badges = ams_draw::compute_tool_badge_labels(partial, info, -1, -1);
            CHECK(badges.prefix == 'T');
            CHECK(badges.numbers == layout.physical_to_virtual_label);
        }
    }

    SECTION("an unparseable name is not identity either") {
        ams_draw::SystemToolLayout bogus = layout;
        bogus.physical_to_extruder_name[0] = "extruderX";
        CHECK_FALSE(ams_draw::layout_has_extruder_identity(bogus));
        CHECK(ams_draw::compute_tool_badge_labels(bogus, info, -1, -1).prefix == 'T');
    }

    SECTION("a short vector is not identity either") {
        ams_draw::SystemToolLayout truncated = layout;
        truncated.physical_to_extruder_name.pop_back();
        CHECK_FALSE(ams_draw::layout_has_extruder_identity(truncated));
        CHECK(ams_draw::compute_tool_badge_labels(truncated, info, -1, -1).prefix == 'T');
    }

    SECTION("an empty layout has no identity") {
        ams_draw::SystemToolLayout empty;
        CHECK_FALSE(ams_draw::layout_has_extruder_identity(empty));
        CHECK(ams_draw::compute_tool_badge_labels(empty, info, -1, -1).numbers.empty());
    }
}
