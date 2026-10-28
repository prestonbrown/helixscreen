// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_height_layer_dwell.cpp
 * @brief When a layer derived from commanded Z may be shown.
 *
 * Run with: ./build/bin/helix-tests "[layer_tracking][zhop]"
 *
 * The numbers model a 0.4mm Z-hop over 0.13mm layers: the hop reads three
 * layers above the one printing, for well under a second.
 */

#include "height_layer_dwell.h"

#include "../catch_amalgamated.hpp"

using helix::decide_height_layer;
using helix::HeightLayerDecision;
using helix::HeightLayerPending;
using helix::kHeightLayerDwellSeconds;

TEST_CASE("Height layer dwell: the first derived layer shows at once", "[layer_tracking][zhop]") {
    const HeightLayerDecision d = decide_height_layer(true, 0, 38, {}, 100.0);
    REQUIRE(d.show == 38);
    REQUIRE(d.pending.layer == 0);
}

TEST_CASE("Height layer dwell: a Z-hop shorter than the dwell never shows",
          "[layer_tracking][zhop]") {
    // Hop up: 38 -> 41 is only a candidate.
    HeightLayerDecision d = decide_height_layer(false, 38, 41, {}, 100.0);
    REQUIRE(d.show == 38);
    REQUIRE(d.pending.layer == 41);
    REQUIRE(d.pending.since_s == 100.0);

    // Still up just short of the dwell.
    d = decide_height_layer(false, 38, 41, d.pending, 100.0 + kHeightLayerDwellSeconds - 0.1);
    REQUIRE(d.show == 38);
    REQUIRE(d.pending.layer == 41);

    // Back down onto the layer being printed: the candidate is dropped.
    d = decide_height_layer(false, 38, 38, d.pending, 101.6);
    REQUIRE(d.show == 38);
    REQUIRE(d.pending.layer == 0);

    // The next hop starts its own dwell rather than inheriting the old start.
    d = decide_height_layer(false, 38, 41, d.pending, 103.0);
    REQUIRE(d.show == 38);
    REQUIRE(d.pending.since_s == 103.0);
}

TEST_CASE("Height layer dwell: a real layer change shows once Z holds it",
          "[layer_tracking][zhop]") {
    HeightLayerDecision d = decide_height_layer(false, 38, 39, {}, 200.0);
    REQUIRE(d.show == 38);

    d = decide_height_layer(false, 38, 39, d.pending, 200.0 + kHeightLayerDwellSeconds);
    REQUIRE(d.show == 39);
    REQUIRE(d.pending.layer == 0);
}

TEST_CASE("Height layer dwell: a layer reached through a hop waits from where Z settled",
          "[layer_tracking][zhop]") {
    // Layer change with a hop: up to 41 at t=300, down onto 39 at t=300.8.
    HeightLayerDecision d = decide_height_layer(false, 38, 41, {}, 300.0);
    d = decide_height_layer(false, 38, 39, d.pending, 300.8);
    REQUIRE(d.show == 38);
    REQUIRE(d.pending.layer == 39);
    REQUIRE(d.pending.since_s == 300.8);

    // 1.5s after the hop began but not after Z settled on 39.
    d = decide_height_layer(false, 38, 39, d.pending, 301.6);
    REQUIRE(d.show == 38);

    d = decide_height_layer(false, 38, 39, d.pending, 300.8 + kHeightLayerDwellSeconds);
    REQUIRE(d.show == 39);
}
