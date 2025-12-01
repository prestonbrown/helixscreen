// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_settings.h"

#include "app_globals.h"
#include "config.h"
#include "moonraker_client.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "ui_event_safety.h"
#include "ui_nav.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"

#include <spdlog/spdlog.h>

#include <memory>

// Version string (could come from build system)
#ifndef HELIX_VERSION
#define HELIX_VERSION "1.0.0-dev"
#endif

// Forward declarations for class-based API
class BedMeshPanel;
BedMeshPanel& get_global_bed_mesh_panel();
ZOffsetCalibrationPanel& get_global_zoffset_cal_panel();
PIDCalibrationPanel& get_global_pid_cal_panel();
MoonrakerClient* get_moonraker_client();

// ============================================================================
// CONSTRUCTOR
// ============================================================================

SettingsPanel::SettingsPanel(PrinterState& printer_state, MoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::trace("[{}] Constructor", get_name());
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void SettingsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Initialize SettingsManager subjects (for reactive binding)
    SettingsManager::instance().init_subjects();

    // Note: BedMeshPanel subjects are initialized in main.cpp during startup

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
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

    spdlog::info("[{}] Setup complete", get_name());
}

// ============================================================================
// SETUP HELPERS
// ============================================================================

void SettingsPanel::setup_toggle_handlers() {
    auto& settings = SettingsManager::instance();

    // === Dark Mode Toggle ===
    lv_obj_t* dark_mode_row = lv_obj_find_by_name(panel_, "row_dark_mode");
    if (dark_mode_row) {
        dark_mode_switch_ = lv_obj_find_by_name(dark_mode_row, "toggle");
        if (dark_mode_switch_) {
            // Set initial state from SettingsManager
            if (settings.get_dark_mode()) {
                lv_obj_add_state(dark_mode_switch_, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(dark_mode_switch_, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(dark_mode_switch_, on_dark_mode_changed, LV_EVENT_VALUE_CHANGED,
                                this);
            spdlog::debug("[{}]   ✓ Dark mode toggle", get_name());
        }
    }

    // === LED Light Toggle ===
    lv_obj_t* led_light_row = lv_obj_find_by_name(panel_, "row_led_light");
    if (led_light_row) {
        led_light_switch_ = lv_obj_find_by_name(led_light_row, "toggle");
        if (led_light_switch_) {
            // LED state from SettingsManager (ephemeral, starts off)
            if (settings.get_led_enabled()) {
                lv_obj_add_state(led_light_switch_, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(led_light_switch_, on_led_light_changed, LV_EVENT_VALUE_CHANGED,
                                this);
            spdlog::debug("[{}]   ✓ LED light toggle", get_name());
        }
    }

    // === Sounds Toggle (placeholder) ===
    lv_obj_t* sounds_row = lv_obj_find_by_name(panel_, "row_sounds");
    if (sounds_row) {
        sounds_switch_ = lv_obj_find_by_name(sounds_row, "toggle");
        if (sounds_switch_) {
            if (settings.get_sounds_enabled()) {
                lv_obj_add_state(sounds_switch_, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sounds_switch_, on_sounds_changed, LV_EVENT_VALUE_CHANGED, this);
            spdlog::debug("[{}]   ✓ Sounds toggle", get_name());
        }
    }

    // === Completion Alert Toggle ===
    lv_obj_t* completion_row = lv_obj_find_by_name(panel_, "row_completion_alert");
    if (completion_row) {
        completion_alert_switch_ = lv_obj_find_by_name(completion_row, "toggle");
        if (completion_alert_switch_) {
            if (settings.get_completion_alert()) {
                lv_obj_add_state(completion_alert_switch_, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(completion_alert_switch_, on_completion_alert_changed,
                                LV_EVENT_VALUE_CHANGED, this);
            spdlog::debug("[{}]   ✓ Completion alert toggle", get_name());
        }
    }
}

void SettingsPanel::setup_action_handlers() {
    // === Display Settings Row ===
    display_settings_row_ = lv_obj_find_by_name(panel_, "row_display_settings");
    if (display_settings_row_) {
        lv_obj_add_event_cb(display_settings_row_, on_display_settings_clicked, LV_EVENT_CLICKED,
                            this);
        spdlog::debug("[{}]   ✓ Display settings action row", get_name());
    }

    // === Bed Mesh Row ===
    bed_mesh_row_ = lv_obj_find_by_name(panel_, "row_bed_mesh");
    if (bed_mesh_row_) {
        lv_obj_add_event_cb(bed_mesh_row_, on_bed_mesh_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Bed mesh action row", get_name());
    }

    // === Z-Offset Row ===
    z_offset_row_ = lv_obj_find_by_name(panel_, "row_z_offset");
    if (z_offset_row_) {
        lv_obj_add_event_cb(z_offset_row_, on_z_offset_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Z-offset action row", get_name());
    }

    // === PID Tuning Row ===
    pid_tuning_row_ = lv_obj_find_by_name(panel_, "row_pid_tuning");
    if (pid_tuning_row_) {
        lv_obj_add_event_cb(pid_tuning_row_, on_pid_tuning_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ PID tuning action row", get_name());
    }

    // === Network Row ===
    network_row_ = lv_obj_find_by_name(panel_, "row_network");
    if (network_row_) {
        lv_obj_add_event_cb(network_row_, on_network_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Network action row", get_name());
    }

    // === Factory Reset Row ===
    factory_reset_row_ = lv_obj_find_by_name(panel_, "row_factory_reset");
    if (factory_reset_row_) {
        lv_obj_add_event_cb(factory_reset_row_, on_factory_reset_clicked, LV_EVENT_CLICKED, this);
        spdlog::debug("[{}]   ✓ Factory reset action row", get_name());
    }
}

void SettingsPanel::populate_info_rows() {
    // === Version ===
    lv_obj_t* version_row = lv_obj_find_by_name(panel_, "row_version");
    if (version_row) {
        version_value_ = lv_obj_find_by_name(version_row, "value");
        if (version_value_) {
            lv_label_set_text(version_value_, HELIX_VERSION);
            spdlog::debug("[{}]   ✓ Version: {}", get_name(), HELIX_VERSION);
        }
    }

    // === Printer Name (from PrinterState or Config) ===
    lv_obj_t* printer_row = lv_obj_find_by_name(panel_, "row_printer");
    if (printer_row) {
        printer_value_ = lv_obj_find_by_name(printer_row, "value");
        if (printer_value_) {
            // Try to get printer name from config
            Config* config = Config::get_instance();
            std::string printer_name =
                config->get<std::string>(config->df() + "printer_name", "Unknown");
            lv_label_set_text(printer_value_, printer_name.c_str());
            spdlog::debug("[{}]   ✓ Printer: {}", get_name(), printer_name);
        }
    }

    // === Klipper Version (reactive binding from PrinterState) ===
    lv_obj_t* klipper_row = lv_obj_find_by_name(panel_, "row_klipper");
    if (klipper_row) {
        klipper_value_ = lv_obj_find_by_name(klipper_row, "value");
        if (klipper_value_) {
            // Bind to reactive subject - updates automatically after discovery
            lv_label_bind_text(klipper_value_, printer_state_.get_klipper_version_subject(), "%s");
            spdlog::debug("[{}]   ✓ Klipper version bound to subject", get_name());
        }
    }

    // === Moonraker Version (reactive binding from PrinterState) ===
    lv_obj_t* moonraker_row = lv_obj_find_by_name(panel_, "row_moonraker");
    if (moonraker_row) {
        moonraker_value_ = lv_obj_find_by_name(moonraker_row, "value");
        if (moonraker_value_) {
            // Bind to reactive subject - updates automatically after discovery
            lv_label_bind_text(moonraker_value_, printer_state_.get_moonraker_version_subject(), "%s");
            spdlog::debug("[{}]   ✓ Moonraker version bound to subject", get_name());
        }
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SettingsPanel::handle_dark_mode_changed(bool enabled) {
    spdlog::info("[{}] Dark mode toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_dark_mode(enabled);
}

void SettingsPanel::handle_display_sleep_changed(int index) {
    int seconds = SettingsManager::index_to_sleep_seconds(index);
    spdlog::info("[{}] Display sleep changed: index {} = {}s", get_name(), index, seconds);
    SettingsManager::instance().set_display_sleep_sec(seconds);
}

void SettingsPanel::handle_led_light_changed(bool enabled) {
    spdlog::info("[{}] LED light toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_led_enabled(enabled);
}

void SettingsPanel::handle_sounds_changed(bool enabled) {
    spdlog::info("[{}] Sounds toggled: {} (placeholder)", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_sounds_enabled(enabled);
}

void SettingsPanel::handle_completion_alert_changed(bool enabled) {
    spdlog::info("[{}] Completion alert toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_completion_alert(enabled);
}

void SettingsPanel::handle_display_settings_clicked() {
    spdlog::debug("[{}] Display Settings clicked - opening overlay", get_name());

    // Create display settings overlay on first access (lazy initialization)
    if (!display_settings_overlay_ && parent_screen_) {
        spdlog::debug("[{}] Creating display settings overlay...", get_name());

        // Create from XML - component name matches filename
        display_settings_overlay_ = static_cast<lv_obj_t*>(
            lv_xml_create(parent_screen_, "display_settings_overlay", nullptr));
        if (display_settings_overlay_) {
            // Wire up back button
            lv_obj_t* header = lv_obj_find_by_name(display_settings_overlay_, "overlay_header");
            if (header) {
                lv_obj_t* back_btn = lv_obj_find_by_name(header, "back_button");
                if (back_btn) {
                    lv_obj_add_event_cb(
                        back_btn,
                        [](lv_event_t*) { ui_nav_go_back(); },
                        LV_EVENT_CLICKED, nullptr);
                }
            }

            // Wire up brightness slider
            lv_obj_t* brightness_slider =
                lv_obj_find_by_name(display_settings_overlay_, "brightness_slider");
            lv_obj_t* brightness_label =
                lv_obj_find_by_name(display_settings_overlay_, "brightness_value_label");
            if (brightness_slider && brightness_label) {
                // Set initial value from settings
                int brightness = SettingsManager::instance().get_brightness();
                lv_slider_set_value(brightness_slider, brightness, LV_ANIM_OFF);
                lv_label_set_text_fmt(brightness_label, "%d%%", brightness);

                // Store label pointer for callback
                lv_obj_set_user_data(brightness_slider, brightness_label);

                // Wire up value change
                lv_obj_add_event_cb(
                    brightness_slider,
                    [](lv_event_t* e) {
                        auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                        int value = lv_slider_get_value(slider);
                        SettingsManager::instance().set_brightness(value);

                        // Update label
                        auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(slider));
                        if (label) {
                            lv_label_set_text_fmt(label, "%d%%", value);
                        }
                    },
                    LV_EVENT_VALUE_CHANGED, nullptr);
            }

            // Wire up timeout preset buttons
            static constexpr struct {
                const char* name;
                int seconds;
            } timeouts[] = {
                {"timeout_never", 0},
                {"timeout_1min", 60},
                {"timeout_5min", 300},
                {"timeout_10min", 600},
                {"timeout_30min", 1800},
            };

            for (const auto& t : timeouts) {
                lv_obj_t* btn = lv_obj_find_by_name(display_settings_overlay_, t.name);
                if (btn) {
                    // Store timeout value as user data (cast int to pointer)
                    lv_obj_set_user_data(btn, reinterpret_cast<void*>(static_cast<intptr_t>(t.seconds)));
                    lv_obj_add_event_cb(
                        btn,
                        [](lv_event_t* e) {
                            auto* button = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                            int seconds = static_cast<int>(
                                reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
                            SettingsManager::instance().set_display_sleep_sec(seconds);
                            spdlog::info("Display sleep set to {}s", seconds);
                        },
                        LV_EVENT_CLICKED, nullptr);
                }
            }

            // Initially hidden
            lv_obj_add_flag(display_settings_overlay_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] Display settings overlay created", get_name());
        } else {
            spdlog::error("[{}] Failed to create display settings overlay from XML", get_name());
            return;
        }
    }

    // Push overlay onto navigation history and show it
    if (display_settings_overlay_) {
        ui_nav_push_overlay(display_settings_overlay_);
    }
}

void SettingsPanel::handle_bed_mesh_clicked() {
    spdlog::debug("[{}] Bed Mesh clicked - opening visualization", get_name());

    // Create bed mesh panel on first access (lazy initialization)
    if (!bed_mesh_panel_ && parent_screen_) {
        spdlog::debug("[{}] Creating bed mesh visualization panel...", get_name());

        // Create from XML
        bed_mesh_panel_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "bed_mesh_panel", nullptr));
        if (bed_mesh_panel_) {
            // Setup event handlers and renderer (class-based API)
            get_global_bed_mesh_panel().setup(bed_mesh_panel_, parent_screen_);

            // Initially hidden
            lv_obj_add_flag(bed_mesh_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] Bed mesh visualization panel created", get_name());
        } else {
            spdlog::error("[{}] Failed to create bed mesh panel from XML", get_name());
            return;
        }
    }

    // Push bed mesh panel onto navigation history and show it
    if (bed_mesh_panel_) {
        ui_nav_push_overlay(bed_mesh_panel_);
    }
}

void SettingsPanel::handle_z_offset_clicked() {
    spdlog::debug("[{}] Z-Offset clicked - opening calibration panel", get_name());

    // Create Z-Offset calibration panel on first access (lazy initialization)
    if (!zoffset_cal_panel_ && parent_screen_) {
        spdlog::debug("[{}] Creating Z-Offset calibration panel...", get_name());

        // Create from XML
        zoffset_cal_panel_ = static_cast<lv_obj_t*>(
            lv_xml_create(parent_screen_, "calibration_zoffset_panel", nullptr));
        if (zoffset_cal_panel_) {
            // Setup event handlers (class-based API)
            MoonrakerClient* client = get_moonraker_client();
            get_global_zoffset_cal_panel().setup(zoffset_cal_panel_, parent_screen_, client);

            // Initially hidden
            lv_obj_add_flag(zoffset_cal_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] Z-Offset calibration panel created", get_name());
        } else {
            spdlog::error("[{}] Failed to create Z-Offset panel from XML", get_name());
            return;
        }
    }

    // Push Z-Offset panel onto navigation history and show it
    if (zoffset_cal_panel_) {
        ui_nav_push_overlay(zoffset_cal_panel_);
    }
}

void SettingsPanel::handle_pid_tuning_clicked() {
    spdlog::debug("[{}] PID Tuning clicked - opening calibration panel", get_name());

    // Create PID calibration panel on first access (lazy initialization)
    if (!pid_cal_panel_ && parent_screen_) {
        spdlog::debug("[{}] Creating PID calibration panel...", get_name());

        // Create from XML
        pid_cal_panel_ = static_cast<lv_obj_t*>(
            lv_xml_create(parent_screen_, "calibration_pid_panel", nullptr));
        if (pid_cal_panel_) {
            // Setup event handlers (class-based API)
            MoonrakerClient* client = get_moonraker_client();
            get_global_pid_cal_panel().setup(pid_cal_panel_, parent_screen_, client);

            // Initially hidden
            lv_obj_add_flag(pid_cal_panel_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] PID calibration panel created", get_name());
        } else {
            spdlog::error("[{}] Failed to create PID panel from XML", get_name());
            return;
        }
    }

    // Push PID panel onto navigation history and show it
    if (pid_cal_panel_) {
        ui_nav_push_overlay(pid_cal_panel_);
    }
}

void SettingsPanel::handle_network_clicked() {
    spdlog::debug("[{}] Network Settings clicked", get_name());

    // Create network settings overlay on first access (lazy initialization)
    if (!network_settings_overlay_ && parent_screen_) {
        spdlog::debug("[{}] Creating network settings overlay...", get_name());

        // Create from XML
        network_settings_overlay_ = static_cast<lv_obj_t*>(
            lv_xml_create(parent_screen_, "network_settings_overlay", nullptr));

        if (network_settings_overlay_) {
            // Wire up header bar back button to use nav stack
            lv_obj_t* header = lv_obj_find_by_name(network_settings_overlay_, "overlay_header");
            if (header) {
                lv_obj_t* back_btn = lv_obj_find_by_name(header, "back_button");
                if (back_btn) {
                    lv_obj_add_event_cb(
                        back_btn,
                        [](lv_event_t*) { ui_nav_go_back(); },
                        LV_EVENT_CLICKED, nullptr);
                }
            }

            // Wire up Scan button
            lv_obj_t* scan_btn = lv_obj_find_by_name(network_settings_overlay_, "scan_btn");
            if (scan_btn) {
                lv_obj_add_event_cb(
                    scan_btn,
                    [](lv_event_t*) {
                        spdlog::info("[SettingsPanel] Network scan requested");
                        // TODO: Trigger WiFiManager scan
                        // WiFiManager::instance().start_scan(...);
                    },
                    LV_EVENT_CLICKED, nullptr);
            }

            // Wire up Disconnect button
            lv_obj_t* disconnect_btn =
                lv_obj_find_by_name(network_settings_overlay_, "disconnect_btn");
            if (disconnect_btn) {
                lv_obj_add_event_cb(
                    disconnect_btn,
                    [](lv_event_t*) {
                        spdlog::info("[SettingsPanel] WiFi disconnect requested");
                        // TODO: Disconnect via WiFiManager
                        // WiFiManager::instance().disconnect();
                    },
                    LV_EVENT_CLICKED, nullptr);
            }

            // Initially hidden
            lv_obj_add_flag(network_settings_overlay_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] Network settings overlay created", get_name());
        } else {
            spdlog::error("[{}] Failed to create network settings overlay from XML", get_name());
            return;
        }
    }

    // Push overlay onto navigation history and show it with slide animation
    if (network_settings_overlay_) {
        ui_nav_push_overlay(network_settings_overlay_);

        // TODO: Update connection status display
        // Update connected_ssid, connected_ip labels based on WiFiManager state
        // Show/hide connected_info vs disconnected_info based on connection state
    }
}

void SettingsPanel::handle_factory_reset_clicked() {
    spdlog::debug("[{}] Factory Reset clicked - showing confirmation dialog", get_name());

    // Create factory reset dialog on first access (lazy initialization)
    if (!factory_reset_dialog_ && parent_screen_) {
        spdlog::debug("[{}] Creating factory reset dialog...", get_name());

        // Create from XML - component name matches filename
        factory_reset_dialog_ = static_cast<lv_obj_t*>(
            lv_xml_create(parent_screen_, "factory_reset_dialog", nullptr));
        if (factory_reset_dialog_) {
            // Wire up Cancel button - just hide the dialog
            lv_obj_t* cancel_btn = lv_obj_find_by_name(factory_reset_dialog_, "cancel_btn");
            if (cancel_btn) {
                // Store dialog pointer for callback
                lv_obj_set_user_data(cancel_btn, factory_reset_dialog_);
                lv_obj_add_event_cb(
                    cancel_btn,
                    [](lv_event_t* e) {
                        auto* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                        auto* dialog = static_cast<lv_obj_t*>(lv_obj_get_user_data(btn));
                        if (dialog) {
                            lv_obj_add_flag(dialog, LV_OBJ_FLAG_HIDDEN);
                        }
                        spdlog::debug("[SettingsPanel] Factory reset cancelled");
                    },
                    LV_EVENT_CLICKED, nullptr);
            }

            // Wire up Reset button - perform factory reset
            lv_obj_t* reset_btn = lv_obj_find_by_name(factory_reset_dialog_, "reset_btn");
            if (reset_btn) {
                lv_obj_add_event_cb(
                    reset_btn,
                    [](lv_event_t*) {
                        spdlog::warn("[SettingsPanel] Factory reset confirmed - resetting config!");

                        // Get config instance and reset
                        Config* config = Config::get_instance();
                        if (config) {
                            // Reset to defaults by clearing the config
                            config->reset_to_defaults();
                            config->save();
                            spdlog::info("[SettingsPanel] Config reset to defaults");
                        }

                        // TODO: In production, this would restart the application
                        // or transition to the setup wizard. For now, just log.
                        spdlog::info("[SettingsPanel] Device should restart or show wizard now");

                        // For development: show a toast or message
                        // In production: call system restart or show wizard
                    },
                    LV_EVENT_CLICKED, nullptr);
            }

            // Also allow clicking backdrop to cancel
            lv_obj_add_event_cb(
                factory_reset_dialog_,
                [](lv_event_t* e) {
                    // Only cancel if clicked directly on backdrop, not on children
                    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                    auto* original = static_cast<lv_obj_t*>(lv_event_get_target(e));
                    if (target == original) {
                        lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
                        spdlog::debug("[SettingsPanel] Factory reset cancelled (backdrop click)");
                    }
                },
                LV_EVENT_CLICKED, nullptr);

            // Start hidden
            lv_obj_add_flag(factory_reset_dialog_, LV_OBJ_FLAG_HIDDEN);
            spdlog::info("[{}] Factory reset dialog created", get_name());
        } else {
            spdlog::error("[{}] Failed to create factory reset dialog from XML", get_name());
            return;
        }
    }

    // Show the dialog
    if (factory_reset_dialog_) {
        lv_obj_remove_flag(factory_reset_dialog_, LV_OBJ_FLAG_HIDDEN);
        // Ensure dialog is on top
        lv_obj_move_foreground(factory_reset_dialog_);
    }
}

// ============================================================================
// STATIC TRAMPOLINES
// ============================================================================

void SettingsPanel::on_dark_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_dark_mode_changed");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self && self->dark_mode_switch_) {
        bool enabled = lv_obj_has_state(self->dark_mode_switch_, LV_STATE_CHECKED);
        self->handle_dark_mode_changed(enabled);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_display_sleep_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_display_sleep_changed");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self && self->display_sleep_dropdown_) {
        int index = lv_dropdown_get_selected(self->display_sleep_dropdown_);
        self->handle_display_sleep_changed(index);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_led_light_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_led_light_changed");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self && self->led_light_switch_) {
        bool enabled = lv_obj_has_state(self->led_light_switch_, LV_STATE_CHECKED);
        self->handle_led_light_changed(enabled);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_sounds_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_sounds_changed");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self && self->sounds_switch_) {
        bool enabled = lv_obj_has_state(self->sounds_switch_, LV_STATE_CHECKED);
        self->handle_sounds_changed(enabled);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_completion_alert_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_completion_alert_changed");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self && self->completion_alert_switch_) {
        bool enabled = lv_obj_has_state(self->completion_alert_switch_, LV_STATE_CHECKED);
        self->handle_completion_alert_changed(enabled);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_display_settings_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_display_settings_clicked");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_display_settings_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_bed_mesh_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_bed_mesh_clicked");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_bed_mesh_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_z_offset_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_z_offset_clicked");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_z_offset_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_pid_tuning_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_pid_tuning_clicked");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_pid_tuning_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_network_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_network_clicked");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_network_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_factory_reset_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_factory_reset_clicked");
    auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
    if (self) {
        self->handle_factory_reset_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<SettingsPanel> g_settings_panel;

SettingsPanel& get_global_settings_panel() {
    if (!g_settings_panel) {
        g_settings_panel = std::make_unique<SettingsPanel>(get_printer_state(), nullptr);
    }
    return *g_settings_panel;
}
