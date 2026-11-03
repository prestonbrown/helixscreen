// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_install_modal.h"

#include "ui_event_safety.h"
#include "ui_icon.h"
#include "ui_update_queue.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif
#include <thread>

// ============================================================================
// Configuration
// ============================================================================

void PluginInstallModal::set_installer(helix::HelixPluginInstaller* installer) {
    installer_ = installer;
}

void PluginInstallModal::set_on_install_complete(InstallCompleteCallback cb) {
    on_install_complete_cb_ = std::move(cb);
}

// Static member initialization
bool PluginInstallModal::callbacks_registered_ = false;
PluginInstallModal* PluginInstallModal::s_active_instance_ = nullptr;

// ============================================================================
// Constructor
// ============================================================================

PluginInstallModal::PluginInstallModal() {
    // Register callbacks once before any modal is shown
    register_callbacks();
}

// ============================================================================
// Static Callback Registration
// ============================================================================

void PluginInstallModal::register_callbacks() {
    if (callbacks_registered_) {
        return;
    }
    lv_xml_register_event_cb(nullptr, "on_plugin_install_clicked", install_clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_plugin_copy_clicked", copy_clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_plugin_install_cancel", cancel_clicked_cb);
    lv_xml_register_event_cb(nullptr, "on_plugin_install_ok", ok_clicked_cb);
    callbacks_registered_ = true;
    spdlog::trace("[PluginInstallModal] Event callbacks registered");
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void PluginInstallModal::on_show() {
    // Find widgets
    local_description_ = find_widget("local_description");
    remote_description_ = find_widget("remote_description");
    command_textarea_ = find_widget("command_textarea");
    local_button_row_ = find_widget("local_button_row");
    remote_button_row_ = find_widget("remote_button_row");
    result_button_row_ = find_widget("result_button_row");
    installing_container_ = find_widget("installing_container");
    result_container_ = find_widget("result_container");
    checkbox_container_ = find_widget("checkbox_container");
    dont_ask_checkbox_ = find_widget("dont_ask_checkbox");
    phase_tracking_checkbox_ = find_widget("phase_tracking_checkbox");
    copy_feedback_ = find_widget("copy_feedback");

    // Determine mode based on installer
    is_local_mode_ = installer_ && installer_->is_local_moonraker();

    spdlog::info("[Plugin Install] Showing in {} mode", is_local_mode_ ? "LOCAL" : "REMOTE");

    if (is_local_mode_) {
        // LOCAL mode: Show install button
        if (local_description_)
            lv_obj_remove_flag(local_description_, LV_OBJ_FLAG_HIDDEN);
        if (remote_description_)
            lv_obj_add_flag(remote_description_, LV_OBJ_FLAG_HIDDEN);
        if (local_button_row_)
            lv_obj_remove_flag(local_button_row_, LV_OBJ_FLAG_HIDDEN);
        if (remote_button_row_)
            lv_obj_add_flag(remote_button_row_, LV_OBJ_FLAG_HIDDEN);
    } else {
        // REMOTE mode: Show curl command
        if (local_description_)
            lv_obj_add_flag(local_description_, LV_OBJ_FLAG_HIDDEN);
        if (remote_description_)
            lv_obj_remove_flag(remote_description_, LV_OBJ_FLAG_HIDDEN);
        if (local_button_row_)
            lv_obj_add_flag(local_button_row_, LV_OBJ_FLAG_HIDDEN);
        if (remote_button_row_)
            lv_obj_remove_flag(remote_button_row_, LV_OBJ_FLAG_HIDDEN);

        // Populate the curl command
        if (command_textarea_ && installer_) {
            std::string cmd = installer_->get_remote_install_command();
            lv_textarea_set_text(command_textarea_, cmd.c_str());
        }

        // Reset copy feedback from previous show
        if (copy_feedback_) {
            lv_label_set_text(copy_feedback_, "");
            lv_obj_add_flag(copy_feedback_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Wire cancel button and set user_data for all buttons
    wire_cancel_button("btn_cancel");
    wire_cancel_button("btn_done");
    wire_ok_button("btn_ok");

    // Set active instance for static XML callbacks
    s_active_instance_ = this;
}

void PluginInstallModal::on_hide() {
    // Clear active instance
    s_active_instance_ = nullptr;

    // Clear widget references
    local_description_ = nullptr;
    remote_description_ = nullptr;
    command_textarea_ = nullptr;
    local_button_row_ = nullptr;
    remote_button_row_ = nullptr;
    result_button_row_ = nullptr;
    installing_container_ = nullptr;
    result_container_ = nullptr;
    checkbox_container_ = nullptr;
    dont_ask_checkbox_ = nullptr;
    phase_tracking_checkbox_ = nullptr;
    copy_feedback_ = nullptr;
}

void PluginInstallModal::on_cancel() {
    check_dont_ask_preference();
    hide();
}

// ============================================================================
// UI State Management
// ============================================================================

void PluginInstallModal::show_installing_state() {
    // Hide all content except installing spinner
    if (local_description_)
        lv_obj_add_flag(local_description_, LV_OBJ_FLAG_HIDDEN);
    if (remote_description_)
        lv_obj_add_flag(remote_description_, LV_OBJ_FLAG_HIDDEN);
    if (local_button_row_)
        lv_obj_add_flag(local_button_row_, LV_OBJ_FLAG_HIDDEN);
    if (remote_button_row_)
        lv_obj_add_flag(remote_button_row_, LV_OBJ_FLAG_HIDDEN);
    if (checkbox_container_)
        lv_obj_add_flag(checkbox_container_, LV_OBJ_FLAG_HIDDEN);

    // Show installing container
    if (installing_container_)
        lv_obj_remove_flag(installing_container_, LV_OBJ_FLAG_HIDDEN);
}

void PluginInstallModal::show_result_state(bool success, const std::string& message) {
    // Hide installing spinner
    if (installing_container_)
        lv_obj_add_flag(installing_container_, LV_OBJ_FLAG_HIDDEN);

    // Show result container
    if (result_container_)
        lv_obj_remove_flag(result_container_, LV_OBJ_FLAG_HIDDEN);

    // Show OK button row
    if (result_button_row_)
        lv_obj_remove_flag(result_button_row_, LV_OBJ_FLAG_HIDDEN);

    // Update result content
    lv_obj_t* result_icon = find_widget("result_icon");
    lv_obj_t* result_title = find_widget("result_title");
    lv_obj_t* result_message = find_widget("result_message");

    if (success) {
        if (result_icon) {
            ui_icon_set_source(result_icon, "check_circle");
            ui_icon_set_variant(result_icon, "success");
        }
        if (result_title) {
            lv_label_set_text(result_title, lv_tr("Success!"));
        }
    } else {
        if (result_icon) {
            ui_icon_set_source(result_icon, "alert_circle");
            ui_icon_set_variant(result_icon, "danger");
        }
        if (result_title) {
            lv_label_set_text(result_title, lv_tr("Installation Failed"));
        }
    }

    if (result_message) {
        lv_label_set_text(result_message, message.c_str());
    }
}

void PluginInstallModal::check_dont_ask_preference() {
    if (dont_ask_checkbox_ && installer_) {
        bool checked = lv_obj_has_state(dont_ask_checkbox_, LV_STATE_CHECKED);
        if (checked) {
            spdlog::info("[Plugin Install] User selected 'Don't ask again'");
            installer_->set_install_declined();
        }
    }
}

// ============================================================================
// Button Handlers
// ============================================================================

void PluginInstallModal::on_install_clicked() {
    if (!installer_) {
        spdlog::error("[Plugin Install] No installer set");
        return;
    }

    // Check if phase tracking is enabled (checkbox is checked by default)
    bool enable_phase_tracking = false;
    if (phase_tracking_checkbox_) {
        enable_phase_tracking = lv_obj_has_state(phase_tracking_checkbox_, LV_STATE_CHECKED);
    }

    spdlog::info("[Plugin Install] Starting local installation (phase_tracking={})",
                 enable_phase_tracking);
    show_installing_state();

    // Run installation synchronously. This blocks the UI but is necessary because
    // std::thread causes SIGABRT on ARM Linux with static glibc linking when the
    // thread exits. The install script typically runs in <30 seconds, and this is
    // a one-time operation, so blocking is acceptable.
    //
    // Technical background: On ARM Linux with musl or static glibc, thread-local
    // storage (TLS) cleanup during std::thread exit can trigger SIGABRT. This
    // affects any code that uses TLS (spdlog, std::function, etc.) on a detached
    // thread. The only reliable workaround is to avoid detached threads entirely.
    auto install_result = installer_->install_local_sync(enable_phase_tracking);

    spdlog::info("[Plugin Install] Installation {}: {}",
                 install_result.success ? "succeeded" : "failed", install_result.message);

    show_result_state(install_result.success, install_result.message);
    check_dont_ask_preference();

    if (on_install_complete_cb_) {
        on_install_complete_cb_(install_result.success);
    }
}

void PluginInstallModal::on_copy_clicked() {
    if (!command_textarea_) {
        return;
    }

    // Get the command text
    const char* cmd = lv_textarea_get_text(command_textarea_);
    if (!cmd || strlen(cmd) == 0) {
        return;
    }

    spdlog::info("[Plugin Install] Copying command to clipboard");

#ifdef HELIX_DISPLAY_SDL
    // Use SDL's cross-platform clipboard API (safe, no shell injection)
    int result = SDL_SetClipboardText(cmd);

    if (copy_feedback_) {
        if (result == 0) {
            lv_label_set_text(copy_feedback_, lv_tr("Copied to clipboard!"));
            spdlog::debug("[Plugin Install] Command copied successfully");
        } else {
            lv_label_set_text(copy_feedback_, lv_tr("Copy failed - use SSH manually"));
            spdlog::warn("[Plugin Install] SDL clipboard failed: {}", SDL_GetError());
        }
        lv_obj_remove_flag(copy_feedback_, LV_OBJ_FLAG_HIDDEN);
    }
#else
    // No clipboard support on framebuffer displays
    if (copy_feedback_) {
        lv_label_set_text(copy_feedback_, lv_tr("Clipboard unavailable - use SSH"));
        lv_obj_remove_flag(copy_feedback_, LV_OBJ_FLAG_HIDDEN);
    }
    spdlog::info("[Plugin Install] Clipboard not available on this platform");
#endif
}

// ============================================================================
// Static Event Handlers
// ============================================================================

void PluginInstallModal::install_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] install_clicked_cb");

    if (s_active_instance_) {
        s_active_instance_->on_install_clicked();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PluginInstallModal::copy_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] copy_clicked_cb");

    if (s_active_instance_) {
        s_active_instance_->on_copy_clicked();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PluginInstallModal::cancel_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] cancel_clicked_cb");

    if (s_active_instance_) {
        s_active_instance_->on_cancel();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PluginInstallModal::ok_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] ok_clicked_cb");

    if (s_active_instance_) {
        s_active_instance_->on_ok();
    }

    LVGL_SAFE_EVENT_CB_END();
}
