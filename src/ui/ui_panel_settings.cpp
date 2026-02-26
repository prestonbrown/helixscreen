// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_settings.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_ams_spoolman_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_change_host_modal.h"
#include "ui_debug_bundle_modal.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_overlay_network_settings.h"
#include "ui_panel_history_dashboard.h"
#include "ui_panel_memory_stats.h"
#include "ui_panel_power.h"
#include "ui_settings_display.h"
#include "ui_settings_hardware_health.h"
#include "ui_settings_led.h"
#include "ui_settings_machine_limits.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_panel_widgets.h"
#include "ui_settings_plugins.h"
#include "ui_settings_sensors.h"
#include "ui_settings_sound.h"
#include "ui_settings_telemetry_data.h"
#include "ui_severity_card.h"
#include "ui_snake_game.h"
#include "ui_toast_manager.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_wizard_hardware_selector.h"

#include "app_globals.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "device_display_name.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "input_settings_manager.h"
#include "led/led_controller.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_manager.h"
#include "observer_factory.h"
#include "platform_info.h"
#include "printer_hardware.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "sound_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "ui/ui_lazy_panel_helper.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

using namespace helix;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

SettingsPanel::SettingsPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::trace("[{}] Constructor", get_name());
}

SettingsPanel::~SettingsPanel() {
    // Applying [L041]: deinit_subjects() as first line in destructor
    deinit_subjects();

    // Note: Klipper/Moonraker/OS version observers bound declaratively in XML
    if (lv_is_initialized()) {
        // Unregister overlay callbacks to prevent dangling 'this' in callbacks
        auto& nav = NavigationManager::instance();
        if (factory_reset_dialog_) {
            nav.unregister_overlay_close_callback(factory_reset_dialog_);
        }
    }
    // Note: Don't log here - spdlog may be destroyed during static destruction
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

// Static callback for XML event_cb (registered with lv_xml_register_event_cb)
static void on_completion_alert_dropdown_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto mode = static_cast<CompletionAlertMode>(index);
    spdlog::info("[SettingsPanel] Completion alert changed: {} ({})", index,
                 index == 0 ? "Off" : (index == 1 ? "Notification" : "Alert"));
    AudioSettingsManager::instance().set_completion_alert_mode(mode);
}

// Static callback for cancel escalation timeout dropdown
static void on_cancel_escalation_timeout_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    static constexpr int TIMEOUT_VALUES[] = {15, 30, 60, 120};
    int seconds = TIMEOUT_VALUES[std::max(0, std::min(3, index))];
    spdlog::info("[SettingsPanel] Cancel escalation timeout changed: {}s (index {})", seconds,
                 index);
    SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(seconds);
}

// Static callback for display dim dropdown
static void on_display_dim_dropdown_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    int seconds = DisplaySettingsManager::index_to_dim_seconds(index);
    spdlog::info("[SettingsPanel] Display dim changed: index {} = {}s", index, seconds);
    DisplaySettingsManager::instance().set_display_dim_sec(seconds);
}

// Static callback for display sleep dropdown
static void on_display_sleep_dropdown_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    int seconds = DisplaySettingsManager::index_to_sleep_seconds(index);
    spdlog::info("[SettingsPanel] Display sleep changed: index {} = {}s", index, seconds);
    DisplaySettingsManager::instance().set_display_sleep_sec(seconds);
}

// Static callback for bed mesh render mode dropdown
static void on_bed_mesh_mode_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int mode = static_cast<int>(lv_dropdown_get_selected(dropdown));
    spdlog::info("[SettingsPanel] Bed mesh render mode changed: {} ({})", mode,
                 mode == 0 ? "Auto" : (mode == 1 ? "3D" : "2D"));
    DisplaySettingsManager::instance().set_bed_mesh_render_mode(mode);
}

// Static callback for Z movement style dropdown
static void on_z_movement_style_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto style = static_cast<ZMovementStyle>(index);
    spdlog::info("[SettingsPanel] Z movement style changed: {} ({})", index,
                 index == 0 ? "Auto" : (index == 1 ? "Bed Moves" : "Nozzle Moves"));
    SettingsManager::instance().set_z_movement_style(style);
}

// Static callback for G-code render mode dropdown
static void on_gcode_mode_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int mode = static_cast<int>(lv_dropdown_get_selected(dropdown));
    spdlog::info("[SettingsPanel] G-code render mode changed: {} ({})", mode,
                 mode == 0 ? "Auto" : (mode == 1 ? "3D" : "2D Layers"));
    DisplaySettingsManager::instance().set_gcode_render_mode(mode);
}

// Static callback for time format dropdown
static void on_time_format_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto format = static_cast<TimeFormat>(index);
    spdlog::info("[SettingsPanel] Time format changed: {} ({})", index,
                 index == 0 ? "12 Hour" : "24 Hour");
    DisplaySettingsManager::instance().set_time_format(format);
}

// Static callback for language dropdown
static void on_language_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    std::string lang_code = SystemSettingsManager::language_index_to_code(index);
    spdlog::info("[SettingsPanel] Language changed: index {} ({})", index, lang_code);
    SystemSettingsManager::instance().set_language_by_index(index);
}

// Static callback for update channel dropdown
static void on_update_channel_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_update_channel_changed");
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));

    // Dev channel (2) requires dev_url to be configured
    bool rejected = false;
    if (index == 2) {
        auto* config = Config::get_instance();
        std::string dev_url = config ? config->get<std::string>("/update/dev_url", "") : "";
        if (dev_url.empty()) {
            spdlog::warn("[SettingsPanel] Dev channel selected but no dev_url configured");
            // Revert to previous value
            int current = SystemSettingsManager::instance().get_update_channel();
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current));
            ToastManager::instance().show(ToastSeverity::WARNING,
                                          lv_tr("Dev channel requires dev_url in config"), 3000);
            rejected = true;
        }
    }

    if (!rejected) {
        spdlog::info("[SettingsPanel] Update channel changed: {} ({})", index,
                     index == 0 ? "Stable" : (index == 1 ? "Beta" : "Dev"));
        SystemSettingsManager::instance().set_update_channel(index);
    }
    LVGL_SAFE_EVENT_CB_END();
}

// Static callback for version row tap (toggle beta_features via 7-tap secret)
// Like Android's "tap build number 7 times" to enable developer mode
static constexpr int kSecretTapCount = 7;
static constexpr uint32_t kSecretTapTimeoutMs = 2000; // Reset counter after 2s of no taps

static void on_version_clicked(lv_event_t*) {
    static int tap_count = 0;
    static uint32_t last_tap_time = 0;

    uint32_t now = lv_tick_get();

    // Reset counter if too much time has passed
    if (now - last_tap_time > kSecretTapTimeoutMs) {
        tap_count = 0;
    }
    last_tap_time = now;
    tap_count++;

    int remaining = kSecretTapCount - tap_count;

    if (remaining > 0 && remaining <= 3) {
        // Show countdown - say "enable" or "disable" based on current state
        Config* config = Config::get_instance();
        bool currently_on = config && config->is_beta_features_enabled();
        const char* action = currently_on ? lv_tr("disable") : lv_tr("enable");
        std::string msg =
            remaining == 1
                ? fmt::format(lv_tr("1 more tap to {} beta features"), action)
                : fmt::format(lv_tr("{} more taps to {} beta features"), remaining, action);
        ToastManager::instance().show(ToastSeverity::INFO, msg.c_str(), 1000);
    } else if (remaining == 0) {
        // Toggle beta_features config flag and reactive subject
        Config* config = Config::get_instance();
        if (config) {
            bool currently_enabled = config->is_beta_features_enabled();
            bool new_value = !currently_enabled;
            config->set("/beta_features", new_value);
            config->save();

            // Update the reactive subject so UI elements respond immediately
            lv_subject_t* subject = lv_xml_get_subject(nullptr, "show_beta_features");
            if (subject) {
                lv_subject_set_int(subject, new_value ? 1 : 0);
            }

            ToastManager::instance().show(
                ToastSeverity::SUCCESS,
                new_value ? lv_tr("Beta features: ON") : lv_tr("Beta features: OFF"), 1500);
            spdlog::info("[SettingsPanel] Beta features toggled via 7-tap secret: {}",
                         new_value ? "ON" : "OFF");
        }
        tap_count = 0; // Reset for next time
    }
}

// Static callback for printer name row tap — 7-tap snake easter egg
static void on_printer_name_clicked(lv_event_t*) {
    static int tap_count = 0;
    static uint32_t last_tap_time = 0;

    uint32_t now = lv_tick_get();

    if (now - last_tap_time > kSecretTapTimeoutMs) {
        tap_count = 0;
    }
    last_tap_time = now;
    tap_count++;

    int remaining = kSecretTapCount - tap_count;

    if (remaining > 0 && remaining <= 3) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d more tap%s...", remaining, remaining == 1 ? "" : "s");
        ToastManager::instance().show(ToastSeverity::INFO, buf, 800);
    } else if (remaining == 0) {
        tap_count = 0;
        spdlog::info("[SettingsPanel] Snake easter egg triggered!");
        helix::SnakeGame::show();
    }
}

// Note: Sensors overlay callbacks are now in SensorSettingsOverlay class
// See ui_settings_sensors.cpp
// Note: Macro Buttons overlay callbacks are now in MacroButtonsOverlay class
// See ui_settings_macro_buttons.cpp

// Static callback for check updates button
static void on_check_updates_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_check_updates_clicked");
    spdlog::info("[SettingsPanel] Check for updates requested");
    UpdateChecker::instance().check_for_updates();
    LVGL_SAFE_EVENT_CB_END();
}

// Static callback for install update row (opens download modal)
static void on_install_update_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_install_update_clicked");
    spdlog::info("[SettingsPanel] Install update requested");
    get_global_settings_panel().show_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

// Static callback to start downloading update
static void on_update_download_start(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_update_download_start");
    spdlog::info("[SettingsPanel] Starting update download");
    UpdateChecker::instance().start_download();
    LVGL_SAFE_EVENT_CB_END();
}

// Static callback to cancel download
static void on_update_download_cancel(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_update_download_cancel");
    spdlog::info("[SettingsPanel] Download cancelled by user");
    UpdateChecker::instance().cancel_download();
    get_global_settings_panel().hide_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

// Static callback to dismiss download modal (close without action)
static void on_update_download_dismiss(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_update_download_dismiss");
    get_global_settings_panel().hide_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// MODAL DIALOG STATIC CALLBACKS (XML event_cb)
// ============================================================================

static void on_factory_reset_confirm(lv_event_t* e) {
    (void)e;
    spdlog::info("[SettingsPanel] User confirmed factory reset");
    auto& panel = get_global_settings_panel();
    panel.perform_factory_reset();
}

static void on_factory_reset_cancel(lv_event_t* e) {
    (void)e;
    spdlog::info("[SettingsPanel] User cancelled factory reset");
    auto& panel = get_global_settings_panel();
    if (panel.factory_reset_dialog_) {
        NavigationManager::instance().go_back(); // Animation + callback will handle cleanup
    }
}

void SettingsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize settings subjects across all domain managers (for reactive binding)
    SettingsManager::instance().init_subjects();

    // Note: LED config loading moved to MoonrakerManager::create_api() for centralized init

    // Note: brightness_value subject is now managed by DisplaySettingsOverlay
    // See ui_settings_display.cpp

    // Initialize info row subjects (for reactive binding)
    UI_MANAGED_SUBJECT_STRING(version_value_subject_, version_value_buf_, "—", "version_value",
                              subjects_);

    // Formatted version for About row description (e.g., "Current Version: 1.2.3")
    UI_MANAGED_SUBJECT_STRING(about_version_description_subject_, about_version_description_buf_,
                              "—", "about_version_description", subjects_);

    UI_MANAGED_SUBJECT_STRING(printer_value_subject_, printer_value_buf_, "—", "printer_value",
                              subjects_);

    UI_MANAGED_SUBJECT_STRING(printer_host_value_subject_, printer_host_value_buf_, "—",
                              "printer_host_value", subjects_);

    UI_MANAGED_SUBJECT_STRING(print_hours_value_subject_, print_hours_value_buf_, "—",
                              "print_hours_value", subjects_);

    UI_MANAGED_SUBJECT_STRING(update_current_version_subject_, update_current_version_buf_,
                              helix_version(), "update_current_version", subjects_);

    // LED chip selection (no subject needed - chips handle their own state)

    // Initialize visibility subjects (controls which settings are shown)
    // Touch calibration: show on touch displays (non-SDL) OR in test mode (for testing on desktop)
#ifdef HELIX_DISPLAY_SDL
    bool show_touch_cal = get_runtime_config()->is_test_mode();
#else
    DisplayManager* dm = DisplayManager::instance();
    bool show_touch_cal = dm && dm->needs_touch_calibration();
#endif
    lv_subject_init_int(&show_touch_calibration_subject_, show_touch_cal ? 1 : 0);
    subjects_.register_subject(&show_touch_calibration_subject_);
    lv_xml_register_subject(nullptr, "show_touch_calibration", &show_touch_calibration_subject_);

    // Note: show_beta_features subject is initialized globally in app_globals.cpp

    // Platform visibility subjects — hidden on Android where OS manages these
    bool on_android = helix::is_android_platform();

    lv_subject_init_int(&show_network_settings_subject_, on_android ? 0 : 1);
    subjects_.register_subject(&show_network_settings_subject_);
    lv_xml_register_subject(nullptr, "show_network_settings", &show_network_settings_subject_);

    lv_subject_init_int(&show_update_settings_subject_, on_android ? 0 : 1);
    subjects_.register_subject(&show_update_settings_subject_);
    lv_xml_register_subject(nullptr, "show_update_settings", &show_update_settings_subject_);

    lv_subject_init_int(&show_backlight_settings_subject_, on_android ? 0 : 1);
    subjects_.register_subject(&show_backlight_settings_subject_);
    lv_xml_register_subject(nullptr, "show_backlight_settings", &show_backlight_settings_subject_);

    // Touch calibration status - show "Calibrated" or "Not calibrated" in row description
    Config* config = Config::get_instance();
    bool is_calibrated = config && config->get<bool>("/input/calibration/valid", false);
    const char* status_text = is_calibrated ? lv_tr("Calibrated") : lv_tr("Not calibrated");
    UI_MANAGED_SUBJECT_STRING(touch_cal_status_subject_, touch_cal_status_buf_, status_text,
                              "touch_cal_status", subjects_);

    // Register XML event callbacks for dropdowns, toggles, and action rows
    register_xml_callbacks({
        // Dropdowns
        {"on_completion_alert_changed", on_completion_alert_dropdown_changed},
        {"on_display_dim_changed", on_display_dim_dropdown_changed},
        {"on_display_sleep_changed", on_display_sleep_dropdown_changed},
        {"on_bed_mesh_mode_changed", on_bed_mesh_mode_changed},
        {"on_gcode_mode_changed", on_gcode_mode_changed},
        {"on_z_movement_style_changed", on_z_movement_style_changed},
        {"on_time_format_changed", on_time_format_changed},
        {"on_language_changed", on_language_changed},
        {"on_update_channel_changed", on_update_channel_changed},
        {"on_version_clicked", on_version_clicked},
        {"on_printer_name_clicked", on_printer_name_clicked},

        // Toggle switches
        {"on_dark_mode_changed", on_dark_mode_changed},
        {"on_animations_changed", on_animations_changed},
        {"on_gcode_3d_changed", on_gcode_3d_changed},
        {"on_led_light_changed", on_led_light_changed},
        {"on_led_settings_clicked", on_led_settings_clicked},
        // Note: on_retraction_row_clicked is registered by RetractionSettingsOverlay
        {"on_sound_settings_clicked", on_sound_settings_clicked},
        {"on_estop_confirm_changed", on_estop_confirm_changed},
        {"on_cancel_escalation_changed", on_cancel_escalation_changed},
        {"on_cancel_escalation_timeout_changed", on_cancel_escalation_timeout_changed},
        {"on_telemetry_changed", SettingsPanel::on_telemetry_changed},
        {"on_telemetry_view_data", SettingsPanel::on_telemetry_view_data},

        // Action rows
        {"on_display_settings_clicked", on_display_settings_clicked},
        {"on_panel_widgets_clicked", SettingsPanel::on_panel_widgets_clicked},
        // Note: on_printer_image_clicked moved to PrinterManagerOverlay
        {"on_filament_sensors_clicked", on_filament_sensors_clicked},
    });

    // Note: Sensors overlay callbacks are now handled by SensorSettingsOverlay
    // See ui_settings_sensors.h
    helix::settings::get_sensor_settings_overlay().register_callbacks();

    // Note: Display Settings overlay callbacks are now handled by DisplaySettingsOverlay
    // See ui_settings_display.h

    // Settings action rows and overlay navigation callbacks
    register_xml_callbacks({
        {"on_ams_settings_clicked", on_ams_settings_clicked},
        {"on_spoolman_settings_clicked", on_spoolman_settings_clicked},
        {"on_macro_buttons_clicked", on_macro_buttons_clicked},
        {"on_machine_limits_clicked", on_machine_limits_clicked},
        {"on_network_clicked", on_network_clicked},
        {"on_power_devices_clicked", on_power_devices_clicked},
        {"on_factory_reset_clicked", on_factory_reset_clicked},
        {"on_hardware_health_clicked", on_hardware_health_clicked},
        {"on_plugins_clicked", on_plugins_clicked},
        // Note: on_check_updates_clicked, on_install_update_clicked registered in
        // register_settings_panel_callbacks()
        {"on_update_download_start", on_update_download_start},
        {"on_update_download_cancel", on_update_download_cancel},
        {"on_update_download_dismiss", on_update_download_dismiss},

        // Overlay callbacks
        {"on_restart_later_clicked", on_restart_later_clicked},
        {"on_restart_now_clicked", on_restart_now_clicked},

        // Modal dialog callbacks
        {"on_factory_reset_confirm", on_factory_reset_confirm},
        {"on_factory_reset_cancel", on_factory_reset_cancel},
        {"on_header_back_clicked", on_header_back_clicked},
        // Note: on_brightness_changed is now handled by DisplaySettingsOverlay
    });

    // Note: BedMeshPanel subjects are initialized in main.cpp during startup

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SettingsPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[{}] Deinitializing subjects", get_name());

    // Deinit all subjects via SubjectManager (handles 7 string subjects)
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

void SettingsPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Setup all handlers and bindings
    setup_toggle_handlers();
    setup_action_handlers();
    populate_info_rows();

    spdlog::debug("[{}] Setup complete", get_name());
}

// ============================================================================
// SETUP HELPERS
// ============================================================================

void SettingsPanel::setup_toggle_handlers() {
    auto& display_settings = DisplaySettingsManager::instance();
    auto& system_settings = SystemSettingsManager::instance();
    auto& safety_settings = SafetySettingsManager::instance();

    // === Dark Mode Toggle ===
    // Event handler wired via XML <event_cb>, just set initial state here
    lv_obj_t* dark_mode_row = lv_obj_find_by_name(panel_, "row_dark_mode");
    if (dark_mode_row) {
        dark_mode_switch_ = lv_obj_find_by_name(dark_mode_row, "toggle");
        if (dark_mode_switch_) {
            // Set initial state from DisplaySettingsManager
            if (display_settings.get_dark_mode()) {
                lv_obj_add_state(dark_mode_switch_, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(dark_mode_switch_, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}]   ✓ Dark mode toggle", get_name());
        }
    }

    // === Animations Toggle ===
    // Event handler wired via XML <event_cb>, just set initial state here
    lv_obj_t* animations_row = lv_obj_find_by_name(panel_, "row_animations");
    if (animations_row) {
        animations_switch_ = lv_obj_find_by_name(animations_row, "toggle");
        if (animations_switch_) {
            // Set initial state from DisplaySettingsManager
            if (display_settings.get_animations_enabled()) {
                lv_obj_add_state(animations_switch_, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(animations_switch_, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}]   ✓ Animations toggle", get_name());
        }
    }

    // LED chip selection moved to LedSettingsOverlay

    // === LED Light Toggle ===
    // Event handler wired via XML <event_cb>, sync toggle with actual printer LED state
    lv_obj_t* led_light_row = lv_obj_find_by_name(panel_, "row_led_light");
    if (led_light_row) {
        led_light_switch_ = lv_obj_find_by_name(led_light_row, "toggle");
        if (led_light_switch_) {
            // Sync toggle with actual printer LED state via observer
            led_state_observer_ = helix::ui::observe_int_sync<SettingsPanel>(
                printer_state_.get_led_state_subject(), this, [](SettingsPanel* self, int value) {
                    if (self->led_light_switch_) {
                        bool on = value != 0;
                        if (on) {
                            lv_obj_add_state(self->led_light_switch_, LV_STATE_CHECKED);
                        } else {
                            lv_obj_remove_state(self->led_light_switch_, LV_STATE_CHECKED);
                        }
                    }
                });
            spdlog::trace("[{}]   ✓ LED light toggle (observing printer state)", get_name());
        }
    }

    // === Telemetry Toggle ===
    lv_obj_t* telemetry_row = lv_obj_find_by_name(panel_, "row_telemetry");
    if (telemetry_row) {
        telemetry_switch_ = lv_obj_find_by_name(telemetry_row, "toggle");
        if (telemetry_switch_) {
            if (system_settings.get_telemetry_enabled()) {
                lv_obj_add_state(telemetry_switch_, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(telemetry_switch_, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}]   telemetry toggle", get_name());
        }
    }

    // === Completion Alert Dropdown ===
    // Event handler wired via XML <event_cb>, just set initial value here (options set in XML)
    lv_obj_t* completion_row = lv_obj_find_by_name(panel_, "row_completion_alert");
    if (completion_row) {
        completion_alert_dropdown_ = lv_obj_find_by_name(completion_row, "dropdown");
        if (completion_alert_dropdown_) {
            auto mode = AudioSettingsManager::instance().get_completion_alert_mode();
            lv_dropdown_set_selected(completion_alert_dropdown_, static_cast<uint32_t>(mode));
            spdlog::trace("[{}]   ✓ Completion alert dropdown (mode={})", get_name(),
                          static_cast<int>(mode));
        }
    }

    // === Z Movement Style Dropdown ===
    // Event handler wired via XML <event_cb>, just set initial value here (options set in XML)
    lv_obj_t* z_movement_row = lv_obj_find_by_name(panel_, "row_z_movement_style");
    if (z_movement_row) {
        lv_obj_t* z_movement_dropdown = lv_obj_find_by_name(z_movement_row, "dropdown");
        if (z_movement_dropdown) {
            auto style = SettingsManager::instance().get_z_movement_style();
            lv_dropdown_set_selected(z_movement_dropdown, static_cast<uint32_t>(style));
            spdlog::trace("[{}]   ✓ Z movement style dropdown (style={})", get_name(),
                          static_cast<int>(style));
        }
    }

    // === Language Dropdown ===
    // Event handler wired via XML <event_cb>, options populated from SystemSettingsManager
    lv_obj_t* language_row = lv_obj_find_by_name(panel_, "row_language");
    if (language_row) {
        language_dropdown_ = lv_obj_find_by_name(language_row, "dropdown");
        if (language_dropdown_) {
            lv_dropdown_set_options(language_dropdown_,
                                    SystemSettingsManager::get_language_options());
            int lang_index = system_settings.get_language_index();
            lv_dropdown_set_selected(language_dropdown_, static_cast<uint32_t>(lang_index));
            spdlog::trace("[{}]   ✓ Language dropdown (index={})", get_name(), lang_index);
        }
    }

    // === E-Stop Confirmation Toggle ===
    // Event handler wired via XML <event_cb>, just set initial state here
    lv_obj_t* estop_confirm_row = lv_obj_find_by_name(panel_, "row_estop_confirm");
    if (estop_confirm_row) {
        estop_confirm_switch_ = lv_obj_find_by_name(estop_confirm_row, "toggle");
        if (estop_confirm_switch_) {
            if (safety_settings.get_estop_require_confirmation()) {
                lv_obj_add_state(estop_confirm_switch_, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}]   ✓ E-Stop confirmation toggle", get_name());
        }
    }
}

void SettingsPanel::setup_action_handlers() {
    // All action row event handlers are wired via XML <event_cb>
    // Just cache the row references for potential future use

    // === Display Settings Row ===
    display_settings_row_ = lv_obj_find_by_name(panel_, "row_display_settings");
    if (display_settings_row_) {
        spdlog::trace("[{}]   ✓ Display settings action row", get_name());
    }

    // === Filament Sensors Row ===
    filament_sensors_row_ = lv_obj_find_by_name(panel_, "row_filament_sensors");
    if (filament_sensors_row_) {
        spdlog::trace("[{}]   ✓ Filament sensors action row", get_name());
    }

    // === Network Row ===
    network_row_ = lv_obj_find_by_name(panel_, "row_network");
    if (network_row_) {
        spdlog::trace("[{}]   ✓ Network action row", get_name());
    }

    // === Factory Reset Row ===
    factory_reset_row_ = lv_obj_find_by_name(panel_, "row_factory_reset");
    if (factory_reset_row_) {
        spdlog::trace("[{}]   ✓ Factory reset action row", get_name());
    }

    // === Hardware Health Row (reactive label binding) ===
    lv_obj_t* hardware_health_row = lv_obj_find_by_name(panel_, "row_hardware_health");
    if (hardware_health_row) {
        lv_obj_t* label = lv_obj_find_by_name(hardware_health_row, "label");
        if (label) {
            // Bind to subject with %s format (string passthrough)
            lv_label_bind_text(label, get_printer_state().get_hardware_issues_label_subject(),
                               "%s");
            spdlog::trace("[{}]   ✓ Hardware health row with reactive label", get_name());
        }
    }

    // === Touch Calibration Row (reactive description binding) ===
    lv_obj_t* touch_cal_row = lv_obj_find_by_name(panel_, "row_touch_calibration");
    if (touch_cal_row) {
        lv_obj_t* description = lv_obj_find_by_name(touch_cal_row, "description");
        if (description) {
            // Bind to subject for "Calibrated" / "Not calibrated" status
            lv_label_bind_text(description, &touch_cal_status_subject_, "%s");
            spdlog::trace("[{}]   ✓ Touch calibration row with reactive description", get_name());
        }
    }

    // === About HelixScreen Row (description shows formatted version) ===
    lv_obj_t* about_row = lv_obj_find_by_name(panel_, "row_about");
    if (about_row) {
        lv_obj_t* description = lv_obj_find_by_name(about_row, "description");
        if (description) {
            lv_label_bind_text(description, &about_version_description_subject_, "%s");
            spdlog::trace("[{}]   About row with version description", get_name());
        }
    }

    // Note: Check for Updates row bound declaratively in XML
}

void SettingsPanel::populate_info_rows() {
    // === Version (subject used by About overlay and About row description) ===
    lv_subject_copy_string(&version_value_subject_, helix_version());
    std::string about_desc = std::string(lv_tr("Current Version")) + ": " + helix_version();
    lv_subject_copy_string(&about_version_description_subject_, about_desc.c_str());
    spdlog::trace("[{}]   Version subject: {}", get_name(), helix_version());

    // === Printer Name (from PrinterState or Config) ===
    lv_obj_t* printer_row = lv_obj_find_by_name(panel_, "row_printer");
    if (printer_row) {
        printer_value_ = lv_obj_find_by_name(printer_row, "value");
        if (printer_value_) {
            // Try to get printer name from config (wizard stores at /printer/name)
            Config* config = Config::get_instance();
            std::string printer_name =
                config->get<std::string>(helix::wizard::PRINTER_NAME, "Unknown");
            // Update subject (label binding happens in XML)
            lv_subject_copy_string(&printer_value_subject_, printer_name.c_str());
            spdlog::trace("[{}]   ✓ Printer: {}", get_name(), printer_name);
        }
    }

    // === Printer Host (action row - shows IP/hostname:port as description) ===
    lv_obj_t* host_row = lv_obj_find_by_name(panel_, "row_printer_host");
    if (host_row) {
        lv_obj_t* description = lv_obj_find_by_name(host_row, "description");
        if (description) {
            Config* config = Config::get_instance();
            std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
            int port = config->get<int>(config->df() + "moonraker_port", 7125);

            if (!host.empty()) {
                std::string host_display = host + ":" + std::to_string(port);
                lv_subject_copy_string(&printer_host_value_subject_, host_display.c_str());
            }
            lv_label_bind_text(description, &printer_host_value_subject_, "%s");
            spdlog::trace("[{}]   ✓ Printer Host action row with reactive description", get_name());
        }
    }

    // Note: Klipper/Moonraker/OS version rows bound declaratively in settings_panel.xml

    // Print hours: fetched on-demand via fetch_print_hours() after connection is live.
    // Called from discovery complete callback and on notify_history_changed events.
}

// ============================================================================
// LIVE DATA FETCHING
// ============================================================================

void SettingsPanel::fetch_print_hours() {
    if (!api_ || !subjects_initialized_) {
        return;
    }

    api_->history().get_history_totals(
        [this](const PrintHistoryTotals& totals) {
            std::string formatted = helix::format::duration(static_cast<int>(totals.total_time));
            helix::ui::queue_update([this, formatted]() {
                if (subjects_initialized_) {
                    lv_subject_copy_string(&print_hours_value_subject_, formatted.c_str());
                    spdlog::trace("[{}] Print hours updated: {}", get_name(), formatted);
                }
            });
        },
        [this](const MoonrakerError& err) {
            spdlog::warn("[{}] Failed to fetch print hours: {}", get_name(), err.message);
        });
}

void SettingsPanel::populate_led_chips() {
    // LED chip selection has been moved to LedSettingsOverlay.
    // This method is kept as a no-op stub for callers that haven't been updated yet.
    spdlog::trace("[{}] populate_led_chips() is now handled by LedSettingsOverlay", get_name());
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SettingsPanel::handle_dark_mode_changed(bool enabled) {
    spdlog::info("[{}] Dark mode toggled: {}", get_name(), enabled ? "ON" : "OFF");

    // Save the setting and apply live
    DisplaySettingsManager::instance().set_dark_mode(enabled);
    theme_manager_apply_theme(theme_manager_get_active_theme(), enabled);
}

void SettingsPanel::handle_animations_changed(bool enabled) {
    spdlog::info("[{}] Animations toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_animations_enabled(enabled);
}

void SettingsPanel::handle_gcode_3d_changed(bool enabled) {
    spdlog::info("[{}] G-code 3D preview toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_gcode_3d_enabled(enabled);
}

void SettingsPanel::handle_display_sleep_changed(int index) {
    int seconds = DisplaySettingsManager::index_to_sleep_seconds(index);
    spdlog::info("[{}] Display sleep changed: index {} = {}s", get_name(), index, seconds);
    DisplaySettingsManager::instance().set_display_sleep_sec(seconds);
}

void SettingsPanel::handle_led_light_changed(bool enabled) {
    spdlog::info("[{}] LED light toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_led_enabled(enabled);
}

// handle_led_chip_clicked moved to LedSettingsOverlay

void SettingsPanel::handle_estop_confirm_changed(bool enabled) {
    spdlog::info("[{}] E-Stop confirmation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_estop_require_confirmation(enabled);
    // Update EmergencyStopOverlay immediately
    EmergencyStopOverlay::instance().set_require_confirmation(enabled);
}

void SettingsPanel::handle_cancel_escalation_changed(bool enabled) {
    spdlog::info("[{}] Cancel escalation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_cancel_escalation_enabled(enabled);
}

void SettingsPanel::handle_telemetry_changed(bool enabled) {
    spdlog::info("[{}] Telemetry toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SystemSettingsManager::instance().set_telemetry_enabled(enabled);
    if (enabled) {
        ToastManager::instance().show(
            ToastSeverity::SUCCESS,
            lv_tr("Thanks! TOTALLY anonymous usage data helps improve HelixScreen."), 4000);
    }
}

void SettingsPanel::handle_telemetry_view_data_clicked() {
    spdlog::debug("[{}] View Telemetry Data clicked - delegating to TelemetryDataOverlay",
                  get_name());

    auto& overlay = helix::settings::get_telemetry_data_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::show_restart_prompt() {
    // Already showing
    if (restart_prompt_dialog_) {
        return;
    }

    restart_prompt_dialog_ = helix::ui::modal_show("restart_prompt_dialog");
    if (restart_prompt_dialog_) {
        spdlog::debug("[{}] Restart prompt dialog shown via Modal system", get_name());
        // Clear pending flag so we don't show again until next change
        InputSettingsManager::instance().clear_restart_pending();
    }
}

void SettingsPanel::handle_debug_bundle_clicked() {
    spdlog::info("[SettingsPanel] Upload Debug Bundle clicked");
    auto* modal = new DebugBundleModal();
    modal->show_modal(lv_screen_active());
}

void SettingsPanel::handle_discord_clicked() {
    spdlog::info("[SettingsPanel] Discord clicked");
    // i18n: URL, do not translate
    ToastManager::instance().show(ToastSeverity::INFO, "Join us at discord.gg/helixscreen", 5000);
}

void SettingsPanel::handle_docs_clicked() {
    spdlog::info("[SettingsPanel] Documentation clicked");
    // i18n: URL, do not translate
    ToastManager::instance().show(ToastSeverity::INFO, "Visit docs.helixscreen.org", 5000);
}

void SettingsPanel::handle_sound_settings_clicked() {
    spdlog::debug("[{}] Sound Settings clicked - delegating to SoundSettingsOverlay", get_name());

    auto& overlay = helix::settings::get_sound_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_led_settings_clicked() {
    spdlog::debug("[{}] LED Settings clicked - delegating to LedSettingsOverlay", get_name());

    auto& overlay = helix::settings::get_led_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_display_settings_clicked() {
    spdlog::debug("[{}] Display Settings clicked - delegating to DisplaySettingsOverlay",
                  get_name());

    auto& overlay = helix::settings::get_display_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_panel_widgets_clicked() {
    spdlog::debug("[{}] Home Widgets clicked - delegating to PanelWidgetsOverlay", get_name());

    auto& overlay = helix::settings::get_panel_widgets_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_filament_sensors_clicked() {
    spdlog::debug("[{}] Sensors clicked - delegating to SensorSettingsOverlay", get_name());

    auto& overlay = helix::settings::get_sensor_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_ams_settings_clicked() {
    spdlog::debug("[{}] AMS Settings clicked - opening Device Operations", get_name());

    auto& overlay = helix::ui::get_ams_device_operations_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_spoolman_settings_clicked() {
    spdlog::debug("[{}] Spoolman Settings clicked - opening Spoolman overlay", get_name());

    auto& overlay = helix::ui::get_ams_spoolman_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }
    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        overlay.set_api(api);
    }
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_macro_buttons_clicked() {
    spdlog::debug("[{}] Macro Buttons clicked - delegating to MacroButtonsOverlay", get_name());

    auto& overlay = helix::settings::get_macro_buttons_overlay();
    overlay.show(parent_screen_);
}

// Note: populate_macro_dropdowns() moved to MacroButtonsOverlay::populate_dropdowns()
// See ui_settings_macro_buttons.cpp
// Note: populate_sensor_list() moved to SensorSettingsOverlay::populate_switch_sensors()
// See ui_settings_sensors.cpp

void SettingsPanel::handle_machine_limits_clicked() {
    spdlog::debug("[{}] Machine Limits clicked - delegating to MachineLimitsOverlay", get_name());

    auto& overlay = helix::settings::get_machine_limits_overlay();
    overlay.set_api(api_);
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_change_host_clicked() {
    spdlog::debug("[{}] Change Host clicked", get_name());

    if (!change_host_modal_) {
        change_host_modal_ = std::make_unique<ChangeHostModal>();
    }

    change_host_modal_->set_completion_callback([this](bool changed) {
        if (!changed)
            return;

        // Update host display subject from config
        Config* config = Config::get_instance();
        std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
        int port = config->get<int>(config->df() + "moonraker_port", 7125);
        std::string host_display = host + ":" + std::to_string(port);
        lv_subject_copy_string(&printer_host_value_subject_, host_display.c_str());

        // Reconnect to the new host
        MoonrakerClient* client = get_moonraker_client();
        MoonrakerManager* manager = get_moonraker_manager();

        if (!client || !manager) {
            spdlog::error("[{}] Cannot reconnect - client or manager not available", get_name());
            return;
        }

        // Suppress recovery modal during intentional switch
        EmergencyStopOverlay::instance().suppress_recovery_dialog(5000);

        // Disconnect current connection
        client->disconnect();

        // Build new URLs and connect with full discovery pipeline
        std::string ws_url = "ws://" + host + ":" + std::to_string(port) + "/websocket";
        std::string http_url = "http://" + host + ":" + std::to_string(port);

        spdlog::info("[{}] Reconnecting to {}:{}", get_name(), host, port);
        manager->connect(ws_url, http_url);
    });

    change_host_modal_->show_modal(lv_screen_active());
}

void SettingsPanel::handle_network_clicked() {
    spdlog::debug("[{}] Network Settings clicked", get_name());

    auto& overlay = get_network_settings_overlay();

    if (!overlay.is_created()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
    }

    overlay.show();
}

void SettingsPanel::handle_power_devices_clicked() {
    spdlog::debug("[{}] Power Devices clicked", get_name());

    auto& panel = get_global_power_panel();
    lv_obj_t* overlay = panel.get_or_create_overlay(parent_screen_);
    if (overlay) {
        NavigationManager::instance().push_overlay(overlay);
    } else {
        spdlog::error("[{}] Failed to open Power panel", get_name());
    }
}

void SettingsPanel::handle_touch_calibration_clicked() {
    DisplayManager* dm = DisplayManager::instance();
    if (dm && !dm->needs_touch_calibration()) {
        spdlog::debug("[{}] Touch calibration not needed for this device", get_name());
        return;
    }

    spdlog::debug("[{}] Touch Calibration clicked", get_name());

    auto& overlay = helix::ui::get_touch_calibration_overlay();

    if (!overlay.is_created()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
    }

    overlay.show([this](bool success) {
        if (success) {
            // Update status when calibration completes successfully
            lv_subject_copy_string(&touch_cal_status_subject_, lv_tr("Calibrated"));
            spdlog::info("[{}] Touch calibration completed - updated status", get_name());
        }
    });
}

void SettingsPanel::handle_restart_helix_clicked() {
    spdlog::info("[SettingsPanel] Restart HelixScreen requested");
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Restarting HelixScreen..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::async_call(
        [](void*) {
            spdlog::info("[SettingsPanel] Initiating restart...");
            app_request_restart_service();
        },
        nullptr);
}

void SettingsPanel::handle_factory_reset_clicked() {
    spdlog::debug("[{}] Factory Reset clicked - showing confirmation dialog", get_name());

    // Create dialog on first use (lazy initialization)
    if (!factory_reset_dialog_ && parent_screen_) {
        spdlog::debug("[{}] Creating factory reset dialog...", get_name());

        // Create self-contained factory_reset_modal component
        // Callbacks are already wired via XML event_cb elements
        factory_reset_dialog_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "factory_reset_modal", nullptr));

        if (factory_reset_dialog_) {
            // Start hidden
            lv_obj_add_flag(factory_reset_dialog_, LV_OBJ_FLAG_HIDDEN);

            // Register close callback to delete dialog when animation completes
            NavigationManager::instance().register_overlay_close_callback(
                factory_reset_dialog_, [this]() {
                    helix::ui::safe_delete(factory_reset_dialog_);
                    factory_reset_dialog_ = nullptr;
                });

            spdlog::info("[{}] Factory reset dialog created", get_name());
        } else {
            spdlog::error("[{}] Failed to create factory reset dialog", get_name());
            return;
        }
    }

    // Show the dialog via navigation stack
    if (factory_reset_dialog_) {
        NavigationManager::instance().push_overlay(factory_reset_dialog_);
    }
}

void SettingsPanel::handle_plugins_clicked() {
    spdlog::debug("[{}] Plugins clicked - opening overlay", get_name());

    auto& overlay = get_settings_plugins_overlay();

    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
    }

    // Show the overlay via navigation stack
    if (overlay.get_root()) {
        NavigationManager::instance().register_overlay_instance(overlay.get_root(), &overlay);
        NavigationManager::instance().push_overlay(overlay.get_root());
    }
}

void SettingsPanel::show_update_download_modal() {
    if (!update_download_modal_) {
        update_download_modal_ = helix::ui::modal_show("update_download_modal");
    }

    // Set to Confirming state with version info
    auto info = UpdateChecker::instance().get_cached_update();
    std::string text = info ? fmt::format(lv_tr("Download v{}?"), info->version)
                            : std::string(lv_tr("Download update?"));
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Confirming, 0,
                                                     text);
}

void SettingsPanel::hide_update_download_modal() {
    if (update_download_modal_) {
        helix::ui::modal_hide(update_download_modal_);
        update_download_modal_ = nullptr;
    }
    // Reset download state
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
}

void SettingsPanel::perform_factory_reset() {
    spdlog::warn("[{}] Performing factory reset - resetting config!", get_name());

    // Get config instance and reset
    Config* config = Config::get_instance();
    if (config) {
        config->reset_to_defaults();
        config->save();
        spdlog::info("[{}] Config reset to defaults", get_name());
    }

    // Hide the dialog - animation + callback will handle cleanup
    if (factory_reset_dialog_) {
        NavigationManager::instance().go_back();
    }

    // Show confirmation toast and restart
    ToastManager::instance().show(ToastSeverity::INFO,
                                  lv_tr("Settings reset to defaults. Restarting..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::async_call(
        [](void*) {
            spdlog::info("[SettingsPanel] Restarting after factory reset...");
            app_request_restart_service();
        },
        nullptr);
}

void SettingsPanel::handle_hardware_health_clicked() {
    spdlog::debug("[{}] Hardware Health clicked - delegating to HardwareHealthOverlay", get_name());

    auto& overlay = helix::settings::get_hardware_health_overlay();
    overlay.set_printer_state(&printer_state_);
    overlay.show(parent_screen_);
}

// Note: populate_hardware_issues() moved to HardwareHealthOverlay
// See ui_settings_hardware_health.cpp

// Note: handle_hardware_action() and related methods moved to HardwareHealthOverlay
// See ui_settings_hardware_health.cpp

// ============================================================================
// STATIC TRAMPOLINES (XML event_cb pattern - use global singleton)
// ============================================================================

void SettingsPanel::on_dark_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_dark_mode_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_dark_mode_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_animations_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_animations_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_animations_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_gcode_3d_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_gcode_3d_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_gcode_3d_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_display_sleep_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_display_sleep_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_global_settings_panel().handle_display_sleep_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_led_light_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_led_light_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_led_light_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_estop_confirm_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_estop_confirm_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_estop_confirm_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_cancel_escalation_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_cancel_escalation_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_cancel_escalation_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_debug_bundle_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_debug_bundle_clicked");
    get_global_settings_panel().handle_debug_bundle_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_discord_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_discord_clicked");
    get_global_settings_panel().handle_discord_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_docs_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_docs_clicked");
    get_global_settings_panel().handle_docs_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_telemetry_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_telemetry_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_telemetry_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_telemetry_view_data(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_telemetry_view_data");
    get_global_settings_panel().handle_telemetry_view_data_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_sound_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_sound_settings_clicked");
    get_global_settings_panel().handle_sound_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_led_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_led_settings_clicked");
    get_global_settings_panel().handle_led_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_display_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_display_settings_clicked");
    get_global_settings_panel().handle_display_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_panel_widgets_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_panel_widgets_clicked");
    get_global_settings_panel().handle_panel_widgets_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_filament_sensors_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_filament_sensors_clicked");
    get_global_settings_panel().handle_filament_sensors_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_ams_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_ams_settings_clicked");
    get_global_settings_panel().handle_ams_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_spoolman_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_spoolman_settings_clicked");
    get_global_settings_panel().handle_spoolman_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_macro_buttons_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_macro_buttons_clicked");
    get_global_settings_panel().handle_macro_buttons_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_machine_limits_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_machine_limits_clicked");
    get_global_settings_panel().handle_machine_limits_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_change_host_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_change_host_clicked");
    get_global_settings_panel().handle_change_host_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_network_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_network_clicked");
    get_global_settings_panel().handle_network_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_power_devices_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_power_devices_clicked");
    get_global_settings_panel().handle_power_devices_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_touch_calibration_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_touch_calibration_clicked");
    get_global_settings_panel().handle_touch_calibration_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_factory_reset_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_factory_reset_clicked");
    get_global_settings_panel().handle_factory_reset_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_hardware_health_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_hardware_health_clicked");
    get_global_settings_panel().handle_hardware_health_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_plugins_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_plugins_clicked");
    get_global_settings_panel().handle_plugins_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_restart_helix_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_restart_helix_settings_clicked");
    get_global_settings_panel().handle_restart_helix_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_print_hours_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_print_hours_clicked");
    get_global_settings_panel().handle_print_hours_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::handle_print_hours_clicked() {
    helix::ui::lazy_create_and_push_overlay<HistoryDashboardPanel>(
        get_global_history_dashboard_panel, history_dashboard_panel_, parent_screen_,
        "Print History", get_name());
}

// ============================================================================
// STATIC TRAMPOLINES - OVERLAYS
// ============================================================================

// Note: Machine limits overlay callbacks are now in MachineLimitsOverlay class
// See ui_settings_machine_limits.cpp

void SettingsPanel::on_restart_later_clicked(lv_event_t* /* e */) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_restart_later_clicked");
    auto& panel = get_global_settings_panel();
    if (panel.restart_prompt_dialog_) {
        helix::ui::modal_hide(panel.restart_prompt_dialog_);
        panel.restart_prompt_dialog_ = nullptr;
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_restart_now_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_restart_now_clicked");
    spdlog::info("[SettingsPanel] User requested restart (input settings changed)");
    app_request_restart_service();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_header_back_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_header_back_clicked");
    NavigationManager::instance().go_back();
    LVGL_SAFE_EVENT_CB_END();
}

// Note: on_brightness_changed is now handled by DisplaySettingsOverlay
// See ui_settings_display.cpp

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<SettingsPanel> g_settings_panel;

SettingsPanel& get_global_settings_panel() {
    if (!g_settings_panel) {
        g_settings_panel = std::make_unique<SettingsPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("SettingsPanel",
                                                         []() { g_settings_panel.reset(); });
    }
    return *g_settings_panel;
}

// Register callbacks BEFORE settings_panel.xml registration per [L013]
void register_settings_panel_callbacks() {
    spdlog::trace("[SettingsPanel] Registering XML callbacks for settings_panel.xml");

    register_xml_callbacks({
        // Toggle callbacks used in settings_panel.xml
        {"on_animations_changed", SettingsPanel::on_animations_changed},
        {"on_gcode_3d_changed", SettingsPanel::on_gcode_3d_changed},
        {"on_led_light_changed", SettingsPanel::on_led_light_changed},
        {"on_led_settings_clicked", SettingsPanel::on_led_settings_clicked},
        {"on_sound_settings_clicked", SettingsPanel::on_sound_settings_clicked},
        {"on_estop_confirm_changed", SettingsPanel::on_estop_confirm_changed},
        {"on_cancel_escalation_changed", SettingsPanel::on_cancel_escalation_changed},
        {"on_cancel_escalation_timeout_changed", on_cancel_escalation_timeout_changed},
        {"on_telemetry_changed", SettingsPanel::on_telemetry_changed},
        {"on_telemetry_view_data", SettingsPanel::on_telemetry_view_data},

        // Action row callbacks used in settings_panel.xml
        {"on_display_settings_clicked", SettingsPanel::on_display_settings_clicked},
        {"on_panel_widgets_clicked", SettingsPanel::on_panel_widgets_clicked},
        {"on_filament_sensors_clicked", SettingsPanel::on_filament_sensors_clicked},
        {"on_macro_buttons_clicked", SettingsPanel::on_macro_buttons_clicked},
        {"on_machine_limits_clicked", SettingsPanel::on_machine_limits_clicked},
        {"on_network_clicked", SettingsPanel::on_network_clicked},
        {"on_power_devices_clicked", SettingsPanel::on_power_devices_clicked},
        {"on_touch_calibration_clicked", SettingsPanel::on_touch_calibration_clicked},
        {"on_factory_reset_clicked", SettingsPanel::on_factory_reset_clicked},
        {"on_hardware_health_clicked", SettingsPanel::on_hardware_health_clicked},
        {"on_restart_helix_settings_clicked", SettingsPanel::on_restart_helix_settings_clicked},
        {"on_print_hours_clicked", SettingsPanel::on_print_hours_clicked},
        {"on_change_host_clicked", SettingsPanel::on_change_host_clicked},
        // Help & Support callbacks
        {"on_debug_bundle_clicked", SettingsPanel::on_debug_bundle_clicked},
        {"on_discord_clicked", SettingsPanel::on_discord_clicked},
        {"on_docs_clicked", SettingsPanel::on_docs_clicked},

        {"on_check_updates_clicked", on_check_updates_clicked},
        {"on_install_update_clicked", on_install_update_clicked},
    });
}
