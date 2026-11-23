// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pa_model.h"

#include "json_utils.h"

#include <string>

#include "hv/json.hpp"

namespace helix::pa {

namespace {

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
    return prev;
}

} // namespace helix::pa
