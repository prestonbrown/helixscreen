// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ghost_thread_flag.cpp
 * @brief The ghost worker's completion must stay visible to the main thread.
 *
 * ghost_thread_running_ is written from two threads: the spawner sets it, and
 * the worker clears it on every exit path. That is only safe while the
 * spawner's store is ordered BEFORE the thread exists. Claim it afterwards —
 * `t = std::thread(...); running.store(true);` — and std::thread is free to
 * run the whole body first, so the store lands on top of the worker's clear
 * and strands the flag set.
 *
 * Nothing recovers from that within a view. render() gates the respawn on the
 * flag being clear, so the ghost never rebuilds; ui_gcode_viewer keeps
 * "Building preview: N%" up and holds frame_complete false, which is what
 * stops a preview from ever settling. It surfaced first as a one-in-three
 * red on the ghost pass suite, where a stranded flag fails the settle loop.
 *
 * The window is one instruction wide, measured at 0.3-0.5% of spawns on a
 * 32-core host, so a single spawn proves nothing and this repeats it: at 2000
 * spawns a re-broken ordering is caught with ~99.9% probability. Correctly
 * ordered, it cannot fail at any count — the store then happens-before the
 * thread body by std::thread's own guarantee, so the worker's clear is always
 * the last write.
 */

#include "../test_helpers/gcode_layer_renderer_test_access.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"

#include <glm/glm.hpp>

#include "../catch_amalgamated.hpp"

using namespace helix::gcode;

namespace {

/// Small on purpose: every spawn memsets this buffer on the main thread, and
/// the quantity under test is the spawn count, not the pixels.
constexpr int kCanvas = 64;

/// Enough layers that start_background_ghost_render() has work to hand over —
/// it returns without spawning when the layer count is zero.
constexpr int kLayers = 3;

/// Spawns per run. Sized from the measured per-spawn strand rate so a
/// regression shows up essentially every run; see the file comment.
constexpr int kSpawns = 10000;

ParsedGCodeFile make_small_tower() {
    ParsedGCodeFile gcode;
    const int16_t obj = gcode.intern_object_name("box");

    for (int i = 0; i < kLayers; ++i) {
        Layer layer;
        const float z = 0.2f * static_cast<float>(i + 1);
        layer.z_height = z;

        const glm::vec3 c[4] = {
            {20.0f, 20.0f, z}, {40.0f, 20.0f, z}, {40.0f, 40.0f, z}, {20.0f, 40.0f, z}};
        for (int k = 0; k < 4; ++k) {
            ToolpathSegment seg;
            seg.start = c[k];
            seg.end = c[(k + 1) % 4];
            seg.is_extrusion = true;
            seg.object_name_index = obj;
            layer.segments.push_back(seg);
            layer.bounding_box.expand(seg.start);
            layer.bounding_box.expand(seg.end);
        }
        layer.segment_count_extrusion = 4;
        gcode.layers.push_back(std::move(layer));
        gcode.total_segments += 4;
    }

    gcode.global_bounding_box.expand(glm::vec3(20.0f, 20.0f, 0.2f));
    gcode.global_bounding_box.expand(glm::vec3(40.0f, 40.0f, 0.2f * static_cast<float>(kLayers)));
    return gcode;
}

} // namespace

TEST_CASE("a finished ghost build never strands the running flag", "[gcode][ghost][threading]") {
    ParsedGCodeFile gcode = make_small_tower();

    GCodeLayerRenderer renderer;
    renderer.set_canvas_size(kCanvas, kCanvas);
    renderer.set_gcode(&gcode);
    renderer.set_view_mode(GCodeLayerRenderer::ViewMode::FRONT);
    renderer.set_ghost_mode(true);

    int spawned = 0;
    int stranded = 0;
    int first_strand = -1;

    for (int i = 0; i < kSpawns; ++i) {
        if (!GCodeLayerRendererTestAccess::start_and_join_ghost_build(renderer)) {
            continue;
        }
        ++spawned;
        // The worker has exited and its writes are visible through the join, so
        // a flag still set here can only be a claim that landed after it.
        if (renderer.is_ghost_build_running()) {
            ++stranded;
            if (first_strand < 0) {
                first_strand = i;
            }
        }
    }

    INFO("spawned=" << spawned << " of " << kSpawns << ", stranded=" << stranded
                    << ", first at spawn " << first_strand);

    // A run that never spawned would report zero strands for the wrong reason.
    REQUIRE(spawned == kSpawns);
    CHECK(stranded == 0);
}
