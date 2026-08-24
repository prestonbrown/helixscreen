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

    // Split everything after '=' by ';' and trim each token. Empty/invalid
    // tokens become empty strings so palette stays slot-aligned with tool
    // indices: e.g. "#A;;#B" → ["#A", "", "#B"].
    std::string_view rest = line.substr(eq + 1);
    while (!rest.empty()) {
        size_t semi = rest.find(';');
        std::string_view tok = (semi == std::string_view::npos) ? rest : rest.substr(0, semi);
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
        // Pure-empty tokens (back-to-back semicolons) also become empty
        // placeholders to preserve slot alignment.
        else if (semi != std::string_view::npos) {
            out_palette.emplace_back();
        }

        if (semi == std::string_view::npos)
            break;
        rest.remove_prefix(semi + 1);
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

} // namespace helix::gcode
