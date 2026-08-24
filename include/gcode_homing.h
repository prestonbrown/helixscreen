// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_homing.h
 * @brief Detect app-emitted homing commands (G28) in a G-code script.
 *
 * Used to block app-initiated homing while a print is active. On loadcell-Z
 * printers (e.g. AD5X) a G28 probes the nozzle DOWN into the bed; issued
 * mid-print it drives the nozzle into the part -> collision. See the guards in
 * IMoonrakerAPI::execute_gcode / MoonrakerMotionAPI::execute_gcode.
 */

#pragma once

#include <cctype>
#include <sstream>
#include <string>

namespace helix {

/// True if @p script issues a homing command: `G28` as the first whitespace-
/// delimited token of ANY line, matched case-insensitively.
///
/// Matches: "G28", "g28", "G28 X", "G28 X Y", "G28\nG1 X10" (any line homes).
/// Does NOT match: "G1 X28" (G28 as an argument), "M28", "G280" (different
/// command), or the empty string. The whole-token comparison is what keeps
/// "G280" and "G1 X28" out — a substring match would wrongly trip on both.
inline bool is_homing_gcode(const std::string& script) {
    std::istringstream lines(script);
    std::string line;
    while (std::getline(lines, line)) {
        // First non-blank character on the line.
        const size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) {
            continue; // blank / whitespace-only line
        }
        // First token spans until the next whitespace (or end of line).
        const size_t end = line.find_first_of(" \t\r", start);
        std::string token =
            line.substr(start, end == std::string::npos ? std::string::npos : end - start);
        for (char& c : token) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (token == "G28") {
            return true;
        }
    }
    return false;
}

} // namespace helix
