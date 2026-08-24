// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_footer_summary.h"

#include "gcode_color_metadata.h"
#include "operation_patterns.h"

#include <algorithm>
#include <cstdlib>

namespace helix::gcode {

namespace {

// Anything below this counts as "did not print with this tool". Sits just
// above float noise rather than at a physically meaningful weight: a tool the
// file really uses must never be dropped (its filament check would be skipped
// silently), while an extra chip for a tool that barely extrudes is harmless.
constexpr double USED_GRAMS_EPSILON = 1e-9;

std::string_view trim(std::string_view sv) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!sv.empty() && is_space(sv.front())) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && is_space(sv.back())) {
        sv.remove_suffix(1);
    }
    return sv;
}

// The key of a `; key = value` comment, lowercased, with the leading
// semicolons and surrounding whitespace removed. Empty when the line has no
// '=' (so it is not a key/value comment at all).
std::string comment_key(std::string_view line, size_t eq) {
    std::string_view key = line.substr(0, eq);
    while (!key.empty() && (key.front() == ';' || key.front() == ' ' || key.front() == '\t')) {
        key.remove_prefix(1);
    }
    return to_lower(std::string(trim(key)));
}

bool is_colour_key(const std::string& key) {
    return key == "filament_colour" || key == "filament_color" || key == "extruder_colour" ||
           key == "extruder_color";
}

bool is_extruder_colour_key(const std::string& key) {
    return key == "extruder_colour" || key == "extruder_color";
}

// Parse one comma-separated grams vector into the indices that printed.
// Unparsable tokens count as zero but still consume their slot, so a garbled
// entry never shifts the tools after it.
std::set<int> used_indices_from_grams(std::string_view values) {
    std::set<int> used;
    int index = 0;
    while (true) {
        const size_t comma = values.find(',');
        std::string_view tok =
            trim(comma == std::string_view::npos ? values : values.substr(0, comma));
        if (!tok.empty()) {
            const std::string owned(tok);
            char* end = nullptr;
            const double grams = std::strtod(owned.c_str(), &end);
            if (end != owned.c_str() && grams > USED_GRAMS_EPSILON) {
                used.insert(index);
            }
        }
        ++index;
        if (comma == std::string_view::npos) {
            break;
        }
        values.remove_prefix(comma + 1);
    }
    return used;
}

} // namespace

GcodeFooterSummary parse_gcode_footer_summary(std::string_view tail) {
    GcodeFooterSummary summary;

    // Palettes are collected separately so extruder_colour can take precedence
    // over filament_colour regardless of which the slicer wrote first.
    std::vector<std::string> extruder_palette;
    std::vector<std::string> filament_palette;

    while (!tail.empty()) {
        const size_t nl = tail.find('\n');
        std::string_view line = (nl == std::string_view::npos) ? tail : tail.substr(0, nl);
        if (nl != std::string_view::npos) {
            tail.remove_prefix(nl + 1);
        } else {
            tail = {};
        }

        // Only comments carry slicer metadata; skip the moves cheaply.
        const std::string_view body = trim(line);
        if (body.empty() || body.front() != ';') {
            continue;
        }
        const size_t eq = body.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }

        const std::string key = comment_key(body, eq);
        if (key == "filament used [g]") {
            // Last occurrence wins: on a windowed read the earliest match may
            // be from an older per-object block, and the file's final summary
            // is the one that describes the whole print.
            summary.has_usage_line = true;
            summary.tools_used = used_indices_from_grams(body.substr(eq + 1));
        } else if (is_colour_key(key)) {
            std::vector<std::string> palette;
            if (parse_filament_color_palette(body, palette)) {
                if (is_extruder_colour_key(key)) {
                    extruder_palette = std::move(palette);
                } else {
                    filament_palette = std::move(palette);
                }
            }
        }
    }

    summary.colours =
        extruder_palette.empty() ? std::move(filament_palette) : std::move(extruder_palette);
    return summary;
}

size_t gcode_tail_window_bytes(uint64_t file_size, uint64_t gcode_end_byte) {
    uint64_t window = GCODE_TAIL_WINDOW_DEFAULT;

    // A gcode_end_byte at or past EOF is nonsense (or a file with no footer at
    // all) — fall back to the fixed window rather than asking for 0 bytes.
    if (gcode_end_byte > 0 && gcode_end_byte < file_size) {
        window = file_size - gcode_end_byte;
    }

    window = std::clamp<uint64_t>(window, GCODE_TAIL_WINDOW_MIN, GCODE_TAIL_WINDOW_MAX);
    if (file_size > 0) {
        window = std::min<uint64_t>(window, file_size);
    }
    return static_cast<size_t>(std::max<uint64_t>(window, 1));
}

} // namespace helix::gcode
