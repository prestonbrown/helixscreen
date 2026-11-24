// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <string>
#include <vector>

namespace helix {

class PrinterDiscovery;

/// The macros that throw a filament-path bypass on hardware with no bypass
/// command of its own. Empty names mean this machine cannot bypass, which is
/// the honest answer for one that exposes neither a switch nor a wrapper.
struct BypassMacros {
    std::string on;  ///< Engages bypass (disables the managed filament path)
    std::string off; ///< Releases it again
};

/// Which of a backend's @p conventional wrapper names this printer defines; a
/// name is empty when discovery did not see it.
[[nodiscard]] BypassMacros detect_bypass_macros(const PrinterDiscovery& hw,
                                                const BypassMacros& conventional);

/// Choices for a bypass macro picker: kAutoMacro followed by every macro whose
/// name mentions BYPASS, sorted. Empty when the printer has none.
[[nodiscard]] std::vector<std::string> bypass_macro_options(const PrinterDiscovery& hw);

/// resolve_bypass_macros() over an already-detected pair, for a caller that
/// keeps detection and re-resolves when the user picks another macro.
[[nodiscard]] BypassMacros resolve_bypass_macros(const BypassMacros& detected,
                                                 const std::string& on_choice,
                                                 const std::string& off_choice);

/// Resolve the bypass macros for this printer, from discovery and the user's
/// stored overrides.
///
/// Auto-detection looks for the backend's @p conventional names; a machine exposing neither
/// resolves to empty, so no bypass is offered rather than a control with
/// nothing safe behind it. An explicit override is honoured verbatim even when
/// discovery has not seen it - the owner may know something discovery does not,
/// and Klipper's own error is the honest signal.
///
/// Driving the underlying pin directly is deliberately not an option here: it
/// would skip the unload-first and refuse-during-print guards the wrapper
/// macros exist to enforce.
[[nodiscard]] BypassMacros resolve_bypass_macros(const PrinterDiscovery& hw,
                                                 const BypassMacros& conventional,
                                                 const std::string& on_override,
                                                 const std::string& off_override);

/**
 * @brief Whether the bypass controls should be available on this machine.
 *
 * Folds the user's force-bypass override into the firmware's own report. Four
 * places need this answer — the AmsState subject that drives the sidebar toggle,
 * the Device Operations section, the bypass node on the filament path, and the
 * backends' own enable_bypass() guards — and they must agree, or the toggle
 * appears and then refuses to act. bypass_node_visible() already exists because
 * that exact condition drifted across four render sites; this keeps the override
 * from repeating it.
 *
 * The firmware value is left untouched so the override can be switched off and
 * reality returns without a re-parse.
 *
 * @param firmware_supports_bypass AmsSystemInfo::supports_bypass, as reported
 * @param force_override User setting: offer bypass despite a firmware "no"
 * @return true when bypass UI and operations should be offered
 */
[[nodiscard]] constexpr bool bypass_available(bool firmware_supports_bypass, bool force_override,
                                              bool overridable) {
    return firmware_supports_bypass || (force_override && overridable);
}

/// Whether the "Enable Bypass Controls" override can change anything on this
/// system: there is a filament system, its firmware says no, and its kind could
/// act on a yes.
[[nodiscard]] inline bool bypass_override_offered(const AmsSystemInfo& info) {
    return info.presents_filament_system() && !info.supports_bypass &&
           ams_type_traits(info.type).bypass_overridable;
}
/**
 * @brief Whether a bypass toggle should be offered at all on this machine.
 *
 * The two static reasons a toggle is pointless: bypass is not available here,
 * and a hardware sensor owns the bypass so the firmware ignores the command.
 * BypassToggleController refuses on both, so a surface asking this before it
 * draws a toggle cannot offer a control the controller will only refuse.
 *
 * Not yet asked everywhere: the AMS sidebar's bypass_row and the Device
 * Operations row each gate on availability alone, or spell the pair out as two
 * separate XML bindings. On AFC hardware reporting a bypass sensor the sidebar
 * therefore draws a live switch that is refused on every tap.
 *
 * Deliberately NOT including the print guard: that one is live state, so a
 * toggle stays drawn and goes disabled rather than vanishing mid-print.
 *
 * @param available            bypass_available() / bypass_available_for()
 * @param has_hardware_sensor  AmsSystemInfo::has_hardware_bypass_sensor
 */
[[nodiscard]] constexpr bool bypass_toggle_offered(bool available, bool has_hardware_sensor) {
    return available && !has_hardware_sensor;
}

/// Gather bypass_available()'s override input from settings. Split from the pure
/// rule above so the rule is testable without standing up SettingsManager, which
/// is the same split bypass_node_visible() / bypass_node_visible_for() uses.
[[nodiscard]] bool bypass_available_for(const AmsSystemInfo& info);

} // namespace helix
