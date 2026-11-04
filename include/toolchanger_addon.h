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

#include "json_fwd.h"
#include "macro_choice.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace helix {
class PrinterDiscovery;
}

namespace helix::toolchanger_addon {

using helix::kAutoMacro;

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
    /// from -2. Only [medusahc] publishes the flag, and only the flag is a fault on
    /// its own: -2 is one value for two answers, a pin fault and "the switch
    /// pattern matches no settled configuration". A swap in flight is always the
    /// second, because the tool is between its dock and the head.
    bool sensor_error_reported = false;
    /// The frame carried the controller's `sensor_error` flag at all, true or
    /// false. Without it, sensor_error_reported = false says nothing.
    bool sensor_error_named = false;
    /// The machine's raw phase word, or empty when the frame did not say:
    /// MedusaHC's `operation` (idle/changing/dropping/picking), or Z-Mod's.
    std::string operation;
    /// Irbis3D's `last_error`: the message its last failed operation latched,
    /// empty once the next operation begins. nullopt when the frame did not
    /// name it.
    std::optional<std::string> last_error;
    /// The tool the controller's current or last operation was moving to (or,
    /// for a park, the tool being parked); -1 for none. nullopt when the frame
    /// did not name it.
    std::optional<int> target_tool;
    /// True when this frame carried a phase word that names the swap DIRECTION
    /// (dropping vs picking). False when it carried none. Available from the first
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
    /// Spelled `sensors` ({"e":1,"t0":1,...}) on the wire.
    std::vector<std::optional<bool>> docks;
    /// Whether anything is on the head at all (`sensors.e`).
    /// nullopt when the frame did not say.
    std::optional<bool> head_loaded;
    /// Frame-side gripper released. nullopt when this machine does not report it
    /// at all. [medusahc] publishes `feeder_open`, so in practice every machine
    /// carrying it fills this in; the nullopt case is a
    /// changer with no such extra. The difference between "closed" and "never
    /// said" is what decides whether the step bar can name the release/grip
    /// phases (see AmsBackendToolChanger::get_operation_step_model).
    std::optional<bool> feeder_open;
};

/// How a swap is commanded on a machine where klipper-toolchanger is not the
/// one doing it. Default-constructed - `present` false - means the printer has
/// [toolchanger] and its SELECT_TOOL/UNSELECT_TOOL own the swap.
///
/// `present` is also the tool-mapping answer: these commands name the physical
/// tool outright, and klipper-toolchanger's ASSIGN_TOOL does not reach them,
/// so a remap has to rewrite the job file instead.
struct ToolCommands {
    bool present = false;
    std::string provider_name; ///< Machine it came from, for logs
    /// Prefixed to the tool number: "T" sends T0, T1, ... Empty when absent.
    std::string select_prefix;
    /// Unmounts whatever is on the head; may be several lines. Empty when the
    /// machine has no such command and the tool can only be swapped for another.
    std::string unselect;
    /// klipper-toolchanger runs beside these commands, so INITIALIZE_TOOLCHANGER
    /// exists even though it does not drive the swap.
    bool initialize_available = false;
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
    /// Per-head filament presence, nullopt when the frame did not say.
    std::optional<bool> filament_present;
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

/// Filament feed through a changer's own shim, where the firmware offers one.
/// Default-constructed means a head's Load is only a tool mount.
struct ToolFeed {
    bool present = false;
    std::string provider_name; ///< Machine it came from, for logs
    /// The gcode that feeds head n, indexed by head. An empty entry means the
    /// printer published nothing to feed that head with; callers refuse rather
    /// than build a command of their own.
    std::vector<std::string> head_gcode;
};

/// The feed this printer offers, or an absent capability.
ToolFeed resolve_tool_feed(const PrinterDiscovery& hw);

/// Whether this printer moves filament only through the mounted tool, so a load
/// or unload has to mount the tool first (mount_feed_sequence.h).
bool feeds_through_mount(const PrinterDiscovery& hw);

/// The firmware material source this printer has, or an absent capability.
MaterialSource resolve_material_source(const PrinterDiscovery& hw);

/// Pull a material reading out of a status frame. nullopt means no news.
std::optional<MaterialReading> read_materials(const nlohmann::json& status, int max_slots);

/// Whether any provider claims this printer.
bool present(const PrinterDiscovery& hw);

/// Whether the Z-Mod Creator 5 changer row claims this printer: `zmod_color`
/// plus the carriage grab buttons. The AD5X Z-Mod also publishes `zmod_color`
/// but has no carriage buttons, so it cannot match. Backend selection asks this
/// to tell Z-Mod's afclite AFC shim (which publishes the `AFC` objects an AFC
/// panel wants) from a machine that is really a head changer.
bool zmod_c5_present(const PrinterDiscovery& hw);

/// Whether the firmware picks each tool's lane itself at print time. Z-Mod's
/// afclite, a lane-picking shim that publishes AFC-shaped objects, asks for the
/// lanes in its own material prompt when the print starts, and refuses SET_MAP.
bool firmware_picks_lanes_at_print(const PrinterDiscovery& hw);

/// The Z-Mod Creator 5 provider row's name, so callers holding machine_name()
/// or a ToolSensor's provider_name can ask whether the active row speaks the
/// Z-Mod dialect — whose swap phases arrive as gcode-response markers rather
/// than a status object's phase word.
inline constexpr const char* kZmodC5ProviderName = "Creator 5 Pro";

/// Classify one gcode-response line from Z-Mod's Creator 5 changer macros into
/// the phase word the changer state machine consumes. Z-Mod narrates a swap
/// through respond_info lines — the grab/unload retry loops ("Grab attempt for
/// T0: 1 of 5...") and the per-head offset echoes that bracket `_T_IN` /
/// `_T_OUT` ("X-Offset: 0.0000 _T_IN") — and each marker spells the swap
/// direction, so each maps onto the picking/dropping vocabulary the MedusaHC
/// controllers publish. nullopt for every other console line. If a firmware
/// build does not echo the markers, nothing matches and the operation step bar
/// stays suppressed — a marker that never arrives cannot paint a wrong step.
std::optional<ToolReading> parse_zmod_marker(const std::string& line);

/// Classify one line of Z-Mod's T_INFO output (`T_INFO` is read-only: it
/// answers state, it moves nothing). Each tool row is one dock/carriage fact —
/// "T1: HEAD" puts tool 1 on the carriage, "T0: home" seats it in its dock,
/// "ERROR (both)" is a sensor fault, "?" is no news for that tool — so each
/// line applies on its own and no block buffering is needed. The `Offset:`
/// line closes the block (it is always last), which is the only place the
/// answer "nothing is on the carriage" can be stated: every tool row said
/// `home` and none said HEAD. nullopt for every other console line.
std::optional<ToolReading> parse_zmod_tinfo_line(const std::string& line, bool& block_ended_out);

/// What Z-Mod's afclite shim said in one status frame. The object is the
/// firmware's own AFC compat surface; the fields read here are the ones whose
/// semantics a boolean or the bundle evidence fixes. `current_state` is
/// deliberately carried as an opaque word: its vocabulary beyond "idle" is
/// unverified, so callers log it rather than act on it.
struct AfcStateReading {
    std::string current_state; ///< As published; empty when the frame did not say
    bool error_state = false;  ///< The shim's own fault flag
    std::string message;       ///< Its fault text, when it sent one
    /// `lanes[].map` verbatim ("T0", "T2", ...), indexed by lane; empty when
    /// the frame carried no lane table. Read-only visibility: the lane→tool
    /// relation is the firmware's own, and what it means for head indices is
    /// exactly what a field capture has to settle before anything acts on it.
    std::vector<std::string> lane_maps;
};

/// Pull an AfcStateReading out of a Moonraker status frame carrying the
/// `zmod_afclite` (or its `AFC` alias) object. nullopt when the frame carried
/// neither object.
std::optional<AfcStateReading> read_afc_state(const nlohmann::json& status);

/// One afclite lane's Spoolman spool as its `AFC_lane <name>` object publishes
/// it; spool_id 0 when the lane has none.
struct AfcLaneSpool {
    std::string lane; ///< The shim's lane name ("E0" is head 0)
    int spool_id = 0;
};

/// The `spool_id` of every `AFC_lane <name>` object a status frame carries.
/// nullopt when it carried none: a frame without the field is not a clearing.
std::optional<std::vector<AfcLaneSpool>> read_afc_lane_spools(const nlohmann::json& status);

/// The dock sensor this printer exposes, or an absent capability.
ToolSensor resolve_tool_sensor(const PrinterDiscovery& hw);

/// Gcode that brushes the mounted tool's nozzle clean, or empty when the
/// printer has no such command.
std::string clean_nozzle_gcode(const PrinterDiscovery& hw);

/// A temperature floor the controller's resume command enforces: before a
/// print it paused may resume onto a tool it must pick again, that tool's
/// nozzle must be ABOVE min_nozzle_c.
struct ResumeRule {
    int min_nozzle_c = 0; ///< 0 for a controller with no such rule
    std::string command;  ///< the controller command that enforces it
};

/// This printer's controller's resume rule, whether or not its RESUME runs it.
ResumeRule resume_rule(const PrinterDiscovery& hw);

/// Whether @p rule binds: the printer's RESUME macro, as configfile reports
/// it, runs the command that enforces it. Wiring RESUME to that command is an
/// install step the owner may skip, so a rule with no caller is no rule.
bool resume_rule_in_force(const ResumeRule& rule);

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
/// reports once at rest, is a fault.
///
/// Irbis3D's flag is a latch on the last operation, cleared only when the next
/// one begins, while its current_tool follows the dock sensors live. At rest, a
/// flag beside a definite carriage reading (-1 or a tool) describes a past
/// failure: the sensors can tell which tool is mounted.
[[nodiscard]] bool sensor_error_is_fault(const ToolReading& reading, bool swap_in_flight);

} // namespace helix::toolchanger_addon
