// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "macro_params.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace helix {

enum class MacroParamKnowledge { KNOWN_PARAMS, KNOWN_NO_PARAMS, UNKNOWN };

/// A gcode_macro template's call to action_emergency_stop, when it makes one.
struct PrinterStopCall {
    bool calls = false; ///< The template calls action_emergency_stop
    /// The call's argument when it is a plain quoted string literal; empty for no
    /// argument or anything Klipper would have to evaluate.
    std::string message;
};

/// Find the first action_emergency_stop(...) call in a gcode_macro template.
[[nodiscard]] PrinterStopCall find_printer_stop_call(std::string_view gcode_template);

/// Whether a gcode_macro template switches the Spoolman active spool: it runs
/// SET_ACTIVE_SPOOL, or calls Moonraker's spoolman_set_active_spool remote method.
[[nodiscard]] bool template_sets_active_spool(std::string_view gcode_template);

/// What a gcode_macro template does to temperature targets.
struct TemplateTemperatureUse {
    /// Each target set by SET_HEATER_TEMPERATURE (`heater <name>`) or
    /// SET_TEMPERATURE_FAN_TARGET (`temperature_fan <name>`), names lower-case.
    /// A name Klipper has to evaluate is `*`: it may be any of that kind.
    std::vector<std::string> targets;
    /// The upper-case first word of each line once leading Jinja blocks are
    /// stripped: the commands and macros the template runs.
    std::vector<std::string> commands;
};

[[nodiscard]] TemplateTemperatureUse template_temperature_use(std::string_view gcode_template);

struct CachedMacroInfo {
    MacroParamKnowledge knowledge = MacroParamKnowledge::UNKNOWN;
    std::vector<MacroParam> params;
    std::string description;        ///< From Klipper gcode_macro description field
    bool stops_printer = false;     ///< Running the macro shuts the printer down
    std::string stop_message;       ///< PrinterStopCall::message, when stops_printer
    bool sets_active_spool = false; ///< The template switches the Spoolman active spool
    /// The macro's `variable_<name>` config values as written, keyed by
    /// <name>: its state as configured, before any SET_GCODE_VARIABLE.
    std::map<std::string, std::string> variables;
    bool has_template = false; ///< configfile carried the macro's gcode
    TemplateTemperatureUse temperature_use;
};

/// Cache for macro parameter information, populated once during printer discovery.
/// Avoids per-click configfile.config queries by pre-parsing all gcode_macro templates.
class MacroParamCache {
  public:
    static MacroParamCache& instance();

    /// Populate cache from configfile.config response.
    /// @param config The configfile.config JSON object (keys like "gcode_macro clean_nozzle")
    /// @param known_macros Set of known macro names (uppercase, from printer's object list)
    void populate_from_configfile(const nlohmann::json& config,
                                  const std::unordered_set<std::string>& known_macros);

    /// Lookup cached info for a macro (case-insensitive).
    [[nodiscard]] CachedMacroInfo get(const std::string& macro_name) const;

    /// True if the macro is registered with Klipper (case-insensitive). Used to
    /// gate features that require a specific gcode_macro to be defined in the
    /// firmware (e.g. K2 AI detect needs LOAD_AI_RUN — not all variants ship it).
    [[nodiscard]] bool has_macro(const std::string& macro_name) const;

    /// Whether running @p macro_name sets @p target_full_name's temperature
    /// (`heater_generic chamber`, `temperature_fan exhaust`, `extruder`), following
    /// the macros it calls. nullopt when that cannot be read: the macro, or one it
    /// calls, has no body in configfile.
    [[nodiscard]] std::optional<bool> macro_sets_target(const std::string& macro_name,
                                                        const std::string& target_full_name) const;

    /// Whether running @p macro_name runs @p command (case-insensitive), as a
    /// line of its template or of a macro it calls. False when configfile
    /// carried no body for it.
    [[nodiscard]] bool macro_runs(const std::string& macro_name, const std::string& command) const;

    /// Upper-case names of the macros whose template stops the printer, each with
    /// its stop message (empty when not a literal).
    [[nodiscard]] std::map<std::string, std::string> printer_stop_commands() const;

    /// Whether populate_from_configfile() has read a configfile since the last clear().
    [[nodiscard]] bool is_populated() const;

    /// Changes whenever the cached macro set may have changed (any populate or
    /// clear). An answer computed against an older value describes a macro set
    /// this printer may no longer have, so it is not reusable.
    [[nodiscard]] uint64_t generation() const;

    /// Clear all cached state (call on disconnect/reconnect).
    void clear();

  private:
    MacroParamCache() = default;

    mutable std::mutex mutex_;
    // Key: lowercase macro name (e.g., "clean_nozzle")
    std::unordered_map<std::string, CachedMacroInfo> cache_;
    bool populated_ = false;
    uint64_t generation_ = 0;
};

} // namespace helix
