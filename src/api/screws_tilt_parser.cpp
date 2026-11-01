// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screws_tilt_parser.h"

#include "text_io.h"

#include <spdlog/spdlog.h>

#include <cctype>

namespace helix {

bool parse_screws_tilt_line(const std::string& line, ScrewTiltResult& out) {
    // Format: "// screw_name (base) : x=X, y=Y, z=Z" for reference
    // Format: "// screw_name : x=X, y=Y, z=Z : adjust DIR TT:MM" for non-reference

    if (line.rfind("//", 0) != 0) {
        return false;
    }

    ScrewTiltResult result;

    // Find the screw name (after "//" and any whitespace, before first " :" or " (")
    size_t name_start = 2; // Skip "//"
    while (name_start < line.length() && line[name_start] == ' ') {
        name_start++;
    }

    // "// action:..." is a host-action command (a macro's prompt dialog), which
    // can quote screw-shaped text but is never one of Klipper's result lines.
    if (line.compare(name_start, 7, "action:") == 0) {
        return false;
    }

    size_t name_end = line.find(" :");
    size_t base_pos = line.find(" (base)");

    if (base_pos != std::string::npos && (name_end == std::string::npos || base_pos < name_end)) {
        // Reference screw with "(base)" marker
        result.screw_name = line.substr(name_start, base_pos - name_start);
        result.is_reference = true;
    } else if (name_end != std::string::npos) {
        result.screw_name = line.substr(name_start, name_end - name_start);
        result.is_reference = false;
    } else {
        // Can't parse - skip this line
        spdlog::debug("[ScrewsTiltParser] Could not parse line: {}", line);
        return false;
    }

    // Trim whitespace from screw name (leading and trailing)
    while (!result.screw_name.empty() && result.screw_name.front() == ' ') {
        result.screw_name.erase(0, 1);
    }
    while (!result.screw_name.empty() && result.screw_name.back() == ' ') {
        result.screw_name.pop_back();
    }

    // Parse x, y, z values — look for "x=", "y=", "z="
    auto parse_float = [&line](const std::string& prefix) -> float {
        size_t pos = line.find(prefix);
        if (pos == std::string::npos) {
            return 0.0f;
        }
        pos += prefix.length();
        // Find end of number (next comma, space, or end of line)
        size_t end = line.find_first_of(", ", pos);
        if (end == std::string::npos) {
            end = line.length();
        }
        return text_io::parse_leading<float>(line.substr(pos, end - pos)).value_or(0.0f);
    };

    result.x_pos = parse_float("x=");
    result.y_pos = parse_float("y=");
    result.z_height = parse_float("z=");

    // Parse adjustment for non-reference screws
    // Look for ": adjust CW 01:15" or ": adjust CCW 00:30"
    if (!result.is_reference) {
        size_t adjust_pos = line.find(": adjust ");
        if (adjust_pos != std::string::npos) {
            result.adjustment = line.substr(adjust_pos + 9); // Skip ": adjust "
            // Trim any trailing whitespace
            while (!result.adjustment.empty() &&
                   std::isspace(static_cast<unsigned char>(result.adjustment.back()))) {
                result.adjustment.pop_back();
            }
        }
    }

    spdlog::debug("[ScrewsTiltParser] Parsed: {} at ({:.1f}, {:.1f}) z={:.3f} {}",
                  result.screw_name, result.x_pos, result.y_pos, result.z_height,
                  result.is_reference ? "(reference)" : result.adjustment);

    out = std::move(result);
    return true;
}

} // namespace helix
