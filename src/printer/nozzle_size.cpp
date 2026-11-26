// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_size.h"

#include <algorithm>
#include <cmath>

namespace helix::nozzle_size {
namespace {

bool usable(const std::optional<double>& d) {
    return d && std::isfinite(*d) && *d > 0.0;
}

std::optional<double> at(const std::vector<std::optional<double>>& v, int tool) {
    if (tool < 0 || static_cast<size_t>(tool) >= v.size()) {
        return std::nullopt;
    }
    return v[static_cast<size_t>(tool)];
}

std::string extruder_name(int tool) {
    return tool == 0 ? "extruder" : "extruder" + std::to_string(tool);
}

size_t head_count(const PrinterInputs& in) {
    return std::max(in.config_diameters.size(), in.live_diameters.size());
}

} // namespace

std::optional<double> resolve(std::optional<double> live, std::optional<double> config) {
    if (usable(live)) {
        return live;
    }
    if (usable(config)) {
        return config;
    }
    return std::nullopt;
}

std::optional<double> head_nozzle(const PrinterInputs& inputs, int tool) {
    return resolve(at(inputs.live_diameters, tool), at(inputs.config_diameters, tool));
}

Capability capability(const PrinterInputs& in) {
    Capability cap;
    for (size_t i = 0; i < head_count(in); ++i) {
        const int tool = static_cast<int>(i);
        HeadCapability h;
        h.tool = tool;
        h.extruder = extruder_name(tool);
        h.current = head_nozzle(in, tool);
        h.choices.assign(kCommonSizes.begin(), kCommonSizes.end());

        // One row per mechanism, most specific first.
        if (in.vendor_records_present) {
            h.mechanism = Mechanism::VendorRpc;
            h.idle_only = true;
            h.choices = in.vendor_choices;
        } else if (in.set_nozzle_diameter_registered && tool == 0) {
            h.mechanism = Mechanism::KlipperCommand;
        } else if (!in.config_supervised) {
            h.mechanism = Mechanism::ConfigEdit;
            h.restart_required = true;
            h.idle_only = true;
        } else {
            h.choices.clear();
        }
        h.settable = h.mechanism != Mechanism::None;
        cap.heads.push_back(std::move(h));
    }
    return cap;
}

} // namespace helix::nozzle_size
