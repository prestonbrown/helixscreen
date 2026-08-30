// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_crash_report_modal.h
 * @brief Modal dialog shown after a crash — offers to send report to developer
 *
 * Delivery priority:
 * 1. Auto-send to CF Worker (crash.helixscreen.org → GitHub issue)
 * 2. QR code (pre-filled GitHub issue URL for phone scanning)
 * 3. File fallback (~/helixscreen/crash_report.txt for SCP)
 */

#include "ui_modal.h"

#include "system/crash_reporter.h"

#include <lvgl.h>

class CrashReportModal : public Modal {
  public:
    CrashReportModal();
    ~CrashReportModal() override;

    // Non-copyable
    CrashReportModal(const CrashReportModal&) = delete;
    CrashReportModal& operator=(const CrashReportModal&) = delete;

    /// Set the crash report data before showing
    void set_report(const CrashReporter::CrashReport& report);

    /// Show the modal on the given parent
    bool show_modal(lv_obj_t* parent);

    /// One-shot form: show on the active screen and hand the instance to
    /// ModalStack, which frees it when its entry goes (#1382).
    static bool show_owned(const CrashReporter::CrashReport& report);

    const char* get_name() const override {
        return "Crash Report";
    }
    const char* component_name() const override {
        return "crash_report_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    // Subjects for XML data binding
    lv_subject_t details_subject_;
    lv_subject_t status_subject_;
    lv_subject_t show_qr_subject_;
    bool subjects_initialized_ = false;

    // Subject string buffers
    char details_buf_[512] = {};
    char status_buf_[256] = {};

    // Crash report data
    CrashReporter::CrashReport report_;

    // Flipped on first Send click so double-taps don't kick off a second
    // bundle upload + report POST. Never reset — after a send we either
    // succeed-and-hide or show the QR fallback (also terminal).
    bool sending_ = false;

    // Subject management
    void init_subjects();
    void deinit_subjects();

    // Callback registration (idempotent)
    static void register_callbacks();
    static bool callbacks_registered_;
    static CrashReportModal* active_instance_;

    // Static event callbacks (dispatch to active instance)
    static void on_send_cb(lv_event_t* e);
    static void on_dismiss_cb(lv_event_t* e);

    // Instance event handlers
    void handle_send();
    void handle_dismiss();

    // Delivery logic — two-phase:
    //   attempt_delivery() kicks off a debug bundle upload (non-blocking).
    //   send_with_bundle() fires when the bundle lands (or fails), passing
    //   the share code through to try_auto_send so the issue body links to it.
    void attempt_delivery();
    void send_with_bundle(const std::string& share_code);
    void show_qr_code(const std::string& url);
};
