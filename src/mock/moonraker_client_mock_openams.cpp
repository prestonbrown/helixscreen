// SPDX-License-Identifier: GPL-3.0-or-later

// OpenAMS mock hardware (HELIX_MOCK_AMS=openams): the status the openams plugin
// publishes, in its single-unit and multi-unit shapes (HELIX_MOCK_OPENAMS_UNITS),
// and the dryers behind it, so real discovery builds the production backend under
// --test.

#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

using namespace helix;
using mock_internal::effective_mock_ams_env;

bool MoonrakerClientMock::is_mock_openams() const {
    return effective_mock_ams_env() == "openams";
}

namespace {

// One unit of the openams plugin shape: its hub lane and what its family is.
struct OpenAmsUnitSpec {
    const char* name;
    const char* display_name;
    const char* family;
    const char* lane;
    int bays;
    double dryer_max_c;
    bool dryer_requires_unloaded;
};

constexpr OpenAmsUnitSpec kAmsHt{"ams_ht", "AMS HT", "ams_ht", "fps", 1, 80.0, false};
constexpr OpenAmsUnitSpec kAms2{"ams2", "AMS 2 Pro", "ams2", "fps", 4, 65.0, true};
constexpr OpenAmsUnitSpec kAms2b{"ams2b", "AMS 2 Pro", "ams2", "fps2", 4, 65.0, true};
constexpr OpenAmsUnitSpec kAms2c{"ams2c", "AMS 2 Pro", "ams2", "fps2", 4, 65.0, true};

std::vector<OpenAmsUnitSpec> openams_unit_specs() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    const std::string shape = units ? units : "";
    if (shape == "two_lanes") {
        return {kAmsHt, kAms2, kAms2b, kAms2c};
    }
    if (shape == "shared") {
        return {kAmsHt, kAms2};
    }
    if (shape == "fleet") {
        // An AMS HT then eleven AMS 2 Pro: units 1-10 feed lane `fps`, the last two `fps2`.
        static constexpr const char* kNames[] = {"ams_ht", "ams2_1", "ams2_2",  "ams2_3",
                                                 "ams2_4", "ams2_5", "ams2_6",  "ams2_7",
                                                 "ams2_8", "ams2_9", "ams2_10", "ams2_11"};
        std::vector<OpenAmsUnitSpec> fleet = {kAmsHt};
        for (std::size_t i = 1; i < std::size(kNames); ++i) {
            OpenAmsUnitSpec spec = kAms2;
            spec.name = kNames[i];
            spec.lane = i < 10 ? "fps" : "fps2";
            fleet.push_back(spec);
        }
        return fleet;
    }
    return {};
}

// The extruder each lane feeds; the lane's position in the plugin's lane list.
const char* openams_lane_extruder(const std::string& lane) {
    return lane == "fps2" ? "extruder1" : "extruder";
}

} // namespace

bool MoonrakerClientMock::openams_shared_lane_units() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    return units && std::string(units) == "shared";
}

bool MoonrakerClientMock::openams_fleet_lane_units() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    return units && std::string(units) == "fleet";
}

double MoonrakerClientMock::openams_ambient_c(int unit) {
    return openams_fleet_lane_units() ? 24.0 + 1.5 * (unit % 5) : 27.7;
}

bool MoonrakerClientMock::openams_two_lane_units() {
    const char* units = std::getenv("HELIX_MOCK_OPENAMS_UNITS");
    return units && std::string(units) == "two_lanes";
}

bool MoonrakerClientMock::openams_plugin_units() {
    return openams_shared_lane_units() || openams_two_lane_units() || openams_fleet_lane_units();
}

int MoonrakerClientMock::openams_unit_count() {
    return static_cast<int>(openams_unit_specs().size());
}

int MoonrakerClientMock::openams_slot_count() {
    int slots = 0;
    for (const auto& spec : openams_unit_specs()) {
        slots += spec.bays;
    }
    return slots;
}

nlohmann::json MoonrakerClientMock::openams_shared_status_json() const {
    // Units spread over one or two hub lanes (an AMS HT and AMS 2 Pro on `fps`, and
    // with two_lanes two AMS 2 Pro on `fps2`): each lane is one hub, one FPS and one
    // extruder. Groups T0-Tn name a slot each; they are filament groups, not
    // toolheads.
    const auto specs = openams_unit_specs();
    const int loaded = openams_loaded_slot_.load();

    std::vector<std::string> lane_ids;
    nlohmann::json units = nlohmann::json::array();
    nlohmann::json groups = nlohmann::json::array();
    std::vector<std::string> slot_lane;
    int next_slot = 0;
    for (std::size_t u = 0; u < specs.size(); ++u) {
        const auto& spec = specs[u];
        if (std::find(lane_ids.begin(), lane_ids.end(), spec.lane) == lane_ids.end()) {
            lane_ids.push_back(spec.lane);
        }
        nlohmann::json slots = nlohmann::json::array();
        for (int b = 0; b < spec.bays; ++b) {
            const int id = next_slot++;
            slots.push_back({{"id", id}, {"bay", b}, {"ready", true}, {"loaded", id == loaded}});
            slot_lane.push_back(spec.lane);
            const std::string name =
                "T" + std::to_string(id); // DISPLAY_NUMBERING_OK: OpenAMS group id
            groups.push_back({{"name", name}, {"lane", spec.lane}, {"slots", {id}}});
        }
        units.push_back({{"id", std::to_string(u + 1)},
                         {"name", spec.name},
                         {"kind", "oams"},
                         {"topology", "hub"},
                         {"lane", spec.lane},
                         {"connected", true},
                         {"slots", slots}});
    }

    const bool loaded_valid = loaded >= 0 && loaded < static_cast<int>(slot_lane.size());
    nlohmann::json lanes = nlohmann::json::array();
    for (const auto& id : lane_ids) {
        const bool here = loaded_valid && slot_lane[loaded] == id;
        nlohmann::json lane = {{"id", id},          {"state", here ? "loaded" : "unloaded"},
                               {"following", here}, {"direction", 1},
                               {"pressure", 0.79},  {"set_point", 0.5}};
        if (here) {
            lane["current_group"] =
                "T" + std::to_string(loaded); // DISPLAY_NUMBERING_OK: OpenAMS group id
            lane["current_slot"] = loaded;
        } else {
            lane["current_group"] = nullptr;
            lane["current_slot"] = -1;
        }
        lanes.push_back(lane);
    }
    nlohmann::json status = {{"api_version", 1}, {"schema", "openams.manager"},
                             {"ready", true},    {"lanes", lanes},
                             {"units", units},   {"groups", groups}};
    // klipper_openams publishes nothing beyond the versioned core above and
    // advertises its macros; the openams plugin adds per-lane and topology views
    // and advertises the manager commands the Mainsail panel runs.
    const char* api = std::getenv("HELIX_MOCK_OPENAMS_API");
    if (api && std::string(api) == "legacy") {
        status["commands"] = {{"load", "OPENAMS_LOAD"},
                              {"unload", "OPENAMS_UNLOAD"},
                              {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                              {"reset", "OAMSM_CLEAR_ERRORS"}};
    } else {
        status["commands"] = {{"load", "OAMSM_LOAD_TO_TOOLHEAD"},
                              {"unload", "OAMSM_UNLOAD_FROM_TOOLHEAD"},
                              {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
                              {"reset", "OAMSM_CLEAR_ERRORS"}};
        status["devices"] = nlohmann::json::object();
        for (size_t i = 0; i < specs.size(); ++i) {
            nlohmann::json device = openams_device_json(static_cast<int>(i));
            device["faults"] = nlohmann::json::array();
            if (i == 0 && openams_fault_active_.load()) {
                const char* code = std::getenv("HELIX_MOCK_OPENAMS_FAULT");
                device["faults"].push_back({{"severity", "stop"},
                                            {"code", code && *code ? code : "motor_drive_fault"},
                                            {"text", code && *code ? code : "motor_drive_fault"},
                                            {"bay", nullptr},
                                            {"actions", {"clear_fault"}},
                                            {"source", "firmware"}});
            }
            status["devices"][specs[i].name] = device;
        }
        status["devices"]["ams_ht"].update(openams_device_json(0));
        for (std::size_t u = 1; u < specs.size(); ++u) {
            status["devices"][specs[u].name] = openams_device_json(static_cast<int>(u));
            if (!status["devices"][specs[u].name].contains("faults")) {
                status["devices"][specs[u].name]["faults"] = nlohmann::json::array();
            }
        }
        nlohmann::json by_fps = nlohmann::json::object();
        nlohmann::json fps_ids = nlohmann::json::array();
        for (const auto& id : lane_ids) {
            const bool here = loaded_valid && slot_lane[loaded] == id;
            by_fps[id] = {{"op", here ? "loaded" : "idle"},
                          {"pressure", 0.79},
                          {"set_point", 0.5},
                          {"extruder", openams_lane_extruder(id)}};
            fps_ids.push_back(id);
        }
        status["lanes_by_fps"] = by_fps;
        nlohmann::json oams = nlohmann::json::object();
        for (std::size_t u = 0; u < specs.size(); ++u) {
            oams[specs[u].name] = {
                {"idx", static_cast<int>(u) + 1}, {"lane", specs[u].lane}, {"bays", specs[u].bays}};
        }
        status["topology"] = {{"schema_version", 1}, {"fps", fps_ids}, {"oams", oams}};
    }
    return status;
}

nlohmann::json MoonrakerClientMock::openams_device_json(int unit) const {
    // What the openams plugin publishes per unit: capabilities, the environment
    // sensor, the dryer and the actions the unit advertises. An AMS 2 Pro's dryer
    // needs every bay unloaded and tops out at 65 C.
    const auto specs = openams_unit_specs();
    const OpenAmsUnitSpec& spec = specs.at(static_cast<std::size_t>(unit));
    const bool ht = unit == 0;
    const OpenAmsDryerSim& sim = openams_dryers_[unit];
    const double target = sim.target_c.load();
    const double remaining = sim.remaining_s.load();
    const double chamber = sim.chamber_c.load();
    const bool running = remaining > 0.0 && target > 0.0;
    const char* state = !running ? "off" : (chamber < target - 1.0 ? "heating" : "holding");
    const int bays = spec.bays;
    nlohmann::json device = {
        {"oams_idx", unit + 1},
        {"connected", true},
        {"capabilities",
         {{"family", spec.family},
          {"display_name", spec.display_name},
          {"bays", bays},
          {"dryer", true},
          {"dryer_target_min_c", 45.0},
          {"dryer_target_max_c", spec.dryer_max_c},
          {"dryer_requires_unloaded", spec.dryer_requires_unloaded},
          {"heater_count", 1},
          {"fan_count", 1}}},
        {"environment",
         {{"temp_c", std::round(chamber * 10.0) / 10.0},
          {"rh_pct", running                      ? 18.0
                     : openams_fleet_lane_units() ? 22.0 + (unit * 7) % 30
                                                  : (ht ? 29.0 : 31.0)},
          {"source", "firmware"}}},
        {"dryer",
         {{"state", state},
          {"target_c", running ? target : 0.0},
          {"remaining_s", running ? static_cast<int>(remaining) : 0},
          {"heater_pct", !running ? 0.0 : (chamber < target - 1.0 ? 100.0 : 35.0)},
          {"fan_pct", running ? 60.0 : 0.0},
          {"fault", "none"},
          {"adapter", nullptr}}},
        {"telemetry",
         {{"dryer",
           {{"state_name", state},
            {"target_c", running ? target : 0.0},
            {"chamber_c", std::round(chamber * 10.0) / 10.0},
            {"remaining_s", running ? static_cast<int>(remaining) : 0}}}}},
        {"supported_actions",
         running ? nlohmann::json::array(
                       {"clear_errors", "load", "unload", "follower", "dryer_stop", "clear_fault"})
                 : nlohmann::json::array({"clear_errors", "load", "unload", "follower",
                                          "dryer_start", "dryer_stop", "clear_fault"})}};
    return device;
}

void MoonrakerClientMock::service_openams_dryers(double dt_s) {
    constexpr double kRateCPerS = 0.5;
    for (int unit = 0; unit < kOpenAmsMaxUnits; ++unit) {
        OpenAmsDryerSim& sim = openams_dryers_[unit];
        const double kAmbientC = openams_ambient_c(unit);
        const double remaining = sim.remaining_s.load();
        const double target = sim.target_c.load();
        double chamber = sim.chamber_c.load();
        if (remaining > 0.0 && target > 0.0) {
            chamber = chamber < target ? std::min(target, chamber + kRateCPerS * dt_s) : target;
            sim.remaining_s = std::max(0.0, remaining - dt_s);
            if (sim.remaining_s.load() <= 0.0) {
                sim.target_c = 0.0;
            }
        } else if (chamber > kAmbientC) {
            chamber = std::max(kAmbientC, chamber - kRateCPerS * dt_s);
        }
        sim.chamber_c = chamber;
    }
}

nlohmann::json MoonrakerClientMock::openams_status_json() const {
    if (openams_plugin_units()) {
        return openams_shared_status_json();
    }
    // One hub unit, four bays, one FPS lane. Only bays 2 and 3 hold spools;
    // the lane's current slot follows openams_loaded_slot_.
    static const char* const kGroupOfSlot[] = {"T0", "T1", "T2", "T0"};
    const int loaded = openams_loaded_slot_.load();
    nlohmann::json slots = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) {
        slots.push_back({{"id", i}, {"bay", i}, {"ready", i >= 2}, {"loaded", i == loaded}});
    }
    nlohmann::json lane = {{"id", "fps"},
                           {"state", loaded >= 0 ? "loaded" : "unloaded"},
                           {"pressure", 0.5},
                           {"set_point", 0.5}};
    if (loaded >= 0) {
        lane["current_group"] = kGroupOfSlot[loaded];
        lane["current_slot"] = loaded;
    } else {
        lane["current_group"] = nullptr;
        lane["current_slot"] = -1;
    }
    return {{"api_version", 1},
            {"schema", "openams.manager"},
            {"ready", true},
            {"commands",
             {{"load", "OPENAMS_LOAD"},
              {"unload", "OPENAMS_UNLOAD"},
              {"cancel", "OAMSM_LOAD_FILAMENT_CANCEL"},
              {"reset", "OAMSM_CLEAR_ERRORS"}}},
            {"lanes", nlohmann::json::array({lane})},
            {"units", nlohmann::json::array({{{"id", "1"},
                                              {"name", "OpenAMS"},
                                              {"kind", "oams"},
                                              {"topology", "hub"},
                                              {"lane", "fps"},
                                              {"connected", true},
                                              {"slots", slots}}})},
            {"groups", nlohmann::json::array({{{"name", "T0"}, {"lane", "fps"}, {"slots", {0, 3}}},
                                              {{"name", "T1"}, {"lane", "fps"}, {"slots", {1}}},
                                              {{"name", "T2"}, {"lane", "fps"}, {"slots", {2}}}})}};
}

void MoonrakerClientMock::service_openams_late_links(uint32_t tick) {
    // HELIX_MOCK_OPENAMS_LATE_LINKS=1: the spool links of slots 3 and 4 arrive
    // ~10s after start, the way openams_spoolman writes lane_data and announces
    // it once a Spoolman lookup finishes.
    static constexpr uint32_t kLateTick = 20;
    if (tick != kLateTick || !is_mock_openams() || !openams_plugin_units() ||
        !std::getenv("HELIX_MOCK_OPENAMS_LATE_LINKS")) {
        return;
    }
    static const char* const kColors[] = {"#1F3A93", "#F5F5F5"};
    for (int i = 0; i < 2; ++i) {
        mock_db_set("lane_data", "lane" + std::to_string(i + 4),
                    json{{"lane", std::to_string(i + 3)},
                         {"color", kColors[i]},
                         {"material", "PETG"},
                         {"spool_id", 40 + i},
                         {"name", "Late spool"}});
    }
    dispatch_method_callback("notify_openams_spoolman_status",
                             json{{"method", "notify_openams_spoolman_status"},
                                  {"params", json::array({json{{"state", "ready"}}})}});
}
