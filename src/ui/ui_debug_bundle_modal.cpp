// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_debug_bundle_modal.h"

#include "ui_keyboard_manager.h"

#include "lvgl/src/others/translation/lv_translation.h"
#include "system/debug_bundle_collector.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <memory>

// =============================================================================
// Static Members
// =============================================================================

bool DebugBundleModal::callbacks_registered_ = false;
DebugBundleModal* DebugBundleModal::active_instance_ = nullptr;

// =============================================================================
// Constructor / Destructor
// =============================================================================

DebugBundleModal::DebugBundleModal() {
    spdlog::debug("[DebugBundleModal] Constructed");
}

DebugBundleModal::~DebugBundleModal() {
    deinit_subjects(); // MUST be before member destruction
    if (active_instance_ == this) {
        active_instance_ = nullptr;
    }
    spdlog::trace("[DebugBundleModal] Destroyed");
}

// =============================================================================
// Public API
// =============================================================================

bool DebugBundleModal::show_modal(lv_obj_t* parent) {
    register_callbacks();
    init_subjects();

    // Reset to consent state
    lv_subject_set_int(&state_subject_, 0);
    lv_subject_set_int(&include_logs_subject_, 0);
    lv_subject_copy_string(&status_subject_, "");
    lv_subject_copy_string(&share_code_subject_, "");
    lv_subject_copy_string(&error_subject_, "");

    // Call base class show
    bool result = show(parent);
    if (result && dialog()) {
        active_instance_ = this;
    }

    return result;
}

bool DebugBundleModal::show_owned() {
    auto modal = std::make_unique<DebugBundleModal>();
    if (!modal->show_modal(lv_screen_active())) {
        // show_modal() already registered the XML-named subjects; the global
        // subject map must not outlive the instance the unique_ptr frees here.
        modal->deinit_subjects();
        return false;
    }
    lv_obj_t* backdrop = modal->backdrop();
    ModalStack::instance().assume_ownership(backdrop, std::move(modal));
    return true;
}

// =============================================================================
// Lifecycle Hooks
// =============================================================================

void DebugBundleModal::on_show() {
    spdlog::debug("[DebugBundleModal] on_show");
    if (dialog()) {
        lv_obj_t* note_ta = lv_obj_find_by_name(dialog(), "user_note_textarea");
        if (note_ta) {
            lv_textarea_set_max_length(note_ta, 500);
            KeyboardManager::instance().register_textarea(note_ta);
        }
    }
}

void DebugBundleModal::on_hide() {
    spdlog::debug("[DebugBundleModal] on_hide");
    active_instance_ = nullptr;
    // No self-delete: the ModalStack owns this instance and frees it when its
    // entry goes (#1382).
}

// =============================================================================
// Subject Management
// =============================================================================

void DebugBundleModal::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    lv_subject_init_int(&state_subject_, 0);
    lv_subject_init_string(&status_subject_, status_buf_, nullptr, sizeof(status_buf_), "");
    lv_subject_init_string(&share_code_subject_, share_code_buf_, nullptr, sizeof(share_code_buf_),
                           "");
    lv_subject_init_string(&error_subject_, error_buf_, nullptr, sizeof(error_buf_), "");
    lv_subject_init_int(&include_logs_subject_, 0);

    lv_xml_register_subject(nullptr, "debug_bundle_state", &state_subject_);
    lv_xml_register_subject(nullptr, "debug_bundle_status", &status_subject_);
    lv_xml_register_subject(nullptr, "debug_bundle_share_code", &share_code_subject_);
    lv_xml_register_subject(nullptr, "debug_bundle_error", &error_subject_);
    lv_xml_register_subject(nullptr, "debug_bundle_include_logs", &include_logs_subject_);

    subjects_initialized_ = true;
}

void DebugBundleModal::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    lv_subject_deinit(&state_subject_);
    lv_subject_deinit(&status_subject_);
    lv_subject_deinit(&share_code_subject_);
    lv_subject_deinit(&error_subject_);
    lv_subject_deinit(&include_logs_subject_);

    subjects_initialized_ = false;
}

// =============================================================================
// Callback Registration
// =============================================================================

void DebugBundleModal::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }

    lv_xml_register_event_cb(nullptr, "on_debug_bundle_upload", on_upload_cb);
    lv_xml_register_event_cb(nullptr, "on_debug_bundle_cancel", on_cancel_cb);
    lv_xml_register_event_cb(nullptr, "on_debug_bundle_done", on_done_cb);
    lv_xml_register_event_cb(nullptr, "on_debug_bundle_close", on_close_cb);

    callbacks_registered_ = true;
}

// =============================================================================
// Static Event Callbacks
// =============================================================================

void DebugBundleModal::on_upload_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_upload();
    }
}

void DebugBundleModal::on_cancel_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_cancel();
    }
}

void DebugBundleModal::on_done_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_done();
    }
}

void DebugBundleModal::on_close_cb(lv_event_t* /*e*/) {
    if (active_instance_) {
        active_instance_->handle_close();
    }
}

// =============================================================================
// Instance Event Handlers
// =============================================================================

void DebugBundleModal::handle_upload() {
    spdlog::info("[DebugBundleModal] Upload clicked");

    // Build options from UI state (read textarea before state transition hides it)
    helix::BundleOptions options;
    options.include_klipper_logs = (lv_subject_get_int(&include_logs_subject_) != 0);
    options.include_moonraker_logs = options.include_klipper_logs;

    if (dialog()) {
        lv_obj_t* note_ta = lv_obj_find_by_name(dialog(), "user_note_textarea");
        if (note_ta) {
            const char* text = lv_textarea_get_text(note_ta);
            if (text && text[0] != '\0') {
                options.user_note = text;
            }
        }
    }

    // Transition to uploading state
    lv_subject_set_int(&state_subject_, 1);
    lv_subject_copy_string(&status_subject_, lv_tr("Collecting data..."));

    auto token = lifetime_.token();

    helix::DebugBundleCollector::upload_async(
        options, [this, token](const helix::BundleResult& result) {
            if (token.expired()) {
                spdlog::debug("[DebugBundleModal] Modal destroyed "
                              "during upload, ignoring result");
                return;
            }
            // Marshal to UI thread — this callback fires from the upload thread
            token.defer([this, result]() {
                if (result.success) {
                    lv_subject_copy_string(&share_code_subject_, result.share_code.c_str());
                    lv_subject_set_int(&state_subject_, 2);
                    spdlog::info("[DebugBundleModal] Upload succeeded, "
                                 "share code: {}",
                                 result.share_code);
                } else {
                    lv_subject_copy_string(&error_subject_, result.error_message.c_str());
                    lv_subject_set_int(&state_subject_, 3);
                    spdlog::warn("[DebugBundleModal] Upload failed: {}", result.error_message);
                }
            });
        });
}

void DebugBundleModal::handle_cancel() {
    spdlog::debug("[DebugBundleModal] Cancel clicked");
    hide();
}

void DebugBundleModal::handle_done() {
    spdlog::debug("[DebugBundleModal] Done clicked");
    hide();
}

void DebugBundleModal::handle_close() {
    spdlog::debug("[DebugBundleModal] Close clicked");
    hide();
}
