// SPDX-License-Identifier: GPL-3.0-or-later

// AFC on one Box Turtle (HELIX_MOCK_AMS=afc): the objects AFC-Klipper-Add-On
// publishes for `[AFC_BoxTurtle Turtle_1]` with four `[AFC_stepper laneN]`, one hub,
// one TurtleNeck buffer and one extruder, and the Python commands that change them,
// so real discovery builds the production AmsBackendAfc under --test. The shapes
// follow tests/fixtures/afc_*.json, captured from a real Box Turtle.

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace helix;
using mock_internal::effective_mock_ams_env;

namespace {

constexpr const char* kUnit = "Turtle_1";
constexpr int kMaxLanes = 8;

/// A lane's filament as the mock loads it. Spools 1-3 mirror the mock Spoolman's;
/// the fourth lane is unlinked and weightless, the untracked-filament path.
struct LaneSpool {
    const char* name; ///< Spoolman's filament name, "" when unlinked
    const char* material;
    const char* color;
    int spool_id;  ///< 0 = not linked
    double weight; ///< < 0 = unknown
};
constexpr LaneSpool kLaneSpools[kMaxLanes] = {
    {"Jet Black", "PLA", "#1A1A2E", 1, 850.0},
    {"Silk Blue", "Silk PLA", "#26DCD9", 2, 750.0},
    {"Pop Blue", "ASA", "#00AEFF", 3, 500.0},
    {"", "PETG", "#FF6600", 0, -1.0},
    {"Fire Engine Red", "ABS", "#D20000", 4, 100.0},
    {"Signal Yellow", "PETG", "#F4E111", 5, 1000.0},
    {"Clear", "TPU", "#E8E8E8", 6, 600.0},
    {"Gray ASA", "ASA", "#8A949E", 7, 1000.0},
};

std::string lane_name(int lane) {
    return "lane" + std::to_string(lane + 1);
}

std::string tool_name(int number) {
    return "T" + std::to_string(number); // DISPLAY_NUMBERING_OK: AFC map wire value
}

/// The value after " KEY=" on a gcode line, empty when absent.
std::string param(const std::string& gcode, const char* key) {
    const std::string needle = std::string(" ") + key + "=";
    const size_t at = gcode.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + needle.size();
    const size_t end = gcode.find_first_of(" \t", start);
    return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

} // namespace

bool MoonrakerClientMock::is_mock_afc_boxturtle() const {
    if (is_mock_reforge_afc()) {
        return false;
    }
    const std::string mode = effective_mock_ams_env();
    return mode == "afc" || mode == "box_turtle" || mode == "boxturtle";
}

int MoonrakerClientMock::afc_lane_count() {
    return std::clamp(get_runtime_config()->mock_ams_gate_count, 1, kMaxLanes);
}

void MoonrakerClientMock::init_afc_boxturtle() {
    std::lock_guard<std::mutex> lock(afc_mutex_);
    afc_lanes_.clear();
    for (int i = 0; i < afc_lane_count(); ++i) {
        const LaneSpool& s = kLaneSpools[i];
        afc_lanes_.push_back({s.name, s.material, s.color, s.spool_id, s.weight, i, true});
    }
    afc_loaded_ = 0;
}

void MoonrakerClientMock::append_afc_boxturtle_objects(nlohmann::json& objects) const {
    objects.push_back("AFC");
    objects.push_back(std::string("AFC_BoxTurtle ") + kUnit);
    objects.push_back(std::string("AFC_buffer ") + kUnit);
    objects.push_back("AFC_extruder extruder");
    objects.push_back(std::string("AFC_hub ") + kUnit);
    for (int i = 0; i < afc_lane_count(); ++i) {
        objects.push_back("AFC_stepper " + lane_name(i));
    }
}

nlohmann::json MoonrakerClientMock::afc_boxturtle_status_json() const {
    std::vector<AfcLane> lanes;
    int loaded = -1;
    std::string state;
    {
        std::lock_guard<std::mutex> lock(afc_mutex_);
        lanes = afc_lanes_;
        loaded = afc_loaded_;
        state = afc_state_;
    }

    nlohmann::json names = nlohmann::json::array();
    for (size_t i = 0; i < lanes.size(); ++i) {
        names.push_back(lane_name(static_cast<int>(i)));
    }
    const nlohmann::json loaded_lane =
        loaded >= 0 ? nlohmann::json(lane_name(loaded)) : nlohmann::json(nullptr);

    nlohmann::json out;
    out["AFC"] = {{"version", "1.2.0"},
                  {"current_load", loaded_lane},
                  {"current_lane", nullptr},
                  {"next_lane", nullptr},
                  {"current_state", state},
                  {"current_toolchange", 0},
                  {"number_of_toolchanges", 0},
                  {"spoolman", nullptr},
                  {"td1_present", false},
                  {"lane_data_enabled", false},
                  {"error_state", false},
                  {"bypass_state", false},
                  {"quiet_mode", false},
                  {"position_saved", false},
                  {"units", {std::string("Box_Turtle ") + kUnit}},
                  {"lanes", names},
                  {"extruders", {"extruder"}},
                  {"hubs", {kUnit}},
                  {"buffers", {kUnit}},
                  {"message", {{"message", ""}, {"type", ""}}},
                  {"led_state", true}};
    out[std::string("AFC_BoxTurtle ") + kUnit] = {
        {"lanes", names}, {"extruders", {"extruder"}}, {"hubs", {kUnit}}, {"buffers", {kUnit}}};
    out[std::string("AFC_hub ") + kUnit] = {
        {"state", loaded >= 0}, {"afc_bowden_length", 1285.0}, {"cut", false}, {"lanes", names}};
    out[std::string("AFC_buffer ") + kUnit] = {{"enabled", true},
                                               {"fault_detection_enabled", false},
                                               {"distance_to_fault", nullptr},
                                               {"lanes", names},
                                               {"state", "Trailing"}};
    out["AFC_extruder extruder"] = {{"buffer", nullptr},
                                    {"lane_loaded", loaded_lane},
                                    {"lanes", names},
                                    {"tool_start", "EBBCan:gpio6"},
                                    {"tool_start_status", loaded >= 0},
                                    {"tool_end", "EBBCan:gpio24"},
                                    {"tool_end_status", loaded >= 0},
                                    {"tool_stn", 42.0},
                                    {"tool_stn_unload", 90.0}};

    for (size_t i = 0; i < lanes.size(); ++i) {
        const AfcLane& l = lanes[i];
        const bool tooled = static_cast<int>(i) == loaded;
        out["AFC_stepper " + lane_name(static_cast<int>(i))] = {
            {"name", lane_name(static_cast<int>(i))},
            {"unit", kUnit},
            {"hub", kUnit},
            {"extruder", "extruder"},
            {"buffer", kUnit},
            {"buffer_status", tooled ? nlohmann::json("Trailing") : nlohmann::json(nullptr)},
            {"lane", static_cast<int>(i) + 1},
            {"map", l.map >= 0 ? nlohmann::json(tool_name(l.map)) : nlohmann::json(nullptr)},
            {"load", l.present},
            {"prep", l.present},
            {"tool_loaded", tooled},
            {"loaded_to_hub", tooled},
            {"material", l.material},
            {"spool_id", l.spool_id > 0 ? nlohmann::json(l.spool_id) : nlohmann::json(nullptr)},
            {"color", l.color},
            {"weight", l.weight >= 0.0 ? nlohmann::json(l.weight) : nlohmann::json(nullptr)},
            {"initial_weight", l.weight >= 0.0 ? nlohmann::json(1000.0) : nlohmann::json(nullptr)},
            {"filament_name", l.name},
            {"extruder_temp", nullptr},
            {"runout_lane", nullptr},
            {"filament_status", tooled ? "In Tool" : (l.present ? "Ready" : "Not Ready")},
            {"status", tooled ? "Tooled" : (l.present ? "Loaded" : "None")},
            {"dist_hub", 194.57},
            {"td1_color", ""},
            {"td1_td", ""},
            {"td1_scan_time", ""}};
    }
    return out;
}

nlohmann::json MoonrakerClientMock::afc_status_json() const {
    if (is_mock_reforge_afc()) {
        return reforge_afc_status_json();
    }
    if (is_mock_afc_boxturtle()) {
        return afc_boxturtle_status_json();
    }
    return nlohmann::json::object();
}

void MoonrakerClientMock::afc_move(int lane, const char* state) {
    // AFC reports the operation, then the lane at rest in the toolhead.
    {
        std::lock_guard<std::mutex> lock(afc_mutex_);
        afc_state_ = state;
    }
    dispatch_status_update({{"AFC", {{"current_state", state}}}});
    std::vector<helix::SharedCallback<void()>> steps;
    play_console_steps(std::move(steps), 1500, [this, lane] {
        {
            std::lock_guard<std::mutex> lock(afc_mutex_);
            afc_loaded_ = lane;
            afc_state_ = "Idle";
        }
        spdlog::info("[MoonrakerClientMock] AFC: {} in the toolhead",
                     lane >= 0 ? lane_name(lane) : std::string("nothing"));
        dispatch_status_update(afc_boxturtle_status_json());
    });
}

MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_afc_boxturtle(const std::string& gcode) {
    const std::string cmd = gcode.substr(0, gcode.find_first_of(" \t\r\n"));

    // The lane a LANE= parameter names, -1 when it names none.
    const auto lane_param = [&]() -> int {
        const std::string name = param(gcode, "LANE");
        std::lock_guard<std::mutex> lock(afc_mutex_);
        for (size_t i = 0; i < afc_lanes_.size(); ++i) {
            if (name == lane_name(static_cast<int>(i))) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const auto reject = [&](const std::string& message) -> GcodeResult {
        dispatch_gcode_response("!! " + message);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = message;
        return 1;
    };
    // SET_* edits one field of the named lane and republishes it.
    const auto edit = [&](auto&& apply) -> GcodeResult {
        const int lane = lane_param();
        if (lane < 0) {
            return reject("AFC: " + gcode + " names no lane");
        }
        {
            std::lock_guard<std::mutex> lock(afc_mutex_);
            apply(afc_lanes_[lane]);
        }
        dispatch_status_update(afc_boxturtle_status_json());
        return 0;
    };

    if (cmd == "TOOL_LOAD" || cmd == "CHANGE_TOOL") {
        const int lane = lane_param();
        if (lane < 0) {
            return reject("AFC: " + gcode + " names no lane");
        }
        afc_move(lane, "Loading");
        return 0;
    }
    if (cmd.size() >= 2 && cmd[0] == 'T' &&
        cmd.find_first_not_of("0123456789", 1) == std::string::npos) {
        // AFC owns T<n> and resolves it through the lane map.
        const auto number = text_io::parse_leading<int>(cmd.substr(1));
        int lane = -1;
        {
            std::lock_guard<std::mutex> lock(afc_mutex_);
            for (size_t i = 0; i < afc_lanes_.size(); ++i) {
                if (number && afc_lanes_[i].map == *number) {
                    lane = static_cast<int>(i);
                }
            }
        }
        if (lane < 0) {
            return reject("AFC: " + cmd + " is mapped to no lane");
        }
        afc_move(lane, "Loading");
        return 0;
    }
    if (cmd == "TOOL_UNLOAD") {
        afc_move(-1, "Unloading");
        return 0;
    }
    if (cmd == "SET_MATERIAL") {
        const std::string material = param(gcode, "MATERIAL");
        return edit([&](AfcLane& l) { l.material = material; });
    }
    if (cmd == "SET_COLOR") {
        std::string color = param(gcode, "COLOR");
        if (!color.empty() && color[0] != '#') {
            color.insert(0, "#");
        }
        return edit([&](AfcLane& l) { l.color = color; });
    }
    if (cmd == "SET_WEIGHT") {
        const double weight = text_io::parse_leading<double>(param(gcode, "WEIGHT")).value_or(0.0);
        return edit([&](AfcLane& l) { l.weight = weight; });
    }
    if (cmd == "SET_SPOOL_ID") {
        const int id = text_io::parse_leading<int>(param(gcode, "SPOOL_ID")).value_or(0);
        return edit([&](AfcLane& l) {
            l.spool_id = id;
            if (id <= 0) {
                l.name.clear();
            }
        });
    }
    if (cmd == "SET_MAP") {
        const std::string map = param(gcode, "MAP");
        const auto number = map.size() > 1 && (map[0] == 'T' || map[0] == 't')
                                ? text_io::parse_leading<int>(map.substr(1))
                                : std::nullopt;
        if (!number) {
            return reject("AFC: " + gcode + " names no tool");
        }
        return edit([&](AfcLane& l) { l.map = *number; });
    }
    if (cmd == "LANE_UNLOAD") {
        // Ejects the lane's filament back to the spool: it reads empty after.
        return edit([](AfcLane& l) { l.present = false; });
    }
    return std::nullopt;
}
