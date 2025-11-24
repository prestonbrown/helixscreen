/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#include "../catch_amalgamated.hpp"
#include "lvgl/lvgl.h"
#include <filesystem>
#include <string>

// Panel headers - only include headers that have init_subjects() functions
#include "ui_panel_home.h"
#include "ui_panel_controls.h"
#include "ui_panel_motion.h"
#include "ui_panel_controls_temp.h"
#include "ui_panel_controls_extrusion.h"
#include "ui_panel_filament.h"
#include "ui_panel_settings.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"

/**
 * @file test_ui_panels_smoke.cpp
 * @brief Smoke tests for UI panels
 *
 * These tests verify that:
 * 1. XML panel files exist and are readable
 * 2. Panel initialization functions can be called without crashing
 * 3. Basic panel creation works (for panels with create functions)
 *
 * NOTE: These are smoke tests, not comprehensive functionality tests.
 * The goal is to catch obvious issues like missing files or initialization crashes.
 */

// ============================================================================
// Test Fixture
// ============================================================================

class PanelSmokeTestFixture {
public:
    PanelSmokeTestFixture() {
        // Initialize LVGL once for all tests
        static bool lvgl_initialized = false;
        if (!lvgl_initialized) {
            lv_init();
            lvgl_initialized = true;
        }

        // Create headless display for testing
        static lv_color_t buf[800 * 10];
        display = lv_display_create(800, 480);
        lv_display_set_buffers(display, buf, nullptr, sizeof(buf),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, [](lv_display_t* disp,
                                             const lv_area_t* area,
                                             uint8_t* px_map) {
            lv_display_flush_ready(disp);  // Dummy flush for headless testing
        });

        // Create test screen
        screen = lv_obj_create(lv_screen_active());
        lv_obj_set_size(screen, 800, 480);
    }

    ~PanelSmokeTestFixture() {
        if (screen) lv_obj_delete(screen);
        if (display) lv_display_delete(display);
    }

    bool xml_file_exists(const std::string& filename) {
        std::string path = "ui_xml/" + filename;
        return std::filesystem::exists(path);
    }

    lv_obj_t* screen = nullptr;
    lv_display_t* display = nullptr;
};

// ============================================================================
// XML File Existence Tests
// ============================================================================

TEST_CASE("UI Panels: XML files exist", "[ui_panels][smoke][xml]") {
    PanelSmokeTestFixture fixture;

    SECTION("home_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("home_panel.xml"));
    }

    SECTION("controls_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("controls_panel.xml"));
    }

    SECTION("motion_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("motion_panel.xml"));
    }

    SECTION("nozzle_temp_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("nozzle_temp_panel.xml"));
    }

    SECTION("bed_temp_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("bed_temp_panel.xml"));
    }

    SECTION("extrusion_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("extrusion_panel.xml"));
    }

    SECTION("filament_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("filament_panel.xml"));
    }

    SECTION("settings_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("settings_panel.xml"));
    }

    SECTION("bed_mesh_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("bed_mesh_panel.xml"));
    }

    SECTION("print_select_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("print_select_panel.xml"));
    }

    SECTION("print_status_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("print_status_panel.xml"));
    }

    SECTION("advanced_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("advanced_panel.xml"));
    }

    SECTION("notification_history_panel.xml exists") {
        REQUIRE(fixture.xml_file_exists("notification_history_panel.xml"));
    }
}

// ============================================================================
// Panel Subject Initialization Tests
// ============================================================================

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Home panel initialization",
                 "[ui_panels][smoke][home]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_home_init_subjects());
    }

    SECTION("create function exists and returns non-null") {
        ui_panel_home_init_subjects();
        lv_obj_t* panel = ui_panel_home_create(screen);
        REQUIRE(panel != nullptr);
        REQUIRE(lv_obj_is_valid(panel));
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Controls panel initialization",
                 "[ui_panels][smoke][controls]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_controls_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Motion panel initialization",
                 "[ui_panels][smoke][motion]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_motion_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Temperature panels initialization",
                 "[ui_panels][smoke][temperature]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_controls_temp_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Extrusion panel initialization",
                 "[ui_panels][smoke][extrusion]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_controls_extrusion_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Filament panel initialization",
                 "[ui_panels][smoke][filament]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_filament_init_subjects());
    }

    SECTION("create function exists and returns non-null") {
        ui_panel_filament_init_subjects();
        lv_obj_t* panel = ui_panel_filament_create(screen);
        REQUIRE(panel != nullptr);
        REQUIRE(lv_obj_is_valid(panel));
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Settings panel initialization",
                 "[ui_panels][smoke][settings]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_settings_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Bed mesh panel initialization",
                 "[ui_panels][smoke][bed_mesh]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_bed_mesh_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Print select panel initialization",
                 "[ui_panels][smoke][print_select]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_print_select_init_subjects());
    }
}

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Print status panel initialization",
                 "[ui_panels][smoke][print_status]") {

    SECTION("init_subjects does not crash") {
        REQUIRE_NOTHROW(ui_panel_print_status_init_subjects());
    }
}

// ============================================================================
// Multiple Panel Initialization Test
// ============================================================================

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Initialize all panels in sequence",
                 "[ui_panels][smoke][integration]") {

    SECTION("All panels can be initialized without conflicts") {
        REQUIRE_NOTHROW(ui_panel_home_init_subjects());
        REQUIRE_NOTHROW(ui_panel_controls_init_subjects());
        REQUIRE_NOTHROW(ui_panel_motion_init_subjects());
        REQUIRE_NOTHROW(ui_panel_controls_temp_init_subjects());
        REQUIRE_NOTHROW(ui_panel_controls_extrusion_init_subjects());
        REQUIRE_NOTHROW(ui_panel_filament_init_subjects());
        REQUIRE_NOTHROW(ui_panel_settings_init_subjects());
        REQUIRE_NOTHROW(ui_panel_bed_mesh_init_subjects());
        REQUIRE_NOTHROW(ui_panel_print_select_init_subjects());
        REQUIRE_NOTHROW(ui_panel_print_status_init_subjects());
    }
}

// ============================================================================
// Panel Creation Tests (for panels with create functions)
// ============================================================================

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Panel creation functions",
                 "[ui_panels][smoke][creation]") {

    SECTION("Home panel creation") {
        ui_panel_home_init_subjects();
        lv_obj_t* panel = ui_panel_home_create(screen);
        REQUIRE(panel != nullptr);
        REQUIRE(lv_obj_is_valid(panel));
        REQUIRE(lv_obj_get_parent(panel) == screen);
    }

    SECTION("Filament panel creation") {
        ui_panel_filament_init_subjects();
        lv_obj_t* panel = ui_panel_filament_create(screen);
        REQUIRE(panel != nullptr);
        REQUIRE(lv_obj_is_valid(panel));
        REQUIRE(lv_obj_get_parent(panel) == screen);
    }
}

// ============================================================================
// Panel Cleanup Tests
// ============================================================================

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Panel cleanup does not crash",
                 "[ui_panels][smoke][cleanup]") {

    SECTION("Home panel cleanup") {
        ui_panel_home_init_subjects();
        lv_obj_t* panel = ui_panel_home_create(screen);
        REQUIRE(panel != nullptr);
        REQUIRE_NOTHROW(lv_obj_delete(panel));
    }

    SECTION("Filament panel cleanup") {
        ui_panel_filament_init_subjects();
        lv_obj_t* panel = ui_panel_filament_create(screen);
        REQUIRE(panel != nullptr);
        REQUIRE_NOTHROW(lv_obj_delete(panel));
    }
}

// ============================================================================
// Basic Panel Structure Tests
// ============================================================================

TEST_CASE_METHOD(PanelSmokeTestFixture,
                 "UI Panels: Created panels have valid structure",
                 "[ui_panels][smoke][structure]") {

    SECTION("Home panel has children") {
        ui_panel_home_init_subjects();
        lv_obj_t* panel = ui_panel_home_create(screen);
        REQUIRE(panel != nullptr);

        // Panel should have child widgets
        uint32_t child_count = lv_obj_get_child_count(panel);
        REQUIRE(child_count > 0);
    }

    SECTION("Filament panel has children") {
        ui_panel_filament_init_subjects();
        lv_obj_t* panel = ui_panel_filament_create(screen);
        REQUIRE(panel != nullptr);

        // Panel should have child widgets
        uint32_t child_count = lv_obj_get_child_count(panel);
        REQUIRE(child_count > 0);
    }
}
