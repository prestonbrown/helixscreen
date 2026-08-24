// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_hardware.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_power.h"
#include "ui_printer_list_overlay.h"
#include "ui_settings_fans.h"
#include "ui_settings_led.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_sensors.h"
#include "ui_spoolman_overlay.h"

#include "app_globals.h"
#include "i_moonraker_api.h"
#include "static_panel_registry.h"

#if HELIX_HAS_CAMERA
// Defined in src/ui/panel_widgets/camera_widget.cpp; that directory is not on
// the include path, so forward-declare rather than including the header.
namespace helix {
void open_standalone_camera_fullscreen(lv_obj_t* parent_screen);
}
#endif

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<HardwareSettingsOverlay> g_hardware_settings_overlay;

HardwareSettingsOverlay& get_hardware_settings_overlay() {
    if (!g_hardware_settings_overlay) {
        g_hardware_settings_overlay = std::make_unique<HardwareSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "HardwareSettingsOverlay", []() { g_hardware_settings_overlay.reset(); });
    }
    return *g_hardware_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

HardwareSettingsOverlay::HardwareSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

HardwareSettingsOverlay::~HardwareSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void HardwareSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // No overlay-specific subjects needed; all conditional visibility
    // is driven by subjects registered elsewhere (ams_slot_count,
    // fans_version, filament_sensor_count, printer_has_led, power_device_count).
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void HardwareSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_printers_clicked", on_printers_clicked},
        {"on_camera_view_clicked", on_camera_view_clicked},
        {"on_ams_settings_clicked", on_ams_settings_clicked},
        {"on_fans_settings_clicked", on_fans_settings_clicked},
        {"on_filament_sensors_clicked", on_filament_sensors_clicked},
        {"on_led_settings_clicked", on_led_settings_clicked},
        {"on_power_devices_clicked", on_power_devices_clicked},
        {"on_spoolman_settings_clicked", on_spoolman_settings_clicked},
        {"on_macro_buttons_clicked", on_macro_buttons_clicked},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* HardwareSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_hardware_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void HardwareSettingsOverlay::show(lv_obj_t* parent_screen) {
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

void HardwareSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    // All rows are action rows with no local state to initialize
}

void HardwareSettingsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void HardwareSettingsOverlay::on_printers_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_printers_clicked");
    auto& overlay = helix::ui::get_printer_list_overlay();
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_camera_view_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_camera_view_clicked");
#if HELIX_HAS_CAMERA
    helix::open_standalone_camera_fullscreen(get_hardware_settings_overlay().parent_screen_);
#else
    spdlog::debug("[HardwareSettingsOverlay] Camera support disabled in this build");
#endif
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_ams_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_ams_settings_clicked");
    auto& overlay = helix::ui::get_ams_device_operations_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_fans_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_fans_settings_clicked");
    auto& overlay = helix::settings::get_fan_settings_overlay();
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_filament_sensors_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_filament_sensors_clicked");
    auto& overlay = helix::settings::get_sensor_settings_overlay();
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_led_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_led_settings_clicked");
    auto& overlay = helix::settings::get_led_settings_overlay();
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_power_devices_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_power_devices_clicked");
    auto& panel = get_global_power_panel();
    lv_obj_t* overlay = panel.get_or_create_overlay(get_hardware_settings_overlay().parent_screen_);
    if (overlay) {
        NavigationManager::instance().push_overlay(overlay);
    } else {
        spdlog::error("[HardwareSettingsOverlay] Failed to open Power panel");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_spoolman_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_spoolman_settings_clicked");
    auto& overlay = helix::ui::get_spoolman_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        overlay.set_api(api);
    }
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void HardwareSettingsOverlay::on_macro_buttons_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HardwareSettingsOverlay] on_macro_buttons_clicked");
    auto& overlay = helix::settings::get_macro_buttons_overlay();
    overlay.show(get_hardware_settings_overlay().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
