// SPDX-License-Identifier: GPL-3.0-or-later

// Bondtech INDX mock hardware (HELIX_MOCK_AMS=indx): the objects, configfile
// sections and macro state Bondtech's upstream macros publish (BondtechAB/INDX
// macros/*.cfg), so real discovery builds the production AmsBackendIndx under
// --test. A tool change publishes its park and its pickup before the gcode
// acks, as the real macro does, and the heater follows INDX's own
// M104/M109 rule: nothing heats with no tool on the head or a no-hotend tool.

#include "core/text_io.h"
#include "env_knobs.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "moonraker_error.h"
#include "simulated_clock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <string>

namespace {

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

/// The number after an M-code's letter parameter ("M104 S210" -> "210").
std::string mcode_param(const std::string& gcode, char letter) {
    for (size_t at = gcode.find(' '); at != std::string::npos; at = gcode.find(' ', at + 1)) {
        if (at + 1 < gcode.size() &&
            std::toupper(static_cast<unsigned char>(gcode[at + 1])) == letter) {
            const size_t start = at + 2;
            const size_t end = gcode.find_first_of(" \t", start);
            return gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
    }
    return {};
}

/// "T<n>" -> n, else -1.
int tool_command(const std::string& cmd) {
    if (cmd.size() < 2 || cmd[0] != 'T' ||
        !std::all_of(cmd.begin() + 1, cmd.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return -1;
    }
    return helix::text_io::parse_leading<int>(cmd.substr(1)).value_or(-1);
}

} // namespace

bool MoonrakerClientMock::is_mock_indx() const {
    return helix::mock::effective_mock_ams(std::getenv("HELIX_MOCK_AMS"),
                                           std::getenv("HELIX_MOCK_PRINTER")) == "indx";
}

void MoonrakerClientMock::read_indx_knobs() {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    indx_ = IndxMockState{};
    indx_.tool_count =
        static_cast<int>(helix::env_float("HELIX_MOCK_INDX_TOOLS", 6.0f, 2.0f, 10.0f));
    const char* fault = std::getenv("HELIX_MOCK_INDX_FAULT");
    const std::string f = fault ? fault : "";
    indx_.seat_suspect = f == "seat_suspect" ? 1 : -1;
    indx_.docks_calibrated = f != "docks_uncalibrated";
    indx_.unrecognized = f == "unrecognized";
    indx_.kalico = f == "kalico";
    indx_.spool_macros = helix::env_flag("HELIX_MOCK_INDX_SPOOL_MACROS");
    indx_.no_heat_tools = {indx_.tool_count - 1};
    indx_.spool_ids.clear();
    for (int i = 0; i < indx_.tool_count; ++i) {
        indx_.spool_ids.push_back(i + 1);
        // Tool 0 is the reference every other tool is measured against.
        indx_.offsets.push_back({0.1 * i, -0.05 * i, 0.02 * i});
    }
    spdlog::info("[MoonrakerClientMock] INDX mock: {} tools, fault={}, spool macros={}",
                 indx_.tool_count, f.empty() ? "none" : f, indx_.spool_macros);
}

void MoonrakerClientMock::append_indx_objects(nlohmann::json& objects) const {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    if (!indx_.kalico) {
        objects.push_back("indx");
    }
    objects.push_back("save_variables");
    objects.push_back("load_cell_probe");
    for (const char* macro :
         {"TOOL_POSITIONS", "_TOOLCHANGE_VARS", "PARK_TOOL", "LOAD_FILAMENT", "SET_TOOL_FILAMENT",
          "TOOL_STATUS", "MANUAL_TOOL_REMOVE", "MANUAL_TOOL_SEAT", "MODE_STEALTH", "MODE_NORMAL",
          "MODE_SPORT", "TC_CYCLE_ALL"}) {
        objects.push_back(std::string("gcode_macro ") + macro);
    }
    if (!indx_.unrecognized) {
        objects.push_back("gcode_macro CHANGE_TOOL");
    }
    for (int i = 0; i < indx_.tool_count; ++i) {
        objects.push_back("gcode_macro T" + std::to_string(i));
    }
}

nlohmann::json MoonrakerClientMock::indx_config_sections() const {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    nlohmann::json config = nlohmann::json::object();
    config["indx"] = {{"mcu", "indxmcu"}};
    config["gcode_macro TOOL_POSITIONS"] = {
        {"variable_tool_count", std::to_string(indx_.tool_count)}};
    for (int i = 0; i < indx_.tool_count; ++i) {
        std::string gcode = "CHANGE_TOOL TOOL=" + std::to_string(i);
        if (indx_.spool_macros) {
            gcode = "SET_ACTIVE_SPOOL ID={printer['gcode_macro T" + std::to_string(i) +
                    "'].spool_id}\n" + gcode;
        }
        config["gcode_macro T" + std::to_string(i)] = {{"gcode", gcode}};
    }
    config["gcode_macro LOAD_FILAMENT"] = {
        {"gcode", "{% set t = params.TOOL|int %}\n{% set type = params.TYPE|default('') %}\n"
                  "{% set temp = params.TEMP|default(0) %}\nCHANGE_TOOL TOOL={t}"}};
    return config;
}

nlohmann::json MoonrakerClientMock::indx_status_json() const {
    std::lock_guard<std::mutex> lock(indx_mutex_);
    nlohmann::json status = nlohmann::json::object();
    nlohmann::json variables = {{"active_tool", indx_.active_tool},
                                {"toolchange_count", indx_.toolchange_count},
                                {"seat_suspect", indx_.seat_suspect},
                                {"toolhead_state", indx_.toolhead_state},
                                {"global_z_offset", indx_.global_z}};
    for (size_t i = 0; i < indx_.offsets.size(); ++i) {
        const std::string prefix = "t" + std::to_string(i) + "_offset_";
        variables[prefix + "x"] = indx_.offsets[i][0];
        variables[prefix + "y"] = indx_.offsets[i][1];
        variables[prefix + "z"] = indx_.offsets[i][2];
    }
    status["save_variables"] = {{"variables", variables}};
    nlohmann::json tools = nlohmann::json::array();
    for (int i = 0; i < indx_.tool_count; ++i) {
        tools.push_back({{"x", 45.0 * i}, {"dock_y", -26.0}});
    }
    status["gcode_macro TOOL_POSITIONS"] = {{"tool_count", indx_.tool_count},
                                            {"tools", tools},
                                            {"no_heat_tools", indx_.no_heat_tools},
                                            {"docks_calibrated", indx_.docks_calibrated}};
    status["gcode_macro _TOOLCHANGE_VARS"] = {{"toolhead_state", indx_.toolhead_state},
                                              {"mode", indx_.mode}};
    status["gcode_macro LOAD_FILAMENT"] = {
        {"filament_presets",
         {{"PLA", {{"temp", 210}, {"density", 1.24}, {"hc", 1.80}}},
          {"PETG", {{"temp", 240}, {"density", 1.27}, {"hc", 1.30}}},
          {"ABS", {{"temp", 245}, {"density", 1.04}, {"hc", 1.40}}},
          {"ASA", {{"temp", 250}, {"density", 1.07}, {"hc", 1.40}}},
          {"PA", {{"temp", 260}, {"density", 1.14}, {"hc", 1.70}}},
          {"PC", {{"temp", 270}, {"density", 1.20}, {"hc", 1.20}}},
          {"TPU", {{"temp", 230}, {"density", 1.21}, {"hc", 1.80}, {"speed", 2.0}}}}}};
    for (int i = 0; i < indx_.tool_count; ++i) {
        nlohmann::json t = {{"active", i == indx_.active_tool}};
        if (indx_.spool_macros) {
            t["spool_id"] = indx_.spool_ids[static_cast<size_t>(i)];
        }
        status["gcode_macro T" + std::to_string(i)] = t;
    }
    return status;
}

void MoonrakerClientMock::publish_indx_if_changed() {
    nlohmann::json frame = indx_status_json();
    {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        if (frame == indx_published_) {
            return;
        }
        indx_published_ = frame;
    }
    dispatch_status_update(frame);
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_indx(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    auto refuse = [this](const std::string& message) {
        spdlog::warn("[MoonrakerClientMock] INDX gcode rejected: {}", message);
        dispatch_gcode_response("!! " + message);
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = message;
        return 1;
    };

    // INDX's own M104/M109 heat only a mounted tool that has a hotend, and a
    // direct SET_HEATER_TEMPERATURE on the coil is refused the same way here,
    // so a missing gate in the screen shows as a target that never moves.
    const bool heat_cmd = cmd == "M104" || cmd == "M109" ||
                          (cmd == "SET_HEATER_TEMPERATURE" && param(gcode, "HEATER") == "extruder");
    if (heat_cmd) {
        const std::string value =
            cmd == "SET_HEATER_TEMPERATURE" ? param(gcode, "TARGET") : mcode_param(gcode, 'S');
        const double target = helix::text_io::parse_leading<double>(value).value_or(0.0);
        std::lock_guard<std::mutex> lock(indx_mutex_);
        const bool no_heat = std::count(indx_.no_heat_tools.begin(), indx_.no_heat_tools.end(),
                                        indx_.active_tool) > 0;
        if (target > 0 && (indx_.active_tool < 0 || no_heat)) {
            spdlog::info("[MoonrakerClientMock] INDX: {} {:.0f}: {}, not heating", cmd, target,
                         indx_.active_tool < 0 ? "no tool on the head" : "tool has no hotend");
            return 0;
        }
        return std::nullopt;
    }

    // What every macro that docks refuses on. Caller holds indx_mutex_.
    auto dock_refusal = [this]() -> std::string {
        if (indx_.seat_suspect >= 0) {
            return "T" +
                   std::to_string(
                       indx_.seat_suspect) + // DISPLAY_NUMBERING_OK: Klipper macro error text
                   " may be half seated on the head";
        }
        if (!indx_.docks_calibrated) {
            return "INDX: the dock positions are not calibrated yet";
        }
        return {};
    };
    if (cmd == "TC_CYCLE_ALL") {
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(indx_mutex_);
            if (auto reason = dock_refusal(); !reason.empty()) {
                return refuse(reason);
            }
            count = indx_.tool_count;
        }
        for (int t = 0; t < count; ++t) {
            run_indx_change(t, 0.0);
        }
        run_indx_change(-1, 0.0);
        spdlog::info("[MoonrakerClientMock] INDX TC_CYCLE_ALL: cycled {} tools", count);
        return 0;
    }

    int tool = tool_command(cmd);
    if (cmd == "CHANGE_TOOL" || cmd == "LOAD_FILAMENT") {
        tool = helix::text_io::parse_leading<int>(param(gcode, "TOOL")).value_or(-1);
        if (tool < 0) {
            return refuse(cmd + " requires TOOL=<n>");
        }
    }
    const bool park = cmd == "PARK_TOOL";
    if (cmd == "MANUAL_TOOL_REMOVE" || cmd == "MANUAL_TOOL_SEAT") {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        indx_.seat_suspect = -1;
        if (cmd == "MANUAL_TOOL_REMOVE") {
            indx_.active_tool = -1;
        } else if (auto t = helix::text_io::parse_leading<int>(param(gcode, "TOOL"))) {
            indx_.active_tool = *t;
        }
    }
    if (cmd == "MANUAL_TOOL_REMOVE" || cmd == "MANUAL_TOOL_SEAT") {
        publish_indx_if_changed();
        return 0;
    }
    if (cmd == "MODE_STEALTH" || cmd == "MODE_NORMAL" || cmd == "MODE_SPORT") {
        {
            std::lock_guard<std::mutex> lock(indx_mutex_);
            indx_.mode = helix::text_io::to_lower(cmd.substr(5));
        }
        publish_indx_if_changed();
        return 0;
    }
    if (cmd == "SET_TOOL_FILAMENT") {
        for (const char* key : {"TOOL", "DENSITY", "HEAT_CAPACITY"}) {
            if (param(gcode, key).empty()) {
                return refuse(std::string("Error on 'SET_TOOL_FILAMENT': missing ") + key);
            }
        }
        spdlog::info("[MoonrakerClientMock] INDX: {}", gcode);
        return 0;
    }
    if (cmd == "INDX_CALIBRATE" || cmd == "INDX_FAN_CALIBRATE") {
        if (cmd == "INDX_CALIBRATE" && extruder_target_.load() > 0) {
            return refuse(
                "Cannot calibrate while the heater is active. Turn the heater off first.");
        }
        {
            std::lock_guard<std::mutex> lock(indx_mutex_);
            if (indx_.active_tool < 0) {
                return refuse("No tool on the head");
            }
        }
        dispatch_gcode_response(cmd == "INDX_CALIBRATE"
                                    ? "INDX calibration complete. Run SAVE_CONFIG to save the new "
                                      "parameters and restart."
                                    : "Run SAVE_CONFIG to save the new fan parameters.");
        return 0;
    }
    if (tool < 0 && !park) {
        return std::nullopt;
    }

    double target = extruder_target_.load();
    if (cmd == "LOAD_FILAMENT") {
        // LOAD_FILAMENT heats to TEMP=, else its TYPE preset's load temperature.
        static const std::map<std::string, double> kPresetTemps = {
            {"PLA", 210}, {"PETG", 240}, {"ABS", 245}, {"ASA", 250},
            {"PA", 260},  {"PC", 270},   {"TPU", 230}};
        const auto preset = kPresetTemps.find(helix::text_io::to_upper(param(gcode, "TYPE")));
        target = helix::text_io::parse_leading<double>(param(gcode, "TEMP"))
                     .value_or(preset != kPresetTemps.end() ? preset->second : 0.0);
    }
    {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        if (cmd == "CHANGE_TOOL" && indx_.unrecognized) {
            return std::nullopt;
        }
        if (auto reason = dock_refusal(); !reason.empty()) {
            return refuse(reason);
        }
        if (tool >= indx_.tool_count) {
            return refuse("T" +
                          std::to_string(tool) + // DISPLAY_NUMBERING_OK: Klipper macro error text
                          " is not configured");
        }
        if (park && indx_.active_tool < 0) {
            return 0;
        }
        if (!park && tool == indx_.active_tool && cmd != "LOAD_FILAMENT") {
            return 0;
        }
    }

    // Klipper runs the macro to the end before the gcode call returns, so the
    // park and the pickup are both on the wire before this acks.
    run_indx_change(park ? -1 : tool, target);
    if (cmd == "LOAD_FILAMENT") {
        dispatch_gcode_response("LOAD_FILAMENT: T" + std::to_string(tool) + " loaded");
    }
    spdlog::info("[MoonrakerClientMock] INDX {}: {}", cmd, park ? "parking" : "changing tool");
    return 0;
}

void MoonrakerClientMock::run_indx_change(int tool, double target_c) {
    // The coil goes off before the head drives into a dock.
    set_extruder_target(0.0);
    bool parks = false;
    {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        parks = indx_.active_tool >= 0 && tool != indx_.active_tool;
    }
    if (parks) {
        {
            std::lock_guard<std::mutex> lock(indx_mutex_);
            indx_.active_tool = -1;
            indx_.toolhead_state = 2;
        }
        publish_indx_if_changed();
    }
    if (tool < 0) {
        return;
    }
    bool heat = false;
    {
        std::lock_guard<std::mutex> lock(indx_mutex_);
        indx_.active_tool = tool;
        indx_.toolhead_state = 0;
        ++indx_.toolchange_count;
        heat = std::count(indx_.no_heat_tools.begin(), indx_.no_heat_tools.end(), tool) == 0 &&
               target_c > 0;
    }
    publish_indx_if_changed();
    if (heat) {
        set_extruder_target(target_c);
    }
}

bool MoonrakerClientMock::simulate_indx_blocking(
    const std::string& script, helix::SharedCallback<void(const nlohmann::json&)> success_cb,
    helix::SharedCallback<void(const MoonrakerError&)> error_cb) {
    if (!is_mock_indx()) {
        return false;
    }
    const std::string cmd =
        helix::text_io::to_upper(script.substr(0, script.find_first_of(" \t\n")));
    if (cmd != "INDX_CALIBRATE" && cmd != "INDX_FAN_CALIBRATE" && cmd != "TC_CYCLE_ALL") {
        return false;
    }
    if (gcode_script(script) != 0) {
        if (error_cb) {
            error_cb(
                MoonrakerError::json_rpc_error("printer.gcode.script", get_last_gcode_error()));
        }
        return true;
    }
    // A calibration heats to 200C and cools again, a cycle visits every dock:
    // minutes on the printer, seconds at --sim-speed 6.
    constexpr int kBlockingSimMs = 60000;
    play_console_steps(
        {}, static_cast<uint32_t>(std::max(1, sim_speed().shorten_wait_ms(kBlockingSimMs))),
        [success_cb = std::move(success_cb)] {
            if (success_cb) {
                success_cb(nlohmann::json::object());
            }
        });
    return true;
}
