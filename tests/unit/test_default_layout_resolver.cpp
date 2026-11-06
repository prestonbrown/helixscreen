// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "default_layout_resolver.h"
#include "panel_widget_registry.h"
#include "scope_exit.h"

#include <algorithm>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::layout::CapabilitySnapshot;
using helix::layout::ResolvedDefault;
using helix::layout::ResolvedPlacement;
using json = nlohmann::json;

namespace {

json parse(const char* text) {
    return json::parse(text, nullptr, false);
}

ResolvedDefault resolve(const json& table, std::string_view tier, const CapabilitySnapshot& caps,
                        int cols = 12, int rows = 8, const json* overlay = nullptr,
                        const std::vector<std::string>& chain = {}, std::string_view preset = "") {
    return helix::layout::resolve_default_layout(table, overlay, tier, cols, rows, chain, preset,
                                                 caps);
}

const ResolvedPlacement* placed_entry(const ResolvedDefault& r, const std::string& id) {
    auto it = std::find_if(r.placed.begin(), r.placed.end(),
                           [&](const ResolvedPlacement& p) { return p.id == id; });
    return it == r.placed.end() ? nullptr : &*it;
}

bool is_disabled(const ResolvedDefault& r, const std::string& id) {
    return std::find(r.disabled.begin(), r.disabled.end(), id) != r.disabled.end();
}

void require_cell(const ResolvedDefault& r, const std::string& id, int col, int row, int colspan,
                  int rowspan) {
    INFO("widget " << id);
    const auto* p = placed_entry(r, id);
    REQUIRE(p != nullptr);
    CHECK(p->col == col);
    CHECK(p->row == row);
    CHECK(p->colspan == colspan);
    CHECK(p->rowspan == rowspan);
}

// SPOOL_ENV as the spec draws it: one 4x2-track region, AMS across it, else an
// ENV cell and a FIL cell side by side.
const char* kSlotTable = R"json({
  "candidate_lists": {
    "ENV": ["dryer", "chamber_temperature", "led", "clock"],
    "FIL": ["filament", "led", {"id": "active_spool", "requires": ["printer_has_spoolman"]}, "clock"]
  },
  "anchors": [
    {"slot": "spool_env",
     "placements": {"medium": {"col": 8, "row": 0, "colspan": 4, "rowspan": 2}},
     "alternatives": [
       {"cells": [{"candidates": ["ams"], "at": [0, 0, 4, 2]}]},
       {"cells": [{"candidates": "ENV", "at": [0, 0, 2, 2]},
                  {"candidates": "FIL", "at": [2, 0, 2, 2]}]}
     ]},
    {"id": "printer_image",
     "placements": {"medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 4}}},
    {"id": "temperature",
     "config": {"style": "big"},
     "placements": {"medium": {"col": 4, "row": 0, "colspan": 2, "rowspan": 2,
                               "config": {"extra": 1}}}}
  ],
  "disabled": {"medium": ["tips"]}
})json";

} // namespace

TEST_CASE("resolver: fixed anchor lands at its tier key with merged config",
          "[default_layout_resolver]") {
    const json table = parse(kSlotTable);
    const auto r = resolve(table, "medium", {});
    REQUIRE(r.ok);
    require_cell(r, "printer_image", 0, 0, 4, 4);
    require_cell(r, "temperature", 4, 0, 2, 2);
    const auto* t = placed_entry(r, "temperature");
    CHECK(t->config == json{{"style", "big"}, {"extra", 1}});
    CHECK(placed_entry(r, "printer_image")->config == json::object());
}

TEST_CASE("resolver: grid-qualified key beats the bare tier key", "[default_layout_resolver]") {
    const json table = parse(R"json({"anchors": [
      {"id": "printer_image", "placements": {
         "medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 4},
         "medium@14x8": {"col": 2, "row": 0, "colspan": 4, "rowspan": 4}}}]})json");
    require_cell(resolve(table, "medium", {}, 14, 8), "printer_image", 2, 0, 4, 4);
    require_cell(resolve(table, "medium", {}, 12, 8), "printer_image", 0, 0, 4, 4);
}

TEST_CASE("resolver: tier fallback chains", "[default_layout_resolver]") {
    SECTION("micro falls to tiny, then small") {
        const json t1 = parse(R"json({"anchors": [{"id": "clock", "placements": {
            "tiny": {"col": 1, "row": 0, "colspan": 4, "rowspan": 2},
            "small": {"col": 2, "row": 0, "colspan": 4, "rowspan": 2}}}]})json");
        require_cell(resolve(t1, "micro", {}), "clock", 1, 0, 4, 2);
        const json t2 = parse(R"json({"anchors": [{"id": "clock", "placements": {
            "small": {"col": 2, "row": 0, "colspan": 4, "rowspan": 2}}}]})json");
        require_cell(resolve(t2, "micro", {}), "clock", 2, 0, 4, 2);
    }
    SECTION("xxlarge falls to xlarge, then large") {
        const json t1 = parse(R"json({"anchors": [{"id": "clock", "placements": {
            "xlarge": {"col": 3, "row": 0, "colspan": 4, "rowspan": 2},
            "large": {"col": 4, "row": 0, "colspan": 4, "rowspan": 2}}}]})json");
        require_cell(resolve(t1, "xxlarge", {}), "clock", 3, 0, 4, 2);
        const json t2 = parse(R"json({"anchors": [{"id": "clock", "placements": {
            "large": {"col": 4, "row": 0, "colspan": 4, "rowspan": 2}}}]})json");
        require_cell(resolve(t2, "xxlarge", {}), "clock", 4, 0, 4, 2);
    }
    SECTION("medium has no fallback") {
        const json t = parse(R"json({"anchors": [{"id": "clock", "placements": {
            "small": {"col": 0, "row": 0, "colspan": 4, "rowspan": 2}}}]})json");
        CHECK_FALSE(resolve(t, "medium", {}).ok);
    }
}

TEST_CASE("resolver: SPOOL_ENV slot picks by capability", "[default_layout_resolver]") {
    const json table = parse(kSlotTable);

    SECTION("an AMS takes the whole region") {
        const auto r = resolve(table, "medium", {{{"ams_slot_count", 4}}});
        REQUIRE(r.ok);
        require_cell(r, "ams", 8, 0, 4, 2);
        CHECK(placed_entry(r, "filament") == nullptr);
        CHECK(placed_entry(r, "dryer") == nullptr);
    }
    SECTION("no AMS: ENV then FIL, each on its own cell") {
        const auto r = resolve(
            table, "medium",
            {{{"ams_slot_count", 0}, {"home_dryer_available", 1}, {"filament_sensor_count", 1}}});
        require_cell(r, "dryer", 8, 0, 2, 2);
        require_cell(r, "filament", 10, 0, 2, 2);
        CHECK(placed_entry(r, "ams") == nullptr);
    }
    SECTION("a closed gate skips to the next candidate") {
        const auto r =
            resolve(table, "medium", {{{"printer_has_chamber", 1}, {"led_controllable", 1}}});
        require_cell(r, "chamber_temperature", 8, 0, 2, 2);
        // filament's gate is closed and LED is free, so FIL takes it.
        require_cell(r, "led", 10, 0, 2, 2);
    }
    SECTION("requires printer_has_spoolman=0 skips active_spool") {
        // ENV takes led, FIL has no filament sensor and no LED left.
        const auto r =
            resolve(table, "medium", {{{"led_controllable", 1}, {"printer_has_spoolman", 0}}});
        require_cell(r, "led", 8, 0, 2, 2);
        CHECK(placed_entry(r, "active_spool") == nullptr);
        require_cell(r, "clock", 10, 0, 2, 2);
    }
    SECTION("requires printer_has_spoolman=1 admits active_spool") {
        const auto r =
            resolve(table, "medium", {{{"led_controllable", 1}, {"printer_has_spoolman", 1}}});
        require_cell(r, "active_spool", 10, 0, 2, 2);
    }
}

TEST_CASE("resolver: a widget already placed is skipped by a later cell",
          "[default_layout_resolver]") {
    SECTION("placed by a fixed anchor") {
        const json table = parse(R"json({"anchors": [
          {"slot": "s", "placements": {"medium": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}},
           "alternatives": [{"cells": [{"candidates": ["led", "clock"], "at": [0, 0, 2, 2]}]}]},
          {"id": "led", "placements": {"medium": {"col": 4, "row": 0, "colspan": 2, "rowspan": 2}}}
        ]})json");
        const auto r = resolve(table, "medium", {{{"led_controllable", 1}}});
        require_cell(r, "led", 4, 0, 2, 2);
        require_cell(r, "clock", 0, 0, 2, 2);
        CHECK(std::count_if(r.placed.begin(), r.placed.end(),
                            [](const ResolvedPlacement& p) { return p.id == "led"; }) == 1);
    }
    SECTION("placed by an earlier slot") {
        const json table = parse(R"json({"anchors": [
          {"slot": "a", "placements": {"medium": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}},
           "alternatives": [{"cells": [{"candidates": ["led", "clock"], "at": [0, 0, 2, 2]}]}]},
          {"slot": "b", "placements": {"medium": {"col": 2, "row": 0, "colspan": 2, "rowspan": 2}},
           "alternatives": [{"cells": [{"candidates": ["led", "notifications"], "at": [0, 0, 2, 2]}]}]}
        ]})json");
        const auto r = resolve(table, "medium", {{{"led_controllable", 1}}});
        require_cell(r, "led", 0, 0, 2, 2);
        require_cell(r, "notifications", 2, 0, 2, 2);
    }
    SECTION("a lead already placed does not win its alternative") {
        const json table = parse(R"json({"anchors": [
          {"id": "clock", "placements": {"medium": {"col": 4, "row": 0, "colspan": 2, "rowspan": 2}}},
          {"slot": "s", "placements": {"medium": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}},
           "alternatives": [
             {"cells": [{"candidates": ["clock"], "at": [0, 0, 2, 2]}]},
             {"cells": [{"candidates": ["notifications"], "at": [0, 0, 2, 2]}]}]}
        ]})json");
        const auto r = resolve(table, "medium", {});
        require_cell(r, "notifications", 0, 0, 2, 2);
        require_cell(r, "clock", 4, 0, 2, 2);
    }
}

TEST_CASE("resolver: losers and tier disabled ids are disabled", "[default_layout_resolver]") {
    const json table = parse(kSlotTable);
    const auto r = resolve(table, "medium", {{{"ams_slot_count", 4}}});
    for (const char* loser :
         {"dryer", "chamber_temperature", "led", "clock", "filament", "active_spool"}) {
        INFO(loser);
        CHECK(is_disabled(r, loser));
    }
    CHECK(is_disabled(r, "tips"));
    CHECK_FALSE(is_disabled(r, "ams"));
    CHECK_FALSE(is_disabled(r, "printer_image"));
    CHECK(std::is_sorted(r.disabled.begin(), r.disabled.end()));

    SECTION("a tier-disabled widget is neither anchored nor a candidate") {
        const json t = parse(R"json({"anchors": [
          {"id": "tips", "placements": {"medium": {"col": 0, "row": 0, "colspan": 8, "rowspan": 2}}},
          {"slot": "s", "placements": {"medium": {"col": 0, "row": 2, "colspan": 2, "rowspan": 2}},
           "alternatives": [{"cells": [{"candidates": ["led", "clock"], "at": [0, 0, 2, 2]}]}]}],
          "disabled": {"medium": ["tips", "led"]}})json");
        const auto r2 = resolve(t, "medium", {{{"led_controllable", 1}}});
        CHECK(placed_entry(r2, "tips") == nullptr);
        require_cell(r2, "clock", 0, 2, 2, 2);
        CHECK(is_disabled(r2, "tips"));
        CHECK(is_disabled(r2, "led"));
    }
}

TEST_CASE("resolver: a grid-qualified disabled list beats the bare tier list",
          "[default_layout_resolver]") {
    // A grid too small for a tier's anchors switches some of them off without
    // touching the tier's other grids, the same way a grid-qualified placement
    // moves them.
    const json table = parse(R"json({"anchors": [
      {"id": "fan_stack", "placements": {"micro": {"col": 6, "row": 2, "colspan": 2, "rowspan": 2}}},
      {"id": "tips", "placements": {"micro": {"col": 0, "row": 0, "colspan": 6, "rowspan": 2}}}],
      "disabled": {"micro": ["tips"], "micro@8x6": ["tips", "fan_stack"]}})json");

    const auto small_grid = resolve(table, "micro", {}, 8, 6);
    CHECK(placed_entry(small_grid, "fan_stack") == nullptr);
    CHECK(is_disabled(small_grid, "fan_stack"));
    CHECK(is_disabled(small_grid, "tips"));

    const auto tier_grid = resolve(table, "micro", {}, 12, 8);
    require_cell(tier_grid, "fan_stack", 6, 2, 2, 2);
    CHECK_FALSE(is_disabled(tier_grid, "fan_stack"));
    CHECK(is_disabled(tier_grid, "tips"));
}

TEST_CASE("resolver: a capability-qualified key replaces its key while the subject is on",
          "[default_layout_resolver]") {
    // One tier, two arrangements: the qualified placements, slot region and
    // disabled list all switch together on the one subject.
    const json table = parse(R"json({"anchors": [
      {"id": "printer_image", "placements": {
         "medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 4},
         "medium+ams_is_tool_changer": {"col": 0, "row": 0, "colspan": 6, "rowspan": 4}}},
      {"id": "temperature", "placements": {"medium": {"col": 4, "row": 0, "colspan": 2, "rowspan": 2}}},
      {"slot": "s", "placements": {
         "medium": {"col": 4, "row": 2, "colspan": 4, "rowspan": 2},
         "medium+ams_is_tool_changer": {"col": 8, "row": 2, "colspan": 4, "rowspan": 2}},
       "alternatives": [{"cells": [{"candidates": [{"id": "tool_switcher", "requires": ["ams_is_tool_changer"]}],
                                    "at": [0, 0, 4, 2]}]}]}],
      "disabled": {"medium": ["tips"], "medium+ams_is_tool_changer": ["tips", "temperature"]}})json");

    const auto tc = resolve(table, "medium", {{{"ams_is_tool_changer", 1}}});
    require_cell(tc, "printer_image", 0, 0, 6, 4);
    require_cell(tc, "tool_switcher", 8, 2, 4, 2);
    CHECK(placed_entry(tc, "temperature") == nullptr);
    CHECK(is_disabled(tc, "temperature"));
    CHECK(tc.signature.find("ams_is_tool_changer=1") != std::string::npos);

    const auto plain = resolve(table, "medium", {{{"ams_is_tool_changer", 0}}});
    require_cell(plain, "printer_image", 0, 0, 4, 4);
    require_cell(plain, "temperature", 4, 0, 2, 2);
    CHECK(placed_entry(plain, "tool_switcher") == nullptr);
    CHECK(tc.signature != plain.signature);
}

TEST_CASE("resolver: a grid-qualified key beats a capability-qualified bare tier key",
          "[default_layout_resolver]") {
    // The grid decides what fits, so a key written for this grid wins over an
    // arrangement written for the tier's own grid; the grid's own capability
    // key beats both.
    json table = parse(R"json({"anchors": [
      {"id": "printer_image", "placements": {
         "micro": {"col": 0, "row": 0, "colspan": 4, "rowspan": 4},
         "micro+ams_is_tool_changer": {"col": 0, "row": 0, "colspan": 6, "rowspan": 4},
         "micro@8x6": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}}]})json");
    const CapabilitySnapshot tc{{{"ams_is_tool_changer", 1}}};
    require_cell(resolve(table, "micro", tc, 8, 6), "printer_image", 0, 0, 2, 2);
    require_cell(resolve(table, "micro", tc, 12, 8), "printer_image", 0, 0, 6, 4);

    table["anchors"][0]["placements"]["micro@8x6+ams_is_tool_changer"] =
        json{{"col", 2}, {"row", 0}, {"colspan", 2}, {"rowspan", 2}};
    require_cell(resolve(table, "micro", tc, 8, 6), "printer_image", 2, 0, 2, 2);
    require_cell(resolve(table, "micro", {}, 8, 6), "printer_image", 0, 0, 2, 2);
}

TEST_CASE("resolver: an alternative that does not fit its region is skipped",
          "[default_layout_resolver]") {
    const json table = parse(R"json({"anchors": [
      {"slot": "s", "placements": {
         "medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 2},
         "tiny": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}},
       "alternatives": [
         {"cells": [{"candidates": ["clock"], "at": [0, 0, 4, 2]}]},
         {"cells": [{"candidates": ["notifications"], "at": [0, 0, 2, 2]}]}]}]})json");
    SECTION("region wide enough: the first alternative wins") {
        const auto r = resolve(table, "medium", {});
        require_cell(r, "clock", 0, 0, 4, 2);
        CHECK(is_disabled(r, "notifications"));
    }
    SECTION("narrow region: the last alternative fills") {
        const auto r = resolve(table, "tiny", {});
        require_cell(r, "notifications", 0, 0, 2, 2);
        CHECK(placed_entry(r, "clock") == nullptr);
        CHECK(is_disabled(r, "clock"));
    }
}

TEST_CASE("resolver: an alternative that runs off the grid gives way to one that fits",
          "[default_layout_resolver]") {
    const json table = parse(R"json({"anchors": [
      {"slot": "s", "placements": {"medium": {"col": 8, "row": 0, "colspan": 4, "rowspan": 2}},
       "alternatives": [
         {"cells": [{"candidates": ["ams"], "at": [0, 0, 4, 2]}]},
         {"cells": [{"candidates": ["notifications"], "at": [0, 0, 2, 2]}]}]}]})json");
    CapabilitySnapshot caps;
    caps.subjects["ams_slot_count"] = 4;
    SECTION("a 12-column grid holds the region: the AMS wins") {
        const auto r = resolve(table, "medium", caps, 12, 8);
        require_cell(r, "ams", 8, 0, 4, 2);
    }
    SECTION("a 10-column grid cuts the region: the alternative that fits wins") {
        const auto r = resolve(table, "medium", caps, 10, 8);
        require_cell(r, "notifications", 8, 0, 2, 2);
        CHECK(placed_entry(r, "ams") == nullptr);
        CHECK(is_disabled(r, "ams"));
    }
    SECTION("an unknown grid checks the region only") {
        const auto r = resolve(table, "medium", caps, 0, 0);
        require_cell(r, "ams", 8, 0, 4, 2);
    }
}

TEST_CASE("resolver: when no alternative fits the grid the slot still picks a winner",
          "[default_layout_resolver]") {
    // The caller auto-places a winner it cannot seat; resolving nothing would
    // disable every candidate instead.
    const json table = parse(kSlotTable);
    CapabilitySnapshot caps;
    caps.subjects["ams_slot_count"] = 4;
    const auto r = resolve(table, "medium", caps, 10, 8);
    require_cell(r, "ams", 8, 0, 4, 2);
    CHECK_FALSE(is_disabled(r, "ams"));
}

TEST_CASE("resolver: the last alternative has no lead requirement", "[default_layout_resolver]") {
    const json table = parse(R"json({"anchors": [
      {"slot": "s", "placements": {"medium": {"col": 2, "row": 2, "colspan": 4, "rowspan": 2}},
       "alternatives": [
         {"cells": [{"candidates": ["ams"], "at": [0, 0, 4, 2]}]},
         {"cells": [{"candidates": ["dryer", "notifications"], "at": [0, 0, 2, 2]},
                    {"candidates": ["dryer", "clock"], "at": [2, 0, 2, 2]}]}]}]})json");
    const auto r = resolve(table, "medium", {});
    require_cell(r, "notifications", 2, 2, 2, 2);
    require_cell(r, "clock", 4, 2, 2, 2);
}

TEST_CASE("resolver: an earlier alternative is taken when any lead-cell candidate qualifies",
          "[default_layout_resolver]") {
    // The pair is not last, and its lead list opens with a closed gate: a later
    // qualifying candidate in that cell still takes the pair over the single.
    const json table = parse(R"json({"anchors": [
      {"slot": "s", "placements": {"medium": {"col": 2, "row": 2, "colspan": 4, "rowspan": 2}},
       "alternatives": [
         {"cells": [{"candidates": ["dryer", "led", "filament"], "at": [0, 0, 2, 2]},
                    {"candidates": ["dryer", "led", "filament"], "at": [2, 0, 2, 2]}]},
         {"cells": [{"candidates": ["notifications"], "at": [0, 0, 2, 2]}]}]}]})json");
    CapabilitySnapshot caps;
    caps.subjects["home_dryer_available"] = 0;
    caps.subjects["led_controllable"] = 1;
    caps.subjects["filament_sensor_count"] = 1;
    const auto r = resolve(table, "medium", caps);
    require_cell(r, "led", 2, 2, 2, 2);
    require_cell(r, "filament", 4, 2, 2, 2);

    CHECK(placed_entry(r, "notifications") == nullptr);

    // No candidate in the lead cell qualifies: the pair gives way to the single.
    caps.subjects["led_controllable"] = 0;
    caps.subjects["filament_sensor_count"] = 0;
    const auto none = resolve(table, "medium", caps);
    REQUIRE(none.placed.size() == 1);
    require_cell(none, "notifications", 2, 2, 2, 2);
}

TEST_CASE("resolver: cells sharing a list fill in written order and leave the rest empty",
          "[default_layout_resolver]") {
    const json table = parse(R"json({
      "candidate_lists": {"R": ["filament", "dryer", "led"]},
      "anchors": [
      {"slot": "s", "placements": {"medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 2}},
       "alternatives": [
         {"cells": [{"candidates": "R", "at": [2, 0, 2, 2]},
                    {"candidates": "R", "at": [0, 0, 2, 2]}]}]}]})json");
    CapabilitySnapshot caps;
    caps.subjects["filament_sensor_count"] = 1;
    caps.subjects["home_dryer_available"] = 0;
    caps.subjects["led_controllable"] = 0;
    const auto r = resolve(table, "medium", caps);
    REQUIRE(r.placed.size() == 1);
    require_cell(r, "filament", 2, 0, 2, 2);
}

TEST_CASE("resolver: overlay replaces and removes by id and slot", "[default_layout_resolver]") {
    const json table = parse(kSlotTable);
    const json overlay = parse(R"json({"anchors": [
      {"id": "printer_image", "remove": true},
      {"id": "temperature", "placements": {"medium": {"col": 6, "row": 2, "colspan": 2, "rowspan": 2}}},
      {"slot": "spool_env", "placements": {"medium": {"col": 0, "row": 4, "colspan": 2, "rowspan": 2}},
       "alternatives": [{"cells": [{"candidates": ["notifications"], "at": [0, 0, 2, 2]}]}]},
      {"id": "clock", "placements": {"medium": {"col": 8, "row": 4, "colspan": 4, "rowspan": 2}}}
    ]})json");
    const auto r = resolve(table, "medium", {{{"ams_slot_count", 4}}}, 12, 8, &overlay);
    REQUIRE(r.ok);
    CHECK(placed_entry(r, "printer_image") == nullptr);
    require_cell(r, "temperature", 6, 2, 2, 2);
    // The replaced anchor's config goes with it.
    CHECK(placed_entry(r, "temperature")->config == json::object());
    require_cell(r, "notifications", 0, 4, 2, 2);
    CHECK(placed_entry(r, "ams") == nullptr);
    require_cell(r, "clock", 8, 4, 4, 2);
}

TEST_CASE("resolver: overlay replaces a named candidate list", "[default_layout_resolver]") {
    const json table = parse(kSlotTable);
    const json overlay =
        parse(R"json({"candidate_lists": {"FIL": ["notifications", "clock"]}})json");
    const auto r = resolve(table, "medium", {{{"filament_sensor_count", 1}}}, 12, 8, &overlay);
    // ENV ends in clock and FIL now leads with notifications, not filament.
    require_cell(r, "clock", 8, 0, 2, 2);
    require_cell(r, "notifications", 10, 0, 2, 2);
    CHECK(placed_entry(r, "filament") == nullptr);
    // ENV is untouched by an overlay that names only FIL.
    CHECK(is_disabled(r, "dryer"));
}

TEST_CASE("resolver: overlay merges into variants", "[default_layout_resolver]") {
    const json table = parse(R"json({
      "anchors": [{"id": "clock", "placements": {"medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 2}}}],
      "variants": {
        "portrait": {"anchors": [
            {"id": "clock", "placements": {"medium": {"col": 0, "row": 6, "colspan": 4, "rowspan": 2}}},
            {"id": "notifications", "placements": {"medium": {"col": 4, "row": 6, "colspan": 2, "rowspan": 2}}}],
          "disabled": {"medium": ["led"]}},
        "ultrawide": [
            {"id": "clock", "placements": {"medium": {"col": 9, "row": 0, "colspan": 4, "rowspan": 2}}},
            {"id": "notifications", "placements": {"medium": {"col": 13, "row": 0, "colspan": 2, "rowspan": 2}}}]
      }})json");
    const std::vector<std::string> portrait{"portrait"};
    const std::vector<std::string> ultrawide{"ultrawide"};

    SECTION("object-form overlay on an object variant: replace, remove, disabled") {
        const json overlay = parse(R"json({"variants": {"portrait": {
            "anchors": [
              {"id": "clock", "placements": {"medium": {"col": 2, "row": 8, "colspan": 4, "rowspan": 2}}},
              {"id": "notifications", "remove": true}],
            "disabled": {"medium": ["tips"]}}}})json");
        const auto r = resolve(table, "medium", {}, 8, 16, &overlay, portrait);
        require_cell(r, "clock", 2, 8, 4, 2);
        CHECK(placed_entry(r, "notifications") == nullptr);
        CHECK(is_disabled(r, "tips"));
        CHECK_FALSE(is_disabled(r, "led"));
    }
    SECTION("bare-array overlay on an object variant") {
        const json overlay = parse(R"json({"variants": {"portrait": [
            {"id": "temperature", "placements": {"medium": {"col": 6, "row": 6, "colspan": 2, "rowspan": 2}}}]}})json");
        const auto r = resolve(table, "medium", {}, 8, 16, &overlay, portrait);
        require_cell(r, "clock", 0, 6, 4, 2);
        require_cell(r, "notifications", 4, 6, 2, 2);
        require_cell(r, "temperature", 6, 6, 2, 2);
        CHECK(is_disabled(r, "led"));
    }
    SECTION("object-form overlay on a bare-array variant") {
        const json overlay = parse(R"json({"variants": {"ultrawide": {
            "anchors": [{"id": "clock", "placements": {"medium": {"col": 20, "row": 0, "colspan": 4, "rowspan": 2}}}],
            "disabled": {"medium": ["tips"]}}}})json");
        const auto r = resolve(table, "medium", {}, 36, 8, &overlay, ultrawide);
        require_cell(r, "clock", 20, 0, 4, 2);
        require_cell(r, "notifications", 13, 0, 2, 2);
        CHECK(is_disabled(r, "tips"));
    }
    SECTION("an overlay can add a variant the base lacks") {
        const json overlay = parse(R"json({"variants": {"tiny_portrait": [
            {"id": "notifications", "placements": {"medium": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}}]}})json");
        const auto r = resolve(table, "medium", {}, 8, 16, &overlay, {"tiny_portrait", "portrait"});
        require_cell(r, "notifications", 0, 0, 2, 2);
        CHECK(placed_entry(r, "clock") == nullptr);
    }
}

TEST_CASE("resolver: variant chain falls through to the base anchors",
          "[default_layout_resolver]") {
    const json table = parse(R"json({
      "anchors": [{"id": "clock", "placements": {"medium": {"col": 0, "row": 0, "colspan": 4, "rowspan": 2}}}],
      "disabled": {"medium": ["tips"]},
      "variants": {
        "portrait": {"anchors": [{"id": "clock", "placements": {"medium": {"col": 2, "row": 6, "colspan": 4, "rowspan": 2}}}],
                     "disabled": {"medium": ["led"]}},
        "ultrawide": [{"id": "clock", "placements": {"medium": {"col": 9, "row": 0, "colspan": 4, "rowspan": 2}}}]
      }})json");
    SECTION("most specific present variant wins, with its own disabled list") {
        const auto r = resolve(table, "medium", {}, 8, 16, nullptr, {"tiny_portrait", "portrait"});
        require_cell(r, "clock", 2, 6, 4, 2);
        CHECK(is_disabled(r, "led"));
        CHECK_FALSE(is_disabled(r, "tips"));
    }
    SECTION("bare-array variant") {
        const auto r = resolve(table, "medium", {}, 36, 8, nullptr, {"ultrawide"});
        require_cell(r, "clock", 9, 0, 4, 2);
    }
    SECTION("no variant matches: base table and base disabled") {
        const auto r = resolve(table, "medium", {}, 12, 8, nullptr, {"tiny_portrait"});
        require_cell(r, "clock", 0, 0, 4, 2);
        CHECK(is_disabled(r, "tips"));
    }
}

TEST_CASE("resolver: signature tracks exactly the consulted subjects",
          "[default_layout_resolver]") {
    const json table = parse(kSlotTable);
    const CapabilitySnapshot base{{{"ams_slot_count", 4}}};
    const auto a = resolve(table, "medium", base);

    CHECK(a.signature.rfind("medium|12|8||", 0) == 0);
    CHECK(a.signature.find("ams_slot_count=4") != std::string::npos);

    SECTION("a consulted subject changing changes it") {
        const auto b = resolve(table, "medium", {{{"ams_slot_count", 0}}});
        CHECK(a.signature != b.signature);
    }
    SECTION("an unconsulted subject changing does not") {
        // With an AMS the ENV/FIL alternative is never read.
        const auto b =
            resolve(table, "medium", {{{"ams_slot_count", 4}, {"home_dryer_available", 1}}});
        CHECK(a.signature == b.signature);
        const auto c = resolve(table, "medium", {{{"ams_slot_count", 4}, {"unrelated", 7}}});
        CHECK(a.signature == c.signature);
    }
    SECTION("tier, grid, variant chain and preset are part of it") {
        CHECK(resolve(table, "medium", base, 14, 8).signature != a.signature);
        CHECK(resolve(table, "medium", base, 12, 8, nullptr, {"portrait"}).signature !=
              a.signature);
        CHECK(resolve(table, "medium", base, 12, 8, nullptr, {}, "ad5x").signature != a.signature);
    }
    SECTION("a table version is appended, and a new one changes it") {
        CHECK(a.signature.find("|table=") == std::string::npos);
        json versioned = table;
        versioned["version"] = 2;
        const auto v2 = resolve(versioned, "medium", base);
        CHECK(v2.signature == a.signature + "|table=2");
        versioned["version"] = 3;
        CHECK(resolve(versioned, "medium", base).signature != v2.signature);
    }
    SECTION("a requires subject is consulted") {
        const CapabilitySnapshot s0{{{"led_controllable", 1}, {"printer_has_spoolman", 0}}};
        const CapabilitySnapshot s1{{{"led_controllable", 1}, {"printer_has_spoolman", 1}}};
        CHECK(resolve(table, "medium", s0).signature != resolve(table, "medium", s1).signature);
    }
}

TEST_CASE("resolver: unknown ids are ignored and malformed tables are not ok",
          "[default_layout_resolver]") {
    SECTION("unknown anchor and unknown candidate") {
        const json table = parse(R"json({"anchors": [
          {"id": "no_such_widget", "placements": {"medium": {"col": 0, "row": 0, "colspan": 2, "rowspan": 2}}},
          {"slot": "s", "placements": {"medium": {"col": 2, "row": 0, "colspan": 2, "rowspan": 2}},
           "alternatives": [{"cells": [{"candidates": ["bogus", "clock"], "at": [0, 0, 2, 2]}]}]}]})json");
        const auto r = resolve(table, "medium", {});
        REQUIRE(r.ok);
        CHECK(placed_entry(r, "no_such_widget") == nullptr);
        CHECK(placed_entry(r, "bogus") == nullptr);
        CHECK_FALSE(is_disabled(r, "bogus"));
        require_cell(r, "clock", 2, 0, 2, 2);
    }
    SECTION("malformed JSON") {
        CHECK_FALSE(resolve(parse("{\"anchors\": [ oops"), "medium", {}).ok);
    }
    SECTION("not an object") {
        CHECK_FALSE(resolve(parse("[1, 2]"), "medium", {}).ok);
    }
    SECTION("anchors not an array") {
        CHECK_FALSE(resolve(parse(R"({"anchors": {"id": "clock"}})"), "medium", {}).ok);
    }
    SECTION("unknown tier") {
        CHECK_FALSE(resolve(parse(kSlotTable), "gigantic", {}).ok);
    }
    SECTION("no anchors for this tier") {
        CHECK_FALSE(resolve(parse(kSlotTable), "large", {}).ok);
    }
}

TEST_CASE("resolver: subjects_named_by lists gates, requires and capability-key subjects",
          "[default_layout_resolver]") {
    const json table = parse(R"json({
      "candidate_lists": {"FIL": ["filament", {"id": "active_spool", "requires": ["printer_has_spoolman"]}, "clock"]},
      "anchors": [
        {"id": "printer_image", "placements": {}},
        {"slot": "s", "placements": {}, "alternatives": [
          {"cells": [{"candidates": ["ams"], "at": [0, 0, 4, 2]}]},
          {"cells": [{"candidates": "FIL", "at": [0, 0, 2, 2]}]}]}],
      "disabled": {"medium+printer_is_enclosed": []},
      "variants": {"portrait": {"anchors": [{"id": "led", "placements": {
                                   "small+printer_has_bed_mesh": {}}}]},
                   "ultrawide": [{"id": "dryer", "placements": {}}]}})json");
    const std::vector<std::string> expected = {
        "ams_slot_count",       "filament_sensor_count", "home_dryer_available", "led_controllable",
        "printer_has_bed_mesh", "printer_has_spoolman",  "printer_is_enclosed"};
    CHECK(helix::layout::subjects_named_by(table) == expected);
    CHECK(helix::layout::subjects_named_by(parse("not json")).empty());
}

TEST_CASE("resolver: arrangement_hash covers the arrangement only", "[default_layout_resolver]") {
    const json pages = parse(R"json({"pages": [{"id": "main", "widgets": [
        {"id": "clock", "enabled": true, "col": 0, "row": 0, "colspan": 4, "rowspan": 2},
        {"id": "led", "enabled": true, "col": 4, "row": 0, "colspan": 2, "rowspan": 2,
         "config": {"strip": "a"}},
        {"id": "plugin_thing", "enabled": true, "col": 6, "row": 0, "colspan": 2, "rowspan": 2}
      ]}], "main_page_index": 0, "next_page_id": 1})json");
    const std::string h = helix::layout::arrangement_hash(pages);
    CHECK_FALSE(h.empty());

    SECTION("config edits do not change it") {
        json p = pages;
        p["pages"][0]["widgets"][1]["config"]["strip"] = "b";
        p["pages"][0]["widgets"][0]["config"] = {{"x", 1}};
        CHECK(helix::layout::arrangement_hash(p) == h);
    }
    SECTION("an appended enabled but unplaced entry does not change it") {
        json p = pages;
        p["pages"][0]["widgets"].push_back({{"id", "fan_stack"},
                                            {"enabled", true},
                                            {"col", -1},
                                            {"row", -1},
                                            {"colspan", 2},
                                            {"rowspan", 2}});
        p["pages"][0]["widgets"].push_back({{"id", "macros"}, {"enabled", true}});
        CHECK(helix::layout::arrangement_hash(p) == h);
    }
    SECTION("a disabled entry with a stale position does not change it") {
        json p = pages;
        p["pages"][0]["widgets"].push_back({{"id", "motion"},
                                            {"enabled", false},
                                            {"col", 8},
                                            {"row", 0},
                                            {"colspan", 2},
                                            {"rowspan", 2}});
        CHECK(helix::layout::arrangement_hash(p) == h);
    }
    SECTION("an unplaced plugin id whose definition registers later does not change it") {
        json p = pages;
        p["pages"][0]["widgets"].push_back({{"id", "resolver_test_plugin"}, {"enabled", true}});
        REQUIRE(helix::find_widget_def("resolver_test_plugin") == nullptr);
        const std::string before = helix::layout::arrangement_hash(p);
        helix::RuntimeWidgetDef def;
        def.id = "resolver_test_plugin";
        def.display_name = "Resolver Test";
        helix::ScopeExit cleanup(
            [] { helix::unregister_runtime_widget_def("resolver_test_plugin"); });
        REQUIRE(helix::register_runtime_widget_def(def));
        REQUIRE(helix::find_widget_def("resolver_test_plugin") != nullptr);
        const std::string after = helix::layout::arrangement_hash(p);
        CHECK(before == h);
        CHECK(after == before);
    }
    SECTION("a placed plugin entry is part of it") {
        json p = pages;
        p["pages"][0]["widgets"][2]["col"] = 9;
        CHECK(helix::layout::arrangement_hash(p) != h);
    }
    SECTION("a move changes it") {
        json p = pages;
        p["pages"][0]["widgets"][0]["row"] = 2;
        CHECK(helix::layout::arrangement_hash(p) != h);
    }
    SECTION("a resize changes it") {
        json p = pages;
        p["pages"][0]["widgets"][1]["colspan"] = 4;
        CHECK(helix::layout::arrangement_hash(p) != h);
    }
    SECTION("an enable flip changes it") {
        json p = pages;
        p["pages"][0]["widgets"][1]["enabled"] = false;
        CHECK(helix::layout::arrangement_hash(p) != h);
    }
    SECTION("a page added changes it") {
        json p = pages;
        p["pages"].push_back({{"id", "page_1"}, {"widgets", json::array()}});
        CHECK(helix::layout::arrangement_hash(p) != h);
    }
    SECTION("a widget moved to another page changes it") {
        json p = pages;
        p["pages"].push_back({{"id", "page_1"}, {"widgets", json::array()}});
        const json moved = p["pages"][0]["widgets"][1];
        json q = p;
        q["pages"][0]["widgets"].erase(1);
        q["pages"][1]["widgets"].push_back(moved);
        CHECK(helix::layout::arrangement_hash(q) != helix::layout::arrangement_hash(p));
    }
}
