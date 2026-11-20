// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_system.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_settings.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "config.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"
#include "system_power.h"
#include "system_settings_manager.h"
#include "u1_stock_screen.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SystemSettingsOverlay> g_system_settings_overlay;

SystemSettingsOverlay& get_system_settings_overlay() {
    if (!g_system_settings_overlay) {
        g_system_settings_overlay = std::make_unique<SystemSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "SystemSettingsOverlay", []() { g_system_settings_overlay.reset(); });
    }
    return *g_system_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SystemSettingsOverlay::SystemSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SystemSettingsOverlay::~SystemSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SystemSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // All subjects used by the system overlay XML are already registered
    // by SettingsPanel (show_network_settings, show_touch_calibration,
    // hardware_has_issues, settings_telemetry_enabled, etc.)
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SystemSettingsOverlay::register_callbacks() {
    // The other callbacks in settings_system_overlay.xml are registered
    // globally by SettingsPanel in init_subjects() and
    // register_settings_panel_callbacks(); only the U1 restore row is ours.
    register_xml_callbacks({
        {"on_restore_stock_screen_clicked",
         [](lv_event_t*) { get_system_settings_overlay().handle_restore_stock_screen_clicked(); }},
    });
    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SystemSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_system_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SystemSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void SystemSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_telemetry_toggle();
    init_touch_cal_description();
    init_host_description();
    init_log_level_dropdown();
}

void SystemSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// INTERNAL INITIALIZATION
// ============================================================================

void SystemSettingsOverlay::init_telemetry_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* telemetry_row = lv_obj_find_by_name(overlay_root_, "row_telemetry");
    if (telemetry_row) {
        lv_obj_t* toggle = lv_obj_find_by_name(telemetry_row, "toggle");
        if (toggle) {
            if (SystemSettingsManager::instance().get_telemetry_enabled()) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}] Telemetry toggle initialized", get_name());
        }
    }
}

void SystemSettingsOverlay::init_touch_cal_description() {
    if (!overlay_root_)
        return;

    // Touch calibration row description is already bound to the
    // touch_cal_status subject via SettingsPanel's setup_action_handlers().
    // For the system overlay, we bind it here if the row exists.
    lv_obj_t* touch_cal_row = lv_obj_find_by_name(overlay_root_, "row_touch_calibration");
    if (touch_cal_row) {
        spdlog::trace("[{}] Touch calibration row present", get_name());
    }
}

void SystemSettingsOverlay::init_host_description() {
    if (!overlay_root_)
        return;

    // Host row description is already bound to printer_host_value subject
    // via SettingsPanel's populate_info_rows(). The subject updates reactively.
    lv_obj_t* host_row = lv_obj_find_by_name(overlay_root_, "row_printer_host");
    if (host_row) {
        spdlog::trace("[{}] Host row present", get_name());
    }
}

void SystemSettingsOverlay::init_log_level_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_log_level");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        // Options are populated + translated by the XML options_tag on row_log_level.
        int index = SystemSettingsManager::instance().get_log_level_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Log level dropdown initialized (index={})", get_name(), index);
    }
}

void SystemSettingsOverlay::handle_restore_stock_screen_clicked() {
    helix::ui::modal_confirm(
        lv_tr("Restore Stock Screen"),
        lv_tr("The printer restarts with the Snapmaker screen, and HelixScreen no longer starts. "
              "Your HelixScreen settings stay on the printer; run the installer again to come "
              "back."),
        ModalSeverity::Warning, lv_tr("Restore"),
        []() { get_system_settings_overlay().perform_restore_stock_screen(); });
}

void SystemSettingsOverlay::perform_restore_stock_screen() {
    const char* path = std::getenv(helix::u1::kRestoreTriggerEnv);
    if (!helix::u1::restore_stock_screen_offered(path) || !helix::u1::write_restore_trigger(path)) {
        ToastManager::instance().show(ToastSeverity::ERROR,
                                      lv_tr("Could not restore the stock screen"), 4000);
        return;
    }
    // The platform hook restores the stock screen at the next start, as root,
    // and reboots again into it.
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Restarting the printer..."), 1500);
    helix::ui::queue_update("SystemSettingsOverlay::restore_stock_reboot", []() {
        if (!SystemPower::reboot_local()) {
            ToastManager::instance().show(
                ToastSeverity::WARNING,
                lv_tr("Restart the printer to finish restoring the stock screen"), 6000);
        }
    });
}

} // namespace helix::settings
