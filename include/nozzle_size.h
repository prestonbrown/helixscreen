// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What nozzle is on each head, and whether the user can change it.
//
// Mainline Klipper reads `nozzle_diameter` from [extruderN] once at startup and
// has no command to change it. Some firmwares report a live diameter, accept a
// command that changes it, or keep their own per-head record. This module is
// the ONLY place that knows those mechanisms. Generic code asks capability()
// and head_nozzle() and never names a firmware.
//
// Detection is by capability: a mechanism matches because its command or record
// is present, never because of a board name. Adding a mechanism means adding
// one branch to head_mechanism() in nozzle_size.cpp.

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class PrinterState;
} // namespace helix

namespace helix::nozzle_size {

/// The diameter a consumer assumes when nothing reports one.
inline constexpr double kDefaultNozzleMm = 0.4;

/// The sizes offered where the printer accepts any value.
inline constexpr std::array<double, 9> kCommonSizes = {0.2, 0.25, 0.3, 0.4, 0.5,
                                                       0.6, 0.8,  1.0, 1.2};

/// How a head's size is changed.
enum class Mechanism {
    None,           ///< read-only
    ConfigEdit,     ///< edit [extruderN] nozzle_diameter, then restart Klipper
    KlipperCommand, ///< a registered SET_NOZZLE_DIAMETER, effective at once
    VendorRpc,      ///< the firmware's own per-head record and call
};

/// Everything the mechanisms decide from. Vectors are indexed by tool number
/// (the N of [extruderN]); a missing or unreported entry is nullopt.
struct PrinterInputs {
    /// `nozzle_diameter` of each [extruderN] in configfile.settings.
    std::vector<std::optional<double>> config_diameters;
    /// `nozzle_diameter` each extruder object reports at runtime. One entry per
    /// extruder the printer has, nullopt while it reports nothing.
    std::vector<std::optional<double>> live_diameters;
    /// Whether the printer registers SET_NOZZLE_DIAMETER. Discovery fills this
    /// in once it asks the printer for its commands; until then it is false.
    bool set_nozzle_diameter_registered = false;
    /// Whether the firmware keeps a nozzle record per head (diameter plus flow type).
    bool vendor_records_present = false;
    /// The diameters that firmware accepts, in mm.
    std::vector<double> vendor_choices;
    /// Whether firmware owns the Klipper config, so HelixScreen may not edit it.
    bool config_supervised = false;
};

struct HeadCapability {
    int extruder_index = 0;        ///< the N of "extruderN": a head is a physical extruder
    std::string extruder;          ///< "extruder", "extruder1"
    std::optional<double> current; ///< live field if reported, else config
    bool settable = false;
    Mechanism mechanism = Mechanism::None;
    bool restart_required = false; ///< the change applies after a Klipper restart
    bool idle_only = false;        ///< refused while printing or paused
    std::vector<double> choices;   ///< sizes the user may pick
    /// Flow types the head can be set to ("Standard", "High Flow"); empty where
    /// the printer has no such choice.
    std::vector<std::string> flow_variants;
};

struct Capability {
    std::vector<HeadCapability> heads;
};

/// The one live-over-config rule: a live diameter that is finite and positive
/// wins; otherwise the config's, if that is usable; otherwise nothing.
[[nodiscard]] std::optional<double> resolve(std::optional<double> live,
                                            std::optional<double> config);

/// Which heads the printer has and how each is changed. Pure.
[[nodiscard]] Capability capability(const PrinterInputs& inputs);

/// The nozzle on extruder @p extruder_index: the single answer every consumer
/// reads. nullopt for a head the printer does not have or that reports nothing.
[[nodiscard]] std::optional<double> head_nozzle(const PrinterInputs& inputs, int extruder_index);

/// The nozzle behind tool @p tool. @p tool_extruders names the extruder each tool
/// heats (ToolState::extruder_name_for_tool; "" for none). Where tools are lanes
/// into one nozzle every tool resolves to that head. An empty list means a tool
/// is its own head.
[[nodiscard]] std::optional<double>
head_nozzle_for_tool(const PrinterInputs& inputs, const std::vector<std::string>& tool_extruders,
                     int tool);

/// PrinterInputs as the running app knows the printer.
[[nodiscard]] PrinterInputs inputs_from_state(PrinterState& state);

} // namespace helix::nozzle_size
