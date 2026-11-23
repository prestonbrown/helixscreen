// SPDX-License-Identifier: GPL-3.0-or-later

#include "motion_envelope.h"

#include <algorithm>
#include <cmath>

#include "hv/json.hpp"

namespace helix::motion_envelope {
namespace {

/// One firmware module that guards a service zone past the print area.
struct Provider {
    /// Status object, also listed in printer.objects.list when present.
    const char* object;
    /// Pull the print-area Y limit (mm) out of that object's status.
    std::optional<float> (*read_y_max)(const nlohmann::json& object_status);
};

/// Creality K2 community firmware's extended_zone_transform: past
/// standard_y_max sits the brush and waste bin, reachable only with X inside
/// safe_x_min..safe_x_max. Its status publishes standard_y_max.
std::optional<float> read_extended_zone(const nlohmann::json& object_status) {
    auto it = object_status.find("standard_y_max");
    if (it == object_status.end() || !it->is_number()) {
        return std::nullopt;
    }
    const double v = it->get<double>();
    if (!std::isfinite(v)) {
        return std::nullopt;
    }
    return static_cast<float>(v);
}

const Provider kProviders[] = {
    {"extended_zone_transform", read_extended_zone},
};

} // namespace

std::vector<std::string> required_status_objects(const std::vector<std::string>& objects) {
    std::vector<std::string> out;
    for (const auto& p : kProviders) {
        if (std::find(objects.begin(), objects.end(), p.object) != objects.end()) {
            out.emplace_back(p.object);
        }
    }
    return out;
}

std::optional<float> read_y_max_mm(const nlohmann::json& status) {
    if (!status.is_object()) {
        return std::nullopt;
    }
    for (const auto& p : kProviders) {
        auto it = status.find(p.object);
        if (it == status.end() || !it->is_object()) {
            continue;
        }
        if (auto v = p.read_y_max(*it)) {
            return v;
        }
    }
    return std::nullopt;
}

} // namespace helix::motion_envelope
