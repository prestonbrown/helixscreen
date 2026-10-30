// SPDX-License-Identifier: GPL-3.0-or-later

#include "indx_contract.h"

#include "ams_backend.h"
#include "display_numbering.h"
#include "i_moonraker_api.h"
#include "json_utils.h"
#include "lvgl.h"
#include "macro_param_cache.h"
#include "printer_discovery.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>

namespace helix::indx {
namespace {

using json = nlohmann::json;

constexpr const char* kToolPositions = "TOOL_POSITIONS";
constexpr const char* kToolchangeVars = "_TOOLCHANGE_VARS";
constexpr const char* kChangeTool = "CHANGE_TOOL";
constexpr const char* kParkTool = "PARK_TOOL";
constexpr const char* kLoadFilament = "LOAD_FILAMENT";
constexpr const char* kCycleAll = "TC_CYCLE_ALL";
constexpr const char* kManualRemove = "MANUAL_TOOL_REMOVE";
constexpr const char* kManualSeat = "MANUAL_TOOL_SEAT";
constexpr const char* kSetToolFilament = "SET_TOOL_FILAMENT";
constexpr std::array<const char*, 3> kModeMacros = {"MODE_STEALTH", "MODE_NORMAL", "MODE_SPORT"};

/// A strict int: Klipper serializes these variables as JSON ints, and a
/// float or string here is a macro set this module does not understand.
std::optional<int> strict_int(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<int>();
}

/// The macro name a status key or object name refers to, upper-case; empty
/// when it is not a gcode_macro.
std::string macro_of(const std::string& key) {
    static constexpr std::string_view kPrefix = "gcode_macro ";
    if (key.size() <= kPrefix.size() ||
        helix::text_io::to_lower(std::string_view(key).substr(0, kPrefix.size())) != kPrefix) {
        return {};
    }
    return helix::text_io::to_upper(std::string_view(key).substr(kPrefix.size()));
}

/// `T<n>` -> n for 0 <= n < kMaxTools, else nullopt.
std::optional<int> tool_macro_index(const std::string& upper_macro) {
    if (upper_macro.size() < 2 || upper_macro[0] != 'T') {
        return std::nullopt;
    }
    const std::string_view digits = std::string_view(upper_macro).substr(1);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    auto n = helix::text_io::parse_leading<int>(digits);
    if (!n || *n >= kMaxTools) {
        return std::nullopt;
    }
    return n;
}

/// The subscription key for @p macro: the config-case object name.
std::string macro_object(const PrinterDiscovery& hw, const char* macro) {
    const std::string configured = hw.macro_config_name(macro);
    return "gcode_macro " + (configured.empty() ? std::string(macro) : configured);
}

bool read_save_variables(const json& vars, Reading& r) {
    bool saw = false;
    if (auto v = strict_int(vars, "active_tool")) {
        r.active_tool = *v;
        saw = true;
    }
    if (auto v = strict_int(vars, "seat_suspect")) {
        r.seat_suspect = *v;
        saw = true;
    }
    if (auto v = strict_int(vars, "toolchange_count")) {
        r.toolchange_count = *v;
        saw = true;
    }
    return saw;
}

bool read_tool_positions(const json& obj, Reading& r) {
    bool saw = false;
    if (auto v = strict_int(obj, "tool_count")) {
        r.tool_count = std::clamp(*v, 0, kMaxTools);
        saw = true;
    }
    auto nh = obj.find("no_heat_tools");
    if (nh != obj.end() && nh->is_array()) {
        std::set<int> tools;
        for (const auto& t : *nh) {
            if (t.is_number_integer()) {
                tools.insert(t.get<int>());
            }
        }
        r.no_heat_tools = std::move(tools);
        saw = true;
    }
    auto dc = obj.find("docks_calibrated");
    if (dc != obj.end() && dc->is_boolean()) {
        r.docks_calibrated = dc->get<bool>();
        saw = true;
    }
    return saw;
}

bool read_toolchange_vars(const json& obj, Reading& r) {
    bool saw = false;
    if (auto v = strict_int(obj, "toolhead_state")) {
        r.toolhead_state = *v;
        saw = true;
    }
    auto mode = obj.find("mode");
    if (mode != obj.end() && mode->is_string()) {
        r.speed_mode = mode->get<std::string>();
        saw = true;
    }
    return saw;
}

bool read_presets(const json& obj, Reading& r) {
    auto it = obj.find("filament_presets");
    if (it == obj.end() || !it->is_object()) {
        return false;
    }
    std::map<std::string, FilamentPreset> presets;
    for (const auto& [name, preset] : it->items()) {
        if (!preset.is_object()) {
            continue;
        }
        FilamentPreset p;
        p.temp_c = json_util::safe_float(preset, "temp");
        p.density = json_util::safe_float(preset, "density");
        p.heat_capacity = json_util::safe_float(preset, "hc");
        presets[name] = p;
    }
    r.presets = std::move(presets);
    return true;
}

} // namespace

std::optional<Reading> read_status(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    Reading r;
    bool saw = false;
    for (const auto& [key, value] : status.items()) {
        if (!value.is_object()) {
            continue;
        }
        if (key == "save_variables") {
            auto vars = value.find("variables");
            if (vars != value.end() && vars->is_object()) {
                saw |= read_save_variables(*vars, r);
            }
            continue;
        }
        const std::string macro = macro_of(key);
        if (macro == kToolPositions) {
            saw |= read_tool_positions(value, r);
        } else if (macro == kToolchangeVars) {
            saw |= read_toolchange_vars(value, r);
        } else if (macro == kLoadFilament) {
            saw |= read_presets(value, r);
        } else if (auto tool = tool_macro_index(macro)) {
            if (auto id = strict_int(value, "spool_id")) {
                r.tool_spool_ids[*tool] = *id;
                saw = true;
            }
        }
    }
    if (!saw) {
        return std::nullopt;
    }
    return r;
}

void merge(Reading& into, const Reading& update) {
    const auto take = [](auto& held, const auto& fresh) {
        if (fresh.has_value()) {
            held = fresh;
        }
    };
    take(into.active_tool, update.active_tool);
    take(into.seat_suspect, update.seat_suspect);
    take(into.toolchange_count, update.toolchange_count);
    take(into.toolhead_state, update.toolhead_state);
    take(into.speed_mode, update.speed_mode);
    take(into.tool_count, update.tool_count);
    take(into.no_heat_tools, update.no_heat_tools);
    take(into.docks_calibrated, update.docks_calibrated);
    take(into.presets, update.presets);
    for (const auto& [tool, id] : update.tool_spool_ids) {
        into.tool_spool_ids[tool] = id;
    }
}

bool present(const PrinterDiscovery& hw) {
    return hw.has_indx();
}

Contract check_contract(const PrinterDiscovery& hw) {
    Contract c;
    if (!hw.has_save_variables()) {
        c.missing.emplace_back("save_variables");
    }
    for (const char* macro : {kToolPositions, kToolchangeVars, kChangeTool, kParkTool}) {
        if (!hw.has_macro(macro)) {
            c.missing.push_back(std::string("gcode_macro ") + macro);
        }
    }
    c.recognized = c.missing.empty();
    c.has_load_filament = hw.has_macro(kLoadFilament);
    c.has_speed_modes = std::all_of(kModeMacros.begin(), kModeMacros.end(),
                                    [&](const char* m) { return hw.has_macro(m); });
    c.has_cycle_all = hw.has_macro(kCycleAll);
    c.has_manual_tool_seat = hw.has_macro(kManualSeat);
    c.has_manual_tool_recovery = c.has_manual_tool_seat && hw.has_macro(kManualRemove);
    c.has_set_tool_filament = hw.has_macro(kSetToolFilament);
    return c;
}

std::string missing_text(const Contract& contract) {
    std::string text;
    for (const auto& name : contract.missing) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

bool macros_set_active_spool(const PrinterDiscovery& hw) {
    const auto& cache = MacroParamCache::instance();
    for (const auto& macro : hw.macros()) {
        if (macro == kChangeTool || tool_macro_index(macro)) {
            if (cache.get(macro).sets_active_spool) {
                return true;
            }
        }
    }
    return false;
}

nlohmann::json required_status_objects(const PrinterDiscovery& hw) {
    json objects = json::object();
    if (!present(hw)) {
        return objects;
    }
    objects["save_variables"] = json::array({"variables"});
    if (hw.has_macro(kToolPositions)) {
        objects[macro_object(hw, kToolPositions)] =
            json::array({"tool_count", "no_heat_tools", "docks_calibrated"});
    }
    if (hw.has_macro(kToolchangeVars)) {
        objects[macro_object(hw, kToolchangeVars)] = json::array({"toolhead_state", "mode"});
    }
    if (hw.has_macro(kLoadFilament)) {
        objects[macro_object(hw, kLoadFilament)] = json::array({"filament_presets"});
    }
    for (const auto& macro : hw.macros()) {
        if (tool_macro_index(macro)) {
            objects[macro_object(hw, macro.c_str())] = json::array({"spool_id"});
        }
    }
    return objects;
}

std::string change_tool_gcode(const PrinterDiscovery& hw, int tool) {
    const std::string t_macro =
        "T" + std::to_string(tool); // DISPLAY_NUMBERING_OK: gcode macro name
    if (hw.has_macro(t_macro)) {
        return t_macro;
    }
    return fmt::format("{} TOOL={}", kChangeTool, tool);
}

std::string park_gcode() {
    return kParkTool;
}

std::optional<std::string>
preset_for_material(const std::string& material,
                    const std::map<std::string, FilamentPreset>& presets) {
    if (material.empty() || !IMoonrakerAPI::is_safe_material_param(material)) {
        return std::nullopt;
    }
    std::vector<std::string> keys;
    for (const auto& [key, preset] : presets) {
        if (IMoonrakerAPI::is_safe_gcode_param(key)) {
            keys.push_back(key);
        }
    }
    return match_material(material, keys);
}

std::string load_gcode(int tool, const std::string& material, int temp_c,
                       const std::map<std::string, FilamentPreset>& presets) {
    if (auto key = preset_for_material(material, presets)) {
        return fmt::format("{} TOOL={} TYPE={}", kLoadFilament, tool, *key);
    }
    return fmt::format("{} TOOL={} TEMP={}", kLoadFilament, tool, temp_c);
}

int config_tool_count() {
    const CachedMacroInfo info = MacroParamCache::instance().get(kToolPositions);
    auto it = info.variables.find("tool_count");
    if (it == info.variables.end()) {
        return 0;
    }
    return std::clamp(helix::text_io::parse_leading<int>(it->second).value_or(0), 0, kMaxTools);
}

std::string heat_blocked_reason(int active_tool, const std::set<int>& no_heat_tools) {
    if (active_tool < 0) {
        return lv_tr("No tool on the head");
    }
    if (no_heat_tools.count(active_tool)) {
        return fmt::format(fmt::runtime(lv_tr("{} has no hotend")), tool_text(active_tool));
    }
    return {};
}

namespace {

/// The checks every INDX command shares: a recognized contract and no tool
/// half seated on the head.
std::string contract_or_seat_reason(const Contract& contract, const Reading& state) {
    if (!contract.recognized) {
        return lv_tr("INDX macros not recognized");
    }
    if (state.seat_suspect.value_or(-1) >= 0) {
        return fmt::format(
            fmt::runtime(lv_tr("{} may be half seated on the head. Check it by hand.")),
            tool_text(*state.seat_suspect));
    }
    return {};
}

} // namespace

std::string motion_blocked_reason(const Contract& contract, const Reading& state) {
    if (auto reason = contract_or_seat_reason(contract, state); !reason.empty()) {
        return reason;
    }
    if (state.docks_calibrated.has_value() && !*state.docks_calibrated) {
        return lv_tr("INDX dock positions are not calibrated");
    }
    return {};
}

std::string tool_text(int tool) {
    return helix::ui::tool_label_in_form(helix::ui::LaneNoun::Tool, helix::ui::tool_label(tool),
                                         tool, helix::ui::ToolLabelForm::Full);
}

const char* speed_mode_option(SpeedMode mode) {
    switch (mode) {
    case SpeedMode::Stealth:
        return "Stealth";
    case SpeedMode::Normal:
        return "Normal";
    case SpeedMode::Sport:
        return "Sport";
    }
    return "Normal";
}

std::optional<SpeedMode> parse_speed_mode(std::string_view text) {
    const std::string lower = helix::text_io::to_lower(text);
    for (SpeedMode mode : kSpeedModes) {
        if (lower == helix::text_io::to_lower(speed_mode_option(mode))) {
            return mode;
        }
    }
    return std::nullopt;
}

std::string speed_mode_gcode(SpeedMode mode) {
    return kModeMacros[static_cast<size_t>(mode)];
}

std::string cycle_all_gcode() {
    return kCycleAll;
}

std::string manual_remove_gcode() {
    return kManualRemove;
}

std::string manual_seat_gcode(int tool) {
    return fmt::format("{} TOOL={}", kManualSeat, tool);
}

std::string calibration_gcode(HeaterCalibration which) {
    return which == HeaterCalibration::Heater ? "INDX_CALIBRATE" : "INDX_FAN_CALIBRATE";
}

std::string calibration_blocked_reason(const Contract& contract, const Reading& state,
                                       HeaterCalibration which, double nozzle_target_c) {
    if (auto reason = contract_or_seat_reason(contract, state); !reason.empty()) {
        return reason;
    }
    if (auto reason = heat_blocked_reason(state.active_tool.value_or(-1),
                                          state.no_heat_tools.value_or(std::set<int>{}));
        !reason.empty()) {
        return reason;
    }
    if (which == HeaterCalibration::Heater && nozzle_target_c > 0) {
        return lv_tr("Turn the nozzle heater off first");
    }
    return {};
}

std::string set_tool_filament_gcode(int tool, const FilamentPreset& preset) {
    return fmt::format("{} TOOL={} DENSITY={:.3f} HEAT_CAPACITY={:.3f}", kSetToolFilament, tool,
                       preset.density, preset.heat_capacity);
}

bool supports_seat_by_hand(const PrinterDiscovery& hw) {
    if (!present(hw)) {
        return false;
    }
    const Contract contract = check_contract(hw);
    return contract.recognized && contract.has_manual_tool_seat;
}

std::string seat_confirm_message(int tool) {
    return fmt::format(fmt::runtime(lv_tr("Hold {} fully on the head. The latch locks onto it "
                                          "and it becomes the mounted tool.")),
                       tool_text(tool));
}

std::string remove_confirm_message(int tool) {
    return fmt::format(fmt::runtime(lv_tr("The latch opens and {} can drop. Hold it before you "
                                          "continue, then put it in its dock.")),
                       tool_text(tool));
}

} // namespace helix::indx
