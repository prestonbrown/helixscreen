// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace helix {

/** @brief Toolhead rendering style (Auto=detect from printer type, or force) */
enum class ToolheadStyle : uint8_t {
    AUTO = 0,
    DEFAULT = 1,
    A4T = 2,
    ANTHEAD = 3,
    JABBERWOCKY = 4,
    STEALTHBURNER = 5,
    CREALITY_K1 = 6,
    CREALITY_K2 = 7
};

} // namespace helix
