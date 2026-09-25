// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gcode_layer_renderer.h"

#include <chrono>
#include <thread>

namespace helix {
namespace gcode {

/// Reaches the background ghost pass, which is private because nothing in the
/// app starts it deliberately — render() does, on the first frame after a load.
/// A test that wants to measure what the pass costs has to start it on purpose.
class GCodeLayerRendererTestAccess {
  public:
    /// Run one full ghost pass and block until the worker has finished.
    ///
    /// Waits on the thread's own running flag rather than a timeout: the point
    /// of the measurement is how much work the pass does, so cutting it short
    /// would report a number the pass did not actually stop at.
    static void run_ghost_pass(GCodeLayerRenderer& renderer) {
        renderer.start_background_ghost_render();

        while (renderer.ghost_thread_running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (renderer.ghost_thread_.joinable()) {
            renderer.ghost_thread_.join();
        }
    }

    /// Start one ghost build and block until the worker has fully exited,
    /// WITHOUT consulting the running flag. run_ghost_pass() spins on that
    /// flag, which is fine when the flag is trusted and a hang when it is the
    /// thing under test. Returns false when no worker was spawned at all (no
    /// layers to draw, or the OS refused the thread).
    static bool start_and_join_ghost_build(GCodeLayerRenderer& renderer) {
        renderer.start_background_ghost_render();
        if (!renderer.ghost_thread_.joinable()) {
            return false;
        }
        renderer.ghost_thread_.join();
        return true;
    }

    /// True when the pass ran to completion rather than bailing out early.
    static bool ghost_completed(const GCodeLayerRenderer& renderer) {
        return renderer.ghost_thread_ready_.load();
    }

    /// The raw ARGB8888 ghost buffer the pass filled, for a test that wants to
    /// look at the pixels rather than trust a layer count.
    static const uint8_t* ghost_pixels(const GCodeLayerRenderer& renderer) {
        return renderer.ghost_raw_buffer_.get();
    }
    static int ghost_width(const GCodeLayerRenderer& renderer) {
        return renderer.ghost_raw_width_;
    }
    static int ghost_height(const GCodeLayerRenderer& renderer) {
        return renderer.ghost_raw_height_;
    }
    static size_t ghost_stride(const GCodeLayerRenderer& renderer) {
        return renderer.ghost_raw_stride_;
    }

    /// The per-tool palette every draw path resolves segment colors through.
    /// Private for that reason; a test asking "which color is tool N wearing
    /// now" - after a slicer palette, after AMS overrides, after a retraction -
    /// has no other way to see the answer.
    static const GCodeColorPalette& tool_palette(const GCodeLayerRenderer& renderer) {
        return renderer.tool_palette_;
    }

    /// Drive one solid-cache batch directly. render() only reaches this path
    /// after warmup frames with a live canvas; a budget test wants the batch
    /// in isolation, with layers_per_frame_ pinned high so a wall-clock box
    /// is the only thing that can stop the batch early.
    static int render_solid_batch(GCodeLayerRenderer& renderer, int from_layer, int to_layer,
                                  int width, int height) {
        renderer.ensure_cache(width, height);
        return renderer.render_layers_to_cache(from_layer, to_layer);
    }

    /// Pin the adaptive layer count. render_layers_to_cache reads it per call;
    /// adaptation only runs from render(), which the direct-call tests skip.
    static void pin_layers_per_frame(GCodeLayerRenderer& renderer, int layers) {
        renderer.layers_per_frame_ = layers;
    }

    /// The per-segment draw gate, private because every draw path consults it
    /// internally. A test pins its feature-type filtering here.
    static bool renders_segment(const GCodeLayerRenderer& renderer, const ToolpathSegment& seg) {
        return renderer.should_render_segment(seg);
    }
};

} // namespace gcode
} // namespace helix
