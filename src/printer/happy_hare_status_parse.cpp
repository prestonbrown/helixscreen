// src/printer/happy_hare_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "happy_hare_status_parse.h"

#include "ams_status_json.h"
#include "json_utils.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace helix::happy_hare {
namespace {

namespace tio = ::helix::text_io;

std::optional<int> read_number_as_int(const nlohmann::json& v) {
    if (v.is_number()) {
        return v.get<int>();
    }
    return std::nullopt;
}

/// gate_color_rgb entry: a 0xRRGGBB integer (traditional), or [r, g, b] floats
/// in 0..1 (EMU).
std::optional<uint32_t> read_gate_rgb(const nlohmann::json& v) {
    if (v.is_number_integer()) {
        return static_cast<uint32_t>(v.get<int>());
    }
    if (v.is_array() && v.size() >= 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) {
        const auto channel = [&](size_t i) {
            return static_cast<uint8_t>(std::clamp(v[i].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
        };
        return (static_cast<uint32_t>(channel(0)) << 16) |
               (static_cast<uint32_t>(channel(1)) << 8) | static_cast<uint32_t>(channel(2));
    }
    return std::nullopt;
}

std::optional<ams::ColorReading> read_gate_color(const nlohmann::json& v) {
    if (!v.is_string()) {
        return std::nullopt;
    }
    return ams::read_lane_color(v.get<std::string>());
}

/// Per-unit gate counts from `num_gates`: a "6,4" string (dissimilar
/// multi-unit), a plain integer (EMU, one unit) or an array ([8] or [6, 4]).
/// Empty when it names none.
std::vector<int> read_num_gates(const nlohmann::json& ng) {
    std::vector<int> counts;
    if (ng.is_string()) {
        const std::string text = ng.get<std::string>();
        for (std::string_view sv : tio::lines(text, ',')) {
            const std::string token(sv);
            const auto count = tio::parse_leading<int>(token);
            if (!count) {
                spdlog::warn("[AMS HappyHare] Ignoring invalid token in num_gates string");
            } else if (*count > 0) {
                counts.push_back(*count);
            } else {
                spdlog::warn("[AMS HappyHare] Ignoring non-positive gate count {} in "
                             "num_gates string",
                             *count);
            }
        }
    } else if (ng.is_number_integer()) {
        if (ng.get<int>() > 0) {
            counts.push_back(ng.get<int>());
        }
    } else if (ng.is_array()) {
        for (const auto& c : ng) {
            if (c.is_number_integer() && c.get<int>() > 0) {
                counts.push_back(c.get<int>());
            }
        }
    }
    return counts;
}

EncoderDelta read_encoder(const nlohmann::json& encoder) {
    EncoderDelta d;
    d.flow_rate = ams::read_field<int>(encoder, "flow_rate");
    d.desired_headroom = ams::read_field<float>(encoder, "desired_headroom");
    d.detection_length = ams::read_field<float>(encoder, "detection_length");
    d.headroom = ams::read_field<float>(encoder, "headroom");
    d.min_headroom = ams::read_field<float>(encoder, "min_headroom");
    return d;
}

FlowguardDelta read_flowguard(const nlohmann::json& fg) {
    FlowguardDelta d;
    d.enabled = ams::read_field<bool>(fg, "enabled");
    d.active = ams::read_field<bool>(fg, "active");
    d.trigger = ams::read_field<std::string>(fg, "trigger");
    d.level = ams::read_field<float>(fg, "level");
    d.max_clog = ams::read_field<float>(fg, "max_clog");
    d.max_tangle = ams::read_field<float>(fg, "max_tangle");
    d.encoder_mode = ams::read_field<int>(fg, "encoder_mode");
    d.buffer_data = fg.contains("level") || fg.contains("trigger") || fg.contains("max_clog");
    return d;
}

MmuSensorsDelta read_sensors(const nlohmann::json& sensors) {
    MmuSensorsDelta d;
    for (auto it = sensors.begin(); it != sensors.end(); ++it) {
        const std::string& key = it.key();
        for (const std::string_view prefix : {"mmu_pre_gate_", "mmu_entry_"}) {
            if (key.rfind(prefix, 0) != 0) {
                continue;
            }
            const auto gate = tio::parse_leading<int>(key.substr(prefix.size()));
            if (gate && *gate >= 0) {
                d.pre_gate.emplace_back(*gate, it.value().is_boolean() && it.value().get<bool>());
            }
        }
    }
    // v4 publishes a disabled sensor as null, and its parameter guards treat
    // a disabled sensor as not fitted.
    auto fitted = [&sensors](const char* key) {
        const auto it = sensors.find(key);
        return it != sensors.end() && !it->is_null();
    };
    d.has_toolhead_sensor = fitted("toolhead");
    d.has_extruder_sensor = fitted("extruder");
    if (sensors.contains("mmu_pre_gate")) {
        d.aggregate_pre_gate =
            sensors["mmu_pre_gate"].is_boolean() && sensors["mmu_pre_gate"].get<bool>();
    }
    return d;
}

DryingObjectDelta read_drying_object(const nlohmann::json& drying) {
    DryingObjectDelta d;
    d.active = ams::read_field<bool>(drying, "active");
    d.current_temp_c = ams::read_field<float>(drying, "current_temp");
    d.target_temp_c = ams::read_field<float>(drying, "target_temp");
    d.remaining_min = ams::read_integer_field(drying, "remaining_min");
    d.duration_min = ams::read_integer_field(drying, "duration_min");
    d.fan_pct = ams::read_integer_field(drying, "fan_pct");
    return d;
}

/// Which v4 section holds a tunable.
enum class ParamScope { Machine, Unit, Toolhead };

struct ParamRow {
    std::string_view key;  ///< HelixScreen's name
    std::string_view v3;   ///< before 3.42
    std::string_view v342; ///< 3.42 to 3.x
    std::string_view v4;
    ParamScope scope;
    double since = 0; ///< the first v3 version accepting it, 0 for all
};

// Every tunable the backend reads from configfile or sends through
// MMU_TEST_CONFIG, checked against each release's MMU_TEST_CONFIG and config
// readers. v3 refuses a parameter that is not one of its own attributes
// (cmd_MMU_TEST_CONFIG illegal_params). v4 sources: mmu_machine_parameters.py
// (Machine), unit/mmu_unit_parameters.py and the selector parameter classes,
// which read [mmu_unit_parameters] too (Unit), unit/mmu_toolhead_wrapper.py
// (Toolhead).
constexpr ParamRow kParams[] = {
    {"form_tip_macro", "form_tip_macro", "form_tip_macro", "form_tip_macro", ParamScope::Machine},
    {"extruder_load_speed", "extruder_load_speed", "extruder_load_speed", "extruder_load_speed",
     ParamScope::Machine},
    {"extruder_unload_speed", "extruder_unload_speed", "extruder_unload_speed",
     "extruder_unload_speed", ParamScope::Machine},
    {"gear_from_spool_speed", "gear_from_spool_speed", "gear_from_spool_speed", "gear_load_speed",
     ParamScope::Unit},
    {"gear_from_buffer_speed", "gear_from_buffer_speed", "gear_from_buffer_speed",
     "gear_from_filament_buffer_speed", ParamScope::Unit},
    {"gear_unload_speed", "gear_unload_speed", "gear_unload_speed", "gear_unload_speed",
     ParamScope::Unit, 3.10},
    {"selector_move_speed", "selector_move_speed", "selector_move_speed", "selector_move_speed",
     ParamScope::Unit},
    {"sync_to_extruder", "sync_to_extruder", "sync_to_extruder", "sync_to_extruder",
     ParamScope::Unit},
    {"heater_max_temp", "heater_max_temp", "heater_max_temp", "heater_max_temp", ParamScope::Unit},
    // Clog detection mode, 0 off, 1 static (manual), 2 automatic, in every
    // version. In static mode the length is the calibrated clog length before
    // 3.42 and the encoder's maximum motion from 3.42 on.
    {"clog_detection", "enable_clog_detection", "flowguard_encoder_mode", "flowguard_encoder_mode",
     ParamScope::Unit},
    {"detection_length", "mmu_calibration_clog_length", "flowguard_encoder_max_motion",
     "flowguard_encoder_max_motion", ParamScope::Unit},
    {"toolhead_sensor_to_nozzle", "toolhead_sensor_to_nozzle", "toolhead_sensor_to_nozzle",
     "toolhead_sensor_to_nozzle", ParamScope::Toolhead},
    {"toolhead_extruder_to_nozzle", "toolhead_extruder_to_nozzle", "toolhead_extruder_to_nozzle",
     "toolhead_extruder_to_nozzle", ParamScope::Toolhead},
    {"toolhead_entry_to_extruder", "toolhead_entry_to_extruder", "toolhead_entry_to_extruder",
     "toolhead_entry_to_extruder", ParamScope::Toolhead},
    {"toolhead_ooze_reduction", "toolhead_ooze_reduction", "toolhead_ooze_reduction",
     "toolhead_ooze_reduction", ParamScope::Toolhead},
};

const ParamRow* find_param_row(std::string_view key) {
    for (const auto& row : kParams) {
        if (row.key == key) {
            return &row;
        }
    }
    return nullptr;
}

const nlohmann::json* find_member(const nlohmann::json& obj, const std::string& key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

std::string read_string_member(const nlohmann::json& obj, const std::string& key) {
    const auto* v = find_member(obj, key);
    return v && v->is_string() ? v->get<std::string>() : std::string{};
}

/// A Happy Hare config list (e.g. environment_sensors) as trimmed names.
/// Moonraker may return it as a JSON array or as a comma-separated string.
std::vector<std::string> read_config_list(const nlohmann::json* v) {
    std::vector<std::string> out;
    if (!v) {
        return out;
    }
    auto push = [&](std::string_view item) {
        const auto trimmed = tio::trim(item);
        if (!trimmed.empty()) {
            out.emplace_back(trimmed);
        }
    };
    if (v->is_array()) {
        for (const auto& e : *v) {
            if (e.is_string()) {
                push(e.get<std::string>());
            }
        }
    } else if (v->is_string()) {
        const std::string text = v->get<std::string>();
        for (std::string_view item : tio::lines(text, ',')) {
            push(item);
        }
    }
    return out;
}

MachineUnit read_machine_unit(const nlohmann::json& settings, const nlohmann::json& fields) {
    MachineUnit u;
    u.display_name = read_string_member(fields, "display_name");
    u.selector_type = read_string_member(fields, "selector_type");
    if (const auto* first = find_member(fields, "first_gate")) {
        u.first_gate = ams::read_integer(*first).value_or(-1);
    }
    if (const auto* count = find_member(fields, "num_gates")) {
        u.num_gates = std::max(ams::read_integer(*count).value_or(0), 0);
    }
    u.filament_heater = read_string_member(fields, "filament_heater");
    u.environment_sensor = read_string_member(fields, "environment_sensor");
    u.filament_heaters = read_config_list(find_member(fields, "filament_heaters"));
    u.environment_sensors = read_config_list(find_member(fields, "environment_sensors"));
    if (const auto* v = find_member(fields, "has_bypass"); v && v->is_boolean()) {
        u.has_bypass = v->get<bool>();
    }
    if (const auto* v = find_member(fields, "filament_always_gripped"); v && v->is_boolean()) {
        u.filament_always_gripped = v->get<bool>();
    }
    if (const auto* v = find_member(fields, "filament_buffer"); v && v->is_boolean()) {
        u.filament_buffer = v->get<bool>();
    }
    // v4 names the unit's encoder on its own [mmu_unit <name>] section; Klipper
    // lowercases section names in configfile.settings.
    const std::string name = tio::to_lower(read_string_member(fields, "name"));
    if (const auto* cfg = name.empty() ? nullptr : find_member(settings, "mmu_unit " + name)) {
        u.has_encoder = !read_string_member(*cfg, "encoder").empty();
    }
    return u;
}

} // namespace

std::vector<MachineUnit> read_machine_units(const nlohmann::json& settings,
                                            const nlohmann::json& live_mmu_machine) {
    std::vector<MachineUnit> units;
    for (int u = 0;; ++u) {
        const auto* fields = find_member(live_mmu_machine, "unit_" + std::to_string(u));
        if (!fields || !fields->is_object()) {
            break;
        }
        units.push_back(read_machine_unit(settings, *fields));
    }
    if (units.empty()) {
        if (const auto* config = find_member(settings, "mmu_machine");
            config && config->is_object() && !config->empty()) {
            units.push_back(read_machine_unit(settings, *config));
        }
    }
    return units;
}

bool unit_supports(const MachineUnit& unit, UnitFeature feature, bool v4) {
    const std::string& sel = unit.selector_type;
    const bool type_b = sel == "VirtualSelector";
    if (!v4) {
        switch (feature) {
        case UnitFeature::Servo:
        case UnitFeature::SelectorSpeed:
        case UnitFeature::Encoder:
            return !type_b;
        case UnitFeature::SyncToExtruder:
        case UnitFeature::FilamentBuffer:
            return true;
        }
        return true;
    }
    switch (feature) {
    case UnitFeature::Servo:
        // MMU_SERVO is registered by LinearServoSelector and its multi-gear subclass.
        return sel.empty() || sel == "LinearServoSelector" || sel == "LinearMultiGearServoSelector";
    case UnitFeature::SelectorSpeed:
        // selector_move_speed is a parameter of the linear, rotary and indexed selectors.
        return sel.empty() || sel.rfind("Linear", 0) == 0 || sel == "RotarySelector" ||
               sel == "IndexedSelector";
    case UnitFeature::Encoder:
        return unit.has_encoder.value_or(!type_b);
    case UnitFeature::SyncToExtruder:
        return !unit.filament_always_gripped;
    case UnitFeature::FilamentBuffer:
        return unit.filament_buffer.value_or(true);
    }
    return true;
}

UnitObjects collect_unit_objects(const std::vector<MachineUnit>& units, UnitObjectKind kind) {
    const bool heater = kind == UnitObjectKind::Heater;
    auto scalar = [heater](const MachineUnit& u) -> const std::string& {
        return heater ? u.filament_heater : u.environment_sensor;
    };
    auto list = [heater](const MachineUnit& u) -> const std::vector<std::string>& {
        return heater ? u.filament_heaters : u.environment_sensors;
    };

    UnitObjects out;
    if (units.size() == 1) {
        out.shared = scalar(units[0]);
        out.per_gate = list(units[0]);
        return out;
    }
    const bool one_shared =
        !units.empty() && std::all_of(units.begin(), units.end(), [&](const MachineUnit& u) {
            return list(u).empty() && scalar(u) == scalar(units[0]);
        });
    if (one_shared) {
        out.shared = scalar(units[0]);
        return out;
    }
    bool any = false;
    for (const auto& u : units) {
        if (!list(u).empty()) {
            out.per_gate.insert(out.per_gate.end(), list(u).begin(), list(u).end());
        } else {
            out.per_gate.insert(out.per_gate.end(), static_cast<size_t>(u.num_gates), scalar(u));
        }
        any = any || !scalar(u).empty() || !list(u).empty();
    }
    if (!any) {
        out.per_gate.clear();
    }
    return out;
}

MachineLayout read_machine_layout(const nlohmann::json& settings,
                                  const nlohmann::json& live_mmu_machine) {
    MachineLayout layout;
    static const nlohmann::json empty = nlohmann::json::object();
    const auto* config_machine = find_member(settings, "mmu_machine");
    const nlohmann::json& config_mm = config_machine ? *config_machine : empty;

    layout.version = read_string_member(live_mmu_machine, "happy_hare_version");
    if (layout.version.empty()) {
        layout.version = read_string_member(config_mm, "happy_hare_version");
    }
    if (layout.version.empty()) {
        // v3 keeps its version on [mmu], as a number such as 3.42.
        if (const auto* mmu = find_member(settings, "mmu")) {
            if (const auto* v = find_member(*mmu, "happy_hare_version"); v && v->is_number()) {
                char text[32];
                std::snprintf(text, sizeof(text), "%g", v->get<double>());
                layout.version = text;
            } else if (v && v->is_string()) {
                layout.version = v->get<std::string>();
            }
        }
    }
    layout.version_number = tio::parse_leading<double>(layout.version).value_or(0.0);
    layout.v4 = layout.version_number >= 4;
    if (!layout.v4) {
        return layout;
    }

    if (const auto* units = find_member(live_mmu_machine, "num_units")) {
        if (const auto n = ams::read_integer(*units)) {
            layout.num_units = std::max(*n, 1);
        }
    }
    for (const auto& unit : read_machine_units(settings, live_mmu_machine)) {
        if (unit.has_bypass) {
            layout.has_bypass = layout.has_bypass.value_or(false) || *unit.has_bypass;
        }
    }

    // Klipper lowercases section names in configfile.settings; unit and
    // toolhead names keep the case they were configured with.
    std::string unit;
    if (const auto* unit0 = find_member(live_mmu_machine, "unit_0")) {
        unit = read_string_member(*unit0, "name");
    }
    if (unit.empty()) {
        if (const auto* units = find_member(config_mm, "units");
            units && units->is_array() && !units->empty() && (*units)[0].is_string()) {
            unit = (*units)[0].get<std::string>();
        }
    }
    unit = tio::to_lower(unit);
    if (!unit.empty()) {
        layout.unit_params_section = "mmu_unit_parameters " + unit;
        std::string toolhead;
        if (const auto* unit_cfg = find_member(settings, "mmu_unit " + unit)) {
            toolhead = read_string_member(*unit_cfg, "toolhead");
        }
        layout.toolhead_section =
            "mmu_toolhead " + tio::to_lower(toolhead.empty() ? "default" : toolhead);
    }
    return layout;
}

std::string_view param_name(std::string_view key, const MachineLayout& layout) {
    const ParamRow* row = find_param_row(key);
    if (!row) {
        return key;
    }
    if (layout.v4) {
        return row->v4;
    }
    // An unknown version is not refused anything.
    if (layout.version_number > 0 && layout.version_number < row->since) {
        return {};
    }
    return layout.version_number >= 3.42 ? row->v342 : row->v3;
}

bool param_is_per_unit(std::string_view key) {
    const ParamRow* row = find_param_row(key);
    return row && row->scope != ParamScope::Machine;
}

const nlohmann::json* find_config_param(const nlohmann::json& settings, const MachineLayout& layout,
                                        std::string_view key) {
    const std::string name(param_name(key, layout));
    if (name.empty()) {
        return nullptr;
    }
    if (!layout.v4) {
        const auto* mmu = find_member(settings, "mmu");
        return mmu ? find_member(*mmu, name) : nullptr;
    }
    const ParamRow* row = find_param_row(key);
    if (!row) {
        return nullptr;
    }
    std::string section;
    switch (row->scope) {
    case ParamScope::Machine:
        section = "mmu_parameters";
        break;
    case ParamScope::Unit:
        section = layout.unit_params_section;
        break;
    case ParamScope::Toolhead:
        section = layout.toolhead_section;
        break;
    }
    const auto* params = find_member(settings, section);
    return params ? find_member(*params, name) : nullptr;
}

std::optional<float> read_config_number(const nlohmann::json* v) {
    if (!v) {
        return std::nullopt;
    }
    if (v->is_number()) {
        return v->get<float>();
    }
    if (v->is_string()) {
        return tio::parse_leading<float>(v->get<std::string>());
    }
    return std::nullopt;
}

std::vector<EntrySensorReading> parse_entry_sensor_objects(const nlohmann::json& params) {
    std::vector<EntrySensorReading> readings;
    if (!params.is_object()) {
        return readings;
    }
    constexpr std::string_view prefix = "filament_switch_sensor mmu_entry_";
    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string& key = it.key();
        if (key.rfind(prefix, 0) != 0 || !it.value().is_object()) {
            continue;
        }
        const auto gate = tio::parse_leading<int>(key.substr(prefix.size()));
        if (!gate || *gate < 0) {
            continue;
        }
        EntrySensorReading r;
        r.gate = *gate;
        r.detected = ams::read_field<bool>(it.value(), "filament_detected");
        r.enabled = ams::read_field<bool>(it.value(), "enabled");
        if (r.detected || r.enabled) {
            readings.push_back(r);
        }
    }
    return readings;
}

MmuCoreDelta parse_core(const nlohmann::json& mmu) {
    MmuCoreDelta d;
    d.gate = ams::read_integer_field(mmu, "gate");
    d.tool = ams::read_integer_field(mmu, "tool");
    if (const auto filament = ams::read_field<std::string>(mmu, "filament")) {
        d.filament_loaded = (*filament == "Loaded");
    }
    d.reason_for_pause = ams::read_field<std::string>(mmu, "reason_for_pause");
    d.action = ams::read_field<std::string>(mmu, "action");
    d.filament_pos = ams::read_integer_field(mmu, "filament_pos");
    if (const auto progress = ams::read_integer_field(mmu, "bowden_progress")) {
        d.bowden_progress = std::clamp(*progress, -1, 100);
    }
    d.has_bypass = ams::read_field<bool>(mmu, "has_bypass");
    return d;
}

MmuTopologyDelta parse_topology(const nlohmann::json& mmu) {
    MmuTopologyDelta d;
    if (const auto units = ams::read_integer_field(mmu, "num_units")) {
        d.num_units = std::max(*units, 1);
    }

    std::vector<int> counts;
    if (mmu.contains("num_gates")) {
        counts = read_num_gates(mmu["num_gates"]);
    }
    // An explicit unit_gate_counts array wins over num_gates.
    if (const auto unit_counts = ams::read_array<int>(mmu, "unit_gate_counts", ams::read_integer)) {
        std::vector<int> explicit_counts;
        for (const auto& c : *unit_counts) {
            if (c) {
                explicit_counts.push_back(*c);
            }
        }
        if (!explicit_counts.empty()) {
            counts = std::move(explicit_counts);
        }
    }
    if (!counts.empty()) {
        d.gate_counts = std::move(counts);
    }

    d.active_unit = ams::read_integer_field(mmu, "unit");

    if (const auto ttg = ams::read_array<int>(mmu, "ttg_map", ams::read_integer)) {
        std::vector<int> map;
        map.reserve(ttg->size());
        for (const auto& mapping : *ttg) {
            if (mapping) {
                map.push_back(*mapping);
            }
        }
        d.ttg_map = std::move(map);
    }
    return d;
}

GateIdentityDelta parse_gate_identity(const nlohmann::json& mmu) {
    GateIdentityDelta d;
    d.gate_status = ams::read_array<int>(mmu, "gate_status", ams::read_integer);
    d.color_rgb = ams::read_array<uint32_t>(mmu, "gate_color_rgb", read_gate_rgb);
    d.color = ams::read_array<ams::ColorReading>(mmu, "gate_color", read_gate_color);
    d.material = ams::read_array<std::string>(mmu, "gate_material");
    d.spool_id = ams::read_array<int>(mmu, "gate_spool_id", ams::read_integer);
    d.temperature = ams::read_array<int>(mmu, "gate_temperature", read_number_as_int);
    d.name = ams::read_array<std::string>(mmu, "gate_name");
    d.filament_name = ams::read_array<std::string>(mmu, "gate_filament_name");
    d.endless_spool_group = ams::read_array<int>(mmu, "endless_spool_groups", ams::read_integer);
    return d;
}

MmuTelemetryDelta parse_telemetry(const nlohmann::json& mmu) {
    MmuTelemetryDelta d;
    d.espooler_active = ams::read_field<std::string>(mmu, "espooler_active");
    if (const auto it = mmu.find("espooler"); it != mmu.end() && it->is_array()) {
        std::vector<std::string> ops;
        ops.reserve(it->size());
        for (const auto& op : *it) {
            ops.push_back(op.is_string() ? op.get<std::string>() : std::string{});
        }
        d.espooler = std::move(ops);
    }
    d.sync_feedback_state = ams::read_field<std::string>(mmu, "sync_feedback_state");
    d.sync_feedback_bias = ams::read_field<float>(mmu, "sync_feedback_bias_modelled");
    d.sync_feedback_bias_raw = ams::read_field<float>(mmu, "sync_feedback_bias_raw");
    d.sync_drive = ams::read_field<bool>(mmu, "sync_drive");
    d.clog_detection_enabled = ams::read_integer_field(mmu, "clog_detection_enabled");

    if (mmu.contains("encoder") && mmu["encoder"].is_object()) {
        d.encoder = read_encoder(mmu["encoder"]);
    }
    if (mmu.contains("flowguard") && mmu["flowguard"].is_object()) {
        d.flowguard = read_flowguard(mmu["flowguard"]);
    }
    if (mmu.contains("leds") && mmu["leds"].is_object()) {
        const auto& leds = mmu["leds"];
        if (leds.contains("unit0") && leds["unit0"].is_object()) {
            d.led_exit_effect = ams::read_field<std::string>(leds["unit0"], "exit_effect");
        }
    }

    d.sync_feedback_flow_rate = ams::read_field<float>(mmu, "sync_feedback_flow_rate");
    d.toolchange_purge_volume = ams::read_field<float>(mmu, "toolchange_purge_volume");

    // The count of completed tool changes (1 = first swap done) as a 0-based
    // index.
    if (const auto count = ams::read_integer_field(mmu, "num_toolchanges")) {
        d.current_toolchange = (*count > 0) ? (*count - 1) : -1;
    }
    if (mmu.contains("slicer_tool_map") && mmu["slicer_tool_map"].is_object()) {
        d.number_of_toolchanges =
            ams::read_integer_field(mmu["slicer_tool_map"], "total_toolchanges").value_or(0);
    }

    if (const auto mode = ams::read_field<std::string>(mmu, "spoolman_support")) {
        d.spoolman_mode = spoolman_mode_from_string(*mode);
    }
    d.pending_spool_id = ams::read_integer_field(mmu, "pending_spool_id");

    auto is_null = [&mmu](const char* key) {
        const auto it = mmu.find(key);
        return it != mmu.end() && it->is_null();
    };
    d.sync_feedback_bias_null = is_null("sync_feedback_bias_modelled");
    d.sync_feedback_bias_raw_null = is_null("sync_feedback_bias_raw");
    d.flowguard_null = is_null("flowguard");
    d.encoder_null = is_null("encoder");
    d.v4_marker = mmu.contains("tangle_prevention");
    return d;
}

MmuStatusDelta parse_mmu_status(const nlohmann::json& mmu) {
    MmuStatusDelta d;
    d.core = parse_core(mmu);
    d.topology = parse_topology(mmu);
    d.identity = parse_gate_identity(mmu);
    d.telemetry = parse_telemetry(mmu);

    if (mmu.contains("sensors") && mmu["sensors"].is_object()) {
        d.sensors = read_sensors(mmu["sensors"]);
    }

    if (mmu.contains("drying_state")) {
        const auto& drying = mmu["drying_state"];
        if (drying.is_object()) {
            DryingDelta dd;
            dd.object = read_drying_object(drying);
            d.drying = std::move(dd);
        } else if (drying.is_array()) {
            // One state per gate: "active"/"queued" heat, anything else is off. A
            // non-string keeps its place as an empty string so the vector stays
            // indexable by gate number.
            DryingDelta dd;
            std::vector<std::string> states;
            states.reserve(drying.size());
            for (const auto& entry : drying) {
                states.push_back(entry.is_string() ? entry.get<std::string>() : std::string{});
            }
            dd.per_gate = std::move(states);
            d.drying = std::move(dd);
        }
    }

    // Happy Hare publishes the endless-spool ENABLE bit under two keys, both
    // tagged DEPRECATED in mmu.py's get_status() with no replacement shipped:
    // read the newer spelling and fall back to the older one.
    for (const char* key : {"endless_spool_enabled", "endless_spool"}) {
        if (mmu.contains(key) && !mmu[key].is_null()) {
            d.endless_spool_enabled = json_util::safe_bool(mmu, key, false);
            break;
        }
    }
    return d;
}

} // namespace helix::happy_hare
