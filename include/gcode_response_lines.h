// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string>

#include "hv/json.hpp"

namespace helix {

/// Calls `fn(const std::string&)` for each line of a `notify_gcode_response` frame.
/// params is `[["l1", "l2"]]` or `["l1", "l2"]`; Moonraker has sent both. A frame without a
/// non-empty params array, and any non-string element, is skipped.
template <typename Fn> void for_each_gcode_response_line(const nlohmann::json& msg, Fn&& fn) {
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }
    const auto& params = msg["params"];
    const auto& lines = params[0].is_array() ? params[0] : params;
    if (!params[0].is_array() && !params[0].is_string()) {
        return;
    }
    for (const auto& line : lines) {
        if (line.is_string()) {
            fn(line.get<std::string>());
        }
    }
}

/// Layer numbers a G-code response line reports; -1 for a field the line does not carry.
struct LayerLine {
    int current = -1;
    int total = -1;
};

/// Reads `SET_PRINT_STATS_INFO CURRENT_LAYER=N [TOTAL_LAYER=N]` (Klipper's echo of the
/// command) or the `;LAYER:N` comment OrcaSlicer, PrusaSlicer and Cura emit. The comment form
/// is consulted only when the command form gave no current layer.
inline LayerLine parse_layer_line(const std::string& line) {
    LayerLine out;
    if (line.empty()) {
        return out;
    }
    if (line.find("SET_PRINT_STATS_INFO") != std::string::npos) {
        auto pos = line.find("CURRENT_LAYER=");
        if (pos != std::string::npos) {
            out.current = std::atoi(line.c_str() + pos + 14);
        }
        pos = line.find("TOTAL_LAYER=");
        if (pos != std::string::npos) {
            out.total = std::atoi(line.c_str() + pos + 12);
        }
    }
    if (out.current < 0 && line.size() >= 8 && line.compare(0, 7, ";LAYER:") == 0) {
        out.current = std::atoi(line.c_str() + 7);
    }
    return out;
}

} // namespace helix
