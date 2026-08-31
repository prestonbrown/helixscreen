// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstring>

namespace helix::gcode_viewer {

/// Why enhanced shading (SSAO) ended up on or off. Drives the startup log line.
enum class SsaoReason {
    DefaultOn,          ///< Unconstrained device, no override
    ConstrainedReduced, ///< Constrained tier: outline kept, antialiasing dropped
    EnvForcedOn,        ///< HELIX_SSAO=1
    EnvForcedOff,       ///< HELIX_SSAO=0
};

/// Outcome of the SSAO tiering decision.
struct SsaoDecision {
    bool enabled;
    SsaoReason reason;
};

/**
 * @brief Whether the 2D renderer should antialias its strokes.
 *
 * Split out from `enabled` because one flag was doing two jobs with wildly
 * different price tags. Enhanced shading means both the silhouette outline pass
 * and Aa::On for every extrusion stroke, and those cost:
 *
 *   - outline pass:  ~2 ms per cache revalidation, measured on an AD5M
 *   - antialiasing:  ~6x the aliased rasterization cost, measured by
 *                    tests/unit/test_gcode_raster_bench.cpp
 *
 * The second is what makes a preview take about 2.2x as long to appear on a
 * constrained device; the first is nearly free and is where most of the visible
 * quality comes from. Tying them together forced an all-or-nothing choice
 * between "looks flat" and "builds slowly".
 *
 * So a constrained device keeps the outline and loses the antialiasing.
 * HELIX_SSAO still governs both, because forcing shading on or off for
 * comparison should mean all of it.
 */
inline bool decide_antialias_enabled(bool constrained_device, const char* ssao_env) {
    if (ssao_env && std::strcmp(ssao_env, "0") == 0) {
        return false;
    }
    if (ssao_env && std::strcmp(ssao_env, "1") == 0) {
        return true;
    }
    return !constrained_device;
}

/**
 * @brief Decide whether the viewer starts with enhanced shading enabled.
 *
 * Pure function. Enhanced shading is ON by default but OFF on constrained
 * devices: the SSAO cache is a full-canvas ARGB8888 buffer (~592KB at 368x402,
 * more at larger sizes) held for the life of the viewer, and it is the third
 * such buffer alongside the solid cache and the ghost buffer. On a 47MB AD5M
 * that is real money for a shading nicety.
 *
 * HELIX_SSAO overrides the tier in either direction so the effect can still be
 * forced on for comparison, or off on a machine that would otherwise get it.
 * Any other value of the variable (including empty) is ignored — only the exact
 * strings "0" and "1" are honored.
 *
 * @param constrained_device MemoryInfo::is_constrained_device() for this host.
 * @param ssao_env           Raw HELIX_SSAO value, or nullptr when unset.
 */
inline SsaoDecision decide_ssao_enabled(bool constrained_device, const char* ssao_env) {
    if (ssao_env && std::strcmp(ssao_env, "0") == 0) {
        return SsaoDecision{false, SsaoReason::EnvForcedOff};
    }
    if (ssao_env && std::strcmp(ssao_env, "1") == 0) {
        return SsaoDecision{true, SsaoReason::EnvForcedOn};
    }
    if (constrained_device) {
        // The outline pass STAYS ON here, which reverses the previous answer.
        //
        // It was switched off for these devices because it cost a full-canvas
        // ARGB8888 buffer they could not spare. That buffer is gone: the pass
        // records only the pixels it darkens, about 6KB against the old 427KB.
        // What remains is roughly 2ms per cache revalidation, measured on a real
        // AD5M, which buys most of the visible quality.
        //
        // The expensive half - antialiasing every stroke, about 6x the aliased
        // cost - is what decide_antialias_enabled() drops instead. That is where
        // the "preview takes 2.2x longer to appear" came from, and none of the
        // look depends on it nearly as much.
        return SsaoDecision{true, SsaoReason::ConstrainedReduced};
    }
    return SsaoDecision{true, SsaoReason::DefaultOn};
}

} // namespace helix::gcode_viewer
