// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_toolhead_preset_slots.cpp
 * @brief The Toolhead widget's preset slots in its config: the defaults, the round trip,
 * and what a malformed entry becomes.
 *
 * Run with: ./build/bin/helix-tests "[toolhead][presets]"
 */

#include "toolhead_preset_config_modal.h"
#include "toolhead_preset_slots.h"

#include <algorithm>
#include <set>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using nlohmann::json;
using Kind = ToolheadPresetSlot::Kind;

namespace {

ToolheadPresetSlot position(MotionPreset p) {
    ToolheadPresetSlot slot;
    slot.kind = Kind::Position;
    slot.position = p;
    return slot;
}

ToolheadPresetSlot of_kind(Kind kind, std::string macro = {}) {
    ToolheadPresetSlot slot;
    slot.kind = kind;
    slot.macro = std::move(macro);
    return slot;
}

void check_same(const ToolheadPresetSlot& a, const ToolheadPresetSlot& b) {
    CHECK(a.kind == b.kind);
    if (a.kind == Kind::Position && b.kind == Kind::Position)
        CHECK(a.position == b.position);
    CHECK(a.macro == b.macro);
}

constexpr MotionPreset kAllPresets[] = {
    MotionPreset::RearLeft,  MotionPreset::Rear,   MotionPreset::RearRight,
    MotionPreset::Left,      MotionPreset::Center, MotionPreset::Right,
    MotionPreset::FrontLeft, MotionPreset::Front,  MotionPreset::FrontRight,
};

} // namespace

TEST_CASE("toolhead presets: the defaults are Front, Center, Park", "[toolhead][presets]") {
    const auto slots = default_toolhead_preset_slots();
    check_same(slots[0], position(MotionPreset::Front));
    check_same(slots[1], position(MotionPreset::Center));
    check_same(slots[2], of_kind(Kind::Park));
}

TEST_CASE("toolhead presets: a missing or malformed array gives the defaults",
          "[toolhead][presets]") {
    const auto defaults = default_toolhead_preset_slots();
    for (const json& config :
         {json::object(), json{{"presets", "front"}}, json{{"other", 1}}, json::array()}) {
        INFO(config.dump());
        const auto slots = toolhead_preset_slots_from_config(config);
        for (std::size_t i = 0; i < slots.size(); ++i)
            check_same(slots[i], defaults[i]);
    }
}

TEST_CASE("toolhead presets: a JSON round trip keeps every kind", "[toolhead][presets]") {
    const std::vector<ToolheadPresetSlots> cases = {
        {position(MotionPreset::RearRight), of_kind(Kind::ZUp), of_kind(Kind::ZDown)},
        {of_kind(Kind::Macro, "CLEAN_NOZZLE"), of_kind(Kind::Empty), of_kind(Kind::Park)},
        {position(MotionPreset::FrontLeft), position(MotionPreset::Left),
         of_kind(Kind::Macro, "PURGE")},
        {of_kind(Kind::DockTool), position(MotionPreset::Center), of_kind(Kind::DockTool)},
    };
    for (const auto& slots : cases) {
        json config = {{"keep", true}};
        toolhead_preset_slots_to_config(slots, config);
        INFO(config.dump());
        CHECK(config["keep"] == true);
        const auto back = toolhead_preset_slots_from_config(config);
        for (std::size_t i = 0; i < slots.size(); ++i)
            check_same(back[i], slots[i]);
    }
}

TEST_CASE("toolhead presets: a bad entry becomes Empty and keeps its place",
          "[toolhead][presets]") {
    const json config = {{"presets", json::array({
                                         json{{"kind", "teleport"}},
                                         json{{"kind", "position"}, {"position", "upstairs"}},
                                         json{{"kind", "park"}},
                                     })}};
    const auto slots = toolhead_preset_slots_from_config(config);
    CHECK(slots[0].kind == Kind::Empty);
    CHECK(slots[1].kind == Kind::Empty);
    CHECK(slots[2].kind == Kind::Park);

    // A macro slot with no name is nothing to run.
    const auto unnamed = toolhead_preset_slots_from_config(
        json{{"presets", json::array({json{{"kind", "macro"}}, 5, json{{"kind", 3}}})}});
    CHECK(unnamed[0].kind == Kind::Empty);
    CHECK(unnamed[1].kind == Kind::Empty);
    CHECK(unnamed[2].kind == Kind::Empty);
}

TEST_CASE("toolhead presets: every position token round-trips", "[toolhead][presets]") {
    std::set<std::string> tokens;
    for (const MotionPreset p : kAllPresets) {
        const std::string token = motion_preset_token(p);
        INFO(token);
        tokens.insert(token);
        const auto back = motion_preset_from_token(token);
        REQUIRE(back.has_value());
        CHECK(*back == p);
    }
    CHECK(tokens.size() == std::size(kAllPresets));
    CHECK_FALSE(motion_preset_from_token("").has_value());
    CHECK_FALSE(motion_preset_from_token("Front").has_value());
}

TEST_CASE("toolhead presets: only the macro and empty slots leave the toolhead alone",
          "[toolhead][presets]") {
    CHECK(toolhead_slot_moves_toolhead(position(MotionPreset::Center)));
    CHECK(toolhead_slot_moves_toolhead(of_kind(Kind::Park)));
    CHECK(toolhead_slot_moves_toolhead(of_kind(Kind::ZUp)));
    CHECK(toolhead_slot_moves_toolhead(of_kind(Kind::ZDown)));
    CHECK_FALSE(toolhead_slot_moves_toolhead(of_kind(Kind::Macro, "X")));
    CHECK_FALSE(toolhead_slot_moves_toolhead(of_kind(Kind::Empty)));
}

TEST_CASE("toolhead presets: the third default is Dock tool only where a tool can dock",
          "[toolhead][presets][dock]") {
    check_same(default_toolhead_preset_slots(true)[2], of_kind(Kind::DockTool));
    check_same(default_toolhead_preset_slots(false)[2], of_kind(Kind::Park));
    check_same(default_toolhead_preset_slots()[2], of_kind(Kind::Park));
    // The other two slots do not change with it.
    check_same(default_toolhead_preset_slots(true)[0], position(MotionPreset::Front));
    check_same(default_toolhead_preset_slots(true)[1], position(MotionPreset::Center));

    // An unsaved config resolves per call, so it follows the machine once it is known.
    CHECK(toolhead_preset_slots_from_config(json::object(), true)[2].kind == Kind::DockTool);
    CHECK(toolhead_preset_slots_from_config(json::object(), false)[2].kind == Kind::Park);
}

TEST_CASE("toolhead presets: a saved layout is never changed by tool parking",
          "[toolhead][presets][dock]") {
    json park_saved = json::object();
    toolhead_preset_slots_to_config(
        {position(MotionPreset::Front), position(MotionPreset::Center), of_kind(Kind::Park)},
        park_saved);
    json dock_saved = json::object();
    toolhead_preset_slots_to_config(
        {position(MotionPreset::Front), position(MotionPreset::Center), of_kind(Kind::DockTool)},
        dock_saved);
    REQUIRE(toolhead_preset_slots_saved(park_saved));
    REQUIRE(toolhead_preset_slots_saved(dock_saved));
    CHECK_FALSE(toolhead_preset_slots_saved(json::object()));
    CHECK_FALSE(toolhead_preset_slots_saved(json{{"presets", "front"}}));

    CHECK(toolhead_preset_slots_from_config(park_saved, true)[2].kind == Kind::Park);
    CHECK(toolhead_preset_slots_from_config(dock_saved, false)[2].kind == Kind::DockTool);
    // A saved Dock tool keeps its token on disk.
    CHECK(dock_saved["presets"][2]["kind"] == "tool_park");
}

TEST_CASE("toolhead presets: Dock tool reads and draws differently from Park",
          "[toolhead][presets][dock]") {
    const auto dock = of_kind(Kind::DockTool);
    const auto park = of_kind(Kind::Park);
    CHECK(toolhead_preset_slot_label(dock) == "Dock tool");
    CHECK(toolhead_preset_slot_label(dock) != toolhead_preset_slot_label(park));
    CHECK(std::string(toolhead_preset_slot_icon(dock)) == "eject");
    CHECK(std::string(toolhead_preset_slot_icon(dock)) != toolhead_preset_slot_icon(park));
    CHECK_FALSE(toolhead_slot_moves_toolhead(dock));
}

TEST_CASE("toolhead presets: the picker offers Dock tool only where a tool can dock",
          "[toolhead][presets][dock]") {
    const auto count = [](const std::vector<ToolheadPresetSlot>& all, Kind kind) {
        return std::count_if(all.begin(), all.end(),
                             [kind](const ToolheadPresetSlot& s) { return s.kind == kind; });
    };
    const auto with = ToolheadPresetConfigModal::choices(true);
    const auto without = ToolheadPresetConfigModal::choices(false);
    CHECK(count(with, Kind::DockTool) == 1);
    CHECK(count(without, Kind::DockTool) == 0);
    // Plain Park stays on offer either way.
    CHECK(count(with, Kind::Park) == 1);
    CHECK(count(without, Kind::Park) == 1);
    CHECK(with.size() == without.size() + 1);
}
