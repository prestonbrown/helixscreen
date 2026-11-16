// tests/unit/test_snapmaker_print_preferences.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "snapmaker_print_preferences.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::snapmaker;

namespace {
nlohmann::json ptc(const nlohmann::json& fields) {
    nlohmann::json s = nlohmann::json::object();
    s["print_task_config"] = fields;
    return s;
}
} // namespace

TEST_CASE("snapmaker prefs: every field reads out of a full frame", "[snapmaker][prefs]") {
    auto p = read_print_preferences(ptc({
        {"auto_replenish_filament", true},
        {"replenish_ignore_color", false},
        {"filament_entangle_detect", true},
        {"filament_entangle_sen", "medium"},
        {"end_led_turn_off", true},
        {"end_unload_filament", {false, true, false, true}},
    }));
    REQUIRE(p.auto_replenish.value() == true);
    REQUIRE(p.replenish_ignore_color.value() == false);
    REQUIRE(p.filament_entangle_detect.value() == true);
    REQUIRE(p.filament_entangle_sen.value() == "medium");
    REQUIRE(p.end_led_turn_off.value() == true);
    REQUIRE(p.end_unload_filament == std::vector<bool>{false, true, false, true});
}

TEST_CASE("snapmaker prefs: the per-head soft-filament flags read and survive a delta",
          "[snapmaker][prefs][end_unload]") {
    // The end macro never unloads a soft-filament head, whatever the list says.
    auto held = read_print_preferences(ptc({{"filament_soft", {false, true, 0, 1}}}));
    REQUIRE(held.filament_soft == std::vector<bool>{false, true, false, true});
    // A flag about the loaded filament is not a setting to offer.
    REQUIRE(held.empty());

    held.merge(read_print_preferences(ptc({{"end_led_turn_off", true}})));
    REQUIRE(held.filament_soft == std::vector<bool>{false, true, false, true});
    held.merge(read_print_preferences(ptc({{"filament_soft", {true, false, false, false}}})));
    REQUIRE(held.filament_soft == std::vector<bool>{true, false, false, false});

    // Read-only: the write never carries it.
    PrintPreferences changes;
    changes.filament_soft = {true};
    REQUIRE(write_print_preferences_gcode(changes).empty());
}

TEST_CASE("snapmaker prefs: a field the frame omits stays nullopt", "[snapmaker][prefs]") {
    // Moonraker sends deltas. Silence is not an off.
    auto p = read_print_preferences(ptc({{"end_led_turn_off", true}}));
    REQUIRE(p.end_led_turn_off.value() == true);
    REQUIRE_FALSE(p.auto_replenish.has_value());
    REQUIRE_FALSE(p.filament_entangle_sen.has_value());
    REQUIRE(p.end_unload_filament.empty());
}

TEST_CASE("snapmaker prefs: a frame without print_task_config yields nothing",
          "[snapmaker][prefs]") {
    nlohmann::json other = nlohmann::json::object();
    other["toolhead"] = {{"homed_axes", "xyz"}};
    auto p = read_print_preferences(other);
    REQUIRE_FALSE(p.auto_replenish.has_value());
}

TEST_CASE("snapmaker prefs: integer and boolean spellings both read", "[snapmaker][prefs]") {
    auto p = read_print_preferences(
        ptc({{"auto_replenish_filament", 1}, {"filament_entangle_detect", 0}}));
    REQUIRE(p.auto_replenish.value() == true);
    REQUIRE(p.filament_entangle_detect.value() == false);
}

TEST_CASE("snapmaker prefs: a nested reprint_info copy is never read", "[snapmaker][prefs]") {
    // The live print_task_config frame carries a nested reprint_info object
    // with its own same-named fields. The stored preferences are the top-level
    // ones, so a lookup that reached into reprint_info would pick up the other
    // object's values.
    const nlohmann::json reprint_info = {
        {"end_unload_filament", {true, true, true, true}},
        {"auto_bed_leveling", true},
        {"flow_calibrate", false},
        {"time_lapse_camera", true},
        {"extruders_used", {0, 1, 2, 3}},
    };
    auto both = read_print_preferences(ptc({
        {"auto_replenish_filament", true},
        {"end_unload_filament", {false, false, false, false}},
        {"reprint_info", reprint_info},
    }));
    REQUIRE(both.auto_replenish.value() == true);
    REQUIRE(both.end_unload_filament == std::vector<bool>{false, false, false, false});

    auto nested_only = read_print_preferences(ptc({{"reprint_info", reprint_info}}));
    REQUIRE(nested_only.end_unload_filament.empty());
    REQUIRE_FALSE(nested_only.auto_replenish.has_value());
}

TEST_CASE("snapmaker prefs: the write sends ONLY what changed", "[snapmaker][prefs]") {
    // SET_PRINT_PREFERENCES is a setter: an omitted parameter keeps its stored
    // value, so sending untouched fields would rewrite settings the user did
    // not ask to change.
    PrintPreferences changes;
    changes.filament_entangle_detect = true;
    const std::string g = write_print_preferences_gcode(changes);
    REQUIRE(g == "SET_PRINT_PREFERENCES FILAMENT_ENTANGLE_DETECT=1");
}

TEST_CASE("snapmaker prefs: booleans render as 1/0 and the enum renders bare",
          "[snapmaker][prefs]") {
    PrintPreferences changes;
    changes.auto_replenish = false;
    changes.filament_entangle_sen = "high";
    const std::string g = write_print_preferences_gcode(changes);
    REQUIRE(g.find("AUTO_REPLENISH_FILAMENT=0") != std::string::npos);
    REQUIRE(g.find("FILAMENT_ENTANGLE_SEN=high") != std::string::npos);
}

TEST_CASE("snapmaker prefs: the per-tool array renders as a Python list literal",
          "[snapmaker][prefs]") {
    PrintPreferences changes;
    changes.end_unload_filament = {true, false, true, false};
    REQUIRE(write_print_preferences_gcode(changes) ==
            "SET_PRINT_PREFERENCES END_UNLOAD_FILAMENT=[1,0,1,0]");
}

TEST_CASE("snapmaker prefs: nothing to change renders an empty string", "[snapmaker][prefs]") {
    // The caller must be able to tell "no write needed" from "write this",
    // rather than sending a bare command that sets nothing.
    REQUIRE(write_print_preferences_gcode(PrintPreferences{}).empty());
}

TEST_CASE("snapmaker prefs: merge keeps a held field the incoming frame omits",
          "[snapmaker][prefs]") {
    PrintPreferences held;
    held.auto_replenish = true;
    held.filament_entangle_sen = "low";
    held.end_unload_filament = {true, false};

    PrintPreferences incoming;
    incoming.end_led_turn_off = false;
    incoming.filament_entangle_sen = "high";
    held.merge(incoming);

    REQUIRE(held.auto_replenish.value() == true);
    REQUIRE(held.end_led_turn_off.value() == false);
    REQUIRE(held.filament_entangle_sen.value() == "high");
    REQUIRE(held.end_unload_filament == std::vector<bool>{true, false});

    PrintPreferences replacement;
    replacement.end_unload_filament = {false};
    held.merge(replacement);
    REQUIRE(held.end_unload_filament == std::vector<bool>{false});
}
