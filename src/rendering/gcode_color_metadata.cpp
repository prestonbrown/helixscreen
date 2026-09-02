// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_color_metadata.h"

#include "operation_patterns.h"

#include <cctype>
#include <cstring>

namespace helix::gcode {

namespace {

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Trim leading/trailing whitespace from a string_view.
std::string_view trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' ||
                           sv.front() == '\n' || sv.front() == '"' || sv.front() == '\'')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' ||
                           sv.back() == '\n' || sv.back() == '"' || sv.back() == '\'')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

bool parse_filament_color_palette(std::string_view line, std::vector<std::string>& out_palette) {
    out_palette.clear();

    // Limit keyword search to the part of the line before '=' to avoid matching
    // a value that happens to contain "filament_colour" etc. as a substring.
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    std::string_view key_part = line.substr(0, eq);
    bool keyword_match =
        contains_ci(key_part, "extruder_colour") || contains_ci(key_part, "extruder_color") ||
        contains_ci(key_part, "filament_colour") || contains_ci(key_part, "filament_color");
    if (!keyword_match) {
        return false;
    }

    // Split everything after '=' by ';' or ',' and trim each token. Slicers
    // emit both separator forms (OrcaSlicer's joined-config variant uses ',').
    // Empty/invalid tokens become empty strings so palette stays slot-aligned
    // with tool indices: e.g. "#A;;#B" → ["#A", "", "#B"].
    std::string_view rest = line.substr(eq + 1);
    while (!rest.empty()) {
        size_t sep = rest.find_first_of(";,");
        std::string_view tok = (sep == std::string_view::npos) ? rest : rest.substr(0, sep);
        tok = trim(tok);

        if (!tok.empty() && tok.front() == '#') {
            // Validate body: '#' followed by 6 or 8 hex digits.
            std::string_view body = tok.substr(1);
            bool valid = (body.size() == 6 || body.size() == 8);
            if (valid) {
                for (char c : body) {
                    if (!is_hex_digit(c)) {
                        valid = false;
                        break;
                    }
                }
            }
            out_palette.emplace_back(valid ? std::string(tok) : std::string());
        } else if (!tok.empty()) {
            out_palette.emplace_back();
        }
        // Pure-empty tokens (back-to-back separators) also become empty
        // placeholders to preserve slot alignment.
        else if (sep != std::string_view::npos) {
            out_palette.emplace_back();
        }

        if (sep == std::string_view::npos)
            break;
        rest.remove_prefix(sep + 1);
    }

    // "Found" requires at least one non-empty entry — a line like
    // `; extruder_colour = ` shouldn't claim to have parsed a palette.
    for (const auto& s : out_palette) {
        if (!s.empty()) {
            return true;
        }
    }
    out_palette.clear();
    return false;
}

std::string_view clean_color_hex(std::string_view value) {
    value = trim(value);
    if (value.empty() || value.front() != '#') {
        return {};
    }
    const std::string_view body = value.substr(1);
    if (body.size() != 6 && body.size() != 8) {
        return {};
    }
    for (const char c : body) {
        if (!is_hex_digit(c)) {
            return {};
        }
    }
    return value;
}

FileColorDecision classify_file_colors(const std::vector<std::string>& palette,
                                       const std::string& filament_color, int initial_tool_index) {
    FileColorDecision decision;
    decision.initial_tool = initial_tool_index;

    if (palette.size() > 1) {
        decision.palette = palette;
        return decision;
    }

    // Single-color print: prefer palette[initial_tool_index] when the slicer
    // emitted a multi-color metadata line and the gcode actually starts on a
    // non-T0 tool; fall back to the leading filament_color when the palette
    // doesn't cover the active tool or is absent.
    decision.single_color = filament_color;
    if (initial_tool_index >= 0 && initial_tool_index < static_cast<int>(palette.size()) &&
        !palette[static_cast<size_t>(initial_tool_index)].empty()) {
        decision.single_color = palette[static_cast<size_t>(initial_tool_index)];
    }
    return decision;
}

FileColorDecision classify_file_colors(const std::vector<std::string>& palette,
                                       const std::string& filament_color_hex) {
    FileColorDecision decision;
    decision.palette = palette;
    if (filament_color_hex.length() >= 2) {
        decision.single_color = filament_color_hex;
    }
    return decision;
}

} // namespace helix::gcode
