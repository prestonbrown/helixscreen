// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "hv/json.hpp" // JSON_HPP_OK: inline reader of the json

namespace helix {
struct PlatformCapabilities;
}

/**
 * @file board_fingerprint.h
 * @brief One string naming the board a measurement was taken on
 *
 * Anything that measures the hardware once and remembers the result (screensaver levels, the
 * glass probe) keys it on this, so a config moved to another board measures again.
 */

namespace helix::ui {

/// What the board fingerprint is made of.
struct BoardFacts {
    std::string display_backend; ///< "drm", "egl", "fbdev" or "sdl"
    int cores = 0;
    float bogomips = 0.0f;
    int32_t width = 0;
    int32_t height = 0;
    int color_depth = 0;
};

/// The board and app version a remembered measurement was taken under. A record
/// whose key differs from the running one is stale: the board was swapped, or
/// the code that measured changed.
struct MeasurementKey {
    std::string board;
    std::string version;

    bool operator==(const MeasurementKey& o) const {
        return board == o.board && version == o.version;
    }
};

/// The "board" and "version" strings of a stored record, or nullopt unless both are present
/// and strings. Each store parses its own payload around this.
inline std::optional<MeasurementKey> parse_measurement_key(const nlohmann::json& node) {
    if (!node.is_object()) {
        return std::nullopt;
    }
    const auto board = node.find("board");
    const auto version = node.find("version");
    if (board == node.end() || !board->is_string() || version == node.end() ||
        !version->is_string()) {
        return std::nullopt;
    }
    return MeasurementKey{board->get<std::string>(), version->get<std::string>()};
}

/// Adds the key's "board" and "version" to a record being written.
inline void write_measurement_key(nlohmann::json& node, const MeasurementKey& key) {
    node["board"] = key.board;
    node["version"] = key.version;
}

/// "<backend>/<cores>c/<bogomips to the nearest 100>bm/<width>x<height>/<depth>bpp"
std::string board_fingerprint(const BoardFacts& facts);

/// The facts for this run: `caps`, the default display's resolution and the build's color
/// depth. An empty `display_backend` reads as "unknown". Main thread.
BoardFacts current_board_facts(const helix::PlatformCapabilities& caps,
                               const std::string& display_backend);

} // namespace helix::ui
