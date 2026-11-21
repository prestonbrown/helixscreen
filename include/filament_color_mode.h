// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

/**
 * @file filament_color_mode.h
 * @brief How a multi-color filament's colors run, in the lane spec's words.
 *
 * The lane record names it `color_mode` (prestonbrown/lane-status-spec,
 * REFERENCE.md): "solid" for one color, "gradient" when the color changes
 * along the filament's length, "coaxial" when the colors run side by side the
 * whole length (dual-color silk). Spoolman spells the two multi-color kinds
 * `multi_color_direction` "longitudinal" and "coaxial".
 *
 * A slot holds the mode as one of these strings next to its colors; empty
 * reads as solid, so a slot that never heard of modes stays one color.
 */
namespace helix::color_mode {

inline constexpr std::string_view kSolid = "solid";
inline constexpr std::string_view kGradient = "gradient";
inline constexpr std::string_view kCoaxial = "coaxial";

/// "gradient" or "coaxial" for a recognized multi-color mode, else empty
/// (solid). Accepts the spec's names and Spoolman's, case-insensitively.
std::string normalize(std::string_view mode);

/// The Spoolman `multi_color_direction` for a mode: "longitudinal" for
/// gradient, "coaxial" for coaxial, empty for solid.
std::string to_spoolman_direction(std::string_view mode);

/// The spec name to publish: "gradient", "coaxial", or "solid".
std::string_view to_spec(std::string_view mode);

} // namespace helix::color_mode
