// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ghost_mode.cpp
 * @brief Pins the default GhostRenderMode.
 */

#include "gcode_ghost_mode.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::DEFAULT_GHOST_RENDER_MODE;
using helix::gcode::GhostRenderMode;

TEST_CASE("Ghost rendering defaults to Stipple", "[gcode][ghost][ghost_mode]") {
    CHECK(DEFAULT_GHOST_RENDER_MODE == GhostRenderMode::Stipple);
}
