// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cli_args.h"

#include "app_globals.h"
#include "helix_version.h"
#include "runtime_config.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

// External globals that CLI args modify
extern int g_screen_width;
extern int g_screen_height;

// Logging configuration globals (defined here, populated by parse_cli_args)
// These are extern'd by application.cpp for use during logging initialization
std::string g_log_dest_cli; // CLI override for log destination
std::string g_log_file_cli; // CLI override for log file path
std::string
    g_log_level_cli; // CLI override for log level (trace/debug/info/warn/error/critical/off)

namespace helix {

std::optional<PanelId> panel_name_to_id(const char* name) {
    if (strcmp(name, "home") == 0)
        return PanelId::Home;
    if (strcmp(name, "controls") == 0)
        return PanelId::Controls;
    if (strcmp(name, "filament") == 0)
        return PanelId::Filament;
    if (strcmp(name, "settings") == 0)
        return PanelId::Settings;
    if (strcmp(name, "advanced") == 0)
        return PanelId::Advanced;
    if (strcmp(name, "print-select") == 0 || strcmp(name, "print_select") == 0)
        return PanelId::PrintSelect;
    return std::nullopt;
}

void print_test_mode_banner() {
    RuntimeConfig& config = *get_runtime_config();

    printf("╔════════════════════════════════════════╗\n");
    printf("║           TEST MODE ENABLED            ║\n");
    printf("╚════════════════════════════════════════╝\n");

    if (config.use_real_wifi)
        printf("  Using REAL WiFi hardware\n");
    else
        printf("  Using MOCK WiFi backend\n");

    if (config.use_real_ethernet)
        printf("  Using REAL Ethernet hardware\n");
    else
        printf("  Using MOCK Ethernet backend\n");

    if (config.use_real_moonraker)
        printf("  Using REAL Moonraker connection\n");
    else
        printf("  Using MOCK Moonraker responses\n");

    if (config.use_real_files)
        printf("  Using REAL files from printer\n");
    else
        printf("  Using TEST file data\n");

    if (config.simulate_disconnect)
        printf("  SIMULATING DISCONNECTED STATE\n");

    if (config.disable_mock_ams)
        printf("  Mock AMS DISABLED (runout modal enabled)\n");

    printf("  Config: %s\n", RuntimeConfig::TEST_CONFIG_PATH);

    printf("\n");
}

// Helper to parse integer with validation
static bool parse_int(const char* str, long min_val, long max_val, int& out, const char* name) {
    char* endptr;
    long val = strtol(str, &endptr, 10);
    if (*endptr != '\0' || val < min_val || val > max_val) {
        printf("Error: invalid %s (must be %ld-%ld): %s\n", name, min_val, max_val, str);
        return false;
    }
    out = static_cast<int>(val);
    return true;
}

// Helper to parse double with validation
static bool parse_double(const char* str, double& out, const char* name) {
    char* endptr;
    double val = strtod(str, &endptr);
    if (*endptr != '\0') {
        printf("Error: %s requires a numeric value\n", name);
        return false;
    }
    out = val;
    return true;
}

static void print_help(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -s, --size <size>    Screen size: micro, tiny, small, medium, large, xlarge (or "
           "WxH)\n");
    printf("  -p, --panel <panel>  Initial panel (default: home)\n");
    printf("  -k, --keypad         Show numeric keypad for testing\n");
    printf("  --keyboard           Show keyboard for testing (no textarea)\n");
    printf("  -w, --wizard         Force first-run configuration wizard\n");
    printf("  --wizard-step <step> Jump to specific wizard step for testing\n");
    printf("  --calibrate-touch    Force touch calibration on startup\n");
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
    printf("  --log-dest <dest>    Log destination: auto, journal, syslog, file, console\n");
    printf("  --log-level <level>  Log level: trace, debug, info, warn, error, critical, off\n");
    printf("  --log-file <path>    Log file path (when --log-dest=file)\n");
    printf("  -M, --memory-report  Log memory usage every 30 seconds (development)\n");
    printf("  --show-memory        Show memory stats overlay (press M to toggle)\n");
    printf("  --release-notes      Fetch latest release notes and show in update modal\n");
    printf("  --debug-subjects     Enable verbose subject debugging with stack traces\n");
    printf("  --debug-touches      Draw ripple effects at each touch point for debugging\n");
    printf("  --moonraker <url>    Override Moonraker URL (e.g., ws://192.168.1.112:7125)\n");
    printf("  --rotate <degrees>   Display rotation: 0, 90, 180, 270\n");
    printf("  --layout <type>      Override auto-detected layout (auto, standard, ultrawide, "
           "portrait, micro, micro-portrait, tiny, tiny-portrait)\n");
    printf("  -h, --help           Show this help message\n");
    printf("  -V, --version        Show version information\n");
    printf("\nTest Mode Options:\n");
    printf("  --test               Enable test mode (uses all mocks by default)\n");
    printf("    --real-wifi        Use real WiFi hardware (requires --test)\n");
    printf("    --real-ethernet    Use real Ethernet hardware (requires --test)\n");
    printf("    --real-moonraker   Connect to real printer (requires --test)\n");
    printf("    --real-files       Use real files from printer (requires --test)\n");
    printf("    --real-sensors     Use real sensor data (requires --test)\n");
    printf("    --disconnected     Simulate disconnected state (requires --test)\n");
    printf("    --no-ams           Don't create mock AMS (enables runout modal testing)\n");
    printf("    --test-history     Enable test history API data\n");
    printf("    --sim-speed <n>    Simulation speedup factor (1.0-1000.0, e.g., 100 for 100x)\n");
    printf("    --mock-crash       Write synthetic crash.txt to test crash reporter UI\n");
    printf("    --select-file <name>  Auto-select file in print-select panel\n");
    printf("\nG-code Viewer Options (require --test):\n");
    printf("  --gcode-file <path>  Load specific G-code file in gcode-test panel\n");
    printf("  --camera <params>    Set camera params: \"az:90.5,el:4.0,zoom:15.5\"\n");
    printf("  --gcode-az <deg>     Set camera azimuth angle (degrees)\n");
    printf("  --gcode-el <deg>     Set camera elevation angle (degrees)\n");
    printf("  --gcode-zoom <n>     Set camera zoom level (positive number)\n");
    printf("  --gcode-debug-colors Enable per-face debug coloring\n");
    printf("  --render-2d          Force 2D layer renderer (fast, no 3D)\n");
    printf("  --render-3d          Force 3D GLES renderer\n");
    printf("\nAvailable panels:\n");
    printf("  Base: home, controls, filament, settings, advanced\n");
    printf("  Print: print-select (cards), print-select-list, print-detail\n");
    printf("  Controls: motion, nozzle-temp, bed-temp, fan, led, bed-mesh, pid\n");
    printf("  Settings: display, sensors, touch-cal, hardware-health, network, theme\n");
    printf("  Advanced: zoffset, screws, input-shaper, spoolman, history-dashboard, macros\n");
    printf("  Print: print-status, print-tune\n");
    printf("  Dev: ams, step-test, test, gcode-test, glyphs\n");
    printf("\nScreen sizes:\n");
    printf("  micro    = %dx%d\n", UI_SCREEN_MICRO_W, UI_SCREEN_MICRO_H);
    printf("  tiny     = %dx%d\n", UI_SCREEN_TINY_W, UI_SCREEN_TINY_H);
    printf("  small    = %dx%d\n", UI_SCREEN_SMALL_W, UI_SCREEN_SMALL_H);
    printf("  medium   = %dx%d (default)\n", UI_SCREEN_MEDIUM_W, UI_SCREEN_MEDIUM_H);
    printf("  large    = %dx%d\n", UI_SCREEN_LARGE_W, UI_SCREEN_LARGE_H);
    printf("  xlarge   = %dx%d\n", UI_SCREEN_XLARGE_W, UI_SCREEN_XLARGE_H);
    printf("  WxH      = arbitrary resolution (e.g., -s 1920x1080)\n");
    printf("\nWizard steps:\n");
    printf("  wifi, connection, printer-identify, bed, hotend, fan, led, summary\n");
    printf("\nWindow placement:\n");
    printf("  Use -d to center window on specific display\n");
    printf("  Use -x/-y for exact pixel coordinates (both required)\n");
    printf("  Examples:\n");
    printf("    %s --display 1        # Center on display 1\n", program_name);
    printf("    %s -x 100 -y 200      # Position at (100, 200)\n", program_name);
    printf("\nTest Mode Examples:\n");
    printf("  %s --test                           # Full mock mode\n", program_name);
    printf("  %s --test --real-moonraker          # Test UI with real printer\n", program_name);
    printf("  %s --test --real-wifi --real-files  # Real WiFi and files, mock rest\n",
           program_name);
}

// Parse -p/--panel argument - handles overlays and base panels
static bool parse_panel_arg(const char* panel_arg, CliArgs& args) {
    args.panel_requested = true;

    // Check for overlay panels first (these set flags)
    if (strcmp(panel_arg, "motion") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Controls);
        args.overlays.motion = true;
    } else if (strcmp(panel_arg, "nozzle-temp") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Controls);
        args.overlays.nozzle_temp = true;
    } else if (strcmp(panel_arg, "bed-temp") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Controls);
        args.overlays.bed_temp = true;
    } else if (strcmp(panel_arg, "fan") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Controls);
        args.overlays.fan = true;
    } else if (strcmp(panel_arg, "led") == 0 || strcmp(panel_arg, "led-control") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Home);
        args.overlays.led = true;
    } else if (strcmp(panel_arg, "print-status") == 0 || strcmp(panel_arg, "printing") == 0) {
        args.overlays.print_status = true;
    } else if (strcmp(panel_arg, "print-select-list") == 0 ||
               strcmp(panel_arg, "print_select_list") == 0) {
        // Print select panel in LIST view
        args.initial_panel = static_cast<int>(PanelId::PrintSelect);
        args.overlays.print_select_list = true;
        get_runtime_config()->print_select_list_mode = true;
    } else if (strcmp(panel_arg, "print-detail") == 0 || strcmp(panel_arg, "file-detail") == 0 ||
               strcmp(panel_arg, "print-file-detail") == 0) {
        // Print file detail overlay
        args.initial_panel = static_cast<int>(PanelId::PrintSelect);
        args.overlays.file_detail = true;
    } else if (strcmp(panel_arg, "step-test") == 0 || strcmp(panel_arg, "step_test") == 0) {
        args.overlays.step_test = true;
    } else if (strcmp(panel_arg, "test") == 0) {
        args.overlays.test_panel = true;
    } else if (strcmp(panel_arg, "gcode-test") == 0 || strcmp(panel_arg, "gcode_test") == 0) {
        args.overlays.gcode_test = true;
    } else if (strcmp(panel_arg, "bed-mesh") == 0 || strcmp(panel_arg, "bed_mesh") == 0) {
        args.overlays.bed_mesh = true;
    } else if (strcmp(panel_arg, "zoffset") == 0 || strcmp(panel_arg, "z-offset") == 0) {
        args.overlays.zoffset = true;
    } else if (strcmp(panel_arg, "pid") == 0) {
        args.overlays.pid = true;
    } else if (strcmp(panel_arg, "screws") == 0 || strcmp(panel_arg, "screws-tilt") == 0 ||
               strcmp(panel_arg, "bed-leveling") == 0) {
        args.overlays.screws_tilt = true;
    } else if (strcmp(panel_arg, "input-shaper") == 0 || strcmp(panel_arg, "input_shaper") == 0 ||
               strcmp(panel_arg, "shaper") == 0) {
        args.overlays.input_shaper = true;
    } else if (strcmp(panel_arg, "history-dashboard") == 0 ||
               strcmp(panel_arg, "history_dashboard") == 0 ||
               strcmp(panel_arg, "print-history") == 0) {
        args.overlays.history_dashboard = true;
    } else if (strcmp(panel_arg, "glyphs") == 0) {
        args.overlays.glyphs = true;
    } else if (strcmp(panel_arg, "gradient-test") == 0) {
        args.overlays.gradient_test = true;
    } else if (strcmp(panel_arg, "ams") == 0) {
        args.overlays.ams = true;
    } else if (strcmp(panel_arg, "spoolman") == 0) {
        args.overlays.spoolman = true;
    } else if (strcmp(panel_arg, "wizard-ams-identify") == 0 ||
               strcmp(panel_arg, "wizard_ams_identify") == 0) {
        args.overlays.wizard_ams_identify = true;
    } else if (strcmp(panel_arg, "theme") == 0 || strcmp(panel_arg, "theme-preview") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.theme = true;
    } else if (strcmp(panel_arg, "edit-theme") == 0 || strcmp(panel_arg, "theme-edit") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.theme_edit = true;
    }
    // Settings overlays (for screenshot automation)
    else if (strcmp(panel_arg, "display") == 0 || strcmp(panel_arg, "display-settings") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.display_settings = true;
    } else if (strcmp(panel_arg, "sensors") == 0 || strcmp(panel_arg, "sensor-settings") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.sensor_settings = true;
    } else if (strcmp(panel_arg, "touch-cal") == 0 || strcmp(panel_arg, "touch-calibration") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.touch_calibration = true;
    } else if (strcmp(panel_arg, "hardware-health") == 0 || strcmp(panel_arg, "hardware") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.hardware_health = true;
    } else if (strcmp(panel_arg, "network") == 0 || strcmp(panel_arg, "network-settings") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Settings);
        args.overlays.network_settings = true;
    }
    // Advanced overlays
    else if (strcmp(panel_arg, "macros") == 0) {
        args.initial_panel = static_cast<int>(PanelId::Advanced);
        args.overlays.macros = true;
    } else if (strcmp(panel_arg, "print-tune") == 0 || strcmp(panel_arg, "tune") == 0) {
        args.overlays.print_status = true; // Needs print running
        args.overlays.print_tune = true;
    } else {
        // Try base panel lookup
        auto panel_id = panel_name_to_id(panel_arg);
        if (panel_id) {
            args.initial_panel = static_cast<int>(*panel_id);
        } else {
            printf("Unknown panel: %s\n", panel_arg);
            printf("Available panels: home, controls, motion, nozzle-temp, bed-temp, "
                   "bed-mesh, zoffset, pid, screws, input-shaper, fan, led, ams, "
                   "spoolman, print-status, filament, settings, advanced, print-history, "
                   "print-select, step-test, test, gcode-test, glyphs, gradient-test, "
                   "wizard-ams-identify\n");
            return false;
        }
    }
    return true;
}

// Parse --camera argument (complex format: "az:90.5,el:4.0,zoom:15.5")
static bool parse_camera_arg(const char* camera_str, RuntimeConfig& config) {
    if (camera_str[0] == '\0') {
        printf("Error: --camera requires a non-empty string argument\n");
        printf("Format: --camera \"az:90.5,el:4.0,zoom:15.5\" (each parameter optional)\n");
        return false;
    }

    std::unique_ptr<char, decltype(&free)> str_copy(strdup(camera_str), free);
    char* token = strtok(str_copy.get(), ",");

    while (token != nullptr) {
        while (*token == ' ')
            token++; // Trim whitespace

        if (strncmp(token, "az:", 3) == 0) {
            double val;
            if (!parse_double(token + 3, val, "--camera az"))
                return false;
            config.gcode_camera_azimuth = static_cast<float>(val);
            config.gcode_camera_azimuth_set = true;
        } else if (strncmp(token, "el:", 3) == 0) {
            double val;
            if (!parse_double(token + 3, val, "--camera el"))
                return false;
            config.gcode_camera_elevation = static_cast<float>(val);
            config.gcode_camera_elevation_set = true;
        } else if (strncmp(token, "zoom:", 5) == 0) {
            double val;
            if (!parse_double(token + 5, val, "--camera zoom"))
                return false;
            if (val <= 0) {
                printf("Error: Invalid zoom value in --camera (must be positive): %s\n", token);
                return false;
            }
            config.gcode_camera_zoom = static_cast<float>(val);
            config.gcode_camera_zoom_set = true;
        } else {
            printf("Error: Unknown camera parameter: %s\n", token);
            printf("Valid parameters: az:<degrees>, el:<degrees>, zoom:<factor>\n");
            return false;
        }
        token = strtok(nullptr, ",");
    }
    return true;
}

bool parse_cli_args(int argc, char** argv, CliArgs& args, int& screen_width, int& screen_height) {
    RuntimeConfig& config = *get_runtime_config();

    for (int i = 1; i < argc; i++) {
        // Screen size
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -s/--size requires an argument\n");
                return false;
            }
            const char* size_arg = argv[++i];
            if (strcmp(size_arg, "micro") == 0) {
                screen_width = UI_SCREEN_MICRO_W;
                screen_height = UI_SCREEN_MICRO_H;
                args.screen_size = ScreenSize::MICRO;
            } else if (strcmp(size_arg, "tiny") == 0) {
                screen_width = UI_SCREEN_TINY_W;
                screen_height = UI_SCREEN_TINY_H;
                args.screen_size = ScreenSize::TINY;
            } else if (strcmp(size_arg, "small") == 0) {
                screen_width = UI_SCREEN_SMALL_W;
                screen_height = UI_SCREEN_SMALL_H;
                args.screen_size = ScreenSize::SMALL;
            } else if (strcmp(size_arg, "medium") == 0) {
                screen_width = UI_SCREEN_MEDIUM_W;
                screen_height = UI_SCREEN_MEDIUM_H;
                args.screen_size = ScreenSize::MEDIUM;
            } else if (strcmp(size_arg, "large") == 0) {
                screen_width = UI_SCREEN_LARGE_W;
                screen_height = UI_SCREEN_LARGE_H;
                args.screen_size = ScreenSize::LARGE;
            } else if (strcmp(size_arg, "xlarge") == 0) {
                screen_width = UI_SCREEN_XLARGE_W;
                screen_height = UI_SCREEN_XLARGE_H;
                args.screen_size = ScreenSize::XLARGE;
            } else {
                // Try parsing as WxH format (e.g., "480x400" or "1920x1080")
                int w = 0, h = 0;
                if (sscanf(size_arg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    screen_width = w;
                    screen_height = h;
                    // 480x272-class displays use MICRO for dedicated layout targeting.
                    if (std::min(w, h) <= UI_SCREEN_MICRO_H && std::max(w, h) <= UI_SCREEN_TINY_W) {
                        args.screen_size = ScreenSize::MICRO;
                    } else if (h <= UI_BREAKPOINT_TINY_MAX) {
                        args.screen_size = ScreenSize::TINY;
                    } else if (h <= UI_BREAKPOINT_SMALL_MAX) {
                        args.screen_size = ScreenSize::SMALL;
                    } else if (h <= UI_BREAKPOINT_MEDIUM_MAX) {
                        args.screen_size = ScreenSize::MEDIUM;
                    } else if (h <= UI_BREAKPOINT_LARGE_MAX) {
                        args.screen_size = ScreenSize::LARGE;
                    } else {
                        args.screen_size = ScreenSize::XLARGE;
                    }
                } else {
                    printf("Unknown screen size: %s\n", size_arg);
                    printf("Available sizes: micro, tiny, small, medium, large, xlarge (or WxH "
                           "like 480x400)\n");
                    return false;
                }
            }
        }
        // Panel selection
        else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--panel") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -p/--panel requires an argument\n");
                return false;
            }
            if (!parse_panel_arg(argv[++i], args))
                return false;
        }
        // Simple boolean flags
        else if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "--keypad") == 0) {
            args.overlays.keypad = true;
        } else if (strcmp(argv[i], "--keyboard") == 0 || strcmp(argv[i], "--show-keyboard") == 0) {
            args.overlays.keyboard = true;
        } else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wizard") == 0) {
            args.force_wizard = true;
        }
        // Touch calibration
        else if (strcmp(argv[i], "--calibrate-touch") == 0) {
            args.calibrate_touch = true;
        }
        // Wizard step
        else if (strcmp(argv[i], "--wizard-step") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --wizard-step requires an argument (0-9)\n");
                return false;
            }
            args.wizard_step = atoi(argv[++i]);
            args.force_wizard = true;
            if (args.wizard_step < 0 || args.wizard_step > 9) {
                printf("Error: wizard step must be 0-9\n");
                return false;
            }
        }
        // Display number
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--display") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -d/--display requires a number argument\n");
                return false;
            }
            if (!parse_int(argv[++i], 0, 10, args.display_num, "display number"))
                return false;
        }
        // Window position
        else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--x-pos") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -x/--x-pos requires a number argument\n");
                return false;
            }
            if (!parse_int(argv[++i], 0, 10000, args.x_pos, "x position"))
                return false;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--y-pos") == 0) {
            if (i + 1 >= argc) {
                printf("Error: -y/--y-pos requires a number argument\n");
                return false;
            }
            if (!parse_int(argv[++i], 0, 10000, args.y_pos, "y position"))
                return false;
        }
        // DPI
        else if (strcmp(argv[i], "--dpi") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --dpi requires a number argument\n");
                return false;
            }
            if (!parse_int(argv[++i], 50, 500, args.dpi, "DPI"))
                return false;
        }
        // Screenshot
        else if (strcmp(argv[i], "--screenshot") == 0) {
            args.screenshot_enabled = true;
            if (i + 1 < argc) {
                char* endptr;
                long val = strtol(argv[i + 1], &endptr, 10);
                if (*endptr == '\0' && val > 0 && val <= 60) {
                    args.screenshot_delay_sec = static_cast<int>(val);
                    i++;
                }
            }
        }
        // Timeout
        else if (strcmp(argv[i], "--timeout") == 0 || strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --timeout/-t requires a number argument\n");
                return false;
            }
            if (!parse_int(argv[++i], 1, 3600, args.timeout_sec, "timeout"))
                return false;
        }
        // Theme
        else if (strcmp(argv[i], "--dark") == 0) {
            args.dark_mode_cli = 1;
        } else if (strcmp(argv[i], "--light") == 0) {
            args.dark_mode_cli = 0;
        }
        // Test mode flags
        else if (strcmp(argv[i], "--test") == 0) {
            config.test_mode = true;
        } else if (strcmp(argv[i], "--skip-splash") == 0) {
            config.skip_splash = true;
        } else if (strncmp(argv[i], "--splash-pid=", 13) == 0) {
            config.splash_pid = static_cast<pid_t>(atoi(argv[i] + 13));
            config.skip_splash = true; // External splash already running, don't show internal one
            spdlog::info("[CLI] Splash PID received from launcher: {}", config.splash_pid);
        } else if (strncmp(argv[i], "--rotate=", 9) == 0) {
            args.rotation = atoi(argv[i] + 9);
            spdlog::info("[CLI] Display rotation: {}°", args.rotation);
        } else if (strcmp(argv[i], "--rotate") == 0 && i + 1 < argc) {
            args.rotation = atoi(argv[++i]);
            spdlog::info("[CLI] Display rotation: {}°", args.rotation);
        } else if (strcmp(argv[i], "--layout") == 0 || strncmp(argv[i], "--layout=", 9) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--layout=", 9) == 0) {
                value = argv[i] + 9;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --layout requires an argument\n");
                return false;
            }
            // Validate layout value
            if (strcmp(value, "auto") == 0 || strcmp(value, "standard") == 0 ||
                strcmp(value, "ultrawide") == 0 || strcmp(value, "portrait") == 0 ||
                strcmp(value, "micro") == 0 || strcmp(value, "micro-portrait") == 0 ||
                strcmp(value, "tiny") == 0 || strcmp(value, "tiny-portrait") == 0) {
                args.layout = value;
                spdlog::info("[CLI] Layout override: {}", args.layout);
            } else {
                printf("Error: invalid --layout value: %s\n", value);
                printf("Valid values: auto, standard, ultrawide, portrait, micro, "
                       "micro-portrait, tiny, tiny-portrait\n");
                return false;
            }
        } else if (strcmp(argv[i], "--real-wifi") == 0) {
            config.use_real_wifi = true;
        } else if (strcmp(argv[i], "--real-ethernet") == 0) {
            config.use_real_ethernet = true;
        } else if (strcmp(argv[i], "--real-moonraker") == 0) {
            config.use_real_moonraker = true;
        } else if (strcmp(argv[i], "--real-files") == 0) {
            config.use_real_files = true;
        } else if (strcmp(argv[i], "--real-sensors") == 0) {
            config.use_real_sensors = true;
        } else if (strcmp(argv[i], "--disconnected") == 0) {
            config.simulate_disconnect = true;
        } else if (strcmp(argv[i], "--no-ams") == 0) {
            config.disable_mock_ams = true;
        } else if (strcmp(argv[i], "--test-history") == 0) {
            config.test_history_api = true;
        } else if (strcmp(argv[i], "--sim-speed") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --sim-speed requires a speedup factor (1.0-1000.0)\n");
                return false;
            }
            double val;
            if (!parse_double(argv[++i], val, "--sim-speed"))
                return false;
            if (val < 1.0 || val > 1000.0) {
                printf("Error: --sim-speed must be 1.0-1000.0\n");
                return false;
            }
            config.sim_speedup = val;
        }
        // Select file
        else if (strcmp(argv[i], "--select-file") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --select-file requires a filename argument\n");
                return false;
            }
            config.select_file = argv[++i];
        }
        // G-code options
        else if (strcmp(argv[i], "--gcode-file") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --gcode-file requires a path argument\n");
                return false;
            }
            config.gcode_test_file = argv[++i];
        } else if (strcmp(argv[i], "--gcode-az") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --gcode-az requires a numeric argument\n");
                return false;
            }
            double val;
            if (!parse_double(argv[++i], val, "--gcode-az"))
                return false;
            config.gcode_camera_azimuth = static_cast<float>(val);
            config.gcode_camera_azimuth_set = true;
        } else if (strcmp(argv[i], "--gcode-el") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --gcode-el requires a numeric argument\n");
                return false;
            }
            double val;
            if (!parse_double(argv[++i], val, "--gcode-el"))
                return false;
            config.gcode_camera_elevation = static_cast<float>(val);
            config.gcode_camera_elevation_set = true;
        } else if (strcmp(argv[i], "--gcode-zoom") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --gcode-zoom requires a numeric argument\n");
                return false;
            }
            double val;
            if (!parse_double(argv[++i], val, "--gcode-zoom"))
                return false;
            if (val <= 0) {
                printf("Error: --gcode-zoom requires a positive numeric value\n");
                return false;
            }
            config.gcode_camera_zoom = static_cast<float>(val);
            config.gcode_camera_zoom_set = true;
        } else if (strcmp(argv[i], "--gcode-debug-colors") == 0) {
            config.gcode_debug_colors = true;
        } else if (strcmp(argv[i], "--render-2d") == 0) {
            config.gcode_render_mode = 2; // GcodeViewerRenderMode::Layer2D
        } else if (strcmp(argv[i], "--render-3d") == 0) {
            config.gcode_render_mode = 1; // GcodeViewerRenderMode::Render3D
        } else if (strcmp(argv[i], "--camera") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --camera requires a string argument\n");
                printf("Format: --camera \"az:90.5,el:4.0,zoom:15.5\"\n");
                return false;
            }
            if (!parse_camera_arg(argv[++i], config))
                return false;
        }
        // Verbosity
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-vv") == 0 ||
                 strcmp(argv[i], "-vvv") == 0) {
            const char* p = argv[i];
            while (*p == '-')
                p++;
            while (*p == 'v') {
                args.verbosity++;
                p++;
            }
        } else if (strcmp(argv[i], "--verbose") == 0) {
            args.verbosity++;
        }
        // Memory profiling (development)
        else if (strcmp(argv[i], "--memory-report") == 0 || strcmp(argv[i], "-M") == 0) {
            args.memory_report = true;
        } else if (strcmp(argv[i], "--show-memory") == 0) {
            args.show_memory = true;
        } else if (strcmp(argv[i], "--mock-crash") == 0) {
            config.mock_crash = true;
        } else if (strcmp(argv[i], "--release-notes") == 0) {
            args.overlays.release_notes = true;
        } else if (strcmp(argv[i], "--debug-subjects") == 0) {
            RuntimeConfig::set_debug_subjects(true);
        } else if (strcmp(argv[i], "--debug-touches") == 0) {
            RuntimeConfig::set_debug_touches(true);
        }
        // Moonraker URL override
        else if (strcmp(argv[i], "--moonraker") == 0 || strncmp(argv[i], "--moonraker=", 12) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--moonraker=", 12) == 0) {
                value = argv[i] + 12;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --moonraker requires a URL argument\n");
                return false;
            }
            args.moonraker_url = value;
            // Normalize: accept either host:port or full ws:// URL
            if (args.moonraker_url.find("://") == std::string::npos) {
                // Assume ws:// scheme if not provided
                args.moonraker_url = "ws://" + args.moonraker_url;
            }
            // Append /websocket if not present
            if (args.moonraker_url.find("/websocket") == std::string::npos) {
                args.moonraker_url += "/websocket";
            }
        }
        // Log destination
        else if (strcmp(argv[i], "--log-dest") == 0 || strncmp(argv[i], "--log-dest=", 11) == 0) {
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
        } else if (strcmp(argv[i], "--log-level") == 0 ||
                   strncmp(argv[i], "--log-level=", 12) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--log-level=", 12) == 0) {
                value = argv[i] + 12;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --log-level requires an argument\n");
                return false;
            }
            g_log_level_cli = value;
            if (g_log_level_cli != "trace" && g_log_level_cli != "debug" &&
                g_log_level_cli != "info" && g_log_level_cli != "warn" &&
                g_log_level_cli != "error" && g_log_level_cli != "critical" &&
                g_log_level_cli != "off") {
                printf("Error: invalid --log-level value: %s\n", g_log_level_cli.c_str());
                printf("Valid values: trace, debug, info, warn, error, critical, off\n");
                return false;
            }
        }
        // Help
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return false;
        }
        // Version
        else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("helix-screen %s\n", helix_version_full());
            return false;
        }
        // Legacy: first positional arg is panel name
        else if (i == 1 && argv[i][0] != '-') {
            const char* panel_arg = argv[i];
            args.panel_requested = true;
            if (strcmp(panel_arg, "motion") == 0) {
                args.initial_panel = static_cast<int>(PanelId::Controls);
                args.overlays.motion = true;
            } else if (strcmp(panel_arg, "step-test") == 0 || strcmp(panel_arg, "step_test") == 0) {
                args.overlays.step_test = true;
            } else {
                auto panel_id = panel_name_to_id(panel_arg);
                if (panel_id) {
                    args.initial_panel = static_cast<int>(*panel_id);
                } else {
                    printf("Unknown argument: %s\n", argv[i]);
                    printf("Use --help for usage information\n");
                    return false;
                }
            }
        }
        // Unknown argument — warn but continue (don't crash-loop on forward-compat flags)
        else {
            fprintf(stderr, "Warning: ignoring unknown argument: %s\n", argv[i]);
        }
    }

    // Validate test mode flags
    if ((config.use_real_wifi || config.use_real_ethernet || config.use_real_moonraker ||
         config.use_real_files || config.use_real_sensors) &&
        !config.test_mode) {
        printf("Error: --real-* flags require --test mode\n");
        printf("Use --help for more information\n");
        return false;
    }

    if (config.gcode_test_file && !config.test_mode) {
        printf("Error: --gcode-file requires --test mode\n");
        return false;
    }

    if (config.simulate_disconnect && !config.test_mode) {
        printf("Error: --disconnected requires --test mode\n");
        return false;
    }

    if (config.mock_crash && !config.test_mode) {
        printf("Error: --mock-crash requires --test mode\n");
        return false;
    }

    // Print test mode banner if enabled
    if (config.test_mode) {
        print_test_mode_banner();
    }

    return true;
}

} // namespace helix
