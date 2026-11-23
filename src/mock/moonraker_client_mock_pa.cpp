// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The mock's pressure advance: the per-extruder state SET_PRESSURE_ADVANCE
// edits, the model-aware status it reports (HELIX_MOCK_PA_MODEL), and the
// firmware's pa_test tower (HELIX_MOCK_PA_TEST): TUNING_TOWER arms a sweep and
// PRINT_PA_TOWER prints the tower, riding the mock's ordinary print phase.

#include "core/gcode_params.h"
#include "core/text_io.h"
#include "env_knobs.h"
#include "moonraker_client_mock.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

using ModelKind = MoonrakerClientMock::PaModelKind;

namespace {

constexpr double kTowerHeightMm = 50.0;
constexpr double kTowerLayerHeightMm = 0.2;
constexpr double kTowerFirstLayerHeightMm = 0.2;
/// Simulated seconds one tower layer takes, so a full tower is about two
/// minutes at 1x.
constexpr double kTowerSecondsPerLayer = 0.5;
constexpr double kTimeOffsetLimit = 0.2;

ModelKind env_model() {
    const char* env = std::getenv("HELIX_MOCK_PA_MODEL");
    if (!env) {
        return ModelKind::Classic;
    }
    const std::string name(env);
    if (name == "linear") {
        return ModelKind::Linear;
    }
    if (name == "tanh") {
        return ModelKind::Tanh;
    }
    if (name == "recipr") {
        return ModelKind::Recipr;
    }
    return ModelKind::Classic;
}

std::optional<ModelKind> model_from_name(const std::string& name) {
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

const char* model_name(ModelKind kind) {
    switch (kind) {
    case ModelKind::Linear:
        return "linear";
    case ModelKind::Tanh:
        return "tanh";
    case ModelKind::Recipr:
        return "recipr";
    case ModelKind::Classic:
        break;
    }
    return "";
}

bool is_nonlinear(ModelKind kind) {
    return kind == ModelKind::Tanh || kind == ModelKind::Recipr;
}

std::optional<double> number_param(const std::string& gcode, const char* key) {
    auto v = helix::gcode::gcode_param_value(gcode, key);
    return v ? helix::text_io::parse_leading<double>(*v) : std::nullopt;
}

} // namespace

bool MoonrakerClientMock::pa_test_enabled() {
    return helix::env_flag("HELIX_MOCK_PA_TEST");
}

MoonrakerClientMock::PressureAdvance
MoonrakerClientMock::pressure_advance_for(const std::string& extruder) const {
    std::lock_guard<std::mutex> lock(pressure_advance_mutex_);
    auto it = pressure_advance_.find(extruder);
    if (it != pressure_advance_.end()) {
        return it->second;
    }
    PressureAdvance fresh;
    fresh.model = env_model();
    if (is_nonlinear(fresh.model)) {
        fresh.advance = 0.0;
    }
    return fresh;
}

json MoonrakerClientMock::pressure_advance_status(const PressureAdvance& pa) {
    json status = {{"smooth_time", pa.smooth_time}};
    switch (pa.model) {
    case ModelKind::Classic:
        status["pressure_advance"] = pa.advance;
        break;
    case ModelKind::Linear:
        status["pressure_advance"] = pa.advance;
        status["pressure_advance_model"] = model_name(pa.model);
        status["time_offset"] = pa.time_offset;
        break;
    case ModelKind::Tanh:
    case ModelKind::Recipr:
        status["pressure_advance_model"] = model_name(pa.model);
        status["linear_advance"] = pa.advance;
        status["nonlinear_offset"] = pa.offset;
        status["linearization_velocity"] = pa.velocity;
        status["time_offset"] = pa.time_offset;
        break;
    }
    return status;
}

MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_set_pressure_advance(const std::string& gcode) {
    // Klipper defaults EXTRUDER to the active one; the mock prints on "extruder".
    const std::string extruder =
        helix::gcode::gcode_param_value(gcode, "EXTRUDER").value_or("extruder");
    PressureAdvance pa = pressure_advance_for(extruder);

    auto reject = [&](std::string message) -> GcodeResult {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = std::move(message);
        return 1;
    };

    if (pa.model == ModelKind::Classic) {
        if (auto v = number_param(gcode, "ADVANCE")) {
            pa.advance = std::max(0.0, *v);
        }
        if (auto v = number_param(gcode, "SMOOTH_TIME")) {
            pa.smooth_time = std::max(0.0, *v);
        }
    } else {
        if (auto name = helix::gcode::gcode_param_value(gcode, "MODEL")) {
            auto kind = model_from_name(*name);
            if (!kind) {
                return reject(fmt::format("Invalid MODEL='{}' choice", *name));
            }
            pa.model = *kind;
        }
        if (auto v = number_param(gcode, "ADVANCE")) {
            pa.advance = std::max(0.0, *v);
        }
        if (auto v = number_param(gcode, "OFFSET")) {
            pa.offset = std::max(0.0, *v);
        }
        if (auto v = number_param(gcode, "VELOCITY")) {
            pa.velocity = *v;
        }
        if (auto v = number_param(gcode, "TIME_OFFSET")) {
            if (std::abs(*v) > kTimeOffsetLimit) {
                return reject(fmt::format("TIME_OFFSET={} must be between -{} and {}", *v,
                                          kTimeOffsetLimit, kTimeOffsetLimit));
            }
            pa.time_offset = *v;
        }
        if (auto v = number_param(gcode, "SMOOTH_TIME")) {
            pa.smooth_time = std::max(0.0, *v);
        }
        if (is_nonlinear(pa.model) && pa.offset > 0.0 && pa.velocity <= 0.0) {
            return reject("VELOCITY must be positive when OFFSET is set");
        }
    }
    {
        std::lock_guard<std::mutex> lock(pressure_advance_mutex_);
        pressure_advance_[extruder] = pa;
    }
    spdlog::info("[MoonrakerClientMock] SET_PRESSURE_ADVANCE {} model={} ADVANCE={:.4f} "
                 "OFFSET={:.4f} TIME_OFFSET={:.4f} SMOOTH_TIME={:.4f}",
                 extruder, model_name(pa.model), pa.advance, pa.offset, pa.time_offset,
                 pa.smooth_time);
    dispatch_status_update({{extruder, pressure_advance_status(pa)}});
    // Klipper's own acknowledgement, so console consumers see the same text.
    if (is_nonlinear(pa.model)) {
        dispatch_gcode_response(fmt::format("linear_advance: {:.6f}", pa.advance));
        dispatch_gcode_response(fmt::format("nonlinear_offset: {:.6f}", pa.offset));
    } else {
        dispatch_gcode_response(fmt::format("pressure_advance: {:.6f}", pa.advance));
    }
    dispatch_gcode_response(fmt::format("pressure_advance_smooth_time: {:.6f}", pa.smooth_time));
    return std::nullopt;
}

// TUNING_TOWER COMMAND=SET_PRESSURE_ADVANCE PARAMETER=<p> START=<f> FACTOR=<f>
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_tuning_tower(const std::string& gcode) {
    if (helix::gcode::gcode_param_value(gcode, "COMMAND").value_or("") != "SET_PRESSURE_ADVANCE") {
        return std::nullopt;
    }
    const std::string parameter = helix::gcode::gcode_param_value(gcode, "PARAMETER").value_or("");
    if (parameter != "ADVANCE" && parameter != "OFFSET" && parameter != "TIME_OFFSET") {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ =
            fmt::format("Unknown PARAMETER='{}' for SET_PRESSURE_ADVANCE", parameter);
        return 1;
    }
    std::lock_guard<std::mutex> lock(pa_tower_mutex_);
    pa_tower_.armed = true;
    pa_tower_.parameter = parameter;
    pa_tower_.start = number_param(gcode, "START").value_or(0.0);
    pa_tower_.factor = number_param(gcode, "FACTOR").value_or(0.0);
    spdlog::info("[MoonrakerClientMock] TUNING_TOWER armed: {} = {} + z * {}", parameter,
                 pa_tower_.start, pa_tower_.factor);
    return std::nullopt;
}

// PRINT_PA_TOWER NOZZLE=<f> TARGET_TEMP=<f> [FAST_VELOCITY=<f>]
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_print_pa_tower(const std::string& gcode) {
    auto reject = [&](std::string message) -> GcodeResult {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = std::move(message);
        return 1;
    };
    if (!pa_test_enabled()) {
        return reject("Unknown command:\"PRINT_PA_TOWER\"");
    }
    const double target = number_param(gcode, "TARGET_TEMP").value_or(0.0);
    const double current = extruder_target_.load();
    if (std::abs(current - target) > 0.05) {
        return reject(fmt::format("Extruder target temp {:.1f} does not match the expected "
                                  "TARGET_TEMP={:.1f}. Heat the extruder to the target first.",
                                  current, target));
    }
    // A finished print leaves the sdcard free; only a running one refuses.
    const auto phase = print_phase_.load();
    if (phase == MockPrintPhase::PREHEAT || phase == MockPrintPhase::PRINTING ||
        phase == MockPrintPhase::PAUSED) {
        return reject("Already printing");
    }

    const int layers = static_cast<int>(std::lround((kTowerHeightMm - kTowerFirstLayerHeightMm) /
                                                    kTowerLayerHeightMm)) +
                       1;
    {
        std::lock_guard<std::mutex> lock(pa_tower_mutex_);
        pa_tower_.active = true;
        pa_tower_.layer = -1;
        pa_tower_.layers = layers;
        pa_tower_.layer_height = kTowerLayerHeightMm;
        pa_tower_.first_layer_height = kTowerFirstLayerHeightMm;
    }
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        print_metadata_.estimated_time_seconds = layers * kTowerSecondsPerLayer;
        print_metadata_.layer_count = static_cast<uint32_t>(layers);
        print_metadata_.target_nozzle_temp = target;
        print_metadata_.filament_mm = 0.0;
        print_metadata_.filament_weights_g.clear();
    }
    {
        std::lock_guard<std::mutex> lock(print_mutex_);
        print_filename_ = "PA tower";
    }
    reset_idle_timeout();
    print_progress_.store(0.0);
    total_pause_duration_sim_ = 0.0;
    preheat_start_time_ = std::chrono::steady_clock::now();
    printing_start_time_ = preheat_start_time_;
    print_phase_.store(MockPrintPhase::PRINTING);
    print_state_.store(1);
    spdlog::info("[MoonrakerClientMock] PRINT_PA_TOWER: {} layers at {:.0f}C", layers, target);
    dispatch_print_state_notification("printing");
    service_pa_tower();
    return std::nullopt;
}

void MoonrakerClientMock::service_pa_tower() {
    PaTower tower;
    {
        std::lock_guard<std::mutex> lock(pa_tower_mutex_);
        if (!pa_tower_.active) {
            return;
        }
        tower = pa_tower_;
    }
    const int layer =
        std::clamp(static_cast<int>(print_progress_.load() * tower.layers), 0, tower.layers - 1);
    if (layer == tower.layer) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pa_tower_mutex_);
        pa_tower_.layer = layer;
    }
    const double z = tower.first_layer_height + layer * tower.layer_height;
    pos_z_.store(z);
    dispatch_status_update({{"toolhead", {{"position", {0.0, 0.0, z, 0.0}}}},
                            {"gcode_move", {{"gcode_position", {0.0, 0.0, z, 0.0}}}}});
    if (tower.armed) {
        gcode_set_pressure_advance(fmt::format("SET_PRESSURE_ADVANCE {}={:.6f}", tower.parameter,
                                               tower.start + z * tower.factor));
    }
}

void MoonrakerClientMock::end_pa_tower() {
    std::lock_guard<std::mutex> lock(pa_tower_mutex_);
    pa_tower_.active = false;
    pa_tower_.armed = false;
}
