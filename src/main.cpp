// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

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

#include "ui_bed_mesh.h"
#include "ui_card.h"
#include "ui_component_header_bar.h"
#include "ui_component_keypad.h"
#include "ui_fonts.h"
#include "ui_gcode_viewer.h"
#include "ui_icon.h"
#include "ui_icon_loader.h"
#include "ui_keyboard.h"
#include "ui_nav.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_controls.h"
#include "ui_panel_controls_extrusion.h"
#include "ui_panel_controls_temp.h"
#include "ui_panel_filament.h"
#include "ui_panel_gcode_test.h"
#include "ui_panel_glyphs.h"
#include "ui_panel_home.h"
#include "ui_panel_motion.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_panel_settings.h"
#include "ui_panel_step_test.h"
#include "ui_panel_test.h"
#include "ui_switch.h"
#include "ui_text.h"
#include "ui_theme.h"
#include "ui_utils.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/libs/svg/lv_svg_decoder.h"
#include "lvgl/src/xml/lv_xml.h"
#include "material_icons.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "tips_manager.h"

#include <spdlog/spdlog.h>

#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <queue>

// LVGL display and input
static lv_display_t* display = nullptr;
static lv_indev_t* indev_mouse = nullptr;

// Screen dimensions (configurable via command line, default to small size)
static int SCREEN_WIDTH = UI_SCREEN_SMALL_W;
static int SCREEN_HEIGHT = UI_SCREEN_SMALL_H;

// Printer state management
static PrinterState printer_state;

// Moonraker API instances (initialized in main after config load)
static MoonrakerClient* moonraker_client = nullptr;
static MoonrakerAPI* moonraker_api = nullptr;

// Runtime configuration
static RuntimeConfig g_runtime_config;

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

// Global accessor functions implementation
MoonrakerClient* get_moonraker_client() {
    return moonraker_client;
}

MoonrakerAPI* get_moonraker_api() {
    return moonraker_api;
}

PrinterState& get_printer_state() {
    return printer_state;
}

const RuntimeConfig& get_runtime_config() {
    return g_runtime_config;
}

RuntimeConfig* get_mutable_runtime_config() {
    return &g_runtime_config;
}

// Forward declarations
static void save_screenshot();
static void initialize_moonraker_client(Config* config);

// Parse command-line arguments
// Returns true on success, false if help was shown or error occurred
static bool parse_command_line_args(
    int argc, char** argv, int& initial_panel, bool& show_motion, bool& show_nozzle_temp,
    bool& show_bed_temp, bool& show_extrusion, bool& show_print_status, bool& show_file_detail,
    bool& show_keypad, bool& show_keyboard, bool& show_step_test, bool& show_test_panel,
    bool& show_gcode_test, bool& show_bed_mesh, bool& show_glyphs, bool& force_wizard, int& wizard_step,
    bool& panel_requested, int& display_num, int& x_pos, int& y_pos, bool& screenshot_enabled,
    int& screenshot_delay_sec, int& timeout_sec, int& verbosity, bool& dark_mode,
    bool& theme_requested, int& dpi) {
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
                    return false;
                }
            } else {
                printf("Error: -s/--size requires an argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--panel") == 0) {
            if (i + 1 < argc) {
                const char* panel_arg = argv[++i];
                panel_requested = true; // User explicitly requested a panel
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
                } else if (strcmp(panel_arg, "print-status") == 0 ||
                           strcmp(panel_arg, "printing") == 0) {
                    show_print_status = true;
                } else if (strcmp(panel_arg, "filament") == 0) {
                    initial_panel = UI_PANEL_FILAMENT;
                } else if (strcmp(panel_arg, "settings") == 0) {
                    initial_panel = UI_PANEL_SETTINGS;
                } else if (strcmp(panel_arg, "advanced") == 0) {
                    initial_panel = UI_PANEL_ADVANCED;
                } else if (strcmp(panel_arg, "print-select") == 0 ||
                           strcmp(panel_arg, "print_select") == 0) {
                    initial_panel = UI_PANEL_PRINT_SELECT;
                } else if (strcmp(panel_arg, "file-detail") == 0 ||
                           strcmp(panel_arg, "print-file-detail") == 0) {
                    initial_panel = UI_PANEL_PRINT_SELECT;
                    show_file_detail = true;
                } else if (strcmp(panel_arg, "step-test") == 0 ||
                           strcmp(panel_arg, "step_test") == 0) {
                    show_step_test = true;
                } else if (strcmp(panel_arg, "test") == 0) {
                    show_test_panel = true;
                } else if (strcmp(panel_arg, "gcode-test") == 0 ||
                           strcmp(panel_arg, "gcode_test") == 0) {
                    show_gcode_test = true;
                } else if (strcmp(panel_arg, "bed-mesh") == 0 || strcmp(panel_arg, "bed_mesh") == 0) {
                    show_bed_mesh = true;
                } else if (strcmp(panel_arg, "glyphs") == 0) {
                    show_glyphs = true;
                } else {
                    printf("Unknown panel: %s\n", panel_arg);
                    printf("Available panels: home, controls, motion, nozzle-temp, bed-temp, bed-mesh, "
                           "extrusion, print-status, filament, settings, advanced, print-select, "
                           "step-test, test, gcode-test, glyphs\n");
                    return false;
                }
            } else {
                printf("Error: -p/--panel requires an argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--keypad") == 0) {
            show_keypad = true;
        } else if (strcmp(argv[i], "--keyboard") == 0 || strcmp(argv[i], "--show-keyboard") == 0) {
            show_keyboard = true;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wizard") == 0) {
            force_wizard = true;
        } else if (strcmp(argv[i], "--wizard-step") == 0) {
            if (i + 1 < argc) {
                wizard_step = atoi(argv[++i]);
                force_wizard = true;
                if (wizard_step < 1 || wizard_step > 8) {
                    printf("Error: wizard step must be 1-8\n");
                    return false;
                }
            } else {
                printf("Error: --wizard-step requires an argument (1-8)\n");
                return false;
            }
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--display") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 10) {
                    printf("Error: invalid display number (must be 0-10): %s\n", argv[i]);
                    return false;
                }
                display_num = (int)val;
            } else {
                printf("Error: -d/--display requires a number argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--x-pos") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 10000) {
                    printf("Error: invalid x position (must be 0-10000): %s\n", argv[i]);
                    return false;
                }
                x_pos = (int)val;
            } else {
                printf("Error: -x/--x-pos requires a number argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--y-pos") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 0 || val > 10000) {
                    printf("Error: invalid y position (must be 0-10000): %s\n", argv[i]);
                    return false;
                }
                y_pos = (int)val;
            } else {
                printf("Error: -y/--y-pos requires a number argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "--dpi") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 50 || val > 500) {
                    printf("Error: invalid DPI (must be 50-500): %s\n", argv[i]);
                    return false;
                }
                dpi = (int)val;
            } else {
                printf("Error: --dpi requires a number argument\n");
                return false;
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
                    i++; // Consume the delay argument
                }
                // Otherwise, use default delay (next arg is probably a different flag)
            }
        } else if (strcmp(argv[i], "--timeout") == 0 || strcmp(argv[i], "-t") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[++i], &endptr, 10);
                if (*endptr != '\0' || val < 1 || val > 3600) {
                    printf("Error: invalid timeout (must be 1-3600 seconds): %s\n", argv[i]);
                    return false;
                }
                timeout_sec = (int)val;
            } else {
                printf("Error: --timeout/-t requires a number argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "--dark") == 0) {
            dark_mode = true;
            theme_requested = true;
        } else if (strcmp(argv[i], "--light") == 0) {
            dark_mode = false;
            theme_requested = true;
        } else if (strcmp(argv[i], "--test") == 0) {
            g_runtime_config.test_mode = true;
        } else if (strcmp(argv[i], "--skip-splash") == 0) {
            g_runtime_config.skip_splash = true;
        } else if (strcmp(argv[i], "--real-wifi") == 0) {
            g_runtime_config.use_real_wifi = true;
        } else if (strcmp(argv[i], "--real-ethernet") == 0) {
            g_runtime_config.use_real_ethernet = true;
        } else if (strcmp(argv[i], "--real-moonraker") == 0) {
            g_runtime_config.use_real_moonraker = true;
        } else if (strcmp(argv[i], "--real-files") == 0) {
            g_runtime_config.use_real_files = true;
        } else if (strcmp(argv[i], "--gcode-file") == 0) {
            if (i + 1 < argc) {
                g_runtime_config.gcode_test_file = argv[++i];
            } else {
                printf("Error: --gcode-file requires a path argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "--gcode-az") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                double val = strtod(argv[++i], &endptr);
                if (*endptr != '\0') {
                    printf("Error: --gcode-az requires a numeric value\n");
                    return false;
                }
                g_runtime_config.gcode_camera_azimuth = (float)val;
                g_runtime_config.gcode_camera_azimuth_set = true;
            } else {
                printf("Error: --gcode-az requires a numeric argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "--gcode-el") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                double val = strtod(argv[++i], &endptr);
                if (*endptr != '\0') {
                    printf("Error: --gcode-el requires a numeric value\n");
                    return false;
                }
                g_runtime_config.gcode_camera_elevation = (float)val;
                g_runtime_config.gcode_camera_elevation_set = true;
            } else {
                printf("Error: --gcode-el requires a numeric argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "--gcode-zoom") == 0) {
            if (i + 1 < argc) {
                char* endptr;
                double val = strtod(argv[++i], &endptr);
                if (*endptr != '\0' || val <= 0) {
                    printf("Error: --gcode-zoom requires a positive numeric value\n");
                    return false;
                }
                g_runtime_config.gcode_camera_zoom = (float)val;
                g_runtime_config.gcode_camera_zoom_set = true;
            } else {
                printf("Error: --gcode-zoom requires a numeric argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "--gcode-debug-colors") == 0) {
            g_runtime_config.gcode_debug_colors = true;
        } else if (strcmp(argv[i], "--camera") == 0) {
            if (i + 1 < argc) {
                const char* camera_str = argv[++i];

                // Check for empty string
                if (camera_str[0] == '\0') {
                    printf("Error: --camera requires a non-empty string argument\n");
                    printf("Format: --camera \"az:90.5,el:4.0,zoom:15.5\" (each parameter optional)\n");
                    return false;
                }

                // Parse comma-separated "az:90.5,el:4.0,zoom:15.5" format
                // Each component is optional
                char* str_copy = strdup(camera_str);
                char* token = strtok(str_copy, ",");

                while (token != nullptr) {
                    // Trim leading whitespace
                    while (*token == ' ') token++;

                    // Parse key:value pairs
                    if (strncmp(token, "az:", 3) == 0) {
                        char* endptr;
                        double val = strtod(token + 3, &endptr);
                        if (*endptr == '\0' || *endptr == ' ') {
                            g_runtime_config.gcode_camera_azimuth = (float)val;
                            g_runtime_config.gcode_camera_azimuth_set = true;
                        } else {
                            printf("Error: Invalid azimuth value in --camera: %s\n", token);
                            free(str_copy);
                            return false;
                        }
                    } else if (strncmp(token, "el:", 3) == 0) {
                        char* endptr;
                        double val = strtod(token + 3, &endptr);
                        if (*endptr == '\0' || *endptr == ' ') {
                            g_runtime_config.gcode_camera_elevation = (float)val;
                            g_runtime_config.gcode_camera_elevation_set = true;
                        } else {
                            printf("Error: Invalid elevation value in --camera: %s\n", token);
                            free(str_copy);
                            return false;
                        }
                    } else if (strncmp(token, "zoom:", 5) == 0) {
                        char* endptr;
                        double val = strtod(token + 5, &endptr);
                        if ((*endptr == '\0' || *endptr == ' ') && val > 0) {
                            g_runtime_config.gcode_camera_zoom = (float)val;
                            g_runtime_config.gcode_camera_zoom_set = true;
                        } else {
                            printf("Error: Invalid zoom value in --camera (must be positive): %s\n", token);
                            free(str_copy);
                            return false;
                        }
                    } else {
                        printf("Error: Unknown camera parameter in --camera: %s\n", token);
                        printf("Valid parameters: az:<degrees>, el:<degrees>, zoom:<factor>\n");
                        free(str_copy);
                        return false;
                    }

                    token = strtok(nullptr, ",");
                }

                free(str_copy);
            } else {
                printf("Error: --camera requires a string argument\n");
                printf("Format: --camera \"az:90.5,el:4.0,zoom:15.5\" (each parameter optional)\n");
                return false;
            }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-vv") == 0 ||
                   strcmp(argv[i], "-vvv") == 0) {
            // Count the number of 'v' characters for verbosity level
            const char* p = argv[i];
            while (*p == '-')
                p++; // Skip leading dashes
            while (*p == 'v') {
                verbosity++;
                p++;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbosity++;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -s, --size <size>    Screen size: tiny, small, medium, large (default: "
                   "medium)\n");
            printf("  -p, --panel <panel>  Initial panel (default: home)\n");
            printf("  -k, --keypad         Show numeric keypad for testing\n");
            printf("  --keyboard           Show keyboard for testing (no textarea)\n");
            printf("  -w, --wizard         Force first-run configuration wizard\n");
            printf("  --wizard-step <step> Jump to specific wizard step for testing\n");
            printf("  -d, --display <n>    Display number for window placement (0, 1, 2...)\n");
            printf("  -x, --x-pos <n>      X coordinate for window position\n");
            printf("  -y, --y-pos <n>      Y coordinate for window position\n");
            printf("  --dpi <n>            Display DPI (50-500, default: %d)\n", LV_DPI_DEF);
            printf("  --screenshot [sec]   Take screenshot after delay (default: 2 seconds)\n");
            printf("  -t, --timeout <sec>  Auto-quit after specified seconds (1-3600)\n");
            printf("  --dark               Use dark theme (default)\n");
            printf("  --light              Use light theme\n");
            printf("  --skip-splash        Skip splash screen on startup\n");
            printf("  -v, --verbose        Increase verbosity (-v=info, -vv=debug, -vvv=trace)\n");
            printf("  -h, --help           Show this help message\n");
            printf("\nTest Mode Options:\n");
            printf("  --test               Enable test mode (uses all mocks by default)\n");
            printf("    --real-wifi        Use real WiFi hardware (requires --test)\n");
            printf("    --real-ethernet    Use real Ethernet hardware (requires --test)\n");
            printf("    --real-moonraker   Connect to real printer (requires --test)\n");
            printf("    --real-files       Use real files from printer (requires --test)\n");
            printf("\nG-code Viewer Options:\n");
            printf("  --gcode-file <path>  Load specific G-code file on startup\n");
            printf("  --camera <params>    Set camera params: \"az:90.5,el:4.0,zoom:15.5\"\n");
            printf("                       (each parameter optional, comma-separated)\n");
            printf("  --gcode-az <deg>     Set camera azimuth angle (degrees)\n");
            printf("  --gcode-el <deg>     Set camera elevation angle (degrees)\n");
            printf("  --gcode-zoom <n>     Set camera zoom level (positive number)\n");
            printf("  --gcode-debug-colors Enable per-face debug coloring\n");
            printf("\nAvailable panels:\n");
            printf("  home, controls, motion, nozzle-temp, bed-temp, bed-mesh,\n");
            printf("  extrusion, print-status, filament, settings, advanced,\n");
            printf("  print-select, step-test, test, gcode-test, glyphs\n");
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
            printf("\nTest Mode Examples:\n");
            printf("  %s --test                           # Full mock mode\n", argv[0]);
            printf("  %s --test --real-moonraker          # Test UI with real printer\n", argv[0]);
            printf("  %s --test --real-wifi --real-files  # Real WiFi and files, mock rest\n",
                   argv[0]);
            return false;
        } else {
            // Legacy support: first positional arg is panel name
            if (i == 1 && argv[i][0] != '-') {
                const char* panel_arg = argv[i];
                panel_requested = true; // User explicitly requested a panel
                if (strcmp(panel_arg, "home") == 0) {
                    initial_panel = UI_PANEL_HOME;
                } else if (strcmp(panel_arg, "controls") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                } else if (strcmp(panel_arg, "motion") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_motion = true;
                } else if (strcmp(panel_arg, "print-select") == 0 ||
                           strcmp(panel_arg, "print_select") == 0) {
                    initial_panel = UI_PANEL_PRINT_SELECT;
                } else if (strcmp(panel_arg, "step-test") == 0 ||
                           strcmp(panel_arg, "step_test") == 0) {
                    show_step_test = true;
                } else {
                    printf("Unknown argument: %s\n", argv[i]);
                    printf("Use --help for usage information\n");
                    return false;
                }
            } else {
                printf("Unknown argument: %s\n", argv[i]);
                printf("Use --help for usage information\n");
                return false;
            }
        }
    }

    // Validate test mode flags
    if ((g_runtime_config.use_real_wifi || g_runtime_config.use_real_ethernet ||
         g_runtime_config.use_real_moonraker || g_runtime_config.use_real_files) &&
        !g_runtime_config.test_mode) {
        printf("Error: --real-* flags require --test mode\n");
        printf("Use --help for more information\n");
        return false;
    }

    // Print test mode configuration if enabled
    if (g_runtime_config.test_mode) {
        printf("╔════════════════════════════════════════╗\n");
        printf("║           TEST MODE ENABLED            ║\n");
        printf("╚════════════════════════════════════════╝\n");

        if (g_runtime_config.use_real_wifi)
            printf("  Using REAL WiFi hardware\n");
        else
            printf("  Using MOCK WiFi backend\n");

        if (g_runtime_config.use_real_ethernet)
            printf("  Using REAL Ethernet hardware\n");
        else
            printf("  Using MOCK Ethernet backend\n");

        if (g_runtime_config.use_real_moonraker)
            printf("  Using REAL Moonraker connection\n");
        else
            printf("  Using MOCK Moonraker responses\n");

        if (g_runtime_config.use_real_files)
            printf("  Using REAL files from printer\n");
        else
            printf("  Using TEST file data\n");

        printf("\n");
    }

    return true;
}

// Register fonts and images for XML component system
static void register_fonts_and_images() {
    spdlog::debug("Registering fonts and images...");
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
    lv_xml_register_font(NULL, "montserrat_24", &lv_font_montserrat_24);
    lv_xml_register_font(NULL, "montserrat_28", &lv_font_montserrat_28);
    lv_xml_register_image(NULL, "A:assets/images/printer_400.png",
                          "A:assets/images/printer_400.png");
    lv_xml_register_image(NULL, "filament_spool", "A:assets/images/filament_spool.png");
    lv_xml_register_image(NULL, "A:assets/images/placeholder_thumb_centered.png",
                          "A:assets/images/placeholder_thumb_centered.png");
    lv_xml_register_image(NULL, "A:assets/images/thumbnail-gradient-bg.png",
                          "A:assets/images/thumbnail-gradient-bg.png");
    lv_xml_register_image(NULL, "A:assets/images/thumbnail-placeholder.png",
                          "A:assets/images/thumbnail-placeholder.png");
    lv_xml_register_image(NULL, "A:assets/images/large-extruder-icon.svg",
                          "A:assets/images/large-extruder-icon.svg");
}

// Register XML components from ui_xml/ directory
static void register_xml_components() {
    spdlog::debug("Registering remaining XML components...");
    spdlog::info("[XML DEBUG] Starting XML registration function");

    // Register responsive constants (AFTER globals, BEFORE components that use them)
    ui_switch_register_responsive_constants();
    spdlog::info("[XML DEBUG] Past responsive constants");

    // Register semantic text widgets (AFTER theme init, BEFORE components that use them)
    ui_text_init();

    // Register custom widgets (BEFORE components that use them)
    ui_gcode_viewer_register();

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
    spdlog::info("[XML] Registering bed_mesh_panel.xml...");
    auto ret = lv_xml_register_component_from_file("A:ui_xml/bed_mesh_panel.xml");
    spdlog::info("[XML] bed_mesh_panel.xml registration returned: {}", (int)ret);
    lv_xml_register_component_from_file("A:ui_xml/advanced_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/test_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_select_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/step_progress_test.xml");
    lv_xml_register_component_from_file("A:ui_xml/gcode_test_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/glyphs_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/app_layout.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_container.xml");
    lv_xml_register_component_from_file("A:ui_xml/network_list_item.xml");
    lv_xml_register_component_from_file("A:ui_xml/wifi_password_modal.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_wifi_setup.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_connection.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_printer_identify.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_bed_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_hotend_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_fan_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_led_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_summary.xml");
}

// Initialize all reactive subjects for data binding
static void initialize_subjects() {
    spdlog::debug("Initializing reactive subjects...");
    ui_nav_init();                               // Navigation system (icon colors, active panel)
    ui_panel_home_init_subjects();               // Home panel data bindings
    ui_panel_print_select_init_subjects();       // Print select panel (none yet)
    ui_panel_controls_init_subjects();           // Controls panel launcher
    ui_panel_motion_init_subjects();             // Motion sub-screen position display
    ui_panel_controls_temp_init_subjects();      // Temperature sub-screens
    ui_panel_controls_extrusion_init_subjects(); // Extrusion sub-screen
    ui_panel_filament_init_subjects();           // Filament panel
    ui_panel_settings_init_subjects();           // Settings panel launcher
    ui_panel_print_status_init_subjects();       // Print status screen
    ui_wizard_init_subjects();                   // Wizard subjects (for first-run config)
    printer_state.init_subjects(); // Printer state subjects (CRITICAL: must be before XML creation)
}

// Initialize LVGL with SDL
static bool init_lvgl() {
    lv_init();

    // LVGL's SDL driver handles window creation internally
    display = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!display) {
        spdlog::error("Failed to create LVGL SDL display");
        lv_deinit(); // Clean up partial LVGL state
        return false;
    }

    // Create mouse input device
    indev_mouse = lv_sdl_mouse_create();
    if (!indev_mouse) {
        spdlog::error("Failed to create LVGL SDL mouse input");
        lv_deinit(); // Clean up partial LVGL state
        return false;
    }

    // Create keyboard input device (optional - enables physical keyboard input)
    lv_indev_t* indev_keyboard = lv_sdl_keyboard_create();
    if (indev_keyboard) {
        spdlog::info("Physical keyboard input enabled");

        // Create input group for keyboard navigation and text input
        lv_group_t* input_group = lv_group_create();
        lv_group_set_default(input_group);
        lv_indev_set_group(indev_keyboard, input_group);
        spdlog::debug("Created default input group for keyboard");
    }

    spdlog::info("LVGL initialized: {}x{}", SCREEN_WIDTH, SCREEN_HEIGHT);

    // Initialize SVG decoder for loading .svg files
    lv_svg_decoder_init();

    return true;
}

// Show splash screen with HelixScreen logo
static void show_splash_screen() {
    spdlog::info("Showing splash screen");

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Apply theme background color (app_bg_color runtime constant set by ui_theme_init)
    ui_theme_apply_bg_color(screen, "app_bg_color", LV_PART_MAIN);

    // Disable scrollbars on screen
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Create centered container for logo (disable scrolling)
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);         // Disable scrollbars
    lv_obj_set_style_opa(container, LV_OPA_TRANSP, LV_PART_MAIN); // Start invisible for fade-in
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
        lv_coord_t target_size = (SCREEN_WIDTH * 3) / 5; // 60% of screen width
        if (SCREEN_HEIGHT < 500) {                       // Tiny screen
            target_size = SCREEN_WIDTH / 2;              // 50% on tiny screens
        }

        // Calculate scale: (target_size * 256) / actual_width
        // LVGL uses 1/256 scale units (256 = 100%, 128 = 50%, etc.)
        uint32_t width = header.w;  // Copy bit-field to local var for logging
        uint32_t height = header.h; // Copy bit-field to local var for logging
        int scale = (target_size * 256) / width;
        lv_image_set_scale(logo, scale);

        spdlog::debug("Logo: {}x{} scaled to {} (scale factor: {})", width, height, target_size,
                      scale);
    } else {
        spdlog::warn("Could not get logo dimensions, using default scale");
        lv_image_set_scale(logo, 128); // 50% scale as fallback
    }

    // Create fade-in animation (0.5 seconds)
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, container);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&anim, 500); // 500ms = 0.5 seconds
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&anim, [](void* obj, int32_t value) {
        lv_obj_set_style_opa((lv_obj_t*)obj, value, LV_PART_MAIN);
    });
    lv_anim_start(&anim);

    // Run LVGL timer to process fade-in animation and keep splash visible
    // Total display time: 2 seconds (including 0.5s fade-in)
    uint32_t splash_start = SDL_GetTicks();
    uint32_t splash_duration = 2000; // 2 seconds total

    while (SDL_GetTicks() - splash_start < splash_duration) {
        lv_timer_handler(); // Process animations and rendering
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
    if (!f)
        return false;

    // BMP header (54 bytes total)
    uint32_t file_size = 54 + (width * height * 4);
    uint32_t pixel_offset = 54;
    uint32_t dib_size = 40;
    uint16_t planes = 1;
    uint16_t bpp = 32;
    uint32_t reserved = 0;
    uint32_t compression = 0;
    uint32_t ppm = 2835; // pixels per meter
    uint32_t colors = 0;

    // BMP file header (14 bytes)
    fputc('B', f.get());
    fputc('M', f.get());                  // Signature
    fwrite(&file_size, 4, 1, f.get());    // File size
    fwrite(&reserved, 4, 1, f.get());     // Reserved
    fwrite(&pixel_offset, 4, 1, f.get()); // Pixel data offset

    // DIB header (40 bytes)
    fwrite(&dib_size, 4, 1, f.get());    // DIB header size
    fwrite(&width, 4, 1, f.get());       // Width
    fwrite(&height, 4, 1, f.get());      // Height
    fwrite(&planes, 2, 1, f.get());      // Planes
    fwrite(&bpp, 2, 1, f.get());         // Bits per pixel
    fwrite(&compression, 4, 1, f.get()); // Compression (none)
    uint32_t image_size = width * height * 4;
    fwrite(&image_size, 4, 1, f.get()); // Image size
    fwrite(&ppm, 4, 1, f.get());        // X pixels per meter
    fwrite(&ppm, 4, 1, f.get());        // Y pixels per meter
    fwrite(&colors, 4, 1, f.get());     // Colors in palette
    fwrite(&colors, 4, 1, f.get());     // Important colors

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
    snprintf(filename, sizeof(filename), "/tmp/ui-screenshot-%lu.bmp", (unsigned long)time(NULL));

    // Take snapshot using LVGL's native API (platform-independent)
    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* snapshot = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);

    if (!snapshot) {
        spdlog::error("Failed to take screenshot");
        return;
    }

    // Write BMP file
    if (write_bmp(filename, snapshot->data, snapshot->header.w, snapshot->header.h)) {
        spdlog::info("Screenshot saved: {}", filename);
    } else {
        spdlog::error("Failed to save screenshot");
    }

    // Free snapshot buffer
    lv_draw_buf_destroy(snapshot);
}

// Initialize Moonraker client and API instances
static void initialize_moonraker_client(Config* config) {
    spdlog::info("Initializing Moonraker client...");

    // Create client instance (mock or real based on test mode)
    if (get_runtime_config().should_mock_moonraker()) {
        spdlog::info("[Test Mode] Creating MOCK Moonraker client (Voron 2.4 profile)");
        moonraker_client = new MoonrakerClientMock(MoonrakerClientMock::PrinterType::VORON_24);
    } else {
        spdlog::info("Creating REAL Moonraker client");
        moonraker_client = new MoonrakerClient();
    }

    // Configure timeouts from config file
    uint32_t connection_timeout =
        config->get<int>(config->df() + "moonraker_connection_timeout_ms", 10000);
    uint32_t request_timeout =
        config->get<int>(config->df() + "moonraker_request_timeout_ms", 30000);
    uint32_t keepalive_interval =
        config->get<int>(config->df() + "moonraker_keepalive_interval_ms", 10000);
    uint32_t reconnect_min_delay =
        config->get<int>(config->df() + "moonraker_reconnect_min_delay_ms", 200);
    uint32_t reconnect_max_delay =
        config->get<int>(config->df() + "moonraker_reconnect_max_delay_ms", 2000);

    moonraker_client->configure_timeouts(connection_timeout, request_timeout, keepalive_interval,
                                         reconnect_min_delay, reconnect_max_delay);

    spdlog::debug("Moonraker timeouts configured: connection={}ms, request={}ms, keepalive={}ms",
                  connection_timeout, request_timeout, keepalive_interval);

    // Set up state change callback to automatically update PrinterState
    moonraker_client->set_state_change_callback(
        [](ConnectionState old_state, ConnectionState new_state) {
            const char* messages[] = {
                "Disconnected",     // DISCONNECTED
                "Connecting...",    // CONNECTING
                "Connected",        // CONNECTED
                "Reconnecting...",  // RECONNECTING
                "Connection Failed" // FAILED
            };

            // Convert enum to integer for subject (0-4)
            int state_int = static_cast<int>(new_state);
            printer_state.set_connection_state(state_int, messages[state_int]);

            // Log state transitions
            spdlog::debug("Connection state changed: {} -> {}",
                          messages[static_cast<int>(old_state)], messages[state_int]);
        });

    // Register notification callback to queue updates for main thread
    // CRITICAL: Moonraker callbacks run on background thread, but LVGL is NOT thread-safe
    // Queue notifications here, process on main thread in event loop
    moonraker_client->register_notify_update([](json notification) {
        std::lock_guard<std::mutex> lock(notification_mutex);
        notification_queue.push(notification);
    });

    // Create MoonrakerAPI instance
    spdlog::info("Creating MoonrakerAPI instance...");
    moonraker_api = new MoonrakerAPI(*moonraker_client, printer_state);

    spdlog::info("Moonraker client initialized (not connected yet)");
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
    int initial_panel = -1;          // -1 means auto-select based on screen size
    bool show_motion = false;        // Special flag for motion sub-screen
    bool show_nozzle_temp = false;   // Special flag for nozzle temp sub-screen
    bool show_bed_temp = false;      // Special flag for bed temp sub-screen
    bool show_extrusion = false;     // Special flag for extrusion sub-screen
    bool show_print_status = false;  // Special flag for print status screen
    bool show_file_detail = false;   // Special flag for file detail view
    bool show_keypad = false;        // Special flag for keypad testing
    bool show_keyboard = false;      // Special flag for keyboard testing
    bool show_step_test = false;     // Special flag for step progress widget testing
    bool show_test_panel = false;    // Special flag for test/development panel
    bool show_gcode_test = false;    // Special flag for G-code 3D viewer testing
    bool show_bed_mesh = false;      // Special flag for bed mesh overlay panel
    bool show_glyphs = false;        // Special flag for LVGL glyphs reference panel
    bool force_wizard = false;       // Force wizard to run even if config exists
    int wizard_step = -1;            // Specific wizard step to show (-1 means normal flow)
    bool panel_requested = false;    // Track if user explicitly requested a panel via CLI
    int display_num = -1;            // Display number for window placement (-1 means unset)
    int x_pos = -1;                  // X position for window placement (-1 means unset)
    int y_pos = -1;                  // Y position for window placement (-1 means unset)
    bool screenshot_enabled = false; // Enable automatic screenshot
    int screenshot_delay_sec = 2;    // Screenshot delay in seconds (default: 2)
    int timeout_sec = 0;             // Auto-quit timeout in seconds (0 = disabled)
    int verbosity = 0;               // Verbosity level (0=warn, 1=info, 2=debug, 3=trace)
    bool dark_mode = true; // Theme mode (true=dark, false=light, default until loaded from config)
    bool theme_requested = false; // Track if user explicitly set theme via CLI
    int dpi = -1;                 // Display DPI (-1 means use LV_DPI_DEF from lv_conf.h)

    // Parse command-line arguments (returns false for help/error)
    if (!parse_command_line_args(
            argc, argv, initial_panel, show_motion, show_nozzle_temp, show_bed_temp, show_extrusion,
            show_print_status, show_file_detail, show_keypad, show_keyboard, show_step_test,
            show_test_panel, show_gcode_test, show_bed_mesh, show_glyphs, force_wizard, wizard_step,
            panel_requested, display_num, x_pos, y_pos, screenshot_enabled, screenshot_delay_sec,
            timeout_sec, verbosity, dark_mode, theme_requested, dpi)) {
        return 0; // Help shown or parse error
    }

    // Set spdlog log level based on verbosity flags
    switch (verbosity) {
    case 0:
        spdlog::set_level(spdlog::level::warn); // Default: warnings and errors only
        break;
    case 1:
        spdlog::set_level(spdlog::level::info); // -v: general information
        break;
    case 2:
        spdlog::set_level(spdlog::level::debug); // -vv: debug information
        break;
    default:                                     // 3 or more
        spdlog::set_level(spdlog::level::trace); // -vvv: trace everything
        break;
    }

    spdlog::info("HelixScreen UI Prototype");
    spdlog::info("========================");
    spdlog::info("Target: {}x{}", SCREEN_WIDTH, SCREEN_HEIGHT);
    spdlog::info("DPI: {}{}", (dpi > 0 ? dpi : LV_DPI_DEF), (dpi > 0 ? " (custom)" : " (default)"));
    spdlog::info("Nav Width: {} pixels", UI_NAV_WIDTH(SCREEN_WIDTH));
    spdlog::info("Initial Panel: {}", initial_panel);

    // Initialize config system
    Config* config = Config::get_instance();
    config->init("helixconfig.json");

    // Load theme preference from config if not set by command-line
    if (!theme_requested) {
        dark_mode = config->get<bool>("/dark_mode", true); // Default to dark if not in config
        spdlog::debug("Loaded theme preference from config: {}", dark_mode ? "dark" : "light");
    }

    // Set window position environment variables for LVGL SDL driver
    if (display_num >= 0) {
        char display_str[32];
        snprintf(display_str, sizeof(display_str), "%d", display_num);
        if (setenv("HELIX_SDL_DISPLAY", display_str, 1) != 0) {
            spdlog::error("Failed to set HELIX_SDL_DISPLAY environment variable");
            return 1;
        }
        spdlog::info("Window will be centered on display {}", display_num);
    }
    if (x_pos >= 0 && y_pos >= 0) {
        char x_str[32], y_str[32];
        snprintf(x_str, sizeof(x_str), "%d", x_pos);
        snprintf(y_str, sizeof(y_str), "%d", y_pos);
        if (setenv("HELIX_SDL_XPOS", x_str, 1) != 0 || setenv("HELIX_SDL_YPOS", y_str, 1) != 0) {
            spdlog::error("Failed to set window position environment variables");
            return 1;
        }
        spdlog::info("Window will be positioned at ({}, {})", x_pos, y_pos);
    } else if ((x_pos >= 0 && y_pos < 0) || (x_pos < 0 && y_pos >= 0)) {
        spdlog::warn("Both -x and -y must be specified for exact positioning. Ignoring.");
    }

    // Initialize LVGL (handles SDL internally)
    if (!init_lvgl()) {
        return 1;
    }

    // Apply custom DPI if specified (before theme init)
    if (dpi > 0) {
        lv_display_set_dpi(display, dpi);
        spdlog::info("Display DPI set to: {}", dpi);
    } else {
        spdlog::info("Display DPI: {} (from LV_DPI_DEF)", lv_display_get_dpi(display));
    }

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
        spdlog::info("Loaded {} tips (version: {})", tips_mgr->get_total_tips(),
                     tips_mgr->get_version());
    }

    // Register fonts and images for XML (must be done BEFORE globals.xml for theme init)
    register_fonts_and_images();

    // Register XML components (globals first to make constants available)
    spdlog::debug("Registering XML components...");
    lv_xml_register_component_from_file("A:ui_xml/globals.xml");

    // Initialize LVGL theme from globals.xml constants (after fonts and globals are registered)
    ui_theme_init(display,
                  dark_mode); // dark_mode from command-line args (--dark/--light) or config

    // Save theme preference to config for next launch
    config->set<bool>("/dark_mode", dark_mode);
    config->save();

    // Apply theme background color to screen
    ui_theme_apply_bg_color(screen, "app_bg_color", LV_PART_MAIN);

    // Show splash screen AFTER theme init (skip if requested via --skip-splash or --test)
    // Theme must be initialized first so app_bg_color runtime constant is available
    if (!g_runtime_config.should_skip_splash()) {
        show_splash_screen();
    }

    // Register Material Design icons (64x64, scalable)
    material_icons_register();

    // Register custom widgets (must be before XML component registration)
    ui_icon_register_widget();
    ui_switch_register();
    ui_card_register();
    ui_bed_mesh_register();
    ui_gcode_viewer_register();

    // Initialize component systems (BEFORE XML registration)
    ui_component_header_bar_init();

    // WORKAROUND: Add small delay to stabilize SDL/LVGL initialization
    // Prevents race condition between SDL2 and LVGL 9 XML component registration
    SDL_Delay(100);

    // Register remaining XML components (globals already registered for theme init)
    register_xml_components();

    // Initialize reactive subjects BEFORE creating XML
    initialize_subjects();

    // Create entire UI from XML (single component contains everything)
    lv_obj_t* app_layout = (lv_obj_t*)lv_xml_create(screen, "app_layout", NULL);

    // Force layout calculation for all LV_SIZE_CONTENT widgets
    lv_obj_update_layout(screen);

    // Register app_layout with navigation system (to prevent hiding it)
    ui_nav_set_app_layout(app_layout);

    // Initialize shared overlay backdrop
    ui_nav_init_overlay_backdrop(screen);

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
            spdlog::error("Missing panel {} in content_area - expected {} panels", i,
                          (int)UI_PANEL_COUNT);
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

    // Setup settings panel (wire launcher card click handlers)
    ui_panel_settings_set(panels[UI_PANEL_SETTINGS]);
    ui_panel_settings_wire_events(panels[UI_PANEL_SETTINGS], screen);

    // Initialize numeric keypad modal component (creates reusable keypad widget)
    ui_keypad_init(screen);

    // Create print status panel (overlay for active prints)
    overlay_panels.print_status = (lv_obj_t*)lv_xml_create(screen, "print_status_panel", nullptr);
    if (overlay_panels.print_status) {
        ui_panel_print_status_setup(overlay_panels.print_status, screen);
        lv_obj_add_flag(overlay_panels.print_status, LV_OBJ_FLAG_HIDDEN); // Hidden by default

        // Wire print status panel to print select (for launching prints)
        ui_panel_print_select_set_print_status_panel(overlay_panels.print_status);

        spdlog::debug("Print status panel created and wired to print select");
    } else {
        spdlog::error("Failed to create print status panel");
    }

    spdlog::info("XML UI created successfully with reactive navigation");

    // Initialize Moonraker client EARLY (before wizard, so it's available for connection test)
    // But don't connect yet - just create the instances
    initialize_moonraker_client(config);

    // Initialize global keyboard BEFORE wizard (required for textarea registration)
    // NOTE: Keyboard is created early but will appear on top due to being moved to top layer below
    ui_keyboard_init(screen);

    // Check if first-run wizard is required (skip for special test panels and explicit panel
    // requests)
    bool wizard_active = false;
    if ((force_wizard || config->is_wizard_required()) && !show_step_test && !show_test_panel &&
        !show_keypad && !show_keyboard && !show_gcode_test && !panel_requested) {
        spdlog::info("Starting first-run configuration wizard");

        // Register wizard event callbacks and responsive constants BEFORE creating
        ui_wizard_register_event_callbacks();
        ui_wizard_container_register_responsive_constants();

        lv_obj_t* wizard = ui_wizard_create(screen);

        if (wizard) {
            spdlog::debug("Wizard created successfully");
            wizard_active = true;

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

    // Navigate to initial panel (if not showing wizard and panel was requested)
    if (!wizard_active && initial_panel >= 0) {
        spdlog::info("Navigating to initial panel: {}", initial_panel);
        ui_nav_set_active(static_cast<ui_panel_id_t>(initial_panel));
    }

    // Show requested overlay panels (motion, temp controls, etc.)
    if (!wizard_active) {
        if (show_motion) {
            spdlog::debug("Opening motion overlay as requested by command-line flag");
            overlay_panels.motion = (lv_obj_t*)lv_xml_create(screen, "motion_panel", nullptr);
            if (overlay_panels.motion) {
                ui_panel_motion_setup(overlay_panels.motion, screen);
                ui_nav_push_overlay(overlay_panels.motion);
            }
        }
        if (show_nozzle_temp) {
            spdlog::debug("Opening nozzle temp overlay as requested by command-line flag");
            overlay_panels.nozzle_temp =
                (lv_obj_t*)lv_xml_create(screen, "nozzle_temp_panel", nullptr);
            if (overlay_panels.nozzle_temp) {
                ui_panel_controls_temp_nozzle_setup(overlay_panels.nozzle_temp, screen);
                ui_nav_push_overlay(overlay_panels.nozzle_temp);
            }
        }
        if (show_bed_temp) {
            spdlog::debug("Opening bed temp overlay as requested by command-line flag");
            overlay_panels.bed_temp = (lv_obj_t*)lv_xml_create(screen, "bed_temp_panel", nullptr);
            if (overlay_panels.bed_temp) {
                ui_panel_controls_temp_bed_setup(overlay_panels.bed_temp, screen);
                ui_nav_push_overlay(overlay_panels.bed_temp);
            }
        }
        if (show_extrusion) {
            spdlog::debug("Opening extrusion overlay as requested by command-line flag");
            overlay_panels.extrusion = (lv_obj_t*)lv_xml_create(screen, "extrusion_panel", nullptr);
            if (overlay_panels.extrusion) {
                ui_panel_controls_extrusion_setup(overlay_panels.extrusion, screen);
                ui_nav_push_overlay(overlay_panels.extrusion);
            }
        }
        if (show_print_status && overlay_panels.print_status) {
            spdlog::debug("Opening print status overlay as requested by command-line flag");
            ui_nav_push_overlay(overlay_panels.print_status);
        }
        if (show_bed_mesh) {
            spdlog::debug("Opening bed mesh overlay as requested by command-line flag");
            lv_obj_t* bed_mesh = (lv_obj_t*)lv_xml_create(screen, "bed_mesh_panel", nullptr);
            if (bed_mesh) {
                spdlog::info("Bed mesh overlay created successfully, calling setup");
                ui_panel_bed_mesh_setup(bed_mesh, screen);
                ui_nav_push_overlay(bed_mesh);
                spdlog::info("Bed mesh overlay pushed to nav stack");
            } else {
                spdlog::error("Failed to create bed mesh overlay from XML component 'bed_mesh_panel'");
            }
        }
        if (show_keypad) {
            spdlog::debug("Opening keypad modal as requested by command-line flag");
            ui_keypad_config_t keypad_config = {.initial_value = 0.0f,
                                                .min_value = 0.0f,
                                                .max_value = 300.0f,
                                                .title_label = "Test Keypad",
                                                .unit_label = "°C",
                                                .allow_decimal = true,
                                                .allow_negative = false,
                                                .callback = nullptr,
                                                .user_data = nullptr};
            ui_keypad_show(&keypad_config);
        }
        if (show_keyboard) {
            spdlog::debug("Showing keyboard as requested by command-line flag");
            ui_keyboard_show(nullptr);
        }
        if (show_step_test) {
            spdlog::debug("Creating step progress test widget as requested by command-line flag");
            lv_obj_t* step_test = (lv_obj_t*)lv_xml_create(screen, "step_progress_test", nullptr);
            if (step_test) {
                ui_panel_step_test_setup(step_test);
            }
        }
        if (show_test_panel) {
            spdlog::debug("Opening test panel as requested by command-line flag");
            lv_obj_t* test_panel_obj = (lv_obj_t*)lv_xml_create(screen, "test_panel", nullptr);
            if (test_panel_obj) {
                ui_panel_test_setup(test_panel_obj);
            }
        }
        if (show_file_detail) {
            spdlog::debug("File detail view requested - navigating to print select panel first");
            ui_nav_set_active(UI_PANEL_PRINT_SELECT);
            // Note: File detail requires selecting a specific file, which can't be automated via
            // CLI flag
        }
    }

    // Create G-code test panel if requested (independent of wizard state)
    if (show_gcode_test) {
        spdlog::info("Creating G-code test panel");
        lv_obj_t* gcode_test = ui_panel_gcode_test_create(screen);
        if (gcode_test) {
            spdlog::debug("G-code test panel created successfully");
        } else {
            spdlog::error("Failed to create G-code test panel");
        }
    }

    // Create glyphs panel if requested (independent of wizard state)
    if (show_glyphs) {
        spdlog::info("Creating glyphs reference panel");
        lv_obj_t* glyphs_panel = ui_panel_glyphs_create(screen);
        if (glyphs_panel) {
            spdlog::debug("Glyphs panel created successfully");
        } else {
            spdlog::error("Failed to create glyphs panel");
        }
    }

    // Connect to Moonraker (only if not in wizard and we have saved config)
    // Wizard will handle its own connection test
    std::string saved_host = config->get<std::string>(config->df() + "moonraker_host", "");
    if (!force_wizard && !config->is_wizard_required() && !saved_host.empty()) {
        // Build WebSocket URL from config
        std::string moonraker_url =
            "ws://" + config->get<std::string>(config->df() + "moonraker_host") + ":" +
            std::to_string(config->get<int>(config->df() + "moonraker_port")) + "/websocket";

        // Connect to Moonraker
        spdlog::info("Connecting to Moonraker at {}", moonraker_url);
        int connect_result = moonraker_client->connect(
            moonraker_url.c_str(),
            []() {
                spdlog::info("✓ Connected to Moonraker");
                // State change callback will handle updating PrinterState

                // Start auto-discovery (must be called AFTER connection is established)
                moonraker_client->discover_printer(
                    []() { spdlog::info("✓ Printer auto-discovery complete"); });
            },
            []() {
                spdlog::warn("✗ Disconnected from Moonraker");
                // State change callback will handle updating PrinterState
            });

        if (connect_result != 0) {
            spdlog::error("Failed to initiate Moonraker connection (code {})", connect_result);
            // State change callback will handle updating PrinterState
        }
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

    // Request timeout check timer (check every 2 seconds)
    uint32_t last_timeout_check = SDL_GetTicks();
    uint32_t timeout_check_interval =
        config->get<int>(config->df() + "moonraker_timeout_check_interval_ms", 2000);

    // Main event loop - Let LVGL handle SDL events internally via lv_timer_handler()
    // Loop continues while display exists (exits when window closed)
    while (lv_display_get_next(NULL)) {
        // Check for Cmd+Q (macOS) or Win+Q (Windows) to quit
        SDL_Keymod modifiers = SDL_GetModState();
        const Uint8* keyboard_state = SDL_GetKeyboardState(NULL);
        if ((modifiers & KMOD_GUI) && keyboard_state[SDL_SCANCODE_Q]) {
            spdlog::info("Cmd+Q/Win+Q pressed - exiting...");
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

        // Check for request timeouts (using configured interval)
        if (current_time - last_timeout_check >= timeout_check_interval) {
            moonraker_client->process_timeouts();
            last_timeout_check = current_time;
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
        SDL_Delay(5); // Small delay to prevent 100% CPU usage
    }

    // Cleanup
    spdlog::info("Shutting down...");

    // Clean up Moonraker instances
    delete moonraker_api;
    moonraker_api = nullptr;
    delete moonraker_client;
    moonraker_client = nullptr;

    lv_deinit(); // LVGL handles SDL cleanup internally

    return 0;
}
