// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_overlay_timelapse_install.h"

#include <string>
#include <utility>

/// Private wizard-position access for TimelapseInstallOverlay tests.
struct TimelapseInstallOverlayTestAccess {
    /// Park the wizard on its restart step, as a run that got that far leaves it.
    static void park_at_restart(TimelapseInstallOverlay& o) {
        o.current_step_ = TimelapseInstallOverlay::Step::RESTART_MOONRAKER;
    }
    static bool at_restart(const TimelapseInstallOverlay& o) {
        return o.current_step_ == TimelapseInstallOverlay::Step::RESTART_MOONRAKER;
    }
    static bool wizard_active(const TimelapseInstallOverlay& o) {
        return o.wizard_active_;
    }
    /// Put the wizard on its SSH-instructions step, the remote route of step 3.
    static void show_remote_route(TimelapseInstallOverlay& o) {
        o.wizard_active_ = true;
        o.step_show_install_instructions();
    }
    /// The commands a local install runs, in place of real processes.
    static void set_runner(TimelapseInstallOverlay& o, helix::ProcessRunner runner) {
        o.runner_override_ = std::move(runner);
    }
    /// Start step 3's local route, as the plugin-absent reply does.
    static void install_locally(TimelapseInstallOverlay& o) {
        o.step_install_locally();
    }
    static bool install_running(const TimelapseInstallOverlay& o) {
        return o.install_running_.load();
    }
    static unsigned generation(const TimelapseInstallOverlay& o) {
        return o.run_generation_;
    }
    /// Deliver a local install's outcome as the worker of run @p gen does.
    static void finish_install(TimelapseInstallOverlay& o, unsigned gen,
                               const helix::LocalPluginResult& result) {
        o.on_local_install_finished(gen, result);
    }
    static void park_at_install(TimelapseInstallOverlay& o) {
        o.current_step_ = TimelapseInstallOverlay::Step::INSTALL_PLUGIN;
    }
    static void restart_moonraker(TimelapseInstallOverlay& o) {
        o.step_restart_moonraker();
    }
    static bool at_install(const TimelapseInstallOverlay& o) {
        return o.current_step_ == TimelapseInstallOverlay::Step::INSTALL_PLUGIN;
    }
    static bool at_configure(const TimelapseInstallOverlay& o) {
        return o.current_step_ == TimelapseInstallOverlay::Step::CONFIGURE_MOONRAKER;
    }
    static std::string status(TimelapseInstallOverlay& o) {
        return lv_subject_get_string(&o.status_subject_);
    }
    static int card(TimelapseInstallOverlay& o) {
        return lv_subject_get_int(&o.card_subject_);
    }
    static bool action_visible(TimelapseInstallOverlay& o) {
        return lv_subject_get_int(&o.action_visible_subject_) != 0;
    }

    /// Fire the footer action the way its button does.
    static bool run_action(TimelapseInstallOverlay& o) {
        if (!o.action_callback_) {
            return false;
        }
        o.action_callback_();
        return true;
    }
};
