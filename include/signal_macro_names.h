// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The signal macros (_HELIX_READY, _HELIX_ENDED, _HELIX_RESET, _HELIX_PHASE_*)
// carry a leading underscore so Fluidd and Mainsail leave them out of their
// macro lists. A printer whose own config still calls a visible spelling
// (HELIX_READY) gets a forwarder of that name appended to helix_macros.cfg,
// so the call keeps working while the pack itself defines only hidden names.
//
// No I/O and no LVGL: callers fetch configfile.settings and do the uploads.

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

} // namespace helix::signal_macros
