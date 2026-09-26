// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_database.h"
#include "macro_param_cache.h"
#include "macro_param_modal.h"

#include <functional>
#include <map>
#include <string>
#include <unordered_set>

#include "hv/json.hpp"

class IMoonrakerAPI;
struct MoonrakerError;

namespace helix {

class PrinterDiscovery;

/// Build a G-code string from a macro name and parameter result.
/// Generates SET_GCODE_VARIABLE commands for variable overrides,
/// then the macro call with inline KEY=VALUE params.
[[nodiscard]] std::string build_macro_gcode(const std::string& macro_name,
                                            const MacroParamResult& result);

/// Execute a macro via IMoonrakerAPI with the given parameters.
/// Builds gcode via build_macro_gcode() and sends via api->execute_gcode().
///
/// @p hw is required rather than defaulted because it decides how a FAILED rpc
/// is reported: a macro that restarts the host never acks, and calling that a
/// failure is the bug classify_macro_rpc_failure() exists to stop. A defaulted
/// parameter would let a call site added later silently opt back into it.
///
/// @param caller_tag Log tag for spdlog messages (e.g., "[MacrosPanel]")
/// @param hw         This printer's discovery, for the wrapped-restart check.
void execute_macro_gcode(IMoonrakerAPI* api, const std::string& macro_name,
                         const MacroParamResult& result, const char* caller_tag,
                         const PrinterDiscovery& hw);

/// How a whole-material preheat heats the printer.
enum class MaterialPreheatMode {
    TEMPERATURES,            ///< No macro assigned: send the preset temperatures.
    TEMPERATURES_THEN_MACRO, ///< Send the temperatures, then run the additive macro.
    MACRO_ONLY,              ///< The macro sets every temperature itself.
    MACRO_MISSING,           ///< Assigned, but this printer lacks it: send the temperatures.
};

struct MaterialPreheatPlan {
    MaterialPreheatMode mode;
    std::string macro; ///< Empty for TEMPERATURES.
};

/// Decide a material preheat from its override and this printer's macros.
/// Material overrides are shared across printers, so an assigned macro may
/// exist on another printer only; that plans MACRO_MISSING, never a macro send.
[[nodiscard]] MaterialPreheatPlan plan_material_preheat(const filament::MaterialOverride* override,
                                                        const PrinterDiscovery& hw);

/// Carry out @p plan. The temperature sender runs synchronously, before an
/// additive macro, or alone for TEMPERATURES and MACRO_MISSING.
/// Call on the UI thread; the sender is never retained. Filament-change heating
/// and individual heater controls do not use this policy.
void execute_material_preheat(IMoonrakerAPI* api, const MaterialPreheatPlan& plan,
                              const std::function<void()>& set_temperatures, const char* caller_tag,
                              const PrinterDiscovery& hw);

/// Commands that restart the Klipper host or halt the printer. Seeds both the
/// name check below and the macro-body analysis.
[[nodiscard]] const std::unordered_set<std::string>& dangerous_command_names();

/// Commands after which the host comes back on its own (SAVE_CONFIG and the
/// restarts). A dropped rpc from one of these is a restart in progress.
[[nodiscard]] const std::unordered_set<std::string>& host_restarting_command_names();

/// Commands after which the host stays down until someone intervenes (M112 and
/// the shutdowns). A dropped rpc from one of these is a halt, not a restart -
/// telling the user "Firmware restarting..." there promises a recovery that is
/// not coming.
[[nodiscard]] const std::unordered_set<std::string>& host_halting_command_names();

/// Macro names, uppercased, whose bodies reach one of those commands - directly
/// or through other macros.
///
/// Reads `configfile.settings`, where every `gcode_macro <name>` section carries
/// its `gcode` text, and follows the call graph to a fixpoint. A name-only check
/// cannot see a wrapped restart: ZMOD's AUTO_FULL_BED_LEVEL calls _SAVE_CONFIG
/// calls SAVE_CONFIG, so the shipped "Bed Level" button restarts klippy while
/// looking inert (ghzserg/z_ad5x@204105d). Only command-position tokens count,
/// because bodies talk about these commands in RESPOND messages.
[[nodiscard]] std::unordered_set<std::string>
analyze_host_restarting_macros(const nlohmann::json& config_settings);

/// Same call-graph walk, seeded with the halting commands instead. Separate from
/// the restarting set rather than a flag on it because a macro can reach both,
/// and the two answers differ in what the user is promised afterwards.
[[nodiscard]] std::unordered_set<std::string>
analyze_host_halting_macros(const nlohmann::json& config_settings);

/// Check if a macro name is potentially dangerous (SAVE_CONFIG, FIRMWARE_RESTART, etc.).
[[nodiscard]] bool is_dangerous_macro(const std::string& name);

/// Same, plus this printer's own macros that reach one of those commands, as
/// recorded on PrinterDiscovery during discovery. Prefer this overload wherever
/// a confirmation is being decided; the name-only form cannot see a wrapper.
[[nodiscard]] bool is_dangerous_macro(const std::string& name, const PrinterDiscovery& hw);

/// How a macro click proceeds, decided by decide_macro_run().
enum class MacroRunAction {
    ConfirmDangerous, ///< The dangerous-macro dialog must be accepted first.
    Run,              ///< Execute now with the returned params.
    ConfirmRun,       ///< Ask "Run X?" first, then execute with no params.
    Prompt,           ///< Show the parameter modal, prefilled with the returned params.
    PromptUnknown,    ///< Show the free-form modal for a macro whose params are unknown.
};

/// What one caller knows about the click it is dispatching. A caller that never
/// checks a flag leaves it false.
struct MacroRunRequest {
    bool dangerous = false;           ///< is_dangerous_macro() on this printer.
    bool dangerous_confirmed = false; ///< the dangerous-macro dialog was already accepted.
    bool prompt_for_params = true;    ///< false: never raise the param modal, run with no params.
    bool confirm_plain_run = false;   ///< ask "Run X?" before a run that raises no param modal.
    /// Candidate values for the macro's declared parameters, keyed by name.
    std::map<std::string, std::string> known_values;
};

/// The action to take plus the parameters it settled on: Run carries the params
/// to send (empty unless a full prefill supplied them), Prompt carries the
/// prefill for the modal.
struct MacroRunDecision {
    MacroRunAction action = MacroRunAction::Run;
    std::map<std::string, std::string> params;
};

/// Decide how to run a macro: confirm it as dangerous, ask "Run X?", raise the
/// parameter modal, or run it now. The one rule shared by the macro panel, the
/// favorite-macro widget, the filament router and the quick buttons; each
/// caller maps the returned action onto its own dialogs and lifetime handling.
///
/// An unconfirmed dangerous macro outranks everything. A click that will raise
/// no param modal (prompt_for_params false, or the macro takes none) is a plain
/// run, confirmed only when asked and never twice for a dangerous macro. A
/// macro with declared parameters runs without a prompt when known_values
/// covers every one of them, else prompts with the names it did cover; an
/// UNKNOWN macro always prompts free-form. KNOWN_PARAMS always carries at
/// least one parameter (MacroParamCache never stores an empty list as known).
[[nodiscard]] MacroRunDecision decide_macro_run(const CachedMacroInfo& cached,
                                                const MacroRunRequest& req);

/// What a macro does to the Klipper host, which decides how to read a dropped rpc.
enum class MacroHostEffect {
    None,     ///< An ordinary macro. A failed rpc is a failure.
    Restarts, ///< The host goes away and comes back (SAVE_CONFIG, RESTART).
    Halts,    ///< The host goes away and stays down (M112, SHUTDOWN, EMERGENCY_STOP).
};

/// What to tell the user when a macro's G-code RPC comes back failed.
enum class MacroFailureReport {
    Error,           ///< A genuine failure. Show it.
    ExpectedRestart, ///< The host went away mid-command and is coming back.
    ExpectedHalt,    ///< The host went away mid-command and is NOT coming back.
};

/// Which effect @p name has on this printer, name check plus its own wrappers.
///
/// Halts wins a tie. A macro whose body reaches both a restart and a halt leaves
/// the host down, and promising a restart there is the failure mode this
/// distinction exists to prevent.
[[nodiscard]] MacroHostEffect macro_host_effect(const std::string& name,
                                                const PrinterDiscovery& hw);

/// Decide which of those a failed macro RPC actually is.
///
/// Klipper's SAVE_CONFIG writes printer.cfg and then restarts the host as its
/// last act, so it NEVER acknowledges the command - Moonraker fails the pending
/// `printer.gcode.script` with 503 "Klippy Disconnected". A macro that ends in
/// one (ZMOD's AUTO_FULL_BED_LEVEL, shipped as the "Bed Level" button on eight
/// presets) therefore reports a red "Bed Level failed" toast every time it
/// SUCCEEDS. Same defect as prestonbrown/helixscreen#1359, one layer out: that
/// one was the panels taking a dropped SAVE_CONFIG rpc at face value, this is
/// the macro runner doing it.
///
/// Both halves have to hold, which is why the error is inspected rather than
/// waved through on @p macro_restarts_host alone: a macro CAN be rejected
/// outright (unknown command, unhomed axis), and Klipper's own rejection text
/// arrives as a JSON-RPC error just like the disconnect does. Only the shapes a
/// vanishing host produces are absorbed; anything carrying Klipper's own
/// complaint is still a real failure and still gets said out loud.
///
/// The halt case is why this reports three outcomes rather than two. Both are
/// absorbed - the macro did what it said - but only a restart may arm
/// begin_expected_klippy_restart(), which suppresses the recovery dialog. A halt
/// needs that dialog promptly, and needs no toast of its own: the dialog says
/// everything a toast could, and says it about a printer that has stopped.
///
/// @param effect  macro_host_effect(name, hw).
/// @param err     The error the RPC came back with.
[[nodiscard]] MacroFailureReport classify_macro_rpc_failure(MacroHostEffect effect,
                                                            const MoonrakerError& err);

} // namespace helix
