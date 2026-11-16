// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <string>
#include <vector>

namespace helix {

/// What a parameter's |default(...) filter holds.
enum class MacroDefaultKind {
    Absent,     ///< No |default(...) filter
    Literal,    ///< A number, a quoted string, or true/false/none: usable as the value itself
    Expression, ///< Evaluated by Klipper when the macro runs (a variable lookup, arithmetic)
};

/// Parsed macro parameter with optional default value
struct MacroParam {
    std::string name; ///< Parameter name (uppercase, e.g., "EXTRUDER_TEMP")
    /// Text inside |default(...): a literal with its quotes stripped, an expression
    /// verbatim. Empty if none.
    std::string default_value;
    bool is_variable = false; ///< True for Klipper variable_* fields (SET_GCODE_VARIABLE)
    MacroDefaultKind default_kind = MacroDefaultKind::Absent;
};

/// Parse macro parameters from a Klipper gcode_macro template.
/// Detects params.NAME, params['NAME'], params["NAME"] and 'NAME' [not] in params,
/// and extracts |default(VALUE) when present. Deduplicates by name.
[[nodiscard]] std::vector<MacroParam> parse_macro_params(const std::string& gcode_template);

/// Parse raw "KEY=VALUE KEY2=VALUE2" text into a parameter map.
/// Keys are uppercased to match Klipper convention. A token that is not
/// KEY=VALUE (no '=', or nothing before it) is left out of the map and, when
/// @p rejected is given, appended to it as typed.
[[nodiscard]] std::map<std::string, std::string>
parse_raw_macro_params(const std::string& raw_text, std::vector<std::string>* rejected = nullptr);

/// The text parse_raw_macro_params() reads back as @p values: "KEY=VALUE"
/// pairs joined by single spaces, in key order.
[[nodiscard]] std::string format_raw_macro_params(const std::map<std::string, std::string>& values);

/// Result from macro parameter modal: inline params and variable overrides
struct MacroParamResult {
    std::map<std::string, std::string> params;    ///< Inline params (MACRO KEY=VALUE)
    std::map<std::string, std::string> variables; ///< Variable overrides (SET_GCODE_VARIABLE)
};

} // namespace helix
