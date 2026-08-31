// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file ui_debug_bundle_modal.h
 * @brief Modal dialog for uploading diagnostic debug bundles to support
 *
 * 4-state modal: consent -> uploading -> success/error
 * Uses integer subject state machine with XML bind_flag_if_not_eq visibility.
 */

#include "ui_modal.h"

#include <lvgl.h>

class DebugBundleModal : public Modal {
  public:
    DebugBundleModal();
    ~DebugBundleModal() override;

    // Non-copyable
    DebugBundleModal(const DebugBundleModal&) = delete;
    DebugBundleModal& operator=(const DebugBundleModal&) = delete;

    /// Show the modal and return success
    bool show_modal(lv_obj_t* parent);

    /// One-shot owned show: create, show on the active screen, and hand the
    /// instance to ModalStack, which frees it when its entry goes (#1382).
    /// Returns false if the XML failed to create; the instance is not leaked.
    static bool show_owned();

    const char* get_name() const override {
        return "Debug Bundle";
    }
    const char* component_name() const override {
        return "debug_bundle_modal";
    }

  protected:
    void on_show() override;
    void on_hide() override;

  private:
    // Subject state machine: 0=consent, 1=uploading, 2=success, 3=error
    lv_subject_t state_subject_;
    lv_subject_t status_subject_;
    lv_subject_t share_code_subject_;
    lv_subject_t error_subject_;
    lv_subject_t include_logs_subject_;
    bool subjects_initialized_ = false;

    // Subject string buffers
    char status_buf_[256] = {};
    char share_code_buf_[32] = {};
    char error_buf_[256] = {};

    // Subject management
    void init_subjects();
    void deinit_subjects();

    // Callback registration (idempotent)
    static void register_callbacks();
    static bool callbacks_registered_;
    static DebugBundleModal* active_instance_;

    // Static event callbacks (dispatch to active instance)
    static void on_upload_cb(lv_event_t* e);
    static void on_cancel_cb(lv_event_t* e);
    static void on_done_cb(lv_event_t* e);
    static void on_close_cb(lv_event_t* e);

    // Instance handlers
    void handle_upload();
    void handle_cancel();
    void handle_done();
    void handle_close();
};
