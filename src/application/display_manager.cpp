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

#include "print_lifecycle_state.h"

// Private LVGL header for direct flush_cb capture (matches application.cpp pattern)
#include "ui_effects.h"
#include "ui_fatal_error.h"
#include "ui_update_queue.h"

#include "../../include/pending_startup_warnings.h"
#include "app_constants.h"
#include "app_globals.h"
#include "board_identity.h"
#include "config.h"
#include "display/lv_display_private.h"
#include "display_settings_manager.h"
#include "flush_stride.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"        // not in the lvgl.h umbrella
#include "lvgl/src/misc/cache/instance/lv_image_header_cache.h" // not in the lvgl.h umbrella
#include "lvgl/src/misc/lv_timer_private.h" // lv_timer_t::period; LVGL has no period getter
#include "lvgl_log_handler.h"
#include "printer_state.h"
#include "refresh_period_hold.h"
#include "refresh_timing_env.h"
#include "remote_screen_fb0_sink.h"
#include "remote_screen_service.h"
#include "rotation_probe.h"
#include "runtime_config.h"
#include "screen_hide_hold.h"
#include "tap_latch.h"
#include "touch_calibration_wrapper.h"
#if HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_clock.h"
#endif

#include "pending_startup_warnings.h"
#include "system/telemetry_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>

#ifdef HELIX_DISPLAY_SDL
#include "app_globals.h" // For app_request_quit()

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

DisplayManager::DisplayManager()
    : m_sleep(DisplaySleepHost{
          m_backend, m_backlight, m_display, m_shutting_down, [] { cancel_pointer_presses(); },
          [this](lv_display_flush_cb_t cb) { restore_flush_cb(cb); },
          [] { return helix::RemoteScreenService::instance().viewer_active(); }}) {}

DisplayManager::~DisplayManager() {
    shutdown();
}

namespace {

/// Reads a non-negative integer env var, or `fallback` when unset or malformed.
long env_count(const char* name, long fallback) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(v, &end, 10);
    if (end == v || *end != '\0' || parsed < 0) {
        spdlog::warn("[DisplayManager] {}='{}' is not a non-negative integer, ignoring", name, v);
        return fallback;
    }
    return parsed;
}

/// Sizes LVGL's decoded-image cache from the environment. 0, the default, leaves it off.
///
/// Any size is safe. An image whose decoded size exceeds the whole cache cannot be
/// stored - LVGL reports that before it evicts anything - and
/// `patches/lvgl-image-cache-oversize-uncached.patch` makes the decoder draw those
/// uncached instead of discarding the decode. Sizing below the working set therefore
/// buys less, it does not blank anything.
///
/// Decoded size is the source asset's w*h*4: lodepng always produces ARGB8888, so a
/// 16bpp panel does not shrink it. Shipped printer PNGs reach 18 MB decoded, well beyond
/// any cache worth giving an embedded board.
void apply_image_cache_env() {
    const long cache_kb = env_count("HELIX_IMAGE_CACHE_KB", 0);
    if (cache_kb > 0) {
        lv_image_cache_resize(static_cast<uint32_t>(cache_kb) * 1024u, false);
        spdlog::info("[DisplayManager] Image cache: {} KB", cache_kb);
    }
}

} // namespace

void DisplayManager::schedule_brightness_reassert() {
    // Schedule delayed brightness override to counteract ForgeX's delayed_gcode.
    // On AD5M, Klipper's reset_screen fires ~3s after Klipper becomes READY.
    // Klipper typically becomes ready 10-20s after boot, so a 20s delay ensures
    // we fire AFTER the delayed_gcode dims the screen.
    // Only the FlashForge firmwares dim behind our back.
    if (helix::board::current_info().firmware_dims_backlight_after_boot) {
        m_brightness_reassert_timer.reset(lv_timer_create(
            [](lv_timer_t* t) {
                auto* dm = static_cast<DisplayManager*>(lv_timer_get_user_data(t));
                dm->m_brightness_reassert_timer.release();
                if (dm->m_backlight && dm->m_backlight->is_available()) {
                    const int brightness = DisplaySettingsManager::instance().user_brightness();
                    dm->m_backlight->set_brightness(brightness);
                    spdlog::info("[DisplayManager] Delayed brightness override: {}%", brightness);
                }
                lv_timer_delete(t);
            },
            20000, this));
    }
}

bool DisplayManager::init(const Config& config) {
    if (m_initialized) {
        spdlog::warn("[DisplayManager] Already initialized, call shutdown() first");
        return false;
    }

    // Initialize LVGL library
    lv_init();

    // lv_init() builds both cache objects even when lv_conf.h sizes them at 0,
    // so they can be sized here rather than at compile time. Both default to 0,
    // which is the behaviour every board ships with today.
    apply_image_cache_env();

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
        TelemetryManager::instance().record_error("display", "init_failed",
                                                  "no display backend available");
        // lv_deinit() first, then lv_xml_deinit() — the teardown order shutdown()
        // documents. Nothing is registered yet on this path, so either order works
        // today; keeping one order everywhere is what stops the shutdown ordering
        // bug from being reintroduced by copying this block.
        lv_deinit();
        lv_xml_deinit();
        return false;
    }

    spdlog::debug("[DisplayManager] Using backend: {}", m_backend->name());

    // Determine display dimensions
    m_width = config.width;
    m_height = config.height;
    m_size_was_explicit = config.size_was_explicit;

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

    // Propagate the "user explicitly asked for -s WxH" flag into the backend so
    // it can log warnings / enqueue toasts on fallback. Virtual dispatch: only
    // fbdev/DRM act on it; SDL ignores it.
    auto propagate_size_explicit = [&](DisplayBackend* backend) {
        if (backend) {
            backend->set_size_was_explicit(config.size_was_explicit);
        }
    };
    propagate_size_explicit(m_backend.get());

    // Create LVGL display
    m_display = m_backend->create_display(m_width, m_height);

    // If the primary backend failed to create a display, try falling back
    // to a different backend in-process (e.g., DRM passed is_available()
    // but mode setting or buffer allocation failed → try fbdev).
    if (DisplayBackend::should_try_fbdev_fallback(m_backend.get(), m_display)) {
        spdlog::warn("[DisplayManager] {} backend failed to create display, "
                     "attempting fbdev fallback",
                     m_backend->name());
        m_backend.reset();
        m_backend = DisplayBackend::create(DisplayBackendType::FBDEV);
        if (m_backend && m_backend->is_available()) {
            if (config.splash_active) {
                m_backend->set_splash_active(true);
            }
            propagate_size_explicit(m_backend.get());
            m_display = m_backend->create_display(m_width, m_height);
            if (m_display) {
                spdlog::info("[DisplayManager] Fbdev fallback succeeded at {}x{}", m_width,
                             m_height);
                warn_fbdev_high_dpi();
            }
        }
    }

    if (!m_display) {
        spdlog::error("[DisplayManager] Failed to create display (all backends exhausted)");
        TelemetryManager::instance().record_error("display", "init_failed",
                                                  "all display backends exhausted");
        m_backend.reset();
        // Same teardown order as above and as shutdown().
        lv_deinit();
        lv_xml_deinit();
        return false;
    }

    if (m_backend->is_gpu_accelerated()) {
        spdlog::info("[Display] Rendering: GPU-accelerated (OpenGL ES via EGL)");
    } else if (m_backend->type() == DisplayBackendType::DRM) {
        spdlog::info("[Display] Rendering: CPU (DRM dumb buffers)");
    }

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
    //
    // Also before create_input_pointer() below, deliberately: the backends'
    // stored-touch-range gate asks display_rotation_degrees() (the same helper
    // the calibration solver gates on, #1394), and that reads the display
    // rather than `/display/rotate` precisely because the block below draws
    // on three inputs the key does not see. Move input creation above this
    // block and both gates go blind again.
    apply_startup_rotation(config);

    // Initialize UI update queue for thread-safe async updates
    // Creates the queue's drain timer, which needs lv_init() but not a display
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
                "Add \"touch_device\": \"/dev/input/event1\" to settings.json",
                nullptr};

            ui_show_fatal_error("No Input Device",
                                "Could not find or open a touch/pointer input device.\n"
                                "The UI requires an input device to function.",
                                suggestions, 30000);

            m_backend.reset();
            // Order matters here, unlike the two paths above: ui_show_fatal_error()
            // has just built and shown a widget tree. Freeing the component scopes
            // first would free styles those widgets still point at, and lv_deinit()
            // runs layout passes while tearing them down — the use-after-free
            // shutdown() hit. Destroy the widgets first, then the scopes.
            lv_deinit();
            lv_xml_deinit();
            return false;
        }
#else
        // On desktop (SDL), continue without pointer - mouse is optional
        spdlog::warn("[DisplayManager] No pointer input device created - touch/mouse disabled");
#endif
    }

    // Configure scroll behavior and sleep-aware wrapper
    m_refresh_timing = helix::refresh_timing_from_env();
    finish_input_setup(config.scroll_throw, config.scroll_limit);
    spdlog::info("[DisplayManager] Refresh pacing: period {} ms (0 = LVGL default, scope {}), "
                 "screensaver {} ms (0 = global period), loop floor {} ms, {} ms while a "
                 "screensaver runs",
                 m_refresh_timing.refr_period_ms, m_refresh_timing.scope_all ? "all" : "display",
                 m_refresh_timing.screensaver_refr_period_ms, m_refresh_timing.loop_min_sleep_ms,
                 m_refresh_timing.screensaver_loop_min_sleep_ms);

#if HELIX_ENABLE_SCREENSAVER
    // The gate halves the screensaver budget during prints and keys stored levels by display path.
    ScreensaverManager::instance().set_host(screensaver_host(m_backend.get()));
#endif

    // Create backlight backend (auto-detects hardware)
    m_backlight = BacklightBackend::create();
    spdlog::info("[DisplayManager] Backlight: {} (available: {})", m_backlight->name(),
                 m_backlight->is_available());

    // Resolve hardware vs software blank strategy.
    // Config override: /display/hardware_blank (0 or 1). Missing (-1) = auto-detect.
    bool use_hardware_blank = false;
    {
        int hw_blank_override =
            helix::Config::get_instance()->get<int>("/display/hardware_blank", -1);
        if (hw_blank_override >= 0) {
            use_hardware_blank = (hw_blank_override != 0);
            spdlog::info("[DisplayManager] Hardware blank: {} (config override)",
                         use_hardware_blank);
        } else {
            use_hardware_blank = m_backlight && m_backlight->supports_hardware_blank();
            spdlog::info("[DisplayManager] Hardware blank: {} (auto-detected from {})",
                         use_hardware_blank, m_backlight ? m_backlight->name() : "none");
        }
    }

    // Real panel power-off (FB_BLANK_POWERDOWN / DRM connector DPMS-off) is the
    // last resort for a panel with no controllable backlight. A device with a
    // usable backlight sleeps by writing brightness 0 in enter_sleep(), because a
    // power-down its driver does not expect can wedge the display engine or leave
    // the panel lit on a no-signal pattern. See should_use_power_off().
    //
    // Config override: /display/panel_power_off (0 or 1). Missing (-1) = auto.
    // 1 cuts the panel on hardware whose backlight write leaves the LEDs lit
    // (#1594); 0 keeps it powered on a panel that does not recover from it.
    bool has_usable_backlight = m_backlight && m_backlight->is_available();
    bool backend_can_power_off = m_backend && m_backend->supports_power_off();
    bool use_power_off = false;
    {
        int power_off_override =
            helix::Config::get_instance()->get<int>("/display/panel_power_off", -1);
        if (power_off_override >= 0) {
            use_power_off = (power_off_override != 0) && backend_can_power_off;
            spdlog::info("[DisplayManager] Display power-off: {} (config override)", use_power_off);
        } else {
            use_power_off = helix::should_use_power_off(use_hardware_blank, has_usable_backlight,
                                                        backend_can_power_off);
            spdlog::info("[DisplayManager] Display power-off: {} ({})", use_power_off,
                         use_power_off
                             ? m_backend->name()
                             : (has_usable_backlight ? "backlight off (no panel power-off)"
                                                     : "software overlay fallback"));
        }
    }

    // Force backlight ON at startup - ensures display is visible even if
    // previous instance left it off or in an unknown state
    if (m_backlight && m_backlight->is_available()) {
        m_backlight->set_brightness(100);
        spdlog::debug("[DisplayManager] Backlight forced ON at 100% for startup");

        schedule_brightness_reassert();
    }

    // Dim and sleep behavior
    helix::Config* cfg = helix::Config::get_instance();
    DisplaySleepConfig sleep_config;
    sleep_config.use_hardware_blank = use_hardware_blank;
    sleep_config.use_power_off = use_power_off;
    sleep_config.dim_timeout_sec = cfg->get<int>("/display/dim_sec", 600);
    sleep_config.dim_brightness_percent =
        std::clamp(cfg->get<int>("/display/dim_brightness", 30), 1, 100);
    spdlog::debug("[DisplayManager] Display dim: {}s timeout, {}% brightness",
                  sleep_config.dim_timeout_sec, sleep_config.dim_brightness_percent);

    // Whether to power off the backlight during display sleep.
    // Default true (most platforms). Set to false on platforms where backlight
    // power-off prevents wake-on-touch (e.g. AD5X). When false, the software
    // overlay makes the screen appear off while the backlight stays powered.
    sleep_config.sleep_backlight_off = cfg->get<bool>("/display/sleep_backlight_off", true);
    if (!sleep_config.sleep_backlight_off) {
        spdlog::info("[DisplayManager] Backlight will stay on during sleep (config override)");
    }
    m_sleep.configure(sleep_config);

    // Debug touch visualization: draw ripple at each touch point.
    install_debug_touch_timer();

    spdlog::trace("[DisplayManager] Initialized: {}x{}", width(), height());
    m_initialized = true;
    set_active_instance(this);

    // Install framebuffer color transform hook AFTER the backend's flush_cb
    // is set, so the splash-suspend path captures our wrapper (#803).
    install_color_transform_hook();

    // Gate the remote-screen fb0 mirror on the platform-hook env export. When set
    // (Snapmaker U1 fbdev path), attach the fb0 sink so rendered frames also land
    // in /dev/fb0 for the firmware's fb-http snapshot daemon.
    if (const char* dev = std::getenv("HELIX_REMOTE_SCREEN_FB0")) {
        auto sink = std::make_unique<helix::Fb0MailboxSink>(dev);
        m_remote_screen.add_sink(std::move(sink));
        m_remote_screen.start();
    }
    {
        helix::Config* cfg = helix::Config::get_instance();
        float gamma = static_cast<float>(cfg->get<double>("/display/gamma", 1.0));
        int warmth = cfg->get<int>("/display/warmth", 0);
        int tint = cfg->get<int>("/display/tint", 0);
        float r_gain = static_cast<float>(cfg->get<double>("/display/r_gain", 1.0));
        float g_gain = static_cast<float>(cfg->get<double>("/display/g_gain", 1.0));
        float b_gain = static_cast<float>(cfg->get<double>("/display/b_gain", 1.0));
        m_color_transform.set_panel_gain(r_gain, g_gain, b_gain);
        m_color_transform.set(gamma, warmth, tint);
        if (!m_color_transform.is_identity()) {
            spdlog::info("[DisplayManager] Color transform active: gamma={:.2f}, "
                         "warmth={}, tint={}, panel_gain=({:.3f},{:.3f},{:.3f})",
                         gamma, warmth, tint, r_gain, g_gain, b_gain);
        }
    }

    return true;
}

DisplayManager* DisplayManager::instance() {
    return s_instance;
}

void DisplayManager::set_active_instance(DisplayManager* dm) {
    s_instance = dm;
}

void DisplayManager::shutdown() {
    // Ahead of the initialized check: a handler can be installed on a manager that
    // never ran init(), and its event callback holds this manager either way.
    remove_resize_handler();
    if (!m_initialized) {
        return;
    }

    m_shutting_down = true;
    set_active_instance(nullptr);
    spdlog::debug("[DisplayManager] Shutting down");

    // Stop the remote-screen mirror FIRST so no sink write races a freed
    // framebuffer during the LVGL/display teardown below.
    m_remote_screen.stop();

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

    m_sleep.abandon_lvgl_state();
    m_brightness_reassert_timer.reset();

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

    // Deinitialize LVGL FIRST, then the helix-xml engine.
    //
    // A component scope owns the styles its instances use: component_scope_free()
    // clears scope->style_ll, freeing every lv_style_t in it. Widgets keep raw
    // pointers to those styles, so freeing the scopes while the widget tree is
    // still standing leaves live objects pointing at reclaimed style memory —
    // and lv_deinit()'s own teardown runs layouts as it goes, so a flex pass
    // reads a freed style before the object is deleted (heap-use-after-free in
    // get_prop_core, via lv_obj_get_style_flex_grow).
    //
    // The reverse order is safe: lv_deinit() only needs the widget tree and its
    // own allocator, neither of which the XML engine owns, and lv_free() here is
    // plain free() (LV_USE_STDLIB_MALLOC = clib), so releasing scopes afterwards
    // needs nothing from LVGL. The <subject_expr> observers detached below are
    // attached to app-owned subjects, not objects, so lv_deinit() leaves them
    // alone for lv_xml_deinit() to remove — which is why theme_manager_deinit()
    // must run after all of this (see Application::shutdown()).
    if (lv_is_initialized()) {
        lv_deinit();
    }

    // Frees component scopes, their styles, fonts and <subject_expr> observers.
    lv_xml_deinit();

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

void DisplayManager::configure_pointer(int scroll_throw, int scroll_limit) {
    watch_pointer();
    configure_scroll(scroll_throw, scroll_limit);
    // Long-press threshold — user-configurable global setting (#1245), default
    // app_constants::input::kLongPressMs. InputSettingsManager::set_long_press_time
    // live-applies changes.
    auto* cfg = helix::Config::get_instance();
    const int long_press_ms = cfg->get<int>("/input/long_press_time",
                                            static_cast<int>(app_constants::input::kLongPressMs));
    lv_indev_set_long_press_time(m_pointer, long_press_ms);

    // Config rather than InputSettingsManager: this runs before its subjects exist.
    // Both read the same key; the Settings toggle asks for a restart.
    m_scroll_guard = helix::ScrollClickGuard::from_settings(
        cfg->get<bool>("/input/scroll_guard", false),
        cfg->get<int>("/input/scroll_guard_cooldown_ms",
                      static_cast<int>(helix::ScrollClickGuard::kDefaultCooldownMs)),
        std::getenv("HELIX_SCROLL_GUARD"), std::getenv("HELIX_SCROLL_GUARD_COOLDOWN_MS"),
        scroll_limit);

    // SDL's event handler identifies the mouse device by checking if
    // read_cb == sdl_mouse_read, which a wrapper breaks.
    // Callback chain: sleep_aware_read_cb (runs the scroll guard) -> calibrated_read_cb
    // -> evdev_read_cb (calibrated_read installed by backend, sleep wrapper installed here)
    if (m_backend->type() != DisplayBackendType::SDL) {
        install_sleep_aware_input_wrapper();
    }
    if (m_scroll_guard.enabled && !m_original_pointer_read_cb) {
        spdlog::info("[DisplayManager] Post-scroll click guard not applied: the {} pointer "
                     "has no input wrapper",
                     m_backend->name());
    } else if (m_scroll_guard.enabled) {
        spdlog::info("[DisplayManager] Post-scroll click guard: {} ms cooldown, {} px scroll limit",
                     m_scroll_guard.cooldown_ms, m_scroll_guard.scroll_limit_px);
    }
}

void DisplayManager::watch_pointer() {
    m_indev_delete_watch.watch(m_pointer, &m_pointer);
}

void DisplayManager::watch_keyboard() {
    m_indev_delete_watch.watch(m_keyboard, &m_keyboard);
}

void DisplayManager::debug_touch_tick(lv_timer_t* /*t*/) {
    if (!RuntimeConfig::debug_touches())
        return;

    auto* dm = DisplayManager::instance();
    if (!dm)
        return;

    // Suppress while the touch-calibration UI is active: it draws its own
    // (correct) ripple, and during point capture affine is disabled — so
    // lv_indev_get_point() returns RAW coords and this would draw a
    // Y-inverted ripple, which looks like a calibration bug but isn't
    // (prestonbrown/helixscreen#943).
    if (dm->is_touch_calibration_active())
        return;

    // Read the current pointer through the manager rather than a copy
    // captured at timer-creation time, so an unplug (which clears m_pointer)
    // is seen here too instead of reading a freed indev.
    auto* indev = dm->pointer_input();
    if (!indev)
        return;

    lv_indev_state_t state = lv_indev_get_state(indev);
    if (state != LV_INDEV_STATE_PRESSED)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    static lv_coord_t last_x = -100, last_y = -100;
    lv_coord_t dx = point.x - last_x;
    lv_coord_t dy = point.y - last_y;
    if (dx * dx + dy * dy < 25) // <5px movement
        return;

    last_x = point.x;
    last_y = point.y;
    helix::ui::create_ripple(lv_layer_top(), point.x, point.y, 10, 40, 300);
}

lv_timer_t* DisplayManager::install_debug_touch_timer() {
    if (!m_pointer) {
        return nullptr;
    }
    // Timer runs unconditionally; flag is checked inside so the Settings
    // toggle takes effect without a restart.
    return lv_timer_create(&DisplayManager::debug_touch_tick, 30, nullptr);
}

void DisplayManager::finish_input_setup(int scroll_throw, int scroll_limit) {
    if (m_pointer) {
        configure_pointer(scroll_throw, scroll_limit);
    }

    // Create keyboard input device (optional)
    create_keyboard_input();

    // Refresh pacing overrides, now that the refresh, animation, input and update-queue
    // timers they set all exist.
    helix::apply_refresh_timing(m_refresh_timing);
}

void DisplayManager::create_keyboard_input() {
    m_keyboard = m_backend->create_input_keyboard();
    DisplaySettingsManager::instance().set_hardware_keyboard_present(
        m_backend->has_hardware_keyboard());
    if (m_keyboard) {
        watch_keyboard();
        setup_keyboard_group();
        spdlog::trace("[DisplayManager] Keyboard input enabled");
    }
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

void DisplayManager::restore_flush_cb(lv_display_flush_cb_t flush_cb) {
    if (!m_display || !flush_cb) {
        return;
    }
    lv_display_set_flush_cb(m_display, flush_cb);
    if (m_backend) {
        m_backend->request_full_upload();
    }
}

// ============================================================================
// Display sleep forwarders
// ============================================================================

void DisplayManager::check_display_sleep() {
    m_sleep.tick();
}

void DisplayManager::wake_display() {
    m_sleep.wake();
}

void DisplayManager::ensure_display_on() {
    m_sleep.ensure_on();
}

void DisplayManager::set_dim_timeout(int seconds) {
    m_sleep.set_dim_timeout(seconds);
}

void DisplayManager::restore_display_on_shutdown() {
    m_sleep.restore_on_shutdown();
}

void DisplayManager::hold_flush_for_power_off() {
    m_sleep.hold_flush_for_power_off();
}

#if HELIX_ENABLE_SCREENSAVER
helix::ui::SaverHost DisplayManager::screensaver_host(const DisplayBackend* backend) {
    helix::ui::SaverHost host;
    host.is_printing = [] {
        return job_holds_machine(get_printer_state().print_state().get_print_lifecycle());
    };
    // The remaining time the print status panel shows, smoothed by PrintEtaEstimator.
    host.print_status = [] {
        helix::PrinterPrintState& print = get_printer_state().print_state();
        helix::ui::ClockPrint status;
        status.phase = helix::ui::clock_print_phase(print.get_print_lifecycle());
        status.remaining_s = lv_subject_get_int(print.get_print_time_left_subject());
        status.progress_pct = lv_subject_get_int(print.get_print_progress_display_subject());
        return status;
    };
    host.display_backend =
        backend ? helix::ui::display_backend_key(backend->type(), backend->is_gpu_accelerated())
                : "unknown";
    return host;
}

void DisplayManager::preview_screensaver(int type) {
    m_sleep.preview_screensaver(type);
}
#endif

void DisplayManager::set_backlight_brightness(int percent) {
    percent = std::clamp(percent, 0, 100);
    if (m_backlight) {
        m_backlight->set_brightness(percent);
    }
}

bool DisplayManager::has_backlight_control() const {
    return m_backlight && m_backlight->is_available();
}

bool DisplayManager::has_dimming_control() const {
    return m_backlight && m_backlight->supports_dimming();
}

bool DisplayManager::is_software_rotated() const {
    return m_display && m_backend && m_backend->type() == DisplayBackendType::FBDEV &&
           m_backend->applied_rotation_degrees(m_display) != 0;
}

// ============================================================================
// Touch Calibration
// ============================================================================

bool DisplayManager::apply_touch_calibration(const helix::TouchCalibration& cal) {
    if (!cal.valid) {
        spdlog::debug("[DisplayManager] Invalid calibration");
        return false;
    }
    if (!m_backend) {
        return false;
    }
    return m_backend->set_calibration(cal);
}

helix::LiveTouchRange DisplayManager::current_touch_range() const {
    helix::TouchRangeDiagnostics diag;
    helix::LiveTouchRange live;
    if (helix::get_touch_range_diagnostics(diag) && diag.pipeline.configured_valid) {
        live.range.valid = true;
        live.range.swap_axes = diag.pipeline.swap_axes;
        live.range.min_x = diag.pipeline.min_x;
        live.range.max_x = diag.pipeline.max_x;
        live.range.min_y = diag.pipeline.min_y;
        live.range.max_y = diag.pipeline.max_y;
        live.source = diag.pipeline.source;
        if (live.source == helix::TouchRangeSource::Stored) {
            live.range.capture_rotation = diag.pipeline.stored.capture_rotation;
        }
    }
    return live;
}

helix::TouchCalibration DisplayManager::get_current_calibration() const {
    if (!m_backend) {
        return {};
    }
    return m_backend->get_calibration();
}

bool DisplayManager::needs_touch_calibration() const {
    if (!m_backend) {
        return false;
    }
    return m_backend->needs_touch_calibration();
}

bool DisplayManager::supports_touch_calibration() const {
    if (!m_backend) {
        return false;
    }
    return m_backend->supports_touch_calibration();
}

void DisplayManager::disable_affine_calibration() {
    if (m_backend) {
        m_backend->disable_affine_calibration();
    }
}

void DisplayManager::enable_affine_calibration() {
    if (m_backend) {
        m_backend->enable_affine_calibration();
    }
}

// ============================================================================
// Wake-touch press cancel
// ============================================================================

void DisplayManager::cancel_pointer_presses() {
    // A pointer whose read callback is not wrapped (SDL's mouse, which Android's touch also
    // arrives through) has already handed the wake touch to LVGL. lv_indev_reset() drops
    // that press's target, long-press clock and scroll, and lv_indev_wait_release() keeps
    // LVGL from re-pressing, scrolling or clicking anything until the finger lifts.
    // A wrapped pointer reports the wake touch as released, so only the reset applies.
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                lv_indev_wait_release(indev);
            }
            lv_indev_reset(indev, nullptr);
        }
        indev = lv_indev_get_next(indev);
    }
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

    // A press while asleep, dimmed or under a screensaver wakes the display, and LVGL sees
    // that touch as released until the finger lifts.
    const bool was_swallowing = dm->m_wake_latch.swallowing();
    const helix::WakeTouchLatch::Sample out = dm->m_wake_latch.filter(
        {data->state == LV_INDEV_STATE_PRESSED, data->point.x, data->point.y}, lv_tick_get(),
        [dm](int32_t x, int32_t y) { return dm->m_sleep.note_press(x, y); });
    data->state = out.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = out.x;
    data->point.y = out.y;
    if (was_swallowing && !dm->m_wake_latch.swallowing()) {
        spdlog::info("[DisplayManager] Wake touch released; input resumes");
    }

    // After the wake handling, so a suppressed press can never cost a wake request.
    dm->m_scroll_guard.filter(data->state, data->point, lv_tick_get());
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
// DRM→fbdev Fallback
// ============================================================================

bool DisplayManager::try_drm_to_fbdev_fallback(lv_display_rotation_t rot, bool splash_active) {
    if (m_backend->type() != DisplayBackendType::DRM ||
        m_backend->supports_hardware_rotation(rot)) {
        return true; // No fallback needed
    }

    // Input devices are bound to the DRM backend and the display freed below, so
    // the swap is only safe before init() creates them.
    if (m_pointer || m_keyboard) {
        spdlog::error("[DisplayManager] fbdev fallback requested after input devices exist; "
                      "continuing without rotation");
        return false;
    }

    spdlog::warn("[DisplayManager] DRM lacks hardware rotation for {}°, "
                 "falling back to fbdev (flicker-free software rotation)",
                 static_cast<int>(rot) * 90);
    // m_sleep reads m_backend and m_display through references, so the swap below needs no
    // retarget.
    lv_display_delete(m_display); // intentional: switching backend before lv_deinit
    m_display = nullptr;
    m_backend.reset();
    m_backend = DisplayBackend::create(DisplayBackendType::FBDEV);
    if (m_backend && m_backend->is_available()) {
        if (splash_active) {
            m_backend->set_splash_active(true);
        }
        m_backend->set_size_was_explicit(m_size_was_explicit);
        m_display = m_backend->create_display(m_width, m_height);
    }
    if (!m_display) {
        spdlog::error("[DisplayManager] Fbdev fallback for rotation also failed. "
                      "On a non-EGL build, use the kernel panel_orientation parameter "
                      "instead: add panel_orientation=right_side_up (or left_side_up, "
                      "upside_down) to /boot/firmware/cmdline.txt and remove the "
                      "\"rotate\" key from settings.json.");
        return false;
    }
    spdlog::info("[DisplayManager] Fbdev fallback succeeded at {}x{}", m_width, m_height);
    warn_fbdev_high_dpi();
    return true;
}

void DisplayManager::warn_fbdev_high_dpi() {
    static constexpr int kHighDpiThreshold = 1920;
    if (m_width <= kHighDpiThreshold && m_height <= kHighDpiThreshold) {
        return;
    }
    spdlog::warn("[DisplayManager] Fbdev resolution {}x{} exceeds {}px on one axis. "
                 "Cannot auto-downscale in fbdev mode. Configure a lower resolution "
                 "via kernel parameters (e.g., framebuffer_width/framebuffer_height "
                 "in /boot/firmware/config.txt on Raspberry Pi) and reboot.",
                 m_width, m_height, kHighDpiThreshold);
    char toast_msg[256];
    snprintf(toast_msg, sizeof(toast_msg),
             lv_tr("Display resolution is very high (%dx%d). Text may appear small. "
                   "Reduce framebuffer resolution in /boot/firmware/config.txt for "
                   "best results."),
             m_width, m_height);
    helix::PendingStartupWarnings::instance().enqueue(
        helix::PendingStartupWarnings::Severity::WARNING, toast_msg);
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

    // DRM backend: interactive rotation probe crashes because switching between
    // DIRECT and FULL render modes during probe triggers LVGL assertion in
    // layer_reshape_draw_buf(). DRM rotation is handled via kernel
    // panel_orientation auto-detection instead.
    // Guard at compile time: DRM builds link LVGL's DRM driver which uses
    // render modes incompatible with the probe, even if DRM init failed and
    // we fell back to fbdev.
#ifdef HELIX_DISPLAY_DRM
    spdlog::info("[DisplayManager] Rotation probe skipped — "
                 "use panel_orientation kernel parameter for auto-detection");
    return;
#else
    if (m_backend && m_backend->type() == DisplayBackendType::DRM) {
        spdlog::info("[DisplayManager] Rotation probe skipped — "
                     "use panel_orientation kernel parameter for auto-detection");
        return;
    }
#endif

    // On SDL, rotation doesn't visually rotate (DIRECT render mode limitation),
    // but the probe UI and tap detection still work for testing the flow.
    bool is_sdl = (m_backend && m_backend->type() == DisplayBackendType::SDL);
    if (is_sdl) {
        spdlog::info("[DisplayManager] Rotation probe on SDL: display won't rotate, "
                     "but UI and tap detection work for testing");
    }

    // m_width/m_height hold the unrotated size requested of the backend.
    const int phys_w = m_width;
    const int phys_h = m_height;

    spdlog::info("[DisplayManager] Starting rotation probe (physical={}x{})", phys_w, phys_h);

    helix::RotationProbeHost host{
        m_pointer, is_sdl,
        [this, phys_w, phys_h](lv_display_rotation_t rot) {
            settle_display_rotation(rot, phys_w, phys_h);
        },
        [this](bool suspended) { set_resize_fanout_suspended(suspended); }};
    helix::RotationProbe(std::move(host)).run();
}

void DisplayManager::apply_startup_rotation(const Config& config) {
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

    // A first-boot kernel panel_orientation arrives as config.rotation
    // (Application::init_display()). The board's mount is decided here, where
    // the framebuffer geometry that confirms it is known. The interactive
    // probe runs later, from Application::run_rotation_probe_and_layout().
    bool from_board_mount = false;
    if (rotation_degrees == 0 && config.first_boot_probe) {
        const int mount =
            helix::first_boot_board_mount(config.kernel_orientation, m_width, m_height);
        if (mount > 0) {
            spdlog::info("[DisplayManager] First boot: board panel mount {}° for {}x{}", mount,
                         m_width, m_height);
            rotation_degrees = mount;
            from_board_mount = true;
        }
    }

    // Apply rotation from config, env, or CLI
    if (rotation_degrees != 0) {
#ifdef HELIX_DISPLAY_SDL
        // LVGL's SDL driver only supports software rotation in PARTIAL render mode,
        // but we use DIRECT mode for performance. Skip rotation on SDL — it's only
        // for desktop dev. On embedded (fbdev/DRM) rotation works correctly.
        spdlog::warn("[DisplayManager] Rotation {}° requested but SDL backend does not "
                     "support software rotation (DIRECT render mode). Ignoring on desktop.",
                     rotation_degrees);
#else
        int phys_w = m_width;
        int phys_h = m_height;

        lv_display_rotation_t lv_rot = degrees_to_lv_rotation(rotation_degrees);

        // If DRM backend can't do hardware rotation, fall back to fbdev
        // which handles software rotation flicker-free via LVGL's native path.
        if (!try_drm_to_fbdev_fallback(lv_rot, config.splash_active)) {
            // Fallback failed (a DSI display on a non-EGL build, with no fbdev).
            // Continue without rotation rather than aborting — a
            // working unrotated display is better than no display.
            // lv_display_set_rotation() is deliberately NOT called, which
            // is what makes the display the honest source: `/display/rotate`
            // still says 90/180/270 here while nothing is rotated, and a
            // gate believing the key would throw away a perfectly good
            // stored touch range on every boot of such a unit.
            spdlog::warn("[DisplayManager] Continuing without rotation. "
                         "On a non-EGL build, use panel_orientation in "
                         "/boot/firmware/cmdline.txt instead.");
            rotation_degrees = 0;
        } else {
            // After the fallback, which may have swapped m_backend.
            settle_display_rotation(lv_rot, phys_w, phys_h);
        }

        spdlog::info("[DisplayManager] Display rotated {}° — effective resolution: {}x{}",
                     rotation_degrees, width(), height());
#endif
    }
    if (config.first_boot_probe) {
        m_first_boot_orientation = config.kernel_orientation >= 0 ? config.kernel_orientation
                                   : from_board_mount && rotation_degrees > 0 ? rotation_degrees
                                                                              : -1;
    }
}

void DisplayManager::settle_display_rotation(lv_display_rotation_t rot, int phys_w, int phys_h) {
    // The backend may clear LVGL's rotation when the scanout plane rotates instead (#1275,
    // #1587), which is why width()/height() read the display rather than a cache.
    m_backend->set_display_rotation(m_display, rot, phys_w, phys_h);
}

// ============================================================================
// Window Resize Handler (Desktop/SDL)
// ============================================================================

void DisplayManager::fan_out_resize() {
    if (m_shutting_down) {
        return;
    }

    // A timer armed before the suspension began must not fan out either - the
    // rotation probe polls for taps through lv_timer_handler(), and this is the
    // callback that can sit in a multi-second theme refresh while it does.
    if (m_resize_fanout_suspended) {
        spdlog::debug("[DisplayManager] Resize fanout suspended - dropping pending debounce");
        return;
    }

    spdlog::debug(
        "[DisplayManager] Resize debounce complete: {}x{}, calling {} registered callbacks",
        width(), height(), m_resize_callbacks.size());

    // The debounce is disarmed before this runs, so a SIZE_CHANGED raised by a
    // callback arms a fresh one. A callback may register another (panels do so
    // lazily), which can reallocate the vector: index it, and stop at the count
    // this resize started with so a newcomer runs from the next resize on.
    const size_t count = m_resize_callbacks.size();
    for (size_t i = 0; i < count; i++) {
        if (ResizeCallback callback = m_resize_callbacks[i]) {
            callback();
        }
    }
}

void DisplayManager::on_screen_size_changed(lv_obj_t* screen) {
    if (m_shutting_down) {
        return;
    }

    lv_coord_t width = lv_obj_get_width(screen);
    lv_coord_t height = lv_obj_get_height(screen);

    if (m_resize_fanout_suspended) {
        spdlog::debug("[DisplayManager] Screen size changed to {}x{} while resize fanout "
                      "suspended - no debounce armed",
                      width, height);
        return;
    }

    spdlog::debug("[DisplayManager] Screen size changed to {}x{}, resetting debounce timer", width,
                  height);
    m_resize_debounce.schedule([this]() { fan_out_resize(); });
}

void DisplayManager::resize_event_cb(lv_event_t* e) {
    auto* self = static_cast<DisplayManager*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    switch (lv_event_get_code(e)) {
    case LV_EVENT_SIZE_CHANGED:
        self->on_screen_size_changed(static_cast<lv_obj_t*>(lv_event_get_target(e)));
        break;
    case LV_EVENT_DELETE:
        // LVGL drops the screen's callbacks with it, so there is nothing left to detach.
        self->m_resize_screen = nullptr;
        break;
    default:
        break;
    }
}

void DisplayManager::init_resize_handler(lv_obj_t* screen) {
    if (!screen) {
        spdlog::error("[DisplayManager] Cannot init resize handler: screen is null");
        return;
    }

    remove_resize_handler();
    lv_obj_add_event_cb(screen, resize_event_cb, LV_EVENT_SIZE_CHANGED, this);
    lv_obj_add_event_cb(screen, resize_event_cb, LV_EVENT_DELETE, this);
    m_resize_screen = screen;

    spdlog::trace("[DisplayManager] Resize handler initialized on screen");
}

void DisplayManager::remove_resize_handler() {
    m_resize_debounce.cancel();
    // m_resize_screen is cleared when the screen is deleted, so a non-null one is live.
    if (m_resize_screen && lv_is_initialized()) {
        lv_obj_remove_event_cb_with_user_data(m_resize_screen, resize_event_cb, this);
    }
    m_resize_screen = nullptr;
}

void DisplayManager::set_resize_fanout_suspended(bool suspended) {
    if (m_resize_fanout_suspended == suspended) {
        return;
    }
    m_resize_fanout_suspended = suspended;
    spdlog::debug("[DisplayManager] Resize callback fanout {}",
                  suspended ? "suspended" : "resumed");
}

void DisplayManager::register_resize_callback(ResizeCallback callback) {
    if (!callback) {
        spdlog::warn("[DisplayManager] Attempted to register null resize callback");
        return;
    }

    // Deduplicate — same function pointer may be registered on panel re-activation
    if (std::find(m_resize_callbacks.begin(), m_resize_callbacks.end(), callback) !=
        m_resize_callbacks.end()) {
        return;
    }

    m_resize_callbacks.push_back(callback);
    spdlog::trace("[DisplayManager] Registered resize callback ({} total)",
                  m_resize_callbacks.size());
}

// ============================================================================
// Color Transform (gamma + warmth)
// ============================================================================

void DisplayManager::install_color_transform_hook() {
    if (!m_display) {
        return;
    }
    if (m_original_flush_cb_for_color) {
        return; // Already installed
    }
    m_original_flush_cb_for_color = m_display->flush_cb;
    if (!m_original_flush_cb_for_color) {
        return;
    }
    lv_display_set_flush_cb(m_display, [](lv_display_t* d, const lv_area_t* area, uint8_t* px_map) {
        DisplayManager* self = DisplayManager::instance();
        if (self && area && px_map) {
            // Apply the per-channel color transform in place (only when non-identity).
            if (!self->m_color_transform.is_identity()) {
                const lv_color_format_t cf = lv_display_get_color_format(d);
                const int w = lv_area_get_width(area);
                const int h = lv_area_get_height(area);
                const auto reg = helix::ColorTransform::select_flush_region(
                    lv_display_get_buf_active(d), *area, cf, lv_display_get_render_mode(d));
                self->m_color_transform.apply_area(px_map, reg.stride_bytes, reg.x, reg.y, w, h,
                                                   cf);
            }
            // Mirror the (post-transform) pixels to any remote-screen sink. Runs
            // on every flush regardless of the color transform (the U1 has none).
            if (self->m_remote_screen.any_active()) {
                const lv_color_format_t cf = lv_display_get_color_format(d);
                helix::RemoteScreenFrame f;
                f.px_map = px_map;
                f.x1 = area->x1;
                f.y1 = area->y1;
                f.x2 = area->x2;
                f.y2 = area->y2;
                f.disp_w = lv_display_get_horizontal_resolution(d);
                f.disp_h = lv_display_get_vertical_resolution(d);
                f.color_format = (int)cf;
                // Same dbuf-stride-or-fallback rule the colour-transform walk
                // uses; the shared helper owns the derivation (#1610).
                lv_draw_buf_t* dbuf = lv_display_get_buf_active(d);
                f.src_stride = helix::flush_px_map_stride(dbuf, lv_area_get_width(area), cf);
                // Hand the sink the real readable length so it never has to guess
                // one from stride * disp_h. Only claim it when px_map IS the active
                // draw buffer: with screen rotation (and any other backend that
                // flushes from a scratch buffer) px_map belongs to a different
                // allocation whose size we do not know, and 0 tells the sink so.
                if (dbuf && dbuf->data == px_map && dbuf->data_size > 0) {
                    f.px_map_len = static_cast<size_t>(dbuf->data_size);
                }
                // Declare where px_map's pixel (0,0) sits on the display. In
                // partial mode lv_refr.c reshapes the draw buffer to the dirty
                // area and flushes from its origin, so the rect's pixels start
                // at row 0 rather than at their absolute coordinates; the sink
                // has no way to tell the two layouts apart on its own. Direct
                // and full mode keep the buffer origin, i.e. (0,0) (#1334).
                if (lv_display_get_render_mode(d) == LV_DISPLAY_RENDER_MODE_PARTIAL) {
                    f.px_map_x = area->x1;
                    f.px_map_y = area->y1;
                }
                // Map the LVGL render format to our LVGL-independent sink enum.
                // The U1 DRM dumb buffer is RGB565 (16bpp); desktop/other paths
                // are ARGB8888/XRGB8888 (32bpp, BGRA in memory).
                switch (cf) {
                case LV_COLOR_FORMAT_RGB565:
                    f.src_format = helix::RemoteScreenPixelFormat::RGB565;
                    break;
                case LV_COLOR_FORMAT_ARGB8888:
                case LV_COLOR_FORMAT_XRGB8888:
                    f.src_format = helix::RemoteScreenPixelFormat::BGRA8888;
                    break;
                default:
                    f.src_format = helix::RemoteScreenPixelFormat::Unknown;
                    break;
                }
                f.last_in_refresh = lv_display_flush_is_last(d);
                self->m_remote_screen.on_frame(f);
            }
        }
        // Forward to the original backend flush
        if (self && self->m_original_flush_cb_for_color) {
            self->m_original_flush_cb_for_color(d, area, px_map);
        } else {
            lv_display_flush_ready(d);
        }
    });
    spdlog::debug("[DisplayManager] Color transform flush hook installed");
}

void DisplayManager::set_color_transform(float gamma, int warmth, int tint) {
    m_color_transform.set(gamma, warmth, tint);
    if (m_display) {
        // Force a full repaint so the new LUT is visible immediately.
        lv_obj_invalidate(lv_display_get_screen_active(m_display));
        // Every pixel the backend already holds carries the previous transform.
        if (m_backend) {
            m_backend->request_full_upload();
        }
    }
    spdlog::info("[DisplayManager] Color transform: gamma={:.2f}, warmth={}, tint={} (identity={})",
                 gamma, warmth, tint, m_color_transform.is_identity());
}
