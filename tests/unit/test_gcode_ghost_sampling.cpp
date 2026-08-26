// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ghost_sampling.cpp
 * @brief Unit tests for plan_ghost_sampling() — how many layers the background
 *        ghost pass is allowed to visit, and at what stride.
 *
 * The ghost is the faded silhouette of the layers above the one being shown.
 * It is built on a background thread that walks the file layer by layer. In
 * full-load mode each step is a pointer into an already-parsed ParsedGCodeFile,
 * so walking every layer is nearly free. In streaming mode the same step is a
 * seek-and-parse against the file on disk, so walking every layer re-parses the
 * whole file a second time.
 *
 * That is the regression these pin. Large files on the print-detail screen were
 * moved onto streaming, and the ghost pass — unchanged, and unaware of the
 * mode — turned into a second full parse of a 133MB file on a dual-core
 * Cortex-A7, starting the moment the layer index finished. It saturated a core
 * for minutes, evicted the LRU entries the visible layer needed, and left the
 * UI unable to keep up with touch.
 *
 * The fix bounds the work instead of the fidelity: a streamed ghost visits at
 * most GHOST_MAX_SAMPLED_LAYERS layers no matter how big the file is, spaced
 * evenly so the silhouette still spans the whole model. Full-load mode keeps
 * visiting every layer, because there it costs nothing.
 */

#include "gcode_ghost_sampling.h"

#include "../catch_amalgamated.hpp"

using helix::gcode::GHOST_MAX_SAMPLED_LAYERS;
using helix::gcode::plan_ghost_sampling;

TEST_CASE("plan_ghost_sampling: full-load mode still visits every layer",
          "[gcode][ghost][sampling]") {
    // Full load holds the whole parse in memory; a step there is a pointer
    // dereference, so bounding it would cost fidelity and buy nothing.
    for (int total : {1, 50, 128, 1000, 5000, 40000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/false);
        CAPTURE(total);
        CHECK(plan.step == 1);
        CHECK(plan.count == total);
    }
}

TEST_CASE("plan_ghost_sampling: a streamed file small enough is not degraded",
          "[gcode][ghost][sampling]") {
    // Under the budget there is nothing to save, so the ghost stays exact.
    for (int total : {1, 2, 64, GHOST_MAX_SAMPLED_LAYERS}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true);
        CAPTURE(total);
        CHECK(plan.step == 1);
        CHECK(plan.count == total);
    }
}

TEST_CASE("plan_ghost_sampling: a streamed file never exceeds the layer budget",
          "[gcode][ghost][sampling]") {
    // This is the property that actually protects the UI thread: the number of
    // seek-and-parses stops growing with the file. 5000 layers is the shape of
    // the 133MB K2 Plus file this was found on; 200000 is well past anything
    // real, and must still be bounded.
    for (int total : {129, 200, 999, 5000, 20000, 200000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true);
        CAPTURE(total);
        CHECK(plan.step > 1);
        CHECK(plan.count <= GHOST_MAX_SAMPLED_LAYERS);
        CHECK(plan.count > 0);
    }
}

TEST_CASE("plan_ghost_sampling: the budget holds for every size in a dense sweep",
          "[gcode][ghost][sampling]") {
    // The bound is a ceil-of-a-ceil, which is exactly where an off-by-one
    // would hide. Sweep every size across the first few multiples of the
    // budget rather than trusting the round numbers above.
    for (int total = 1; total <= GHOST_MAX_SAMPLED_LAYERS * 8; ++total) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true);
        CAPTURE(total);
        REQUIRE(plan.step >= 1);
        REQUIRE(plan.count > 0);
        REQUIRE(plan.count <= GHOST_MAX_SAMPLED_LAYERS);
    }
}

TEST_CASE("plan_ghost_sampling: sampling spans the full height of the model",
          "[gcode][ghost][sampling]") {
    // A ghost that samples only the bottom of the object is worse than no
    // ghost: it claims the print ends where the sampling stopped. The last
    // visited index must land within one stride of the top layer.
    for (int total : {129, 1000, 5000, 20000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true);
        int last_visited = plan.step * (plan.count - 1);
        CAPTURE(total, plan.step, plan.count, last_visited);
        CHECK(last_visited < total);
        CHECK(last_visited >= total - plan.step);
    }
}

TEST_CASE("plan_ghost_sampling: an empty or nonsense layer count visits nothing",
          "[gcode][ghost][sampling]") {
    // get_layer_count() returns int and can legitimately be 0 before an index
    // is built. A negative can only be a bug upstream, but the loop that
    // consumes this must not be handed a stride of 0 or a negative count.
    for (int total : {0, -1, -5000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true);
        CAPTURE(total);
        CHECK(plan.count == 0);
        CHECK(plan.step >= 1);
    }
    auto full = plan_ghost_sampling(0, /*streaming=*/false);
    CHECK(full.count == 0);
    CHECK(full.step >= 1);
}
