// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_system.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav.h"
#include "ui_overlay_performance.h"
#include "ui_panel_common.h"
#include "ui_settings_security.h"
#include "ui_settings_storage.h"
#if !defined(HELIX_PLATFORM_ESP32)
#include "ui_settings_telemetry_data.h"
#include "ui_telemetry_info_modal.h"
#include "ui_wizard_telemetry.h"
#endif
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "lvgl.h"
#include "src/ui/panel_widgets/shutdown_widget.h"
#include "system_power.h"
#include "system_settings_manager.h"
#include "u1_stock_screen.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace helix::settings {

void SystemSettingsOverlay::register_callbacks() {
    using helix::ui::event_selected;
    register_xml_callbacks({
        {"on_security_clicked",
         [](lv_event_t*) {
             get_security_settings_overlay().show(get_system_settings_overlay().parent_screen_);
         }},
#if !defined(HELIX_PLATFORM_ESP32)
        // The firmware has no telemetry queue to show, and its row is hidden.
        {"on_telemetry_view_data",
         [](lv_event_t*) {
             get_telemetry_data_overlay().show(get_system_settings_overlay().parent_screen_);
         }},
#else
        {"on_telemetry_view_data", [](lv_event_t*) {}},
#endif
        // telemetry_opt_in_row's callbacks, for this page and the setup wizard alike.
        {"on_telemetry_changed",
         [](lv_event_t* e) {
             bool on = helix::ui::event_checked(e);
             SystemSettingsManager::instance().set_telemetry_enabled(on);
             if (on) {
                 ToastManager::instance().show(
                     ToastSeverity::SUCCESS,
                     lv_tr("Thanks! Anonymous usage data helps improve HelixScreen."), 4000);
             }
         }},
#if !defined(HELIX_PLATFORM_ESP32)
        {"on_telemetry_info",
         [](lv_event_t*) {
             get_wizard_telemetry_step()->init_info_text_subject();
             Modal::show_owned(std::make_unique<TelemetryInfoModal>(), lv_screen_active());
         }},
#else
        // The firmware sends no telemetry and hides the row.
        {"on_telemetry_info", [](lv_event_t*) {}},
#endif
        {"on_log_level_changed",
         [](lv_event_t* e) {
             SystemSettingsManager::instance().set_log_level_by_index(event_selected(e));
         }},
        {"on_storage_clicked",
         [](lv_event_t*) {
             get_storage_settings_overlay().show(get_system_settings_overlay().parent_screen_);
         }},
        {"on_system_performance_clicked",
         [](lv_event_t*) { get_system_settings_overlay().open_performance(); }},
        {"on_restart_helix_settings_clicked",
         [](lv_event_t*) { get_system_settings_overlay().handle_restart_helix_clicked(); }},
        {"on_factory_reset_clicked",
         [](lv_event_t*) { get_system_settings_overlay().handle_factory_reset_clicked(); }},
        {"on_restore_stock_screen_clicked",
         [](lv_event_t*) { get_system_settings_overlay().handle_restore_stock_screen_clicked(); }},
    });
}

void SystemSettingsOverlay::open_performance() {
    spdlog::debug("[{}] Performance clicked - opening overlay", get_name());

    auto* overlay = helix::ui::UiOverlayPerformance::instance().create(lv_screen_active());
    if (!overlay) {
        spdlog::error("[{}] Failed to create Performance overlay", get_name());
        return;
    }

    // UiOverlayPerformance carries no IPanelLifecycle, so it registers with a null
    // lifecycle: that is what separates an intentional lifecycle-less overlay from a
    // caller who forgot to register. Without it the push is recorded as "unreg" in
    // panel telemetry and crash breadcrumbs, and strict mode aborts.
    helix::nav::register_overlay(overlay, nullptr);
    helix::nav::push_overlay(overlay);
}

void SystemSettingsOverlay::handle_restart_helix_clicked() {
    spdlog::info("[{}] Restart HelixScreen requested", get_name());
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Restarting HelixScreen..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::queue_update("SystemSettingsOverlay::restart", []() {
        spdlog::info("[SystemSettingsOverlay] Initiating restart...");
        app_request_restart_service();
    });
}

void SystemSettingsOverlay::handle_factory_reset_clicked() {
    spdlog::debug("[{}] Factory Reset clicked - showing confirmation dialog", get_name());
    helix::ui::ConfirmOptions options;
    options.destructive = true;
    helix::ui::modal_confirm(
        lv_tr("Factory Reset"),
        lv_tr("This will reset all settings to defaults. This action cannot be undone."),
        ModalSeverity::Error, lv_tr("Reset"),
        []() { get_system_settings_overlay().perform_factory_reset(); }, options);
}

void SystemSettingsOverlay::perform_factory_reset() {
    spdlog::warn("[{}] Performing factory reset - resetting config!", get_name());

    // Get config instance and reset
    Config* config = Config::get_instance();
    config->reset_to_defaults();
    config->save();
    spdlog::info("[{}] Config reset to defaults", get_name());

    // Show confirmation toast and restart
    ToastManager::instance().show(ToastSeverity::INFO,
                                  lv_tr("Settings reset to defaults. Restarting..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::queue_update("SystemSettingsOverlay::factory_reset_restart", []() {
        spdlog::info("[SystemSettingsOverlay] Restarting after factory reset...");
        app_request_restart_service();
    });
}

namespace {

/// The verdict for a Restore Stock Screen request now, with the refusal toast
/// already shown when it is not Proceed.
helix::u1::RestoreStockVerdict restore_stock_verdict_now() {
    const auto verdict = helix::u1::decide_restore_stock(std::getenv(helix::u1::kRestoreTriggerEnv),
                                                         helix::power_job_active());
    if (verdict == helix::u1::RestoreStockVerdict::JobActive) {
        ToastManager::instance().show(
            ToastSeverity::WARNING, lv_tr("Cannot restore the stock screen while printing"), 4000);
    } else if (verdict == helix::u1::RestoreStockVerdict::NotOffered) {
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Could not restore the stock screen"), 4000);
    }
    return verdict;
}

} // namespace

void SystemSettingsOverlay::handle_restore_stock_screen_clicked() {
    if (restore_stock_verdict_now() != helix::u1::RestoreStockVerdict::Proceed) {
        return;
    }
    helix::ui::ConfirmOptions options;
    options.destructive = true;
    helix::ui::modal_confirm(
        lv_tr("Restore Stock Screen"),
        lv_tr("The printer restarts with the Snapmaker screen, and HelixScreen no longer starts. "
              "Your HelixScreen settings stay on the printer; run the installer again to come "
              "back."),
        ModalSeverity::Warning, lv_tr("Restore"),
        []() { get_system_settings_overlay().perform_restore_stock_screen(); }, options);
}

void SystemSettingsOverlay::perform_restore_stock_screen() {
    if (restore_stock_verdict_now() != helix::u1::RestoreStockVerdict::Proceed) {
        return;
    }
    const std::string path = std::getenv(helix::u1::kRestoreTriggerEnv);
    if (!helix::u1::write_restore_trigger(path)) {
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Could not restore the stock screen"), 4000);
        return;
    }
    // The boot delegate restores the stock screen at the next boot. A trigger
    // left behind by a reboot that never happened would fire at some later,
    // unasked-for boot, so it goes when the reboot does not dispatch.
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Restarting the printer..."), 1500);
    helix::ui::queue_update("SystemSettingsOverlay::restore_stock_reboot", [path]() {
        if (SystemPower::reboot_local() != PowerResult::Dispatched) {
            std::remove(path.c_str());
            ToastManager::instance().show(
                ToastSeverity::ERROR,
                lv_tr("Could not restart the printer; the stock screen was not restored"), 6000);
        }
    });
}

} // namespace helix::settings
