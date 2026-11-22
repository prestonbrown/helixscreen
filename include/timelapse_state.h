// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "core/observer_guard.h"
#include "core/subject.h"
#include "json_fwd.h"
#include "print_lifecycle_state.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <functional>
#include <lvgl.h>
#include <mutex>
#include <string>

namespace helix {

class IMoonrakerClient;
class ITimelapseAPI;
enum class PrintJobState;

namespace timelapse {

/**
 * @brief Says when the print a setting was changed for has ended.
 *
 * Fed every print lifecycle state from the moment the change is made. The
 * lifecycle can still hold the previous job's end when it starts, so an end
 * counts only after this print has been seen running. Answers true once.
 */
class PrintEndLatch {
  public:
    /// True when @p state ends the print this latch was armed for.
    bool ended_by(PrintState state) {
        if (fired_) {
            return false;
        }
        if (job_holds_machine(state)) {
            seen_running_ = true;
            return false;
        }
        if (seen_running_ && (state == PrintState::Complete || state == PrintState::Cancelled ||
                              state == PrintState::Error)) {
            fired_ = true;
            return true;
        }
        return false;
    }

  private:
    bool seen_running_ = false;
    bool fired_ = false;
};

/// The active printer's settings key holding the mode a Hyperlapse-for-this-print
/// switch owes back (helix::pending_restore).
inline constexpr const char* kModeRestoreKey = "timelapse/pending_mode_restore";

/// What to do about a persisted mode restore.
enum class ModeRestoreStep {
    Keep,      ///< Nothing to do now
    FetchMode, ///< Read the plugin's mode, then decide
    Send,      ///< The plugin still reads Hyperlapse: put the owed mode back
    Clear,     ///< The plugin no longer reads Hyperlapse: forget the restore
};

/**
 * @brief Decide a persisted mode restore.
 *
 * Kept while nothing is owed, while this session watches the print's end
 * itself, and while the printer reports a print running or nothing yet. Once
 * it reports none, the plugin's mode decides: still Hyperlapse is put back; any
 * other mode means the restore already happened or the user picked a mode
 * since, and the restore is dropped.
 *
 * @param current_mode The plugin's mode once read; nullptr before.
 */
[[nodiscard]] inline ModeRestoreStep mode_restore_step(bool pending, bool watching_print_end,
                                                       ReportedPrintActivity activity,
                                                       const std::string* current_mode) {
    if (!pending || watching_print_end || activity != ReportedPrintActivity::Idle) {
        return ModeRestoreStep::Keep;
    }
    if (current_mode == nullptr) {
        return ModeRestoreStep::FetchMode;
    }
    return *current_mode == "hyperlapse" ? ModeRestoreStep::Send : ModeRestoreStep::Clear;
}

} // namespace timelapse

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

    /// Whether moonraker-timelapse records in Layer mode ("layermacro"), as last
    /// read at discovery or written from here. False until known. Any thread.
    bool layer_mode() const {
        return layer_mode_.load();
    }
    void set_mode(const std::string& mode) {
        layer_mode_.store(mode == "layermacro");
    }

    /**
     * @brief Record the print that has just started in Hyperlapse mode, then
     *        switch the plugin back to Layer mode when that print ends.
     *
     * The plugin has no per-print mode: this writes its global setting twice.
     * Main thread.
     */
    void record_print_as_hyperlapse();

    /**
     * @brief Settle a mode restore a Hyperlapse switch persisted.
     *
     * Decided by timelapse::mode_restore_step() with @p activity, reading the
     * mode from @p api when it must. Main thread.
     */
    void settle_mode_restore(ITimelapseAPI* api, ReportedPrintActivity activity);

    /// settle_mode_restore() against the connected printer. While it reports a
    /// print running, the restore waits for that print's end. Main thread.
    void settle_mode_restore(ReportedPrintActivity activity);

    /// The user picked a mode: no restore is owed any more. Main thread.
    void forget_mode_restore();

  private:
    TimelapseState() = default;

    /// Settle the restore when the print running now ends; @p already_running
    /// when the printer has reported it running.
    void watch_print_end(bool already_running);

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
    helix::Subject timelapse_render_progress_{};
    helix::Subject timelapse_render_status_{};
    helix::Subject timelapse_frame_count_{};
    helix::Subject timelapse_capture_info_{};

    // String buffers for string subjects
    char timelapse_render_status_buf_[32]{};
    char timelapse_capture_info_buf_[256]{};

    std::atomic<bool> layer_mode_{false};
    timelapse::PrintEndLatch print_end_latch_;
    ObserverGuard print_end_observer_;
    bool watching_print_end_ = false;

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
