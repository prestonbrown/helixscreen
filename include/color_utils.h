// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>

/**
 * @file color_utils.h
 * @brief Color manipulation and naming utilities
 *
 * Provides functions for describing colors in natural language,
 * converting between color spaces, and color manipulation.
 */

namespace helix {

/**
 * @brief Get a human-readable description of an RGB color
 *
 * Uses HSL color space to generate descriptive names like:
 * - "Vibrant Red", "Deep Blue", "Light Muted Green"
 * - Special cases: "White", "Black", "Dark Gray"
 *
 * Algorithm ported from Klipper DESCRIBE_COLOR macro.
 *
 * @param rgb RGB color value (0x00RRGGBB format)
 * @return Descriptive color name string
 *
 * @example
 * describe_color(0xFF0000); // Returns "Red" or "Vibrant Red"
 * describe_color(0x808080); // Returns "Gray"
 * describe_color(0x1A237E); // Returns "Dark Blue" or "Deep Indigo"
 */
std::string describe_color(uint32_t rgb);

/**
 * @brief Convert RGB to HSL color space
 *
 * @param rgb RGB color value (0x00RRGGBB format)
 * @param h Output: Hue 0-360
 * @param s Output: Saturation 0-100
 * @param l Output: Lightness 0-100
 */
void rgb_to_hsl(uint32_t rgb, float& h, float& s, float& l);

/**
 * @brief Parse hex color string into RGB value
 *
 * Accepts: #RRGGBB, RRGGBB, #RGB, RGB, 0xRRGGBB, #RRGGBBAA, RRGGBBAA
 * Case-insensitive, trims leading/trailing whitespace.
 *
 * 8-digit input is #RRGGBBAA: the alpha byte is dropped and the RGB returned,
 * because no consumer in this tree composites gcode or filament colors. Slicers
 * emit the 8-digit form, so rejecting it is what pushed callers onto ad-hoc
 * strtol conversions that read the channels off by one byte.
 *
 * Digits accumulate into a uint32_t directly rather than via strtol, so an
 * 8-digit value with RR >= 0x80 does not saturate to LONG_MAX on 32-bit ARM.
 *
 * @param input Input string (null-terminated)
 * @param out_rgb Output: parsed RGB value (0x00RRGGBB format)
 * @return true if valid, false otherwise
 */
bool parse_hex_color(const char* input, uint32_t& out_rgb);

/**
 * @brief Parse hex color string into RGB value (optional-returning overload)
 *
 * Convenience wrapper around parse_hex_color(const char*, uint32_t&) that
 * returns std::optional<uint32_t> instead of using an out-param.
 *
 * Accepts: #RRGGBB, RRGGBB, #RGB, RGB, 0xRRGGBB, #RRGGBBAA, RRGGBBAA
 * Case-insensitive, trims leading/trailing whitespace. 8-digit input drops alpha.
 *
 * @param hex_str Input string
 * @return Parsed RGB value (0x00RRGGBB format), or std::nullopt if invalid
 */
std::optional<uint32_t> parse_hex_color(const std::string& hex_str);

/**
 * @brief Convert RGB value to #RRGGBB hex string
 *
 * @param rgb RGB color value (0x00RRGGBB format)
 * @return Hex string in "#RRGGBB" format (e.g., "#FF0000")
 */
std::string color_to_hex_string(uint32_t rgb);

} // namespace helix
