// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_size.h"

#include "app_globals.h"
#include "gcode_help.h"
#include "i_moonraker_client.h"
#include "klipper_extruder_naming.h"
#include "platform_info.h"
#include "printer_state.h"
#include "snapmaker_nozzle_properties.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
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

namespace {
snapmaker::nozzle::Nozzle reported_nozzle_of(const PrinterInputs& in, int index) {
    snapmaker::nozzle::Nozzle n;
    n.diameter = entry_at(in.live_diameters, index);
    if (index >= 0 && static_cast<size_t>(index) < in.live_volume_types.size() &&
        !in.live_volume_types[static_cast<size_t>(index)].empty()) {
        n.volume_type = in.live_volume_types[static_cast<size_t>(index)];
    }
    return n;
}
} // namespace

Capability nozzle_capability(const PrinterInputs& in) {
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
            const snapmaker::nozzle::Nozzle reported = reported_nozzle_of(in, tool);
            if (reported.volume_type) {
                h.flow_current = snapmaker::nozzle::flow_option(*reported.volume_type);
            }
            h.flow_changeable = snapmaker::nozzle::flow_selectable(reported);
            if (!h.flow_changeable) {
                h.flow_note = TR_NOOP("High Flow needs a 0.4 mm nozzle");
            }
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
        if (in.live_volume_types.size() <= slot) {
            in.live_volume_types.resize(slot + 1);
        }
        in.live_volume_types[slot] = info.nozzle_volume_type;
        if (!info.nozzle_volume_type.empty() && state.get_discovery().has_snapmaker()) {
            in.vendor_records_present = true;
        }
    }
    if (in.vendor_records_present) {
        in.vendor_choices.assign(snapmaker::nozzle::kDiameters.begin(),
                                 snapmaker::nozzle::kDiameters.end());
    }

    in.config_supervised = firmware_supervised();
    in.printing = state.print_state().is_print_in_progress();
    in.set_nozzle_diameter_registered = size_command_registered();
    return in;
}

namespace {

ChangePlan refuse(const char* reason) {
    ChangePlan plan;
    plan.reason = reason;
    return plan;
}

bool same_size(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

/// The head's row, or nullptr; refusal reasons for a head that cannot change.
const HeadCapability* settable_head(const Capability& cap, const PrinterInputs& in,
                                    int extruder_index, ChangePlan& refusal) {
    for (const auto& h : cap.heads) {
        if (h.extruder_index != extruder_index) {
            continue;
        }
        if (!h.settable) {
            refusal =
                refuse(in.config_supervised ? kFirmwareSupervisedConfigRefusal
                                            : TR_NOOP("This nozzle size can't be changed here"));
            return nullptr;
        }
        if (h.idle_only && in.printing) {
            refusal = refuse(TR_NOOP("Finish or cancel the print first"));
            return nullptr;
        }
        return &h;
    }
    refusal = refuse(TR_NOOP("This printer has no such toolhead"));
    return nullptr;
}

std::string size_text(double mm) {
    return fmt::format("{}", mm);
}

ChangePlan vendor_plan(const PrinterInputs& in, const HeadCapability& h,
                       const snapmaker::nozzle::Nozzle& wanted) {
    const auto params = snapmaker::nozzle::change_params(
        h.extruder_index, reported_nozzle_of(in, h.extruder_index), wanted);
    if (!params) {
        return refuse(TR_NOOP("The head is already set that way"));
    }
    ChangePlan plan;
    plan.kind = ChangePlan::Kind::VendorRpc;
    plan.method = snapmaker::nozzle::kMethod;
    plan.params = *params;
    return plan;
}

} // namespace

ChangePlan plan_change(const PrinterInputs& in, int extruder_index, double mm) {
    ChangePlan refusal;
    const Capability cap = nozzle_capability(in);
    const HeadCapability* h = settable_head(cap, in, extruder_index, refusal);
    if (h == nullptr) {
        return refusal;
    }
    const bool offered = std::any_of(h->choices.begin(), h->choices.end(),
                                     [mm](double c) { return same_size(c, mm); });
    if (!offered) {
        return refuse(TR_NOOP("This printer doesn't accept that nozzle size"));
    }
    if (h->mechanism != Mechanism::VendorRpc && h->current && same_size(*h->current, mm)) {
        return refuse(TR_NOOP("That size is already set"));
    }
    ChangePlan plan;
    switch (h->mechanism) {
    case Mechanism::KlipperCommand:
        plan.kind = ChangePlan::Kind::KlipperCommand;
        plan.gcode = "SET_NOZZLE_DIAMETER VALUE=" + size_text(mm);
        return plan;
    case Mechanism::VendorRpc: {
        snapmaker::nozzle::Nozzle wanted;
        wanted.diameter = mm;
        return vendor_plan(in, *h, wanted);
    }
    case Mechanism::ConfigEdit:
        plan.kind = ChangePlan::Kind::ConfigEdit;
        plan.section = h->extruder;
        plan.key = "nozzle_diameter";
        plan.value = size_text(mm);
        plan.restart_required = true;
        return plan;
    case Mechanism::None:
        break;
    }
    return refuse(TR_NOOP("This nozzle size can't be changed here"));
}

ChangePlan plan_flow_change(const PrinterInputs& in, int extruder_index,
                            const std::string& variant) {
    ChangePlan refusal;
    const Capability cap = nozzle_capability(in);
    const HeadCapability* h = settable_head(cap, in, extruder_index, refusal);
    if (h == nullptr) {
        return refusal;
    }
    if (h->flow_variants.empty() || h->mechanism != Mechanism::VendorRpc) {
        return refuse(TR_NOOP("This toolhead has no flow type to change"));
    }
    snapmaker::nozzle::Nozzle wanted;
    wanted.volume_type = snapmaker::nozzle::volume_type_from_option(variant);
    if (!wanted.volume_type) {
        return refuse(TR_NOOP("This toolhead has no such flow type"));
    }
    return vendor_plan(in, *h, wanted);
}

std::string size_text_mm(double mm) {
    return fmt::format("{} mm", mm);
}

std::optional<std::string> confirm_message(const ChangePlan& plan, double mm) {
    switch (plan.kind) {
    case ChangePlan::Kind::ConfigEdit:
        return TR_NOOP("This sets the nozzle size in the printer config.");
    case ChangePlan::Kind::KlipperCommand:
        return TR_NOOP("This applies now. Klipper's extrusion limit follows at its next restart.");
    case ChangePlan::Kind::VendorRpc:
        if (same_size(mm, 0.2)) {
            return TR_NOOP("A 0.2 mm nozzle can't print every filament, including PA, PC, TPU "
                           "and carbon- or glass-filled types. If this head has one of them "
                           "loaded, the printer clears its filament info and you'll need to set "
                           "it again.");
        }
        return std::nullopt;
    case ChangePlan::Kind::Refused:
        break;
    }
    return std::nullopt;
}

std::optional<bool> help_lists_size_command(const nlohmann::json& response) {
    return gcode_help_lists(response, "SET_NOZZLE_DIAMETER");
}

namespace {
std::atomic<bool> g_size_command_registered{false};
} // namespace

bool size_command_registered() {
    return g_size_command_registered.load();
}

void set_size_command_registered(bool registered) {
    g_size_command_registered.store(registered);
}

void probe_size_command(IMoonrakerClient& client) {
    set_size_command_registered(false);
    client.send_jsonrpc(
        "printer.gcode.help", nlohmann::json::object(),
        [](const nlohmann::json& response) {
            set_size_command_registered(help_lists_size_command(response).value_or(false));
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[NozzleSize] Could not list gcode commands: {}", err.message);
        },
        0,   // default timeout
        true // silent: a probe; the size stays editable through the config
    );
}

} // namespace helix::nozzle_size
