// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "async_lifetime_guard.h"
#include "helix_plugin_installer.h"

#include <atomic>
#include <functional>
#include <string>

namespace helix {
class IMoonrakerAPI;
class PrinterState;
struct PluginInstallModalTestAccess; // test-only friend (tests/test_helpers/)
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
 * Both restart Moonraker. Shows the warning toast when it refuses.
 *
 * @return true when refused
 */
bool refuse_plugin_change_during_job(PrinterState& printer_state);

/// Whether a job holds the machine, so the plugin must not change now. No toast.
bool job_blocks_plugin_change(PrinterState& printer_state);

} // namespace helix::ui

/**
 * @brief Modal dialog prompting user to install the helix_print plugin
 *
 * Two display modes:
 * - LOCAL: Connected to localhost - shows "Install Plugin" button for auto-install
 * - REMOTE: Connected to remote printer - shows curl command with "Copy" button
 *
 * Any close with "Don't ask again" ticked declines for good
 * (HelixPluginInstaller::set_install_declined()), which outranks the snooze a
 * user's close also leaves (HelixPluginInstaller::snooze_install_prompt()).
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
    }

    const char* get_name() const override {
        return "Plugin Install";
    }
    const char* component_name() const override {
        return "plugin_install_modal";
    }
    HELIX_KIND_TAG(PluginInstallModal)

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

    /// Asked again when the install ends, since a job can start while install.sh
    /// runs; true holds the Moonraker restart back. Reports nothing itself.
    /// Unset never holds it.
    using JobCheck = std::function<bool()>;
    void set_job_check(JobCheck check);

  protected:
    void on_show() override;
    void on_hide() override;
    void on_cancel() override;
    void on_escape() override {
        hide(ModalCloseReason::EscKey);
    }
    void on_dismissed() override;

  private:
    friend struct helix::PluginInstallModalTestAccess;

    /// Runs the install off the main thread; unset submits to HttpExecutor::slow().
    using InstallSubmitter = std::function<void(std::function<void()>)>;

    helix::HelixPluginInstaller* installer_ = nullptr;
    MoonrakerRestarter moonraker_restarter_;
    JobGate job_gate_;
    JobCheck job_check_;

    // Destruction guard - prevents async callbacks from accessing destroyed object
    std::atomic<bool> is_destroying_{false};

    // UI state
    bool is_local_mode_ = false;

    // install.sh runs on a worker; its outcome comes back to on_install_finished().
    // Closing the dialog does not drop it (the Moonraker restart and the
    // installer state still follow), so it has its own guard rather than
    // lifetime_, which hide() invalidates.
    bool install_running_ = false;
    InstallSubmitter install_submitter_;
    helix::AsyncLifetimeGuard install_lifetime_;

    // Widget references (populated in on_show)
    lv_obj_t* command_textarea_ = nullptr;
    lv_obj_t* dont_ask_checkbox_ = nullptr;

    // Internal handlers
    void on_install_clicked();
    void on_install_finished(const helix::HelixPluginInstaller::LocalInstallJob& job,
                             const helix::HelixPluginInstaller::LocalInstallRun& run);
    void on_copy_clicked();
    void show_installing_state();
    void show_result_state(bool success, const std::string& message);

    // Static event handlers for XML callbacks
    static void install_clicked_cb(lv_event_t* e);
    static void copy_clicked_cb(lv_event_t* e);
    static void cancel_clicked_cb(lv_event_t* e);
    static void ok_clicked_cb(lv_event_t* e);

    // Register XML event callbacks (called once in constructor)
    static void register_callbacks();
    static bool callbacks_registered_;
};
