// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_size.h"

#include "app_globals.h"
#include "core/update_queue.h"
#include "gcode_help.h"
#include "i_moonraker_client.h"
#include "klipper_extruder_naming.h"
#include "platform_info.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "snapmaker_nozzle_properties.h"
#include "translation_loader.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

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

std::string resolve_volume_type(const std::string& live, const std::string& stored) {
    return live.empty() ? stored : live;
}

std::string head_volume_type(const PrinterInputs& in, int extruder_index) {
    if (extruder_index < 0 || static_cast<size_t>(extruder_index) >= head_count(in)) {
        return {};
    }
    const auto at_index = [extruder_index](const std::vector<std::string>& v) {
        return static_cast<size_t>(extruder_index) < v.size()
                   ? v[static_cast<size_t>(extruder_index)]
                   : std::string();
    };
    return resolve_volume_type(at_index(in.live_volume_types), at_index(in.stored_volume_types));
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

        // One row per mechanism, most specific first. A head the firmware owns, or whose
        // mechanism is not known yet, is read-only: the config is never edited on a guess.
        const bool has_config_section = static_cast<size_t>(tool) < in.config_diameters.size() &&
                                        in.config_diameters[static_cast<size_t>(tool)].has_value();
        const bool reports_live = usable(entry_at(in.live_diameters, tool));
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
        } else if (in.vendor_records_pending || in.size_command == CommandProbe::Unknown) {
            h.read_only_reason = TR_NOOP("Checking what this printer supports...");
        } else if (in.size_command == CommandProbe::Present && !in.size_command_runtime_only &&
                   tool == 0) {
            h.mechanism = Mechanism::KlipperCommand;
            h.idle_only = true;
        } else if (reports_live && in.size_command == CommandProbe::Absent) {
            // No command: the firmware keeps the value (a firmware that registers the command
            // for another head reports one too, and its heads fall through to the config).
            h.read_only_reason = TR_NOOP("The printer's firmware sets this nozzle size");
        } else if (in.config_supervised) {
            h.read_only_reason = kFirmwareSupervisedConfigRefusal;
        } else if (!has_config_section) {
            h.read_only_reason = TR_NOOP("The printer config has no section for this toolhead");
        } else {
            h.mechanism = Mechanism::ConfigEdit;
            h.restart_required = true;
            h.idle_only = true;
        }
        if (h.mechanism == Mechanism::None) {
            h.choices.clear();
        }
        h.settable = h.mechanism != Mechanism::None;
        if (h.mechanism != Mechanism::VendorRpc) {
            // The flow type is the user's statement here, applied at once and kept by
            // HelixScreen, whether or not the config can be edited.
            h.flow_variants = snapmaker::nozzle::flow_options();
            h.flow_current = snapmaker::nozzle::flow_option(head_volume_type(in, h.extruder_index));
            // A type the printer reports is not the user's to override.
            const bool reported_flow = static_cast<size_t>(tool) < in.live_volume_types.size() &&
                                       !in.live_volume_types[static_cast<size_t>(tool)].empty();
            h.flow_changeable = !reported_flow;
            if (reported_flow) {
                h.flow_note = TR_NOOP("The printer reports this flow type");
            }
        }
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

    const SettingsManager& settings = SettingsManager::instance();
    for (int i = 0; i < SettingsManager::kNozzleFlowMaxHeads; ++i) {
        in.stored_volume_types.push_back(!settings.get_nozzle_flow_set(i) ? ""
                                         : settings.get_nozzle_high_flow(i)
                                             ? snapmaker::nozzle::kHighFlow
                                             : snapmaker::nozzle::kStandard);
    }
    in.config_supervised = firmware_supervised();
    in.vendor_records_pending = state.get_discovery().has_snapmaker() && !in.vendor_records_present;
    in.printing = printer_has_job(state.print_state().get_print_job_state()) ||
                  state.print_state().is_print_in_progress();
    in.size_command = size_command_state();
    in.size_command_runtime_only = state.get_discovery().is_kalico();
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
            refusal = refuse(h.read_only_reason.empty()
                                 ? TR_NOOP("This nozzle size can't be changed here")
                                 : h.read_only_reason.c_str());
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
    const auto matched = std::find_if(h->choices.begin(), h->choices.end(),
                                      [mm](double c) { return same_size(c, mm); });
    if (matched == h->choices.end()) {
        return refuse(TR_NOOP("This printer doesn't accept that nozzle size"));
    }
    // What goes out is the listed size, never the requested number.
    mm = *matched;
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
    const Capability cap = nozzle_capability(in);
    const HeadCapability* h = nullptr;
    for (const auto& head : cap.heads) {
        if (head.extruder_index == extruder_index) {
            h = &head;
        }
    }
    if (h == nullptr) {
        return refuse(TR_NOOP("This printer has no such toolhead"));
    }
    if (h->flow_variants.empty()) {
        return refuse(TR_NOOP("This toolhead has no flow type to change"));
    }
    if (h->mechanism != Mechanism::VendorRpc && !h->flow_changeable) {
        return refuse(h->flow_note.c_str());
    }
    snapmaker::nozzle::Nozzle wanted;
    wanted.volume_type = snapmaker::nozzle::volume_type_from_option(variant);
    if (!wanted.volume_type) {
        return refuse(TR_NOOP("This toolhead has no such flow type"));
    }
    if (h->mechanism == Mechanism::VendorRpc) {
        ChangePlan refusal;
        if (settable_head(cap, in, extruder_index, refusal) == nullptr) {
            return refusal;
        }
        return vendor_plan(in, *h, wanted);
    }
    if (*wanted.volume_type == head_volume_type(in, extruder_index)) {
        return refuse(TR_NOOP("The head is already set that way"));
    }
    ChangePlan plan;
    plan.kind = ChangePlan::Kind::StoredFlow;
    plan.extruder_index = extruder_index;
    plan.value = *wanted.volume_type;
    return plan;
}

void apply_stored_flow(const ChangePlan& plan) {
    if (plan.kind == ChangePlan::Kind::StoredFlow) {
        SettingsManager::instance().set_nozzle_high_flow(
            plan.extruder_index, plan.value == snapmaker::nozzle::kHighFlow);
    }
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
    case ChangePlan::Kind::StoredFlow:
    case ChangePlan::Kind::Refused:
        break;
    }
    return std::nullopt;
}

std::optional<bool> help_lists_size_command(const nlohmann::json& response) {
    return gcode_help_lists(response, "SET_NOZZLE_DIAMETER");
}

namespace {
std::atomic<int> g_size_command{static_cast<int>(CommandProbe::Unknown)};
} // namespace

CommandProbe size_command_state() {
    return static_cast<CommandProbe>(g_size_command.load());
}

void set_size_command_state(CommandProbe state) {
    g_size_command.store(static_cast<int>(state));
}

namespace {
std::atomic<bool> g_probe_in_flight{false};
std::mutex g_listener_mutex;
std::function<void()> g_listener;
} // namespace

void set_size_command_listener(std::function<void()> listener) {
    std::lock_guard<std::mutex> lock(g_listener_mutex);
    g_listener = std::move(listener);
}

void probe_size_command(IMoonrakerClient& client) {
    set_size_command_state(CommandProbe::Unknown);
    g_probe_in_flight.store(true);
    client.send_jsonrpc(
        "printer.gcode.help", nlohmann::json::object(),
        [](const nlohmann::json& response) {
            // A reply with no command list says nothing: the size stays unknown.
            const auto listed = help_lists_size_command(response);
            set_size_command_state(!listed   ? CommandProbe::Unknown
                                   : *listed ? CommandProbe::Present
                                             : CommandProbe::Absent);
            g_probe_in_flight.store(false);
            if (listed) {
                ui::queue_update("NozzleSize::probe_settled", []() {
                    std::function<void()> listener;
                    {
                        std::lock_guard<std::mutex> lock(g_listener_mutex);
                        listener = g_listener;
                    }
                    if (listener) {
                        listener();
                    }
                });
            }
        },
        [](const MoonrakerError& err) {
            spdlog::debug("[NozzleSize] Could not list gcode commands: {}", err.message);
            g_probe_in_flight.store(false);
        },
        0,   // default timeout
        true // silent: a probe; until it answers, no size can be changed
    );
}

void reprobe_size_command_if_unknown(IMoonrakerClient& client) {
    if (size_command_state() == CommandProbe::Unknown && !g_probe_in_flight.load()) {
        probe_size_command(client);
    }
}

} // namespace helix::nozzle_size
