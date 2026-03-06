// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xml_registration.h"

#include "ui_ams_current_tool.h"
#include "ui_ams_device_operations_overlay.h"
#include "ui_ams_device_section_detail_overlay.h"
#include "ui_button.h"
#include "ui_carousel.h"
#include "ui_confetti.h"
#include "ui_fan_dial.h"
#include "ui_overlay_temp_graph.h"
#include "ui_fonts.h"
#include "ui_gcode_viewer.h"
#include "ui_hsv_picker.h"
#include "ui_icon_codepoints.h"
#include "ui_markdown.h"
#include "ui_notification_badge.h"
#include "ui_panel_home.h"
#include "ui_panel_settings.h"
#include "ui_progress_bar.h"
#include "ui_spinner.h"
#include "ui_spool_canvas.h"
#include "ui_switch.h"
#include "ui_text.h"
#include "ui_text_input.h"
#include "ui_z_offset_indicator.h"

#include "layout_manager.h"
#include "static_subject_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix {

/**
 * No-op callback for optional event handlers in XML components.
 * When a component has an optional callback prop with default="",
 * LVGL tries to find a callback named "" which doesn't exist.
 * Registering this no-op callback silences those warnings.
 */
static void noop_event_callback(lv_event_t* /*e*/) {
    // Intentionally empty - used for optional callbacks that weren't provided
}

/**
 * No-op subject for optional subject bindings in XML components.
 * When a component has an optional subject prop with default="",
 * LVGL tries to find a subject named "" which doesn't exist.
 * Registering this no-op subject silences those warnings.
 */
static lv_subject_t s_noop_subject;
static bool s_noop_subject_initialized = false;

/**
 * Register responsive constants for color picker sizing based on screen dimensions
 * Call this BEFORE registering XML components that use the color picker
 */
static void register_color_picker_responsive_constants() {
    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Preview swatch size and text height scale with screen
    const char* preview_size;
    const char* preview_size_small;
    const char* text_height;
    const char* theme_swatch_size;
    if (greater_res <= UI_BREAKPOINT_SMALL_MAX) {
        preview_size = "40";
        preview_size_small = "20";
        text_height = "52";
        theme_swatch_size = "24";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) {
        preview_size = "48";
        preview_size_small = "24";
        text_height = "60";
        theme_swatch_size = "28";
    } else {
        preview_size = "56";
        preview_size_small = "28";
        text_height = "68";
        theme_swatch_size = "32";
    }

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (scope) {
        lv_xml_register_const(scope, "color_preview_size", preview_size);
        lv_xml_register_const(scope, "color_preview_size_small", preview_size_small);
        lv_xml_register_const(scope, "color_text_height", text_height);
        lv_xml_register_const(scope, "theme_swatch_size", theme_swatch_size);
        spdlog::debug(
            "[Color Picker] Registered color_preview_size={}, theme_swatch_size={} for screen {}px",
            preview_size, theme_swatch_size, greater_res);
    }
}

/**
 * Register responsive constants into the color_picker component scope.
 * Must be called AFTER register_xml("color_picker.xml") so the scope exists.
 */
static void register_color_picker_component_constants() {
    lv_display_t* display = lv_display_get_default();
    int32_t ver_res = lv_display_get_vertical_resolution(display);

    // Swatch size: smaller on compact screens
    const char* swatch_size = ver_res <= UI_BREAKPOINT_SMALL_MAX ? "28" : "32";

    // HSV picker: size proportionally to screen height
    // On TINY (full-screen modal), chrome is ~142px (header+tabs+padding+dividers+buttons)
    // On larger screens (modal popup), use ~38% of screen height
    static char sv_buf[8];
    static char hue_buf[8];
    int32_t computed_sv;
    if (ver_res <= UI_BREAKPOINT_TINY_MAX) {
        // Full-screen: fill available vertical space
        // Chrome: header(48) + divider(1) + tabs(36) + content pad(16) + spacer divider(1) + buttons(40)
        constexpr int32_t chrome = 142;
        int32_t available = ver_res - chrome;
        // sv_size + gap(4) + hue_height, where hue = sv/8
        computed_sv = (available - 4) * 8 / 9;
    } else {
        computed_sv = ver_res * 38 / 100;
    }
    computed_sv = LV_CLAMP(48, computed_sv, 240);
    int32_t computed_hue = LV_MAX(computed_sv / 9, 8);
    snprintf(sv_buf, sizeof(sv_buf), "%d", computed_sv);
    snprintf(hue_buf, sizeof(hue_buf), "%d", computed_hue);

    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("color_picker");
    if (scope) {
        lv_xml_register_const(scope, "swatch_size", swatch_size);
        lv_xml_register_const(scope, "sv_size", sv_buf);
        lv_xml_register_const(scope, "hue_height", hue_buf);
        spdlog::debug("[Color Picker] Registered swatch_size={}, sv_size={}, hue_height={} "
                      "for height {}px",
                      swatch_size, sv_buf, hue_buf, ver_res);
    }
}

/**
 * Toggle password visibility on a sibling textarea.
 * Finds "password_input" by walking up to the shared parent container,
 * then swaps the eye/eye_off icon on the button.
 */
static void on_toggle_password_visibility(lv_event_t* e) {
    auto* btn = (lv_obj_t*)lv_event_get_target(e);
    auto* container = lv_obj_get_parent(btn);
    if (!container)
        return;

    auto* textarea = (lv_obj_t*)lv_obj_find_by_name(container, "password_input");
    if (!textarea)
        return;

    bool was_password = lv_textarea_get_password_mode(textarea);
    lv_textarea_set_password_mode(textarea, !was_password);

    // Swap icon: eye_off when hidden (password mode), eye when visible
    auto* icon = (lv_obj_t*)lv_obj_find_by_name(btn, "eye_toggle_icon");
    if (icon) {
        const char* cp = ui_icon::lookup_codepoint(was_password ? "eye" : "eye_off");
        if (cp)
            lv_label_set_text(icon, cp);
    }
}

static void register_xml(const char* filename) {
    auto& lm = helix::LayoutManager::instance();
    std::string path = "A:" + lm.resolve_xml_path(filename);
    lv_xml_register_component_from_file(path.c_str());
}

void register_xml_components() {
    spdlog::trace("[XML Registration] Registering XML components...");

    // Register responsive constants (AFTER globals, BEFORE components that use them)
    ui_switch_register_responsive_constants();
    register_color_picker_responsive_constants();

    // Register semantic text widgets (AFTER theme init, BEFORE components that use them)
    ui_text_init();
    ui_text_input_init();         // <text_input> with bind_text support
    ui_spinner_init();            // <spinner> with responsive sizing
    ui_button_init();             // <ui_button> with variant styles and auto-contrast
    ui_markdown_init();           // <ui_markdown> with theme-aware markdown rendering
    ui_notification_badge_init(); // <notification_badge> with auto-contrast text
    ui_carousel_init();           // <ui_carousel> horizontal scroll-snap carousel
    register_xml("carousel.xml"); // <carousel> XML component wrapping ui_carousel
    ui_confetti_init();           // <ui_confetti> celebration animation canvas

    // Register no-op callback and subject for optional handlers in XML components
    // This silences warnings when components use callback/subject props with default=""
    lv_xml_register_event_cb(nullptr, "", noop_event_callback);

    // Global utility callbacks used by multiple components
    lv_xml_register_event_cb(nullptr, "on_toggle_password_visibility",
                             on_toggle_password_visibility);
    lv_xml_register_event_cb(nullptr, "on_edit_done_clicked",
                             [](lv_event_t*) { get_global_home_panel().exit_grid_edit_mode(); });
    lv_xml_register_event_cb(nullptr, "on_edit_add_widget_clicked",
                             [](lv_event_t*) { get_global_home_panel().open_widget_catalog(); });
    lv_subject_init_int(&s_noop_subject, 0);
    lv_xml_register_subject(nullptr, "", &s_noop_subject);
    s_noop_subject_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("XmlSubjects", helix::deinit_xml_subjects);

    // Register custom widgets (BEFORE components that use them)
    ui_gcode_viewer_register();
    ui_spool_canvas_register();       // Needed by Spoolman panel (and AMS panel)
    ui_hsv_picker_register();         // HSV color picker for edit filament modal
    ui_z_offset_indicator_register(); // Z-offset nozzle indicator
    ui_ams_current_tool_init();       // AMS current tool indicator callbacks
    // NOTE: Other AMS widgets (ams_slot, filament_path_canvas) are
    // registered lazily in ui_panel_ams.cpp when the AMS panel is first accessed

    // AMS edit modal (MUST be after spool_canvas and hsv_picker registration)
    // Registered globally so FilamentPanel can use it without AMS panel lazy init
    register_xml("spoolman_spool_item.xml");
    register_xml("ams_edit_modal.xml");

    // Spoolman components (MUST be after spool_canvas registration)
    register_xml("spoolman_spool_row.xml");
    register_xml("spoolman_context_menu.xml");
    register_xml("spoolman_edit_modal.xml");
    register_xml("spoolman_panel.xml");

    // Spool wizard components
    register_xml("wizard_vendor_row.xml");
    register_xml("wizard_filament_row.xml");
    register_xml("create_vendor_modal.xml");
    register_xml("create_filament_modal.xml");
    register_xml("spool_wizard.xml");

    // Core UI components
    register_xml("icon.xml");
    register_xml("status_pill.xml");
    register_xml("filament_sensor_indicator.xml");
    register_xml("humidity_indicator.xml");
    register_xml("width_indicator.xml");
    register_xml("probe_indicator.xml");
    register_xml("filament_sensor_row.xml");
    register_xml("temp_display.xml");
    register_xml("components/nozzle_icon.xml");
    register_xml("header_bar.xml");
    register_xml("overlay_backdrop.xml");
    register_xml("overlay_panel.xml");
    register_xml("widget_catalog_overlay.xml");
    register_xml("toast_notification.xml");

    // Utility components (dividers, button rows, headers - used by modals and other components)
    register_xml("centered_column.xml");
    register_xml("divider_horizontal.xml");
    register_xml("divider_vertical.xml");
    register_xml("modal_button_row.xml");
    register_xml("modal_header.xml");
    register_xml("empty_state.xml");
    register_xml("connecting_state.xml");
    register_xml("info_note.xml");
    register_xml("form_field.xml");
    register_xml("ui_multiselect.xml");

    // Shared progress bar component (gradient indicator)
    ui_progress_bar_init();
    register_xml("components/progress_bar.xml");

    // Beta feature indicators (badge before wrapper - dependency order)
    register_xml("beta_badge.xml");
    register_xml("beta_feature.xml");

    // emergency_stop_button.xml removed - E-Stop buttons are now embedded in panels
    register_xml("estop_confirmation_dialog.xml");
    register_xml("klipper_recovery_dialog.xml");
    register_xml("print_cancel_confirm_modal.xml");
    register_xml("print_completion_modal.xml");
    register_xml("save_z_offset_modal.xml");
    register_xml("exclude_object_modal.xml");

    // Notification history
    register_xml("notification_history_panel.xml");
    register_xml("notification_history_item.xml");

    // Modal dialogs
    register_xml("crash_report_modal.xml");
    register_xml("debug_bundle_modal.xml");
    register_xml("modal_dialog.xml");
    register_xml("numeric_keypad_panel.xml");
    register_xml("runout_guidance_modal.xml");
    register_xml("shutdown_modal.xml");
    register_xml("plugin_install_modal.xml");
    register_xml("macro_enhance_modal.xml");
    register_xml("action_prompt_modal.xml");
    register_xml("info_qr_modal.xml");
    register_xml("color_picker.xml");
    register_color_picker_component_constants();

    // Print file components
    register_xml("print_file_card.xml");
    register_xml("print_file_list_row.xml");
    register_xml("print_file_detail.xml");

    // Panel widget components (dynamic instantiation from PanelWidgetConfig)
    register_xml("components/panel_widget_printer_image.xml");
    register_xml("components/panel_widget_power.xml");
    register_xml("components/panel_widget_network.xml");
    register_xml("components/panel_widget_notifications.xml");
    register_xml("components/panel_widget_firmware_restart.xml");
    register_xml("components/panel_widget_ams.xml");
    register_xml("components/panel_widget_camera.xml");
    register_xml("components/camera_fullscreen.xml");
    register_xml("components/panel_widget_temperature.xml");
    register_xml("components/panel_widget_temp_stack.xml");
    register_xml("components/panel_widget_temp_carousel.xml");
    register_xml("components/panel_widget_led.xml");
    register_xml("components/panel_widget_led_controls.xml");
    register_xml("components/panel_widget_humidity.xml");
    register_xml("components/panel_widget_width_sensor.xml");
    register_xml("components/panel_widget_filament.xml");
    register_xml("components/panel_widget_thermistor.xml");
    register_xml("components/panel_widget_thermistor_carousel.xml");
    register_xml("components/panel_widget_fan_stack.xml");
    register_xml("components/panel_widget_fan_carousel.xml");
    register_xml("components/panel_widget_favorite_macro.xml");
    register_xml("components/panel_widget_clock.xml");
    register_xml("components/panel_widget_tips.xml");
    register_xml("components/panel_widget_print_status.xml");
    register_xml("components/panel_widget_shutdown.xml");
    register_xml("components/panel_widget_job_queue.xml");
    register_xml("components/clog_meter_page.xml");
    register_xml("components/panel_widget_clog_detection.xml");
    register_xml("components/panel_widget_print_stats.xml");
    register_xml("components/panel_widget_gcode_console.xml");
    register_xml("components/clog_detection_config_modal.xml");
    register_xml("components/buffer_status_modal.xml");
    register_xml("job_queue_modal.xml");
    register_xml("thermistor_sensor_picker.xml");
    register_xml("thermistor_configure_picker.xml");
    register_xml("favorite_macro_picker.xml");
    register_xml("macro_param_modal.xml");

    // Main navigation and panels
    register_xml("navigation_bar.xml");
    register_xml("home_panel.xml");
    register_xml("controls_panel.xml");
    register_xml("motion_panel.xml");
    // TODO: Remove these old per-heater overlays once application.cpp's
    // --overlays command-line paths and TempControlPanel::xml_component_name()
    // are updated to use the unified TempGraphOverlay instead.
    register_xml("nozzle_temp_panel.xml");
    register_xml("bed_temp_panel.xml");
    register_xml("chamber_temp_panel.xml");
    register_xml("temp_graph_overlay.xml");
    // Register TempGraphOverlay event callbacks at startup (before XML is parsed)
    lv_xml_register_event_cb(nullptr, "on_temp_graph_preset_clicked",
                             TempGraphOverlay::on_temp_graph_preset_clicked);
    lv_xml_register_event_cb(nullptr, "on_temp_graph_custom_clicked",
                             TempGraphOverlay::on_temp_graph_custom_clicked);
    register_xml("fan_arc_core.xml");
    register_xml("fan_dial.xml");
    register_fan_dial_callbacks(); // Register FanDial event callbacks
    register_xml("fan_status_card.xml");
    register_xml("fan_control_overlay.xml");
    register_xml("led_action_chip.xml");
    register_xml("led_color_swatch.xml");
    register_xml("led_control_overlay.xml");
    register_xml("ams_current_tool.xml");
    register_xml("exclude_objects_list_overlay.xml");
    register_xml("print_status_panel.xml");
    register_xml("print_tune_panel.xml");
    register_xml("filament_panel.xml");

    // NOTE: AMS panel (ams_panel.xml) is registered lazily in ui_panel_ams.cpp
    // AMS Device Operations (accessed from Settings > AMS)
    helix::ui::get_ams_device_operations_overlay().register_callbacks();
    register_xml("ams_device_operations.xml");
    helix::ui::get_ams_device_section_detail_overlay().register_callbacks();
    register_xml("ams_device_section_detail.xml");

    // Spoolman Settings (accessed from Settings > Spoolman, future)
    register_xml("ams_settings_spoolman.xml");

    // Feature parity panels
    register_xml("macro_card.xml");
    register_xml("macro_panel.xml");
    register_xml("console_panel.xml");
    register_xml("power_device_row.xml");
    register_xml("power_panel.xml");
    register_xml("screws_tilt_panel.xml");
    register_xml("input_shaper_panel.xml");

    // Print history panels
    register_xml("history_list_row.xml");
    register_xml("history_list_panel.xml");
    register_xml("history_detail_overlay.xml");
    register_xml("history_dashboard_panel.xml");

    // Settings components (must be registered before settings_panel)
    register_xml("setting_section_header.xml");
    register_xml("setting_toggle_row.xml");
    register_xml("setting_dropdown_row.xml");
    register_xml("setting_action_row.xml");
    register_xml("setting_info_row.xml");
    register_xml("setting_slider_row.xml");
    register_xml("setting_led_chip_row.xml");
    register_xml("setting_state_row.xml");
    register_xml("setting_detail_panel.xml");
    register_xml("setting_form_dropdown.xml");
    register_xml("setting_form_input.xml");
    register_xml("setting_macro_card.xml");
    register_settings_panel_callbacks(); // Register callbacks before XML parse [L013]
    register_xml("settings_panel.xml");
    register_xml("restart_prompt_dialog.xml");
    register_xml("factory_reset_modal.xml");
    register_xml("update_download_modal.xml");
    register_xml("update_notify_modal.xml");
    register_xml("change_host_modal.xml");

    // Calibration panels (overlays launched from settings)
    register_xml("calibration_zoffset_panel.xml");
    register_xml("calibration_pid_panel.xml");

    // Bed mesh modals (must be registered before bed_mesh_panel which uses them)
    register_xml("bed_mesh_calibrate_modal.xml");
    register_xml("bed_mesh_rename_modal.xml");
    register_xml("bed_mesh_save_config_modal.xml");
    register_xml("bed_mesh_panel.xml");

    // Settings overlay panels
    register_xml("display_settings_overlay.xml");
register_xml("sound_settings_overlay.xml");
    register_xml("label_printer_settings.xml");
    register_xml("led_settings_overlay.xml");
    register_xml("theme_editor_overlay.xml");
    register_xml("theme_preview_overlay.xml");
    register_xml("theme_save_as_modal.xml");
    register_xml("sensors_overlay.xml");
    // Probe type-specific panels (registered before probe_overlay)
    register_xml("probe_bltouch_panel.xml");
    register_xml("probe_overlay.xml");
    register_xml("macro_buttons_overlay.xml");
    register_xml("hardware_issue_row.xml");
    register_xml("hardware_health_overlay.xml");
    register_xml("network_settings_overlay.xml");
    register_xml("retraction_settings_overlay.xml");
    register_xml("machine_limits_overlay.xml");
    register_xml("timelapse_settings_overlay.xml");
    register_xml("timelapse_install_overlay.xml");
    register_xml("plugin_card.xml");
    register_xml("settings_plugins_overlay.xml");
    register_xml("touch_calibration_overlay.xml");
    register_xml("printer_image_list_item.xml");
    register_xml("printer_image_overlay.xml");
    register_xml("hidden_network_modal.xml");
    register_xml("network_test_modal.xml");
    register_xml("filament_preset_edit_modal.xml");
    register_xml("wifi_network_item.xml");
    register_xml("telemetry_data_overlay.xml");
    register_xml("about_settings_overlay.xml");
    register_xml("material_temps_overlay.xml");

    // Printer manager overlay (launched from home screen printer image)
    register_xml("printer_manager_overlay.xml");

    // Development tools
    register_xml("memory_stats_overlay.xml");

    // Additional panels
    register_xml("advanced_panel.xml");
    register_xml("test_panel.xml");
    register_xml("print_select_panel.xml");
    register_xml("gcode_test_panel.xml");
    register_xml("glyphs_panel.xml");

    // App layout
    register_xml("app_layout.xml");

    // Wizard components
    register_xml("wizard_touch_calibration.xml");
    register_xml("wizard_header_bar.xml");
    register_xml("wizard_container.xml");
    register_xml("network_list_item.xml");
    register_xml("wifi_password_modal.xml");
    register_xml("wizard_wifi_setup.xml");
    register_xml("wizard_connection.xml");
    register_xml("wizard_printer_identify.xml");
    register_xml("wizard_heater_select.xml");
    register_xml("wizard_fan_select.xml");
    register_xml("wizard_ams_identify.xml");
    register_xml("wizard_led_select.xml");
    register_xml("wizard_filament_sensor_select.xml");
    register_xml("wizard_probe_sensor_select.xml");
    register_xml("wizard_input_shaper.xml");
    register_xml("wizard_language_chooser.xml");
    register_xml("wizard_summary.xml");
    register_xml("wizard_telemetry.xml");
    register_xml("telemetry_info_modal.xml");

    spdlog::trace("[XML Registration] XML component registration complete");
}

void deinit_xml_subjects() {
    if (s_noop_subject_initialized) {
        lv_subject_deinit(&s_noop_subject);
        s_noop_subject_initialized = false;
        spdlog::debug("[XML Registration] No-op subject deinitialized");
    }
}

} // namespace helix
