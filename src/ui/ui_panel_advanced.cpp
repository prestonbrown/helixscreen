// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_advanced.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_overlay_timelapse_install.h"
#include "ui_overlay_timelapse_videos.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_console.h"
#include "ui_panel_macros.h"
#include "ui_panel_spoolman.h"
#include "ui_toast_manager.h"

#include "app_globals.h"
#include "config.h"
#include "exception_policy.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "observer_factory.h"
#include "panel_widgets/shutdown_widget.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

using namespace helix;

// Global instance (singleton pattern matching SettingsPanel)
static std::unique_ptr<AdvancedPanel> g_advanced_panel;

AdvancedPanel& get_global_advanced_panel() {
    // Should be initialized by main.cpp before use
    if (!g_advanced_panel) {
        spdlog::error("[Advanced Panel] get_global_advanced_panel() called before initialization!");
        helix::throw_or_abort(std::runtime_error("AdvancedPanel not initialized"));
    }
    return *g_advanced_panel;
}

// Called by main.cpp to initialize the global instance
void init_global_advanced_panel(PrinterState& printer_state, IMoonrakerAPI* api) {
    g_advanced_panel = std::make_unique<AdvancedPanel>(printer_state, api);
    StaticPanelRegistry::instance().register_destroy("AdvancedPanel",
                                                     []() { g_advanced_panel.reset(); });
}

// ============================================================================
// CONSTRUCTOR
// ============================================================================

AdvancedPanel::AdvancedPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::trace("[{}] Constructor", get_name());
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void AdvancedPanel::init_subjects() {
    // Register XML event callbacks (must be done BEFORE XML is created)
    register_xml_callbacks({
        {"on_advanced_spoolman", on_spoolman_clicked},
        {"on_advanced_macros", on_macros_clicked},
        {"on_console_row_clicked", on_console_clicked},
        {"on_history_row_clicked", on_history_clicked},
        {"on_helix_plugin_install_clicked", on_helix_plugin_install_clicked},
        {"on_helix_plugin_uninstall_clicked", on_helix_plugin_uninstall_clicked},
        {"on_helix_macros_install_clicked", on_helix_macros_install_clicked},
        {"on_helix_macros_update_clicked", on_helix_macros_update_clicked},
        {"on_pid_tuning_clicked", on_pid_tuning_clicked},
        {"on_timelapse_videos_clicked", on_timelapse_videos_clicked},
        {"on_timelapse_setup_clicked", on_timelapse_setup_clicked},
        {"on_advanced_power_clicked", on_advanced_power_clicked},
    });

    // Note: Input shaping uses on_input_shaper_row_clicked registered by InputShaperPanel
    // Note: Restart row doesn't exist - restart buttons have their own callbacks in
    // ui_emergency_stop.cpp

    subjects_initialized_ = true;
    spdlog::trace("[{}] Event callbacks registered", get_name());
}

void AdvancedPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Event handlers are now declaratively bound via XML event_cb elements
    // No imperative lv_obj_add_event_cb() calls needed

    // The print-active watcher outlives panel navigation (this C++ object is
    // app-lifetime), so a restart queued mid-print still finds its moment
    // after the user has left the panel.
    wire_macro_restart_observer();

#if defined(HELIX_PLATFORM_ESP32)
    // v1 Core+AMS cut has no camera → no timelapse. Force-hide the Timelapse
    // Videos row (its open handler is a no-op stub on this build). Imperative
    // rather than a stacked bind_flag because the row already carries a
    // printer_has_timelapse binding and this helix-xml build has no
    // compound-condition (subject_expr/cond) support. Compiled out on desktop →
    // zero desktop impact.
    if (auto* timelapse_row =
            helix::ui::find_required(panel_, "row_timelapse_videos", get_name())) {
        // DECLARATIVE_OK: compile-time capability (#if), no runtime subject exists
        lv_obj_add_flag(timelapse_row, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    spdlog::debug("[{}] Setup complete", get_name());
}

void AdvancedPanel::on_activate() {
    spdlog::debug("[{}] Activated", get_name());
    // Note: Plugin detection now happens automatically in discovery flow (application.cpp)
}

// ============================================================================
// NAVIGATION HANDLERS
// ============================================================================

void AdvancedPanel::handle_spoolman_clicked() {
    get_global_spoolman_panel().show(parent_screen_);
}

void AdvancedPanel::handle_macros_clicked() {
    get_global_macros_panel().show(parent_screen_);
}

void AdvancedPanel::handle_console_clicked() {
    get_global_console_panel().show(parent_screen_);
}

void AdvancedPanel::handle_history_clicked() {
    get_global_history_dashboard_panel().show(parent_screen_);
}

void AdvancedPanel::handle_pid_tuning_clicked() {
    spdlog::debug("[{}] PID Tuning clicked - opening calibration panel", get_name());

#if defined(HELIX_PLATFORM_ESP32)
    // PID calibration is excluded from the v1 Core+AMS cut; get_global_pid_cal_panel()
    // returns a null-vtable link stub, so init_subjects() below would LoadProhibited.
    helix::ui::show_feature_unavailable_toast();
    return;
#endif

    auto& overlay = get_global_pid_cal_panel();
    overlay.set_api(get_moonraker_api());
    overlay.show(parent_screen_);
}

// ============================================================================
// STATIC EVENT CALLBACKS (registered via lv_xml_register_event_cb)
// ============================================================================

void AdvancedPanel::on_spoolman_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_spoolman_clicked();
}

void AdvancedPanel::on_macros_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_macros_clicked();
}

void AdvancedPanel::on_console_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_console_clicked();
}

void AdvancedPanel::on_history_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_history_clicked();
}

void AdvancedPanel::on_helix_plugin_install_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_helix_plugin_install_clicked();
}

void AdvancedPanel::on_helix_plugin_uninstall_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_helix_plugin_uninstall_clicked();
}

void AdvancedPanel::on_helix_macros_install_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_helix_macros_install_clicked();
}

void AdvancedPanel::on_helix_macros_update_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_helix_macros_update_clicked();
}

void AdvancedPanel::on_pid_tuning_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_pid_tuning_clicked();
}

void AdvancedPanel::on_timelapse_videos_clicked(lv_event_t* /*e*/) {
    spdlog::debug("[AdvancedPanel] Timelapse Videos clicked");
    open_timelapse_videos();
}

void AdvancedPanel::on_timelapse_setup_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_timelapse_setup_clicked();
}

void AdvancedPanel::on_advanced_power_clicked(lv_event_t* /*e*/) {
    get_global_advanced_panel().handle_power_clicked();
}

// ============================================================================
// POWER HANDLER — shares the same dialog the home-panel power widget uses
// ============================================================================

void AdvancedPanel::handle_power_clicked() {
    spdlog::debug("[{}] Power row clicked — opening shutdown dialog", get_name());
    helix::show_shutdown_dialog(api_, shutdown_modal_, object_lifetime_, lv_screen_active());
}

// ============================================================================
// TIMELAPSE SETUP HANDLER
// ============================================================================

void AdvancedPanel::handle_timelapse_setup_clicked() {
    spdlog::info("[{}] Timelapse setup clicked", get_name());
    open_timelapse_install();
}

// ============================================================================
// HELIX HELPER MACRO HANDLERS (helix_macros.cfg)
// ============================================================================

bool AdvancedPanel::macro_job_holds_machine() const {
    // job_holds_machine, not print_active: a host-side Preparing job has
    // print_active == 0 while the toolhead moves, and restarting Klipper
    // through it kills a job the app has already committed to.
    return lv_subject_get_int(printer_state_.print_state().get_job_holds_machine_subject()) != 0;
}

void AdvancedPanel::handle_helix_macros_install_clicked() {
    spdlog::debug("[{}] Helper macros install clicked", get_name());

#if defined(HELIX_PLATFORM_ESP32)
    // Helper-macro install/update is excluded from the v1 Core+AMS cut;
    // MacroManager is a link stub on this build.
    helix::ui::show_feature_unavailable_toast();
    return;
#endif

    if (!api_) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Not connected to printer"),
                                      2000);
        return;
    }

    const int status =
        lv_subject_get_int(printer_state_.plugin_status_state().get_helix_macros_status_subject());
    if (status == static_cast<int>(HelixMacrosStatus::Unknown)) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Printer status unknown"), 2000);
        return;
    }
    if (status == static_cast<int>(HelixMacrosStatus::RestartPending)) {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      lv_tr("Macros installed - restart pending"), 3000);
        return;
    }
    if (status != static_cast<int>(HelixMacrosStatus::NotInstalled)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Helper macros already installed"),
                                      2000);
        return;
    }

    helix::ui::ConfirmOptions opts;
    opts.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr("Install Helper Macros?"),
        macro_job_holds_machine()
            ? lv_tr("This installs the HelixScreen helper macros on the printer and adds them "
                    "to printer.cfg. A print is running, so Klipper restarts after it finishes.")
            : lv_tr("This installs the HelixScreen helper macros on the printer, adds them to "
                    "printer.cfg, and restarts Klipper."),
        ModalSeverity::Warning, lv_tr("Install"), [this]() { run_helix_macros_stage(false); },
        opts);
}

void AdvancedPanel::handle_helix_macros_update_clicked() {
    spdlog::debug("[{}] Helper macros update clicked", get_name());

#if defined(HELIX_PLATFORM_ESP32)
    // Helper-macro install/update is excluded from the v1 Core+AMS cut;
    // MacroManager is a link stub on this build.
    helix::ui::show_feature_unavailable_toast();
    return;
#endif

    if (!api_) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Not connected to printer"),
                                      2000);
        return;
    }

    const int status =
        lv_subject_get_int(printer_state_.plugin_status_state().get_helix_macros_status_subject());
    if (status != static_cast<int>(HelixMacrosStatus::Outdated)) {
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Helper macros are up to date"),
                                      2000);
        return;
    }

    helix::ui::ConfirmOptions opts;
    opts.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr("Update Helper Macros?"),
        macro_job_holds_machine()
            ? lv_tr("This replaces the printer's helper macros with the current version. A print "
                    "is running, so Klipper restarts after it finishes.")
            : lv_tr("This replaces the printer's helper macros with the current version and "
                    "restarts Klipper."),
        ModalSeverity::Warning, lv_tr("Update"), [this]() { run_helix_macros_stage(true); }, opts);
}

void AdvancedPanel::run_helix_macros_stage(bool update) {
    if (!api_) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Not connected to printer"),
                                      2000);
        return;
    }

    macro_manager_ = std::make_unique<helix::MacroManager>(*api_, printer_state_.get_discovery());

    auto on_staged = [this, update]() {
        spdlog::info("[{}] Helper macro files staged (update={})", get_name(), update);
        if (restart_helix_macros_when_idle()) {
            return; // restart fired; its callbacks report the outcome
        }
        // A print is active: queue the restart for the print-complete offer.
        // The staged files are safe wherever they sit and activate at
        // whatever Klipper restart happens next, organic or offered.
        printer_state_.plugin_status_state().set_helix_macros_restart_pending(true);
        macro_restart_offer_made_ = false;
        ToastManager::instance().show(
            ToastSeverity::SUCCESS,
            update ? lv_tr("Macros updated. Restart Klipper after the print to activate them.")
                   : lv_tr("Macros installed. Restart Klipper after the print to activate "
                           "them."),
            4000);
    };
    auto on_error = [this](const MoonrakerError& err) {
        spdlog::error("[{}] Helper macro staging failed: {}", get_name(), err.message);
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Failed to install helper macros"), 4000);
    };

    if (update) {
        macro_manager_->update_files(on_staged, on_error);
    } else {
        macro_manager_->install_files(on_staged, on_error);
    }
}

bool AdvancedPanel::restart_helix_macros_when_idle() {
    // Never restart during an active print; hard-refuse, the caller queues.
    if (macro_job_holds_machine() || !macro_manager_) {
        return false;
    }

    macro_manager_->request_restart(
        [this]() {
            spdlog::info("[{}] Klipper restart accepted; helper macros activating", get_name());
            ToastManager::instance().show(ToastSeverity::SUCCESS,
                                          lv_tr("Klipper restarting - macros activating"), 3000);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Klipper restart request failed: {}", get_name(), err.message);
            // The files are staged but unactivated: keep offering the restart.
            printer_state_.plugin_status_state().set_helix_macros_restart_pending(true);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Failed to restart Klipper"),
                                          4000);
        });
    return true;
}

void AdvancedPanel::offer_helix_macros_restart() {
    spdlog::info("[{}] Offering the deferred Klipper restart for staged helper macros", get_name());

    helix::ui::ConfirmOptions opts;
    opts.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr("Restart Klipper?"),
        lv_tr("The helper macros are installed and activate after Klipper restarts."),
        ModalSeverity::Warning, lv_tr("Restart Now"),
        [this]() {
            if (!restart_helix_macros_when_idle()) {
                // A print started between the offer and this tap. The pending
                // state keeps telling the truth in the row; declining here is
                // a refusal to act, not a loss of the staged files.
                ToastManager::instance().show(ToastSeverity::INFO,
                                              lv_tr("A print is in progress - restart still "
                                                    "pending"),
                                              3000);
            }
        },
        opts);
}

void AdvancedPanel::wire_macro_restart_observer() {
    if (macro_observer_wired_) {
        return;
    }

    macro_job_observer_ = helix::ui::observe<int>(
        printer_state_.print_state().get_job_holds_machine_subject(), this,
        [](AdvancedPanel* self, int holds) {
            // The machine went idle by the same predicate the restart guard
            // uses, so the offer can never pop into a Preparing window.
            if (holds != 0) {
                return;
            }
            const int status = lv_subject_get_int(
                self->printer_state_.plugin_status_state().get_helix_macros_status_subject());
            if (status != static_cast<int>(HelixMacrosStatus::RestartPending)) {
                // A restart landed and activated the macros: re-arm so the
                // next staging gets its own offer.
                self->macro_restart_offer_made_ = false;
                return;
            }
            if (self->macro_restart_offer_made_) {
                return; // one offer per staging
            }
            self->macro_restart_offer_made_ = true;
            self->offer_helix_macros_restart();
        },
        printer_state_.get_subjects_lifetime());
    macro_observer_wired_ = true;
    spdlog::debug("[{}] Macro restart observer wired", get_name());
}

// ============================================================================
// HELIXPRINT PLUGIN HANDLERS
// ============================================================================

void AdvancedPanel::handle_helix_plugin_install_clicked() {
    spdlog::debug("[{}] HelixPrint Plugin Install clicked", get_name());

    // Double-check plugin isn't already installed (defensive)
    if (printer_state_.plugin_status_state().service_has_helix_plugin()) {
        spdlog::info("[{}] Plugin already installed", get_name());
        ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Plugin already installed"), 2000);
        return;
    }

    // Update installer's websocket URL for local/remote detection
    if (api_) {
        plugin_installer_.set_websocket_url(api_->get_websocket_url());
    }

    // Show the install modal
    plugin_install_modal_.set_installer(&plugin_installer_);
    plugin_install_modal_.set_on_install_complete([this](bool success) {
        if (success) {
            printer_state_.set_helix_plugin_installed(true);
            ToastManager::instance().show(ToastSeverity::SUCCESS,
                                          lv_tr("Plugin installed successfully"), 2000);
        }
    });
    plugin_install_modal_.show(lv_screen_active());
}

void AdvancedPanel::handle_helix_plugin_uninstall_clicked() {
    spdlog::debug("[{}] HelixPrint Plugin Uninstall clicked", get_name());

    helix::ui::ConfirmOptions opts;
    opts.owner_token = object_lifetime_.token();
    helix::ui::modal_confirm(
        lv_tr("Uninstall HelixPrint Plugin?"),
        lv_tr("This removes the plugin's Moonraker component and restarts Moonraker. The printer "
              "will briefly disconnect. If old phase-tracking lines are still in PRINT_START, "
              "they are removed too, with backups kept. A Klipper restart applies that."),
        ModalSeverity::Error, lv_tr("Uninstall"), [this]() { run_helix_plugin_uninstall(); }, opts);
}

void AdvancedPanel::run_helix_plugin_uninstall() {
    using UninstallOutcome = helix::UninstallOutcome;

    // Update installer's websocket URL for local/remote detection
    if (api_) {
        plugin_installer_.set_websocket_url(api_->get_websocket_url());
    }

    auto on_done = [this](UninstallOutcome outcome, const std::string& message) {
        const char* outcome_name = outcome == UninstallOutcome::SUCCESS ? "succeeded"
                                   : outcome == UninstallOutcome::NEEDS_ATTENTION
                                       ? "needs attention"
                                       : "failed";
        spdlog::info("[{}] Plugin uninstall {}: {}", get_name(), outcome_name, message);

        // The plugin itself is gone in both SUCCESS and NEEDS_ATTENTION - only
        // a FAILED uninstall leaves it installed.
        if (outcome != UninstallOutcome::FAILED) {
            printer_state_.set_helix_plugin_installed(false);
        }

        switch (outcome) {
        case UninstallOutcome::SUCCESS:
            ToastManager::instance().show(ToastSeverity::SUCCESS, message.c_str(), 2000);
            break;
        case UninstallOutcome::NEEDS_ATTENTION:
            ToastManager::instance().show(ToastSeverity::WARNING, message.c_str(), 5000);
            break;
        case UninstallOutcome::FAILED:
            ToastManager::instance().show(ToastSeverity::ERROR, message.c_str(), 4000);
            break;
        }
    };

    // The installer forks the bundled script and waits for it right here, on the
    // LVGL thread — the same blocking call the install modal makes, and for the
    // same reason (see PluginInstallModal::on_install_clicked).
    if (uninstall_runner_) {
        uninstall_runner_(on_done);
    } else {
        plugin_installer_.uninstall_local(on_done);
    }
}
