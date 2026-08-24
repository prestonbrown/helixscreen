// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file helix_splash.cpp
 * @brief Minimal splash screen binary for embedded targets
 *
 * This is a lightweight splash screen that starts instantly while the main
 * helix-screen application initializes in parallel. It displays the
 * HelixScreen logo with a fade-in animation (on capable hardware) and
 * automatically exits when the main app takes over the framebuffer.
 *
 * Design goals:
 * - Minimal dependencies (LVGL + display backend only, no libhv/spdlog/etc)
 * - Fast startup (~50ms to first frame)
 * - Automatic handoff when main app opens display
 * - Graceful exit on SIGTERM/SIGINT
 *
 * For desktop development, the main app uses its own splash screen.
 * This binary is only built and used on embedded Linux targets.
 */

#include "backlight_backend.h"
#include "data_root_resolver.h"
#include "display_backend.h"
#include "helix_version.h"
#include "splash_status.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <lvgl.h>
#include <memory>
#include <regex>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Signal handling for graceful shutdown
// SIGTERM/SIGINT: graceful shutdown (e.g., system shutdown)
// SIGUSR1: main app is ready, hand off display immediately
static volatile sig_atomic_t g_quit = 0;
// Set only by SIGUSR1 (app-ready handoff). Distinguishes a handoff exit — where
// the successor (helix-screen's fb0 mirror) takes over /dev/fb0 — from a
// shutdown/backstop exit. On handoff we must NOT clear fb0 (see teardown below).
static volatile sig_atomic_t g_ready = 0;

// Define the LVGL assert callback pointer for splash binary
// (normally defined in logging_init.cpp, but splash doesn't link that)
#include "lvgl_assert_handler.h"
helix_assert_callback_t g_helix_assert_cpp_callback = nullptr;

static void signal_handler(int sig) {
    // SIGUSR1 is the app-ready handoff: the successor owns fb0, so mark ready so
    // teardown skips the framebuffer clear. SIGTERM/SIGINT are shutdown/backstop
    // exits and leave g_ready unset, so teardown clears fb0 as before.
    if (sig == SIGUSR1) {
        g_ready = 1;
    }
    g_quit = 1;
}

// Default screen dimensions (can be overridden via command line)
static constexpr int DEFAULT_WIDTH = 800;
static constexpr int DEFAULT_HEIGHT = 480;

// Splash timing
static constexpr int FADE_DURATION_MS = 1000; // Fade-in duration
static constexpr int FRAME_DELAY_US = 16000;  // ~60 FPS

// Defense-in-depth self-timeout. helix-screen normally signals SIGUSR1 once
// discovery completes (or its 8s timeout fires); the watchdog also reaps us
// on exit. If both paths fail (e.g. helix-screen never received --splash-pid
// because the watchdog skipped adoption on DRM), splash would otherwise spin
// forever. Cap our own lifetime so any upstream failure is bounded.
//
// On slow devices the init-script Moonraker gate runs BEFORE helix-screen
// launches and can exceed this cap, so the gate writes a heartbeat/status file
// that extends our lifetime while it is actively progressing (see
// include/splash_status.h). Absent that file we keep the legacy fixed cap.
static constexpr int MAX_LIFETIME_SEC = 30;

// Path of the boot-status file the launcher/init gate writes while it waits.
// Each rewrite is a heartbeat; the first line is the message we display.
static std::string splash_status_path() {
    if (const char* p = getenv("HELIX_SPLASH_STATUS_FILE")) {
        if (*p != '\0') {
            return p;
        }
    }
    return "/tmp/helix-splash-status";
}

// Free-memory floor (KiB) below which the splash voluntarily exits so it can
// never be the process that tips a tight-RAM device into OOM. Overridable per
// platform via HELIX_SPLASH_MIN_FREE_KB; 0 disables the valve.
static long splash_min_free_kb() {
    if (const char* p = getenv("HELIX_SPLASH_MIN_FREE_KB")) {
        char* end = nullptr;
        const long v = strtol(p, &end, 10);
        if (end != p && v >= 0) {
            return v;
        }
    }
    return 8192; // ~8 MiB
}

// Read brightness from config file (simple parsing, no JSON library)
// Returns configured brightness (10-100) or default_value on failure
static int read_config_brightness(int default_value = 100) {
    // writable_path honors HELIX_CONFIG_DIR (Yocto) or "config/" (tarball);
    // legacy paths are kept for old install layouts.
    const std::string main_settings = helix::writable_path("settings.json");
    const std::string main_legacy = helix::writable_path("helixconfig.json");
    const std::string paths[] = {main_settings, main_legacy, "helixconfig.json",
                                 "/opt/helixscreen/helixconfig.json"};

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // Simple regex to find "brightness": <number>
        std::regex brightness_regex(R"("brightness"\s*:\s*(\d+))");
        std::smatch match;
        if (std::regex_search(content, match, brightness_regex) && match.size() > 1) {
            int brightness = std::stoi(match[1].str());
            // Clamp to valid range
            if (brightness < 10)
                brightness = 10;
            if (brightness > 100)
                brightness = 100;
            return brightness;
        }
    }

    return default_value;
}

// Background colors for each mode
static constexpr uint32_t BG_COLOR_DARK = 0x121212;    // App theme dark background
static constexpr uint32_t BG_COLOR_3D_DARK = 0x2D2D2D; // 3D splash dark (sampled from image edges)
static constexpr uint32_t BG_COLOR_3D_LIGHT =
    0xDBDBDF; // 3D splash light (sampled from image edges)

// Read dark_mode setting from config file (same parsing approach as brightness)
// Returns configured value or default_value on failure
static bool read_config_dark_mode(bool default_value = true) {
    const std::string main_settings = helix::writable_path("settings.json");
    const std::string main_legacy = helix::writable_path("helixconfig.json");
    const std::string paths[] = {main_settings, main_legacy, "helixconfig.json",
                                 "/opt/helixscreen/helixconfig.json"};

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // Simple regex to find "dark_mode": true/false
        std::regex dark_mode_regex(R"("dark_mode"\s*:\s*(true|false))");
        std::smatch match;
        if (std::regex_search(content, match, dark_mode_regex) && match.size() > 1) {
            bool result = (match[1].str() == "true");
            fprintf(stderr, "helix-splash: dark_mode=%s (from %s)\n", result ? "true" : "false",
                    path.c_str());
            return result;
        }
    }

    return default_value;
}

// Get size name for a screen width (matches prerendered_images.cpp logic)
static const char* get_splash_3d_size_name(int screen_width, int screen_height) {
    // Ultra-wide displays (e.g. 1920x440): wide but very short
    if (screen_width >= 1100 && screen_height < 500)
        return "ultrawide";
    if (screen_width < 600) {
        // Distinguish K1 (480x400) from generic tiny (480x320)
        return (screen_height >= 380) ? "tiny_alt" : "tiny";
    }
    if (screen_width < 900)
        return "small";
    if (screen_width < 1100)
        return "medium";
    return "large";
}

// Known heights for pre-rendered splash images (from gen_splash_3d.py SCREEN_SIZES)
static int get_splash_3d_target_height(const char* size_name) {
    if (strcmp(size_name, "tiny") == 0)
        return 320;
    if (strcmp(size_name, "tiny_alt") == 0)
        return 400;
    if (strcmp(size_name, "small") == 0)
        return 480;
    if (strcmp(size_name, "medium") == 0)
        return 600;
    if (strcmp(size_name, "large") == 0)
        return 720;
    if (strcmp(size_name, "ultrawide") == 0)
        return 440;
    return 0;
}

/**
 * @brief Parse command line arguments
 */
static void parse_args(int argc, char** argv, int& width, int& height, int& rotation) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            rotation = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: helix-splash [-w width] [-h height] [-r rotation]\n");
            printf("  -w <width>    Screen width (default: %d)\n", DEFAULT_WIDTH);
            printf("  -h <height>   Screen height (default: %d)\n", DEFAULT_HEIGHT);
            printf("  -r <degrees>  Display rotation: 0, 90, 180, 270 (default: from config)\n");
            exit(0);
        }
    }
}

/**
 * @brief Animation callback for fade-in effect
 */
static void fade_anim_cb(void* obj, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), value, LV_PART_MAIN);
}

/**
 * @brief Create and configure the splash screen UI
 *
 * Tries 3D full-screen splash first (dark/light based on config),
 * falls back to pre-rendered logo, then to PNG with runtime scaling.
 */
static lv_obj_t* create_splash_ui(lv_obj_t* screen, int width, int height, bool dark_mode,
                                  bool use_fade) {
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // Try full-screen 3D splash first
    const char* size_name = get_splash_3d_size_name(width, height);
    const char* mode_name = dark_mode ? "dark" : "light";

    // Build path to 3D splash image
    char splash_3d_path[128];
    snprintf(splash_3d_path, sizeof(splash_3d_path),
             "assets/images/prerendered/splash-3d-%s-%s.bin", mode_name, size_name);

    struct stat st;
    bool use_3d = (stat(splash_3d_path, &st) == 0);

    // Fallback: try base "tiny" if tiny_alt not found
    if (!use_3d && strcmp(size_name, "tiny_alt") == 0) {
        snprintf(splash_3d_path, sizeof(splash_3d_path),
                 "assets/images/prerendered/splash-3d-%s-tiny.bin", mode_name);
        use_3d = (stat(splash_3d_path, &st) == 0);
    }

    // Also check for 3D source PNG fallback
    char splash_3d_png[128];
    bool use_3d_png = false;
    if (!use_3d) {
        snprintf(splash_3d_png, sizeof(splash_3d_png), "assets/images/helixscreen-logo-3d-%s.png",
                 mode_name);
        use_3d_png = (stat(splash_3d_png, &st) == 0);
    }

    // Safety: skip pre-rendered .bin if it would be taller than the screen
    if (use_3d) {
        int target_h = get_splash_3d_target_height(size_name);
        if (target_h > 0 && target_h > height) {
            fprintf(stderr,
                    "helix-splash: Pre-rendered %s (%dpx) exceeds screen height %dpx, "
                    "falling back to PNG\n",
                    size_name, target_h, height);
            use_3d = false;
        }
    }

    if (use_3d || use_3d_png) {
        // 3D splash: prerendered bin (full-screen) or source PNG (centered + scaled)
        uint32_t bg_color = dark_mode ? BG_COLOR_3D_DARK : BG_COLOR_3D_LIGHT;
        lv_obj_set_style_bg_color(screen, lv_color_hex(bg_color), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        lv_obj_t* img = lv_image_create(screen);
        lv_obj_set_style_bg_opa(img, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(img, 0, LV_PART_MAIN);

        if (use_3d) {
            // Prerendered bin: full-screen, no scaling needed
            char lvgl_path[140];
            snprintf(lvgl_path, sizeof(lvgl_path), "A:%s", splash_3d_path);
            lv_image_set_src(img, lvgl_path);
            fprintf(stderr, "helix-splash: Using 3D splash (%s, %s, fade=%s)\n", mode_name,
                    size_name, use_fade ? "yes" : "no");
        } else {
            // Source PNG fallback: scale to fit screen width
            char lvgl_path[140];
            snprintf(lvgl_path, sizeof(lvgl_path), "A:%s", splash_3d_png);
            lv_image_set_src(img, lvgl_path);

            lv_image_header_t header;
            if (lv_image_decoder_get_info(lvgl_path, &header) == LV_RESULT_OK && header.w > 0 &&
                header.h > 0) {
                // Fit to screen with 10% vertical margin (5% top + 5% bottom)
                int usable_height = (height * 9) / 10;
                int scale_w = (width * 256) / header.w;
                int scale_h = (usable_height * 256) / header.h;
                int scale = (scale_w < scale_h) ? scale_w : scale_h;
                lv_image_set_scale(img, scale);
                fprintf(stderr, "helix-splash: Using 3D PNG fallback (%s, %dx%d scale=%d)\n",
                        mode_name, (int)header.w, (int)header.h, scale);
            } else {
                fprintf(stderr, "helix-splash: 3D PNG loaded but could not get dimensions\n");
            }
        }

        lv_obj_center(img);

        if (use_fade) {
            lv_obj_set_style_opa(img, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_anim_t anim;
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, img);
            lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
            lv_anim_set_duration(&anim, FADE_DURATION_MS);
            lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
            lv_anim_set_exec_cb(&anim, fade_anim_cb);
            lv_anim_start(&anim);
        } else {
            lv_obj_set_style_opa(img, LV_OPA_COVER, LV_PART_MAIN);
        }
        return img;
    }

    // Fallback: original centered logo approach
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG_COLOR_DARK), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Create container for logo (will be animated)
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(container, LV_OPA_TRANSP, LV_PART_MAIN); // Start invisible for fade-in
    lv_obj_center(container);

    // Create logo image
    lv_obj_t* logo = lv_image_create(container);

    // Ensure image widget has no visible background/border (fix edge artifact)
    lv_obj_set_style_bg_opa(logo, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(logo, 0, LV_PART_MAIN);

    // Check for pre-rendered logo image (centered, not full-screen)
    const char* prerendered_path = "assets/images/prerendered/splash-logo-small.bin";
    bool use_prerendered = (stat(prerendered_path, &st) == 0);

    if (use_prerendered) {
        // Pre-rendered: instant display, no scaling needed!
        lv_image_set_src(logo, "A:assets/images/prerendered/splash-logo-small.bin");
        fprintf(stderr, "helix-splash: Using pre-rendered splash (fast path)\n");
    } else {
        // PNG fallback with runtime scaling (slow but works)
        const char* logo_path = "A:assets/images/helixscreen-logo.png";
        lv_image_set_src(logo, logo_path);
        fprintf(stderr, "helix-splash: Using PNG fallback (slow path)\n");

        // Scale logo to 50% of screen width, but constrain by height too
        lv_image_header_t header;
        if (lv_image_decoder_get_info(logo_path, &header) == LV_RESULT_OK) {
            int target_size = width / 2;
            int scale_w = (target_size * 256) / header.w;
            int usable_h = (height * 9) / 10; // 10% vertical margin
            int scale_h = (usable_h * 256) / header.h;
            int scale = (scale_w < scale_h) ? scale_w : scale_h;
            lv_image_set_scale(logo, scale);
        } else {
            lv_image_set_scale(logo, 128); // Fallback: 50%
        }
    }

    if (use_fade) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, container);
        lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&anim, FADE_DURATION_MS);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&anim, fade_anim_cb);
        lv_anim_start(&anim);
    } else {
        lv_obj_set_style_opa(container, LV_OPA_COVER, LV_PART_MAIN);
    }

    return container;
}

int main(int argc, char** argv) {
    // Set up signal handlers
    // SIGTERM/SIGINT: graceful shutdown
    // SIGUSR1: main app ready, hand off display
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGUSR1, signal_handler);

    // Parse command line arguments (CLI overrides auto-detection)
    int width = 0;
    int height = 0;
    int rotation = 0;
    parse_args(argc, argv, width, height, rotation);

    // Initialize LVGL
    lv_init();

    // Force fbdev for splash to avoid DRM master contention.
    // On DRM systems, only one process can hold the master lease. If splash takes it,
    // helix-screen can't flush frames until splash dies. Using fbdev avoids this
    // entirely since fbdev has no master concept.
    // On DRM-only systems (no /dev/fb0), skip the splash entirely — the main binary
    // will start faster without a splash blocking DRM master acquisition.
    auto backend = DisplayBackend::create(DisplayBackendType::FBDEV);
    if (!backend) {
        fprintf(stderr, "helix-splash: No framebuffer available, skipping splash "
                        "(DRM-only systems don't support concurrent splash)\n");
        return 0;
    }

    // Auto-detect resolution from display hardware if not overridden via CLI
    if (width == 0 || height == 0) {
        auto res = backend->detect_resolution();
        if (res.valid) {
            width = res.width;
            height = res.height;
            fprintf(stderr, "helix-splash: Auto-detected resolution: %dx%d\n", width, height);
        } else {
            width = DEFAULT_WIDTH;
            height = DEFAULT_HEIGHT;
            fprintf(stderr, "helix-splash: Using default resolution: %dx%d\n", width, height);
        }
    }

    // Unblank display via framebuffer ioctl BEFORE creating LVGL display.
    // Essential on AD5M where ForgeX may have blanked the display during boot.
    // Uses same approach as GuppyScreen: FBIOBLANK + FBIOPAN_DISPLAY.
    if (backend->unblank_display()) {
        fprintf(stderr, "helix-splash: Display unblanked via framebuffer ioctl\n");
    }

    // Create display
    lv_display_t* display = backend->create_display(width, height);
    if (!display) {
        fprintf(stderr, "helix-splash: Failed to create display\n");
        return 1;
    }

    // Apply display rotation if configured (CLI arg from watchdog, or config fallback)
    if (rotation == 0) {
        rotation = read_config_rotation(0);
    }
    // Auto-detect from kernel if no config/CLI rotation (first boot).
    // panel_orientation is informational — kernel does NOT rotate the
    // framebuffer, we must do it ourselves.
    if (rotation == 0) {
        int kernel_rot = detect_panel_orientation_from_cmdline();
        if (kernel_rot > 0) {
            rotation = kernel_rot;
            fprintf(stderr, "helix-splash: Auto-detected panel orientation: %d°\n", rotation);
        }
    }
    if (rotation != 0) {
        lv_display_set_rotation(display, degrees_to_lv_rotation(rotation));
        // Update dimensions to match rotated resolution for splash layout
        width = lv_display_get_horizontal_resolution(display);
        height = lv_display_get_vertical_resolution(display);
        fprintf(stderr, "helix-splash: Display rotated %d° — effective resolution: %dx%d\n",
                rotation, width, height);
    }

    // Splash-only: force FULL render mode. lv_linux_fbdev_create() defaults to
    // the compile-time global LV_LINUX_FBDEV_RENDER_MODE (PARTIAL — 60-line
    // stripes), which on a slow panel (e.g. the K2's Allwinner fb) makes the
    // logo visibly wipe down the screen one stripe at a time, even rendered
    // back-to-back via lv_refr_now(). A full-screen off-screen buffer +
    // RENDER_MODE_FULL renders the whole frame, then flushes it to /dev/fb0 in a
    // single pass — one clean fill, no striping. This replaces only THIS
    // display's buffers (the partial ones lv_linux_fbdev_create just allocated);
    // the global render mode that helix-screen shares is untouched. Sized to the
    // post-rotation resolution so a rotated panel still gets a correctly-sized
    // buffer (fbdev's flush_cb software-rotates a FULL frame in one shot).
    if (backend->type() == DisplayBackendType::FBDEV) {
        const lv_color_format_t cf = lv_display_get_color_format(display);
        const uint32_t px_size = lv_color_format_get_size(cf);
        const int32_t hor = lv_display_get_horizontal_resolution(display);
        const int32_t ver = lv_display_get_vertical_resolution(display);
        const size_t full_buf_size = (size_t)hor * (size_t)ver * px_size;
        // Over-allocate by LV_DRAW_BUF_ALIGN-1 and hand LVGL the aligned pointer,
        // mirroring lv_linux_fbdev_create()'s own allocation. Intentionally never
        // freed: the splash is a short-lived process that exits wholesale.
        void* raw = malloc(full_buf_size + LV_DRAW_BUF_ALIGN - 1);
        if (raw != nullptr) {
            uint8_t* aligned = static_cast<uint8_t*>(lv_draw_buf_align(raw, cf));
            lv_display_set_buffers(display, aligned, nullptr, full_buf_size,
                                   LV_DISPLAY_RENDER_MODE_FULL);
            fprintf(stderr,
                    "helix-splash: FULL render mode (%dx%d, %ubpp, %zu KiB) — single-flush "
                    "paint\n",
                    hor, ver, px_size * 8, full_buf_size / 1024);
        } else {
            fprintf(stderr, "helix-splash: FULL-mode buffer alloc failed; keeping PARTIAL\n");
        }
    }

    // Read dark mode preference from config (before framebuffer clear so we use the right color)
    bool dark_mode = read_config_dark_mode(true);

    // Clear framebuffer to remove any pre-existing content (Linux console text)
    // This must happen AFTER create_display (which opens the framebuffer)
    // but BEFORE we render the splash UI
    // Use 3D splash bg color if available, otherwise dark theme bg
    uint32_t clear_color = dark_mode ? BG_COLOR_3D_DARK : BG_COLOR_3D_LIGHT;
    if (backend->clear_framebuffer(clear_color | 0xFF000000)) {
        fprintf(stderr, "helix-splash: Framebuffer cleared\n");
    }

    // Turn on backlight immediately (may have been off from sleep or crash)
    // Use configured brightness instead of hardcoded 100%
    auto backlight = BacklightBackend::create();
    if (backlight && backlight->is_available()) {
        int brightness = read_config_brightness(100);
        backlight->set_brightness(brightness);
        fprintf(stderr, "helix-splash: Backlight ON at %d%%\n", brightness);
    }

    // Create splash UI
    // Fade-in animation only on DRM/SDL backends (fbdev doesn't alpha-blend well)
    bool use_fade = (backend->type() != DisplayBackendType::FBDEV);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* splash_widget = create_splash_ui(screen, width, height, dark_mode, use_fade);
    (void)splash_widget; // Used by animation, no need to track

    // Version number in upper-left corner (subtle, theme-aware)
    lv_obj_t* version_label = lv_label_create(screen);
    lv_label_set_text(version_label, "v" HELIX_VERSION);
    lv_obj_set_style_text_color(
        version_label, dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_opa(version_label, LV_OPA_40, LV_PART_MAIN);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 8, 6);
    (void)version_label;

    // Boot-status line (e.g. "Starting Klipper… 40s"), centered near the bottom.
    // Stays empty until the init gate writes the status file; updated in the
    // main loop below as the file changes.
    lv_obj_t* status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "");
    // Theme-aware text color, matching the version label (white on dark, black on
    // light) so it reads cleanly over the logo's gray art. Transparent
    // background — no backing pill; let the logo show through.
    lv_obj_set_style_text_color(
        status_label, dark_mode ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_opa(status_label, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_label, LV_OPA_TRANSP, LV_PART_MAIN);
    // Upper-right, clear of the centered logo (BOTTOM_MID overlapped it).
    lv_obj_align(status_label, LV_ALIGN_TOP_RIGHT, -8, 8);

    // On fbdev, other processes can write directly to /dev/fb0 behind LVGL's back
    // (e.g., ForgeX S99root boot messages). DRM/SDL are not susceptible since DRM
    // requires master access and SDL is windowed. Periodic full invalidation on fbdev
    // forces LVGL to repaint the entire screen, self-healing any stomped pixels.
    const bool needs_fb_self_heal = (backend->type() == DisplayBackendType::FBDEV);

    // Run the loop at full speed. LVGL's fbdev driver renders PARTIAL (60-line
    // stripes), flushing one stripe per refresh; throttling the loop spaces
    // those stripes out and the logo visibly wipes down the screen "line by
    // line". The splash now exits promptly (splash_should_continue + SIGUSR1),
    // so it is not running long enough for loop CPU to matter.
    const int frame_delay_us = FRAME_DELAY_US;

    // Boot-status heartbeat: the init gate rewrites the status file while it
    // waits for Moonraker. We treat each observed change (mtime or size) as a
    // heartbeat — recorded in the MONOTONIC clock so an NTP jump mid-boot can't
    // make a live heartbeat look stale — and extend our lifetime accordingly.
    const std::string status_path = splash_status_path();
    const helix::splash::SplashLifetimePolicy life_policy{MAX_LIFETIME_SEC, 180};
    struct stat status_prev {};
    bool have_status_prev = false;
    long last_heartbeat_mono = -1;
    std::string current_label; // gate-provided message, without the counter
    std::string shown_status;  // last string pushed to the label (label + counter)
    const long mem_floor_kb = splash_min_free_kb();

    // Main loop - run until signaled to quit
    // Exit signals: SIGTERM, SIGINT (shutdown), SIGUSR1 (main app ready)
    // Lifetime is bounded by splash_should_continue() using CLOCK_MONOTONIC, so
    // a slow or stuttering render loop still trips the safety net.
    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    struct timespec last_heal_ts = start_ts;

    // Paint the whole logo up front in one synchronous pass. lv_refr_now()
    // renders every invalidated stripe back-to-back (no inter-frame sleep), so
    // the splash appears as one quick fill instead of the throttled loop
    // revealing it stripe by stripe.
    lv_refr_now(display);

    while (!g_quit) {
        lv_timer_handler();
        usleep(frame_delay_us);

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        const long now_mono = now_ts.tv_sec;

        // Poll the status file: cheap stat() every loop, read contents only when
        // it changes. A change is a heartbeat and refreshes the status message.
        struct stat status_now {};
        if (stat(status_path.c_str(), &status_now) == 0) {
            const bool changed = !have_status_prev || status_now.st_mtime != status_prev.st_mtime ||
                                 status_now.st_size != status_prev.st_size;
            if (changed) {
                status_prev = status_now;
                have_status_prev = true;
                last_heartbeat_mono = now_mono;

                std::ifstream f(status_path);
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                current_label = helix::splash::sanitize_splash_message(content);
            }
        }

        // Render "<label> <elapsed>s", recomputed every loop so the counter keeps
        // climbing even after the gate hands off and stops rewriting the file —
        // the splash owns the count from its own monotonic start. Only touch the
        // label when the visible string actually changes (at most once per second)
        // to avoid invalidating the screen every frame.
        const std::string status_text =
            helix::splash::compose_splash_status(current_label, now_mono - start_ts.tv_sec);
        if (status_text != shown_status) {
            shown_status = status_text;
            lv_label_set_text(status_label, status_text.c_str());
            lv_obj_invalidate(screen);
        }

        // Periodic full redraw on fbdev to self-heal pixels stomped by other
        // processes writing /dev/fb0 (e.g. ForgeX boot messages). Every ~3s —
        // a full re-render re-runs the partial stripes, so doing it too often
        // produces a visible re-wipe for no benefit once boot UIs are gone.
        if (needs_fb_self_heal && (now_ts.tv_sec - last_heal_ts.tv_sec) >= 3) {
            lv_obj_invalidate(screen);
            last_heal_ts = now_ts;
        }

        // Memory safety valve: the splash must never be the process that tips a
        // tight-RAM device into OOM. If free memory drops below the floor, exit
        // now to free our ~10-15 MB — a brief blank screen beats an OOM kill.
        if (mem_floor_kb > 0) {
            std::ifstream meminfo("/proc/meminfo");
            if (meminfo.is_open()) {
                std::string mic((std::istreambuf_iterator<char>(meminfo)),
                                std::istreambuf_iterator<char>());
                const long avail = helix::splash::parse_meminfo_available_kb(mic);
                if (!helix::splash::splash_memory_ok(avail, mem_floor_kb)) {
                    fprintf(stderr,
                            "helix-splash: low memory (%ld KiB avail < %ld floor), exiting to "
                            "free RAM\n",
                            avail, mem_floor_kb);
                    break;
                }
            }
        }

        if (!helix::splash::splash_should_continue(life_policy, start_ts.tv_sec, now_mono,
                                                   last_heartbeat_mono)) {
            fprintf(stderr, "helix-splash: lifetime ended after %lds (no SIGUSR1), exiting\n",
                    (long)(now_mono - start_ts.tv_sec));
            break;
        }
    }

    // Clear framebuffer to background color before exit — but ONLY on a
    // shutdown/backstop exit (g_ready unset). On an app-ready handoff (SIGUSR1
    // -> g_ready) the successor, helix-screen's fb0 mirror, now owns /dev/fb0;
    // clearing it to dark would blank the remote screen until the next dirty
    // rect, and on an idle UI that repaint never comes — the remote goes black.
    // Leaving the last splash frame in place lets the mirror's full-frame repaint
    // (forced at handoff) take over seamlessly.
    if (!g_ready) {
        lv_obj_clean(screen);                                            // Remove all children
        lv_obj_set_style_bg_color(screen, lv_color_hex(clear_color), 0); // Match splash bg
        lv_obj_invalidate(screen);                                       // Mark for redraw
        lv_timer_handler();                                              // Render the clear
        lv_refr_now(nullptr);                                            // Force immediate refresh
    }

    // Cleanup is handled automatically by destructors
    return 0;
}
