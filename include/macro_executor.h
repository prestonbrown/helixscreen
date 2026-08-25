// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "macro_param_modal.h"

#include <string>
#include <unordered_set>

#include "hv/json.hpp"

class IMoonrakerAPI;

namespace helix {

class PrinterDiscovery;

/// Build a G-code string from a macro name and parameter result.
/// Generates SET_GCODE_VARIABLE commands for variable overrides,
/// then the macro call with inline KEY=VALUE params.
[[nodiscard]] std::string build_macro_gcode(const std::string& macro_name,
                                            const MacroParamResult& result);

/// Execute a macro via IMoonrakerAPI with the given parameters.
/// Builds gcode via build_macro_gcode() and sends via api->execute_gcode().
/// @param caller_tag Log tag for spdlog messages (e.g., "[MacrosPanel]")
void execute_macro_gcode(IMoonrakerAPI* api, const std::string& macro_name,
                         const MacroParamResult& result, const char* caller_tag);

/// Commands that restart the Klipper host or halt the printer. Seeds both the
/// name check below and the macro-body analysis.
[[nodiscard]] const std::unordered_set<std::string>& dangerous_command_names();

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

/// Check if a macro name is potentially dangerous (SAVE_CONFIG, FIRMWARE_RESTART, etc.).
[[nodiscard]] bool is_dangerous_macro(const std::string& name);

/// Same, plus this printer's own macros that reach one of those commands, as
/// recorded on PrinterDiscovery during discovery. Prefer this overload wherever
/// a confirmation is being decided; the name-only form cannot see a wrapper.
[[nodiscard]] bool is_dangerous_macro(const std::string& name, const PrinterDiscovery& hw);

} // namespace helix
