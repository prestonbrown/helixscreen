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
#include "ui_switch.h"
#include "ui_keyboard.h"
#include "ui_wizard.h"
#include "ui_panel_step_test.h"
#include "ui_panel_test.h"
#include "ui_icon_loader.h"
#include "printer_state.h"
#include "moonraker_client.h"
#include "config.h"
#include "tips_manager.h"
#include <spdlog/spdlog.h>
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <queue>
#include <mutex>
#include <memory>

// LVGL display and input
static lv_display_t* display = nullptr;
static lv_indev_t* indev_mouse = nullptr;
static lv_theme_t* current_theme = nullptr;
static bool dark_mode_enabled = true;  // Start in dark mode

// Screen dimensions (configurable via command line, default to medium size)
static int SCREEN_WIDTH = UI_SCREEN_MEDIUM_W;
static int SCREEN_HEIGHT = UI_SCREEN_MEDIUM_H;

// Printer state management
static PrinterState printer_state;

// Thread-safe queue for Moonraker notifications (cross-thread communication)
static std::queue<json> notification_queue;
static std::mutex notification_mutex;

// Overlay panel tracking for proper lifecycle management
struct OverlayPanels {
    lv_obj_t* motion = nullptr;
    lv_obj_t* nozzle_temp = nullptr;
    lv_obj_t* bed_temp = nullptr;
    lv_obj_t* extrusion = nullptr;
    lv_obj_t* print_status = nullptr;
} static overlay_panels;

// Forward declarations
static void save_screenshot();

// Initialize LVGL with SDL
static bool init_lvgl() {
    lv_init();

    // LVGL's SDL driver handles window creation internally
    display = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!display) {
        LV_LOG_ERROR("Failed to create LVGL SDL display");
        lv_deinit();  // Clean up partial LVGL state
        return false;
    }

    // Create mouse input device
    indev_mouse = lv_sdl_mouse_create();
    if (!indev_mouse) {
        LV_LOG_ERROR("Failed to create LVGL SDL mouse input");
        lv_deinit();  // Clean up partial LVGL state
        return false;
    }

    LV_LOG_USER("LVGL initialized: %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT);

    // Initialize SVG decoder for loading .svg files
    lv_svg_decoder_init();

    return true;
}

// Initialize LVGL default theme with custom colors
static void init_theme(bool dark_mode) {
    dark_mode_enabled = dark_mode;

    // Use colors from ui_theme.h (single source of truth)
    lv_color_t primary_color = UI_COLOR_PRIMARY;      // Red accent
    lv_color_t secondary_color = UI_COLOR_SECONDARY;  // Blue accent

    // Initialize default theme with custom colors
    // LVGL 9.4 API takes single font parameter (uses it as base, auto-scales for headings)
    current_theme = lv_theme_default_init(
        display,
        primary_color,
        secondary_color,
        dark_mode,
        UI_FONT_BODY   // Base font (16px body text) from ui_theme.h
    );

    if (current_theme) {
        lv_display_set_theme(display, current_theme);
        spdlog::info("LVGL theme initialized: {} mode", dark_mode ? "dark" : "light");
    } else {
        spdlog::error("Failed to initialize LVGL theme");
    }
}

// Toggle between dark and light modes
static void toggle_dark_mode() {
    bool new_dark_mode = !dark_mode_enabled;
    spdlog::info("Toggling theme: {} → {} mode",
                dark_mode_enabled ? "dark" : "light",
                new_dark_mode ? "dark" : "light");

    // Reinitialize theme with new mode
    init_theme(new_dark_mode);

    // Force redraw of entire screen to apply new theme
    lv_obj_t* screen = lv_screen_active();
    lv_obj_invalidate(screen);

    spdlog::info("Theme toggled to {} mode", new_dark_mode ? "dark" : "light");
}

// Show splash screen with HelixScreen logo
static void show_splash_screen() {
    spdlog::info("Showing splash screen");

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Theme handles background color

    // Disable scrollbars on screen
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Create centered container for logo (disable scrolling)
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);  // Disable scrollbars
    lv_obj_set_style_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);  // Start invisible for fade-in
    lv_obj_center(container);

    // Create image widget for logo
    lv_obj_t* logo = lv_image_create(container);
    const char* logo_path = "A:assets/images/helixscreen-logo.png";
    lv_image_set_src(logo, logo_path);

    // Get actual image dimensions
    lv_image_header_t header;
    lv_result_t res = lv_image_decoder_get_info(logo_path, &header);

    if (res == LV_RESULT_OK) {
        // Scale logo to fill more of the screen (60% of screen width)
        lv_coord_t target_size = (SCREEN_WIDTH * 3) / 5;  // 60% of screen width
        if (SCREEN_HEIGHT < 500) {  // Tiny screen
            target_size = SCREEN_WIDTH / 2;  // 50% on tiny screens
        }

        // Calculate scale: (target_size * 256) / actual_width
        // LVGL uses 1/256 scale units (256 = 100%, 128 = 50%, etc.)
        uint32_t width = header.w;   // Copy bit-field to local var for logging
        uint32_t height = header.h;  // Copy bit-field to local var for logging
        int scale = (target_size * 256) / width;
        lv_image_set_scale(logo, scale);

        spdlog::debug("Logo: {}x{} scaled to {} (scale factor: {})",
                     width, height, target_size, scale);
    } else {
        spdlog::warn("Could not get logo dimensions, using default scale");
        lv_image_set_scale(logo, 128);  // 50% scale as fallback
    }

    // Create fade-in animation (0.5 seconds)
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, container);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 500);  // 500ms = 0.5 seconds
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa((lv_obj_t*)obj, value, LV_PART_MAIN);
    });
    lv_anim_start(&anim);

    // Run LVGL timer to process fade-in animation and keep splash visible
    // Total display time: 2 seconds (including 0.5s fade-in)
    uint32_t splash_start = SDL_GetTicks();
    uint32_t splash_duration = 2000;  // 2 seconds total

    while (SDL_GetTicks() - splash_start < splash_duration) {
        lv_timer_handler();  // Process animations and rendering
        SDL_Delay(5);
    }

    // Clean up splash screen
    lv_obj_delete(container);

    spdlog::info("Splash screen complete");
}

// Save screenshot using SDL renderer
// Simple BMP file writer for ARGB8888 format
static bool write_bmp(const char* filename, const uint8_t* data, int width, int height) {
    // RAII for file handle - automatically closes on all return paths
    std::unique_ptr<FILE, decltype(&fclose)> f(fopen(filename, "wb"), fclose);
    if (!f) return false;

    // BMP header (54 bytes total)
    uint32_t file_size = 54 + (width * height * 4);
    uint32_t pixel_offset = 54;
    uint32_t dib_size = 40;
    uint16_t planes = 1;
    uint16_t bpp = 32;
    uint32_t reserved = 0;
    uint32_t compression = 0;
    uint32_t ppm = 2835;  // pixels per meter
    uint32_t colors = 0;

    // BMP file header (14 bytes)
    fputc('B', f.get()); fputc('M', f.get());            // Signature
    fwrite(&file_size, 4, 1, f.get());                   // File size
    fwrite(&reserved, 4, 1, f.get());                    // Reserved
    fwrite(&pixel_offset, 4, 1, f.get());                // Pixel data offset

    // DIB header (40 bytes)
    fwrite(&dib_size, 4, 1, f.get());                    // DIB header size
    fwrite(&width, 4, 1, f.get());                       // Width
    fwrite(&height, 4, 1, f.get());                      // Height
    fwrite(&planes, 2, 1, f.get());                      // Planes
    fwrite(&bpp, 2, 1, f.get());                         // Bits per pixel
    fwrite(&compression, 4, 1, f.get());                 // Compression (none)
    uint32_t image_size = width * height * 4;
    fwrite(&image_size, 4, 1, f.get());                  // Image size
    fwrite(&ppm, 4, 1, f.get());                         // X pixels per meter
    fwrite(&ppm, 4, 1, f.get());                         // Y pixels per meter
    fwrite(&colors, 4, 1, f.get());                      // Colors in palette
    fwrite(&colors, 4, 1, f.get());                      // Important colors

    // Write pixel data (BMP is bottom-up, so flip rows)
    for (int y = height - 1; y >= 0; y--) {
        fwrite(data + (y * width * 4), 4, width, f.get());
    }

    // File automatically closed by unique_ptr destructor
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
    bool show_test_panel = false;  // Special flag for test/development panel
    bool force_wizard = false;  // Force wizard to run even if config exists
    int wizard_step = -1;  // Specific wizard step to show (-1 means normal flow)
    bool panel_requested = false;  // Track if user explicitly requested a panel via CLI
    int display_num = -1;  // Display number for window placement (-1 means unset)
    int x_pos = -1;  // X position for window placement (-1 means unset)
    int y_pos = -1;  // Y position for window placement (-1 means unset)
    bool screenshot_enabled = false;  // Enable automatic screenshot
    int screenshot_delay_sec = 2;  // Screenshot delay in seconds (default: 2)
    int timeout_sec = 0;  // Auto-quit timeout in seconds (0 = disabled)
    int verbosity = 0;  // Verbosity level (0=warn, 1=info, 2=debug, 3=trace)

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
                panel_requested = true;  // User explicitly requested a panel
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
                } else if (strcmp(panel_arg, "test") == 0) {
                    show_test_panel = true;
                } else {
                    printf("Unknown panel: %s\n", panel_arg);
                    printf("Available panels: home, controls, motion, nozzle-temp, bed-temp, extrusion, print-status, filament, settings, advanced, print-select, step-test, test\n");
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
        } else if (strcmp(argv[i], "--wizard-step") == 0) {
            if (i + 1 < argc) {
                wizard_step = atoi(argv[++i]);
                force_wizard = true;
                if (wizard_step < 1 || wizard_step > 7) {
                    printf("Error: wizard step must be 1-7\n");
                    return 1;
                }
            } else {
                printf("Error: --wizard-step requires an argument (1-7)\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--display") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 10) {
                    printf("Error: invalid display number (must be 0-10): %s\n", argv[i]);
                    return 1;
                }
                display_num = (int)val;
            } else {
                printf("Error: -d/--display requires a number argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--x-pos") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 10000) {
                    printf("Error: invalid x position (must be 0-10000): %s\n", argv[i]);
                    return 1;
                }
                x_pos = (int)val;
            } else {
                printf("Error: -x/--x-pos requires a number argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--y-pos") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 10000) {
                    printf("Error: invalid y position (must be 0-10000): %s\n", argv[i]);
                    return 1;
                }
                y_pos = (int)val;
            } else {
                printf("Error: -y/--y-pos requires a number argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--screenshot") == 0) {
            screenshot_enabled = true;
            // Check if next arg is a number (delay in seconds)
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[i + 1], &endptr, 10);
                // If next arg is a valid number, use it as delay
                if (*endptr == '\0' && val > 0 && val <= 60) {
                    screenshot_delay_sec = (int)val;
                    i++;  // Consume the delay argument
                }
                // Otherwise, use default delay (next arg is probably a different flag)
            }
        } else if (strcmp(argv[i], "--timeout") == 0 || strcmp(argv[i], "-t") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 1 || val > 3600) {
                    printf("Error: invalid timeout (must be 1-3600 seconds): %s\n", argv[i]);
                    return 1;
                }
                timeout_sec = (int)val;
            } else {
                printf("Error: --timeout/-t requires a number argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-vv") == 0 || strcmp(argv[i], "-vvv") == 0) {
            // Count the number of 'v' characters for verbosity level
            const char* p = argv[i];
            while (*p == '-') p++;  // Skip leading dashes
            while (*p == 'v') {
                verbosity++;
                p++;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbosity++;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -s, --size <size>    Screen size: tiny, small, medium, large (default: medium)\n");
            printf("  -p, --panel <panel>  Initial panel (default: home)\n");
            printf("  -k, --keypad         Show numeric keypad for testing\n");
            printf("  -w, --wizard         Force first-run configuration wizard\n");
            printf("  --wizard-step <step> Jump to specific wizard step for testing\n");
            printf("  -d, --display <n>    Display number for window placement (0, 1, 2...)\n");
            printf("  -x, --x-pos <n>      X coordinate for window position\n");
            printf("  -y, --y-pos <n>      Y coordinate for window position\n");
            printf("  --screenshot [sec]   Take screenshot after delay (default: 2 seconds)\n");
            printf("  -t, --timeout <sec>  Auto-quit after specified seconds (1-3600)\n");
            printf("  -v, --verbose        Increase verbosity (-v=info, -vv=debug, -vvv=trace)\n");
            printf("  -h, --help           Show this help message\n");
            printf("\nAvailable panels:\n");
            printf("  home, controls, motion, nozzle-temp, bed-temp, extrusion,\n");
            printf("  print-status, filament, settings, advanced, print-select\n");
            printf("\nScreen sizes:\n");
            printf("  tiny   = %dx%d\n", UI_SCREEN_TINY_W, UI_SCREEN_TINY_H);
            printf("  small  = %dx%d\n", UI_SCREEN_SMALL_W, UI_SCREEN_SMALL_H);
            printf("  medium = %dx%d (default)\n", UI_SCREEN_MEDIUM_W, UI_SCREEN_MEDIUM_H);
            printf("  large  = %dx%d\n", UI_SCREEN_LARGE_W, UI_SCREEN_LARGE_H);
            printf("\nWizard steps:\n");
            printf("  wifi, connection, printer-identify, bed, hotend, fan, led, summary\n");
            printf("\nWindow placement:\n");
            printf("  Use -d to center window on specific display\n");
            printf("  Use -x/-y for exact pixel coordinates (both required)\n");
            printf("  Examples:\n");
            printf("    %s --display 1        # Center on display 1\n", argv[0]);
            printf("    %s -x 100 -y 200      # Position at (100, 200)\n", argv[0]);
            return 0;
        } else {
            // Legacy support: first positional arg is panel name
            if (i == 1 && argv[i][0] != '-') {
                const char* panel_arg = argv[i];
                panel_requested = true;  // User explicitly requested a panel
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

    // Set spdlog log level based on verbosity flags
    switch (verbosity) {
        case 0:
            spdlog::set_level(spdlog::level::warn);  // Default: warnings and errors only
            break;
        case 1:
            spdlog::set_level(spdlog::level::info);  // -v: general information
            break;
        case 2:
            spdlog::set_level(spdlog::level::debug);  // -vv: debug information
            break;
        default:  // 3 or more
            spdlog::set_level(spdlog::level::trace);  // -vvv: trace everything
            break;
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

    // Set window position environment variables for LVGL SDL driver
    if (display_num >= 0) {
        char display_str[32];
        snprintf(display_str, sizeof(display_str), "%d", display_num);
        if (setenv("HELIX_SDL_DISPLAY", display_str, 1) != 0) {
            spdlog::error("Failed to set HELIX_SDL_DISPLAY environment variable");
            return 1;
        }
        printf("Window will be centered on display %d\n", display_num);
    }
    if (x_pos >= 0 && y_pos >= 0) {
        char x_str[32], y_str[32];
        snprintf(x_str, sizeof(x_str), "%d", x_pos);
        snprintf(y_str, sizeof(y_str), "%d", y_pos);
        if (setenv("HELIX_SDL_XPOS", x_str, 1) != 0 || setenv("HELIX_SDL_YPOS", y_str, 1) != 0) {
            spdlog::error("Failed to set window position environment variables");
            return 1;
        }
        printf("Window will be positioned at (%d, %d)\n", x_pos, y_pos);
    } else if ((x_pos >= 0 && y_pos < 0) || (x_pos < 0 && y_pos >= 0)) {
        printf("Warning: Both -x and -y must be specified for exact positioning. Ignoring.\n");
    }

    // Initialize LVGL (handles SDL internally)
    if (!init_lvgl()) {
        return 1;
    }

    // Initialize LVGL theme with dark mode (auto-applies to all widgets)
    init_theme(true);  // Start in dark mode

    // Show splash screen (DISABLED for faster dev iteration)
    // show_splash_screen();

    // Create main screen
    lv_obj_t* screen = lv_screen_active();

    // Set window icon (after screen is created)
    ui_set_window_icon(display);

    // Initialize app-level resize handler for responsive layouts
    ui_resize_handler_init(screen);

    // Initialize tips manager (uses standard C++ file I/O, not LVGL's "A:" filesystem)
    TipsManager* tips_mgr = TipsManager::get_instance();
    if (!tips_mgr->init("data/printing_tips.json")) {
        spdlog::warn("Tips manager failed to initialize - tips will not be available");
    } else {
        spdlog::info("Loaded {} tips (version: {})", tips_mgr->get_total_tips(), tips_mgr->get_version());
    }

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
    lv_xml_register_font(NULL, "montserrat_10", &lv_font_montserrat_10);
    lv_xml_register_font(NULL, "montserrat_12", &lv_font_montserrat_12);
    lv_xml_register_font(NULL, "montserrat_14", &lv_font_montserrat_14);
    lv_xml_register_font(NULL, "montserrat_16", &lv_font_montserrat_16);
    lv_xml_register_font(NULL, "montserrat_20", &lv_font_montserrat_20);
    lv_xml_register_font(NULL, "montserrat_28", &lv_font_montserrat_28);
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

    // Register custom widgets (must be before XML component registration)
    ui_icon_register_widget();
    ui_switch_register();

    // Initialize component systems (BEFORE XML registration)
    ui_component_header_bar_init();

    // WORKAROUND: Add small delay to stabilize SDL/LVGL initialization
    // Prevents race condition between SDL2 and LVGL 9 XML component registration
    SDL_Delay(100);

    // Register XML components (globals first to make constants available)
    LV_LOG_USER("Registering XML components...");
    lv_xml_register_component_from_file("A:ui_xml/globals.xml");

    // Register responsive constants (AFTER globals, BEFORE components that use them)
    ui_switch_register_responsive_constants();

    lv_xml_register_component_from_file("A:ui_xml/icon.xml");
    lv_xml_register_component_from_file("A:ui_xml/header_bar.xml");
    lv_xml_register_component_from_file("A:ui_xml/confirmation_dialog.xml");
    lv_xml_register_component_from_file("A:ui_xml/tip_detail_dialog.xml");
    lv_xml_register_component_from_file("A:ui_xml/numeric_keypad_modal.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_file_card.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_file_list_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_file_detail.xml");
    lv_xml_register_component_from_file("A:ui_xml/navigation_bar.xml");
    lv_xml_register_component_from_file("A:ui_xml/home_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/controls_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/motion_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/nozzle_temp_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/bed_temp_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/extrusion_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_status_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/filament_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/settings_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/advanced_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/test_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_select_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/step_progress_test.xml");
    lv_xml_register_component_from_file("A:ui_xml/app_layout.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_container.xml");
    lv_xml_register_component_from_file("A:ui_xml/network_list_item.xml");
    lv_xml_register_component_from_file("A:ui_xml/wifi_password_modal.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_test_rollers.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_wifi_setup.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_connection.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_printer_identify.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_bed_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_hotend_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_fan_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_led_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_summary.xml");

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

    // Force layout calculation for all LV_SIZE_CONTENT widgets
    lv_obj_update_layout(screen);

    // Register app_layout with navigation system (to prevent hiding it)
    ui_nav_set_app_layout(app_layout);

    // Find navbar and panel widgets
    // app_layout > navbar (child 0), content_area (child 1)
    lv_obj_t* navbar = lv_obj_get_child(app_layout, 0);
    lv_obj_t* content_area = lv_obj_get_child(app_layout, 1);

    // Defensive programming: verify XML structure matches expectations
    if (!navbar || !content_area) {
        spdlog::error("Failed to find navbar/content_area in app_layout - XML structure mismatch");
        spdlog::error("Expected app_layout > navbar (child 0), content_area (child 1)");
        lv_deinit();
        return 1;
    }

    // Wire up navigation button click handlers and trigger initial color update
    ui_nav_wire_events(navbar);

    // Find all panel widgets in content area
    lv_obj_t* panels[UI_PANEL_COUNT];
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panels[i] = lv_obj_get_child(content_area, i);
        if (!panels[i]) {
            spdlog::error("Missing panel {} in content_area - expected {} panels", i, (int)UI_PANEL_COUNT);
            spdlog::error("XML structure changed or panels missing from app_layout.xml");
            lv_deinit();
            return 1;
        }
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
    overlay_panels.print_status = (lv_obj_t*)lv_xml_create(screen, "print_status_panel", nullptr);
    if (overlay_panels.print_status) {
        ui_panel_print_status_setup(overlay_panels.print_status, screen);
        lv_obj_add_flag(overlay_panels.print_status, LV_OBJ_FLAG_HIDDEN);  // Hidden by default

        // Wire print status panel to print select (for launching prints)
        ui_panel_print_select_set_print_status_panel(overlay_panels.print_status);

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

        // Create motion panel (tracked for cleanup)
        overlay_panels.motion = (lv_obj_t*)lv_xml_create(screen, "motion_panel", nullptr);
        if (overlay_panels.motion) {
            ui_panel_motion_setup(overlay_panels.motion, screen);

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

        // Create nozzle temp panel (tracked for cleanup)
        overlay_panels.nozzle_temp = (lv_obj_t*)lv_xml_create(screen, "nozzle_temp_panel", nullptr);
        if (overlay_panels.nozzle_temp) {
            ui_panel_controls_temp_nozzle_setup(overlay_panels.nozzle_temp, screen);

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

        // Create bed temp panel (tracked for cleanup)
        overlay_panels.bed_temp = (lv_obj_t*)lv_xml_create(screen, "bed_temp_panel", nullptr);
        if (overlay_panels.bed_temp) {
            ui_panel_controls_temp_bed_setup(overlay_panels.bed_temp, screen);

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

        // Create extrusion panel (tracked for cleanup)
        overlay_panels.extrusion = (lv_obj_t*)lv_xml_create(screen, "extrusion_panel", nullptr);
        if (overlay_panels.extrusion) {
            ui_panel_controls_extrusion_setup(overlay_panels.extrusion, screen);

            // Hide controls launcher, show extrusion panel
            lv_obj_add_flag(panels[UI_PANEL_CONTROLS], LV_OBJ_FLAG_HIDDEN);

            // Set mock temperature data (nozzle at room temp)
            ui_panel_controls_extrusion_set_temp(25, 0);

            printf("Extrusion panel displayed\n");
        }
    }

    // Special case: Show print status screen if requested
    if (show_print_status) {
        printf("Showing print status screen...\n");

        // Use already-created print status panel (no duplicate creation)
        if (overlay_panels.print_status) {
            // Hide all navigation panels
            for (int i = 0; i < UI_PANEL_COUNT; i++) {
                lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
            }

            // Show print status panel
            lv_obj_clear_flag(overlay_panels.print_status, LV_OBJ_FLAG_HIDDEN);

            // Start mock print simulation (3-hour print, 250 layers)
            ui_panel_print_status_start_mock_print("awesome_benchy.gcode", 250, 10800);

            printf("Print status panel displayed with mock print running\n");
        } else {
            spdlog::error("Print status panel not created - cannot show");
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

    // Special case: Show test/development panel
    if (show_test_panel) {
        printf("Creating and showing test panel...\n");

        // Create test panel (standalone, not part of app_layout)
        lv_obj_t* test_panel = (lv_obj_t*)lv_xml_create(screen, "test_panel", nullptr);
        if (test_panel) {
            // Setup panel (populate info labels)
            ui_panel_test_setup(test_panel);
            // Hide app_layout to show only the test panel
            lv_obj_add_flag(app_layout, LV_OBJ_FLAG_HIDDEN);

            printf("Test panel displayed\n");
        } else {
            LV_LOG_ERROR("Failed to create test panel");
        }
    }

    // Initialize Moonraker connection
    LV_LOG_USER("Initializing Moonraker client...");
    MoonrakerClient moonraker_client;

    // Initialize global keyboard BEFORE wizard (required for textarea registration)
    // NOTE: Keyboard is created early but will appear on top due to being moved to top layer below
    ui_keyboard_init(screen);

    // Check if first-run wizard is required (skip for special test panels and explicit panel requests)
    if ((force_wizard || config->is_wizard_required()) && !show_step_test && !show_test_panel && !show_keypad && !panel_requested) {
        spdlog::info("Starting first-run configuration wizard");

        // Register wizard event callbacks and responsive constants BEFORE creating
        ui_wizard_register_event_callbacks();
        ui_wizard_register_responsive_constants();

        lv_obj_t* wizard = ui_wizard_create(screen);

        if (wizard) {
            spdlog::info("Wizard created successfully");

            // Set initial step (screen loader sets appropriate title)
            int initial_step = (wizard_step >= 1) ? wizard_step : 1;
            ui_wizard_navigate_to_step(initial_step);

            // Move keyboard to top layer so it appears above the full-screen wizard overlay
            lv_obj_t* keyboard = ui_keyboard_get_instance();
            if (keyboard) {
                lv_obj_move_foreground(keyboard);
                spdlog::debug("[Keyboard] Moved to foreground (above wizard overlay)");
            }
        } else {
            spdlog::error("Failed to create wizard");
        }
    }

    // Build WebSocket URL from config
    std::string moonraker_url = "ws://" +
                               config->get<std::string>(config->df() + "moonraker_host") + ":" +
                               std::to_string(config->get<int>(config->df() + "moonraker_port")) + "/websocket";

    // Register notification callback to queue updates for main thread
    // CRITICAL: Moonraker callbacks run on background thread, but LVGL is NOT thread-safe
    // Queue notifications here, process on main thread in event loop
    moonraker_client.register_notify_update([](json& notification) {
        std::lock_guard<std::mutex> lock(notification_mutex);
        notification_queue.push(notification);
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

    // Auto-screenshot timer (configurable delay after UI creation)
    uint32_t screenshot_time = SDL_GetTicks() + (screenshot_delay_sec * 1000);
    bool screenshot_taken = false;

    // Auto-quit timeout timer (if enabled)
    uint32_t start_time = SDL_GetTicks();
    uint32_t timeout_ms = timeout_sec * 1000;

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

        // Auto-screenshot after configured delay (only if enabled)
        if (screenshot_enabled && !screenshot_taken && SDL_GetTicks() >= screenshot_time) {
            save_screenshot();
            screenshot_taken = true;
        }

        // Auto-quit after timeout (if enabled)
        if (timeout_sec > 0 && (SDL_GetTicks() - start_time) >= timeout_ms) {
            spdlog::info("Timeout reached ({} seconds) - exiting...", timeout_sec);
            break;
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

        // Process queued Moonraker notifications on main thread (LVGL thread-safety)
        {
            std::lock_guard<std::mutex> lock(notification_mutex);
            while (!notification_queue.empty()) {
                json notification = notification_queue.front();
                notification_queue.pop();
                printer_state.update_from_notification(notification);
            }
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

