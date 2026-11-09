// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix {

/** @brief Toolhead rendering style (Auto=detect from printer type, or force) */
enum class ToolheadStyle : int8_t {
    AUTO = 0,
    DEFAULT = 1,
    A4T = 2,
    ANTHEAD = 3,
    JABBERWOCKY = 4,
    STEALTHBURNER = 5,
    CREALITY_K1 = 6,
    CREALITY_K2 = 7,
    DRAGONBURNER = 8,
    MINI_STEALTHBURNER = 9,
    BAMBU = 10,
    ENDER3 = 11,
    XOL = 12,
    SNAPMAKER_U1 = 13,
    FLASHFORGE_AD5 = 14,
    FLASHFORGE_CREATOR5 = 15,
    ELEGOO_CENTAURI = 16,
    ARTILLERY_M1 = 17,
    QIDI = 18,
    SOVOL_SV08 = 19
};

/// The highest ToolheadStyle value, the upper bound of the stored setting.
inline constexpr int kLastToolheadStyle = static_cast<int>(ToolheadStyle::SOVOL_SV08);

} // namespace helix
