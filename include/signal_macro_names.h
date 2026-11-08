// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The signal macros (_HELIX_READY, _HELIX_ENDED, _HELIX_RESET, _HELIX_PHASE_*)
// carry a leading underscore so Fluidd and Mainsail leave them out of their
// macro lists. A printer whose own config still calls a visible spelling
// (HELIX_READY) gets a forwarder of that name appended to helix_macros.cfg,
// so the call keeps working while the pack itself defines only hidden names.
//
// Which forwarders a printer needs comes from scan() over configfile.settings:
// every name some config outside the pack calls, unless the user defined that
// name themselves. drift() compares that with what is loaded.
//
// No I/O and no LVGL: callers fetch configfile.settings and do the uploads.

#include "json_fwd.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace helix::signal_macros {

/// The signal macros' visible spellings, in pack order. The hidden name is "_" + this.
inline constexpr std::array<std::string_view, 11> kVisibleNames = {"HELIX_READY",
                                                                   "HELIX_ENDED",
                                                                   "HELIX_RESET",
                                                                   "HELIX_PHASE_HOMING",
                                                                   "HELIX_PHASE_HEATING_BED",
                                                                   "HELIX_PHASE_HEATING_NOZZLE",
                                                                   "HELIX_PHASE_BED_MESH",
                                                                   "HELIX_PHASE_QGL",
                                                                   "HELIX_PHASE_Z_TILT",
                                                                   "HELIX_PHASE_CLEANING",
                                                                   "HELIX_PHASE_PURGING"};

/// Every forwarder's description starts with this; it tells our forwarder from
/// a user macro of the same name.
inline constexpr std::string_view kAliasDescriptionPrefix = "HelixScreen alias for ";

[[nodiscard]] std::string hidden_name(std::string_view visible);

/// One [gcode_macro <visible>] forwarder per name, in kVisibleNames order whatever
/// the input order; unknown names are dropped; empty input gives "".
[[nodiscard]] std::string alias_block(const std::vector<std::string>& visible_names);

struct SignalCaller {
    std::string
        section; ///< configfile.settings key, lowercased by Klipper ("gcode_macro print_start")
    std::string option; ///< "gcode", "press_gcode", ...
    std::string name;   ///< the visible name it calls, uppercased
    bool operator==(const SignalCaller& o) const;
};

struct SignalMacroScan {
    std::vector<SignalCaller>
        callers; ///< config outside the pack and the aliases calling a visible name
    std::vector<std::string>
        aliases; ///< visible names defined by our forwarder (kAliasDescriptionPrefix)
    std::vector<std::string>
        foreign; ///< visible names defined by a section that is neither ours nor an alias
    bool hidden_loaded = false; ///< every hidden name is defined
};

/// Reads every string option of every section, so gcode in [delayed_gcode],
/// [gcode_button], [idle_timeout] and the like counts as well as macro bodies.
/// A call matches case-insensitively with no [A-Za-z0-9_] on either side, and
/// any mention counts (a Jinja lookup too): a missed call answers "Unknown
/// command", while a spurious one only keeps a name visible.
///
/// A [gcode_macro <visible name>] section is our forwarder when its description
/// is kAliasDescriptionPrefix + the hidden name, an earlier pack's own macro when
/// its gcode names _HELIX_STATE (every pack's signal bodies write it), and the
/// user's own otherwise. A user's own one is in `foreign` and is scanned for calls.
///
/// @param pack_sections the section names the bundled pack defines, both spellings, lowercased
[[nodiscard]] SignalMacroScan scan(const nlohmann::json& config_settings,
                                   const std::vector<std::string>& pack_sections);

/// Visible names to alias: each name a caller uses, minus `foreign` (the user's own
/// macro of that name wins, and a forwarder would merge into it). Sorted in kVisibleNames order.
[[nodiscard]] std::vector<std::string> names_to_alias(const SignalMacroScan& scan,
                                                      const std::vector<std::string>& extra_names);

enum class SignalNameDrift {
    None,         ///< what is loaded is what an update would stage
    MoveToHidden, ///< the old visible pack is loaded: no hidden names
    AddAliases,   ///< a caller uses a name with no definition loaded
    DropAliases,  ///< an alias no caller uses
};

[[nodiscard]] SignalNameDrift drift(const SignalMacroScan& scan,
                                    const std::vector<std::string>& extra_names);

} // namespace helix::signal_macros
