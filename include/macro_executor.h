// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "macro_param_modal.h"

#include <string>
#include <unordered_set>

class IMoonrakerAPI;

namespace helix {

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

/// Check if a macro name is potentially dangerous (SAVE_CONFIG, FIRMWARE_RESTART, etc.).
[[nodiscard]] bool is_dangerous_macro(const std::string& name);

} // namespace helix
