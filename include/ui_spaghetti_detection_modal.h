// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "detection_manager.h"

#include <functional>
#include <string>

namespace helix::detection {

/// Present one detection to the user per the response ladder: nothing when
/// Suppressed, a warning toast when WarnOnly, and for PauseAndRespond the
/// pause the source did not do itself plus the response modal.
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
     * @param frame   Decoded camera frame (may be nullptr to omit the preview)
     */
    void set_detection(const std::string& message, lv_draw_buf_t* frame) {
        message_ = message;
        frame_ = frame;
    }

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

    // Static (shared across instances) because lv_xml_register_subject keeps
    // the first registration for a name: per-instance subjects would leave the
    // registry with dangling pointers once a modal is freed.
    static lv_subject_t tune_available_subject_;
    static char message_buf_[256];
    static lv_subject_t message_subject_;
    static bool subjects_initialized_;

    std::string message_;
    lv_draw_buf_t* frame_ = nullptr;
    Action on_resume_, on_abort_, on_tune_, on_disable_;
};
