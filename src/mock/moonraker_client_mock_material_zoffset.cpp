// SPDX-License-Identifier: GPL-3.0-or-later

// Z offset by material in the mock (HELIX_MOCK_MATERIAL_ZOFFSET,
// prestonbrown/helixscreen#2142). Two printers:
//
// start_print_vars: [gcode_macro _START_PRINT_VARS] carries variable_offset_<MAT>
//   lines, as Jacobean's K2 Kalico does; its START_PRINT applies
//   offset_<material> (else offset_default) with SET_GCODE_OFFSET Z= at print
//   start. MoonrakerAPIMock serves the file that defines the section
//   (overrides.cfg, included from printer.cfg).
// helix_macro: HelixScreen's HELIX_MATERIAL_OFFSET with [save_variables]; a
//   print start applies helix_zoffset_<material> (else _default), preferring
//   the material the screen passed in _HELIX_MATERIAL.next, and records it.

#include "core/text_io.h"
#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

constexpr const char* kVarsObject = "gcode_macro _START_PRINT_VARS";
constexpr const char* kHelixObject = "gcode_macro _HELIX_MATERIAL";

/// The helper macro's key rule: lower case, with '-', ' ', '+' and '.' as '_'.
std::string helix_key(std::string_view material) {
    std::string key = helix::text_io::to_lower(helix::text_io::trim(material));
    for (char& c : key) {
        if (c == '-' || c == ' ' || c == '+' || c == '.') {
            c = '_';
        }
    }
    return key.empty() ? "default" : key;
}

/// NAME=value from a command line, value up to the next blank, quotes stripped.
std::string param(const std::string& gcode, const char* name) {
    const std::string key = std::string(" ") + name + "=";
    const size_t at = gcode.find(key);
    if (at == std::string::npos) {
        return {};
    }
    const size_t start = at + key.size();
    const size_t end = gcode.find_first_of(" \t\n", start);
    std::string value =
        gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::string out;
    for (char c : value) {
        if (c != '\'' && c != '"') {
            out += c;
        }
    }
    return out;
}

} // namespace

MoonrakerClientMock::MaterialZOffsetMock MoonrakerClientMock::material_zoffset_mock() {
    const char* env = std::getenv("HELIX_MOCK_MATERIAL_ZOFFSET");
    const std::string mode = env ? env : "";
    if (mode == "start_print_vars") {
        return MaterialZOffsetMock::StartPrintVars;
    }
    if (mode == "helix_macro") {
        return MaterialZOffsetMock::HelixMacro;
    }
    return MaterialZOffsetMock::None;
}

void MoonrakerClientMock::append_material_zoffset_objects(json& objects) const {
    auto add = [&objects](const std::string& name) {
        for (const auto& o : objects) {
            if (o.is_string() && o.get<std::string>() == name) {
                return;
            }
        }
        objects.push_back(name);
    };
    switch (material_zoffset_mock()) {
    case MaterialZOffsetMock::StartPrintVars:
        add("gcode_macro START_PRINT");
        add(kVarsObject);
        break;
    case MaterialZOffsetMock::HelixMacro:
        add("gcode_macro HELIX_MATERIAL_OFFSET");
        add(kHelixObject);
        add("save_variables");
        break;
    case MaterialZOffsetMock::None:
        break;
    }
}

std::map<std::string, double> MoonrakerClientMock::initial_material_zoffset_values() {
    // A tuned-looking table, so the Settings rows have something to show.
    switch (material_zoffset_mock()) {
    case MaterialZOffsetMock::StartPrintVars:
        return {{"offset_pla", 0.0}, {"offset_petg", 0.04},   {"offset_abs", -0.02},
                {"offset_asa", 0.0}, {"offset_default", 0.0}, {"offset_probe", 0.0}};
    case MaterialZOffsetMock::HelixMacro:
        return {{"helix_zoffset_pla", 0.02},
                {"helix_zoffset_petg", 0.06},
                {"helix_zoffset_default", 0.0}};
    case MaterialZOffsetMock::None:
        break;
    }
    return {};
}

std::map<std::string, std::string> MoonrakerClientMock::material_zoffset_config_files() {
    if (material_zoffset_mock() != MaterialZOffsetMock::StartPrintVars) {
        return {};
    }
    // Where Jacobean's K2 Kalico keeps the table, so a Save can edit it.
    return {{"printer.cfg", "[include overrides.cfg]\n\n[printer]\nkinematics: corexy\n"},
            {"overrides.cfg", "[gcode_macro _START_PRINT_VARS]\n"
                              "# per-material first-layer offsets\n"
                              "variable_offset_PLA: 0\n"
                              "variable_offset_PETG: 0.04\n"
                              "variable_offset_ABS: -0.02\n"
                              "variable_offset_ASA: 0\n"
                              "variable_offset_DEFAULT: 0\n"
                              "variable_offset_PROBE: 0\n"
                              "variable_heat_soak: 0 # minutes\n"
                              "gcode:\n"}};
}

json MoonrakerClientMock::material_zoffset_status_json() const {
    std::lock_guard<std::mutex> lock(material_zoffset_mutex_);
    json status = json::object();
    switch (material_zoffset_mock()) {
    case MaterialZOffsetMock::StartPrintVars: {
        json vars = {{"heat_soak", 0}};
        for (const auto& [k, v] : material_zoffset_values_) {
            vars[k] = v;
        }
        status[kVarsObject] = vars;
        break;
    }
    case MaterialZOffsetMock::HelixMacro: {
        json vars = json::object();
        for (const auto& [k, v] : material_zoffset_values_) {
            vars[k] = v;
        }
        status["save_variables"] = {{"variables", vars}};
        status[kHelixObject] = {{"next", material_zoffset_next_},
                                {"applied", material_zoffset_applied_},
                                {"apply_count", material_zoffset_apply_count_}};
        break;
    }
    case MaterialZOffsetMock::None:
        break;
    }
    return status;
}

MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_material_zoffset(const std::string& gcode) {
    const MaterialZOffsetMock mode = material_zoffset_mock();
    const std::string cmd = gcode.substr(0, gcode.find_first_of(" \t\n"));
    if (cmd == "SET_GCODE_VARIABLE") {
        const std::string macro = helix::text_io::to_upper(param(gcode, "MACRO"));
        const std::string variable = helix::text_io::to_lower(param(gcode, "VARIABLE"));
        const std::string value = param(gcode, "VALUE");
        if (mode == MaterialZOffsetMock::StartPrintVars && macro == "_START_PRINT_VARS") {
            {
                std::lock_guard<std::mutex> lock(material_zoffset_mutex_);
                if (material_zoffset_values_.count(variable) == 0) {
                    std::lock_guard<std::mutex> err_lock(gcode_error_mutex_);
                    last_gcode_error_ = "Unknown gcode_macro variable '" + variable + "'";
                    return 1;
                }
                material_zoffset_values_[variable] =
                    helix::text_io::parse_leading<double>(value).value_or(0.0);
            }
            spdlog::info("[MoonrakerClientMock] _START_PRINT_VARS {} = {}", variable, value);
            dispatch_status_update(material_zoffset_status_json());
            return 0;
        }
        if (mode == MaterialZOffsetMock::HelixMacro && macro == "_HELIX_MATERIAL" &&
            variable == "next") {
            {
                std::lock_guard<std::mutex> lock(material_zoffset_mutex_);
                material_zoffset_next_ = value;
            }
            spdlog::info("[MoonrakerClientMock] _HELIX_MATERIAL next = '{}'", value);
            dispatch_status_update(material_zoffset_status_json());
            return 0;
        }
        return std::nullopt;
    }
    if (cmd == "SAVE_VARIABLE" && mode == MaterialZOffsetMock::HelixMacro) {
        const std::string variable = helix::text_io::to_lower(param(gcode, "VARIABLE"));
        if (variable.rfind("helix_zoffset_", 0) != 0) {
            return std::nullopt;
        }
        {
            std::lock_guard<std::mutex> lock(material_zoffset_mutex_);
            material_zoffset_values_[variable] =
                helix::text_io::parse_leading<double>(param(gcode, "VALUE")).value_or(0.0);
        }
        spdlog::info("[MoonrakerClientMock] SAVE_VARIABLE {} = {}", variable,
                     param(gcode, "VALUE"));
        dispatch_status_update(material_zoffset_status_json());
        return 0;
    }
    return std::nullopt;
}

void MoonrakerClientMock::apply_material_zoffset_at_start(const std::string& filament_type) {
    const MaterialZOffsetMock mode = material_zoffset_mock();
    if (mode == MaterialZOffsetMock::None) {
        return;
    }
    // The slicer passes the first filament: MATERIAL={filament_type[initial_extruder]}.
    const std::string first = std::string(
        helix::text_io::trim(std::string_view(filament_type).substr(0, filament_type.find(';'))));
    double offset = 0.0;
    std::string material;
    {
        std::lock_guard<std::mutex> lock(material_zoffset_mutex_);
        const auto lookup = [this](const std::string& key, const std::string& fallback) {
            auto it = material_zoffset_values_.find(key);
            if (it == material_zoffset_values_.end()) {
                it = material_zoffset_values_.find(fallback);
            }
            return it == material_zoffset_values_.end() ? 0.0 : it->second;
        };
        if (mode == MaterialZOffsetMock::StartPrintVars) {
            material = first;
            offset = lookup("offset_" + helix::text_io::to_lower(material), "offset_default");
        } else {
            material = helix::text_io::to_upper(
                material_zoffset_next_.empty() ? first : material_zoffset_next_);
            offset = lookup("helix_zoffset_" + helix_key(material), "helix_zoffset_default");
            material_zoffset_next_.clear();
            material_zoffset_applied_ = material;
            ++material_zoffset_apply_count_;
        }
    }
    spdlog::info("[MoonrakerClientMock] Print start applies {:.3f} mm for '{}'", offset, material);
    gcode_offset_z_.store(offset);
    dispatch_gcode_move_update();
    dispatch_status_update(material_zoffset_status_json());
}
