// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ghost_mode.cpp
 * @brief Pins GhostRenderMode to a single definition shared by both renderers.
 *
 * The enum used to be declared twice, identically, in gcode_renderer.h and
 * gcode_gles_renderer.h. Two definitions of one type in one namespace is an ODR
 * violation that nothing tripped over only because the GLES header is wrapped in
 * `#ifdef ENABLE_GLES_3D` and the two renderers ship to disjoint platforms.
 *
 * This file includes BOTH renderer headers plus the shared one. On any build
 * where ENABLE_GLES_3D is set - the default on Linux, so ordinary dev and CI
 * builds - that puts both former definitions in a single translation unit, which
 * is precisely the combination that could not compile before. Restoring a local
 * `enum class GhostRenderMode` to either header fails this file with a
 * redefinition error rather than waiting to bite on some future include.
 *
 * On a non-GLES build (macOS, and the constrained printer targets) the GLES
 * header preprocesses to nothing and the case degrades to covering
 * gcode_renderer.h alone. It still compiles and still passes; it just proves
 * less there.
 *
 * The wire values are pinned separately: ui_gcode_viewer_set_ghost_mode(
 * lv_obj_t*, int) is a C-style boundary hard-coding 0=Dimmed and 1=Stipple, so
 * renumbering the enum silently changes what that API means without touching a
 * line of its own code.
 */

#include "gcode_ghost_mode.h"
#include "gcode_gles_renderer.h"
#include "gcode_renderer.h"

#include <type_traits>

#include "../catch_amalgamated.hpp"

using helix::gcode::DEFAULT_GHOST_RENDER_MODE;
using helix::gcode::GhostRenderMode;

TEST_CASE("GhostRenderMode is one shared type, not a per-renderer copy",
          "[gcode][ghost][ghost_mode]") {
    // Reaching the enum through the shared header while gcode_renderer.h is also
    // in the TU is the assertion; a resurrected local copy breaks the build here
    // rather than at some far-off GLES link.
    STATIC_REQUIRE(std::is_enum_v<GhostRenderMode>);
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<GhostRenderMode>, uint8_t>);
}

TEST_CASE("GhostRenderMode wire values are pinned", "[gcode][ghost][ghost_mode]") {
    // ui_gcode_viewer_set_ghost_mode() maps a raw int onto these; renumbering
    // them re-points that API at the wrong mode.
    CHECK(static_cast<uint8_t>(GhostRenderMode::Dimmed) == 0);
    CHECK(static_cast<uint8_t>(GhostRenderMode::Stipple) == 1);
}

TEST_CASE("Ghost rendering defaults to Stipple", "[gcode][ghost][ghost_mode]") {
    // Both renderers previously defaulted through their own copy of this
    // constant; only the GLES header actually declared one, so the default was
    // unstated for the CPU wireframe path.
    CHECK(DEFAULT_GHOST_RENDER_MODE == GhostRenderMode::Stipple);
}
