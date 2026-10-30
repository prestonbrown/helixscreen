// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_system.h"

#include "ui_callback_helpers.h"
#include "ui_nav.h"
#include "ui_overlay_performance.h"
#include "ui_panel_common.h"
#include "ui_settings_security.h"
#include "ui_settings_storage.h"
#if !defined(HELIX_PLATFORM_ESP32)
#include "ui_settings_telemetry_data.h"
#endif
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl.h"
#include "system_settings_manager.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

SystemSettingsOverlay::~SystemSettingsOverlay() {
    if (lv_is_initialized() && factory_reset_dialog_) {
        // Unregister so the close callback cannot fire on a dead 'this'.
        helix::nav::clear_on_close(factory_reset_dialog_);
    }
}

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
#endif
        {"on_telemetry_changed",
         [](lv_event_t* e) {
             bool on = helix::ui::event_checked(e);
             SystemSettingsManager::instance().set_telemetry_enabled(on);
             if (on) {
                 ToastManager::instance().show(
                     ToastSeverity::SUCCESS,
                     lv_tr("Thanks! TOTALLY anonymous usage data helps improve HelixScreen."),
                     4000);
             }
         }},
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
        {"on_factory_reset_confirm",
         [](lv_event_t*) { get_system_settings_overlay().perform_factory_reset(); }},
        {"on_factory_reset_cancel",
         [](lv_event_t*) {
             if (get_system_settings_overlay().factory_reset_dialog_) {
                 helix::nav::go_back(); // Animation + callback clean up
             }
         }},
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

    // Create dialog on first use (lazy initialization)
    if (!factory_reset_dialog_ && parent_screen_) {
        spdlog::debug("[{}] Creating factory reset dialog...", get_name());

        // Create self-contained factory_reset_modal component
        // Callbacks are already wired via XML event_cb elements
        factory_reset_dialog_ = helix::ui::create_xml_hidden(parent_screen_, "factory_reset_modal");

        if (factory_reset_dialog_) {
            // Register as a function-based (nullptr-lifecycle) overlay so
            // crash crumbs show "anon" instead of "unreg".
            helix::nav::register_overlay(factory_reset_dialog_, nullptr);

            // Register close callback to delete dialog when animation completes.
            // Must use safe_delete_deferred — this lambda runs inside
            // UpdateQueue::process_pending(), and synchronous deletion
            // during a batch corrupts LVGL's event linked list (#356, #491).
            helix::nav::on_close(factory_reset_dialog_, [this]() {
                helix::ui::safe_delete_deferred(factory_reset_dialog_);
            });

            spdlog::info("[{}] Factory reset dialog created", get_name());
        } else {
            spdlog::error("[{}] Failed to create factory reset dialog", get_name());
            return;
        }
    }

    // Show the dialog via navigation stack
    if (factory_reset_dialog_) {
        helix::nav::push_overlay(factory_reset_dialog_);
    }
}

void SystemSettingsOverlay::perform_factory_reset() {
    spdlog::warn("[{}] Performing factory reset - resetting config!", get_name());

    // Get config instance and reset
    Config* config = Config::get_instance();
    config->reset_to_defaults();
    config->save();
    spdlog::info("[{}] Config reset to defaults", get_name());

    // Hide the dialog - animation + callback will handle cleanup
    if (factory_reset_dialog_) {
        helix::nav::go_back();
    }

    // Show confirmation toast and restart
    ToastManager::instance().show(ToastSeverity::INFO,
                                  lv_tr("Settings reset to defaults. Restarting..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::queue_update("SystemSettingsOverlay::factory_reset_restart", []() {
        spdlog::info("[SystemSettingsOverlay] Restarting after factory reset...");
        app_request_restart_service();
    });
}

} // namespace helix::settings
