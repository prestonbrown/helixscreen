// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "hv/json.hpp"

/// oams_manager status objects for OpenAMS tests: the backend's cases feed
/// them through handle_status, the parser's read them directly.
namespace openams_json {

using json = nlohmann::json;

inline json all_commands() {
    return json{{"load", "OPENAMS_LOAD"},
                {"unload", "OPENAMS_UNLOAD"},
                {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                {"reset", "OAMSM_CLEAR_ERRORS"}};
}

inline json lane(const std::string& state, const json& group = nullptr,
                 const json& slot = nullptr) {
    return json{{"id", "fps"},          {"state", state},     {"current_group", group},
                {"current_slot", slot}, {"following", false}, {"direction", 0},
                {"message", nullptr}};
}

inline json slot(int id, int bay, bool ready, bool loaded = false) {
    return json{{"id", id}, {"bay", bay}, {"ready", ready}, {"loaded", loaded}};
}

/// One four-bay hub unit on lane `fps`, slot ids 0-3. T0 owns bays 0 and 1,
/// T1 bay 2, T2 bay 3. Bay 0 is empty, the rest hold a spool, nothing loaded.
inline json manager(json lanes = json::array({lane("unloaded")}), json commands = all_commands()) {
    return json{{"api_version", 1},
                {"schema", "openams.manager"},
                {"ready", true},
                {"commands", std::move(commands)},
                {"lanes", std::move(lanes)},
                {"units",
                 json::array({json{{"id", "1"},
                                   {"name", "unit1"},
                                   {"kind", "oams"},
                                   {"topology", "hub"},
                                   {"lane", "fps"},
                                   {"connected", true},
                                   {"slots", json::array({slot(0, 0, false), slot(1, 1, true),
                                                          slot(2, 2, true), slot(3, 3, true)})}}})},
                {"groups", json::array({json{{"name", "T0"}, {"lane", "fps"}, {"slots", {0, 1}}},
                                        json{{"name", "T1"}, {"lane", "fps"}, {"slots", {2}}},
                                        json{{"name", "T2"}, {"lane", "fps"}, {"slots", {3}}}})},
                {"current_group", nullptr}};
}

/// manager() with slot 2 (T1) loaded.
inline json loaded_manager(json commands = all_commands()) {
    json m = manager(json::array({lane("loaded", "T1", 2)}), std::move(commands));
    m["units"][0]["slots"][2]["loaded"] = true;
    return m;
}

inline json toolhead_commands() {
    return json{{"load", "OAMSM_LOAD_TO_TOOLHEAD"},
                {"unload", "OAMSM_UNLOAD_FROM_TOOLHEAD"},
                {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                {"reset", "OAMSM_CLEAR_ERRORS"}};
}

/// The core every manager publishes. @p loaded is the global slot on lane fps,
/// -1 for none. @p openams_extras adds what only the openams plugin publishes.
inline json shared_manager(int loaded = -1, json commands = all_commands(),
                           bool openams_extras = false) {
    json l = lane(loaded >= 0 ? "loaded" : "unloaded",
                  loaded >= 0 ? json("T" + std::to_string(loaded)) : json(),
                  loaded >= 0 ? json(loaded) : json());
    l["pressure"] = 0.79;
    l["set_point"] = 0.5;
    json groups = json::array();
    for (int i = 0; i < 5; ++i) {
        groups.push_back(json{{"name", "T" + std::to_string(i)}, {"lane", "fps"}, {"slots", {i}}});
    }
    json m{{"api_version", 1},
           {"schema", "openams.manager"},
           {"ready", true},
           {"commands", std::move(commands)},
           {"lanes", json::array({l})},
           {"units", json::array({json{{"id", "1"},
                                       {"name", "ams_ht"},
                                       {"kind", "oams"},
                                       {"topology", "hub"},
                                       {"lane", "fps"},
                                       {"connected", true},
                                       {"slots", json::array({slot(0, 0, true, loaded == 0)})}},
                                  json{{"id", "2"},
                                       {"name", "ams2"},
                                       {"kind", "oams"},
                                       {"topology", "hub"},
                                       {"lane", "fps"},
                                       {"connected", true},
                                       {"slots", json::array({slot(1, 0, true, loaded == 1),
                                                              slot(2, 1, true, loaded == 2),
                                                              slot(3, 2, true, loaded == 3),
                                                              slot(4, 3, true, loaded == 4)})}}})},
           {"groups", groups}};
    if (openams_extras) {
        m["lanes_by_fps"] = {{"fps", {{"op", "loaded"}, {"extruder", "extruder"}}}};
        m["topology"] = {{"schema_version", 1}, {"fps", {"fps"}}};
    }
    return m;
}

inline json fault_entry(const std::string& code, const std::string& text, json actions,
                        const std::string& severity = "stop", json bay = nullptr) {
    return json{{"severity", severity}, {"code", code},       {"text", text},
                {"bay", bay},           {"actions", actions}, {"source", "firmware"}};
}

/// shared_manager() with `devices.<unit>.faults` set.
inline json faulted_manager(const json& ams_ht_faults, const json& ams2_faults = json::array(),
                            json commands = toolhead_commands()) {
    json m = shared_manager(-1, std::move(commands), true);
    m["devices"] = {{"ams_ht", {{"faults", ams_ht_faults}}}, {"ams2", {{"faults", ams2_faults}}}};
    return m;
}

inline json dryer_device(bool ht, const std::string& state = "off", double target = 0.0,
                         int remaining_s = 0, bool offer = true) {
    json actions = json::array({"load", "unload"});
    if (offer) {
        // A running cycle withdraws dryer_start.
        if (remaining_s == 0) {
            actions.push_back("dryer_start");
        }
        actions.push_back("dryer_stop");
    }
    return json{
        {"capabilities",
         {{"dryer", true},
          {"dryer_target_min_c", 45.0},
          {"dryer_target_max_c", ht ? 80.0 : 65.0},
          {"dryer_requires_unloaded", !ht}}},
        {"environment", {{"temp_c", ht ? 26.1 : 28.2}, {"rh_pct", ht ? 29.0 : 31.0}}},
        {"dryer",
         {{"state", state}, {"target_c", target}, {"remaining_s", remaining_s}, {"fan_pct", 60.0}}},
        {"telemetry", {{"dryer", {{"chamber_c", 51.5}}}}},
        {"supported_actions", actions}};
}

inline json dryer_manager(int loaded = -1, json ht = dryer_device(true),
                          json ams2 = dryer_device(false)) {
    json m = shared_manager(loaded, toolhead_commands(), true);
    m["devices"] = {{"ams_ht", std::move(ht)}, {"ams2", std::move(ams2)}};
    return m;
}

} // namespace openams_json
