// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

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

/// The highest ToolheadStyle value, the upper bound of the stored setting. The settings
/// dropdown lists every style in enum order, so a style's value is its dropdown index.
inline constexpr int kLastToolheadStyle = static_cast<int>(ToolheadStyle::SOVOL_SV08);

/// Maps a printer-database toolhead_style string to its style; AUTO means "no opinion".
inline ToolheadStyle toolhead_style_from_name(std::string_view name) {
    struct Entry {
        std::string_view name;
        ToolheadStyle style;
    };
    static constexpr Entry kNames[] = {
        {"creality_k1", ToolheadStyle::CREALITY_K1},
        {"creality_k2", ToolheadStyle::CREALITY_K2},
        {"anthead", ToolheadStyle::ANTHEAD},
        {"snapmaker_u1", ToolheadStyle::SNAPMAKER_U1},
        {"flashforge_ad5", ToolheadStyle::FLASHFORGE_AD5},
        {"flashforge_creator5", ToolheadStyle::FLASHFORGE_CREATOR5},
        {"elegoo_centauri", ToolheadStyle::ELEGOO_CENTAURI},
        {"artillery_m1", ToolheadStyle::ARTILLERY_M1},
        {"qidi", ToolheadStyle::QIDI},
        {"sovol_sv08", ToolheadStyle::SOVOL_SV08},
        {"ender3", ToolheadStyle::ENDER3},
        {"default", ToolheadStyle::DEFAULT},
    };
    for (const Entry& entry : kNames) {
        if (entry.name == name)
            return entry.style;
    }
    return ToolheadStyle::AUTO;
}

} // namespace helix
