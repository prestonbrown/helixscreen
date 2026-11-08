// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file cold_pull_temps.h
 * @brief The materials a cold pull offers and the soak / pull temperatures each starts at.
 *
 * Header-only and free of LVGL, so the settings layer that persists a user's edits and
 * the flow that runs the pull share one table (prestonbrown/helixscreen#2011).
 */

#pragma once

#include <algorithm>

namespace helix {

enum class ColdPullMaterial : int { Pla = 0, Nylon = 1, Cleaning = 2 };
inline constexpr int kColdPullMaterialCount = 3;

struct ColdPullTemps {
    int soak_c = 0; ///< Melt the old residue loose; at least as hot as what was printed
    int pull_c = 0; ///< Solid enough to hold together, soft enough to let go of the wall
};

inline bool operator==(const ColdPullTemps& a, const ColdPullTemps& b) {
    return a.soak_c == b.soak_c && a.pull_c == b.pull_c;
}

/// The lowest pull temperature an edit may set.
inline constexpr int kMinPullC = 40;
/// The soak stays at least this far above the pull.
inline constexpr int kMinSoakPullGapC = 60;
/// The soak stays at least this far above the extrusion floor, so the feed extrudes.
inline constexpr int kMinSoakAboveFloorC = 20;

/// Cleaning filament's pull temperature. No maker publishes one; the issue's starting
/// value. FBRC8 pulls at 145 °C on the UM2, so 140 is the other candidate.
inline constexpr int kCleaningFilamentPullC = 100;

/// Defaults per material (sources in the cold pull design notes). An unknown value
/// answers PLA's.
[[nodiscard]] inline ColdPullTemps cold_pull_default_temps(ColdPullMaterial m) {
    switch (m) {
    case ColdPullMaterial::Nylon:
        return {250, 140};
    case ColdPullMaterial::Cleaning:
        return {260, kCleaningFilamentPullC};
    case ColdPullMaterial::Pla:
        break;
    }
    return {230, 90};
}

/// Whether @p v names a ColdPullMaterial.
[[nodiscard]] inline bool cold_pull_material_valid(int v) {
    return v >= 0 && v < kColdPullMaterialCount;
}

/**
 * @brief A user's edit, held inside what the printer can do.
 *
 * Soak within [floor_c + kMinSoakAboveFloorC, ceiling_c] (no upper bound when
 * @p ceiling_c is 0), pull within [kMinPullC, soak_c - kMinSoakPullGapC]. Where the
 * ceiling sits below the soak floor, the ceiling wins: Klipper refuses anything above it.
 */
[[nodiscard]] inline ColdPullTemps clamp_cold_pull_temps(ColdPullTemps t, int floor_c,
                                                         int ceiling_c) {
    int soak = std::max(t.soak_c, floor_c + kMinSoakAboveFloorC);
    if (ceiling_c > 0) {
        soak = std::min(soak, ceiling_c);
    }
    const int pull = std::max(kMinPullC, std::min(t.pull_c, soak - kMinSoakPullGapC));
    return {soak, pull};
}

} // namespace helix
