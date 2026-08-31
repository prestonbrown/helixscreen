// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include <functional>
#include <string>

/**
 * @file ui_spaghetti_detection_modal.h
 * @brief Response modal for spaghetti / print-issue detection
 *
 * Shows a warning title, an optional camera-frame preview, a message, and
 * three actions:
 *   - Resume (primary / on_ok)
 *   - Abort  (secondary / on_cancel)
 *   - Tune   (tertiary / on_tertiary)
 *
 * Each action invokes a settable callback. Resume and Abort hide the modal;
 * Tune leaves it open so the user can return after tuning.
 *
 * Mirrors the print-cancel / runout-guidance modal pattern: buttons are wired
 * programmatically in on_show() via wire_*_button(), not via XML callbacks.
 */
class SpaghettiDetectionModal : public Modal {
  public:
    using Action = std::function<void()>;

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

  private:
    std::string message_;
    lv_draw_buf_t* frame_ = nullptr;
    Action on_resume_, on_abort_, on_tune_;
};
