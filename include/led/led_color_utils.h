// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

#include "hv/json.hpp" // JSON_HPP_OK: inline reader of the json

/**
 * @file led_color_utils.h
 * @brief Shared parsing and channel-conversion helpers for Klipper LED objects.
 *
 * Converts a Moonraker `color_data` payload's 0.0-1.0 channel levels into
 * 0-255 bytes and 0-100 percent. Every reader of `color_data` goes through
 * these so one rounding rule decides brightness everywhere (#1129).
 */

namespace helix::led {

/// One LED's channel levels as reported in `color_data[0]`, 0.0-1.0.
struct RgbwF {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double w = 0.0;
    /// Number of entries in `color_data[0]`. >= 4 means the strip exposes a W
    /// channel — NativeBackend uses this to re-detect RGBW capability.
    int channels = 0;
};

/**
 * @brief Parse `color_data[0]` for one Klipper LED object.
 *
 * @param led_obj The per-LED object from a Moonraker status payload, e.g.
 *                `status["neopixel chamber"]`.
 * @param out     Receives the channel levels; untouched when parsing fails.
 * @return false if the object has no usable color_data (missing, not an array,
 *         empty, fewer than 3 channels, or a non-numeric RGB channel).
 */
inline bool parse_color_data(const nlohmann::json& led_obj, RgbwF& out) {
    if (!led_obj.contains("color_data") || !led_obj["color_data"].is_array() ||
        led_obj["color_data"].empty()) {
        return false;
    }

    // color_data is an array of [R, G, B, W] arrays (one per LED in the strip).
    const auto& first = led_obj["color_data"][0];
    if (!first.is_array() || first.size() < 3) {
        return false;
    }
    // Field-restricted Moonraker subscriptions can deliver null channel
    // values for strips that don't expose a particular component; guard
    // each get<double>() so a null doesn't throw type_error.302 and
    // unwind into main().
    if (!first[0].is_number() || !first[1].is_number() || !first[2].is_number()) {
        return false;
    }

    out.r = first[0].get<double>();
    out.g = first[1].get<double>();
    out.b = first[2].get<double>();
    out.w = (first.size() >= 4 && first[3].is_number()) ? first[3].get<double>() : 0.0;
    out.channels = static_cast<int>(first.size());
    return true;
}

/// Convert a 0.0-1.0 channel level to a 0-255 byte (round half up, clamped).
inline uint8_t to_channel_byte(double v) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(v * 255.0 + 0.5), 0, 255));
}

/// Convert a 0-255 channel byte to 0-100 percent (rounded). A non-zero channel
/// never reports 0% — an LED that is visibly lit must not display "0%".
inline int channel_to_percent(uint8_t c) {
    int pct = (c * 100 + 127) / 255;
    if (pct < 1 && c > 0) {
        pct = 1;
    }
    return pct;
}

/// Scale a channel byte so that `max_c` maps to 255, recovering the
/// full-brightness form of a dimmed color. Returns 0 when `max_c` is 0.
inline uint8_t scale_channel_to_full(uint8_t c, uint8_t max_c) {
    if (max_c == 0) {
        return 0;
    }
    return static_cast<uint8_t>(std::min(255, c * 255 / max_c));
}

/// Pack 0.0-1.0 channel levels into 0x00RRGGBB.
inline uint32_t pack_rgb(double r, double g, double b) {
    return (static_cast<uint32_t>(to_channel_byte(r)) << 16) |
           (static_cast<uint32_t>(to_channel_byte(g)) << 8) | to_channel_byte(b);
}

/// Unpack a packed 0x00RRGGBB color into 0.0-1.0 channel levels.
inline void unpack_rgb(uint32_t rgb, double& r, double& g, double& b) {
    r = static_cast<double>((rgb >> 16) & 0xFF) / 255.0;
    g = static_cast<double>((rgb >> 8) & 0xFF) / 255.0;
    b = static_cast<double>(rgb & 0xFF) / 255.0;
}

} // namespace helix::led
