// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "json_fwd.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <functional>
#include <lvgl.h>
#include <mutex>
#include <string>

namespace helix {

class IMoonrakerClient;
enum class PrintJobState;

/**
 * @brief Manages timelapse recording state and render progress
 *
 * Tracks frame captures during printing and render progress from
 * the Moonraker-Timelapse plugin. Provides subjects for reactive
 * UI updates (frame count, render progress, render status).
 *
 * Events arrive via WebSocket notify_timelapse_event and are
 * dispatched through handle_timelapse_event().
 *
 * @note Thread-safe: handle_timelapse_event() uses helix::ui::queue_update()
 *       for subject updates from WebSocket callbacks.
 */
class TimelapseState {
  public:
    static TimelapseState& instance();

    /**
     * @brief Initialize subjects for XML binding
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Handle a timelapse event from Moonraker
     *
     * Dispatches based on event["action"]:
     * - "newframe": Increments frame count
     * - "render": Updates render progress/status, triggers notifications
     *
     * Thread-safe: Uses helix::ui::queue_update() for subject updates.
     *
     * @param event JSON event from notify_timelapse_event
     */
    void handle_timelapse_event(const nlohmann::json& event);

    /// Route notify_timelapse_event from @p client into handle_timelapse_event().
    /// Idempotent: a repeat call (every discovery) replaces the earlier subscription.
    void attach(IMoonrakerClient& client);

    /// Drop the subscription attach() made. Safe when never attached.
    void detach(IMoonrakerClient& client);

    /**
     * @brief Reset all state (on disconnect or new print)
     *
     * Thread-safe: Uses helix::ui::queue_update() for subject updates.
     */
    void reset();

    /// Render progress as 0-100 percent
    lv_subject_t* get_render_progress_subject() {
        return &timelapse_render_progress_;
    }

    /// Render status: "idle", "rendering", "complete", "error"
    lv_subject_t* get_render_status_subject() {
        return &timelapse_render_status_;
    }

    /// Frame count captured this print
    lv_subject_t* get_frame_count_subject() {
        return &timelapse_frame_count_;
    }

    /// Capture info string (e.g., "benchy.gcode · Mar 10, 14:30")
    lv_subject_t* get_capture_info_subject() {
        return &timelapse_capture_info_;
    }

    /// Last rendered video filename (set on render success)
    std::string get_last_rendered_filename() const {
        std::lock_guard<std::mutex> lock(render_mutex_);
        return last_rendered_filename_;
    }

    /// Callback type for render completion: receives the rendered filename
    using RenderCompleteCallback = std::function<void(const std::string& filename)>;

    /**
     * @brief Register a callback for render completion
     *
     * Thread-safe. The callback is invoked via queue_update() on the UI thread,
     * so it does not need its own synchronization.
     */
    void set_on_render_complete(RenderCompleteCallback callback) {
        std::lock_guard<std::mutex> lock(render_mutex_);
        on_render_complete_ = std::move(callback);
    }

  private:
    TimelapseState() = default;

    // Allow tests to call handle_timelapse_event directly
    friend class TimelapseStateTestAccess;

    SubjectManager subjects_;
    std::atomic<bool> subjects_initialized_{false};

    /// Generation guard for the event/reset handlers that defer their subject
    /// writes to the main thread. Invalidated by `deinit_subjects()`, so a
    /// callback still queued when the subjects go away is dropped instead of
    /// writing into a subject that was deinited underneath it (#1165, #1146).
    /// This class is a process-lifetime singleton, so the exposure is the
    /// deinit/re-init shape rather than a freed `this`. Declared after
    /// `subjects_` so it is destroyed first, ahead of the SubjectManager whose
    /// destructor deinits what those callbacks write.
    AsyncLifetimeGuard async_lifetime_;

    // Subjects
    lv_subject_t timelapse_render_progress_{};
    lv_subject_t timelapse_render_status_{};
    lv_subject_t timelapse_frame_count_{};
    lv_subject_t timelapse_capture_info_{};

    // String buffers for string subjects
    char timelapse_render_status_buf_[32]{};
    char timelapse_capture_info_buf_[256]{};

    // Notification throttling: last progress value that triggered a notification
    std::atomic<int> last_notified_progress_{-1};

    /// Watches the printer's job state so each new print starts its frame
    /// count and capture info over; a render clears them only when one runs
    /// and succeeds. Main thread only, like the two flags below.
    ObserverGuard print_state_obs_;
    bool print_in_progress_ = false;
    /// A print was seen to end. Only then is the next PRINTING a new print:
    /// joining one already running (app start, re-subscription) keeps its frames.
    bool print_ended_ = false;
    void on_print_state_changed(PrintJobState state);

    // Protects last_rendered_filename_ and on_render_complete_ (accessed from
    // both the WebSocket background thread and the UI thread)
    mutable std::mutex render_mutex_;

    // Last successfully rendered filename
    std::string last_rendered_filename_;

    // Optional callback fired on render success (invoked via queue_update)
    RenderCompleteCallback on_render_complete_;
};

} // namespace helix
