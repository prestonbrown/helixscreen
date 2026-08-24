// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cli_args.h"

#include "app_globals.h"
#include "config.h"
#include "helix_version.h"
#include "logging_init.h"
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

    // Resolved, not the raw constant: with HELIX_CONFIG_DIR set the app reads
    // and writes the override path, and a banner naming config/ instead sends
    // you editing a file the run never touches.
    printf("  Config: %s\n", Config::resolve_path(RuntimeConfig::TEST_CONFIG_PATH).c_str());

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
    printf("  -w, --wizard         Force first-run configuration wizard (overrides the\n");
    printf("                       --skip-wizard that --test implies)\n");
    printf("  --wizard-step <step> Jump to specific wizard step for testing\n");
    printf("  --skip-wizard        Suppress the first-run wizard (implied by --test)\n");
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
    printf("  --no-sound           Disable all sound output (prevents audio backend init)\n");
    printf("  --moonraker <url>    Override Moonraker URL (e.g., ws://192.168.1.100:7125)\n");
    printf("  --detect-printer     Detect printer via Moonraker REST, print JSON, exit\n");
    printf("                       (use with --host/--port; default 127.0.0.1:7125)\n");
    printf("  --remote             Enable remote control server (auto in --test mode)\n");
    printf("  --remote-socket <p>  Override remote control socket path\n");
    printf("  --remote-transport <t>  Transport: socket (default) or http\n");
    printf("  --remote-http-bind <h>  HTTP bind host (default 127.0.0.1; implies http)\n");
    printf("  --remote-http-port <n>  HTTP port (default 7130; implies http)\n");
    printf("  --rotate <degrees>   Display rotation: 0, 90, 180, 270\n");
    printf("  --render-2d          Force the G-code viewer to the 2D layer renderer\n");
    printf("  --render-3d          Force the G-code viewer to the 3D GLES renderer\n");
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
    printf("    --real-ams         Use real AMS backend (requires --test)\n");
    printf("    --real-sensors     Use real sensor data (requires --test)\n");
    printf("    --disconnected     Simulate disconnected state (requires --test)\n");
    printf("    --no-ams           Don't create mock AMS (enables runout modal testing)\n");
    printf("    --test-history     Enable test history API data\n");
    printf("    --sim-speed <n>    Simulation speedup factor (1.0-1000.0, e.g., 100 for 100x)\n");
    printf("    --mock-crash       Write synthetic crash.txt to test crash reporter UI\n");
    printf("    --select-file <name>  Auto-select file in print-select panel\n");
    printf("\nG-code Viewer Options (require --test):\n");
    printf("  --gcode-file <path>  Preload a G-code file for the 3D print viewer (--test)\n");
    printf("  --camera <params>    Set camera params: \"az:90.5,el:4.0,zoom:15.5\"\n");
    printf("  --gcode-az <deg>     Set camera azimuth angle (degrees)\n");
    printf("  --gcode-el <deg>     Set camera elevation angle (degrees)\n");
    printf("  --gcode-zoom <n>     Set camera zoom level (positive number)\n");
    printf("  --gcode-debug-colors Enable per-face debug coloring\n");
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

bool parse_screen_size_string(const char* size_str, int& out_width, int& out_height,
                              ScreenSize& out_size) {
    if (strcmp(size_str, "micro") == 0) {
        out_width = UI_SCREEN_MICRO_W;
        out_height = UI_SCREEN_MICRO_H;
        out_size = ScreenSize::MICRO;
    } else if (strcmp(size_str, "tiny") == 0) {
        out_width = UI_SCREEN_TINY_W;
        out_height = UI_SCREEN_TINY_H;
        out_size = ScreenSize::TINY;
    } else if (strcmp(size_str, "small") == 0) {
        out_width = UI_SCREEN_SMALL_W;
        out_height = UI_SCREEN_SMALL_H;
        out_size = ScreenSize::SMALL;
    } else if (strcmp(size_str, "medium") == 0) {
        out_width = UI_SCREEN_MEDIUM_W;
        out_height = UI_SCREEN_MEDIUM_H;
        out_size = ScreenSize::MEDIUM;
    } else if (strcmp(size_str, "large") == 0) {
        out_width = UI_SCREEN_LARGE_W;
        out_height = UI_SCREEN_LARGE_H;
        out_size = ScreenSize::LARGE;
    } else if (strcmp(size_str, "xlarge") == 0) {
        out_width = UI_SCREEN_XLARGE_W;
        out_height = UI_SCREEN_XLARGE_H;
        out_size = ScreenSize::XLARGE;
    } else {
        int w = 0, h = 0;
        if (sscanf(size_str, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            out_width = w;
            out_height = h;
            if (std::min(w, h) <= UI_SCREEN_MICRO_H && std::max(w, h) <= UI_SCREEN_TINY_W) {
                out_size = ScreenSize::MICRO;
            } else if (h <= UI_BREAKPOINT_TINY_MAX) {
                out_size = ScreenSize::TINY;
            } else if (h <= UI_BREAKPOINT_SMALL_MAX) {
                out_size = ScreenSize::SMALL;
            } else if (h <= UI_BREAKPOINT_MEDIUM_MAX) {
                out_size = ScreenSize::MEDIUM;
            } else if (h <= UI_BREAKPOINT_LARGE_MAX) {
                out_size = ScreenSize::LARGE;
            } else {
                out_size = ScreenSize::XLARGE;
            }
        } else {
            return false;
        }
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
            if (!parse_screen_size_string(size_arg, screen_width, screen_height,
                                          args.screen_size)) {
                printf("Unknown screen size: %s\n", size_arg);
                printf("Available sizes: micro, tiny, small, medium, large, xlarge (or WxH "
                       "like 480x400)\n");
                return false;
            }
            args.size_was_explicit = true;
        }
        // Wizard
        else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--wizard") == 0) {
            args.force_wizard = true;
        } else if (strcmp(argv[i], "--skip-wizard") == 0) {
            args.skip_wizard = true;
        }
        // Touch calibration
        else if (strcmp(argv[i], "--calibrate-touch") == 0) {
            args.calibrate_touch = true;
        }
        // Wizard step
        else if (strcmp(argv[i], "--wizard-step") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --wizard-step requires an argument (0-12)\n");
                return false;
            }
            args.wizard_step = atoi(argv[++i]);
            args.force_wizard = true;
            if (args.wizard_step < 0 || args.wizard_step > 12) {
                printf("Error: wizard step must be 0-12\n");
                return false;
            }
        }
        // Headless printer detection
        else if (strcmp(argv[i], "--detect-printer") == 0) {
            args.detect_printer = true;
        } else if (strcmp(argv[i], "--host") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --host requires an argument\n");
                return false;
            }
            args.detect_host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                printf("Error: --port requires an argument\n");
                return false;
            }
            args.detect_port = atoi(argv[++i]);
            if (args.detect_port <= 0 || args.detect_port > 65535) {
                printf("Error: --port must be 1-65535\n");
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
        } else if (strcmp(argv[i], "--real-ams") == 0) {
            config.use_real_ams = true;
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
            args.release_notes = true;
        } else if (strcmp(argv[i], "--no-sound") == 0) {
            config.disable_sound = true;
        } else if (strcmp(argv[i], "--debug-subjects") == 0) {
            RuntimeConfig::set_debug_subjects(true);
        } else if (strcmp(argv[i], "--debug-touches") == 0 ||
                   strcmp(argv[i], "--debug-touch") == 0) {
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
        // Remote control
        else if (strcmp(argv[i], "--remote") == 0) {
            args.remote_control = true;
        } else if (strcmp(argv[i], "--remote-socket") == 0 ||
                   strncmp(argv[i], "--remote-socket=", 16) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--remote-socket=", 16) == 0) {
                value = argv[i] + 16;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --remote-socket requires a path argument\n");
                return false;
            }
            args.remote_socket = value;
            args.remote_control = true; // Implies --remote
        } else if (strcmp(argv[i], "--remote-transport") == 0 ||
                   strncmp(argv[i], "--remote-transport=", 19) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--remote-transport=", 19) == 0) {
                value = argv[i] + 19;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --remote-transport requires socket|http\n");
                return false;
            }
            if (strcmp(value, "socket") != 0 && strcmp(value, "http") != 0) {
                printf("Error: --remote-transport must be 'socket' or 'http'\n");
                return false;
            }
            args.remote_transport = value;
            args.remote_control = true; // Implies --remote
        } else if (strcmp(argv[i], "--remote-http-bind") == 0 ||
                   strncmp(argv[i], "--remote-http-bind=", 19) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--remote-http-bind=", 19) == 0) {
                value = argv[i] + 19;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --remote-http-bind requires a host argument\n");
                return false;
            }
            args.remote_http_bind = value;
            args.remote_transport = "http"; // Selecting an HTTP option implies http
            args.remote_control = true;
        } else if (strcmp(argv[i], "--remote-http-port") == 0 ||
                   strncmp(argv[i], "--remote-http-port=", 19) == 0) {
            const char* value = nullptr;
            if (strncmp(argv[i], "--remote-http-port=", 19) == 0) {
                value = argv[i] + 19;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                printf("Error: --remote-http-port requires a port argument\n");
                return false;
            }
            int port = atoi(value);
            if (port < 1 || port > 65535) {
                printf("Error: --remote-http-port must be 1..65535 (got '%s')\n", value);
                return false;
            }
            args.remote_http_port = port;
            args.remote_transport = "http"; // Selecting an HTTP option implies http
            args.remote_control = true;
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
            // Shared with the HELIX_LOG_DEST reader in Application::init_logging()
            // so the accepted set cannot drift between the flag and the env var.
            if (!helix::logging::is_valid_log_target(g_log_dest_cli)) {
                printf("Error: invalid --log-dest value: %s\n", g_log_dest_cli.c_str());
                printf("Valid values: %s\n", helix::logging::log_target_accepted_values());
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
            // Shared with the HELIX_LOG_LEVEL reader in Application::init_logging().
            if (!helix::logging::is_valid_log_level(g_log_level_cli)) {
                printf("Error: invalid --log-level value: %s\n", g_log_level_cli.c_str());
                printf("Valid values: %s\n", helix::logging::log_level_accepted_values());
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
        // Unknown argument — warn but continue (don't crash-loop on forward-compat flags)
        else {
            fprintf(stderr, "Warning: ignoring unknown argument: %s\n", argv[i]);
        }
    }

    // Validate test mode flags
    if ((config.use_real_wifi || config.use_real_ethernet || config.use_real_moonraker ||
         config.use_real_files || config.use_real_ams || config.use_real_sensors) &&
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

    // --test implies --skip-wizard. Mock mode sets the active printer to
    // "mock-printer", and the wizard gate reads
    // printers.<active>.wizard_completed — a key no real settings.json carries,
    // so a mock boot always landed on the wizard and silently swallowed every
    // ctl navigate/click. Applied after the full parse so flag order does not
    // matter; -w/--wizard still wins, which is how the screenshot pipeline
    // captures the wizard itself.
    if (config.test_mode && !args.force_wizard) {
        args.skip_wizard = true;
    }

    // Print test mode banner if enabled
    if (config.test_mode) {
        print_test_mode_banner();
    }

    return true;
}

} // namespace helix
