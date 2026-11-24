// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_model.h"

#include "json_utils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string>

#include "hv/json.hpp"

namespace helix::pa {

const char* model_name(ModelKind kind) {
    switch (kind) {
    case ModelKind::Recipr:
        return "recipr";
    case ModelKind::Tanh:
        return "tanh";
    case ModelKind::Classic:
    case ModelKind::Linear:
        break;
    }
    return "linear";
}

namespace {

const char* param_name(NlParam p) {
    switch (p) {
    case NlParam::Offset:
        return "OFFSET";
    case NlParam::TimeOffset:
        return "TIME_OFFSET";
    case NlParam::Advance:
        break;
    }
    return "ADVANCE";
}

/// Six decimals, the precision the firmware itself reports these at.
std::string num(float v) {
    return fmt::format("{:.6f}", v);
}

std::optional<ModelKind> kind_from_name(const std::string& name) {
    if (name == "linear") {
        return ModelKind::Linear;
    }
    if (name == "tanh") {
        return ModelKind::Tanh;
    }
    if (name == "recipr") {
        return ModelKind::Recipr;
    }
    return std::nullopt;
}

void read_into(const nlohmann::json& status, const char* key, std::optional<float>& out) {
    if (const auto v = json_util::read_field<float>(status, key)) {
        out = *v;
    }
}

} // namespace

PaModel read_pa_model(const nlohmann::json& extruder_status, PaModel prev) {
    if (const auto name =
            json_util::read_field<std::string>(extruder_status, "pressure_advance_model")) {
        if (const auto kind = kind_from_name(*name); kind && *kind != prev.kind) {
            prev.kind = *kind;
            // Each side reports only its own parameters; tanh and recipr share theirs.
            if (prev.nonlinear()) {
                prev.k.reset();
            } else {
                prev.linear_advance.reset();
                prev.nonlinear_offset.reset();
                prev.linearization_velocity.reset();
            }
        }
    }
    read_into(extruder_status, "pressure_advance", prev.k);
    read_into(extruder_status, "linear_advance", prev.linear_advance);
    read_into(extruder_status, "nonlinear_offset", prev.nonlinear_offset);
    read_into(extruder_status, "linearization_velocity", prev.linearization_velocity);
    read_into(extruder_status, "time_offset", prev.time_offset);
    read_into(extruder_status, "smooth_time", prev.smooth_time);
    return prev;
}

std::optional<NonlinearValues> nonlinear_values(const PaModel& m) {
    if (!m.nonlinear() || !m.linear_advance || !m.nonlinear_offset || !m.linearization_velocity) {
        return std::nullopt;
    }
    NonlinearValues v;
    v.model = m.kind;
    v.linear_advance = *m.linear_advance;
    v.nonlinear_offset = *m.nonlinear_offset;
    v.linearization_velocity = *m.linearization_velocity;
    v.time_offset = m.time_offset.value_or(0.0f);
    v.smooth_time = m.smooth_time.value_or(kNonlinearSmoothTime);
    return v;
}

std::string set_nonlinear_gcode(const std::string& extruder, const NonlinearValues& v) {
    const ModelKind model = v.model == ModelKind::Recipr ? ModelKind::Recipr : ModelKind::Tanh;
    return fmt::format("SET_PRESSURE_ADVANCE EXTRUDER={} MODEL={} ADVANCE={} OFFSET={} VELOCITY={} "
                       "TIME_OFFSET={} SMOOTH_TIME={}",
                       extruder, model_name(model), num(v.linear_advance), num(v.nonlinear_offset),
                       num(v.linearization_velocity), num(v.time_offset), num(v.smooth_time));
}

std::string restore_model_gcode(const std::string& extruder, const PaModel& original) {
    if (const auto v = nonlinear_values(original)) {
        return set_nonlinear_gcode(extruder, *v);
    }
    std::string g = fmt::format("SET_PRESSURE_ADVANCE EXTRUDER={} MODEL=linear ADVANCE={}",
                                extruder, num(original.k.value_or(0.0f)));
    if (original.time_offset) {
        g += " TIME_OFFSET=" + num(*original.time_offset);
    }
    if (original.smooth_time) {
        g += " SMOOTH_TIME=" + num(*original.smooth_time);
    }
    return g;
}

std::string disarm_tuning_tower_gcode() {
    return "TUNING_TOWER COMMAND=G4 PARAMETER=P START=0 FACTOR=0";
}

bool has_tower_module(const std::vector<std::string>& printer_objects) {
    return std::find(printer_objects.begin(), printer_objects.end(), kTowerObject) !=
           printer_objects.end();
}

std::pair<std::string, std::string> tower_section_key() {
    // Kalico's guide: keep the slow segment at 20 mm/s to probe the speeds of
    // overhangs and seams.
    return {"slow_velocity", "20"};
}

float value_at(const TowerSweep& sweep, float height_mm) {
    return std::max(0.0f, sweep.start + std::max(0.0f, height_mm) * sweep.factor);
}

std::string tuning_tower_gcode(const TowerSweep& sweep) {
    return fmt::format("TUNING_TOWER COMMAND=SET_PRESSURE_ADVANCE PARAMETER={} START={} FACTOR={}",
                       param_name(sweep.param), num(sweep.start), num(sweep.factor));
}

std::string print_tower_gcode(float nozzle_diameter_mm, int target_temp, int fast_velocity_mm_s) {
    std::string g =
        fmt::format("PRINT_PA_TOWER NOZZLE={:.2f} TARGET_TEMP={}", nozzle_diameter_mm, target_temp);
    if (fast_velocity_mm_s > 0) {
        g += fmt::format(" FAST_VELOCITY={}", fast_velocity_mm_s);
    }
    return g;
}

std::vector<ExtruderConfigKey> nonlinear_config_keys(const NonlinearValues& v) {
    const ModelKind model = v.model == ModelKind::Recipr ? ModelKind::Recipr : ModelKind::Tanh;
    return {
        {"pressure_advance", "", true},
        {"pressure_advance_model", model_name(model)},
        {"linear_advance", num(v.linear_advance)},
        {"nonlinear_offset", num(v.nonlinear_offset)},
        {"linearization_velocity", num(v.linearization_velocity)},
        {"pressure_advance_time_offset", num(v.time_offset)},
        {"pressure_advance_smooth_time", num(v.smooth_time)},
    };
}

} // namespace helix::pa
