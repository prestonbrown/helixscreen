// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The signal macros (_HELIX_READY, _HELIX_ENDED, _HELIX_RESET, _HELIX_PHASE_*)
// carry a leading underscore so Fluidd and Mainsail leave them out of their
// macro lists. A printer whose own config still calls a visible spelling
// (HELIX_READY) gets a forwarder of that name appended to helix_macros.cfg,
// so the call keeps working while the pack itself defines only hidden names.
//
// Which forwarders a printer needs comes from scan() over configfile.settings.
// A printer that had the visible names loaded (an earlier pack, or our
// forwarders), or whose config calls any of them, keeps all eleven, since a
// sliced file's start G-code may call any of them and the scan cannot see it.
// A name the user defined themselves is never forwarded. drift() compares that
// with what is loaded.
//
// No I/O and no LVGL: callers fetch configfile.settings and do the uploads.

#include "helix_plugin_features.h"
#include "json_fwd.h"

#include <array>
#include <optional>
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

/// The description every pack ever gave each signal macro, in kVisibleNames order.
/// It is how scan() tells an earlier pack's [gcode_macro HELIX_READY] from the
/// user's own macro of that name.
inline constexpr std::array<std::string_view, kVisibleNames.size()> kPackDescriptions = {
    "Signal that print preparation is complete - ADD THIS TO END OF PRINT_START",
    "Signal that print has ended",
    "Reset HelixScreen state (for cancel/error recovery)",
    "Signal homing phase",
    "Signal bed heating phase",
    "Signal nozzle heating phase",
    "Signal bed mesh/leveling phase",
    "Signal quad gantry level phase",
    "Signal Z tilt adjustment phase",
    "Signal nozzle cleaning phase",
    "Signal purge/priming phase"};

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
    bool hidden_loaded = false;  ///< every hidden name is defined
    bool visible_loaded = false; ///< a pack or our forwarder defines some visible name
};

/// Reads every string option of every section, so gcode in [delayed_gcode],
/// [gcode_button], [idle_timeout] and the like counts as well as macro bodies.
/// A call matches case-insensitively with no [A-Za-z0-9_] on either side, and
/// any mention counts (a Jinja lookup too): a missed call answers "Unknown
/// command", while a spurious one only keeps a name visible.
///
/// A [gcode_macro <visible name>] section is our forwarder when its description
/// is kAliasDescriptionPrefix + the hidden name, an earlier pack's own macro when
/// its description is that name's kPackDescriptions entry, and the user's own
/// otherwise. A user's own one is in `foreign` and is scanned for calls.
///
/// @param pack_sections the section names the bundled pack defines, both spellings, lowercased
[[nodiscard]] SignalMacroScan scan(const nlohmann::json& config_settings,
                                   const std::vector<std::string>& pack_sections);

/// Visible names to alias: all eleven when the visible names were loaded, a
/// caller uses one, or `extra_names` holds one; none otherwise. Never a name in
/// `foreign` (the user's own macro of that name wins, and a forwarder would merge
/// into it). Sorted in kVisibleNames order.
[[nodiscard]] std::vector<std::string> names_to_alias(const SignalMacroScan& scan,
                                                      const std::vector<std::string>& extra_names);

enum class SignalNameDrift {
    None,         ///< what is loaded is what an update would stage
    MoveToHidden, ///< the old visible pack is loaded: no hidden names
    AddAliases,   ///< a caller uses a name with no definition loaded
    DropAliases,  ///< a forwarder names_to_alias would not stage
};

[[nodiscard]] SignalNameDrift drift(const SignalMacroScan& scan,
                                    const std::vector<std::string>& extra_names);

/// What the helper-macros row says about the signal names; the row maps it to text.
enum class SignalNamesLine {
    None,                 ///< nothing to say: no scan, or the hidden names are not loaded yet
    AllHidden,            ///< every signal macro is hidden from macro lists
    AliasesKept,          ///< forwarders keep the visible names working, and listed
    PluginWillRename,     ///< PRINT_START's phase-tracking calls; the plugin renames them
    UpdatePluginToRename, ///< the same calls, and an installed plugin too old to rename them
};

/// The names of the macros HelixScreen before 1.1 put phase-tracking calls in.
inline constexpr std::array<std::string_view, 3> kInstrumentedSections = {
    "gcode_macro print_start", "gcode_macro start_print", "gcode_macro _print_start"};

/// configfile.settings carries no comments, so the marker blocks themselves are
/// invisible here; a visible call in a macro the old instrumentation wrote to
/// stands in for them.
[[nodiscard]] inline bool marker_callers_present(const SignalMacroScan& scan) {
    for (const auto& caller : scan.callers) {
        for (auto section : kInstrumentedSections) {
            if (caller.section == section && caller.option == "gcode") {
                return true;
            }
        }
    }
    return false;
}

/// What the plugin's last signal macro migration report says it will do with
/// the marker blocks (GET /server/helix/signal_migration, its `state`).
enum class SignalMigrationReport {
    Unknown,      ///< not read, or no plugin that reports one
    NothingFound, ///< it found no visible call in a marker block
    WillRename,   ///< it renamed them, or will once Klipper has the hidden names
    WontRename,   ///< switched off, firmware-managed, or a file it would not touch
};

/// The report for a `state` string of the plugin's migration report.
[[nodiscard]] inline SignalMigrationReport signal_migration_report(std::string_view state) {
    if (state == "nothing") {
        return SignalMigrationReport::NothingFound;
    }
    if (state == "done" || state == "waiting_for_pack") {
        return SignalMigrationReport::WillRename;
    }
    if (state.empty()) {
        return SignalMigrationReport::Unknown;
    }
    return SignalMigrationReport::WontRename;
}

/// What the HelixPrint plugin can do about the marker blocks.
struct SignalMigrationPlugin {
    bool installed = false; ///< a HelixPrint plugin answers
    bool reported = false;  ///< it lists signal_macro_migration, even one it cannot run
    PluginFeatureState feature = PluginFeatureState::Absent;
    SignalMigrationReport report = SignalMigrationReport::Unknown;
};

/// Header-only, so the macros row's state links where the scan does not. The
/// plugin lines show only with a plugin installed, and the rename line only
/// once the plugin's own report says it renames: the marker blocks are
/// invisible here, and a hand-written call in PRINT_START is no block.
[[nodiscard]] inline SignalNamesLine signal_names_line(const SignalMacroScan& scan,
                                                       const SignalMigrationPlugin& plugin) {
    if (!scan.hidden_loaded) {
        return SignalNamesLine::None;
    }
    if (marker_callers_present(scan) && plugin.installed) {
        if (!plugin.reported) {
            return SignalNamesLine::UpdatePluginToRename;
        }
        if (plugin.feature == PluginFeatureState::Ready &&
            plugin.report == SignalMigrationReport::WillRename) {
            return SignalNamesLine::PluginWillRename;
        }
    }
    if (!scan.aliases.empty() || !scan.callers.empty()) {
        return SignalNamesLine::AliasesKept;
    }
    return SignalNamesLine::AllHidden;
}

/// The visible signal name in a Klipper `Unknown command:"HELIX_X"` response,
/// uppercased, or nullopt for any other line.
[[nodiscard]] std::optional<std::string> unknown_signal_command(std::string_view response);

} // namespace helix::signal_macros
