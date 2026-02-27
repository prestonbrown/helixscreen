// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_wizard.h"

#include "ui_error_reporting.h"
#include "ui_panel_home.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"
#include "ui_wizard_ams_identify.h"
#include "ui_wizard_connection.h"
#include "ui_wizard_fan_select.h"
#include "ui_wizard_filament_sensor_select.h"
#include "ui_wizard_heater_select.h"
#include "ui_wizard_input_shaper.h"
#include "ui_wizard_language_chooser.h"
#include "ui_wizard_led_select.h"
#include "ui_wizard_printer_identify.h"
#include "ui_wizard_probe_sensor_select.h"
#include "ui_wizard_summary.h"
#include "ui_wizard_touch_calibration.h"
#include "ui_wizard_wifi.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "filament_sensor_manager.h"
#include "hardware_validator.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "platform_info.h"
#include "runtime_config.h"
#include "static_panel_registry.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "wizard_config_paths.h"
#include "wizard_step_logic.h"

#include <spdlog/spdlog.h>

#include <cstdio>

using namespace helix;

// Subject declarations (static/global scope required)
static lv_subject_t current_step;
static lv_subject_t total_steps;
static lv_subject_t wizard_title;
static lv_subject_t wizard_step_current;  // String for display, e.g., "1"
static lv_subject_t wizard_step_total;    // String for display, e.g., "7"
static lv_subject_t wizard_is_final_step; // Int: 0=not final, 1=final (for button visibility)
static lv_subject_t wizard_back_visible;

// Non-static: accessible from other wizard step files
lv_subject_t connection_test_passed; // Global: 0=connection not validated, 1=validated or N/A
lv_subject_t wizard_subtitle;        // Global: accessible for dynamic subtitle updates
lv_subject_t wizard_show_skip;       // Global: 0=show Next, 1=show Skip (for touch calibration)

// SubjectManager for RAII cleanup of wizard subjects
static SubjectManager wizard_subjects_;

// String buffers (must be persistent)
static char wizard_title_buffer[64];
static char wizard_step_current_buffer[8];
static char wizard_step_total_buffer[8];
static char wizard_subtitle_buffer[128];

// Wizard container instance
static lv_obj_t* wizard_container = nullptr;

// Track current screen for proper cleanup (-1 = no screen loaded yet)
static int current_screen_step = -1;

// Track if touch calibration step (0) is being skipped - not fbdev or already calibrated
static bool touch_cal_step_skipped = false;

// Track if language step (1) is being skipped - language already set
static bool language_step_skipped = false;

// Track if WiFi step (2) is being skipped - Android manages WiFi natively
static bool wifi_step_skipped = false;

// Track if AMS step (7) is being skipped - no AMS detected
static bool ams_step_skipped = false;

// Track if LED step (8) is being skipped - no LEDs discovered
static bool led_step_skipped = false;

// Track if filament sensor step (9) is being skipped - <2 standalone sensors
static bool filament_step_skipped = false;

// Track if probe sensor step (10) is being skipped - no unassigned switch sensors
static bool probe_step_skipped = false;

// Track if input shaper step (11) is being skipped - no accelerometer
static bool input_shaper_step_skipped = false;

// Track if we've calculated the actual step total (happens after connection step)
static bool skips_precalculated = false;

// Guard against rapid double-clicks during navigation
static bool navigating = false;

// Forward declarations
static void on_back_clicked(lv_event_t* e);
static void on_next_clicked(lv_event_t* e);
static void ui_wizard_load_screen(int step);
static void ui_wizard_cleanup_current_screen();
static const char* get_step_title_from_xml(int step);
static const char* get_step_subtitle_from_xml(int step);
static void ui_wizard_precalculate_skips();

// ============================================================================
// Step Metadata (read from XML <consts>)
// ============================================================================

/**
 * Map step number to XML component name
 * Each component defines its own step_title in its <consts> block
 */
static const char* const STEP_COMPONENT_NAMES[] = {
    "wizard_touch_calibration",      // 0 (may be skipped on non-fbdev)
    "wizard_language_chooser",       // 1 (may be skipped if language already set)
    "wizard_wifi_setup",             // 2
    "wizard_connection",             // 3
    "wizard_printer_identify",       // 4
    "wizard_heater_select",          // 5
    "wizard_fan_select",             // 6
    "wizard_ams_identify",           // 7 (may be skipped if no AMS)
    "wizard_led_select",             // 8 (may be skipped if no LEDs)
    "wizard_filament_sensor_select", // 9 (may be skipped if <2 sensors)
    "wizard_probe_sensor_select",    // 10 (may be skipped if no unassigned sensors)
    "wizard_input_shaper",           // 11 (may be skipped if no accelerometer)
    "wizard_summary"                 // 12
};
static constexpr int STEP_COMPONENT_COUNT = 12; // Last step number (summary at step 12)

/**
 * Get step title from XML component's <consts> block
 *
 * Each wizard step XML file defines:
 *   <consts>
 *     <str name="step_title" value="WiFi Setup"/>
 *     <int name="step_order" value="1"/>
 *   </consts>
 *
 * This function reads step_title from the component's scope at runtime,
 * eliminating hardcoded title strings in C++.
 */
static const char* get_step_title_from_xml(int step) {
    if (step < 0 || step > STEP_COMPONENT_COUNT) {
        spdlog::warn("[Wizard] Invalid step {} for title lookup", step);
        return "Unknown Step";
    }

    const char* comp_name = STEP_COMPONENT_NAMES[step];
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(comp_name);
    if (!scope) {
        spdlog::warn("[Wizard] Component scope not found for '{}'", comp_name);
        return "Unknown Step";
    }

    const char* title = lv_xml_get_const(scope, "step_title");
    if (!title) {
        spdlog::warn("[Wizard] step_title not found in '{}' consts", comp_name);
        return "Unknown Step";
    }

    return title;
}

/**
 * Get step subtitle from XML component's <consts> block
 *
 * Subtitles provide contextual hints (e.g., "Skip if using Ethernet")
 * that appear below the title in the wizard header.
 */
static const char* get_step_subtitle_from_xml(int step) {
    if (step < 0 || step > STEP_COMPONENT_COUNT) {
        return "";
    }

    const char* comp_name = STEP_COMPONENT_NAMES[step];
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope(comp_name);
    if (!scope) {
        return "";
    }

    const char* subtitle = lv_xml_get_const(scope, "step_subtitle");
    return subtitle ? subtitle : "";
}

// Track if subjects have been initialized (to avoid double-deinit)
static bool wizard_subjects_initialized = false;

void ui_wizard_init_subjects() {
    spdlog::debug("[Wizard] Initializing subjects");

    // Initialize subjects with defaults using managed macros for RAII cleanup
    UI_MANAGED_SUBJECT_INT(current_step, 1, "current_step", wizard_subjects_);
    UI_MANAGED_SUBJECT_INT(total_steps, 11, "total_steps",
                           wizard_subjects_); // 11 steps: WiFi, Connection, Printer, Heater,
                                              // Fan, AMS, LED, Filament, Probe, Input Shaper,
                                              // Summary

    UI_MANAGED_SUBJECT_STRING(wizard_title, wizard_title_buffer, "Welcome", "wizard_title",
                              wizard_subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_step_current, wizard_step_current_buffer, "1",
                              "wizard_step_current", wizard_subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_step_total, wizard_step_total_buffer, "10",
                              "wizard_step_total", wizard_subjects_);
    UI_MANAGED_SUBJECT_INT(wizard_is_final_step, 0, "wizard_is_final_step", wizard_subjects_);
    UI_MANAGED_SUBJECT_STRING(wizard_subtitle, wizard_subtitle_buffer, "", "wizard_subtitle",
                              wizard_subjects_);

    // Initialize connection_test_passed to 1 (enabled by default for all steps)
    // Step 2 (connection) will set it to 0 until test passes
    UI_MANAGED_SUBJECT_INT(connection_test_passed, 1, "connection_test_passed", wizard_subjects_);

    // Initialize wizard_back_visible to 1 (visible by default)
    // Step navigation will hide it when at first visible step
    UI_MANAGED_SUBJECT_INT(wizard_back_visible, 1, "wizard_back_visible", wizard_subjects_);

    // Initialize wizard_show_skip to 0 (show Next by default)
    // Touch calibration step sets to 1 to show Skip button instead
    UI_MANAGED_SUBJECT_INT(wizard_show_skip, 0, "wizard_show_skip", wizard_subjects_);

    wizard_subjects_initialized = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticPanelRegistry::instance().register_destroy("WizardSubjects", ui_wizard_deinit_subjects);

    spdlog::debug("[Wizard] Subjects initialized ({} subjects registered)",
                  wizard_subjects_.count());
}

void ui_wizard_deinit_subjects() {
    if (!wizard_subjects_initialized) {
        return;
    }

    // Reset screen step tracking FIRST to prevent cleanup from accessing
    // already-destroyed wizard step objects. During StaticPanelRegistry::destroy_all(),
    // step objects (registered lazily after WizardSubjects) are destroyed first in LIFO
    // order. If cleanup calls their getters, the getter re-creates the object and calls
    // register_destroy(), invalidating the destroy_all() iterator → crash.
    // The step destructors already handled their own cleanup when their unique_ptrs were reset.
    current_screen_step = -1;

    // Delete wizard container BEFORE deinitializing subjects
    // This triggers proper widget cleanup: DELETE callbacks fire
    // and remove observers from subjects while subjects are still valid.
    // Without this, shutdown while on a wizard page would leave widgets
    // with observers pointing to deinitialized subjects, causing crashes
    // in lv_deinit() when those widgets are deleted.
    if (wizard_container && lv_is_initialized()) {
        spdlog::debug("[Wizard] Deleting wizard container during deinit");
        helix::ui::safe_delete(wizard_container);
        current_screen_step = -1;
    }

    // Use SubjectManager for RAII cleanup - handles all registered subjects
    wizard_subjects_.deinit_all();
    wizard_subjects_initialized = false;
    spdlog::debug("[Wizard] Subjects deinitialized");
}

// Helper type for constant name/value pairs
struct WizardConstant {
    const char* name;
    const char* value;
};

// Helper: Register array of constants to a scope
static void register_constants_to_scope(lv_xml_component_scope_t* scope,
                                        const WizardConstant* constants) {
    if (!scope)
        return;
    for (int i = 0; constants[i].name != nullptr; i++) {
        lv_xml_register_const(scope, constants[i].name, constants[i].value);
    }
}

void ui_wizard_container_register_responsive_constants() {
    spdlog::debug("[Wizard] Registering responsive constants to wizard_container scope");

    // Detect screen size using custom breakpoints
    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Determine button width based on breakpoint (only responsive constant remaining)
    const char* button_width;
    const char* size_label;

    if (greater_res <= UI_BREAKPOINT_SMALL_MAX) { // ≤480: 480x320
        button_width = "110";
        size_label = "SMALL";
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480
        button_width = "140";
        size_label = "MEDIUM";
    } else { // >800: 1024x600+
        button_width = "160";
        size_label = "LARGE";
    }

    spdlog::debug("[Wizard] Screen size: {} (greater_res={}px)", size_label, greater_res);

    // Register button width constant
    WizardConstant constants[] = {
        {"wizard_button_width", button_width}, {nullptr, nullptr} // Sentinel
    };

    // Register to wizard_container scope (parent)
    lv_xml_component_scope_t* parent_scope = lv_xml_component_get_scope("wizard_container");
    register_constants_to_scope(parent_scope, constants);

    // Define child components that inherit this constant
    const char* children[] = {
        "wizard_touch_calibration",
        "wizard_wifi_setup",
        "wizard_connection",
        "wizard_printer_identify",
        "wizard_heater_select",
        "wizard_fan_select",
        "wizard_ams_identify",
        "wizard_led_select",
        "wizard_filament_sensor_select",
        "wizard_probe_sensor_select",
        "wizard_input_shaper",
        "wizard_language_chooser",
        "wizard_summary",
        nullptr // Sentinel
    };

    // Propagate to all children
    int child_count = 0;
    for (int i = 0; children[i] != nullptr; i++) {
        lv_xml_component_scope_t* child_scope = lv_xml_component_get_scope(children[i]);
        if (child_scope) {
            register_constants_to_scope(child_scope, constants);
            child_count++;
        }
    }

    spdlog::debug(
        "[Wizard] Registered wizard_button_width={} to wizard_container and {} child components",
        button_width, child_count);
}

void ui_wizard_register_event_callbacks() {
    spdlog::debug("[Wizard] Registering event callbacks");
    lv_xml_register_event_cb(nullptr, "on_back_clicked", on_back_clicked);
    lv_xml_register_event_cb(nullptr, "on_next_clicked", on_next_clicked);
}

lv_obj_t* ui_wizard_create(lv_obj_t* parent) {
    spdlog::debug("[Wizard] Creating wizard container");

    // Create wizard from XML (constants already registered)
    wizard_container = (lv_obj_t*)lv_xml_create(parent, "wizard_container", nullptr);

    if (!wizard_container) {
        spdlog::error("[Wizard] Failed to create wizard_container from XML");
        return nullptr;
    }

    // Background color applied automatically by LVGL theme (uses theme->color_card)
    // No explicit styling needed - theme patching in ui_theme.cpp handles this

    // Update layout to ensure SIZE_CONTENT calculates correctly
    lv_obj_update_layout(wizard_container);

    spdlog::debug("[Wizard] Wizard container created successfully");
    return wizard_container;
}

/**
 * Build WizardSkipFlags from current static skip state
 */
static helix::WizardSkipFlags get_current_skip_flags() {
    return helix::WizardSkipFlags{
        .touch_cal = touch_cal_step_skipped,
        .language = language_step_skipped,
        .wifi = wifi_step_skipped,
        .ams = ams_step_skipped,
        .led = led_step_skipped,
        .filament = filament_step_skipped,
        .probe = probe_step_skipped,
        .input_shaper = input_shaper_step_skipped,
    };
}

/**
 * Calculate display step number and total, accounting for skipped steps
 * Delegates to tested wizard_step_logic functions
 */
static void calculate_display_step(int internal_step, int& out_display_step,
                                   int& out_display_total) {
    auto skips = get_current_skip_flags();
    out_display_step = helix::wizard_calculate_display_step(internal_step, skips);
    out_display_total = helix::wizard_calculate_display_total(skips);
}

void ui_wizard_navigate_to_step(int step) {
    spdlog::debug("[Wizard] Navigating to step {}", step);

    // Clamp step to valid range (internal steps are 0-9)
    if (step < 0)
        step = 0;
    if (step > STEP_COMPONENT_COUNT)
        step = STEP_COMPONENT_COUNT;

    // Reset skip flags when starting wizard from the beginning
    // This ensures correct behavior if wizard is restarted after hardware changes
    if (step == 0) {
        touch_cal_step_skipped = false;
        language_step_skipped = false;
        wifi_step_skipped = false;
        ams_step_skipped = false;
        led_step_skipped = false;
        filament_step_skipped = false;
        probe_step_skipped = false;
        input_shaper_step_skipped = false;
        skips_precalculated = false;

        // Auto-skip touch calibration step if not needed
        if (get_wizard_touch_calibration_step()->should_skip()) {
            touch_cal_step_skipped = true;
            step = 1;
            spdlog::info("[Wizard] Skipping touch calibration step");
        }

        // Auto-skip language step if language already set
        if (step == 1 && get_wizard_language_chooser_step()->should_skip()) {
            language_step_skipped = true;
            step = 2;
            spdlog::info("[Wizard] Skipping language step (already configured)");
        }

        // Auto-skip WiFi step on Android (WiFi managed by OS)
        if (step == 2 && helix::is_android_platform()) {
            wifi_step_skipped = true;
            step = 3;
            spdlog::info("[Wizard] Skipping WiFi step (Android platform)");
        }
    }

    // Calculate display step and total for progress indicator
    int display_step, display_total;
    calculate_display_step(step, display_step, display_total);

    // Update current_step subject (internal step number for UI bindings)
    lv_subject_set_int(&current_step, step);

    // Update Back button visibility based on whether we can go back
    // Find the first non-skipped step
    int min_step = 0;
    if (touch_cal_step_skipped)
        min_step = 1;
    if (min_step == 1 && language_step_skipped)
        min_step = 2;
    if (min_step == 2 && wifi_step_skipped)
        min_step = 3;
    lv_subject_set_int(&wizard_back_visible, (step > min_step) ? 1 : 0);

    // Determine if this is the last step (summary is always step 12 internally)
    bool is_last_step = (step == 12);

    // Update final step flag for button visibility binding
    lv_subject_set_int(&wizard_is_final_step, is_last_step ? 1 : 0);

    // Update progress display - step numbers as strings for bind_text
    snprintf(wizard_step_current_buffer, sizeof(wizard_step_current_buffer), "%d", display_step);
    lv_subject_copy_string(&wizard_step_current, wizard_step_current_buffer);

    if (skips_precalculated) {
        snprintf(wizard_step_total_buffer, sizeof(wizard_step_total_buffer), "%d", display_total);
        lv_subject_copy_string(&wizard_step_total, wizard_step_total_buffer);
    }

    // Load screen content (uses internal step number)
    ui_wizard_load_screen(step);

    // Force layout update on entire wizard after screen is loaded
    if (wizard_container) {
        lv_obj_update_layout(wizard_container);
    }

    // Allow next navigation click
    navigating = false;

    spdlog::debug("[Wizard] Updated to step {} of {} (internal: {}), final: {}", display_step,
                  display_total, step, is_last_step);
}

void ui_wizard_set_title(const char* title) {
    if (!title) {
        spdlog::warn("[Wizard] set_title called with nullptr, ignoring");
        return;
    }

    spdlog::debug("[Wizard] Setting title: {}", title);
    lv_subject_copy_string(&wizard_title, title);
}

void ui_wizard_refresh_header_translations() {
    // Re-translate and set the title/subtitle for the current step
    // Called after language changes to update bound subjects with new translations
    //
    // Note: Progress text ("Step X of Y") and buttons (Next/Finish) now use
    // translation_tag in XML, so they auto-refresh. Only title/subtitle need
    // manual refresh since they're step-specific and loaded from XML consts.
    int step = lv_subject_get_int(&current_step);
    const char* title = get_step_title_from_xml(step);
    const char* subtitle = get_step_subtitle_from_xml(step);

    lv_subject_copy_string(&wizard_title, lv_tr(title));
    lv_subject_copy_string(&wizard_subtitle, lv_tr(subtitle));

    spdlog::debug("[Wizard] Refreshed header translations for step {}", step);
}

/**
 * Pre-calculate which steps will be skipped based on hardware data
 *
 * Called after the connection step (step 3) completes so hardware data is available.
 * This ensures the step counter shows consistent totals from step 4 onwards.
 */
static void ui_wizard_precalculate_skips() {
    spdlog::info("[Wizard] Pre-calculating step skips based on hardware data");

    // Touch calibration (step 0) and language (step 1) are already handled at navigation time

    // AMS skip (step 7)
    if (!ams_step_skipped && get_wizard_ams_identify_step()->should_skip()) {
        ams_step_skipped = true;
        spdlog::info("[Wizard] Pre-skip: AMS step will be skipped");
    }

    // LED skip (step 8)
    if (!led_step_skipped && get_wizard_led_select_step()->should_skip()) {
        led_step_skipped = true;
        spdlog::info("[Wizard] Pre-skip: LED step will be skipped");
    }

    // Ensure FilamentSensorManager is populated before skip checks
    auto& fsm = helix::FilamentSensorManager::instance();
    if (fsm.get_sensors().empty()) {
        MoonrakerAPI* api = get_moonraker_api();
        if (api && api->hardware().has_filament_sensors()) {
            fsm.discover_sensors(api->hardware().filament_sensor_names());
            spdlog::debug("[Wizard] Populated FilamentSensorManager for skip calculation");
        }
    }

    // Filament sensor skip (step 9)
    if (!filament_step_skipped && get_wizard_filament_sensor_select_step()->should_skip()) {
        filament_step_skipped = true;
        spdlog::info("[Wizard] Pre-skip: Filament sensor step will be skipped");
    }

    // Probe sensor skip (step 10)
    if (!probe_step_skipped && get_wizard_probe_sensor_select_step()->should_skip()) {
        probe_step_skipped = true;
        spdlog::info("[Wizard] Pre-skip: Probe sensor step will be skipped");
    }

    // Input shaper skip (step 11)
    if (!input_shaper_step_skipped && get_wizard_input_shaper_step()->should_skip()) {
        input_shaper_step_skipped = true;
        spdlog::info("[Wizard] Pre-skip: Input shaper step will be skipped");
    }

    int total_skipped = (touch_cal_step_skipped ? 1 : 0) + (language_step_skipped ? 1 : 0) +
                        (wifi_step_skipped ? 1 : 0) + (ams_step_skipped ? 1 : 0) +
                        (led_step_skipped ? 1 : 0) + (filament_step_skipped ? 1 : 0) +
                        (probe_step_skipped ? 1 : 0) + (input_shaper_step_skipped ? 1 : 0);
    spdlog::info("[Wizard] Pre-calculated skips: {} steps will be skipped, {} total steps",
                 total_skipped, 13 - total_skipped);

    // Mark that we now know the true step count
    skips_precalculated = true;
}

// ============================================================================
// Screen Cleanup
// ============================================================================

/**
 * Cleanup the current wizard screen before navigating to a new one
 *
 * Calls the appropriate cleanup function based on current_screen_step.
 * This ensures resources are properly released and screen pointers are reset.
 */
static void ui_wizard_cleanup_current_screen() {
    if (current_screen_step < 0) {
        return; // No screen loaded yet
    }

    spdlog::debug("[Wizard] Cleaning up screen for step {}", current_screen_step);

    switch (current_screen_step) {
    case 0: // Touch Calibration
        get_wizard_touch_calibration_step()->cleanup();
        break;
    case 1: // Language Chooser
        get_wizard_language_chooser_step()->cleanup();
        break;
    case 2: // WiFi Setup
        get_wizard_wifi_step()->cleanup();
        break;
    case 3: // Moonraker Connection
        get_wizard_connection_step()->cleanup();
        break;
    case 4: // Printer Identification
        get_wizard_printer_identify_step()->cleanup();
        break;
    case 5: // Heater Select (combined bed + hotend)
        get_wizard_heater_select_step()->cleanup();
        break;
    case 6: // Fan Select
        get_wizard_fan_select_step()->cleanup();
        break;
    case 7: // AMS Identify
        get_wizard_ams_identify_step()->cleanup();
        break;
    case 8: // LED Select
        get_wizard_led_select_step()->cleanup();
        break;
    case 9: // Filament Sensor Select
        get_wizard_filament_sensor_select_step()->cleanup();
        break;
    case 10: // Probe Sensor Select
        get_wizard_probe_sensor_select_step()->cleanup();
        break;
    case 11: // Input Shaper
        get_wizard_input_shaper_step()->cleanup();
        break;
    case 12: // Summary
        get_wizard_summary_step()->cleanup();
        break;
    default:
        spdlog::warn("[Wizard] Unknown screen step {} during cleanup", current_screen_step);
        break;
    }
}

// ============================================================================
// Screen Loading
// ============================================================================

static void ui_wizard_load_screen(int step) {
    spdlog::debug("[Wizard] Loading screen for step {}", step);

    // Find wizard_content container
    lv_obj_t* content = lv_obj_find_by_name(wizard_container, "wizard_content");
    if (!content) {
        spdlog::error("[Wizard] wizard_content container not found");
        return;
    }

    // Cleanup previous screen resources BEFORE clearing widgets
    ui_wizard_cleanup_current_screen();

    // Clear existing content (widgets)
    lv_obj_clean(content);
    spdlog::debug("[Wizard] Cleared wizard_content container");

    // Set title and subtitle from XML metadata (no more hardcoded strings!)
    // Use lv_tr() to translate the title/subtitle dynamically based on current language
    const char* title = get_step_title_from_xml(step);
    ui_wizard_set_title(lv_tr(title));
    const char* subtitle = get_step_subtitle_from_xml(step);
    lv_subject_copy_string(&wizard_subtitle, lv_tr(subtitle));

    // Default Next button to enabled - steps that gate on validation (language,
    // connection, printer identify, fan select) will set it to 0 in their init
    lv_subject_set_int(&connection_test_passed, 1);

    // Create appropriate screen based on step
    // Note: Step-specific initialization remains in switch because each step
    // has unique logic (WiFi needs init_wifi_manager, etc.)
    switch (step) {
    case 0: // Touch Calibration
        spdlog::debug("[Wizard] Creating touch calibration screen");
        get_wizard_touch_calibration_step()->init_subjects();
        get_wizard_touch_calibration_step()->register_callbacks();
        get_wizard_touch_calibration_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 1: // Language Chooser
        spdlog::debug("[Wizard] Creating language chooser screen");
        // Disable Next until a language is selected
        lv_subject_set_int(&connection_test_passed, 0);
        get_wizard_language_chooser_step()->init_subjects();
        get_wizard_language_chooser_step()->register_callbacks();
        get_wizard_language_chooser_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 2: // WiFi Setup
        spdlog::debug("[Wizard] Creating WiFi setup screen");
        get_wizard_wifi_step()->init_subjects();
        get_wizard_wifi_step()->register_callbacks();
        get_wizard_wifi_step()->create(content);
        lv_obj_update_layout(content);
        get_wizard_wifi_step()->init_wifi_manager();
        break;

    case 3: // Moonraker Connection
        spdlog::debug("[Wizard] Creating Moonraker connection screen");
        get_wizard_connection_step()->init_subjects();
        get_wizard_connection_step()->register_callbacks();
        get_wizard_connection_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 4: // Printer Identification
        spdlog::debug("[Wizard] Creating printer identification screen");
        get_wizard_printer_identify_step()->init_subjects();
        get_wizard_printer_identify_step()->register_callbacks();
        get_wizard_printer_identify_step()->create(content);
        lv_obj_update_layout(content);
        // Override subtitle with dynamic detection status
        lv_subject_copy_string(&wizard_subtitle,
                               get_wizard_printer_identify_step()->get_detection_status());
        break;

    case 5: // Heater Select (combined bed + hotend)
        spdlog::debug("[Wizard] Creating heater select screen");
        get_wizard_heater_select_step()->init_subjects();
        get_wizard_heater_select_step()->register_callbacks();
        get_wizard_heater_select_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 6: // Fan Select
        spdlog::debug("[Wizard] Creating fan select screen");
        get_wizard_fan_select_step()->init_subjects();
        get_wizard_fan_select_step()->register_callbacks();
        get_wizard_fan_select_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 7: // AMS Identify
        spdlog::debug("[Wizard] Creating AMS identify screen");
        get_wizard_ams_identify_step()->init_subjects();
        get_wizard_ams_identify_step()->register_callbacks();
        (void)get_wizard_ams_identify_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 8: // LED Select
        spdlog::debug("[Wizard] Creating LED select screen");
        get_wizard_led_select_step()->init_subjects();
        get_wizard_led_select_step()->register_callbacks();
        get_wizard_led_select_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 9: // Filament Sensor Select
        spdlog::debug("[Wizard] Creating filament sensor select screen");
        get_wizard_filament_sensor_select_step()->init_subjects();
        get_wizard_filament_sensor_select_step()->register_callbacks();
        get_wizard_filament_sensor_select_step()->create(content);
        lv_obj_update_layout(content);
        // Schedule refresh in case sensors are discovered after screen creation
        // (handles race condition when jumping directly to step 9)
        {
            auto* step = get_wizard_filament_sensor_select_step();
            step->refresh_timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    auto* s = get_wizard_filament_sensor_select_step();
                    s->refresh_timer_ = nullptr;
                    s->refresh();
                    lv_timer_delete(timer);
                },
                1500, nullptr);
        }
        break;

    case 10: // Probe Sensor Select
        spdlog::debug("[Wizard] Creating probe sensor select screen");
        get_wizard_probe_sensor_select_step()->init_subjects();
        get_wizard_probe_sensor_select_step()->register_callbacks();
        get_wizard_probe_sensor_select_step()->create(content);
        lv_obj_update_layout(content);
        // Schedule refresh in case sensors are discovered after screen creation
        {
            auto* step = get_wizard_probe_sensor_select_step();
            step->refresh_timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    auto* s = get_wizard_probe_sensor_select_step();
                    s->refresh_timer_ = nullptr;
                    s->refresh();
                    lv_timer_delete(timer);
                },
                1500, nullptr);
        }
        break;

    case 11: // Input Shaper Calibration
        spdlog::debug("[Wizard] Creating input shaper calibration screen");
        get_wizard_input_shaper_step()->init_subjects();
        get_wizard_input_shaper_step()->register_callbacks();
        get_wizard_input_shaper_step()->create(content);
        lv_obj_update_layout(content);
        break;

    case 12: // Summary
        spdlog::debug("[Wizard] Creating summary screen");
        get_wizard_summary_step()->init_subjects();
        get_wizard_summary_step()->register_callbacks();
        get_wizard_summary_step()->create(content);
        lv_obj_update_layout(content);
        break;

    default:
        spdlog::warn("[Wizard] Invalid step {}, ignoring", step);
        break;
    }

    // Update current screen step tracking
    current_screen_step = step;
}

// ============================================================================
// Wizard Completion
// ============================================================================

void ui_wizard_complete() {
    spdlog::info("[Wizard] Completing wizard and transitioning to main UI");

    // 1. Mark wizard as completed in config
    Config* config = Config::get_instance();
    if (config) {
        spdlog::debug("[Wizard] Setting wizard_completed flag");
        config->set<bool>("/wizard_completed", true);

        // 1b. Populate expected_hardware from wizard selections
        // This prevents "new hardware detected" warnings on subsequent runs
        std::vector<std::string> hardware_paths = {
            helix::wizard::BED_HEATER,    // "/printer/heaters/bed"
            helix::wizard::HOTEND_HEATER, // "/printer/heaters/hotend"
            helix::wizard::PART_FAN,      // "/printer/fans/part"
            helix::wizard::HOTEND_FAN,    // "/printer/fans/hotend"
            helix::wizard::LED_STRIP      // "/printer/leds/strip"
        };

        for (const auto& path : hardware_paths) {
            std::string hw_name = config->get<std::string>(path, "");
            if (!hw_name.empty() && hw_name != "None") {
                HardwareValidator::add_expected_hardware(config, hw_name);
                spdlog::debug("[Wizard] Added '{}' to expected_hardware", hw_name);
            }
        }

        // 1c. Add user-selected runout sensor to expected hardware
        {
            auto& sensor_mgr = helix::FilamentSensorManager::instance();
            auto sensors = sensor_mgr.get_sensors();
            for (const auto& sensor : sensors) {
                if (sensor.role == helix::FilamentSensorRole::RUNOUT &&
                    !sensor.klipper_name.empty()) {
                    HardwareValidator::add_expected_hardware(config, sensor.klipper_name);
                    spdlog::info("[Wizard] Added runout sensor '{}' to expected_hardware",
                                 sensor.klipper_name);
                    break;
                }
            }
        }

        // 1d. Add AMS to expected hardware if detected (step wasn't skipped)
        // This allows the hardware validator to warn if AMS disappears between sessions
        if (!ams_step_skipped) {
            auto& ams = AmsState::instance();
            AmsBackend* backend = ams.get_backend();
            if (backend) {
                AmsType type = backend->get_type();
                std::string ams_hw_name;
                switch (type) {
                case AmsType::AFC:
                    ams_hw_name = "AFC"; // Matches the Klipper object name (uppercase)
                    break;
                case AmsType::HAPPY_HARE:
                    ams_hw_name = "mmu"; // Matches the Klipper object name
                    break;
                case AmsType::TOOL_CHANGER:
                    ams_hw_name = "toolchanger"; // Marker for tool changer detection
                    break;
                case AmsType::VALGACE:
                    ams_hw_name = "valgace"; // ValgACE marker (REST-based, not a Klipper object)
                    break;
                default:
                    break;
                }
                if (!ams_hw_name.empty()) {
                    HardwareValidator::add_expected_hardware(config, ams_hw_name);
                    spdlog::info("[Wizard] Added '{}' to expected hardware", ams_hw_name);
                }
            }
        }

        if (!config->save()) {
            NOTIFY_ERROR("Failed to save setup completion");
        }
    } else {
        LOG_ERROR_INTERNAL("[Wizard] Failed to get config instance to mark wizard complete");
    }

    // 2. Cleanup current wizard screen
    ui_wizard_cleanup_current_screen();

    // 3. Delete wizard container (main UI is already created underneath)
    // SAFETY: Use lv_obj_del_async — the Finish button that triggered this call is a
    // child of wizard_container. Synchronous delete causes use-after-free (issue #80).
    if (wizard_container) {
        spdlog::debug("[Wizard] Deleting wizard container (async)");
        lv_obj_del_async(wizard_container);
        wizard_container = nullptr;
    }

    // 4. Clear global wizard state
    set_wizard_active(false);

    // 5. Schedule deferred runout check - modal may need to show after wizard
    lv_timer_create(
        [](lv_timer_t* timer) {
            auto& fsm = helix::FilamentSensorManager::instance();
            if (fsm.has_any_runout() && get_runtime_config()->should_show_runout_modal()) {
                spdlog::debug("[Wizard] Deferred runout check - triggering modal");
                get_global_home_panel().trigger_idle_runout_check();
            }
            lv_timer_delete(timer);
        },
        500, nullptr); // 500ms delay for UI to stabilize

    // 6. Trigger re-discovery through Application's pre-registered callbacks.
    // Discovery callbacks (set_hardware, init_fans, hardware validation, plugin detection,
    // etc.) were registered in Application::init_moonraker() via setup_discovery_callbacks().
    MoonrakerClient* client = get_moonraker_client();
    if (client && client->get_connection_state() == ConnectionState::CONNECTED) {
        client->discover_printer([]() { spdlog::info("[Wizard] Post-wizard discovery complete"); });
    } else {
        spdlog::warn("[Wizard] Not connected after wizard - subsystems will initialize on restart");
    }

    // Tell Home Panel to reload immediately for printer image, type overlay
    // (LED and other hardware will update async when discovery completes)
    get_global_home_panel().apply_printer_config();

    spdlog::info("[Wizard] Wizard complete, transitioned to main UI");
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_back_clicked(lv_event_t* e) {
    (void)e;
    if (navigating)
        return;
    navigating = true;
    int current = lv_subject_get_int(&current_step);

    // Find minimum step (first non-skipped step)
    int min_step = 0;
    if (touch_cal_step_skipped)
        min_step = 1;
    if (min_step == 1 && language_step_skipped)
        min_step = 2;
    if (min_step == 2 && wifi_step_skipped)
        min_step = 3;

    if (current > min_step) {
        int prev_step = current - 1;

        // Skip input shaper step (11) when going back if it was skipped
        if (prev_step == 11 && input_shaper_step_skipped) {
            prev_step = 10;
        }

        // Skip probe sensor step (10) when going back if it was skipped
        if (prev_step == 10 && probe_step_skipped) {
            prev_step = 9;
        }

        // Skip filament sensor step (9) when going back if it was skipped
        if (prev_step == 9 && filament_step_skipped) {
            prev_step = 8;
        }

        // Skip LED step (8) when going back if it was skipped
        if (prev_step == 8 && led_step_skipped) {
            prev_step = 7;
        }

        // Skip AMS step (7) when going back if it was skipped
        if (prev_step == 7 && ams_step_skipped) {
            prev_step = 6;
        }

        // Skip WiFi step (2) when going back if it was skipped
        if (prev_step == 2 && wifi_step_skipped) {
            prev_step = 1;
        }

        // Skip language step (1) when going back if it was skipped
        if (prev_step == 1 && language_step_skipped) {
            prev_step = 0;
        }

        // Skip touch calibration step (0) when going back if it was skipped
        if (prev_step == 0 && touch_cal_step_skipped) {
            // Can't go back further - touch cal was skipped
            navigating = false;
            return;
        }

        ui_wizard_navigate_to_step(prev_step);
        spdlog::debug("[Wizard] Back button clicked, step: {}", prev_step);
    } else {
        navigating = false;
    }
}

static void on_next_clicked(lv_event_t* e) {
    (void)e;
    if (navigating)
        return;
    navigating = true;
    int current = lv_subject_get_int(&current_step);

    // Summary (step 12) is the last step
    if (current >= STEP_COMPONENT_COUNT) {
        spdlog::info("[Wizard] Finish button clicked, completing wizard");
        ui_wizard_complete();
        return;
    }

    // Commit touch calibration when leaving step 0 (only saves if user completed calibration)
    if (current == 0) {
        get_wizard_touch_calibration_step()->commit_calibration();
    }

    int next_step = current + 1;

    // Skip language step (1) if language already set
    if (next_step == 1 && get_wizard_language_chooser_step()->should_skip()) {
        language_step_skipped = true;
        next_step = 2;
        spdlog::debug("[Wizard] Skipping language step (language already set)");
    }

    // Skip WiFi step (2) on Android
    if (next_step == 2 && helix::is_android_platform()) {
        wifi_step_skipped = true;
        next_step = 3;
        spdlog::debug("[Wizard] Skipping WiFi step (Android platform)");
    }

    // Pre-calculate all skip flags when leaving connection step (step 3)
    // This ensures consistent step totals from step 4 onwards
    if (current == 3) {
        spdlog::info("[Wizard] Leaving connection step, pre-calculating skips...");
        ui_wizard_precalculate_skips();
    }

    // Skip AMS step (7) if no AMS detected
    if (next_step == 7 && get_wizard_ams_identify_step()->should_skip()) {
        ams_step_skipped = true;
        next_step = 8;
        spdlog::debug("[Wizard] Skipping AMS step (no AMS detected)");
    }

    // Skip LED step (8) if no LEDs detected
    if (next_step == 8 && get_wizard_led_select_step()->should_skip()) {
        led_step_skipped = true;
        next_step = 9;
        spdlog::debug("[Wizard] Skipping LED step (no LEDs detected)");
    }

    // Ensure FilamentSensorManager is populated before skip check
    if (next_step == 9) {
        auto& fsm = helix::FilamentSensorManager::instance();
        if (fsm.get_sensors().empty()) {
            MoonrakerAPI* api = get_moonraker_api();
            if (api && api->hardware().has_filament_sensors()) {
                fsm.discover_sensors(api->hardware().filament_sensor_names());
                spdlog::debug("[Wizard] Populated FilamentSensorManager before skip check");
            }
        }
    }

    // Skip filament sensor step (9) if <2 standalone sensors
    if (next_step == 9 && get_wizard_filament_sensor_select_step()->should_skip()) {
        filament_step_skipped = true;

        // Auto-configure single sensor if exactly 1 detected
        auto* step = get_wizard_filament_sensor_select_step();
        if (step->get_standalone_sensor_count() == 1) {
            step->auto_configure_single_sensor();
            spdlog::info("[Wizard] Auto-configured single filament sensor as RUNOUT");
        }
        next_step = 10;
        spdlog::debug("[Wizard] Skipping filament sensor step (<2 sensors)");
    }

    // Skip probe sensor step (10) if no unassigned switch sensors
    if (next_step == 10 && get_wizard_probe_sensor_select_step()->should_skip()) {
        probe_step_skipped = true;
        next_step = 11;
        spdlog::debug("[Wizard] Skipping probe sensor step (no unassigned sensors)");
    }

    // Skip input shaper step (11) if no accelerometer detected
    if (next_step == 11 && get_wizard_input_shaper_step()->should_skip()) {
        input_shaper_step_skipped = true;
        next_step = 12;
        spdlog::debug("[Wizard] Skipping input shaper step (no accelerometer)");
    }

    ui_wizard_navigate_to_step(next_step);
    spdlog::debug("[Wizard] Next button clicked, step: {}", next_step);
}
