// SPDX-License-Identifier: GPL-3.0-or-later

// Reforge (Klipper4FlashForge/firmware) on a Creator 5 Pro, as its master ships
// it: AFC sits on ff_toolchange as one `[AFC_Toolchanger Tools]` unit with an
// `[AFC_extruder e<n>]` per head (ff-afc.cfg), and AFC registers the lane
// `AFC_lane e<n>` for each head itself (AFC_extruder.py, toolchanger_unit).
// The creator5_afc persona publishes those objects so real discovery builds the
// production AFC backend under --test. ff_toolchange's klipper-toolchanger views
// (`toolchanger`, `tool T<n>`) come from the shared toolchanger mock.

#include "core/text_io.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <array>
#include <string>

using namespace helix;

namespace {

constexpr int kHeads = 4;

/// Per head: the material and colour the persona loads it with.
struct HeadSpool {
    const char* material;
    const char* color;
};
constexpr std::array<HeadSpool, kHeads> kHeadSpools = {{
    {"PLA", "#E63A2E"},
    {"PETG", "#2E7BE6"},
    {"PLA", "#F2C230"},
    {"ABS", "#3C3C3C"},
}};

/// ff-afc.cfg's pin_tool_start per head: the fd_ex<n> presence switch.
constexpr std::array<const char*, kHeads> kToolStartPins = {
    "!eheaterboard:PC13", "!eheaterboard:PC14", "!eheaterboard:PC3", "!eheaterboard:PA2"};

std::string head_lane(int head) {
    return "e" + std::to_string(head);
}

std::string head_extruder(int head) {
    return head == 0 ? "extruder" : "extruder" + std::to_string(head);
}

std::string tool_name(int number) {
    return "T" + std::to_string(number); // DISPLAY_NUMBERING_OK: AFC map wire value
}

/// The value after "KEY=" on a gcode line, empty when absent.
std::string param(const std::string& gcode, const char* key) {
    const std::string needle = std::string(key) + "=";
    const size_t at = gcode.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + needle.size();
    const size_t end = gcode.find_first_of(" \t", start);
    return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

/// The head number of a lane or AFC_extruder name `e<n>`, -1 for anything else.
int head_of(const std::string& name) {
    if (name.size() < 2 || name[0] != 'e') {
        return -1;
    }
    const auto n = text_io::parse_leading<int>(name.substr(1));
    return n && *n >= 0 && *n < kHeads ? *n : -1;
}

} // namespace

bool MoonrakerClientMock::is_mock_reforge_afc() const {
    return printer_type_ == PrinterType::FLASHFORGE_CREATOR5_AFC;
}

nlohmann::json MoonrakerClientMock::reforge_afc_status_json() const {
    std::array<int, kHeads> map{};
    int mounted = -1;
    {
        std::lock_guard<std::mutex> lock(reforge_afc_mutex_);
        map = reforge_lane_map_;
        mounted = reforge_mounted_;
    }

    nlohmann::json lanes = nlohmann::json::array();
    nlohmann::json maps = nlohmann::json::array();
    for (int head = 0; head < kHeads; ++head) {
        lanes.push_back(head_lane(head));
        if (map[head] >= 0) {
            maps.push_back(tool_name(map[head]));
        }
    }

    nlohmann::json out;
    out["AFC"] = {{"version", "1.2.7"},
                  {"current_load", mounted >= 0 ? nlohmann::json(head_lane(mounted)) : nullptr},
                  {"current_lane", nullptr},
                  {"next_lane", nullptr},
                  {"current_state", "Idle"},
                  {"current_toolchange", 0},
                  {"number_of_toolchanges", 0},
                  {"spoolman", nullptr},
                  {"td1_present", false},
                  {"lane_data_enabled", false},
                  {"error_state", false},
                  {"bypass_state", false},
                  {"quiet_mode", false},
                  {"position_saved", false},
                  {"units", {"Toolchanger Tools"}},
                  {"lanes", lanes},
                  {"maps", maps},
                  {"extruders", lanes},
                  {"hubs", nlohmann::json::array()},
                  {"buffers", nlohmann::json::array()},
                  {"message", {{"message", ""}, {"type", ""}}},
                  {"led_state", true},
                  {"multiple_tool_mapping", true}};
    out["AFC_Toolchanger Tools"] = {{"lanes", lanes},
                                    {"extruders", lanes},
                                    {"hubs", nlohmann::json::array()},
                                    {"buffers", nlohmann::json::array()}};

    for (int head = 0; head < kHeads; ++head) {
        const std::string lane = head_lane(head);
        // Filament at a head's gate is that head's: every standalone lane reads
        // loaded into its own head.
        out["AFC_extruder " + lane] = {{"tool_stn", 0.0},
                                       {"tool_stn_unload", 0.0},
                                       {"tool_sensor_after_extruder", 0.0},
                                       {"tool_unload_speed", 25.0},
                                       {"tool_load_speed", 25.0},
                                       {"buffer", nullptr},
                                       {"lane_loaded", lane},
                                       {"tool_start", kToolStartPins[head]},
                                       {"tool_start_status", true},
                                       {"tool_end", nullptr},
                                       {"tool_end_status", false},
                                       {"lanes", {lane}},
                                       {"on_shuttle", mounted == head},
                                       {"is_standalone", true},
                                       {"next_pickup", false},
                                       {"status", "Idle"}};
        out["AFC_lane " + lane] = {{"name", lane},
                                   {"unit", "Tools"},
                                   {"hub", "direct"},
                                   {"extruder", lane},
                                   {"buffer", nullptr},
                                   {"buffer_status", nullptr},
                                   {"lane", head},
                                   {"map", map[head] >= 0 ? nlohmann::json(tool_name(map[head]))
                                                          : nlohmann::json(nullptr)},
                                   {"load", true},
                                   {"prep", true},
                                   {"tool_loaded", true},
                                   {"loaded_to_hub", true},
                                   {"material", kHeadSpools[head].material},
                                   {"spool_id", nullptr},
                                   {"color", kHeadSpools[head].color},
                                   {"weight", 1000},
                                   {"extruder_temp", nullptr},
                                   {"bed_temp", nullptr},
                                   {"runout_lane", nullptr},
                                   {"filament_status", "In Tool"},
                                   {"status", "Tool Loaded"},
                                   {"dist_hub", 60}};
    }
    return out;
}

nlohmann::json MoonrakerClientMock::reforge_afc_config_sections() const {
    nlohmann::json sections = nlohmann::json::object();
    if (!is_mock_reforge_afc()) {
        return sections;
    }
    for (int head = 0; head < kHeads; ++head) {
        sections["afc_extruder " + head_lane(head)] = {{"extruder_name", head_extruder(head)},
                                                       {"toolchanger_unit", "Tools"},
                                                       {"u1_park_detector_name", tool_name(head)},
                                                       {"pin_tool_start", kToolStartPins[head]},
                                                       {"map", tool_name(head)}};
    }
    sections["afc_toolchanger tools"] = nlohmann::json::object();
    // ff_print runs its before_macro ahead of the file; the shipped macro
    // prepares (START_PRINT) unless variable_prepare is 0.
    sections["ff_print"] = {{"before_macro", "FF_BEFORE_PRINT_START"},
                            {"after_macro", "FF_AFTER_PRINT_END"}};
    sections["gcode_macro ff_before_print_start"] = {
        {"variable_prepare", "1"}, {"variable_prepare_on_m23", "0"}, {"variable_defer_mesh", "0"}};
    return sections;
}

void MoonrakerClientMock::reforge_mount(int head) {
    {
        std::lock_guard<std::mutex> lock(reforge_afc_mutex_);
        reforge_mounted_ = head;
    }
    spdlog::info("[MoonrakerClientMock] Reforge AFC: {} on the carriage", head_lane(head));
    // ff_toolchange's views and the active extruder follow the grab.
    dispatch_status_update({{"toolhead", {{"extruder", head_extruder(head)}}}});
    dispatch_toolchanger_tool(head);
    dispatch_status_update(reforge_afc_status_json());
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_reforge_afc(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t\r\n");
    const std::string cmd = gcode.substr(0, token_end);

    int head = -1;
    if (cmd == "AFC_SELECT_TOOL") {
        head = head_of(param(gcode, "TOOL"));
    } else if (cmd == "CHANGE_TOOL") {
        head = head_of(param(gcode, "LANE"));
    } else if (cmd == "SET_MAP") {
        const int lane = head_of(param(gcode, "LANE"));
        const std::string map = param(gcode, "MAP");
        const auto number = map.size() > 1 && (map[0] == 'T' || map[0] == 't')
                                ? text_io::parse_leading<int>(map.substr(1))
                                : std::nullopt;
        if (lane < 0 || !number) {
            return std::nullopt;
        }
        {
            // enable_multiple_mapping: the T number moves to the lane, and the
            // lane that carried it keeps none.
            std::lock_guard<std::mutex> lock(reforge_afc_mutex_);
            for (int& mapped : reforge_lane_map_) {
                if (mapped == *number) {
                    mapped = -1;
                }
            }
            reforge_lane_map_[lane] = *number;
        }
        dispatch_status_update(reforge_afc_status_json());
        return 0;
    } else if (cmd.size() >= 2 && cmd[0] == 'T') {
        // AFC owns T0..T3 and resolves each through its lane map.
        const auto number = text_io::parse_leading<int>(cmd.substr(1));
        if (!number || cmd.find_first_not_of("0123456789", 1) != std::string::npos) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(reforge_afc_mutex_);
        for (int lane = 0; lane < kHeads; ++lane) {
            if (reforge_lane_map_[lane] == *number) {
                head = lane;
                break;
            }
        }
    } else {
        return std::nullopt;
    }
    if (head < 0) {
        const std::string message = "AFC: " + gcode + " names no head";
        dispatch_gcode_response("!! " + message);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = message;
        return 1;
    }
    reforge_mount(head);
    return 0;
}
