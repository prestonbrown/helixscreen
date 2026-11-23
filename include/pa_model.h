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
#include <string_view>

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

} // namespace helix::pa
