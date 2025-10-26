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

#include "lvgl/lvgl.h"
#include "lvgl/src/libs/svg/lv_svg_decoder.h"
#include "lvgl/src/others/xml/lv_xml.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "material_icons.h"
#include "ui_panel_home.h"
#include "ui_panel_print_select.h"
#include "ui_panel_controls.h"
#include "ui_panel_motion.h"
#include "ui_panel_controls_temp.h"
#include "ui_panel_controls_extrusion.h"
#include "ui_panel_print_status.h"
#include "ui_panel_filament.h"
#include "ui_component_keypad.h"
#include "ui_component_header_bar.h"
#include "ui_icon.h"
#include "ui_wizard.h"
#include "ui_panel_step_test.h"
#include "printer_state.h"
#include "moonraker_client.h"
#include "config.h"
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>

// LVGL display and input
static lv_display_t* display = nullptr;
static lv_indev_t* indev_mouse = nullptr;

// Screen dimensions (configurable via command line, default to medium size)
static int SCREEN_WIDTH = UI_SCREEN_MEDIUM_W;
static int SCREEN_HEIGHT = UI_SCREEN_MEDIUM_H;

// Printer state management
static PrinterState printer_state;

// Initialize LVGL with SDL
static bool init_lvgl() {
    lv_init();

    // LVGL's SDL driver handles window creation internally
    display = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!display) {
        LV_LOG_ERROR("Failed to create LVGL SDL display");
        return false;
    }

    // Create mouse input device
    indev_mouse = lv_sdl_mouse_create();
    if (!indev_mouse) {
        LV_LOG_ERROR("Failed to create LVGL SDL mouse input");
        return false;
    }

    LV_LOG_USER("LVGL initialized: %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);

    // Initialize SVG decoder for loading .svg files
    lv_svg_decoder_init();

    return true;
}

// Save screenshot using SDL renderer
// Simple BMP file writer for ARGB8888 format
static bool write_bmp(const char* filename, const uint8_t* data, int width, int height) {
    FILE* f = fopen(filename, "wb");
    if (!f) return false;

    // BMP header (54 bytes total)
    uint32_t file_size = 54 + (width * height * 4);
    uint32_t pixel_offset = 54;
    uint32_t dib_size = 40;
    uint16_t planes = 1;
    uint16_t bpp = 32;

    // BMP file header (14 bytes)
    fputc('B', f); fputc('M', f);                        // Signature
    fwrite(&file_size, 4, 1, f);                         // File size
    fwrite((uint32_t[]){0}, 4, 1, f);                    // Reserved
    fwrite(&pixel_offset, 4, 1, f);                      // Pixel data offset

    // DIB header (40 bytes)
    fwrite(&dib_size, 4, 1, f);                          // DIB header size
    fwrite(&width, 4, 1, f);                             // Width
    fwrite(&height, 4, 1, f);                            // Height
    fwrite(&planes, 2, 1, f);                            // Planes
    fwrite(&bpp, 2, 1, f);                               // Bits per pixel
    fwrite((uint32_t[]){0}, 4, 1, f);                    // Compression (none)
    uint32_t image_size = width * height * 4;
    fwrite(&image_size, 4, 1, f);                        // Image size
    fwrite((uint32_t[]){2835}, 4, 1, f);                 // X pixels per meter
    fwrite((uint32_t[]){2835}, 4, 1, f);                 // Y pixels per meter
    fwrite((uint32_t[]){0}, 4, 1, f);                    // Colors in palette
    fwrite((uint32_t[]){0}, 4, 1, f);                    // Important colors

    // Write pixel data (BMP is bottom-up, so flip rows)
    for (int y = height - 1; y >= 0; y--) {
        fwrite(data + (y * width * 4), 4, width, f);
    }

    fclose(f);
    return true;
}

static void save_screenshot() {
    // Generate unique filename with timestamp
    char filename[256];
    snprintf(filename, sizeof(filename), "/tmp/ui-screenshot-%lu.bmp",
             (unsigned long)time(NULL));

    // Take snapshot using LVGL's native API (platform-independent)
    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);

    if (!snapshot) {
        LV_LOG_ERROR("Failed to take screenshot");
        return;
    }

    // Write BMP file
    if (write_bmp(filename, snapshot->data, snapshot->header.w, snapshot->header.h)) {
        LV_LOG_USER("Screenshot saved: %s", filename);
    } else {
        LV_LOG_ERROR("Failed to save screenshot");
    }

    // Free snapshot buffer
    lv_draw_buf_destroy(snapshot);
}

// Mock data generator (simulates printer state changes for testing)
static void update_mock_printer_data() {
    static uint32_t tick_count = 0;
    tick_count++;

    // Simulate temperature ramping (0-210°C over 30 seconds for nozzle, 0-60°C for bed)
    int nozzle_current = static_cast<int>(std::min(210.0, (tick_count / 30.0) * 210.0));
    int bed_current = static_cast<int>(std::min(60.0, (tick_count / 60.0) * 60.0));

    lv_subject_set_int(printer_state.get_extruder_temp_subject(), nozzle_current);
    lv_subject_set_int(printer_state.get_extruder_target_subject(), 210);
    lv_subject_set_int(printer_state.get_bed_temp_subject(), bed_current);
    lv_subject_set_int(printer_state.get_bed_target_subject(), 60);

    // Simulate print progress (0-100% over 2 minutes)
    int progress = static_cast<int>(std::min(100.0, (tick_count / 120.0) * 100.0));
    lv_subject_set_int(printer_state.get_print_progress_subject(), progress);

    // Update print state based on progress
    const char* state = "standby";
    if (progress > 0 && progress < 100) {
        state = "printing";
    } else if (progress >= 100) {
        state = "complete";
    }
    lv_subject_copy_string(printer_state.get_print_state_subject(), state);

    // Simulate jog position (slowly increasing)
    int x = 100 + (tick_count % 50);
    int y = 100 + ((tick_count / 2) % 50);
    int z = 10 + ((tick_count / 10) % 20);
    lv_subject_set_int(printer_state.get_position_x_subject(), x);
    lv_subject_set_int(printer_state.get_position_y_subject(), y);
    lv_subject_set_int(printer_state.get_position_z_subject(), z);

    // Simulate speed/flow (oscillate between 90-110%)
    int speed = 100 + static_cast<int>(10.0 * std::sin(tick_count / 10.0));
    int flow = 100 + static_cast<int>(5.0 * std::cos(tick_count / 15.0));
    int fan = static_cast<int>(std::min(100.0, (tick_count / 20.0) * 100.0));
    lv_subject_set_int(printer_state.get_speed_factor_subject(), speed);
    lv_subject_set_int(printer_state.get_flow_factor_subject(), flow);
    lv_subject_set_int(printer_state.get_fan_speed_subject(), fan);

    // Connection state (simulates connecting → connected after 3 seconds)
    if (tick_count == 3) {
        printer_state.set_connection_state(2, "Connected");
    }
}

// Main application
int main(int argc, char** argv) {
    // Parse command-line arguments
    int initial_panel = -1;  // -1 means auto-select based on screen size
    bool show_motion = false;  // Special flag for motion sub-screen
    bool show_nozzle_temp = false;  // Special flag for nozzle temp sub-screen
    bool show_bed_temp = false;  // Special flag for bed temp sub-screen
    bool show_extrusion = false;  // Special flag for extrusion sub-screen
    bool show_print_status = false;  // Special flag for print status screen
    bool show_file_detail = false;  // Special flag for file detail view
    bool show_keypad = false;  // Special flag for keypad testing
    bool show_step_test = false;  // Special flag for step progress widget testing
    bool force_wizard = false;  // Force wizard to run even if config exists

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (i + 1 < argc) {
                const char* size_arg = argv[++i];
                if (strcmp(size_arg, "tiny") == 0) {
                    SCREEN_WIDTH = UI_SCREEN_TINY_W;
                    SCREEN_HEIGHT = UI_SCREEN_TINY_H;
                } else if (strcmp(size_arg, "small") == 0) {
                    SCREEN_WIDTH = UI_SCREEN_SMALL_W;
                    SCREEN_HEIGHT = UI_SCREEN_SMALL_H;
                } else if (strcmp(size_arg, "medium") == 0) {
                    SCREEN_WIDTH = UI_SCREEN_MEDIUM_W;
                    SCREEN_HEIGHT = UI_SCREEN_MEDIUM_H;
                } else if (strcmp(size_arg, "large") == 0) {
                    SCREEN_WIDTH = UI_SCREEN_LARGE_W;
                    SCREEN_HEIGHT = UI_SCREEN_LARGE_H;
                } else {
                    printf("Unknown screen size: %s\n", size_arg);
                    printf("Available sizes: tiny, small, medium, large\n");
                    return 1;
                }
            } else {
                printf("Error: -s/--size requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--panel") == 0) {
            if (i + 1 < argc) {
                const char* panel_arg = argv[++i];
                if (strcmp(panel_arg, "home") == 0) {
                    initial_panel = UI_PANEL_HOME;
                } else if (strcmp(panel_arg, "controls") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                } else if (strcmp(panel_arg, "motion") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_motion = true;
                } else if (strcmp(panel_arg, "nozzle-temp") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_nozzle_temp = true;
                } else if (strcmp(panel_arg, "bed-temp") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_bed_temp = true;
                } else if (strcmp(panel_arg, "extrusion") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_extrusion = true;
                } else if (strcmp(panel_arg, "print-status") == 0 || strcmp(panel_arg, "printing") == 0) {
                    show_print_status = true;
                } else if (strcmp(panel_arg, "filament") == 0) {
                    initial_panel = UI_PANEL_FILAMENT;
                } else if (strcmp(panel_arg, "settings") == 0) {
                    initial_panel = UI_PANEL_SETTINGS;
                } else if (strcmp(panel_arg, "advanced") == 0) {
                    initial_panel = UI_PANEL_ADVANCED;
                } else if (strcmp(panel_arg, "print-select") == 0 || strcmp(panel_arg, "print_select") == 0) {
                    initial_panel = UI_PANEL_PRINT_SELECT;
                } else if (strcmp(panel_arg, "file-detail") == 0 || strcmp(panel_arg, "print-file-detail") == 0) {
                    initial_panel = UI_PANEL_PRINT_SELECT;
                    show_file_detail = true;
                } else if (strcmp(panel_arg, "step-test") == 0 || strcmp(panel_arg, "step_test") == 0) {
                    show_step_test = true;
                } else {
                    printf("Unknown panel: %s\n", panel_arg);
                    printf("Available panels: home, controls, motion, nozzle-temp, bed-temp, extrusion, print-status, filament, settings, advanced, print-select, step-test\n");
                    return 1;
                }
            } else {
                printf("Error: -p/--panel requires an argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--keypad") == 0) {
            show_keypad = true;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wizard") == 0) {
            force_wizard = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -s, --size <size>    Screen size: tiny, small, medium, large (default: medium)\n");
            printf("  -p, --panel <panel>  Initial panel (default: home)\n");
            printf("  -k, --keypad         Show numeric keypad for testing\n");
            printf("  -w, --wizard         Force first-run configuration wizard\n");
            printf("  -h, --help           Show this help message\n");
            printf("\nAvailable panels:\n");
            printf("  home, controls, motion, nozzle-temp, bed-temp, extrusion,\n");
            printf("  print-status, filament, settings, advanced, print-select\n");
            printf("\nScreen sizes:\n");
            printf("  tiny   = %dx%d\n", UI_SCREEN_TINY_W, UI_SCREEN_TINY_H);
            printf("  small  = %dx%d\n", UI_SCREEN_SMALL_W, UI_SCREEN_SMALL_H);
            printf("  medium = %dx%d (default)\n", UI_SCREEN_MEDIUM_W, UI_SCREEN_MEDIUM_H);
            printf("  large  = %dx%d\n", UI_SCREEN_LARGE_W, UI_SCREEN_LARGE_H);
            return 0;
        } else {
            // Legacy support: first positional arg is panel name
            if (i == 1 && argv[i][0] != '-') {
                const char* panel_arg = argv[i];
                if (strcmp(panel_arg, "home") == 0) {
                    initial_panel = UI_PANEL_HOME;
                } else if (strcmp(panel_arg, "controls") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                } else if (strcmp(panel_arg, "motion") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_motion = true;
                } else if (strcmp(panel_arg, "print-select") == 0 || strcmp(panel_arg, "print_select") == 0) {
                    initial_panel = UI_PANEL_PRINT_SELECT;
                } else if (strcmp(panel_arg, "step-test") == 0 || strcmp(panel_arg, "step_test") == 0) {
                    show_step_test = true;
                } else {
                    printf("Unknown argument: %s\n", argv[i]);
                    printf("Use --help for usage information\n");
                    return 1;
                }
            } else {
                printf("Unknown argument: %s\n", argv[i]);
                printf("Use --help for usage information\n");
                return 1;
            }
        }
    }

    printf("HelixScreen UI Prototype\n");
    printf("========================\n");
    printf("Target: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    printf("Nav Width: %d pixels\n", UI_NAV_WIDTH(SCREEN_WIDTH));
    printf("Initial Panel: %d\n", initial_panel);
    printf("\n");

    // Initialize config system
    Config* config = Config::get_instance();
    config->init("helixconfig.json");

    // Initialize LVGL (handles SDL internally)
    if (!init_lvgl()) {
        return 1;
    }

    // Create main screen
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, UI_COLOR_PANEL_BG, LV_PART_MAIN);

    // Initialize app-level resize handler for responsive layouts
    ui_resize_handler_init(screen);

    // Register fonts and images for XML (must be done before loading components)
    LV_LOG_USER("Registering fonts and images...");
    lv_xml_register_font(NULL, "fa_icons_64", &fa_icons_64);
    lv_xml_register_font(NULL, "fa_icons_48", &fa_icons_48);
    lv_xml_register_font(NULL, "fa_icons_32", &fa_icons_32);
    lv_xml_register_font(NULL, "fa_icons_24", &fa_icons_24);
    lv_xml_register_font(NULL, "fa_icons_16", &fa_icons_16);
    lv_xml_register_font(NULL, "arrows_64", &arrows_64);
    lv_xml_register_font(NULL, "arrows_48", &arrows_48);
    lv_xml_register_font(NULL, "arrows_32", &arrows_32);
    lv_xml_register_font(NULL, "montserrat_14", &lv_font_montserrat_14);
    lv_xml_register_font(NULL, "montserrat_16", &lv_font_montserrat_16);
    lv_xml_register_font(NULL, "montserrat_20", &lv_font_montserrat_20);
    lv_xml_register_font(NULL, "montserrat_28", &lv_font_montserrat_28);
    lv_xml_register_font(NULL, "montserrat_48", &lv_font_montserrat_48);
    lv_xml_register_image(NULL, "A:assets/images/printer_400.png",
                          "A:assets/images/printer_400.png");
    lv_xml_register_image(NULL, "filament_spool",
                          "A:assets/images/filament_spool.png");
    lv_xml_register_image(NULL, "A:assets/images/placeholder_thumb_centered.png",
                          "A:assets/images/placeholder_thumb_centered.png");
    lv_xml_register_image(NULL, "A:assets/images/thumbnail-gradient-bg.png",
                          "A:assets/images/thumbnail-gradient-bg.png");
    lv_xml_register_image(NULL, "A:assets/images/thumbnail-placeholder.png",
                          "A:assets/images/thumbnail-placeholder.png");
    lv_xml_register_image(NULL, "A:assets/images/large-extruder-icon.svg",
                          "A:assets/images/large-extruder-icon.svg");

    // Register Material Design icons (64x64, scalable)
    material_icons_register();

    // Register custom icon widget (must be before icon.xml component registration)
    ui_icon_register_widget();

    // Initialize component systems (BEFORE XML registration)
    ui_component_header_bar_init();

    // WORKAROUND: Add small delay to stabilize SDL/LVGL initialization
    // Prevents race condition between SDL2 and LVGL 9 XML component registration
    SDL_Delay(100);

    // Register XML components (globals first to make constants available)
    LV_LOG_USER("Registering XML components...");
    lv_xml_component_register_from_file("A:ui_xml/globals.xml");
    lv_xml_component_register_from_file("A:ui_xml/icon.xml");
    lv_xml_component_register_from_file("A:ui_xml/header_bar.xml");
    lv_xml_component_register_from_file("A:ui_xml/confirmation_dialog.xml");
    lv_xml_component_register_from_file("A:ui_xml/numeric_keypad_modal.xml");
    lv_xml_component_register_from_file("A:ui_xml/print_file_card.xml");
    lv_xml_component_register_from_file("A:ui_xml/print_file_list_row.xml");
    lv_xml_component_register_from_file("A:ui_xml/print_file_detail.xml");
    lv_xml_component_register_from_file("A:ui_xml/navigation_bar.xml");
    lv_xml_component_register_from_file("A:ui_xml/home_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/controls_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/motion_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/nozzle_temp_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/bed_temp_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/extrusion_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/print_status_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/filament_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/settings_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/advanced_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/print_select_panel.xml");
    lv_xml_component_register_from_file("A:ui_xml/step_progress_test.xml");
    lv_xml_component_register_from_file("A:ui_xml/app_layout.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_container.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_connection.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_printer_identify.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_bed_select.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_hotend_select.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_fan_select.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_led_select.xml");
    lv_xml_component_register_from_file("A:ui_xml/wizard_summary.xml");

    // Initialize reactive subjects BEFORE creating XML
    LV_LOG_USER("Initializing reactive subjects...");
    ui_nav_init();  // Navigation system (icon colors, active panel)
    ui_panel_home_init_subjects();  // Home panel data bindings
    ui_panel_print_select_init_subjects();  // Print select panel (none yet)
    ui_panel_controls_init_subjects();  // Controls panel launcher
    ui_panel_motion_init_subjects();  // Motion sub-screen position display
    ui_panel_controls_temp_init_subjects();  // Temperature sub-screens
    ui_panel_controls_extrusion_init_subjects();  // Extrusion sub-screen
    ui_panel_filament_init_subjects();  // Filament panel
    ui_panel_print_status_init_subjects();  // Print status screen
    ui_wizard_init_subjects();  // Wizard subjects (for first-run config)
    printer_state.init_subjects();  // Printer state subjects (CRITICAL: must be before XML creation)

    // Create entire UI from XML (single component contains everything)
    lv_obj_t* app_layout = (lv_obj_t*)lv_xml_create(screen, "app_layout", NULL);

    // Register app_layout with navigation system (to prevent hiding it)
    ui_nav_set_app_layout(app_layout);

    // Find navbar and panel widgets
    // app_layout > navbar (child 0), content_area (child 1)
    lv_obj_t* navbar = lv_obj_get_child(app_layout, 0);
    lv_obj_t* content_area = lv_obj_get_child(app_layout, 1);

    // Wire up navigation button click handlers and trigger initial color update
    ui_nav_wire_events(navbar);

    // Find all panel widgets in content area
    lv_obj_t* panels[UI_PANEL_COUNT];
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panels[i] = lv_obj_get_child(content_area, i);
    }

    // Register panels with navigation system for show/hide management
    ui_nav_set_panels(panels);

    // Setup home panel observers (panels[0] is home panel)
    ui_panel_home_setup_observers(panels[0]);

    // Setup controls panel (wire launcher card click handlers)
    ui_panel_controls_set(panels[UI_PANEL_CONTROLS]);
    ui_panel_controls_wire_events(panels[UI_PANEL_CONTROLS], screen);

    // Setup print select panel (wires up events, creates overlays, NOTE: data populated later)
    ui_panel_print_select_setup(panels[UI_PANEL_PRINT_SELECT], screen);

    // Setup filament panel (wire preset/action button handlers)
    ui_panel_filament_setup(panels[UI_PANEL_FILAMENT], screen);

    // Initialize numeric keypad modal component (creates reusable keypad widget)
    ui_keypad_init(screen);

    // Create print status panel (overlay for active prints)
    lv_obj_t* print_status_panel = (lv_obj_t*)lv_xml_create(screen, "print_status_panel", nullptr);
    if (print_status_panel) {
        ui_panel_print_status_setup(print_status_panel, screen);
        lv_obj_add_flag(print_status_panel, LV_OBJ_FLAG_HIDDEN);  // Hidden by default

        // Wire print status panel to print select (for launching prints)
        ui_panel_print_select_set_print_status_panel(print_status_panel);

        LV_LOG_USER("Print status panel created and wired to print select");
    } else {
        LV_LOG_ERROR("Failed to create print status panel");
    }

    LV_LOG_USER("XML UI created successfully with reactive navigation");

    // Auto-select home panel if not specified
    if (initial_panel == -1) {
        initial_panel = UI_PANEL_HOME;
    }

    // Switch to initial panel (if different from default HOME)
    if (initial_panel != UI_PANEL_HOME) {
        ui_nav_set_active((ui_panel_id_t)initial_panel);
        printf("Switched to panel %d\n", initial_panel);
    }

    // Force a few render cycles to ensure panel switch and layout complete
    for (int i = 0; i < 5; i++) {
        lv_timer_handler();
        SDL_Delay(10);
    }

    // NOW populate print select panel data (after layout is stable)
    ui_panel_print_select_populate_test_data(panels[UI_PANEL_PRINT_SELECT]);

    // Keypad is initialized and ready to be shown when controls panel buttons are clicked

    // Special case: Show keypad for testing
    if (show_keypad) {
        printf("Auto-opening numeric keypad for testing...\n");
        ui_keypad_config_t config = {
            .initial_value = 210.0f,
            .min_value = 0.0f,
            .max_value = 350.0f,
            .title_label = "Nozzle Temp",
            .unit_label = "°C",
            .allow_decimal = false,
            .allow_negative = false,
            .callback = nullptr,
            .user_data = nullptr
        };
        ui_keypad_show(&config);
    }

    // Special case: Show motion panel if requested
    if (show_motion) {
        printf("Creating and showing motion sub-screen...\n");

        // Create motion panel
        lv_obj_t* motion_panel = (lv_obj_t*)lv_xml_create(screen, "motion_panel", nullptr);
        if (motion_panel) {
            ui_panel_motion_setup(motion_panel, screen);

            // Hide controls launcher, show motion panel
            lv_obj_add_flag(panels[UI_PANEL_CONTROLS], LV_OBJ_FLAG_HIDDEN);

            // Set mock position data
            ui_panel_motion_set_position(120.5f, 105.2f, 15.8f);

            printf("Motion panel displayed\n");
        }
    }

    // Special case: Show nozzle temp panel if requested
    if (show_nozzle_temp) {
        printf("Creating and showing nozzle temperature sub-screen...\n");

        // Create nozzle temp panel
        lv_obj_t* nozzle_temp_panel = (lv_obj_t*)lv_xml_create(screen, "nozzle_temp_panel", nullptr);
        if (nozzle_temp_panel) {
            ui_panel_controls_temp_nozzle_setup(nozzle_temp_panel, screen);

            // Hide controls launcher, show nozzle temp panel
            lv_obj_add_flag(panels[UI_PANEL_CONTROLS], LV_OBJ_FLAG_HIDDEN);

            // Set mock temperature data
            ui_panel_controls_temp_set_nozzle(25, 0);

            printf("Nozzle temp panel displayed\n");
        }
    }

    // Special case: Show bed temp panel if requested
    if (show_bed_temp) {
        printf("Creating and showing bed temperature sub-screen...\n");

        // Create bed temp panel
        lv_obj_t* bed_temp_panel = (lv_obj_t*)lv_xml_create(screen, "bed_temp_panel", nullptr);
        if (bed_temp_panel) {
            ui_panel_controls_temp_bed_setup(bed_temp_panel, screen);

            // Hide controls launcher, show bed temp panel
            lv_obj_add_flag(panels[UI_PANEL_CONTROLS], LV_OBJ_FLAG_HIDDEN);

            // Set mock temperature data
            ui_panel_controls_temp_set_bed(25, 0);

            printf("Bed temp panel displayed\n");
        }
    }

    // Special case: Show extrusion panel if requested
    if (show_extrusion) {
        printf("Creating and showing extrusion sub-screen...\n");

        // Create extrusion panel
        lv_obj_t* extrusion_panel = (lv_obj_t*)lv_xml_create(screen, "extrusion_panel", nullptr);
        if (extrusion_panel) {
            ui_panel_controls_extrusion_setup(extrusion_panel, screen);

            // Hide controls launcher, show extrusion panel
            lv_obj_add_flag(panels[UI_PANEL_CONTROLS], LV_OBJ_FLAG_HIDDEN);

            // Set mock temperature data (nozzle at room temp)
            ui_panel_controls_extrusion_set_temp(25, 0);

            printf("Extrusion panel displayed\n");
        }
    }

    // Special case: Show print status screen if requested
    if (show_print_status) {
        printf("Creating and showing print status screen...\n");

        // Create print status panel
        lv_obj_t* print_status_panel = (lv_obj_t*)lv_xml_create(screen, "print_status_panel", nullptr);
        if (print_status_panel) {
            ui_panel_print_status_setup(print_status_panel, screen);

            // Hide all navigation panels
            for (int i = 0; i < UI_PANEL_COUNT; i++) {
                lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
            }

            // Start mock print simulation (3-hour print, 250 layers)
            ui_panel_print_status_start_mock_print("awesome_benchy.gcode", 250, 10800);

            printf("Print status panel displayed with mock print running\n");
        }
    }

    // Special case: Show file detail view if requested
    if (show_file_detail) {
        printf("Showing print file detail view...\n");

        // Set file data for the first test file
        ui_panel_print_select_set_file("Benchy.gcode",
                                       "A:assets/images/thumbnail-placeholder.png",
                                       "2h 30m", "45g");

        // Show detail view
        ui_panel_print_select_show_detail_view();

        printf("File detail view displayed\n");
    }

    // Special case: Show step progress widget test panel
    if (show_step_test) {
        printf("Creating and showing step progress test panel...\n");

        // Create step test panel (standalone, not part of app_layout)
        lv_obj_t* step_test_panel = (lv_obj_t*)lv_xml_create(screen, "step_progress_test", nullptr);
        if (step_test_panel) {
            ui_panel_step_test_setup(step_test_panel);

            // Hide app_layout to show only the test panel
            lv_obj_add_flag(app_layout, LV_OBJ_FLAG_HIDDEN);

            printf("Step progress test panel displayed\n");
        } else {
            LV_LOG_ERROR("Failed to create step progress test panel");
        }
    }

    // Initialize Moonraker connection
    LV_LOG_USER("Initializing Moonraker client...");
    MoonrakerClient moonraker_client;

    // Check if first-run wizard is required (skip for special test panels)
    if ((force_wizard || config->is_wizard_required()) && !show_step_test && !show_keypad) {
        LV_LOG_USER("Starting first-run configuration wizard");

        lv_obj_t* wizard = ui_wizard_create(screen, config, &moonraker_client, []() {
            LV_LOG_USER("Wizard completed - configuration saved");
            // Config will be reloaded on next connection attempt
        });

        if (wizard) {
            LV_LOG_USER("Wizard created successfully");
        } else {
            LV_LOG_ERROR("Failed to create wizard");
        }
    }

    // Build WebSocket URL from config
    std::string moonraker_url = "ws://" +
                               config->get<std::string>(config->df() + "moonraker_host") + ":" +
                               std::to_string(config->get<int>(config->df() + "moonraker_port")) + "/websocket";

    // Register notification callback to update printer state
    moonraker_client.register_notify_update([](json& notification) {
        printer_state.update_from_notification(notification);
    });

    // Connect to Moonraker
    spdlog::info("Connecting to Moonraker at {}", moonraker_url);
    int connect_result = moonraker_client.connect(moonraker_url.c_str(),
        [&moonraker_client]() {
            spdlog::info("✓ Connected to Moonraker");
            printer_state.set_connection_state(2, "Connected");

            // Start auto-discovery (must be called AFTER connection is established)
            moonraker_client.discover_printer([]() {
                spdlog::info("✓ Printer auto-discovery complete");
            });
        },
        []() {
            spdlog::warn("✗ Disconnected from Moonraker");
            printer_state.set_connection_state(0, "Disconnected");
        }
    );

    if (connect_result != 0) {
        spdlog::error("Failed to initiate Moonraker connection (code {})", connect_result);
        printer_state.set_connection_state(0, "Disconnected");
    }

    // Auto-screenshot timer (2 seconds after UI creation)
    uint32_t screenshot_time = SDL_GetTicks() + 2000;
    bool screenshot_taken = false;

    // Mock print simulation timer (tick every second)
    uint32_t last_tick_time = SDL_GetTicks();

    // Mock printer data timer (tick every second)
    uint32_t last_mock_data_time = SDL_GetTicks();

    // Main event loop - Let LVGL handle SDL events internally via lv_timer_handler()
    // Loop continues while display exists (exits when window closed)
    while (lv_display_get_next(NULL)) {
        // Check for Cmd+Q (macOS) or Win+Q (Windows) to quit
        SDL_Keymod modifiers = SDL_GetModState();
        const Uint8* keyboard_state = SDL_GetKeyboardState(NULL);
        if ((modifiers & KMOD_GUI) && keyboard_state[SDL_SCANCODE_Q]) {
            LV_LOG_USER("Cmd+Q/Win+Q pressed - exiting...");
            break;
        }

        // Auto-screenshot after 2 seconds
        if (!screenshot_taken && SDL_GetTicks() >= screenshot_time) {
            save_screenshot();
            screenshot_taken = true;
        }

        // Tick mock print simulation (once per second)
        uint32_t current_time = SDL_GetTicks();
        if (current_time - last_tick_time >= 1000) {
            ui_panel_print_status_tick_mock_print();
            last_tick_time = current_time;
        }

        // Tick mock printer data (once per second)
        if (current_time - last_mock_data_time >= 1000) {
            update_mock_printer_data();
            last_mock_data_time = current_time;
        }

        // Run LVGL tasks - internally polls SDL events and processes input
        lv_timer_handler();
        fflush(stdout);
        SDL_Delay(5);  // Small delay to prevent 100% CPU usage
    }

    // Cleanup
    LV_LOG_USER("Shutting down...");
    lv_deinit();  // LVGL handles SDL cleanup internally

    return 0;
}

