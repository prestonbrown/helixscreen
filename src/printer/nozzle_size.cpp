// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_size.h"

#include "app_globals.h"
#include "klipper_extruder_naming.h"
#include "platform_info.h"
#include "printer_state.h"
#include "snapmaker_nozzle_properties.h"

#include <algorithm>
#include <cmath>

namespace helix::nozzle_size {
namespace {

bool usable(const std::optional<double>& d) {
    return d && std::isfinite(*d) && *d > 0.0;
}

std::optional<double> entry_at(const std::vector<std::optional<double>>& v, int index) {
    if (index < 0 || static_cast<size_t>(index) >= v.size()) {
        return std::nullopt;
    }
    return v[static_cast<size_t>(index)];
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

std::optional<double> head_nozzle(const PrinterInputs& inputs, int extruder_index) {
    return resolve(entry_at(inputs.live_diameters, extruder_index),
                   entry_at(inputs.config_diameters, extruder_index));
}

std::optional<double> head_nozzle_for_tool(const PrinterInputs& inputs,
                                           const std::vector<std::string>& tool_extruders,
                                           int tool) {
    if (tool_extruders.empty()) {
        return head_nozzle(inputs, tool);
    }
    if (tool < 0 || static_cast<size_t>(tool) >= tool_extruders.size()) {
        return std::nullopt;
    }
    const std::optional<int> extruder =
        tool_number_for_extruder(tool_extruders[static_cast<size_t>(tool)]);
    return extruder ? head_nozzle(inputs, *extruder) : std::nullopt;
}

Capability capability(const PrinterInputs& in) {
    Capability cap;
    for (size_t i = 0; i < head_count(in); ++i) {
        const int tool = static_cast<int>(i);
        HeadCapability h;
        h.extruder_index = tool;
        h.extruder = extruder_name(tool);
        h.current = head_nozzle(in, tool);
        h.choices.assign(kCommonSizes.begin(), kCommonSizes.end());

        // One row per mechanism, most specific first.
        if (in.vendor_records_present) {
            h.mechanism = Mechanism::VendorRpc;
            h.idle_only = true;
            h.choices = in.vendor_choices;
            h.flow_variants = snapmaker::nozzle::flow_options();
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

PrinterInputs inputs_from_state(PrinterState& state) {
    PrinterInputs in;

    if (const auto& machine = state.get_discovery().fit_machine()) {
        for (double d : machine->nozzle_diameters) {
            in.config_diameters.emplace_back(d);
        }
    }

    for (const auto& [name, info] : state.temperature_state().extruders()) {
        const std::optional<int> n = tool_number_for_extruder(name);
        if (!n) {
            continue;
        }
        const auto slot = static_cast<size_t>(*n);
        if (in.live_diameters.size() <= slot) {
            in.live_diameters.resize(slot + 1);
        }
        if (info.nozzle_diameter > 0.0) {
            in.live_diameters[slot] = info.nozzle_diameter;
        }
        if (!info.nozzle_volume_type.empty() && state.get_discovery().has_snapmaker()) {
            in.vendor_records_present = true;
        }
    }
    if (in.vendor_records_present) {
        in.vendor_choices.assign(snapmaker::nozzle::kDiameters.begin(),
                                 snapmaker::nozzle::kDiameters.end());
    }

    in.config_supervised = firmware_supervised();
    return in;
}

} // namespace helix::nozzle_size
