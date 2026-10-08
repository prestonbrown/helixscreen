// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A downscaled copy of what a camera shows, for UI that wants a still of the
// feed (the spaghetti-detection modal). Everything but the draw-buf conversion
// is plain memory, so a worker thread can build one.

#include "async_lifetime_guard.h"
#include "lvgl.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

/// Tightly packed BGR888 pixels: LVGL's RGB888 byte order.
struct CameraFrame {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> bgr;

    bool empty() const {
        return bgr.empty();
    }
};

/// Nearest-neighbour downscale of a BGR888 image to fit max_w x max_h,
/// aspect preserved, never enlarged. Empty for degenerate input.
CameraFrame downscale_bgr(const uint8_t* src, int w, int h, int stride, int max_w, int max_h);

/// LVGL draw buffer holding a copy of @p f (caller frees with lv_draw_buf_destroy).
/// Main thread only. Null for an empty frame.
lv_draw_buf_t* to_draw_buf(const CameraFrame& f);

/// One snapshot to fetch and how to present it.
struct SnapshotTarget {
    std::string url; ///< empty when no camera is configured
    /// Turns the fetched JPEG into a frame fitting max_w x max_h, rotation and
    /// flips applied. Runs on the worker.
    std::function<CameraFrame(const std::string& jpeg, int max_w, int max_h)> decode;
};

/// Where a still of the camera comes from. Injectable so the choice between
/// them is testable without a camera.
struct CameraFrameSources {
    /// Latest frame of a running stream, empty when none runs. Main thread.
    std::function<CameraFrame(int max_w, int max_h)> stream_frame;
    /// The camera to take a snapshot from. Main thread.
    std::function<SnapshotTarget()> snapshot;
    /// Fetch @p url on a worker and call @p done there with the body (empty on failure).
    std::function<void(const std::string& url, std::function<void(std::string)> done)> fetch;
};

/// The sources backed by the running CameraStreams and the camera widget's
/// configured camera (source, rotation, flips), else the auto-picked one.
CameraFrameSources live_camera_sources();

/// A frame now when a stream is running. Otherwise, with a snapshot URL
/// configured, returns empty and later calls @p on_late_frame on the main
/// thread with the decoded snapshot; the call is dropped if @p token expired
/// or the fetch failed. With no camera, returns empty and never calls back.
CameraFrame acquire_camera_frame(const CameraFrameSources& src, int max_w, int max_h,
                                 LifetimeToken token,
                                 std::function<void(CameraFrame)> on_late_frame);

} // namespace helix
