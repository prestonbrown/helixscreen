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
};

} // namespace gcode
} // namespace helix
