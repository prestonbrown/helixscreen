// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_ghost_sampling.h
 * @brief How many layers the background ghost pass may visit, and at what stride.
 *
 * The ghost is the faded silhouette of the layers above the one on screen. It is
 * built once, on a background thread, by walking the file layer by layer. What a
 * single step of that walk costs depends entirely on which parse path the file
 * took:
 *
 * - **Full load** — the step is `&gcode_->layers[i].segments`, a pointer into a
 *   ParsedGCodeFile that is already resident. Walking every layer of even a huge
 *   file is a memory traversal.
 * - **Streaming** — the step is `get_layer_segments(i)`, a seek and parse against
 *   the file on disk. Walking every layer re-parses the entire file, a second
 *   time, after the layer index just finished parsing it once.
 *
 * The ghost pass predates large files being streamed on the print-detail screen
 * and did not distinguish the two, so moving those files onto streaming silently
 * changed its cost model from a memory walk into a full re-parse. On a K2 Plus
 * (dual-core Cortex-A7, 488MB) that meant one core pinned for minutes on a 133MB
 * file starting the instant the index completed, the layer cache thrashed out
 * from under the visible layer, and a UI that could not keep up with touch.
 *
 * The bound below fixes the *work*, not the fidelity: a streamed ghost visits at
 * most GHOST_MAX_SAMPLED_LAYERS layers however large the file is, spaced evenly
 * so the silhouette still spans the whole model rather than trailing off partway
 * up. Full load is left alone — there, every layer is already free.
 *
 * Kept a pure function in a header, rather than folded into the render thread,
 * for the same reason decide_render_mode() is: the interesting branch is the one
 * that only runs on a streamed file on a device, and it is worth being able to
 * test it without one.
 */

#pragma once

namespace helix {
namespace gcode {

/**
 * @brief Upper bound on layers a *streamed* ghost pass will visit.
 *
 * Sized against what the silhouette needs rather than what the file has. The
 * ghost is drawn washed-out and translucent behind the printed layers, so its
 * job is the outline of the remaining print, not its detail. 128 samples resolve
 * that outline on a 480x800 panel while capping the pass at 128 seek-and-parses
 * whether the file holds 500 layers or 50,000.
 */
inline constexpr int GHOST_MAX_SAMPLED_LAYERS = 128;

/**
 * @brief Milliseconds a *streamed* ghost pass sleeps between layers.
 *
 * The pass runs on a background thread that, on the boards streaming exists
 * for, shares two cores with the UI thread. A layer that costs a seek and a
 * parse is long enough that running the whole pass uninterrupted leaves touch
 * unserviced for its duration. Sleeping briefly between layers hands the core
 * back at a known cadence; against the bounded sample above it adds a quarter
 * of a second to a pass that is no longer the thing users are waiting on.
 */
inline constexpr int GHOST_STREAM_YIELD_MS = 2;

/// Which layers a ghost pass visits: every `step`-th layer, `count` of them.
struct GhostSamplePlan {
    int step = 1;  ///< Stride between visited layers. Always >= 1, never 0.
    int count = 0; ///< How many layers the pass visits. Never negative.
};

/**
 * @brief Decide the ghost pass's layer budget for one file.
 *
 * @param total_layers Layer count from the index/parse. May be 0 before an index
 *                     exists; a negative can only be an upstream bug, and is
 *                     treated as empty rather than propagated into a loop bound.
 * @param streaming    True when layers come from GCodeStreamingController (each
 *                     visit is a disk seek and parse), false for a resident
 *                     ParsedGCodeFile (each visit is a pointer dereference).
 * @return The stride and the number of layers to visit.
 *
 * @note In streaming mode the returned `count` is guaranteed <=
 *       GHOST_MAX_SAMPLED_LAYERS, and `step * (count - 1)` lands within one
 *       stride of the top layer, so the sample spans the model's full height.
 */
constexpr GhostSamplePlan plan_ghost_sampling(int total_layers, bool streaming) {
    if (total_layers <= 0) {
        return {1, 0};
    }

    // Resident parse, or few enough layers that the bound would only cost
    // fidelity: visit all of them.
    if (!streaming || total_layers <= GHOST_MAX_SAMPLED_LAYERS) {
        return {1, total_layers};
    }

    // Round the stride up so the count lands at or under the budget: with
    // step = ceil(total / budget), ceil(total / step) <= budget for every total.
    const int step = (total_layers + GHOST_MAX_SAMPLED_LAYERS - 1) / GHOST_MAX_SAMPLED_LAYERS;
    const int count = (total_layers + step - 1) / step;
    return {step, count};
}

} // namespace gcode
} // namespace helix
