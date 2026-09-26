// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Tool-changer dialects: the one place that knows each tool changer's extras -
// the add-on hardware klipper-toolchanger does not model (MedusaHC), and changer
// firmware that has no klipper-toolchanger at all (Z-Mod on the Creator 5 Pro).
//
// MedusaHC bolts a hotend changer onto klipper-toolchanger, which swaps a whole
// toolhead, and `toolchanger.tool_number` is simply whatever SELECT_TOOL last
// set. A hotend changer swaps only the hot end, which brings two things the
// toolchanger object cannot answer:
//
//   1. Which tool is PHYSICALLY on the head. MedusaHC ships toolchanger.cfg with
//      `verify_tool_pickup: False`, so klipper-toolchanger never checks; the
//      truth lives in dock sensors. A failed pickup leaves tool_number claiming
//      a tool that is not there.
//   2. A filament feeder. Only the hot end travels, so the filament is held by a
//      servo gripper on the frame that has to be released around a swap.
//
// This module is the ONLY place that knows each machine's status objects and
// swap commands. AmsBackendToolChanger and the subscription builder ask these
// functions and never name a machine.
//
// Adding a machine means adding one Provider to the table in
// toolchanger_addon.cpp - no call site changes.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix {
class PrinterDiscovery;
}

namespace helix::toolchanger_addon {

/// Sentinel meaning "use the detected default" in the settings picker.
inline constexpr const char* kAutoMacro = "auto";

/// Filament feeder on the frame. Default-constructed is the "no feeder" answer,
/// so a tool changer nobody told anything exposes nothing.
struct Feeder {
    bool present = false;
    std::string provider_name; ///< Machine it came from, for logs and the UI
    /// What the buttons actually send: the detected default, or the user's pick.
    std::string open_gcode;  ///< Releases the filament; empty when no feeder
    std::string close_gcode; ///< Re-grips it; empty when no feeder
    /// What detection chose, ignoring any override. Restored when the user
    /// picks "auto" again, so migrating to a controller that registers the
    /// native command is picked up without revisiting this setting.
    std::string detected_open;
    std::string detected_close;
    /// The user's stored choice: kAutoMacro, or an explicit macro name.
    std::string open_choice{kAutoMacro};
    std::string close_choice{kAutoMacro};
    /// Options for the settings picker: kAutoMacro followed by the plausible
    /// macros this printer reports. Empty when there is nothing to choose from.
    std::vector<std::string> macro_options;
};

/// What the add-on's own sensors say is on the head. Authoritative over
/// `toolchanger.tool_number` when a provider is present.
struct ToolReading {
    /// 0..N-1 mounted, -1 nothing on the head, -2 the sensors cannot tell.
    /// nullopt when this frame did not name it. Moonraker republishes only the
    /// fields that CHANGED, so a frame silent about the carriage is not a frame
    /// reporting an empty one - the same rule `docks` below is written to.
    std::optional<int> current_tool;
    /// current_tool == -2. A distinct state from "no tool": the machine does not
    /// KNOW, and acting on a guess would drive the carriage into a dock.
    bool sensor_error = false;
    /// The controller published `sensor_error` itself rather than us deriving it
    /// from -2. Only Irbis3D publishes the flag, and only the flag is a fault on
    /// its own: -2 is one value for two answers, a pin fault and "the switch
    /// pattern matches no settled configuration". A swap in flight is always the
    /// second, because the tool is between its dock and the head.
    bool sensor_error_reported = false;
    /// The machine's phase word, or empty when the frame did not say.
    ///
    /// The two controllers do NOT share a vocabulary, and this is deliberately
    /// the raw word rather than a normalised enum, because callers need to tell
    /// them apart:
    ///   Irbis3D MedusaHC-Python-Controller  `operation`: idle/picking/dropping
    ///   topi314/MedusaHC                    `state`:     uninitialized/ready/
    ///                                                    changing/error
    /// Verified against both sources, not inferred: `state` is a COARSER
    /// vocabulary than `operation`, not another spelling of it.
    std::string operation;
    /// True when `operation` came from the key that names the swap DIRECTION.
    /// False for a machine whose phase word is only ever "changing", which
    /// cannot say whether it is docking or picking. Available from the first
    /// status frame, unlike the phase words themselves, which only appear once a
    /// swap is already running - so this is what a caller keys on to decide what
    /// it can render BEFORE anything moves.
    bool phase_names_direction = false;
    /// 0 when this frame carried no tool count.
    int tool_count = 0;
    /// Per-dock occupancy, indexed by tool number: true seated, false empty,
    /// nullopt not reported in this frame. EMPTY when the frame carried no dock
    /// state at all - which is not the same answer as "every dock is vacant",
    /// and callers must not conflate them: Moonraker republishes only the fields
    /// that CHANGED.
    ///
    /// Upstream spells it `sensors` ({"e":1,"t0":1,...}), the fork spells it
    /// flat `tool<N>_docked` booleans. Same physical answer.
    std::vector<std::optional<bool>> docks;
    /// Whether anything is on the head at all (`sensors.e` / `head_loaded`).
    /// nullopt when the frame did not say.
    std::optional<bool> head_loaded;
    /// Frame-side gripper released. nullopt when this machine does not report it
    /// at all. BOTH MedusaHC controllers publish `feeder_open`, so in practice
    /// every machine carrying [medusahc] fills this in; the nullopt case is a
    /// changer with no such extra. The difference between "closed" and "never
    /// said" is what decides whether the step bar can name the release/grip
    /// phases (see AmsBackendToolChanger::get_operation_step_model).
    std::optional<bool> feeder_open;
};

/// How a swap is commanded on a machine where klipper-toolchanger is not the
/// one doing it. Default-constructed - `present` false - means the printer has
/// [toolchanger] and its SELECT_TOOL/UNSELECT_TOOL own the swap.
struct ToolCommands {
    bool present = false;
    std::string provider_name; ///< Machine it came from, for logs
    /// Prefixed to the tool number: "T" sends T0, T1, ... Empty when absent.
    std::string select_prefix;
    /// Unmounts whatever is on the head. Empty when the machine has no such
    /// command and the tool can only be swapped for another.
    std::string unselect;
};

/// Presence of an add-on dock sensor. When set, read_tool() is worth calling on
/// every status frame and its answer beats toolchanger.tool_number.
struct ToolSensor {
    bool present = false;
    std::string provider_name;
};

/// Material and colour for one slot, as the firmware stores them.
struct SlotMaterial {
    std::string material;             ///< Empty when the firmware has none set
    std::optional<std::uint32_t> rgb; ///< nullopt when unset or unparseable
};

/// What a firmware material source said in one status frame. Each field is
/// nullopt when the frame did not carry it: Moonraker republishes only what
/// CHANGED, so absence is never "cleared".
struct MaterialReading {
    std::optional<std::vector<std::optional<SlotMaterial>>> slots;
    std::optional<std::vector<std::string>> valid_types;
    std::optional<std::vector<std::pair<int, std::uint32_t>>> palette; ///< (index, 0xRRGGBB)
};

/// A changer whose firmware stores each slot's material and colour itself.
/// Default-constructed means HelixScreen's own store is the only one.
struct MaterialSource {
    bool present = false;
    std::string provider_name;
    /// Gcode that stores @p type and @p rgb for @p slot_index (0-based). @p type
    /// must already be one of the firmware's valid types, safe for a gcode line,
    /// and @p rgb one of its palette colours.
    std::string (*write_gcode)(int slot_index, const std::string& type,
                               std::uint32_t rgb) = nullptr;
};

/// The firmware material source this printer has, or an absent capability.
MaterialSource resolve_material_source(const PrinterDiscovery& hw);

/// Pull a material reading out of a status frame. nullopt means no news.
std::optional<MaterialReading> read_materials(const nlohmann::json& status, int max_slots);

/// Whether any provider claims this printer.
bool present(const PrinterDiscovery& hw);

/// The dock sensor this printer exposes, or an absent capability.
ToolSensor resolve_tool_sensor(const PrinterDiscovery& hw);

/// The swap commands this printer needs, or an absent capability meaning
/// klipper-toolchanger is there and owns them.
ToolCommands resolve_tool_commands(const PrinterDiscovery& hw);

/// Machine name for logs and the AMS unit label ("MedusaHC"), or empty.
std::string machine_name(const PrinterDiscovery& hw);

/// The feeder this printer exposes, or an absent capability.
///
/// @param open_override,close_override User-chosen macro names. "auto" (or
///        empty) keeps the detected default, which prefers the controller's
///        native command when the printer has it. A name that is not actually
///        on the printer is still honoured: the user may know something
///        discovery does not, and Klipper's own error is the honest signal.
Feeder resolve_feeder(const PrinterDiscovery& hw, const std::string& open_override = "auto",
                      const std::string& close_override = "auto");

/// Macros on this printer that could plausibly drive a feeder, for the picker
/// in AMS settings. Sorted, and deliberately filtered: a printer has hundreds of
/// macros and a raw list is unusable.
std::vector<std::string> feeder_macro_candidates(const PrinterDiscovery& hw);

/// Klipper status objects that must be subscribed for read_tool() to ever
/// return a value. Empty when no provider matches.
std::vector<std::string> required_status_objects(const PrinterDiscovery& hw);

/// Pull an authoritative reading out of a Moonraker status frame. nullopt means
/// "no news" - either this printer has no add-on, or this frame simply carried
/// none of its fields. Callers must treat nullopt as no news, never as cleared:
/// Moonraker only republishes fields whose value CHANGED.
std::optional<ToolReading> read_tool(const nlohmann::json& status);

/// Whether a reading's sensor_error names a fault the user must act on, rather
/// than the transitional geometry every swap produces.
///
/// `current_tool == -2` is one value for two answers upstream: a pin that read
/// neither 0 nor 1, and a switch pattern matching none of the settled
/// configurations. A tool in transit between its dock and the head is always the
/// second. So only a flag the controller published itself, or a -2 it still
/// reports once at rest, is a fault. topi314's controller draws the same line,
/// escalating -2 to state:"error" only while its machine state is ready.
[[nodiscard]] bool sensor_error_is_fault(const ToolReading& reading, bool swap_in_flight);

} // namespace helix::toolchanger_addon
