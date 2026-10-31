// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "helix_plugin_installer.h"

#include <atomic>
#include <functional>
#include <string>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
} // namespace helix

namespace helix::ui {

/**
 * @brief Ask Moonraker to restart so it loads or drops the helix_print plugin
 *
 * install.sh runs under the service's NoNewPrivileges and cannot sudo, so it is
 * always run with --no-restart and Moonraker is asked to restart itself. A
 * request Moonraker rejects is reported with a toast.
 *
 * @return true when a restart was requested; false with no API to ask
 */
bool request_moonraker_restart_for_plugin(IMoonrakerAPI* api);

/**
 * @brief Refuse a plugin install or uninstall while a job holds the machine
 *
 * Both restart Moonraker, and the install also blocks this thread while its
 * script runs. Shows the warning toast when it refuses.
 *
 * @return true when refused
 */
bool refuse_plugin_change_during_job(PrinterState& printer_state);

} // namespace helix::ui

/**
 * @brief Modal dialog prompting user to install the helix_print plugin
 *
 * Two display modes:
 * - LOCAL: Connected to localhost - shows "Install Plugin" button for auto-install
 * - REMOTE: Connected to remote printer - shows curl command with "Copy" button
 *
 * The modal remembers user preference via "Don't ask again" checkbox, which
 * persists to config via HelixPluginInstaller::set_install_declined().
 *
 * Usage:
 * @code
 *   PluginInstallModal modal;
 *   modal.set_installer(&installer_);
 *   modal.show(lv_screen_active());
 * @endcode
 */
class PluginInstallModal : public Modal {
  public:
    /// Gets Moonraker to load a freshly installed plugin; returns whether a
    /// restart was requested.
    using MoonrakerRestarter = std::function<bool()>;

    PluginInstallModal();
    ~PluginInstallModal() override {
        // Signal destruction to prevent async callbacks from accessing destroyed object
        is_destroying_.store(true);
        // A shown modal's owner can destroy it without on_hide() running, and the static
        // XML callbacks would then reach a freed instance.
        if (s_active_instance_ == this) {
            s_active_instance_ = nullptr;
        }
    }

    const char* get_name() const override {
        return "Plugin Install";
    }
    const char* component_name() const override {
        return "plugin_install_modal";
    }

    /**
     * @brief Set the plugin installer instance
     *
     * Required before showing the modal. The installer determines whether
     * to show local or remote mode.
     */
    void set_installer(helix::HelixPluginInstaller* installer);

    /**
     * @brief Set what a successful local install calls to load the plugin
     *
     * Files on disk are not a loaded plugin: Moonraker loads it on restart.
     * Without a restarter, or when it requests none, the result says a
     * restart is still needed instead of claiming one is under way.
     */
    void set_moonraker_restarter(MoonrakerRestarter restarter);

    /// Asked when Install is tapped, since a job can start while the modal is
    /// open; true refuses (it reports why itself). Unset never refuses.
    using JobGate = std::function<bool()>;
    void set_job_gate(JobGate gate);

  protected:
    void on_show() override;
    void on_hide() override;
    void on_cancel() override;
    // ESC closes without recording the "don't ask again" choice.
    void on_escape() override {
        hide(ModalCloseReason::EscKey);
    }

  private:
    helix::HelixPluginInstaller* installer_ = nullptr;
    MoonrakerRestarter moonraker_restarter_;
    JobGate job_gate_;

    // Destruction guard - prevents async callbacks from accessing destroyed object
    std::atomic<bool> is_destroying_{false};

    // UI state
    bool is_local_mode_ = false;

    // Widget references (populated in on_show)
    lv_obj_t* local_description_ = nullptr;
    lv_obj_t* remote_description_ = nullptr;
    lv_obj_t* command_textarea_ = nullptr;
    lv_obj_t* local_button_row_ = nullptr;
    lv_obj_t* remote_button_row_ = nullptr;
    lv_obj_t* result_button_row_ = nullptr;
    lv_obj_t* installing_container_ = nullptr;
    lv_obj_t* result_container_ = nullptr;
    lv_obj_t* checkbox_container_ = nullptr;
    lv_obj_t* dont_ask_checkbox_ = nullptr;
    lv_obj_t* copy_feedback_ = nullptr;

    // Internal handlers
    void on_install_clicked();
    void on_copy_clicked();
    void show_installing_state();
    void show_result_state(bool success, const std::string& message);
    void check_dont_ask_preference();

    // Static event handlers for XML callbacks
    static void install_clicked_cb(lv_event_t* e);
    static void copy_clicked_cb(lv_event_t* e);
    static void cancel_clicked_cb(lv_event_t* e);
    static void ok_clicked_cb(lv_event_t* e);

    // Register XML event callbacks (called once in constructor)
    static void register_callbacks();
    static bool callbacks_registered_;

    // Active instance pointer for static XML callbacks (only one modal visible at a time)
    static PluginInstallModal* s_active_instance_;
};
