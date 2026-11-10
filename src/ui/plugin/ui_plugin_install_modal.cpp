// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_plugin_install_modal.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_toast_manager.h"

#include "core/static_subject_registry.h"
#include "core/update_queue.h"
#include "i_moonraker_api.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "subject_managed_panel.h"
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

void PluginInstallModal::set_moonraker_restarter(MoonrakerRestarter restarter) {
    moonraker_restarter_ = std::move(restarter);
}

void PluginInstallModal::set_job_gate(JobGate gate) {
    job_gate_ = std::move(gate);
}

bool helix::ui::refuse_plugin_change_during_job(helix::PrinterState& printer_state) {
    if (!job_holds_machine(printer_state.print_state().get_print_lifecycle())) {
        return false;
    }
    NOTIFY_WARNING(lv_tr("Cannot change the HelixPrint plugin while printing"));
    return true;
}

bool helix::ui::request_moonraker_restart_for_plugin(helix::IMoonrakerAPI* api) {
    if (!api) {
        spdlog::warn("[Plugin Install] No Moonraker API - cannot restart Moonraker for the plugin");
        return false;
    }
    api->restart_moonraker(
        []() { spdlog::info("[Plugin Install] Moonraker restart requested for the plugin"); },
        [](const MoonrakerError& error) {
            spdlog::warn("[Plugin Install] Moonraker restart for the plugin failed: {}",
                         error.message);
            helix::ui::queue_update("PluginInstall::restart_failed", []() {
                ToastManager::instance().show(
                    ToastSeverity::WARNING, lv_tr("Restart Moonraker to finish the plugin change"),
                    5000);
            });
        });
    return true;
}

// Static member initialization
bool PluginInstallModal::callbacks_registered_ = false;

namespace {

/// What the dialog shows; plugin_install_modal.xml binds each part to its phases.
enum class Phase : int {
    LocalPrompt = 0,
    RemotePrompt = 1,
    Installing = 2,
    Succeeded = 3,
    Failed = 4
};

/// plugin_install_copy_result: which Copy Command outcome is shown, if any.
enum class CopyResult : int { Hidden = 0, Copied = 1, Failed = 2, NoClipboard = 3 };

// One set for the class: a single plugin dialog is on screen at a time, and
// each show() sets them afresh.
struct InstallSubjects {
    lv_subject_t phase{};
    lv_subject_t copy_result{};
    lv_subject_t result_message{};
    char result_message_buf[256] = {};
    SubjectManager manager;
    bool initialized = false;
};

InstallSubjects& install_subjects() {
    static InstallSubjects s;
    return s;
}

void init_install_subjects() {
    auto& s = install_subjects();
    if (s.initialized) {
        return;
    }
    UI_MANAGED_SUBJECT_INT(s.phase, static_cast<int>(Phase::LocalPrompt), "plugin_install_phase",
                           s.manager);
    UI_MANAGED_SUBJECT_INT(s.copy_result, static_cast<int>(CopyResult::Hidden),
                           "plugin_install_copy_result", s.manager);
    UI_MANAGED_SUBJECT_STRING(s.result_message, s.result_message_buf, "",
                              "plugin_install_result_message", s.manager);
    s.initialized = true;
    StaticSubjectRegistry::instance().register_deinit("PluginInstallModal", []() {
        auto& subjects = install_subjects();
        subjects.manager.deinit_all();
        subjects.initialized = false;
    });
}

void set_phase(Phase phase) {
    lv_subject_set_int(&install_subjects().phase, static_cast<int>(phase));
}

void set_copy_result(CopyResult result) {
    lv_subject_set_int(&install_subjects().copy_result, static_cast<int>(result));
}

} // namespace

// ============================================================================
// Constructor
// ============================================================================

PluginInstallModal::PluginInstallModal() {
    // Register callbacks and the dialog's subjects once before any modal is shown
    register_callbacks();
    init_install_subjects();
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
    command_textarea_ = find_widget("command_textarea");
    dont_ask_checkbox_ = find_widget("dont_ask_checkbox");

    // Determine mode based on installer
    is_local_mode_ = installer_ && installer_->is_local_moonraker();

    spdlog::info("[Plugin Install] Showing in {} mode", is_local_mode_ ? "LOCAL" : "REMOTE");

    set_phase(is_local_mode_ ? Phase::LocalPrompt : Phase::RemotePrompt);
    set_copy_result(CopyResult::Hidden);

    // REMOTE mode shows the curl command to run over SSH
    if (!is_local_mode_ && command_textarea_ && installer_) {
        std::string cmd = installer_->get_remote_install_command();
        lv_textarea_set_text(command_textarea_, cmd.c_str());
    }

    // Wire cancel button and set user_data for all buttons
    wire_cancel_button("btn_cancel");
    wire_cancel_button("btn_done");
    wire_ok_button("btn_ok");
}

void PluginInstallModal::on_hide() {
    // Every close passes here, so the box is honoured however the dialog goes.
    if (installer_ && dont_ask_checkbox_ &&
        lv_obj_has_state(dont_ask_checkbox_, LV_STATE_CHECKED)) {
        spdlog::info("[Plugin Install] User selected 'Don't ask again'");
        installer_->set_install_declined();
    }

    // Clear widget references
    command_textarea_ = nullptr;
    dont_ask_checkbox_ = nullptr;
}

void PluginInstallModal::on_cancel() {
    if (installer_) {
        installer_->snooze_install_prompt();
    }
    Modal::on_cancel();
}

void PluginInstallModal::on_dismissed() {
    // Tapping away is a Not Now. A programmatic close is no answer at all.
    if (installer_ && (close_reason_ == ModalCloseReason::BackdropTap ||
                       close_reason_ == ModalCloseReason::EscKey)) {
        installer_->snooze_install_prompt();
    }
}

// ============================================================================
// UI State Management
// ============================================================================

void PluginInstallModal::show_installing_state() {
    set_phase(Phase::Installing);
}

void PluginInstallModal::show_result_state(bool success, const std::string& message) {
    lv_subject_copy_string(&install_subjects().result_message, message.c_str());
    set_phase(success ? Phase::Succeeded : Phase::Failed);
}

// ============================================================================
// Button Handlers
// ============================================================================

void PluginInstallModal::on_install_clicked() {
    if (!installer_) {
        spdlog::error("[Plugin Install] No installer set");
        return;
    }

    if (job_gate_ && job_gate_()) {
        spdlog::info("[Plugin Install] Install refused: a job holds the machine");
        return;
    }

    spdlog::info("[Plugin Install] Starting local installation");
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
    auto install_result = installer_->install_local_sync();

    spdlog::info("[Plugin Install] Installation {}: {}",
                 install_result.success ? "succeeded" : "failed", install_result.message);

    std::string message = install_result.message;
    if (install_result.success) {
        const bool restarting = moonraker_restarter_ && moonraker_restarter_();
        message = restarting ? lv_tr("Plugin installed successfully. Moonraker is restarting...")
                             : lv_tr("Restart Moonraker to finish the plugin change");
    }

    show_result_state(install_result.success, message);
    installer_->snooze_install_prompt();
}

void PluginInstallModal::on_copy_clicked() {
    if (!command_textarea_) {
        return;
    }

    const char* cmd = lv_textarea_get_text(command_textarea_);
    if (!cmd || strlen(cmd) == 0) {
        return;
    }

    spdlog::info("[Plugin Install] Copying command to clipboard");

#ifdef HELIX_DISPLAY_SDL
    // Use SDL's cross-platform clipboard API (safe, no shell injection)
    int result = SDL_SetClipboardText(cmd);

    if (result == 0) {
        set_copy_result(CopyResult::Copied);
        spdlog::debug("[Plugin Install] Command copied successfully");
    } else {
        set_copy_result(CopyResult::Failed);
        spdlog::warn("[Plugin Install] SDL clipboard failed: {}", SDL_GetError());
    }
#else
    // No clipboard support on framebuffer displays
    set_copy_result(CopyResult::NoClipboard);
    spdlog::info("[Plugin Install] Clipboard not available on this platform");
#endif
}

// ============================================================================
// Static Event Handlers
// ============================================================================

void PluginInstallModal::install_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] install_clicked_cb");

    if (auto* self = Modal::from_event<PluginInstallModal>(e)) {
        self->on_install_clicked();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PluginInstallModal::copy_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] copy_clicked_cb");

    if (auto* self = Modal::from_event<PluginInstallModal>(e)) {
        self->on_copy_clicked();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PluginInstallModal::cancel_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] cancel_clicked_cb");

    if (auto* self = Modal::from_event<PluginInstallModal>(e)) {
        self->on_cancel();
    }

    LVGL_SAFE_EVENT_CB_END();
}

void PluginInstallModal::ok_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PluginInstallModal] ok_clicked_cb");

    if (auto* self = Modal::from_event<PluginInstallModal>(e)) {
        self->on_ok();
    }

    LVGL_SAFE_EVENT_CB_END();
}
