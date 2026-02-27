// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file display_manager.cpp
 * @brief LVGL display and input device lifecycle management
 *
 * @pattern Manager wrapping DisplayBackend with RAII lifecycle
 * @threading Main thread only
 * @gotchas NEVER call lv_display_delete/lv_group_delete manually - lv_deinit() handles all cleanup
 *
 * @see application.cpp
 */

#include "display_manager.h"

#ifdef HELIX_DISPLAY_FBDEV
#include "display_backend_fbdev.h"
#endif

#ifdef HELIX_DISPLAY_DRM
#include "display_backend_drm.h"
#endif

#include "ui_effects.h"
#include "ui_fatal_error.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "config.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "lvgl_log_handler.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <string>

#ifdef HELIX_DISPLAY_SDL
#include "app_globals.h" // For app_request_quit()
#include "drivers/sdl/lv_sdl_window.h"

#include <SDL.h>
#endif

#ifndef HELIX_DISPLAY_SDL
#include <time.h>
#endif

using namespace helix;

// Static instance pointer for global access (e.g., from print_completion)
static DisplayManager* s_instance = nullptr;

#ifdef HELIX_DISPLAY_SDL
/**
 * @brief SDL event filter to intercept window close before LVGL processes it
 *
 * CRITICAL: Without this filter, clicking the window close button (X) causes LVGL's
 * SDL driver to immediately delete the display DURING lv_timer_handler().
 * This destroys all LVGL objects while timer callbacks may still be running, causing
 * use-after-free crashes.
 *
 * By intercepting SDL_WINDOWEVENT_CLOSE here and returning 0, we:
 * 1. Prevent LVGL from seeing the event (so it won't delete the display)
 * 2. Signal graceful shutdown via app_request_quit()
 * 3. Let Application::shutdown() clean up in the proper order
 *
 * @param userdata Unused
 * @param event SDL event to filter
 * @return 1 to pass event through, 0 to drop it
 */
static int sdl_event_filter(void* /*userdata*/, SDL_Event* event) {
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE) {
        spdlog::info("[DisplayManager] Window close intercepted - requesting graceful shutdown");
        app_request_quit();
        return 0; // Drop event - don't let LVGL's SDL driver see it
    }
    return 1; // Pass all other events through
}
#endif

DisplayManager::DisplayManager() = default;

DisplayManager::~DisplayManager() {
    shutdown();
}

bool DisplayManager::init(const Config& config) {
    if (m_initialized) {
        spdlog::warn("[DisplayManager] Already initialized, call shutdown() first");
        return false;
    }

    // Initialize LVGL library
    lv_init();

    // Register LVGL log handler immediately after lv_init() so that DRM/fbdev
    // driver errors are captured via spdlog (lv_init resets callbacks, so this
    // must come after it but before any display backend setup).
    helix::logging::register_lvgl_log_handler();

    // Initialize helix-xml engine (extracted from LVGL 9.5)
    // Must be called after lv_init() - sets up XML component scopes, widget registry, etc.
    lv_xml_init();

    // Create display backend (auto-detects: DRM → framebuffer → SDL)
    m_backend = DisplayBackend::create_auto();
    if (!m_backend) {
        spdlog::error("[DisplayManager] No display backend available");
        lv_xml_deinit();
        lv_deinit();
        return false;
    }

    spdlog::debug("[DisplayManager] Using backend: {}", m_backend->name());

    // Determine display dimensions
    m_width = config.width;
    m_height = config.height;

    // Auto-detect resolution for non-SDL backends when no dimensions specified
    if (m_width == 0 && m_height == 0 && m_backend->type() != DisplayBackendType::SDL) {
        auto detected = m_backend->detect_resolution();
        // Validate detected dimensions are within reasonable bounds
        if (detected.valid && detected.width >= 100 && detected.height >= 100 &&
            detected.width <= 8192 && detected.height <= 8192) {
            m_width = detected.width;
            m_height = detected.height;
            spdlog::info("[DisplayManager] Auto-detected resolution: {}x{}", m_width, m_height);
        } else if (detected.valid) {
            // Detection returned but with bogus values
            m_width = 800;
            m_height = 480;
            spdlog::warn("[DisplayManager] Detected resolution {}x{} out of bounds, using default",
                         detected.width, detected.height);
        } else {
            // Fall back to default 800x480
            m_width = 800;
            m_height = 480;
            spdlog::warn("[DisplayManager] Resolution detection failed, using default {}x{}",
                         m_width, m_height);
        }
    } else if (m_width == 0 || m_height == 0) {
        // SDL backend or partial dimensions specified - use defaults
        m_width = (m_width > 0) ? m_width : 800;
        m_height = (m_height > 0) ? m_height : 480;
        spdlog::debug("[DisplayManager] Using configured/default resolution: {}x{}", m_width,
                      m_height);
    }

    // Tell backend to skip FBIOBLANK when splash owns the framebuffer
    if (config.splash_active) {
        m_backend->set_splash_active(true);
    }

    // Create LVGL display
    m_display = m_backend->create_display(m_width, m_height);

    // If the primary backend failed to create a display, try falling back
    // to a different backend in-process (e.g., DRM passed is_available()
    // but mode setting or buffer allocation failed → try fbdev).
    if (!m_display && m_backend->type() != DisplayBackendType::FBDEV) {
        spdlog::warn("[DisplayManager] {} backend failed to create display, "
                     "attempting fbdev fallback",
                     m_backend->name());
        m_backend.reset();
        m_backend = DisplayBackend::create(DisplayBackendType::FBDEV);
        if (m_backend && m_backend->is_available()) {
            if (config.splash_active) {
                m_backend->set_splash_active(true);
            }
            m_display = m_backend->create_display(m_width, m_height);
            if (m_display) {
                spdlog::info("[DisplayManager] Fbdev fallback succeeded at {}x{}", m_width,
                             m_height);
            }
        }
    }

    if (!m_display) {
        spdlog::error("[DisplayManager] Failed to create display (all backends exhausted)");
        m_backend.reset();
        lv_xml_deinit();
        lv_deinit();
        return false;
    }

#ifdef HELIX_DISPLAY_DRM
    if (m_backend->type() == DisplayBackendType::DRM) {
        auto* drm = static_cast<DisplayBackendDRM*>(m_backend.get());
        if (drm->is_gpu_accelerated()) {
            spdlog::info("[Display] Rendering: GPU-accelerated (OpenGL ES via EGL)");
        } else {
            spdlog::info("[Display] Rendering: CPU (DRM dumb buffers)");
        }
    }
#endif

    // Unblank display via framebuffer ioctl AFTER creating LVGL display.
    // On AD5M, the FBIOBLANK state may be tied to the fd - calling it after
    // LVGL opens /dev/fb0 ensures the unblank persists while the display runs.
    // Uses same approach as GuppyScreen: FBIOBLANK + FBIOPAN_DISPLAY.
    //
    // Skip when splash is active: the splash process already unblanked the display
    // and is actively rendering to fb0. Calling FBIOBLANK + FBIOPAN_DISPLAY disrupts
    // the splash image and causes visible flicker.
    if (!config.splash_active) {
        if (m_backend->unblank_display()) {
            spdlog::info("[DisplayManager] Display unblanked via framebuffer ioctl");
        }
    } else {
        spdlog::debug("[DisplayManager] Skipping unblank — splash process owns framebuffer");
    }

    // Apply display rotation if configured.
    // Must happen AFTER display creation but BEFORE UI init so layout uses
    // the rotated resolution. LVGL auto-swaps width/height when rotation is set.
    {
        // CLI/config rotation (passed via Config struct)
        int rotation_degrees = config.rotation;

        // Environment variable override (highest priority)
        const char* env_rotate = std::getenv("HELIX_DISPLAY_ROTATION");
        if (env_rotate) {
            rotation_degrees = std::atoi(env_rotate);
            spdlog::info("[DisplayManager] HELIX_DISPLAY_ROTATION={} override", rotation_degrees);
        }

        // Fall back to config file if not set via Config struct or env
        if (rotation_degrees == 0) {
            rotation_degrees = helix::Config::get_instance()->get<int>("/display/rotate", 0);
        }

        if (rotation_degrees != 0) {
#ifdef HELIX_DISPLAY_SDL
            // LVGL's SDL driver only supports software rotation in PARTIAL render mode,
            // but we use DIRECT mode for performance. Skip rotation on SDL — it's only
            // for desktop dev. On embedded (fbdev/DRM) rotation works correctly.
            spdlog::warn("[DisplayManager] Rotation {}° requested but SDL backend does not "
                         "support software rotation (DIRECT render mode). Ignoring on desktop.",
                         rotation_degrees);
#else
            // Capture physical dimensions before rotation changes them
            int phys_w = m_width;
            int phys_h = m_height;

            lv_display_rotation_t lv_rot = degrees_to_lv_rotation(rotation_degrees);
            lv_display_set_rotation(m_display, lv_rot);

            // Update tracked dimensions to match rotated resolution
            m_width = lv_display_get_horizontal_resolution(m_display);
            m_height = lv_display_get_vertical_resolution(m_display);

            // Auto-rotate touch coordinates to match display rotation
            m_backend->set_display_rotation(lv_rot, phys_w, phys_h);

            spdlog::info("[DisplayManager] Display rotated {}° — effective resolution: {}x{}",
                         rotation_degrees, m_width, m_height);
#endif
        }
    }

    // Initialize UI update queue for thread-safe async updates
    // Must be done AFTER display is created - registers LV_EVENT_REFR_START handler
    helix::ui::update_queue_init();

#ifdef HELIX_DISPLAY_SDL
    // Install event filter to intercept window close before LVGL sees it.
    // CRITICAL: Must use SDL_SetEventFilter (not SDL_AddEventWatch) because only
    // SetEventFilter can actually DROP events (return 0 = drop). AddEventWatch
    // calls the callback but ignores the return value - events still reach the queue.
    // Without filtering, LVGL's SDL driver sees SDL_WINDOWEVENT_CLOSE, calls
    // lv_display_delete() mid-timer-handler, destroying all objects while animation
    // timers still reference them → use-after-free crash.
    SDL_SetEventFilter(sdl_event_filter, nullptr);
    spdlog::trace("[DisplayManager] Installed SDL event filter for graceful window close");
#endif

    // Create pointer input device (mouse/touch)
    m_pointer = m_backend->create_input_pointer();
    if (!m_pointer) {
#if defined(HELIX_DISPLAY_DRM) || defined(HELIX_DISPLAY_FBDEV)
        if (config.require_pointer) {
            // On embedded platforms, no input device is fatal
            spdlog::error("[DisplayManager] No input device found - cannot operate touchscreen UI");

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
                                suggestions, 30000);

            m_backend.reset();
            lv_xml_deinit();
            lv_deinit();
            return false;
        }
#else
        // On desktop (SDL), continue without pointer - mouse is optional
        spdlog::warn("[DisplayManager] No pointer input device created - touch/mouse disabled");
#endif
    }

    // Configure scroll behavior and sleep-aware wrapper
    if (m_pointer) {
        configure_scroll(config.scroll_throw, config.scroll_limit);
#ifndef HELIX_DISPLAY_SDL
        // Only install on embedded - SDL's event handler identifies the mouse device
        // by checking if read_cb == sdl_mouse_read, which our wrapper breaks.
        // Callback chain: sleep_aware_read_cb -> calibrated_read_cb -> evdev_read_cb
        // (calibrated_read installed by backend, sleep wrapper installed here)
        install_sleep_aware_input_wrapper();
#endif
    }

    // Create keyboard input device (optional)
    m_keyboard = m_backend->create_input_keyboard();
    if (m_keyboard) {
        setup_keyboard_group();
        spdlog::trace("[DisplayManager] Physical keyboard input enabled");
    }

    // Create backlight backend (auto-detects hardware)
    m_backlight = BacklightBackend::create();
    spdlog::info("[DisplayManager] Backlight: {} (available: {})", m_backlight->name(),
                 m_backlight->is_available());

    // Resolve hardware vs software blank strategy.
    // Config override: /display/hardware_blank (0 or 1). Missing (-1) = auto-detect.
    {
        int hw_blank_override =
            helix::Config::get_instance()->get<int>("/display/hardware_blank", -1);
        if (hw_blank_override >= 0) {
            m_use_hardware_blank = (hw_blank_override != 0);
            spdlog::info("[DisplayManager] Hardware blank: {} (config override)",
                         m_use_hardware_blank);
        } else {
            m_use_hardware_blank = m_backlight && m_backlight->supports_hardware_blank();
            spdlog::info("[DisplayManager] Hardware blank: {} (auto-detected from {})",
                         m_use_hardware_blank, m_backlight ? m_backlight->name() : "none");
        }
    }

    // Force backlight ON at startup - ensures display is visible even if
    // previous instance left it off or in an unknown state
    if (m_backlight && m_backlight->is_available()) {
        m_backlight->set_brightness(100);
        spdlog::debug("[DisplayManager] Backlight forced ON at 100% for startup");

        // Schedule delayed brightness override to counteract ForgeX's delayed_gcode.
        // On AD5M, Klipper's reset_screen fires ~3s after Klipper becomes READY.
        // Klipper typically becomes ready 10-20s after boot, so a 20s delay ensures
        // we fire AFTER the delayed_gcode dims the screen.
        // Only needed on Allwinner (AD5M) - other platforms don't have this issue.
        if (std::string_view(m_backlight->name()) == "Allwinner") {
            lv_timer_create(
                [](lv_timer_t* t) {
                    auto* dm = static_cast<DisplayManager*>(lv_timer_get_user_data(t));
                    if (dm && dm->m_backlight && dm->m_backlight->is_available()) {
                        int brightness = DisplaySettingsManager::instance().get_brightness();
                        brightness = std::clamp(brightness, 10, 100);
                        dm->m_backlight->set_brightness(brightness);
                        spdlog::info("[DisplayManager] Delayed brightness override: {}%",
                                     brightness);
                    }
                    lv_timer_delete(t);
                },
                20000, this);
        }
    }

    // Load dim settings from config
    helix::Config* cfg = helix::Config::get_instance();
    m_dim_timeout_sec = cfg->get<int>("/display/dim_sec", 300);
    m_dim_brightness_percent = std::clamp(cfg->get<int>("/display/dim_brightness", 30), 1, 100);
    spdlog::debug("[DisplayManager] Display dim: {}s timeout, {}% brightness", m_dim_timeout_sec,
                  m_dim_brightness_percent);

    // Debug touch visualization: draw ripple at each touch point
    if (RuntimeConfig::debug_touches() && m_pointer) {
        spdlog::info("[DisplayManager] Debug touch visualization enabled");
        lv_timer_create(
            [](lv_timer_t* t) {
                auto* indev = static_cast<lv_indev_t*>(lv_timer_get_user_data(t));
                if (!indev)
                    return;

                lv_indev_state_t state = lv_indev_get_state(indev);
                if (state != LV_INDEV_STATE_PRESSED)
                    return;

                lv_point_t point;
                lv_indev_get_point(indev, &point);

                // Throttle: only create ripple when position changes
                static lv_coord_t last_x = -100, last_y = -100;
                lv_coord_t dx = point.x - last_x;
                lv_coord_t dy = point.y - last_y;
                if (dx * dx + dy * dy < 25) // <5px movement
                    return;

                last_x = point.x;
                last_y = point.y;
                helix::ui::create_ripple(lv_layer_top(), point.x, point.y, 10, 40, 300);
            },
            30, m_pointer);
    }

    spdlog::trace("[DisplayManager] Initialized: {}x{}", m_width, m_height);
    m_initialized = true;
    s_instance = this;
    return true;
}

DisplayManager* DisplayManager::instance() {
    return s_instance;
}

void DisplayManager::shutdown() {
    if (!m_initialized) {
        return;
    }

    m_shutting_down = true;
    s_instance = nullptr;
    spdlog::debug("[DisplayManager] Shutting down");

    // NOTE: We do NOT call lv_group_delete(m_input_group) here because:
    // 1. Objects in the group may already be freed (panels deleted before display)
    // 2. lv_deinit() calls lv_group_deinit() which safely clears the group list
    // 3. lv_group_delete() iterates objects and would crash on dangling pointers
    m_input_group = nullptr;

    // Reset input device pointers (LVGL manages their memory)
    m_keyboard = nullptr;
    m_pointer = nullptr;

    // NOTE: We do NOT call lv_display_delete() here because:
    // lv_deinit() iterates all displays and deletes them.
    // Manually deleting first causes double-free crash.
    m_display = nullptr;

    // Sleep overlay is an LVGL object freed by lv_deinit() — just clear the pointer.
    // Don't call destroy_sleep_overlay() here because lv_obj_delete() ordering
    // relative to other LVGL teardown is fragile.
    m_sleep_overlay = nullptr;
    m_use_hardware_blank = false;

    // Release backends
    m_backlight.reset();
    m_backend.reset();

    // Shutdown UI update queue before LVGL
    helix::ui::update_queue_shutdown();

    // Quit SDL before LVGL deinit - must be called outside the SDL event handler.
#ifdef HELIX_DISPLAY_SDL
    // Remove our event filter before SDL cleanup
    SDL_SetEventFilter(nullptr, nullptr);
    lv_sdl_quit();
#endif

    // Deinitialize helix-xml engine before LVGL (frees component scopes, fonts, etc.)
    lv_xml_deinit();

    // Deinitialize LVGL (guard against static destruction order issues)
    if (lv_is_initialized()) {
        lv_deinit();
    }

    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

void DisplayManager::configure_scroll(int scroll_throw, int scroll_limit) {
    if (!m_pointer) {
        return;
    }

    lv_indev_set_scroll_throw(m_pointer, static_cast<uint8_t>(scroll_throw));
    lv_indev_set_scroll_limit(m_pointer, static_cast<uint8_t>(scroll_limit));
    spdlog::trace("[DisplayManager] Scroll config: throw={}, limit={}", scroll_throw, scroll_limit);
}

void DisplayManager::setup_keyboard_group() {
    if (!m_keyboard) {
        return;
    }

    m_input_group = lv_group_create();
    lv_group_set_default(m_input_group);
    lv_indev_set_group(m_keyboard, m_input_group);
    spdlog::trace("[DisplayManager] Created default input group for keyboard");
}

// ============================================================================
// Static Timing Functions
// ============================================================================

uint32_t DisplayManager::get_ticks() {
#ifdef HELIX_DISPLAY_SDL
    return SDL_GetTicks();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

void DisplayManager::delay(uint32_t ms) {
#ifdef HELIX_DISPLAY_SDL
    SDL_Delay(ms);
#else
    struct timespec ts = {static_cast<time_t>(ms / 1000),
                          static_cast<long>((ms % 1000) * 1000000L)};
    nanosleep(&ts, nullptr);
#endif
}

// ============================================================================
// Sleep Entry
// ============================================================================

void DisplayManager::enter_sleep(int timeout_sec) {
    m_display_sleeping = true;
    if (m_use_hardware_blank) {
        if (m_backend) {
            m_backend->blank_display();
        }
        if (m_backlight) {
            m_backlight->set_brightness(0);
        }
        spdlog::info("[DisplayManager] Display sleeping (hardware blank) after {}s", timeout_sec);
    } else {
        create_sleep_overlay();
        if (m_backlight && m_backlight->is_available()) {
            m_backlight->set_brightness(0);
        }
        spdlog::info("[DisplayManager] Display sleeping (software overlay) after {}s", timeout_sec);
    }
}

// ============================================================================
// Software Sleep Overlay
// ============================================================================

void DisplayManager::create_sleep_overlay() {
    if (m_sleep_overlay) {
        return;
    }
    m_sleep_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(m_sleep_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(m_sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(m_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m_sleep_overlay, 0, 0);
    lv_obj_set_style_pad_all(m_sleep_overlay, 0, 0);
    lv_obj_remove_flag(m_sleep_overlay, LV_OBJ_FLAG_CLICKABLE);
    spdlog::debug("[DisplayManager] Software sleep overlay created");
}

void DisplayManager::destroy_sleep_overlay() {
    if (!m_sleep_overlay) {
        return;
    }
    lv_obj_delete(m_sleep_overlay);
    m_sleep_overlay = nullptr;
    spdlog::debug("[DisplayManager] Software sleep overlay destroyed");
}

// ============================================================================
// Display Sleep Management
// ============================================================================

void DisplayManager::check_display_sleep() {
    // If sleep-while-printing is disabled, inhibit sleep/dim during active prints
    if (!DisplaySettingsManager::instance().get_sleep_while_printing()) {
        PrintJobState job_state = get_printer_state().get_print_job_state();
        if (job_state == PrintJobState::PRINTING || job_state == PrintJobState::PAUSED) {
            // Reset LVGL activity timer so we don't immediately sleep when print ends
            lv_display_trigger_activity(nullptr);
            return;
        }
    }

    // Get configured sleep timeout from settings (0 = disabled)
    int sleep_timeout_sec = DisplaySettingsManager::instance().get_display_sleep_sec();

    // Get LVGL inactivity time (milliseconds since last touch/input)
    uint32_t inactive_ms = lv_display_get_inactive_time(nullptr);
    uint32_t dim_timeout_ms =
        (m_dim_timeout_sec > 0) ? static_cast<uint32_t>(m_dim_timeout_sec) * 1000U : UINT32_MAX;
    uint32_t sleep_timeout_ms =
        (sleep_timeout_sec > 0) ? static_cast<uint32_t>(sleep_timeout_sec) * 1000U : UINT32_MAX;

    // Periodic debug logging (every 30 seconds when inactive > 10s)
    static uint32_t last_log_time = 0;
    uint32_t now = get_ticks();
    if (inactive_ms > 10000 && (now - last_log_time) >= 30000) {
        spdlog::trace(
            "[DisplayManager] Sleep check: inactive={}s, dim_timeout={}s, sleep_timeout={}s, "
            "dimmed={}, sleeping={}, backlight={}",
            inactive_ms / 1000, m_dim_timeout_sec, sleep_timeout_sec, m_display_dimmed,
            m_display_sleeping, m_backlight ? "yes" : "no");
        last_log_time = now;
    }

    // Check for activity (touch detected within last 500ms)
    bool activity_detected = (inactive_ms < 500);

    if (m_display_sleeping) {
        // Wake via sleep_aware_read_cb (embedded) or LVGL activity detection (SDL).
        // On SDL, the sleep-aware wrapper isn't installed because it breaks SDL's
        // mouse device identification, so we fall back to LVGL activity tracking.
        if (m_wake_requested || activity_detected) {
            m_wake_requested = false;
            wake_display();
        }
    } else if (m_display_dimmed) {
        // Currently dimmed - wake on touch, or go to sleep if timeout exceeded
        if (activity_detected) {
            wake_display();
        } else if (sleep_timeout_sec > 0 && inactive_ms >= sleep_timeout_ms) {
            // Transition from dimmed to sleeping
            enter_sleep(sleep_timeout_sec);
        }
    } else {
        // Currently awake - check if we should dim or sleep
        if (sleep_timeout_sec > 0 && inactive_ms >= sleep_timeout_ms) {
            // Skip dim, go straight to sleep (sleep timeout <= dim timeout)
            enter_sleep(sleep_timeout_sec);
        } else if (m_dim_timeout_sec > 0 && inactive_ms >= dim_timeout_ms) {
            // Dim the display
            m_display_dimmed = true;
            if (m_backlight) {
                m_backlight->set_brightness(m_dim_brightness_percent);
            }
            spdlog::info("[DisplayManager] Display dimmed to {}% after {}s inactivity",
                         m_dim_brightness_percent, m_dim_timeout_sec);
        }
    }
}

void DisplayManager::wake_display() {
    if (m_shutting_down) {
        return; // Shutdown in progress — avoid touching LVGL objects
    }

    if (!m_display_sleeping && !m_display_dimmed) {
        return; // Already fully awake
    }

    bool was_sleeping = m_display_sleeping;
    m_display_sleeping = false;
    m_display_dimmed = false;

    // Gate input if waking from full sleep (not dim)
    // This prevents the wake touch from triggering UI actions
    if (was_sleeping) {
        disable_input_briefly();

        if (m_use_hardware_blank) {
            // Unblank framebuffer when waking from full sleep (not just dim).
            // On AD5M, the FBIOBLANK ioctl is needed to actually turn on the display.
            if (m_backend) {
                m_backend->unblank_display();
            }
        } else {
            // Remove software sleep overlay
            destroy_sleep_overlay();
        }

        // Force full screen repaint after wake. Hardware path: some HDMI hardware
        // clears framebuffer memory during FBIOBLANK (#19). Software path: ensures
        // UI is fully rendered after overlay removal.
        lv_obj_invalidate(lv_screen_active());

        // Reset LVGL's inactivity timer so we don't immediately go back to sleep.
        // When touch is absorbed by sleep_aware_read_cb, LVGL doesn't register activity,
        // so without this the display would wake and immediately sleep again.
        lv_display_trigger_activity(nullptr);
    }

    // Restore configured brightness from settings
    int brightness = DisplaySettingsManager::instance().get_brightness();
    brightness = std::clamp(brightness, 10, 100);

    if (m_backlight) {
        m_backlight->set_brightness(brightness);
    }
    spdlog::info("[DisplayManager] Display woken from {}, brightness restored to {}%",
                 was_sleeping ? "sleep" : "dim", brightness);
}

void DisplayManager::ensure_display_on() {
    // Force display awake at startup regardless of previous state
    m_display_sleeping = false;
    m_display_dimmed = false;

    // Get configured brightness (or default to 50%)
    int brightness = DisplaySettingsManager::instance().get_brightness();
    brightness = std::clamp(brightness, 10, 100);

    // Apply to hardware - this ensures display is visible
    if (m_backlight) {
        m_backlight->set_brightness(brightness);
    }
    spdlog::info("[DisplayManager] Startup: forcing display ON at {}% brightness", brightness);
}

void DisplayManager::set_dim_timeout(int seconds) {
    m_dim_timeout_sec = seconds;
    spdlog::debug("[DisplayManager] Dim timeout set to {}s", seconds);
}

void DisplayManager::restore_display_on_shutdown() {
    // Clean up software sleep overlay if active
    destroy_sleep_overlay();

    // Ensure display is awake before exiting so next app doesn't start with black screen
    int brightness = DisplaySettingsManager::instance().get_brightness();
    brightness = std::clamp(brightness, 10, 100);

    if (m_backlight) {
        m_backlight->set_brightness(brightness);
    }
    m_display_sleeping = false;
    spdlog::debug("[DisplayManager] Shutdown: restoring display to {}% brightness", brightness);
}

void DisplayManager::set_backlight_brightness(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (m_backlight) {
        m_backlight->set_brightness(percent);
    }
}

bool DisplayManager::has_backlight_control() const {
    return m_backlight && m_backlight->is_available();
}

// ============================================================================
// Touch Calibration
// ============================================================================

bool DisplayManager::apply_touch_calibration(const helix::TouchCalibration& cal) {
    if (!cal.valid) {
        spdlog::debug("[DisplayManager] Invalid calibration");
        return false;
    }

#ifdef HELIX_DISPLAY_FBDEV
    if (m_backend && m_backend->type() == DisplayBackendType::FBDEV) {
        auto* fbdev = static_cast<DisplayBackendFbdev*>(m_backend.get());
        return fbdev->set_calibration(cal);
    }
#endif

    spdlog::debug("[DisplayManager] Touch calibration not applicable to current backend");
    return false;
}

helix::TouchCalibration DisplayManager::get_current_calibration() const {
#ifdef HELIX_DISPLAY_FBDEV
    if (m_backend && m_backend->type() == DisplayBackendType::FBDEV) {
        auto* fbdev = static_cast<DisplayBackendFbdev*>(m_backend.get());
        return fbdev->get_calibration();
    }
#endif

    // Return invalid calibration for non-fbdev backends
    return helix::TouchCalibration{};
}

bool DisplayManager::needs_touch_calibration() const {
#ifdef HELIX_DISPLAY_FBDEV
    if (m_backend && m_backend->type() == DisplayBackendType::FBDEV) {
        auto* fbdev = static_cast<DisplayBackendFbdev*>(m_backend.get());
        return fbdev->needs_touch_calibration();
    }
#endif
    return false;
}

// ============================================================================
// Input Gating (Wake-Only First Touch)
// ============================================================================

void DisplayManager::disable_input_briefly() {
    // Disable all pointer input devices
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_enable(indev, false);
        }
        indev = lv_indev_get_next(indev);
    }

    // Schedule re-enable after 200ms via LVGL timer
    lv_timer_create(reenable_input_cb, 200, nullptr);

    spdlog::debug("[DisplayManager] Input disabled for 200ms (wake-only touch)");
}

void DisplayManager::reenable_input_cb(lv_timer_t* timer) {
    // Re-enable all pointer input devices
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_enable(indev, true);
        }
        indev = lv_indev_get_next(indev);
    }

    // Delete the one-shot timer
    lv_timer_delete(timer);

    spdlog::debug("[DisplayManager] Input re-enabled after wake");
}

// ============================================================================
// Sleep-Aware Input Wrapper
// ============================================================================

void DisplayManager::sleep_aware_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* dm = DisplayManager::instance();
    if (!dm) {
        return;
    }

    // Call original callback first (may be evdev, libinput, or calibrated wrapper)
    if (dm->m_original_pointer_read_cb) {
        dm->m_original_pointer_read_cb(indev, data);
    }

    // If sleeping and touch detected, absorb the touch and request wake
    if (dm->m_display_sleeping && data->state == LV_INDEV_STATE_PRESSED) {
        dm->m_wake_requested = true;
        data->state = LV_INDEV_STATE_RELEASED; // Absorb - LVGL sees no press
        spdlog::debug("[DisplayManager] Touch absorbed while sleeping, wake requested");
    }
}

void DisplayManager::install_sleep_aware_input_wrapper() {
    if (!m_pointer) {
        return;
    }

    // Save original read callback
    m_original_pointer_read_cb = lv_indev_get_read_cb(m_pointer);
    if (!m_original_pointer_read_cb) {
        spdlog::warn("[DisplayManager] No read callback on pointer device, sleep-aware wrapper not "
                     "installed");
        return;
    }

    // Install our wrapper
    lv_indev_set_read_cb(m_pointer, sleep_aware_read_cb);
    spdlog::info("[DisplayManager] Sleep-aware input wrapper installed");
}

// ============================================================================
// Rotation Probe (first-boot auto-detect)
// ============================================================================

void DisplayManager::run_rotation_probe() {
    if (!m_display || !m_pointer) {
        spdlog::info("[DisplayManager] Rotation probe skipped: display={}, pointer={}",
                     m_display ? "ok" : "null", m_pointer ? "ok" : "null");
        return;
    }

    // On SDL, rotation doesn't visually rotate (DIRECT render mode limitation),
    // but the probe UI and tap detection still work for testing the flow.
    bool is_sdl = (m_backend && m_backend->type() == DisplayBackendType::SDL);
    if (is_sdl) {
        spdlog::info("[DisplayManager] Rotation probe on SDL: display won't rotate, "
                     "but UI and tap detection work for testing");
    }

    // Fonts for probe UI (compiled-in, available before XML/theme init)
    extern const lv_font_t noto_sans_24;
    extern const lv_font_t noto_sans_16;

    // Physical dimensions: m_width/m_height are pre-rotation at this point
    // because the probe runs before any rotation is applied in init().
    int phys_w = m_width;
    int phys_h = m_height;

    const lv_display_rotation_t rotations[] = {LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90,
                                               LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270};
    const int rotation_degrees[] = {0, 90, 180, 270};
    const int num_rotations = 4;
    const int scan_timeout_ms = 5000;
    const int confirm_timeout_ms = 10000;

    spdlog::info("[DisplayManager] Starting rotation probe (physical={}x{})", phys_w, phys_h);

    // Lambda to create probe screen UI. subtitle_cb generates the subtitle text
    // given rotation degrees and seconds remaining.
    using SubtitleFn = std::function<std::string(int rot_deg, int secs)>;

    auto create_probe_screen = [&](const char* main_text, const char* help_text,
                                   SubtitleFn subtitle_fn, int rot_deg, int timeout_ms,
                                   lv_color_t bg_color) -> std::pair<lv_obj_t*, SubtitleFn> {
        lv_obj_t* scr = lv_screen_active();
        lv_obj_clean(scr);
        lv_obj_set_style_bg_color(scr, bg_color, 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        // Main text — constrain width for narrow screens
        lv_obj_t* main_lbl = lv_label_create(scr);
        lv_label_set_text(main_lbl, main_text);
        lv_obj_set_width(main_lbl, lv_pct(90));
        lv_label_set_long_mode(main_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(main_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(main_lbl, &noto_sans_24, 0);
        lv_obj_set_style_text_align(main_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(main_lbl, LV_ALIGN_CENTER, 0, -30);

        // Help text (smaller, below main)
        if (help_text && help_text[0] != '\0') {
            lv_obj_t* help_lbl = lv_label_create(scr);
            lv_label_set_text(help_lbl, help_text);
            lv_obj_set_width(help_lbl, lv_pct(90));
            lv_label_set_long_mode(help_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(help_lbl, lv_color_hex(0x888888), 0);
            lv_obj_set_style_text_font(help_lbl, &noto_sans_16, 0);
            lv_obj_set_style_text_align(help_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(help_lbl, LV_ALIGN_CENTER, 0, 5);
        }

        // Subtitle (countdown updated externally)
        lv_obj_t* sub_lbl = lv_label_create(scr);
        std::string initial = subtitle_fn(rot_deg, timeout_ms / 1000);
        lv_label_set_text(sub_lbl, initial.c_str());
        lv_obj_set_width(sub_lbl, lv_pct(90));
        lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sub_lbl, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_text_font(sub_lbl, &noto_sans_16, 0);
        lv_obj_set_style_text_align(sub_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(sub_lbl, LV_ALIGN_CENTER, 0, 35);

        return {sub_lbl, subtitle_fn};
    };

    // Disable LVGL's automatic input processing during probe — we read
    // the touch device directly. Without this, both lv_timer_handler() and
    // our direct read_cb call would consume evdev events, causing missed taps.
    lv_indev_enable(m_pointer, false);

    // Lambda for mini event loop that watches for tap.
    // Returns immediately on confirmed tap (no post-tap delay).
    auto wait_for_tap = [&](int timeout_ms, lv_obj_t* countdown_lbl, SubtitleFn subtitle_fn,
                            int rot_deg) -> bool {
        uint32_t start = get_ticks();
        int last_sec = -1;

        while (true) {
            uint32_t elapsed = get_ticks() - start;
            if (elapsed >= static_cast<uint32_t>(timeout_ms)) {
                return false;
            }

            lv_timer_handler();
            delay(10);

            // Update countdown label
            int remaining_sec = static_cast<int>((timeout_ms - elapsed + 999) / 1000);
            if (remaining_sec != last_sec && countdown_lbl) {
                std::string text = subtitle_fn(rot_deg, remaining_sec);
                lv_label_set_text(countdown_lbl, text.c_str());
                last_sec = remaining_sec;
            }

            // Check for tap via direct indev read (LVGL indev is disabled)
            lv_indev_data_t data = {};
            lv_indev_read_cb_t read_cb = lv_indev_get_read_cb(m_pointer);
            if (read_cb) {
                read_cb(m_pointer, &data);
                if (data.state == LV_INDEV_STATE_PRESSED) {
                    // Drain the press — wait for release so it doesn't
                    // carry over to the next screen as a phantom tap.
                    uint32_t release_deadline = get_ticks() + 2000; // 2s max
                    while (get_ticks() < release_deadline) {
                        lv_timer_handler();
                        delay(10);
                        lv_indev_data_t release_data = {};
                        read_cb(m_pointer, &release_data);
                        if (release_data.state == LV_INDEV_STATE_RELEASED) {
                            break;
                        }
                    }
                    return true;
                }
            }
        }
    };

    int confirmed_rotation = -1;

    // Loop until user confirms a rotation. On real hardware, the wrong rotation
    // renders unreadable text so the user can only tap the correct one.
    while (confirmed_rotation < 0) {
        for (int i = 0; i < num_rotations; i++) {
            // Apply rotation (skip on SDL — DIRECT render mode can't rotate)
            if (!is_sdl) {
                lv_display_set_rotation(m_display, rotations[i]);
                m_backend->set_display_rotation(rotations[i], phys_w, phys_h);
                m_width = lv_display_get_horizontal_resolution(m_display);
                m_height = lv_display_get_vertical_resolution(m_display);
            }

            spdlog::info("[DisplayManager] Rotation probe: testing {}° ({}x{})",
                         rotation_degrees[i], m_width, m_height);

            // PHASE 1: Show "tap if readable"
            auto scan_subtitle = [&](int rot_deg, int secs) -> std::string {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         lv_tr("Testing rotation: %d\xc2\xb0 (%d/%d) - %ds remaining"), rot_deg,
                         i + 1, num_rotations, secs);
                return buf;
            };
            auto [sub, sub_fn] = create_probe_screen(
                lv_tr("Tap anywhere if this text is right-side up"),
                lv_tr("HelixScreen is detecting your display orientation"), scan_subtitle,
                rotation_degrees[i], scan_timeout_ms, lv_color_hex(0x1a1a2e));

            bool tapped = wait_for_tap(scan_timeout_ms, sub, sub_fn, rotation_degrees[i]);

            if (!tapped) {
                continue;
            }

            // PHASE 2: Confirm
            spdlog::info("[DisplayManager] Rotation probe: {}° tapped, confirming...",
                         rotation_degrees[i]);

            auto confirm_subtitle = [](int rot_deg, int secs) -> std::string {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         lv_tr("Rotation: %d\xc2\xb0 - %ds remaining (or wait to retry)"), rot_deg,
                         secs);
                return buf;
            };
            auto [confirm_sub, confirm_fn] = create_probe_screen(
                lv_tr("Tap again to confirm this orientation"), "", confirm_subtitle,
                rotation_degrees[i], confirm_timeout_ms, lv_color_hex(0x1a2e1a));

            bool confirmed =
                wait_for_tap(confirm_timeout_ms, confirm_sub, confirm_fn, rotation_degrees[i]);

            if (confirmed) {
                confirmed_rotation = rotation_degrees[i];
                spdlog::info("[DisplayManager] Rotation probe: {}° confirmed!", confirmed_rotation);
                break;
            }

            spdlog::info("[DisplayManager] Rotation probe: {}° not confirmed, continuing scan",
                         rotation_degrees[i]);
        }
    }

    // Save confirmed rotation
    helix::Config* cfg = helix::Config::get_instance();
    cfg->set("/display/rotation_probed", true);
    cfg->set("/display/rotate", confirmed_rotation);
    cfg->save();
    spdlog::info("[DisplayManager] Rotation probe saved: {}°", confirmed_rotation);

    // Ensure display is at the confirmed rotation
    if (!is_sdl) {
        lv_display_rotation_t confirmed_lv_rot = degrees_to_lv_rotation(confirmed_rotation);
        lv_display_set_rotation(m_display, confirmed_lv_rot);
        m_backend->set_display_rotation(confirmed_lv_rot, phys_w, phys_h);
        m_width = lv_display_get_horizontal_resolution(m_display);
        m_height = lv_display_get_vertical_resolution(m_display);
    }

    // Re-enable LVGL input processing for normal operation
    lv_indev_enable(m_pointer, true);

    // Clean screen and reset background for normal UI init.
    // lv_obj_clean() only removes children — the screen's own bg style
    // (set by the probe) must be explicitly cleared so the theme can apply.
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_local_style_prop(scr, LV_STYLE_BG_COLOR, LV_PART_MAIN);
    lv_obj_remove_local_style_prop(scr, LV_STYLE_BG_OPA, LV_PART_MAIN);
}

// ============================================================================
// Window Resize Handler (Desktop/SDL)
// ============================================================================

void DisplayManager::resize_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<DisplayManager*>(lv_timer_get_user_data(timer));
    if (!self || self->m_shutting_down) {
        return;
    }

    spdlog::debug("[DisplayManager] Resize debounce complete, calling {} registered callbacks",
                  self->m_resize_callbacks.size());

    // Call all registered callbacks
    for (auto callback : self->m_resize_callbacks) {
        if (callback) {
            callback();
        }
    }

    // Delete one-shot timer
    lv_timer_delete(timer);
    self->m_resize_debounce_timer = nullptr;
}

void DisplayManager::resize_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_SIZE_CHANGED) {
        auto* self = static_cast<DisplayManager*>(lv_event_get_user_data(e));
        if (!self || self->m_shutting_down) {
            return;
        }

        lv_obj_t* screen = static_cast<lv_obj_t*>(lv_event_get_target(e));
        lv_coord_t width = lv_obj_get_width(screen);
        lv_coord_t height = lv_obj_get_height(screen);

        spdlog::debug("[DisplayManager] Screen size changed to {}x{}, resetting debounce timer",
                      width, height);

        // Reset or create debounce timer
        if (self->m_resize_debounce_timer) {
            lv_timer_reset(self->m_resize_debounce_timer);
        } else {
            self->m_resize_debounce_timer =
                lv_timer_create(resize_timer_cb, RESIZE_DEBOUNCE_MS, self);
            lv_timer_set_repeat_count(self->m_resize_debounce_timer, 1); // One-shot
        }
    }
}

void DisplayManager::init_resize_handler(lv_obj_t* screen) {
    if (!screen) {
        spdlog::error("[DisplayManager] Cannot init resize handler: screen is null");
        return;
    }

    // Add SIZE_CHANGED event listener to screen
    lv_obj_add_event_cb(screen, resize_event_cb, LV_EVENT_SIZE_CHANGED, this);

    spdlog::trace("[DisplayManager] Resize handler initialized on screen");
}

void DisplayManager::register_resize_callback(ResizeCallback callback) {
    if (!callback) {
        spdlog::warn("[DisplayManager] Attempted to register null resize callback");
        return;
    }

    m_resize_callbacks.push_back(callback);
    spdlog::trace("[DisplayManager] Registered resize callback ({} total)",
                  m_resize_callbacks.size());
}
