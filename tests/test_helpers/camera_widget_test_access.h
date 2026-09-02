// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "src/ui/panel_widgets/camera_widget.h"

namespace helix {

/// Test-only accessor for CameraWidget's private stream-lifecycle call counts.
///
/// start_stream()/stop_stream() sit behind their own guards (active_,
/// fullscreen_overlay_) strictly narrower than the on_size_changed()
/// edge-trigger conditions that invoke them, and with no camera configured
/// stream_ always ends up null again after either call returns — so
/// "stream_ non-null" cannot tell a test whether the call happened and
/// no-op'd versus never happened at all. These counters can, without
/// needing a real camera or opening a connection.
///
/// Same friend-TestAccess pattern as tests/test_helpers/application_test_access.h
/// and the other headers here; requires `friend class CameraWidgetTestAccess;`
/// on CameraWidget.
class CameraWidgetTestAccess {
  public:
    static int start_stream_calls(const CameraWidget& w) {
        return w.start_stream_calls_;
    }
    static int stop_stream_calls(const CameraWidget& w) {
        return w.stop_stream_calls_;
    }
    /// True once on_activate() has run. start_stream() is only reachable from
    /// on_size_changed()'s "leaving compact" branch when this is true, so a
    /// test asserting that branch's call count must confirm this first.
    static bool active(const CameraWidget& w) {
        return w.active_;
    }

    /// Cached tile-tree pointers. The teardown-uaf contract is exactly WHICH
    /// raw pointers survive WHICH deletion path (detach vs a raw
    /// lv_obj_delete of the page tree), and that is only observable on the
    /// privates. Read-only.
    static lv_obj_t* camera_image(const CameraWidget& w) {
        return w.camera_image_;
    }
    static lv_obj_t* camera_overlay(const CameraWidget& w) {
        return w.camera_overlay_;
    }
    static lv_obj_t* camera_status(const CameraWidget& w) {
        return w.camera_status_;
    }
    static lv_obj_t* widget_obj(const CameraWidget& w) {
        return w.widget_obj_;
    }

    /// A token off the stream guard, the same way start_stream() takes one for
    /// its frame callback. Lets a test assert the raw-delete hook does NOT
    /// expire it - the camera's deliberate divergence from the other three.
    static helix::LifetimeToken stream_token(const CameraWidget& w) {
        return w.lifetime_.token();
    }
};

} // namespace helix

#endif // HELIX_HAS_CAMERA
