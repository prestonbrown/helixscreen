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
 * The ghost bounds the work instead of the fidelity: a streamed ghost visits at
 * most one layer per row of the ghost buffer, spaced evenly so the silhouette
 * still spans the whole model. The default view is FRONT, where a layer's Z is
 * its screen row, so a budget below the row count would show as stripes and one
 * above it cannot resolve at all - the canvas, not the file, is what bounds
 * useful work. Full-load mode keeps visiting every layer, because there it
 * costs nothing.
 */

#include "gcode_ghost_sampling.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

using helix::gcode::ghost_sample_budget;
using helix::gcode::kGhostMaxSampledLayers;
using helix::gcode::kGhostMinSampledLayers;
using helix::gcode::plan_ghost_sampling;

/// A representative ghost buffer height for the 480x800 panels this runs on.
static constexpr int kRows = 320;

TEST_CASE("plan_ghost_sampling: full-load mode still visits every layer",
          "[gcode][ghost][sampling]") {
    // Full load holds the whole parse in memory; a step there is a pointer
    // dereference, so bounding it would cost fidelity and buy nothing.
    for (int total : {1, 50, 128, 1000, 5000, 40000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/false, kRows);
        CAPTURE(total);
        CHECK(plan.step == 1);
        CHECK(plan.count == total);
    }
}

TEST_CASE("plan_ghost_sampling: a streamed file small enough is not degraded",
          "[gcode][ghost][sampling]") {
    // Under the budget there is nothing to save, so the ghost stays exact.
    for (int total : {1, 2, 64, kRows}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true, kRows);
        CAPTURE(total);
        CHECK(plan.step == 1);
        CHECK(plan.count == total);
    }
}

TEST_CASE("plan_ghost_sampling: a streamed file's cost stops growing with the file",
          "[gcode][ghost][sampling]") {
    // This is the property that protects the UI thread: the number of
    // seek-and-parses is set by the canvas, not the file. 5000 layers is the
    // shape of the 133MB K2 Plus file this was found on; 200000 is well past
    // anything real and must still be bounded.
    for (int total : {999, 5000, 20000, 200000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true, kRows);
        CAPTURE(total);
        CHECK(plan.step > 1);
        CHECK(plan.count > 0);
        CHECK(plan.count < ghost_sample_budget(kRows) * 2);
    }
}

TEST_CASE("plan_ghost_sampling: never fewer samples than the canvas has rows",
          "[gcode][ghost][sampling]") {
    // The regression this pins is a stride that rounds the wrong way. Rounding
    // the stride UP keeps the count under the budget, which reads as the safe
    // direction and is not: 321 layers against a 320-row budget gives stride 2
    // and 161 samples, so a model whose projection fills the canvas has twice
    // as many rows as samples and renders as stripes. Every row the model
    // occupies must be able to get ink.
    for (int total = 1; total <= kRows * 8; ++total) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true, kRows);
        CAPTURE(total);
        REQUIRE(plan.count >= std::min(total, ghost_sample_budget(kRows)));
    }
    // The boundary the ceil version got wrong, called out by name.
    CHECK(plan_ghost_sampling(kRows + 1, true, kRows).count == kRows + 1);
}

TEST_CASE("plan_ghost_sampling: the budget holds for every size in a dense sweep",
          "[gcode][ghost][sampling]") {
    // The bound is a ceil-of-a-ceil, which is exactly where an off-by-one
    // would hide. Sweep every size across the first few multiples of the
    // budget rather than trusting the round numbers above.
    for (int total = 1; total <= kRows * 8; ++total) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true, kRows);
        CAPTURE(total);
        REQUIRE(plan.step >= 1);
        REQUIRE(plan.count > 0);
        REQUIRE(plan.count <= total);
        REQUIRE(plan.count < ghost_sample_budget(kRows) * 2);
    }
}

TEST_CASE("plan_ghost_sampling: sampling spans the full height of the model",
          "[gcode][ghost][sampling]") {
    // A ghost that samples only the bottom of the object is worse than no
    // ghost: it claims the print ends where the sampling stopped. The last
    // visited index must land within one stride of the top layer.
    for (int total : {kRows + 1, 1000, 5000, 20000}) {
        auto plan = plan_ghost_sampling(total, /*streaming=*/true, kRows);
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
        auto plan = plan_ghost_sampling(total, /*streaming=*/true, kRows);
        CAPTURE(total);
        CHECK(plan.count == 0);
        CHECK(plan.step >= 1);
    }
    auto full = plan_ghost_sampling(0, /*streaming=*/false, kRows);
    CHECK(full.count == 0);
    CHECK(full.step >= 1);
}

TEST_CASE("ghost_sample_budget: the canvas sets the budget, clamped at both ends",
          "[gcode][ghost][sampling]") {
    // Between the clamps the budget is the row count itself: one sample per row
    // is exactly enough to draw without gaps and more cannot be distinguished.
    CHECK(ghost_sample_budget(320) == 320);
    CHECK(ghost_sample_budget(kGhostMinSampledLayers) == kGhostMinSampledLayers);
    CHECK(ghost_sample_budget(kGhostMaxSampledLayers) == kGhostMaxSampledLayers);

    // A canvas not sized yet must not produce a budget of zero and a stride
    // that divides by it.
    CHECK(ghost_sample_budget(0) == kGhostMinSampledLayers);
    CHECK(ghost_sample_budget(-1) == kGhostMinSampledLayers);
    CHECK(ghost_sample_budget(1) == kGhostMinSampledLayers);

    // And an unusually tall one must not let the pass grow without limit.
    CHECK(ghost_sample_budget(4000) == kGhostMaxSampledLayers);
}

TEST_CASE("plan_ghost_sampling: a taller canvas earns a denser sample",
          "[gcode][ghost][sampling]") {
    // The point of deriving the budget from the canvas: the same file sampled
    // for a taller ghost buffer visits more layers, because more of them land
    // on distinct rows. A fixed constant could not express this.
    const auto shortc = plan_ghost_sampling(20000, /*streaming=*/true, 100);
    const auto tall = plan_ghost_sampling(20000, /*streaming=*/true, 480);
    CHECK(tall.count > shortc.count);
    CHECK(shortc.count >= ghost_sample_budget(100));
    CHECK(tall.count >= ghost_sample_budget(480));
}

TEST_CASE("plan_ghost_sampling: an unsized canvas still yields a usable stride",
          "[gcode][ghost][sampling]") {
    // start_background_ghost_render() allocates the buffer before launching the
    // thread, but a zero here must degrade to the floor rather than divide by 0.
    const auto plan = plan_ghost_sampling(5000, /*streaming=*/true, 0);
    CHECK(plan.step >= 1);
    CHECK(plan.count >= kGhostMinSampledLayers);
    CHECK(plan.count < kGhostMinSampledLayers * 2);
}

// ---------------------------------------------------------------------------
// The model's own Z extent, not the canvas, bounds useful work in FRONT view.
// A layer moves dz * cos(45deg) * scale rows up the screen, so a flat plate
// spans a few dozen rows however tall the canvas is, and every layer between
// two samples one row apart is painted over by the next.
// ---------------------------------------------------------------------------

using helix::gcode::front_view_z_rows;
using helix::gcode::ghost_model_budget;
using helix::gcode::ghost_samples_per_z_row;
using helix::gcode::kGhostMinModelSamples;
using helix::gcode::kWeakIndexRateMbPerCpuS;
using helix::gcode::plan_ghost_sampling_budget;

TEST_CASE("front_view_z_rows: a layer's Z rises cos(45deg) * scale rows",
          "[gcode][ghost][sampling]") {
    CHECK(front_view_z_rows(100.0f, 1.0f) == Catch::Approx(70.71f).margin(0.01f));
    CHECK(front_view_z_rows(33.0f, 0.94f) == Catch::Approx(21.93f).margin(0.01f));
    CHECK(front_view_z_rows(0.0f, 2.0f) == 0.0f);
    CHECK(front_view_z_rows(-5.0f, 2.0f) == 0.0f);
}

TEST_CASE("ghost_model_budget: samples per row the model's Z spans", "[gcode][ghost][sampling]") {
    // A 33mm plate on a CC1-sized canvas: ~22 rows, where the canvas rule alone
    // would allow 240.
    CHECK(ghost_model_budget(240, 21.93f, 2) == 44);
    CHECK(ghost_model_budget(240, 21.93f, 1) == 22);
    CHECK(ghost_model_budget(240, 21.93f, 0) == 22);
}

TEST_CASE("ghost_samples_per_z_row: a CPU-poor board takes one sample a row",
          "[gcode][ghost][sampling]") {
    CHECK(ghost_samples_per_z_row(10.0) == 1); // CC1 / K2 class
    CHECK(ghost_samples_per_z_row(0.0) == 1);  // not measured
    CHECK(ghost_samples_per_z_row(kWeakIndexRateMbPerCpuS - 0.1) == 1);
    CHECK(ghost_samples_per_z_row(kWeakIndexRateMbPerCpuS) == 2);
    CHECK(ghost_samples_per_z_row(280.0) == 2); // desktop
}

TEST_CASE("ghost_model_budget: never more than the canvas rule allows",
          "[gcode][ghost][sampling]") {
    // A tall model fills the canvas; the canvas budget stays the ceiling.
    CHECK(ghost_model_budget(240, 400.0f, 2) == ghost_sample_budget(240));
    CHECK(ghost_model_budget(4000, 4000.0f, 2) == kGhostMaxSampledLayers);
}

TEST_CASE("ghost_model_budget: a sliver of a model still gets a usable silhouette",
          "[gcode][ghost][sampling]") {
    CHECK(ghost_model_budget(240, 0.5f, 2) == kGhostMinModelSamples);
    CHECK(kGhostMinModelSamples < kGhostMinSampledLayers);
}

TEST_CASE("ghost_model_budget: an unknown Z extent falls back to the canvas rule",
          "[gcode][ghost][sampling]") {
    CHECK(ghost_model_budget(240, 0.0f, 2) == ghost_sample_budget(240));
    CHECK(ghost_model_budget(240, -1.0f, 2) == ghost_sample_budget(240));
    CHECK(ghost_model_budget(0, 0.0f, 2) == kGhostMinSampledLayers);
}

TEST_CASE("plan_ghost_sampling_budget: a flat plate visits a third of its layers",
          "[gcode][ghost][sampling]") {
    // The 37.5MB, 165-layer multiboard plate: the canvas rule visits all 165.
    const auto canvas = plan_ghost_sampling(165, true, 240);
    CHECK(canvas.count == 165);

    const auto model = plan_ghost_sampling_budget(165, true, ghost_model_budget(240, 21.93f, 2));
    CHECK(model.step == 3);
    CHECK(model.count == 55);
    // Still spans the model's full height.
    CHECK(model.step * (model.count - 1) >= 165 - model.step);
}

TEST_CASE("plan_ghost_sampling is plan_ghost_sampling_budget over the canvas budget",
          "[gcode][ghost][sampling]") {
    for (int total : {0, 1, 63, 64, 65, 321, 5000}) {
        for (int rows : {0, 100, 320, 4000}) {
            CAPTURE(total, rows);
            const auto a = plan_ghost_sampling(total, true, rows);
            const auto b = plan_ghost_sampling_budget(total, true, ghost_sample_budget(rows));
            CHECK(a.step == b.step);
            CHECK(a.count == b.count);
        }
    }
    // Full load ignores any budget.
    CHECK(plan_ghost_sampling_budget(5000, false, 16).count == 5000);
}
