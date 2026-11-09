// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "camera_frame.h"
#include "core/observer_guard.h"
#include "detection_manager.h"
#include "subject_managed_panel.h"

#include <functional>
#include <string>

namespace helix::detection {

/// Present one detection to the user per the response ladder: nothing when
/// Suppressed, a warning toast when WarnOnly, and for PauseAndRespond the
/// pause the source did not do itself plus the response modal. A dirty-bed
/// detection skips the ladder: the printer stopped a print it cannot resume,
/// so the bed check modal always shows.
void present_detection(const DetectionEvent& e, DetectionPolicy p);

} // namespace helix::detection

/**
 * @file ui_spaghetti_detection_modal.h
 * @brief Response modal for spaghetti / print-issue detection
 *
 * Shows a warning title, an optional camera-frame preview, a message, and
 * four actions:
 *   - Resume (primary / on_ok)
 *   - Abort  (secondary / on_cancel)
 *   - Tune   (tertiary / on_tertiary)
 *   - Turn off detection (quaternary / on_quaternary)
 *
 * Each action invokes a settable callback. Resume and Abort hide the modal;
 * Tune and Turn off detection leave it open: the print still needs a
 * Resume/Abort decision.
 *
 * Mirrors the print-cancel / runout-guidance modal pattern: buttons are wired
 * programmatically in on_show() via wire_*_button(), not via XML callbacks.
 */
class SpaghettiDetectionModal : public Modal {
  public:
    using Action = std::function<void()>;

    SpaghettiDetectionModal() {
        init_subjects();
    }

    const char* get_name() const override {
        return "Spaghetti Detection";
    }
    const char* component_name() const override {
        return "spaghetti_detection_modal";
    }

    void set_on_resume(Action a) {
        on_resume_ = std::move(a);
    }
    void set_on_abort(Action a) {
        on_abort_ = std::move(a);
    }
    void set_on_tune(Action a) {
        on_tune_ = std::move(a);
    }
    void set_on_disable(Action a) {
        on_disable_ = std::move(a);
    }

    /**
     * @brief Configure the message and optional camera frame
     * @param message Detection message text
     * @param frame   Camera frame to preview (copied; nullptr or empty omits the preview)
     */
    void set_detection(const std::string& message, const helix::CameraFrame* frame = nullptr) {
        message_ = message;
        if (frame)
            attach_frame(*frame);
    }

    /// Show @p frame in the preview, now if the dialog is up, else when it is.
    /// The modal keeps its own pixels; the preview's buffer is freed with the widget.
    void attach_frame(const helix::CameraFrame& frame);

    /// Ask the camera for a still: a running stream's latest frame at once, else
    /// a snapshot that appears when it lands (dropped if the modal closed first).
    void request_camera_frame(const helix::CameraFrameSources& sources);

    // Test hooks (bypass LVGL button events):
    void invoke_resume_for_test() {
        if (on_resume_)
            on_resume_();
        hide();
    }
    void invoke_abort_for_test() {
        if (on_abort_)
            on_abort_();
        hide();
    }
    void invoke_tune_for_test() { // Tune does not hide (mirrors on_tertiary())
        if (on_tune_)
            on_tune_();
    }
    void invoke_disable_for_test() { // Disable does not hide (mirrors on_tune)
        if (on_disable_)
            on_disable_();
    }

  protected:
    void on_show() override;
    void on_ok() override { // Resume
        if (on_resume_)
            on_resume_();
        hide();
    }
    // ESC closes without resuming or aborting: those take a button.
    void on_escape() override {
        hide(ModalCloseReason::EscKey);
    }
    void on_cancel() override { // Abort
        if (on_abort_)
            on_abort_();
        hide();
    }
    void on_tertiary() override { // Tune
        if (on_tune_)
            on_tune_();
    }
    void on_quaternary() override { // Turn off detection: print choice still open
        if (on_disable_)
            on_disable_();
    }

  private:
    static void init_subjects();

    // Shared across instances because the XML registry keeps the first
    // registration for a name; a per-instance subject would dangle once its
    // modal is freed. Torn down through StaticSubjectRegistry.
    static lv_subject_t tune_available_subject_;
    static char message_buf_[256];
    static lv_subject_t message_subject_;
    static SubjectManager subjects_;
    static bool subjects_initialized_;

    std::string message_;
    helix::CameraFrame frame_; // held until on_show() builds the preview's buffer
    void show_preview(lv_obj_t* preview);
    Action on_resume_, on_abort_, on_tune_, on_disable_;
};

namespace helix::detection {

/**
 * @brief The printer's pre-print bed check stopped the print
 *
 * The camera saw something on the bed (often a false alarm) and the firmware
 * stopped a print it cannot resume. Three actions:
 *   - Restart print (primary / on_ok): drop the job and print its file again
 *   - Cancel print (quaternary / on_quaternary). ESC and a backdrop tap close
 *     the modal without deciding anything: cancelling a print takes the button.
 *   - Turn off bed check (tertiary / on_tertiary): leaves the modal open, so
 *     the print still gets its Restart/Cancel decision. The button rides
 *     detection_bed_check_enabled and hides once the check is off.
 *
 * Bound to the paused job it was raised for (bind_to_paused_job()), the modal
 * closes when the printer leaves PAUSED, and Restart/Cancel refuse unless the
 * printer is still paused on that same file: a job cancelled elsewhere and a
 * new one started must never be the one these buttons act on.
 */
class BedCheckModal : public Modal {
  public:
    using Action = std::function<void()>;

    const char* get_name() const override {
        return "Bed Check";
    }
    const char* component_name() const override {
        return "bed_check_modal";
    }

    void set_on_restart(Action a) {
        on_restart_ = std::move(a);
    }
    void set_on_cancel_print(Action a) {
        on_cancel_print_ = std::move(a);
    }
    void set_on_disable_check(Action a) {
        on_disable_check_ = std::move(a);
    }

    /// Tie the modal to the job paused on @p filename (see the class comment).
    void bind_to_paused_job(std::string filename) {
        bound_filename_ = std::move(filename);
        bound_ = true;
    }

    /// Whether the printer is still paused on the bound job. Always true for
    /// an unbound modal.
    bool job_still_paused() const;

  protected:
    void on_show() override;
    void on_hide() override;
    void on_ok() override;
    void on_quaternary() override;
    void on_tertiary() override {
        if (on_disable_check_)
            on_disable_check_();
    }

  private:
    /// Run @p action only while the bound job is still the paused one.
    void act_on_job(const Action& action, const char* what);

    Action on_restart_, on_cancel_print_, on_disable_check_;
    bool bound_ = false;
    std::string bound_filename_;
    ObserverGuard pause_observer_;
};

} // namespace helix::detection
