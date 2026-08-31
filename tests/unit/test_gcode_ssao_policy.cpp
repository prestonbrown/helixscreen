// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ssao_policy.cpp
 * @brief Tests for the viewer's enhanced-shading (SSAO) tiering decision.
 *
 * These call the REAL helix::gcode_viewer::decide_ssao_enabled() used by the
 * viewer constructor. The SSAO cache is a third full-canvas ARGB8888 buffer
 * held for the life of the viewer (~592KB at 368x402, more at larger sizes) —
 * on a 47MB AD5M that is the difference between rendering and OOM, so the
 * constrained tier must stay off unless explicitly forced.
 */

#include "gcode_ssao_policy.h"

#include "../catch_amalgamated.hpp"

using helix::gcode_viewer::decide_antialias_enabled;
using helix::gcode_viewer::decide_ssao_enabled;
using helix::gcode_viewer::SsaoReason;

TEST_CASE("SSAO: on by default when memory is not constrained", "[gcode_viewer][ssao]") {
    auto d = decide_ssao_enabled(/*constrained*/ false, /*env*/ nullptr);
    CHECK(d.enabled);
    CHECK(d.reason == SsaoReason::DefaultOn);
}

TEST_CASE("SSAO: a constrained device keeps the outline and loses antialiasing",
          "[gcode_viewer][ssao]") {
    // AD5M / K1C tier (< 256MB total RAM). This used to return enabled=false.
    //
    // It was off because the outline pass cost a full-canvas ARGB8888 buffer
    // those devices could not spare. That buffer is gone - the pass records only
    // the pixels it darkens, ~6KB against 427KB - and what is left measures
    // about 2ms per cache revalidation on a real AD5M.
    //
    // The expensive half was never the outline. It was that the same flag also
    // turned on antialiased rasterization, at roughly 6x the aliased cost, which
    // is what made a preview take about 2.2x as long to appear.
    auto d = decide_ssao_enabled(/*constrained*/ true, /*env*/ nullptr);
    CHECK(d.enabled);
    CHECK(d.reason == SsaoReason::ConstrainedReduced);

    CHECK_FALSE(decide_antialias_enabled(/*constrained*/ true, /*env*/ nullptr));
    CHECK(decide_antialias_enabled(/*constrained*/ false, /*env*/ nullptr));
}

TEST_CASE("SSAO: HELIX_SSAO=1 forces it on over the constrained tier", "[gcode_viewer][ssao]") {
    // The override exists so the effect can be compared on the small devices.
    auto d = decide_ssao_enabled(/*constrained*/ true, "1");
    CHECK(d.enabled);
    CHECK(d.reason == SsaoReason::EnvForcedOn);
}

TEST_CASE("SSAO: HELIX_SSAO=0 forces it off on a roomy device", "[gcode_viewer][ssao]") {
    auto d = decide_ssao_enabled(/*constrained*/ false, "0");
    CHECK_FALSE(d.enabled);
    CHECK(d.reason == SsaoReason::EnvForcedOff);
}

TEST_CASE("SSAO: env overrides are redundant when they agree with the tier",
          "[gcode_viewer][ssao]") {
    // Same result, but the reason must report the override so the startup log
    // says why rather than implying the tier decided.
    auto on = decide_ssao_enabled(/*constrained*/ false, "1");
    CHECK(on.enabled);
    CHECK(on.reason == SsaoReason::EnvForcedOn);

    auto off = decide_ssao_enabled(/*constrained*/ true, "0");
    CHECK_FALSE(off.enabled);
    CHECK(off.reason == SsaoReason::EnvForcedOff);
}

TEST_CASE("SSAO: unrecognized env values fall through to the tier", "[gcode_viewer][ssao]") {
    // Only the exact strings "0" and "1" are honored — anything else (including
    // empty, "true", "yes") must not silently turn shading on for a 47MB board.
    for (const char* v : {"", "true", "yes", "on", "2", "01"}) {
        CAPTURE(v);
        auto constrained = decide_ssao_enabled(true, v);
        CHECK(constrained.enabled);
        CHECK(constrained.reason == SsaoReason::ConstrainedReduced);
        // The point of the fall-through: an unrecognized value must not buy a
        // 47MB board the expensive half.
        CHECK_FALSE(decide_antialias_enabled(true, v));

        auto roomy = decide_ssao_enabled(false, v);
        CHECK(roomy.enabled);
        CHECK(roomy.reason == SsaoReason::DefaultOn);
    }
}

// ---------------------------------------------------------------------------
// decide_antialias_enabled(): the expensive half, split out so a constrained
// device is not forced to choose between "looks flat" and "builds slowly".
// ---------------------------------------------------------------------------

TEST_CASE("antialiasing follows HELIX_SSAO in both directions", "[gcode_viewer][ssao]") {
    // The override means ALL of enhanced shading, so comparisons stay honest.
    CHECK(decide_antialias_enabled(/*constrained*/ true, "1"));
    CHECK_FALSE(decide_antialias_enabled(/*constrained*/ false, "0"));
}

TEST_CASE("the two halves can disagree, which is the whole point", "[gcode_viewer][ssao]") {
    // On a constrained device with no override: outline yes, antialiasing no.
    // If these ever agree again for that case, the split has been undone and the
    // device is back to paying 6x rasterization for it.
    const auto outline = decide_ssao_enabled(true, nullptr);
    const bool aa = decide_antialias_enabled(true, nullptr);
    CHECK(outline.enabled != aa);
    CHECK(outline.enabled);
    CHECK_FALSE(aa);
}
