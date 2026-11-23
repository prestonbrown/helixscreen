// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pressure-advance model an extruder runs, as its status object reports it.
//
// Classic firmware has one constant K (`pressure_advance`). Firmware with
// selectable models reports `pressure_advance_model`: under `linear` it still
// reports K, and under a nonlinear model (`tanh`, `recipr`) it drops
// `pressure_advance` and reports `linear_advance`, `nonlinear_offset` and
// `linearization_velocity` instead. `SET_PRESSURE_ADVANCE ADVANCE=` sets K under
// a linear model and linear_advance under a nonlinear one, so the value a single
// K readout shows is primary_value() either way.
//
// Generic code asks this module; no caller names the firmware.

#include "json_fwd.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix::pa {

enum class ModelKind : int {
    Classic = 0, ///< the firmware reports no model: one K, nothing to select
    Linear = 1,  ///< a selectable model, set to one K
    Tanh = 2,
    Recipr = 3,
};

struct PaModel {
    ModelKind kind = ModelKind::Classic;
    std::optional<float> k; ///< pressure_advance, under Classic or Linear
    std::optional<float> linear_advance;
    std::optional<float> nonlinear_offset;
    std::optional<float> linearization_velocity;
    std::optional<float> time_offset;

    /// The firmware lets the model be changed.
    bool selectable() const {
        return kind != ModelKind::Classic;
    }
    bool nonlinear() const {
        return kind == ModelKind::Tanh || kind == ModelKind::Recipr;
    }
    /// What SET_PRESSURE_ADVANCE ADVANCE= sets under this model: K, or
    /// linear_advance. nullopt until the firmware reports it.
    std::optional<float> primary_value() const {
        return nonlinear() ? linear_advance : k;
    }
};

/// Extruder status fields the model is read from.
inline constexpr std::array<std::string_view, 6> kStatusFields = {
    "pressure_advance", "pressure_advance_model", "linear_advance",
    "nonlinear_offset", "linearization_velocity", "time_offset"};

/// @p prev updated from one extruder status frame. Moonraker frames are deltas:
/// a field the frame lacks keeps its value, except that a model change drops
/// the parameters the new model does not have.
PaModel read_pa_model(const nlohmann::json& extruder_status, PaModel prev);

// --- Selecting and tuning a nonlinear model ---------------------------------

/// The parameters a nonlinear model runs with.
struct NonlinearValues {
    ModelKind model = ModelKind::Tanh;
    float linear_advance = 0.0f;
    float nonlinear_offset = 0.0f;
    /// Fixed, never swept: 1 for direct drive, 2 for a Bowden tube, where high
    /// acceleration with a large advance asks too much of the extruder at low
    /// velocity.
    float linearization_velocity = 1.0f;
    float time_offset = 0.0f;
};

/// Smoothing every nonlinear setup runs with. Never swept.
inline constexpr float kNonlinearSmoothTime = 0.02f;

/// The values @p m runs with, or nullopt when it is not nonlinear or has not
/// reported them all.
std::optional<NonlinearValues> nonlinear_values(const PaModel& m);

/// Switch @p extruder to the nonlinear model at runtime, no restart, with @p v.
std::string set_nonlinear_gcode(const std::string& extruder, const NonlinearValues& v);

/// Put @p extruder back on the linear model at @p k (0 when it had none).
std::string set_linear_gcode(const std::string& extruder, std::optional<float> k);

/// The status object of the firmware's own pressure-advance test tower. Present
/// in the object list only when the printer's config enables it.
inline constexpr const char* kTowerObject = "pa_test";
bool has_tower_module(const std::vector<std::string>& printer_objects);

/// The config section the tower module needs, and the one key HelixScreen
/// writes into it: every other option defaults, and the tower sizes and
/// centres itself on the bed.
inline constexpr const char* kTowerSection = "pa_test";
std::pair<std::string, std::string> tower_section_key();

enum class NlParam : int { Advance = 0, Offset = 1, TimeOffset = 2 };

/// One tower: @p param follows START + z * FACTOR up the print.
struct TowerSweep {
    NlParam param = NlParam::Advance;
    float start = 0.0f;
    float factor = 0.001f;
};

/// The value @p sweep printed at @p height_mm above the bed.
float value_at(const TowerSweep& sweep, float height_mm);

/// The command that arms the sweep for the next print.
std::string tuning_tower_gcode(const TowerSweep& sweep);

/// The command that prints the tower. The firmware refuses it unless the
/// extruder's target already equals @p target_temp. @p fast_velocity_mm_s is
/// the tower's fastest segment; 0 keeps the config's.
std::string print_tower_gcode(float nozzle_diameter_mm, int target_temp, int fast_velocity_mm_s);

/// One key of an `[extruder]` edit: set to @p value, or commented out.
struct ExtruderConfigKey {
    std::string key;
    std::string value;
    bool comment_out = false;
};

/// What saving @p v writes into the extruder's config section. `pressure_advance`
/// is commented out: the nonlinear model never reads it, and Klipper refuses
/// to start on an option nothing reads.
std::vector<ExtruderConfigKey> nonlinear_config_keys(const NonlinearValues& v);

} // namespace helix::pa
