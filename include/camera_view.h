// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

#if HELIX_HAS_CAMERA

#include "async_lifetime_guard.h"
#include "camera_stream.h"
#include "json_fwd.h"
#include "moonraker_types.h"

#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>

namespace helix {

/**
 * @brief One live camera feed shown in LVGL images.
 *
 * Owns the CameraStream for a view that displays a webcam: resolves the feed,
 * applies its rotation/flip, decodes at display resolution, hands every frame
 * and status message to the owner on the UI thread, and tears the stream down
 * so no image is left pointing at a freed frame buffer.
 *
 * What to show, where, and at which frame rate stays with the owner: the
 * frame callback picks the target image, and the owner sets the frame rate
 * from its own visibility rules (set_max_fps(0) pauses decoding).
 *
 * Main thread only. Most MJPEG streamers accept a single client, so a view
 * starting a camera stops every other view of it (see on_preempted()).
 */
class CameraView {
  public:
    /// A decoded frame, delivered on the UI thread. Valid until the next frame
    /// or stop(); hand it to lv_image_set_src().
    using FrameFn = std::function<void(lv_draw_buf_t* frame)>;
    /// A stream status message, delivered on the UI thread.
    using StatusFn = std::function<void(const std::string& message)>;

    // Both out of line, so a translation unit holding a CameraView never
    // emits CameraStream's destructor itself.
    CameraView();
    ~CameraView();

    CameraView(const CameraView&) = delete;
    CameraView& operator=(const CameraView&) = delete;

    /// Resolve @p source (a webcam name; "" picks automatically) and start
    /// streaming it with the rotation/flip in @p transform_config (a view's
    /// persisted config; empty applies the camera's own flips). Returns false,
    /// starting nothing, when no feed resolves. A running stream is left alone;
    /// one whose worker gave up is stopped first, clearing @p images, the
    /// images this view shows frames in. Any other view streaming the same
    /// camera is told to stop through its on_preempted() handler.
    bool start(const std::string& source, const nlohmann::json& transform_config,
               std::initializer_list<lv_obj_t*> images, FrameFn on_frame, StatusFn on_status);

    /// Stop the stream. Every image in @p images showing a frame is cleared
    /// first, and the render thread is waited on, so freeing the frame buffers
    /// cannot race a draw. Queued frames and statuses are dropped. The
    /// stream's thread is joined on HttpExecutor::fast() when it runs.
    void stop(std::initializer_list<lv_obj_t*> images);

    /// What to do when another view starts streaming this view's camera: the
    /// owner stops this view (with its images) and restarts it when it is
    /// shown again.
    void on_preempted(std::function<void()> handler);

    /// Join @p stream's thread and free it, on HttpExecutor::fast() when it
    /// runs, else here. A stream whose thread had to be detached is leaked
    /// rather than freed, however the job ends: run, or dropped unrun by a
    /// stopping pool.
    static void reap_off_main_thread(std::unique_ptr<CameraStream> stream);

    /// A stream exists (it may still be connecting or have given up).
    bool has_stream() const {
        return stream_ != nullptr;
    }
    bool is_running() const {
        return stream_ && stream_->is_running();
    }

    /// The feed the running stream shows; its name is "" for the auto-pick.
    const WebcamInfo& feed() const {
        return feed_;
    }
    /// The feed's configured frame rate (15 when it names none).
    int target_fps() const {
        return target_fps_;
    }

    /// Cap decoding at @p fps; 0 pauses. No-op without a stream.
    void set_max_fps(int fps);

    /// Re-apply rotation/flip from @p transform_config to the running stream.
    void apply_transform(const nlohmann::json& transform_config);

    /// The token the running stream's callbacks are guarded by. stop()
    /// expires it; nothing else does.
    LifetimeToken stream_token() const {
        return stream_lifetime_.token();
    }

    /// Call @p on_change with true when the display goes to sleep and false
    /// when it wakes, for as long as this view lives. Registers once; a later
    /// call replaces the handler.
    void on_display_sleep(std::function<void(bool sleeping)> on_change);

  private:
    friend class CameraViewTestAccess;

    /// An owner of @p stream whose last release joins it and frees it, or
    /// leaks it when its thread was detached.
    static std::shared_ptr<CameraStream> reaping_owner(std::unique_ptr<CameraStream> stream);
    /// Streams leaked because their thread was detached.
    static std::atomic<int> leaked_streams_;

    std::unique_ptr<CameraStream> stream_;
    WebcamInfo feed_;
    int target_fps_ = 15;

    /// Guards the frame and status deferrals of one stream; stop() bumps it.
    AsyncLifetimeGuard stream_lifetime_;
    std::shared_ptr<std::function<void(bool)>> sleep_handler_;
    std::function<void()> on_preempted_;
};

} // namespace helix

#endif // HELIX_HAS_CAMERA
