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
#include "ui_dialog.h"
#include "ui_emergency_stop.h"
#include "ui_error_reporting.h"
#include "ui_fatal_error.h"
#include "ui_fonts.h"
#include "ui_gcode_viewer.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_icon_loader.h"
#include "ui_keyboard.h"
#include "ui_nav.h"
#include "ui_notification.h"
#include "ui_panel_advanced.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_controls.h"
#include "ui_panel_extrusion.h"
#include "ui_panel_fan.h"
#include "ui_panel_filament.h"
#include "ui_panel_gcode_test.h"
#include "ui_panel_glyphs.h"
#include "ui_panel_home.h"
#include "ui_panel_motion.h"
#include "ui_panel_notification_history.h"
#include "ui_panel_print_select.h"
#include "ui_panel_print_status.h"
#include "ui_panel_settings.h"
#include "ui_panel_step_test.h"
#include "ui_panel_temp_control.h"
#include "ui_panel_test.h"
#include "ui_severity_card.h"
#include "ui_status_bar.h"
#include "ui_switch.h"
#include "ui_text.h"
#include "ui_text_input.h"
#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_utils.h"
#include "ui_wizard.h"
#include "ui_wizard_wifi.h"

#include "app_globals.h"
#include "config.h"
#include "display_backend.h"
#include "gcode_file_modifier.h"
#include "logging_init.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/libs/svg/lv_svg_decoder.h"
#include "lvgl/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "settings_manager.h"
#include "tips_manager.h"
#include "usb_backend_mock.h"
#include "usb_manager.h"

#include <spdlog/spdlog.h>

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <queue>
#include <signal.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// Portable timing functions (SDL-independent for embedded builds)
#ifdef HELIX_DISPLAY_SDL
// Use SDL timing when available (more precise on desktop)
inline uint32_t helix_get_ticks() {
    return SDL_GetTicks();
}
inline void helix_delay(uint32_t ms) {
    SDL_Delay(ms);
}
#else
// POSIX fallback for embedded Linux
#include <time.h>
inline uint32_t helix_get_ticks() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
inline void helix_delay(uint32_t ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}
#endif

// Forward declarations for panel global accessor functions
class HomePanel;
class ControlsPanel;
class MotionPanel;
class SettingsPanel;
class FilamentPanel;
class AdvancedPanel;
class PrintSelectPanel;
class PrintStatusPanel;
class ExtrusionPanel;
class BedMeshPanel;
class StepTestPanel;
class TestPanel;
class GlyphsPanel;
class GcodeTestPanel;

HomePanel& get_global_home_panel();
ControlsPanel& get_global_controls_panel();
MotionPanel& get_global_motion_panel();
SettingsPanel& get_global_settings_panel();
FilamentPanel& get_global_filament_panel();
AdvancedPanel& get_global_advanced_panel();
void init_global_advanced_panel(PrinterState& printer_state, MoonrakerAPI* api);
PrintSelectPanel* get_print_select_panel(PrinterState& printer_state, MoonrakerAPI* api);
PrintStatusPanel& get_global_print_status_panel();
ExtrusionPanel& get_global_extrusion_panel();
BedMeshPanel& get_global_bed_mesh_panel();
StepTestPanel& get_global_step_test_panel();
TestPanel& get_global_test_panel();
GlyphsPanel& get_global_glyphs_panel();
GcodeTestPanel* get_gcode_test_panel(PrinterState& printer_state, MoonrakerAPI* api);

// Ensure we're running from the project root directory.
// If the executable is in build/bin/, change to the project root so relative paths work.
static void ensure_project_root_cwd() {
    char exe_path[PATH_MAX];

#ifdef __APPLE__
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        return; // Failed to get path, assume CWD is correct
    }
    // Resolve symlinks
    char resolved[PATH_MAX];
    if (realpath(exe_path, resolved)) {
        strncpy(exe_path, resolved, PATH_MAX - 1);
        exe_path[PATH_MAX - 1] = '\0';
    }
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        return; // Failed to get path, assume CWD is correct
    }
    exe_path[len] = '\0';
#else
    return; // Unsupported platform, assume CWD is correct
#endif

    // Get directory containing executable
    char* last_slash = strrchr(exe_path, '/');
    if (!last_slash)
        return;
    *last_slash = '\0';

    // Check if we're in build/bin/ and go up two levels
    size_t dir_len = strlen(exe_path);
    const char* suffix = "/build/bin";
    size_t suffix_len = strlen(suffix);

    if (dir_len >= suffix_len && strcmp(exe_path + dir_len - suffix_len, suffix) == 0) {
        // Strip /build/bin to get project root
        exe_path[dir_len - suffix_len] = '\0';

        if (chdir(exe_path) == 0) {
            spdlog::debug("Changed working directory to: {}", exe_path);
        }
    }
}

// Display backend and LVGL display/input
static std::unique_ptr<DisplayBackend> g_display_backend;
static lv_display_t* display = nullptr;
static lv_indev_t* indev_mouse = nullptr;

// Screen dimensions (configurable via command line, default to small = 800x480)
static int SCREEN_WIDTH = UI_SCREEN_SMALL_W;
static int SCREEN_HEIGHT = UI_SCREEN_SMALL_H;

// Local instances (registered with app_globals via setters)
// Note: PrinterState is now a singleton accessed via get_printer_state()
// Using unique_ptr for RAII - raw pointers are passed to app_globals for access
static std::unique_ptr<MoonrakerClient> moonraker_client;
static std::unique_ptr<MoonrakerAPI> moonraker_api;
static std::unique_ptr<TempControlPanel> temp_control_panel;
static std::unique_ptr<UsbManager> usb_manager;

// Panels that need MoonrakerAPI - stored as pointers for deferred set_api() call
static PrintSelectPanel* print_select_panel = nullptr;
static PrintStatusPanel* print_status_panel = nullptr;
static MotionPanel* motion_panel = nullptr;
static ExtrusionPanel* extrusion_panel = nullptr;
static BedMeshPanel* bed_mesh_panel = nullptr;

// Runtime configuration
static RuntimeConfig g_runtime_config;

// Logging configuration (parsed before Config system is available)
static std::string g_log_dest_cli; // CLI override for log destination
static std::string g_log_file_cli; // CLI override for log file path

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
    bool& show_bed_temp, bool& show_extrusion, bool& show_fan, bool& show_print_status,
    bool& show_file_detail, bool& show_keypad, bool& show_keyboard, bool& show_step_test,
    bool& show_test_panel, bool& show_gcode_test, bool& show_bed_mesh, bool& show_zoffset,
    bool& show_pid, bool& show_glyphs, bool& show_gradient_test, bool& force_wizard,
    int& wizard_step, bool& panel_requested, int& display_num, int& x_pos, int& y_pos,
    bool& screenshot_enabled, int& screenshot_delay_sec, int& timeout_sec, int& verbosity,
    int& dark_mode_cli, int& dpi) {
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
                } else if (strcmp(panel_arg, "fan") == 0) {
                    initial_panel = UI_PANEL_CONTROLS;
                    show_fan = true;
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
                } else if (strcmp(panel_arg, "bed-mesh") == 0 ||
                           strcmp(panel_arg, "bed_mesh") == 0) {
                    show_bed_mesh = true;
                } else if (strcmp(panel_arg, "zoffset") == 0 ||
                           strcmp(panel_arg, "z-offset") == 0) {
                    show_zoffset = true;
                } else if (strcmp(panel_arg, "pid") == 0) {
                    show_pid = true;
                } else if (strcmp(panel_arg, "glyphs") == 0) {
                    show_glyphs = true;
                } else if (strcmp(panel_arg, "gradient-test") == 0) {
                    show_gradient_test = true;
                } else {
                    printf("Unknown panel: %s\n", panel_arg);
                    printf("Available panels: home, controls, motion, nozzle-temp, bed-temp, "
                           "bed-mesh, zoffset, pid, extrusion, fan, print-status, filament, "
                           "settings, advanced, "
                           "print-select, step-test, test, gcode-test, glyphs, gradient-test\n");
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
            dark_mode_cli = 1;
        } else if (strcmp(argv[i], "--light") == 0) {
            dark_mode_cli = 0;
        } else if (strcmp(argv[i], "--test") == 0) {
            g_runtime_config.test_mode = true;
        } else if (strcmp(argv[i], "--skip-splash") == 0) {
            g_runtime_config.skip_splash = true;
        } else if (strncmp(argv[i], "--splash-pid=", 13) == 0) {
            // External splash process PID - will send SIGUSR1 when display ready
            g_runtime_config.splash_pid = static_cast<pid_t>(atoi(argv[i] + 13));
            spdlog::info("Splash PID received from launcher: {}", g_runtime_config.splash_pid);
        } else if (strcmp(argv[i], "--real-wifi") == 0) {
            g_runtime_config.use_real_wifi = true;
        } else if (strcmp(argv[i], "--real-ethernet") == 0) {
            g_runtime_config.use_real_ethernet = true;
        } else if (strcmp(argv[i], "--real-moonraker") == 0) {
            g_runtime_config.use_real_moonraker = true;
        } else if (strcmp(argv[i], "--real-files") == 0) {
            g_runtime_config.use_real_files = true;
        } else if (strcmp(argv[i], "--select-file") == 0) {
            if (i + 1 < argc) {
                g_runtime_config.select_file = argv[++i];
            } else {
                printf("Error: --select-file requires a filename argument\n");
                return false;
            }
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
                    printf("Format: --camera \"az:90.5,el:4.0,zoom:15.5\" (each parameter "
                           "optional)\n");
                    return false;
                }

                // Parse comma-separated "az:90.5,el:4.0,zoom:15.5" format
                // Each component is optional
                // Use RAII wrapper to ensure cleanup on all paths
                std::unique_ptr<char, decltype(&free)> str_copy(strdup(camera_str), free);
                char* token = strtok(str_copy.get(), ",");

                while (token != nullptr) {
                    // Trim leading whitespace
                    while (*token == ' ')
                        token++;

                    // Parse key:value pairs
                    if (strncmp(token, "az:", 3) == 0) {
                        char* endptr;
                        double val = strtod(token + 3, &endptr);
                        if (*endptr == '\0' || *endptr == ' ') {
                            g_runtime_config.gcode_camera_azimuth = (float)val;
                            g_runtime_config.gcode_camera_azimuth_set = true;
                        } else {
                            printf("Error: Invalid azimuth value in --camera: %s\n", token);
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
                            return false;
                        }
                    } else if (strncmp(token, "zoom:", 5) == 0) {
                        char* endptr;
                        double val = strtod(token + 5, &endptr);
                        if ((*endptr == '\0' || *endptr == ' ') && val > 0) {
                            g_runtime_config.gcode_camera_zoom = (float)val;
                            g_runtime_config.gcode_camera_zoom_set = true;
                        } else {
                            printf("Error: Invalid zoom value in --camera (must be positive): %s\n",
                                   token);
                            return false;
                        }
                    } else {
                        printf("Error: Unknown camera parameter in --camera: %s\n", token);
                        printf("Valid parameters: az:<degrees>, el:<degrees>, zoom:<factor>\n");
                        return false;
                    }

                    token = strtok(nullptr, ",");
                }
                // str_copy automatically freed by unique_ptr destructor
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
        } else if (strcmp(argv[i], "--log-dest") == 0 || strncmp(argv[i], "--log-dest=", 11) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--log-dest=", 11) == 0) {
                value = argv[i] + 11;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --log-dest requires an argument\n");
                return false;
            }
            g_log_dest_cli = value;
            // Validate the value
            if (g_log_dest_cli != "auto" && g_log_dest_cli != "journal" &&
                g_log_dest_cli != "syslog" && g_log_dest_cli != "file" &&
                g_log_dest_cli != "console") {
                printf("Error: invalid --log-dest value: %s\n", g_log_dest_cli.c_str());
                printf("Valid values: auto, journal, syslog, file, console\n");
                return false;
            }
        } else if (strcmp(argv[i], "--log-file") == 0 || strncmp(argv[i], "--log-file=", 11) == 0) {
            if (strncmp(argv[i], "--log-file=", 11) == 0) {
                g_log_file_cli = argv[i] + 11;
            } else if (i + 1 < argc) {
                g_log_file_cli = argv[++i];
            } else {
                printf("Error: --log-file requires a path argument\n");
                return false;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  -s, --size <size>    Screen size: tiny, small, medium, large (default: "
                   "small)\n");
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
            printf(
                "  --log-dest <dest>    Log destination: auto, journal, syslog, file, console\n");
            printf("  --log-file <path>    Log file path (when --log-dest=file)\n");
            printf("  -h, --help           Show this help message\n");
            printf("\nTest Mode Options:\n");
            printf("  --test               Enable test mode (uses all mocks by default)\n");
            printf("    --real-wifi        Use real WiFi hardware (requires --test)\n");
            printf("    --real-ethernet    Use real Ethernet hardware (requires --test)\n");
            printf("    --real-moonraker   Connect to real printer (requires --test)\n");
            printf("    --real-files       Use real files from printer (requires --test)\n");
            printf("    --select-file <name>  Auto-select file in print-select panel and show "
                   "detail view\n");
            printf("\nG-code Viewer Options (require --test):\n");
            printf("  --gcode-file <path>  Load specific G-code file in gcode-test panel\n");
            printf("  --camera <params>    Set camera params: \"az:90.5,el:4.0,zoom:15.5\"\n");
            printf("                       (each parameter optional, comma-separated)\n");
            printf("  --gcode-az <deg>     Set camera azimuth angle (degrees)\n");
            printf("  --gcode-el <deg>     Set camera elevation angle (degrees)\n");
            printf("  --gcode-zoom <n>     Set camera zoom level (positive number)\n");
            printf("  --gcode-debug-colors Enable per-face debug coloring\n");
            printf("\nAvailable panels:\n");
            printf("  home, controls, motion, nozzle-temp, bed-temp, bed-mesh,\n");
            printf("  zoffset, pid, extrusion, print-status, filament, settings, advanced,\n");
            printf("  print-select, step-test, test, gcode-test, glyphs\n");
            printf("\nScreen sizes:\n");
            printf("  tiny   = %dx%d\n", UI_SCREEN_TINY_W, UI_SCREEN_TINY_H);
            printf("  small  = %dx%d (default)\n", UI_SCREEN_SMALL_W, UI_SCREEN_SMALL_H);
            printf("  medium = %dx%d\n", UI_SCREEN_MEDIUM_W, UI_SCREEN_MEDIUM_H);
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

    // Validate gcode-file requires test mode
    if (g_runtime_config.gcode_test_file && !g_runtime_config.test_mode) {
        printf("Error: --gcode-file requires --test mode\n");
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

/**
 * Register fonts and images for XML component system
 *
 * IMPORTANT: Fonts require TWO steps to work:
 *   1. Enable in lv_conf.h: #define LV_FONT_MONTSERRAT_XX 1
 *   2. Register here with lv_xml_register_font()
 *
 * If either step is missing, LVGL will silently fall back to a different font,
 * causing visual bugs. The semantic text components (text_heading, text_body,
 * text_small) in ui_text.cpp will crash with a clear error if a font is not
 * properly registered - this is intentional to catch configuration errors early.
 *
 * See docs/LVGL9_XML_GUIDE.md "Typography - Semantic Text Components" for details.
 */
static void register_fonts_and_images() {
    spdlog::debug("Registering fonts and images...");

    // Material Design Icons (various sizes for different UI elements)
    // Source: https://pictogrammers.com/library/mdi/
    lv_xml_register_font(NULL, "mdi_icons_64", &mdi_icons_64);
    lv_xml_register_font(NULL, "mdi_icons_48", &mdi_icons_48);
    lv_xml_register_font(NULL, "mdi_icons_32", &mdi_icons_32);
    lv_xml_register_font(NULL, "mdi_icons_24", &mdi_icons_24);
    lv_xml_register_font(NULL, "mdi_icons_16", &mdi_icons_16);

    // Arrow icon fonts (for directional controls)
    lv_xml_register_font(NULL, "arrows_64", &arrows_64);
    lv_xml_register_font(NULL, "arrows_48", &arrows_48);
    lv_xml_register_font(NULL, "arrows_32", &arrows_32);

    // Montserrat text fonts - used by semantic text components:
    // - text_heading uses font_heading (20/26/28 for small/medium/large breakpoints)
    // - text_body uses font_body (14/18/20 for small/medium/large breakpoints)
    // - text_small uses font_small (12/16/18 for small/medium/large breakpoints)
    // ALL sizes used by the responsive typography system MUST be registered here!
    // NOTE: Registering as "montserrat_*" for XML compatibility but using noto_sans_* fonts
    lv_xml_register_font(NULL, "montserrat_10", &noto_sans_10);
    lv_xml_register_font(NULL, "montserrat_12", &noto_sans_12); // text_small (small)
    lv_xml_register_font(NULL, "montserrat_14", &noto_sans_14); // text_body (small)
    lv_xml_register_font(NULL, "montserrat_16", &noto_sans_16); // text_small (medium)
    lv_xml_register_font(NULL, "montserrat_18",
                         &noto_sans_18); // text_body (medium), text_small (large)
    lv_xml_register_font(NULL, "montserrat_20",
                         &noto_sans_20); // text_heading (small), text_body (large)
    lv_xml_register_font(NULL, "montserrat_24", &noto_sans_24);
    lv_xml_register_font(NULL, "montserrat_26", &noto_sans_26); // text_heading (medium)
    lv_xml_register_font(NULL, "montserrat_28",
                         &noto_sans_28); // text_heading (large), numeric displays

    // Noto Sans fonts - same sizes as Montserrat, with extended Unicode support
    // (includes ©®™€£¥°±•… and other symbols)
    lv_xml_register_font(NULL, "noto_sans_10", &noto_sans_10);
    lv_xml_register_font(NULL, "noto_sans_12", &noto_sans_12);
    lv_xml_register_font(NULL, "noto_sans_14", &noto_sans_14);
    lv_xml_register_font(NULL, "noto_sans_16", &noto_sans_16);
    lv_xml_register_font(NULL, "noto_sans_18", &noto_sans_18);
    lv_xml_register_font(NULL, "noto_sans_20", &noto_sans_20);
    lv_xml_register_font(NULL, "noto_sans_24", &noto_sans_24);
    lv_xml_register_font(NULL, "noto_sans_26", &noto_sans_26);
    lv_xml_register_font(NULL, "noto_sans_28", &noto_sans_28);

    // Noto Sans Bold fonts (for future use)
    lv_xml_register_font(NULL, "noto_sans_bold_14", &noto_sans_bold_14);
    lv_xml_register_font(NULL, "noto_sans_bold_16", &noto_sans_bold_16);
    lv_xml_register_font(NULL, "noto_sans_bold_18", &noto_sans_bold_18);
    lv_xml_register_font(NULL, "noto_sans_bold_20", &noto_sans_bold_20);
    lv_xml_register_font(NULL, "noto_sans_bold_24", &noto_sans_bold_24);
    lv_xml_register_font(NULL, "noto_sans_bold_28", &noto_sans_bold_28);

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
    lv_xml_register_image(NULL, "A:assets/images/benchy_thumbnail_white.png",
                          "A:assets/images/benchy_thumbnail_white.png");
}

// Register XML components from ui_xml/ directory
static void register_xml_components() {
    spdlog::debug("Registering remaining XML components...");
    spdlog::debug("[XML DEBUG] Starting XML registration function");

    // Register responsive constants (AFTER globals, BEFORE components that use them)
    ui_switch_register_responsive_constants();
    spdlog::debug("[XML DEBUG] Past responsive constants");

    // Register semantic text widgets (AFTER theme init, BEFORE components that use them)
    ui_text_init();
    ui_text_input_init(); // <text_input> with bind_text support

    // Register custom widgets (BEFORE components that use them)
    ui_gcode_viewer_register();

    lv_xml_register_component_from_file("A:ui_xml/icon.xml");
    lv_xml_register_component_from_file("A:ui_xml/header_bar.xml");
    lv_xml_register_component_from_file("A:ui_xml/overlay_backdrop.xml");   // Modal dimming layer
    lv_xml_register_component_from_file("A:ui_xml/overlay_panel_base.xml"); // Base styling only
    lv_xml_register_component_from_file(
        "A:ui_xml/overlay_panel.xml"); // Depends on header_bar + base
    lv_xml_register_component_from_file("A:ui_xml/status_bar.xml");
    lv_xml_register_component_from_file("A:ui_xml/toast_notification.xml");
    lv_xml_register_component_from_file("A:ui_xml/emergency_stop_button.xml");
    lv_xml_register_component_from_file("A:ui_xml/estop_confirmation_dialog.xml");
    lv_xml_register_component_from_file("A:ui_xml/klipper_recovery_dialog.xml");
    // Note: error_dialog.xml and warning_dialog.xml removed - use modal_dialog instead
    spdlog::debug("[XML] Registering notification_history_panel.xml...");
    auto nh_panel_ret =
        lv_xml_register_component_from_file("A:ui_xml/notification_history_panel.xml");
    spdlog::debug("[XML] notification_history_panel.xml registration returned: {}",
                  (int)nh_panel_ret);
    spdlog::debug("[XML] Registering notification_history_item.xml...");
    auto nh_item_ret =
        lv_xml_register_component_from_file("A:ui_xml/notification_history_item.xml");
    spdlog::debug("[XML] notification_history_item.xml registration returned: {}",
                  (int)nh_item_ret);
    // Note: confirmation_dialog.xml, tip_detail_dialog.xml removed - use modal_dialog instead
    lv_xml_register_component_from_file("A:ui_xml/modal_dialog.xml");
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
    lv_xml_register_component_from_file("A:ui_xml/fan_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_status_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/filament_panel.xml");
    // Settings row components (must be registered before settings_panel)
    lv_xml_register_component_from_file("A:ui_xml/setting_section_header.xml");
    lv_xml_register_component_from_file("A:ui_xml/setting_toggle_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/setting_dropdown_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/setting_action_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/setting_info_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/setting_slider_row.xml");
    lv_xml_register_component_from_file("A:ui_xml/settings_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/restart_prompt_dialog.xml");
    // Calibration panels (overlays launched from settings)
    lv_xml_register_component_from_file("A:ui_xml/calibration_zoffset_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/calibration_pid_panel.xml");
    spdlog::debug("[XML] Registering bed_mesh_panel.xml...");
    auto ret = lv_xml_register_component_from_file("A:ui_xml/bed_mesh_panel.xml");
    spdlog::debug("[XML] bed_mesh_panel.xml registration returned: {}", (int)ret);
    // Settings overlay panels (launched from settings rows)
    lv_xml_register_component_from_file("A:ui_xml/display_settings_overlay.xml");
    lv_xml_register_component_from_file("A:ui_xml/network_settings_overlay.xml");
    // Note: factory_reset_dialog.xml removed - use modal_dialog instead
    lv_xml_register_component_from_file("A:ui_xml/advanced_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/test_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/print_select_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/step_progress_test.xml");
    lv_xml_register_component_from_file("A:ui_xml/gcode_test_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/glyphs_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/gradient_test_panel.xml");
    lv_xml_register_component_from_file("A:ui_xml/app_layout.xml");
    lv_xml_register_component_from_file(
        "A:ui_xml/wizard_header_bar.xml"); // Must come before wizard_container
    lv_xml_register_component_from_file("A:ui_xml/wizard_container.xml");
    lv_xml_register_component_from_file("A:ui_xml/network_list_item.xml");
    lv_xml_register_component_from_file("A:ui_xml/wifi_password_modal.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_wifi_setup.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_connection.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_printer_identify.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_heater_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_fan_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_led_select.xml");
    lv_xml_register_component_from_file("A:ui_xml/wizard_summary.xml");
}

// Initialize all reactive subjects for data binding
static void initialize_subjects() {
    spdlog::debug("Initializing reactive subjects...");
    app_globals_init_subjects();   // Global subjects (notification subject, etc.)
    ui_nav_init();                 // Navigation system (icon colors, active panel)
    ui_status_bar_init_subjects(); // Status bar subjects (printer/network icon states)

    // PrinterState must be initialized BEFORE panels that observe its subjects
    // (e.g., HomePanel observes led_state_, extruder_temp_, connection_state_)
    get_printer_state()
        .init_subjects(); // Printer state subjects (CRITICAL: must be before panel creation)

    get_global_home_panel().init_subjects();                  // Home panel data bindings
    get_global_controls_panel().init_subjects();              // Controls panel launcher
    get_global_filament_panel().init_subjects();              // Filament panel
    get_global_settings_panel().init_subjects();              // Settings panel launcher
    init_global_advanced_panel(get_printer_state(), nullptr); // Initialize advanced panel instance
    get_global_advanced_panel().init_subjects();              // Advanced panel capability subjects
    ui_wizard_init_subjects(); // Wizard subjects (for first-run config)

    // Panels that need MoonrakerAPI - store pointers for deferred set_api()
    print_select_panel = get_print_select_panel(get_printer_state(), nullptr);
    print_select_panel->init_subjects();

    // Initialize UsbManager with mock backend in test mode
    usb_manager = std::make_unique<UsbManager>(g_runtime_config.should_mock_usb());
    if (usb_manager->start()) {
        spdlog::info("UsbManager started (mock={})", g_runtime_config.should_mock_usb());
        print_select_panel->set_usb_manager(usb_manager.get());
    } else {
        spdlog::warn("Failed to start UsbManager");
    }
    print_status_panel = &get_global_print_status_panel();
    print_status_panel->init_subjects();
    motion_panel = &get_global_motion_panel();
    motion_panel->init_subjects();
    extrusion_panel = &get_global_extrusion_panel();
    extrusion_panel->init_subjects();
    bed_mesh_panel = &get_global_bed_mesh_panel();
    bed_mesh_panel->init_subjects();

    // Initialize TempControlPanel (needs PrinterState ready)
    temp_control_panel = std::make_unique<TempControlPanel>(get_printer_state(), nullptr);
    temp_control_panel->init_subjects();

    // Inject TempControlPanel into ControlsPanel for temperature sub-screens
    get_global_controls_panel().set_temp_control_panel(temp_control_panel.get());

    // Inject TempControlPanel into HomePanel for temperature icon click
    get_global_home_panel().set_temp_control_panel(temp_control_panel.get());

    // Initialize notification system (after subjects are ready)
    ui_notification_init();

    // Initialize E-Stop overlay subjects (must be before XML creation)
    EmergencyStopOverlay::instance().init_subjects();

    // Set up USB drive event notifications (after notification system is ready)
    if (usb_manager) {
        usb_manager->set_drive_callback([](UsbEvent event, const UsbDrive& drive) {
            (void)drive; // Currently not using drive info in messages
            if (event == UsbEvent::DRIVE_INSERTED) {
                ui_notification_success("USB drive connected");

                // Show USB tab in PrintSelectPanel
                if (print_select_panel) {
                    print_select_panel->on_usb_drive_inserted();
                }
            } else if (event == UsbEvent::DRIVE_REMOVED) {
                ui_notification_info("USB drive removed");

                // Hide USB tab and switch to Printer source if viewing USB
                if (print_select_panel) {
                    print_select_panel->on_usb_drive_removed();
                }
            }
        });

        // In test mode, schedule demo drive insertion after UI is fully ready
        // This ensures the toast notification is visible to the user
        if (g_runtime_config.should_mock_usb()) {
            // Use LVGL timer to delay insertion - this runs on the main thread after UI init
            lv_timer_create(
                [](lv_timer_t* timer) {
                    if (auto* mock = dynamic_cast<UsbBackendMock*>(usb_manager->get_backend())) {
                        mock->add_demo_drives();
                        spdlog::debug("Added demo USB drives for test mode (delayed)");
                    }
                    lv_timer_delete(timer);
                },
                3000, // 3 second delay for UI to fully initialize
                nullptr);
        }
    }
}

// Initialize LVGL with auto-detected display backend
static bool init_lvgl() {
    lv_init();

    // Create display backend (auto-detects: DRM → framebuffer → SDL)
    g_display_backend = DisplayBackend::create_auto();
    if (!g_display_backend) {
        spdlog::error("No display backend available");
        lv_deinit();
        return false;
    }

    spdlog::info("Using display backend: {}", g_display_backend->name());

    // Create display
    display = g_display_backend->create_display(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!display) {
        spdlog::error("Failed to create display");
        g_display_backend.reset();
        lv_deinit();
        return false;
    }

    // Create pointer input device (mouse/touch)
    indev_mouse = g_display_backend->create_input_pointer();
    if (!indev_mouse) {
#if defined(HELIX_DISPLAY_DRM) || defined(HELIX_DISPLAY_FBDEV)
        // On embedded platforms (DRM/fbdev), no input device is fatal - show error screen
        spdlog::error("No input device found - cannot operate touchscreen UI");

        static const char* suggestions[] = {
            "Check /dev/input/event* devices exist",
            "Ensure user is in 'input' group: sudo usermod -aG input $USER",
            "Check touchscreen driver is loaded: dmesg | grep -i touch",
            "Set HELIX_TOUCH_DEVICE=/dev/input/eventX to override",
            "Add \"touch_device\": \"/dev/input/event1\" to helixconfig.json",
            nullptr};

        ui_show_fatal_error("No Input Device",
                            "Could not find or open a touch/pointer input device.\n"
                            "The UI requires an input device to function.",
                            suggestions,
                            30000 // Show for 30 seconds then exit
        );

        return false;
#else
        // On desktop (SDL), continue without pointer - mouse is optional
        spdlog::warn("No pointer input device created - touch/mouse disabled");
#endif
    }

    // Configure scroll behavior from config (improves touchpad/touchscreen scrolling feel)
    // scroll_throw: momentum decay rate (1-99), higher = faster decay, default LVGL is 10
    // scroll_limit: pixels before scrolling starts, lower = more responsive, default LVGL is 10
    if (indev_mouse) {
        Config* cfg = Config::get_instance();
        int scroll_throw = cfg->get<int>("/input/scroll_throw", 25);
        int scroll_limit = cfg->get<int>("/input/scroll_limit", 5);
        lv_indev_set_scroll_throw(indev_mouse, static_cast<uint8_t>(scroll_throw));
        lv_indev_set_scroll_limit(indev_mouse, static_cast<uint8_t>(scroll_limit));
        spdlog::debug("Scroll config: throw={}, limit={}", scroll_throw, scroll_limit);
    }

    // Create keyboard input device (optional - enables physical keyboard input)
    lv_indev_t* indev_keyboard = g_display_backend->create_input_keyboard();
    if (indev_keyboard) {
        spdlog::debug("Physical keyboard input enabled");

        // Create input group for keyboard navigation and text input
        lv_group_t* input_group = lv_group_create();
        lv_group_set_default(input_group);
        lv_indev_set_group(indev_keyboard, input_group);
        spdlog::debug("Created default input group for keyboard");
    }

    spdlog::debug("LVGL initialized: {}x{}", SCREEN_WIDTH, SCREEN_HEIGHT);

    // Initialize SVG decoder for loading .svg files
    lv_svg_decoder_init();

    return true;
}

// Show splash screen with HelixScreen logo
static void show_splash_screen() {
    spdlog::debug("Showing splash screen");

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
        uint32_t scale = (static_cast<uint32_t>(target_size) * 256U) / width;
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
        lv_obj_set_style_opa((lv_obj_t*)obj, static_cast<lv_opa_t>(value), LV_PART_MAIN);
    });
    lv_anim_start(&anim);

    // Run LVGL timer to process fade-in animation and keep splash visible
    // Total display time: 2 seconds (including 0.5s fade-in)
    uint32_t splash_start = helix_get_ticks();
    uint32_t splash_duration = 2000; // 2 seconds total

    while (helix_get_ticks() - splash_start < splash_duration) {
        lv_timer_handler(); // Process animations and rendering
        helix_delay(5);
    }

    // Clean up splash screen
    lv_obj_delete(container);

    spdlog::debug("Splash screen complete");
}

// Save screenshot using SDL renderer
// Simple BMP file writer for ARGB8888 format
static bool write_bmp(const char* filename, const uint8_t* data, int width, int height) {
    // RAII for file handle - automatically closes on all return paths
    std::unique_ptr<FILE, decltype(&fclose)> f(fopen(filename, "wb"), fclose);
    if (!f)
        return false;

    // BMP header (54 bytes total)
    uint32_t file_size = 54U + (static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4U);
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
    uint32_t image_size = static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4U;
    fwrite(&image_size, 4, 1, f.get()); // Image size
    fwrite(&ppm, 4, 1, f.get());        // X pixels per meter
    fwrite(&ppm, 4, 1, f.get());        // Y pixels per meter
    fwrite(&colors, 4, 1, f.get());     // Colors in palette
    fwrite(&colors, 4, 1, f.get());     // Important colors

    // Write pixel data (BMP is bottom-up, so flip rows)
    for (int y = height - 1; y >= 0; y--) {
        fwrite(data + (static_cast<size_t>(y) * static_cast<size_t>(width) * 4), 4,
               static_cast<size_t>(width), f.get());
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
        NOTIFY_ERROR("Failed to save screenshot");
        LOG_ERROR_INTERNAL("Failed to save screenshot to {}", filename);
    }

    // Free snapshot buffer
    lv_draw_buf_destroy(snapshot);
}

// Initialize Moonraker client and API instances
static void initialize_moonraker_client(Config* config) {
    spdlog::debug("Initializing Moonraker client...");

    // Create client instance (mock or real based on test mode)
    if (get_runtime_config().should_mock_moonraker()) {
        spdlog::debug("[Test Mode] Creating MOCK Moonraker client (Voron 2.4 profile)");
        moonraker_client =
            std::make_unique<MoonrakerClientMock>(MoonrakerClientMock::PrinterType::VORON_24);
    } else {
        spdlog::debug("Creating REAL Moonraker client");
        moonraker_client = std::make_unique<MoonrakerClient>();
    }

    // Register with app_globals (raw pointer for access, main.cpp owns lifetime)
    set_moonraker_client(moonraker_client.get());

    // Configure timeouts from config file
    uint32_t connection_timeout = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_connection_timeout_ms", 10000));
    uint32_t request_timeout = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_request_timeout_ms", 30000));
    uint32_t keepalive_interval = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_keepalive_interval_ms", 10000));
    uint32_t reconnect_min_delay = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_reconnect_min_delay_ms", 200));
    uint32_t reconnect_max_delay = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_reconnect_max_delay_ms", 2000));

    moonraker_client->configure_timeouts(connection_timeout, request_timeout, keepalive_interval,
                                         reconnect_min_delay, reconnect_max_delay);

    spdlog::debug("Moonraker timeouts configured: connection={}ms, request={}ms, keepalive={}ms",
                  connection_timeout, request_timeout, keepalive_interval);

    // Register event handler to translate transport events to UI notifications
    // This decouples the transport layer (MoonrakerClient) from the UI layer
    moonraker_client->register_event_handler([](const MoonrakerEvent& evt) {
        const char* title = nullptr;
        if (evt.type == MoonrakerEventType::CONNECTION_FAILED) {
            title = "Connection Failed";
        } else if (evt.type == MoonrakerEventType::KLIPPY_DISCONNECTED) {
            title = "Printer Firmware Disconnected";
        }

        if (evt.is_error) {
            ui_notification_error(title, evt.message.c_str(),
                                  evt.type == MoonrakerEventType::CONNECTION_FAILED ||
                                      evt.type == MoonrakerEventType::KLIPPY_DISCONNECTED);
        } else {
            ui_notification_warning(evt.message.c_str());
        }
    });

    // Set up state change callback to queue updates for main thread
    // CRITICAL: This callback runs on the Moonraker event loop thread, NOT the main thread.
    // LVGL is NOT thread-safe, so we must NOT call any LVGL functions here.
    // Instead, queue the state change and process it on the main thread.
    moonraker_client->set_state_change_callback([](ConnectionState old_state,
                                                   ConnectionState new_state) {
        spdlog::debug("[main] State change callback invoked: {} -> {} (queueing for main thread)",
                      static_cast<int>(old_state), static_cast<int>(new_state));

        // Queue state change for main thread processing (same mutex as notifications)
        // Use a special JSON object with "_connection_state" marker
        std::lock_guard<std::mutex> lock(notification_mutex);
        json state_change;
        state_change["_connection_state"] = true;
        state_change["old_state"] = static_cast<int>(old_state);
        state_change["new_state"] = static_cast<int>(new_state);
        notification_queue.push(state_change);
    });

    // Register notification callback to queue updates for main thread
    // CRITICAL: Moonraker callbacks run on background thread, but LVGL is NOT thread-safe
    // Queue notifications here, process on main thread in event loop
    moonraker_client->register_notify_update([](json notification) {
        std::lock_guard<std::mutex> lock(notification_mutex);
        notification_queue.push(notification);
    });

    // Create MoonrakerAPI instance (mock or real based on test mode)
    spdlog::debug("Creating MoonrakerAPI instance...");
    if (get_runtime_config().should_use_test_files()) {
        spdlog::debug("[Test Mode] Creating MOCK MoonrakerAPI (local file transfers)");
        moonraker_api = std::make_unique<MoonrakerAPIMock>(*moonraker_client, get_printer_state());
    } else {
        moonraker_api = std::make_unique<MoonrakerAPI>(*moonraker_client, get_printer_state());
    }

    // Register with app_globals (raw pointer for access, main.cpp owns lifetime)
    set_moonraker_api(moonraker_api.get());

    // Update all panels with API reference (pass raw pointer, main.cpp owns lifetime)
    // Note: These panels are initialized in initialize_subjects() before this function
    get_global_home_panel().set_api(moonraker_api.get());
    if (temp_control_panel) {
        temp_control_panel->set_api(moonraker_api.get());
    }
    if (print_select_panel) {
        print_select_panel->set_api(moonraker_api.get());
    }
    if (print_status_panel) {
        print_status_panel->set_api(moonraker_api.get());
    }
    if (motion_panel) {
        motion_panel->set_api(moonraker_api.get());
    }
    if (extrusion_panel) {
        extrusion_panel->set_api(moonraker_api.get());
    }
    if (bed_mesh_panel) {
        bed_mesh_panel->set_api(moonraker_api.get());
    }

    // Initialize E-Stop overlay with dependencies (creates the floating button)
    EmergencyStopOverlay::instance().init(get_printer_state(), moonraker_api.get());
    EmergencyStopOverlay::instance().create();
    // Apply persisted E-Stop confirmation setting
    EmergencyStopOverlay::instance().set_require_confirmation(
        SettingsManager::instance().get_estop_require_confirmation());
    // Set initial panel for visibility tracking (home_panel is default)
    EmergencyStopOverlay::instance().on_panel_changed("home_panel");

    spdlog::debug("Moonraker client initialized (not connected yet)");
}

// Main application
int main(int argc, char** argv) {
    // Store argv early for restart capability (before any modifications)
    app_store_argv(argc, argv);

    // Ensure we're running from the project root for relative path access
    ensure_project_root_cwd();

    // Parse command-line arguments
    int initial_panel = -1;          // -1 means auto-select based on screen size
    bool show_motion = false;        // Special flag for motion sub-screen
    bool show_nozzle_temp = false;   // Special flag for nozzle temp sub-screen
    bool show_bed_temp = false;      // Special flag for bed temp sub-screen
    bool show_extrusion = false;     // Special flag for extrusion sub-screen
    bool show_fan = false;           // Special flag for fan control sub-screen
    bool show_print_status = false;  // Special flag for print status screen
    bool show_file_detail = false;   // Special flag for file detail view
    bool show_keypad = false;        // Special flag for keypad testing
    bool show_keyboard = false;      // Special flag for keyboard testing
    bool show_step_test = false;     // Special flag for step progress widget testing
    bool show_test_panel = false;    // Special flag for test/development panel
    bool show_gcode_test = false;    // Special flag for G-code 3D viewer testing
    bool show_bed_mesh = false;      // Special flag for bed mesh overlay panel
    bool show_zoffset = false;       // Special flag for Z-offset calibration panel
    bool show_pid = false;           // Special flag for PID tuning panel
    bool show_glyphs = false;        // Special flag for LVGL glyphs reference panel
    bool show_gradient_test = false; // Special flag for gradient canvas test panel
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
    int dark_mode_cli = -1;          // Theme from CLI: -1=not set, 0=light, 1=dark
    int dpi = -1;                    // Display DPI (-1 means use LV_DPI_DEF from lv_conf.h)

    // Parse command-line arguments (returns false for help/error)
    if (!parse_command_line_args(
            argc, argv, initial_panel, show_motion, show_nozzle_temp, show_bed_temp, show_extrusion,
            show_fan, show_print_status, show_file_detail, show_keypad, show_keyboard,
            show_step_test, show_test_panel, show_gcode_test, show_bed_mesh, show_zoffset, show_pid,
            show_glyphs, show_gradient_test, force_wizard, wizard_step, panel_requested,
            display_num, x_pos, y_pos, screenshot_enabled, screenshot_delay_sec, timeout_sec,
            verbosity, dark_mode_cli, dpi)) {
        return 0; // Help shown or parse error
    }

    // Check HELIX_AUTO_QUIT_MS environment variable (only if --timeout not specified)
    if (timeout_sec == 0) {
        const char* auto_quit_env = std::getenv("HELIX_AUTO_QUIT_MS");
        if (auto_quit_env != nullptr) {
            char* endptr;
            long val = strtol(auto_quit_env, &endptr, 10);
            if (*endptr == '\0' && val >= 100 && val <= 3600000) {
                // Convert milliseconds to seconds (round up to ensure at least 1 second)
                timeout_sec = static_cast<int>((val + 999) / 1000);
            }
        }
    }

    // Check HELIX_AUTO_SCREENSHOT environment variable
    const char* auto_screenshot_env = std::getenv("HELIX_AUTO_SCREENSHOT");
    if (auto_screenshot_env != nullptr && strcmp(auto_screenshot_env, "1") == 0) {
        screenshot_enabled = true;
    }

    // Initialize config system early so we can read logging settings
    Config* config = Config::get_instance();
    config->init("helixconfig.json");

    // Initialize logging subsystem
    // Priority: CLI > config > auto-detect
    {
        helix::logging::LogConfig log_config;

        // Set log level from verbosity flags
        switch (verbosity) {
        case 0:
            log_config.level = spdlog::level::warn;
            break;
        case 1:
            log_config.level = spdlog::level::info;
            break;
        case 2:
            log_config.level = spdlog::level::debug;
            break;
        default:
            log_config.level = spdlog::level::trace;
            break;
        }

        // Determine log destination: CLI > config > auto
        std::string log_dest_str = g_log_dest_cli;
        if (log_dest_str.empty()) {
            log_dest_str = config->get<std::string>("/log_dest", "auto");
        }
        log_config.target = helix::logging::parse_log_target(log_dest_str);

        // Determine log file path: CLI > config > auto
        log_config.file_path = g_log_file_cli;
        if (log_config.file_path.empty()) {
            log_config.file_path = config->get<std::string>("/log_path", "");
        }

        helix::logging::init(log_config);
    }

    spdlog::info("HelixScreen UI Prototype");
    spdlog::info("========================");
    spdlog::debug("Target: {}x{}", SCREEN_WIDTH, SCREEN_HEIGHT);
    spdlog::debug("DPI: {}{}", (dpi > 0 ? dpi : LV_DPI_DEF),
                  (dpi > 0 ? " (custom)" : " (default)"));
    spdlog::debug("Nav Width: {} pixels", UI_NAV_WIDTH(SCREEN_WIDTH));
    spdlog::debug("Initial Panel: {}", initial_panel);

    // Cleanup stale temp files from G-code modifications (older than 1 hour)
    size_t cleaned = gcode::GCodeFileModifier::cleanup_temp_files();
    if (cleaned > 0) {
        spdlog::info("Cleaned up {} stale G-code temp file(s)", cleaned);
    }

    // Determine theme: CLI overrides config, config overrides default (dark)
    bool dark_mode;
    if (dark_mode_cli >= 0) {
        // CLI explicitly set --dark or --light (temporary override, not saved)
        dark_mode = (dark_mode_cli == 1);
        spdlog::debug("Using CLI theme override: {}", dark_mode ? "dark" : "light");
    } else {
        // Load from config (or default to dark)
        dark_mode = config->get<bool>("/dark_mode", true);
        spdlog::debug("Loaded theme preference from config: {}", dark_mode ? "dark" : "light");
    }

#ifdef HELIX_DISPLAY_SDL
    // Set window position environment variables for LVGL SDL driver (desktop only)
    if (display_num >= 0) {
        char display_str[32];
        snprintf(display_str, sizeof(display_str), "%d", display_num);
        if (setenv("HELIX_SDL_DISPLAY", display_str, 1) != 0) {
            spdlog::error("Failed to set HELIX_SDL_DISPLAY environment variable");
            return 1;
        }
        spdlog::debug("Window will be centered on display {}", display_num);
    }
    if (x_pos >= 0 && y_pos >= 0) {
        char x_str[32], y_str[32];
        snprintf(x_str, sizeof(x_str), "%d", x_pos);
        snprintf(y_str, sizeof(y_str), "%d", y_pos);
        if (setenv("HELIX_SDL_XPOS", x_str, 1) != 0 || setenv("HELIX_SDL_YPOS", y_str, 1) != 0) {
            spdlog::error("Failed to set window position environment variables");
            return 1;
        }
        spdlog::debug("Window will be positioned at ({}, {})", x_pos, y_pos);
    } else if ((x_pos >= 0 && y_pos < 0) || (x_pos < 0 && y_pos >= 0)) {
        spdlog::warn("Both -x and -y must be specified for exact positioning. Ignoring.");
    }
#endif

    // Signal external splash process to exit BEFORE creating our display
    // This is critical for DRM - only one process can hold the display at a time
    if (g_runtime_config.splash_pid > 0) {
        spdlog::info("Signaling splash process (PID {}) to exit...", g_runtime_config.splash_pid);
        if (kill(g_runtime_config.splash_pid, SIGUSR1) == 0) {
            // Wait for splash to actually exit and release DRM resources
            // We can't use waitpid() since we're not the parent, so poll with kill(pid, 0)
            int wait_attempts = 50; // 50 * 20ms = 1 second max
            while (wait_attempts-- > 0 && kill(g_runtime_config.splash_pid, 0) == 0) {
                usleep(20000); // 20ms
            }
            if (wait_attempts <= 0) {
                spdlog::warn("Splash process did not exit in time, proceeding anyway");
            } else {
                spdlog::debug("Splash process exited, proceeding with display init");
            }
        } else {
            spdlog::debug("Splash process already exited (PID {})", g_runtime_config.splash_pid);
        }
        // Clear the PID so we don't try to signal it again later
        g_runtime_config.splash_pid = 0;
    }

    // Initialize LVGL with display backend
    if (!init_lvgl()) {
        return 1;
    }

    // Apply custom DPI if specified (before theme init)
    if (dpi > 0) {
        lv_display_set_dpi(display, dpi);
        spdlog::debug("Display DPI set to: {}", dpi);
    } else {
        spdlog::debug("Display DPI: {} (from LV_DPI_DEF)", lv_display_get_dpi(display));
    }

    // Create main screen
    lv_obj_t* screen = lv_screen_active();

    // Set window icon (after screen is created)
    ui_set_window_icon(display);

    // Initialize app-level resize handler for responsive layouts
    ui_resize_handler_init(screen);

    // Initialize tips manager (uses standard C++ file I/O, not LVGL's "A:" filesystem)
    TipsManager* tips_mgr = TipsManager::get_instance();
    if (!tips_mgr->init("config/printing_tips.json")) {
        spdlog::warn("Tips manager failed to initialize - tips will not be available");
    } else {
        spdlog::debug("Loaded {} tips", tips_mgr->get_total_tips());
    }

    // Register fonts and images for XML (must be done BEFORE globals.xml for theme init)
    register_fonts_and_images();

    // Register XML components (globals first to make constants available)
    spdlog::debug("Registering XML components...");
    lv_xml_register_component_from_file("A:ui_xml/globals.xml");

    // Initialize LVGL theme from globals.xml constants (after fonts and globals are registered)
    ui_theme_init(display,
                  dark_mode); // dark_mode from command-line args (--dark/--light) or config

    // Theme preference is saved by the settings panel when user toggles dark mode

    // Apply theme background color to screen
    ui_theme_apply_bg_color(screen, "app_bg_color", LV_PART_MAIN);

    // Show splash screen AFTER theme init (skip if requested via --skip-splash or --test)
    // Theme must be initialized first so app_bg_color runtime constant is available
    if (!g_runtime_config.should_skip_splash()) {
        show_splash_screen();
    }

    // Register custom widgets (must be before XML component registration)
    // Note: Material Design icons are now font-based (mdi_icons_*.c)
    // Icon lookup happens via ui_icon_codepoints.h
    ui_icon_register_widget();
    ui_switch_register();
    ui_card_register();
    ui_severity_card_register();
    ui_dialog_register();
    ui_bed_mesh_register();
    ui_gcode_viewer_register();
    ui_gradient_canvas_register();

    // Initialize component systems (BEFORE XML registration)
    ui_component_header_bar_init();

    // WORKAROUND: Add small delay to stabilize display/LVGL initialization
    // Prevents race condition between display backend and LVGL 9 XML component registration
    helix_delay(100);

    // Register remaining XML components (globals already registered for theme init)
    register_xml_components();

    // Initialize reactive subjects BEFORE creating XML
    initialize_subjects();

    // Register status bar event callbacks BEFORE creating XML (so LVGL can find them)
    ui_status_bar_register_callbacks();

    // Create entire UI from XML (single component contains everything)
    lv_obj_t* app_layout = (lv_obj_t*)lv_xml_create(screen, "app_layout", NULL);

    // Disable scrollbars on screen to prevent overflow issues with overlay panels
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // Force layout calculation for all LV_SIZE_CONTENT widgets
    lv_obj_update_layout(screen);

    // Register app_layout with navigation system (to prevent hiding it)
    ui_nav_set_app_layout(app_layout);

    // Initialize status bar (must be after XML creation and layout update)
    ui_status_bar_init();

    // Initialize toast notification system (registers close button callback)
    ui_toast_init();

    // Initialize shared overlay backdrop
    ui_nav_init_overlay_backdrop(screen);

    // Find widgets by name (robust to XML structure changes)
    lv_obj_t* navbar = lv_obj_find_by_name(app_layout, "navbar");
    lv_obj_t* content_area = lv_obj_find_by_name(app_layout, "content_area");

    if (!navbar || !content_area) {
        spdlog::error("Failed to find navbar/content_area in app_layout");
        lv_deinit();
        return 1;
    }

    // Wire up navigation button click handlers and trigger initial color update
    ui_nav_wire_events(navbar);

    // NOTE: Status icons (printer, network, notification) are now in home_panel.xml
    // They use XML-defined event callbacks and don't need C++ wiring

    // Find panel container by name (robust to layout changes like removing status_bar)
    lv_obj_t* panel_container = lv_obj_find_by_name(content_area, "panel_container");
    if (!panel_container) {
        spdlog::error("Failed to find panel_container in content_area");
        lv_deinit();
        return 1;
    }

    // Find all panel widgets by name (robust to child order changes)
    static const char* panel_names[UI_PANEL_COUNT] = {"home_panel",     "print_select_panel",
                                                      "controls_panel", "filament_panel",
                                                      "settings_panel", "advanced_panel"};

    lv_obj_t* panels[UI_PANEL_COUNT];
    for (int i = 0; i < UI_PANEL_COUNT; i++) {
        panels[i] = lv_obj_find_by_name(panel_container, panel_names[i]);
        if (!panels[i]) {
            spdlog::error("Missing panel '{}' in panel_container", panel_names[i]);
            lv_deinit();
            return 1;
        }
    }

    // Register panels with navigation system for show/hide management
    ui_nav_set_panels(panels);

    // Setup home panel observers (panels[0] is home panel)
    get_global_home_panel().setup(panels[0], screen);

    // Setup controls panel (wire launcher card click handlers)
    get_global_controls_panel().setup(panels[UI_PANEL_CONTROLS], screen);

    // Setup print select panel (wires up events, creates overlays, NOTE: data populated later)
    get_print_select_panel(get_printer_state(), nullptr)
        ->setup(panels[UI_PANEL_PRINT_SELECT], screen);

    // Setup filament panel (wire preset/action button handlers)
    get_global_filament_panel().setup(panels[UI_PANEL_FILAMENT], screen);

    // Setup settings panel (wire launcher card click handlers)
    get_global_settings_panel().setup(panels[UI_PANEL_SETTINGS], screen);

    // Setup advanced panel (wire action row click handlers)
    get_global_advanced_panel().setup(panels[UI_PANEL_ADVANCED], screen);

    // Initialize numeric keypad modal component (creates reusable keypad widget)
    ui_keypad_init(screen);

    // Create print status panel (overlay for active prints)
    overlay_panels.print_status = (lv_obj_t*)lv_xml_create(screen, "print_status_panel", nullptr);
    if (overlay_panels.print_status) {
        get_global_print_status_panel().setup(overlay_panels.print_status, screen);
        lv_obj_add_flag(overlay_panels.print_status, LV_OBJ_FLAG_HIDDEN); // Hidden by default

        // Wire print status panel to print select (for launching prints)
        get_print_select_panel(get_printer_state(), nullptr)
            ->set_print_status_panel(overlay_panels.print_status);

        spdlog::debug("Print status panel created and wired to print select");
    } else {
        spdlog::error("Failed to create print status panel");
    }

    spdlog::debug("XML UI created successfully with reactive navigation");

    // Test notifications - commented out, uncomment to debug notification history
    // if (get_runtime_config().test_mode) {
    //     NOTIFY_INFO("Info notification test");
    //     NOTIFY_SUCCESS("Success notification test");
    //     NOTIFY_WARNING("Warning notification test");
    //     NOTIFY_ERROR("Error notification test");
    // }

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
        spdlog::debug("Navigating to initial panel: {}", initial_panel);
        ui_nav_set_active(static_cast<ui_panel_id_t>(initial_panel));
    }

    // Show requested overlay panels (motion, temp controls, etc.)
    if (!wizard_active) {
        if (show_motion) {
            spdlog::debug("Opening motion overlay as requested by command-line flag");
            overlay_panels.motion = (lv_obj_t*)lv_xml_create(screen, "motion_panel", nullptr);
            if (overlay_panels.motion) {
                get_global_motion_panel().setup(overlay_panels.motion, screen);
                ui_nav_push_overlay(overlay_panels.motion);
            }
        }
        if (show_nozzle_temp) {
            spdlog::debug("Opening nozzle temp overlay as requested by command-line flag");
            overlay_panels.nozzle_temp =
                (lv_obj_t*)lv_xml_create(screen, "nozzle_temp_panel", nullptr);
            if (overlay_panels.nozzle_temp) {
                temp_control_panel->setup_nozzle_panel(overlay_panels.nozzle_temp, screen);
                ui_nav_push_overlay(overlay_panels.nozzle_temp);
            }
        }
        if (show_bed_temp) {
            spdlog::debug("Opening bed temp overlay as requested by command-line flag");
            overlay_panels.bed_temp = (lv_obj_t*)lv_xml_create(screen, "bed_temp_panel", nullptr);
            if (overlay_panels.bed_temp) {
                temp_control_panel->setup_bed_panel(overlay_panels.bed_temp, screen);
                ui_nav_push_overlay(overlay_panels.bed_temp);
            }
        }
        if (show_extrusion) {
            spdlog::debug("Opening extrusion overlay as requested by command-line flag");
            overlay_panels.extrusion = (lv_obj_t*)lv_xml_create(screen, "extrusion_panel", nullptr);
            if (overlay_panels.extrusion) {
                get_global_extrusion_panel().setup(overlay_panels.extrusion, screen);
                ui_nav_push_overlay(overlay_panels.extrusion);
            }
        }
        if (show_fan) {
            spdlog::debug("Opening fan control overlay as requested by command-line flag");
            auto& fan_panel = get_global_fan_panel();
            if (!fan_panel.are_subjects_initialized()) {
                fan_panel.init_subjects();
            }
            lv_obj_t* fan_obj = (lv_obj_t*)lv_xml_create(screen, "fan_panel", nullptr);
            if (fan_obj) {
                fan_panel.setup(fan_obj, screen);
                ui_nav_push_overlay(fan_obj);
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
                spdlog::debug("Bed mesh overlay created successfully, calling setup");
                get_global_bed_mesh_panel().setup(bed_mesh, screen);
                ui_nav_push_overlay(bed_mesh);
                spdlog::debug("Bed mesh overlay pushed to nav stack");
            } else {
                spdlog::error(
                    "Failed to create bed mesh overlay from XML component 'bed_mesh_panel'");
            }
        }
        if (show_zoffset) {
            spdlog::debug("Opening Z-offset calibration overlay as requested by command-line flag");
            lv_obj_t* zoffset_panel =
                (lv_obj_t*)lv_xml_create(screen, "calibration_zoffset_panel", nullptr);
            if (zoffset_panel) {
                spdlog::debug("Z-offset calibration overlay created successfully, calling setup");
                get_global_zoffset_cal_panel().setup(zoffset_panel, screen, moonraker_client.get());
                ui_nav_push_overlay(zoffset_panel);
                spdlog::debug("Z-offset calibration overlay pushed to nav stack");
            } else {
                spdlog::error("Failed to create Z-offset calibration overlay from XML component "
                              "'calibration_zoffset_panel'");
            }
        }
        if (show_pid) {
            spdlog::debug("Opening PID tuning overlay as requested by command-line flag");
            lv_obj_t* pid_panel =
                (lv_obj_t*)lv_xml_create(screen, "calibration_pid_panel", nullptr);
            if (pid_panel) {
                get_global_pid_cal_panel().setup(pid_panel, screen, moonraker_client.get());
                ui_nav_push_overlay(pid_panel);
                spdlog::debug("PID tuning overlay pushed to nav stack");
            } else {
                spdlog::error("Failed to create PID tuning overlay from XML component "
                              "'calibration_pid_panel'");
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
                get_global_step_test_panel().setup(step_test, screen);
            }
        }
        if (show_test_panel) {
            spdlog::debug("Opening test panel as requested by command-line flag");
            lv_obj_t* test_panel_obj = (lv_obj_t*)lv_xml_create(screen, "test_panel", nullptr);
            if (test_panel_obj) {
                get_global_test_panel().setup(test_panel_obj, screen);
            }
        }
        if (show_file_detail) {
            spdlog::debug("File detail view requested - navigating to print select panel first");
            ui_nav_set_active(UI_PANEL_PRINT_SELECT);
        }

        // Handle --select-file flag: auto-select a file in the print select panel
        if (g_runtime_config.select_file != nullptr) {
            spdlog::info("--select-file flag: Will auto-select file '{}'",
                         g_runtime_config.select_file);
            ui_nav_set_active(UI_PANEL_PRINT_SELECT);
            // Set pending selection - will trigger when file list is loaded
            auto* print_panel = get_print_select_panel(get_printer_state(), moonraker_api.get());
            if (print_panel) {
                print_panel->set_pending_file_selection(g_runtime_config.select_file);
            }
        }
    }

    // Create G-code test panel if requested (independent of wizard state)
    if (show_gcode_test) {
        spdlog::debug("Creating G-code test panel");
        lv_obj_t* gcode_test =
            ui_panel_gcode_test_create(screen); // Uses deprecated wrapper (creates + setups)
        if (gcode_test) {
            spdlog::debug("G-code test panel created successfully");
        } else {
            spdlog::error("Failed to create G-code test panel");
        }
    }

    // Create glyphs panel if requested (independent of wizard state)
    if (show_glyphs) {
        spdlog::debug("Creating glyphs reference panel");
        lv_obj_t* glyphs_panel = ui_panel_glyphs_create(screen);
        if (glyphs_panel) {
            spdlog::debug("Glyphs panel created successfully");
        } else {
            spdlog::error("Failed to create glyphs panel");
        }
    }

    // Create gradient test panel if requested (independent of wizard state)
    if (show_gradient_test) {
        spdlog::debug("Creating gradient test panel");
        lv_obj_t* gradient_panel = (lv_obj_t*)lv_xml_create(screen, "gradient_test_panel", nullptr);
        if (gradient_panel) {
            spdlog::debug("Gradient test panel created successfully");
        } else {
            spdlog::error("Failed to create gradient test panel");
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

        // Build HTTP base URL for file transfers (same host:port, http:// scheme)
        std::string http_base_url =
            "http://" + config->get<std::string>(config->df() + "moonraker_host") + ":" +
            std::to_string(config->get<int>(config->df() + "moonraker_port"));
        moonraker_api->set_http_base_url(http_base_url);

        // Register discovery callback (Observer pattern - decouples Moonraker from PrinterState)
        moonraker_client->set_on_discovery_complete([](const PrinterCapabilities& caps) {
            // Update PrinterState with discovered capabilities for reactive UI bindings
            get_printer_state().set_printer_capabilities(caps);

            // Update version info from client (for Settings About section)
            get_printer_state().set_klipper_version(moonraker_client->get_software_version());
            get_printer_state().set_moonraker_version(moonraker_client->get_moonraker_version());
        });

        // Connect to Moonraker
        spdlog::debug("Connecting to Moonraker at {}", moonraker_url);
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
    uint32_t screenshot_time =
        helix_get_ticks() + (static_cast<uint32_t>(screenshot_delay_sec) * 1000U);
    bool screenshot_taken = false;

    // Auto-quit timeout timer (if enabled)
    uint32_t start_time = helix_get_ticks();
    uint32_t timeout_ms = static_cast<uint32_t>(timeout_sec) * 1000U;

    // Request timeout check timer (check every 2 seconds)
    uint32_t last_timeout_check = helix_get_ticks();
    uint32_t timeout_check_interval = static_cast<uint32_t>(
        config->get<int>(config->df() + "moonraker_timeout_check_interval_ms", 2000));

    // Main event loop - LVGL handles display events internally via lv_timer_handler()
    // Loop continues while display exists and quit not requested
    while (lv_display_get_next(NULL) && !app_quit_requested()) {
#ifdef HELIX_DISPLAY_SDL
        // Desktop keyboard shortcuts (SDL only)
        // Check for Cmd+Q (macOS) or Win+Q (Windows) to quit
        SDL_Keymod modifiers = SDL_GetModState();
        const Uint8* keyboard_state = SDL_GetKeyboardState(NULL);
        if ((modifiers & KMOD_GUI) && keyboard_state[SDL_SCANCODE_Q]) {
            spdlog::info("Cmd+Q/Win+Q pressed - exiting...");
            break;
        }

#endif

        // Auto-screenshot after configured delay (only if enabled)
        if (screenshot_enabled && !screenshot_taken && helix_get_ticks() >= screenshot_time) {
            save_screenshot();
            screenshot_taken = true;
        }

        // Auto-quit after timeout (if enabled)
        if (timeout_sec > 0 && (helix_get_ticks() - start_time) >= timeout_ms) {
            spdlog::info("Timeout reached ({} seconds) - exiting...", timeout_sec);
            break;
        }

        // Check for request timeouts (using configured interval)
        uint32_t current_time = helix_get_ticks();
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

                // Check for connection state change (queued from state_change_callback)
                if (notification.contains("_connection_state")) {
                    int new_state = notification["new_state"].get<int>();
                    static const char* messages[] = {
                        "Disconnected",     // DISCONNECTED
                        "Connecting...",    // CONNECTING
                        "Connected",        // CONNECTED
                        "Reconnecting...",  // RECONNECTING
                        "Connection Failed" // FAILED
                    };
                    spdlog::debug("[main] Processing queued connection state change: {}",
                                  messages[new_state]);
                    get_printer_state().set_printer_connection_state(new_state,
                                                                     messages[new_state]);
                } else {
                    // Regular Moonraker notification
                    get_printer_state().update_from_notification(notification);
                }
            }
        }

        // Run LVGL tasks - handles display events and processes input
        lv_timer_handler();
        fflush(stdout);
        helix_delay(5); // Small delay to prevent 100% CPU usage
    }

    // Cleanup
    spdlog::info("Shutting down...");

    // Clear app_globals references before destroying instances
    set_moonraker_api(nullptr);
    set_moonraker_client(nullptr);

    // Reset unique_ptrs explicitly in correct order (API before client)
    moonraker_api.reset();
    moonraker_client.reset();

    // Clean up USB manager explicitly BEFORE spdlog shutdown.
    // UsbBackendMock::stop() logs, and we need spdlog alive for that.
    usb_manager.reset();

    // Clean up wizard WiFi step explicitly BEFORE lv_deinit and spdlog shutdown.
    // This owns WiFiManager and EthernetManager which have background threads.
    // If destroyed during static destruction, those threads may access destroyed mutexes.
    destroy_wizard_wifi_step();

    lv_deinit(); // LVGL handles SDL cleanup internally

    // Shutdown spdlog BEFORE static destruction begins.
    // Many static unique_ptr<Panel> objects have destructors that may log.
    // If spdlog is destroyed first during static destruction, logging crashes.
    // By calling shutdown() here, we flush and drop all sinks, making any
    // subsequent log calls safe no-ops.
    spdlog::shutdown();

    return 0;
}
