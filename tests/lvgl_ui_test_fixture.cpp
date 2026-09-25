// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_ui_test_fixture.h"

// Asset management
#include "asset_manager.h"

// Theme
#include "theme_manager.h"

// Custom widgets (must be registered before XML components that use them)
#include "ui_bed_mesh.h"
#include "ui_card.h"
#include "ui_component_header_bar.h"
#include "ui_dialog.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_severity_card.h"
#include "ui_switch.h"
#include "ui_temp_display.h"
#include "ui_text_input.h"

// XML registration
#include "xml_registration.h"

// Subject initialization
#include "ui_nav_manager.h"
#include "ui_notification_manager.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/print_status_widget.h"

// Event callbacks
#include "ui_emergency_stop.h"
#include "ui_panel_input_shaper.h"
#include "ui_panel_screws_tilt.h"

// Moonraker (for API/client - disconnected mode)
#include "moonraker_api.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

using namespace helix;

LVGLUITestFixture::LVGLUITestFixture() {
    spdlog::debug("[LVGLUITestFixture] Starting initialization...");

    // The parent constructor (LVGLTestFixture) creates a test screen.
    // We need to delete it temporarily because theme_manager_init() hangs
    // if called when screens exist. We'll recreate it after theme init.
    if (m_test_screen != nullptr) {
        lv_obj_delete(m_test_screen);
        m_test_screen = nullptr;
    }

    // Phase 1: Assets (fonts, images)
    init_assets();

    // Phase 2: Theme (needs assets, no screens present)
    init_theme();

    // Phase 3: Custom widgets (needed by XML components)
    register_widgets();

    // Phase 4: Subjects (MUST be before XML components per L004)
    init_subjects();

    // Phase 5: Event callbacks (MUST be before XML components per L013)
    register_event_callbacks();

    // Phase 6: XML components (now subjects and callbacks are ready)
    register_xml_components();

    // Recreate test screen (with theme applied)
    m_test_screen = lv_obj_create(nullptr);
    if (m_test_screen != nullptr) {
        lv_screen_load(m_test_screen);
    }

    m_fully_initialized = true;
    spdlog::info("[LVGLUITestFixture] Fully initialized");
}

LVGLUITestFixture::~LVGLUITestFixture() {
    cleanup();
}

PrinterState& LVGLUITestFixture::state() {
    return get_printer_state();
}

MoonrakerClient* LVGLUITestFixture::client() {
    return m_client.get();
}

MoonrakerAPI* LVGLUITestFixture::api() {
    return m_api.get();
}

void LVGLUITestFixture::init_assets() {
    spdlog::debug("[LVGLUITestFixture] Registering assets...");
    AssetManager::register_all();
    m_assets_initialized = true;
    spdlog::debug("[LVGLUITestFixture] Assets registered");
}

void LVGLUITestFixture::init_theme() {
    spdlog::debug("[LVGLUITestFixture] Initializing theme...");

    // globals.xml must be registered before theme (provides constants)
    lv_xml_register_component_from_file("A:ui_xml/globals.xml");

    // Initialize theme in light mode for test consistency
    // (dark mode can make screenshots harder to compare)
    theme_manager_init(lv_display_get_default(), false);
    m_theme_initialized = true;
    spdlog::debug("[LVGLUITestFixture] Theme initialized");
}

void LVGLUITestFixture::register_widgets() {
    spdlog::debug("[LVGLUITestFixture] Registering custom widgets...");

    // Register C++ widgets in dependency order
    // These are needed before XML components that embed them
    ui_icon_register_widget();
    ui_switch_register();
    ui_card_register();
    ui_temp_display_init();
    ui_severity_card_register();
    ui_dialog_register();
    ui_bed_mesh_register();
    ui_gcode_viewer_register();
    ui_gradient_canvas_register();

    // Initialize text input widget
    ui_text_input_init();

    // Initialize component systems
    ui_component_header_bar_init();

    m_widgets_registered = true;
    spdlog::debug("[LVGLUITestFixture] Custom widgets registered");
}

void LVGLUITestFixture::init_subjects() {
    spdlog::debug("[LVGLUITestFixture] Initializing subjects...");

    // Force a clean slate: earlier tests in the shard may have called
    // init_subjects(false) (no XML registration) and left subjects_initialized_
    // set.  That guard would cause our init_subjects(true) to skip XML
    // registration, leaving bindings unresolved (0 observers).
    get_printer_state().deinit_subjects();

    // Core subjects (must be first)
    app_globals_init_subjects();
    NavigationManager::instance().init();
    helix::ui::notification_init_subjects();

    // PrinterState subjects (panels depend on these)
    // Pass true to enable XML subject registration
    get_printer_state().init_subjects(true);

    // Wizard subjects (needed for wizard components)
    ui_wizard_init_subjects();

    // Emergency stop overlay subjects (recovery dialog bindings)
    EmergencyStopOverlay::instance().init_subjects();

    // Create disconnected client and API for tests that need them
    m_client = std::make_unique<MoonrakerClient>();
    m_api = std::make_unique<MoonrakerAPI>(*m_client, get_printer_state());

    m_subjects_initialized = true;
    spdlog::debug("[LVGLUITestFixture] Subjects initialized");
}

void LVGLUITestFixture::register_event_callbacks() {
    spdlog::debug("[LVGLUITestFixture] Registering event callbacks...");

    // Wizard callbacks (for navigation buttons)
    ui_wizard_register_event_callbacks();
    ui_wizard_container_register_responsive_constants();

    // Notification callbacks (for status icons)
    helix::ui::notification_register_callbacks();

    // Calibration panel callbacks
    ui_panel_screws_tilt_register_callbacks();
    ui_panel_input_shaper_register_callbacks();

    m_callbacks_registered = true;
    spdlog::debug("[LVGLUITestFixture] Event callbacks registered");
}

void LVGLUITestFixture::register_xml_components() {
    spdlog::debug("[LVGLUITestFixture] Registering XML components...");

    // Use production registration function - registers ALL components
    // in correct dependency order
    helix::register_xml_components();

    m_xml_registered = true;
    spdlog::debug("[LVGLUITestFixture] XML components registered");
}

void LVGLUITestFixture::cleanup() {
    spdlog::debug("[LVGLUITestFixture] Starting cleanup...");

    // Cleanup follows reverse initialization order (per L041)

    // Destroy API before client (API holds reference to client)
    m_api.reset();
    m_client.reset();

    // Deinitialize subjects
    if (m_subjects_initialized) {
        // Emergency stop overlay subjects
        EmergencyStopOverlay::instance().deinit_subjects();

        // Wizard subjects
        ui_wizard_deinit_subjects();

        // The formatter singleton observes PrinterState subjects; deiniting them
        // first leaves its guards pointing at freed observer nodes, which the
        // base fixture's reset_all() then walks.
        helix::PrintStatusWidget::destroy_formatter_for_test();

        // PrinterState subjects
        get_printer_state().deinit_subjects();

        // Core singleton subjects - must be deinitialized to clear observers
        // before widgets are destroyed, otherwise style broadcasts will hit
        // stale observers pointing to deleted objects
        helix::ui::notification_deinit_subjects();
        app_globals_deinit_subjects();
        NavigationManager::instance().deinit_subjects();

        m_subjects_initialized = false;
    }

    // XML subjects cleanup
    if (m_xml_registered) {
        helix::deinit_xml_subjects();
        m_xml_registered = false;
    }

    m_fully_initialized = false;
    spdlog::debug("[LVGLUITestFixture] Cleanup complete");
}
