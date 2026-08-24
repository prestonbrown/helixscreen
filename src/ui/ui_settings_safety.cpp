// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_safety.cpp
 * @brief Implementation of SafetySettingsOverlay
 */

#include "ui_settings_safety.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "audio_settings_manager.h"
#include "post_op_cooldown_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SafetySettingsOverlay> g_safety_settings_overlay;

SafetySettingsOverlay& get_safety_settings_overlay() {
    if (!g_safety_settings_overlay) {
        g_safety_settings_overlay = std::make_unique<SafetySettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "SafetySettingsOverlay", []() { g_safety_settings_overlay.reset(); });
    }
    return *g_safety_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SafetySettingsOverlay::SafetySettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SafetySettingsOverlay::~SafetySettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SafetySettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Subjects for cancel escalation are owned by SafetySettingsManager
    // (settings_cancel_escalation_enabled, settings_cancel_escalation_timeout)

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SafetySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_estop_confirm_changed", on_estop_confirm_changed},
        {"on_cancel_escalation_changed", on_cancel_escalation_changed},
        {"on_cancel_escalation_timeout_changed", on_cancel_escalation_timeout_changed},
        {"on_completion_alert_changed", on_completion_alert_changed},
        {"on_min_toast_severity_changed", on_min_toast_severity_changed},
        {"on_macro_confirm_changed", on_macro_confirm_changed},
        {"on_allow_cold_extrude_changed", on_allow_cold_extrude_changed},
        {"on_filament_auto_cooldown_changed", on_filament_auto_cooldown_changed},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SafetySettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_safety_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SafetySettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Register for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack (on_activate will initialize widgets)
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void SafetySettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_estop_toggle();
    init_completion_alert_dropdown();
}

void SafetySettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// WIDGET INITIALIZATION
// ============================================================================

void SafetySettingsOverlay::init_estop_toggle() {
    auto& safety_settings = SafetySettingsManager::instance();

    lv_obj_t* estop_row = lv_obj_find_by_name(overlay_root_, "row_estop_confirm");
    if (estop_row) {
        lv_obj_t* toggle = lv_obj_find_by_name(estop_row, "toggle");
        if (toggle) {
            if (safety_settings.get_estop_require_confirmation()) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}] E-Stop confirmation toggle initialized", get_name());
        }
    }
}

void SafetySettingsOverlay::init_completion_alert_dropdown() {
    lv_obj_t* completion_row = lv_obj_find_by_name(overlay_root_, "row_completion_alert");
    if (completion_row) {
        lv_obj_t* dropdown = lv_obj_find_by_name(completion_row, "dropdown");
        if (dropdown) {
            auto mode = AudioSettingsManager::instance().get_completion_alert_mode();
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(mode));
            spdlog::trace("[{}] Completion alert dropdown initialized (mode={})", get_name(),
                          static_cast<int>(mode));
        }
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SafetySettingsOverlay::handle_estop_confirm_changed(bool enabled) {
    spdlog::info("[{}] E-Stop confirmation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_estop_require_confirmation(enabled);
    EmergencyStopOverlay::instance().set_require_confirmation(enabled);
}

void SafetySettingsOverlay::handle_cancel_escalation_changed(bool enabled) {
    spdlog::info("[{}] Cancel escalation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_cancel_escalation_enabled(enabled);
}

void SafetySettingsOverlay::handle_cancel_escalation_timeout_changed(int index) {
    static constexpr int TIMEOUT_VALUES[] = {15, 30, 60, 120};
    int seconds = TIMEOUT_VALUES[std::max(0, std::min(3, index))];
    spdlog::info("[{}] Cancel escalation timeout changed: {}s (index {})", get_name(), seconds,
                 index);
    SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(seconds);
}

void SafetySettingsOverlay::handle_completion_alert_changed(int index) {
    auto mode = static_cast<CompletionAlertMode>(index);
    spdlog::info("[{}] Completion alert changed: {} ({})", get_name(), index,
                 index == 0 ? "Off" : (index == 1 ? "Notification" : "Alert"));
    AudioSettingsManager::instance().set_completion_alert_mode(mode);
}

void SafetySettingsOverlay::handle_min_toast_severity_changed(int index) {
    spdlog::info("[{}] Min toast severity changed: {} ({})", get_name(), index,
                 index == 0 ? "All" : (index == 1 ? "Warnings & errors" : "Errors only"));
    SafetySettingsManager::instance().set_min_toast_severity(index);
}

void SafetySettingsOverlay::handle_macro_confirm_changed(bool enabled) {
    spdlog::info("[{}] Macro run confirmation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_macro_require_confirmation(enabled);
}

void SafetySettingsOverlay::handle_allow_cold_extrude_changed(bool enabled) {
    spdlog::info("[{}] Allow cold load/unload toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_allow_cold_extrude(enabled);
}

void SafetySettingsOverlay::handle_filament_auto_cooldown_changed(bool enabled) {
    spdlog::info("[{}] Post-op nozzle cooldown toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_filament_auto_cooldown(enabled);
    // Turning it off mid-countdown should take effect now, not in two minutes.
    if (!enabled) {
        PostOpCooldownManager::instance().cancel();
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SafetySettingsOverlay::on_estop_confirm_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_estop_confirm_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_safety_settings_overlay().handle_estop_confirm_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_cancel_escalation_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_cancel_escalation_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_safety_settings_overlay().handle_cancel_escalation_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_cancel_escalation_timeout_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_cancel_escalation_timeout_changed");
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_safety_settings_overlay().handle_cancel_escalation_timeout_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_completion_alert_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_completion_alert_changed");
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_safety_settings_overlay().handle_completion_alert_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_min_toast_severity_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_min_toast_severity_changed");
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_safety_settings_overlay().handle_min_toast_severity_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_macro_confirm_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_macro_confirm_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_safety_settings_overlay().handle_macro_confirm_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_allow_cold_extrude_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_allow_cold_extrude_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_safety_settings_overlay().handle_allow_cold_extrude_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SafetySettingsOverlay::on_filament_auto_cooldown_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SafetySettingsOverlay] on_filament_auto_cooldown_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_safety_settings_overlay().handle_filament_auto_cooldown_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
