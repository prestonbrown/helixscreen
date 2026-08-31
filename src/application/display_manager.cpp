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
#include "config.h"
#include "display/lv_display_private.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "lvgl_log_handler.h"
#include "printer_state.h"
#include "remote_screen_fb0_sink.h"
#include "runtime_config.h"
#include "tap_latch.h"
#ifdef HELIX_ENABLE_SCREENSAVER
#include "ui_nav_manager.h"

#include "screensaver.h"
#endif

#include "ui_lock_screen.h"

#include "lock_manager.h"
#include "pending_startup_warnings.h"
#include "system/telemetry_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
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

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <SDL_system.h>
#include <jni.h>

// ---------------------------------------------------------------------------
// JNI bridge to HelixActivity's window flags (#1245)
//
// Mirrors android_set_navbar_always_visible() in display_settings_manager.cpp —
// same guard shape, same ExceptionClear() on every failure path, and the same
// shared helix_activity_class() for class resolution. It lives HERE rather than
// being exported from display_settings_manager.h because DisplayManager is the
// only caller: which mechanism cuts the panel is display-output policy, not a
// persisted setting.
// Putting an Android-only declaration in the settings header to reach it would
// file the API under the wrong owner. There is exactly one copy of each helper.
//
// Note we do NOT use SDL_EnableScreenSaver()/SDL_DisableScreenSaver(), which
// reach the same window flag via COMMAND_SET_KEEP_SCREEN_ON: they early-return
// when SDL's cached suspend_screensaver already matches, so a re-assert after
// Android recreates the window is silently dropped. HelixActivity keeps the
// desired state in a static and re-applies it from onResume(), which is the
// behaviour we actually need.
// ---------------------------------------------------------------------------

/// Ask HelixActivity to add/clear FLAG_KEEP_SCREEN_ON (applied on the UI thread).
static void android_set_keep_screen_on(bool keep_on) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return;

    // Cached global ref owned by helix_activity_class() — never released here.
    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return;

    jmethodID method = env->GetStaticMethodID(cls, "setKeepScreenOn", "(Z)V");
    if (!method) {
        env->ExceptionClear();
        return;
    }

    env->CallStaticVoidMethod(cls, method, static_cast<jboolean>(keep_on));
}

/// Read HelixActivity's onResume counter. Returns 0 when the bridge is
/// unavailable; both the sleep-entry capture and the idle poll go through this
/// same function, so a bridge that is uniformly broken reads 0 == 0 and simply
/// never self-wakes. A bridge that breaks *between* the two reads costs one
/// spurious wake, which is the harmless direction (the panel is already lit).
static int android_get_resume_seq() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return 0;

    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return 0;

    jmethodID method = env->GetStaticMethodID(cls, "getResumeSeq", "()I");
    if (!method) {
        env->ExceptionClear();
        return 0;
    }

    jint seq = env->CallStaticIntMethod(cls, method);
    return static_cast<int>(seq);
}
#endif // __ANDROID__

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

        // Kernel auto-detection and interactive probing are handled by
        // Application::run_rotation_probe_and_layout(), which checks both
        // rotation_probed and has_rotate_key before overwriting config.

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
                // Fallback failed (EGL/DSI display without fbdev).
                // Continue without rotation rather than aborting — a
                // working unrotated display is better than no display.
                spdlog::warn("[DisplayManager] Continuing without rotation. "
                             "For DSI/EGL displays, use panel_orientation in "
                             "/boot/firmware/cmdline.txt instead.");
                rotation_degrees = 0;
            } else {
                lv_display_set_rotation(m_display, lv_rot);

                // Update tracked dimensions to match rotated resolution
                m_width = lv_display_get_horizontal_resolution(m_display);
                m_height = lv_display_get_vertical_resolution(m_display);

                // Auto-rotate touch coordinates to match display rotation
                m_backend->set_display_rotation(lv_rot, phys_w, phys_h);
            }

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
    if (m_pointer) {
        configure_scroll(config.scroll_throw, config.scroll_limit);
        // Long-press threshold — user-configurable global setting (#1245), default
        // AppConstants::Input::LONG_PRESS_MS. Applied here and on backend swap;
        // InputSettingsManager::set_long_press_time live-applies changes.
        const int long_press_ms = helix::Config::get_instance()->get<int>(
            "/input/long_press_time", static_cast<int>(AppConstants::Input::LONG_PRESS_MS));
        lv_indev_set_long_press_time(m_pointer, long_press_ms);
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

    // Real panel power-off path (#1049): LAST RESORT only — for devices with NO
    // controllable backlight (generic HDMI panels / Backlight-None, like the
    // original #1049 reporter and the CB1). On those, FB_BLANK_POWERDOWN / DRM
    // connector DPMS-off is the only way to actually cut the panel.
    //
    // Any device WITH a usable backlight turns the backlight off instead (the
    // set_brightness(0) call in enter_sleep()). That is safer and avoids
    // driver-specific CRTC-disable wedges: on the Snapmaker U1 (working pwm
    // backlight, but Hardware blank: false + a DPMS-capable DRM connector), DRM
    // DPMS-off disables the Rockchip VOP2 CRTC and the panel goes PERMANENTLY
    // black — wake's DPMS-on does NOT reliably re-enable VOP2 (see
    // assets/config/platform/hooks-snapmaker-u1.sh "DRM CRTC keepalive"). So
    // gate power-off on having NEITHER a hardware blank NOR a usable backlight.
    bool has_usable_backlight = m_backlight && m_backlight->is_available();
    bool backend_can_power_off = m_backend && m_backend->supports_power_off();
    m_use_power_off =
        should_use_power_off(m_use_hardware_blank, has_usable_backlight, backend_can_power_off);
    spdlog::info("[DisplayManager] Display power-off: {} ({})", m_use_power_off,
                 m_use_power_off ? m_backend->name()
                                 : (has_usable_backlight ? "backlight off (no panel power-off)"
                                                         : "software overlay fallback"));

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
    m_dim_timeout_sec = cfg->get<int>("/display/dim_sec", 600);
    m_dim_brightness_percent = std::clamp(cfg->get<int>("/display/dim_brightness", 30), 1, 100);
    spdlog::debug("[DisplayManager] Display dim: {}s timeout, {}% brightness", m_dim_timeout_sec,
                  m_dim_brightness_percent);

    // Whether to power off the backlight during display sleep.
    // Default true (most platforms). Set to false on platforms where backlight
    // power-off prevents wake-on-touch (e.g. AD5X). When false, the software
    // overlay makes the screen appear off while the backlight stays powered.
    m_sleep_backlight_off = cfg->get<bool>("/display/sleep_backlight_off", true);
    if (!m_sleep_backlight_off) {
        spdlog::info("[DisplayManager] Backlight will stay on during sleep (config override)");
    }

    // Debug touch visualization: draw ripple at each touch point.
    // Timer runs unconditionally; flag is checked inside so the Settings
    // toggle takes effect without a restart.
    if (m_pointer) {
        lv_timer_create(
            [](lv_timer_t* t) {
                if (!RuntimeConfig::debug_touches())
                    return;

                // Suppress while the touch-calibration UI is active: it draws
                // its own (correct) ripple, and during point capture affine is
                // disabled — so lv_indev_get_point() returns RAW coords and this
                // would draw a Y-inverted ripple, which looks like a calibration
                // bug but isn't (prestonbrown/helixscreen#943).
                auto* cal_dm = DisplayManager::instance();
                if (cal_dm && cal_dm->is_touch_calibration_active())
                    return;

                auto* indev = static_cast<lv_indev_t*>(lv_timer_get_user_data(t));
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
            },
            30, m_pointer);
    }

    spdlog::trace("[DisplayManager] Initialized: {}x{}", m_width, m_height);
    m_initialized = true;
    s_instance = this;

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

void DisplayManager::shutdown() {
    if (!m_initialized) {
        return;
    }

    m_shutting_down = true;
    s_instance = nullptr;
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

    // Sleep overlay is an LVGL object freed by lv_deinit() — just clear the pointer.
    // Don't call destroy_sleep_overlay() here because lv_obj_delete() ordering
    // relative to other LVGL teardown is fragile.
    m_sleep_overlay = nullptr;
    m_use_hardware_blank = false;
    m_use_power_off = false;
    // Back to the startup truth (#1245): SDL re-asserts FLAG_KEEP_SCREEN_ON the
    // next time it initializes video, so a re-init must not think we still owe
    // Android a release.
    m_last_sleep_mechanism = SleepMechanism::SoftwareOverlay;
    m_keep_screen_on = true;
    m_resume_seq_at_sleep = 0;

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
    // Remember the values so a post-swap input rebuild (rotation fallback) can
    // reapply them to the freshly-created pointer without re-reading config.
    m_scroll_throw = scroll_throw;
    m_scroll_limit = scroll_limit;
    if (!m_pointer) {
        return;
    }

    lv_indev_set_scroll_throw(m_pointer, static_cast<uint8_t>(scroll_throw));
    lv_indev_set_scroll_limit(m_pointer, static_cast<uint8_t>(scroll_limit));
    spdlog::trace("[DisplayManager] Scroll config: throw={}, limit={}", scroll_throw, scroll_limit);
}

void DisplayManager::rebuild_input_after_backend_swap() {
    // A backend swap (DRM→fbdev rotation fallback) deleted the display and freed
    // the old backend. lv_display_delete() only detaches indevs (sets their
    // display to NULL) — it does not free them — so m_pointer/m_keyboard still
    // point at indevs bound to the gone backend, whose read_cb/user_data now
    // reference freed memory. Delete them and recreate on the current backend,
    // mirroring init()'s input setup so scroll/long-press/sleep-wrapper/keyboard
    // behavior is preserved.
    if (m_pointer) {
        lv_indev_delete(m_pointer); // NOTE: swap, not shutdown
        m_pointer = nullptr;
    }
    if (m_keyboard) {
        lv_indev_delete(m_keyboard); // NOTE: swap, not shutdown
        m_keyboard = nullptr;
    }
    // The saved read callback belonged to the deleted pointer; drop it so the
    // sleep-aware wrapper re-captures the new pointer's callback on reinstall.
    m_original_pointer_read_cb = nullptr;

    m_pointer = m_backend->create_input_pointer();
    if (m_pointer) {
        configure_scroll(m_scroll_throw, m_scroll_limit);
        const int long_press_ms = helix::Config::get_instance()->get<int>(
            "/input/long_press_time", static_cast<int>(AppConstants::Input::LONG_PRESS_MS));
        lv_indev_set_long_press_time(m_pointer, long_press_ms);
#ifndef HELIX_DISPLAY_SDL
        install_sleep_aware_input_wrapper();
#endif
    }

    m_keyboard = m_backend->create_input_keyboard();
    if (m_keyboard) {
        setup_keyboard_group();
    }

    spdlog::info("[DisplayManager] Input rebuilt after backend swap (pointer={}, keyboard={})",
                 m_pointer ? "ok" : "null", m_keyboard ? "ok" : "null");
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
#ifdef HELIX_ENABLE_SCREENSAVER
    // Stop screensaver before entering full sleep
    if (m_screensaver_active) {
        ScreensaverManager::instance().stop();
        m_screensaver_active = false;
    }
#endif
    m_display_sleeping = true;

    SleepMechanism mechanism =
        select_sleep_mechanism(platform_is_android(), m_use_hardware_blank,
                               m_use_power_off && m_backend != nullptr, timeout_sec);

    switch (mechanism) {
    case SleepMechanism::HardwareBlank:
        if (m_backend) {
            m_backend->blank_display();
        }
        break;

    case SleepMechanism::PanelPowerOff:
        // Real panel power-off (fbdev FB_BLANK_POWERDOWN / DRM DPMS off) for
        // HDMI/fbdev devices with no hardware backlight blank (#1049). The panel
        // is actually powered down, so no software overlay is needed. wake_display()
        // restores power BEFORE lv_refr_now() to honor the #303 wake-race.
        if (m_backend->power_off()) {
            // Neutralize the flush so the next page-flip can't re-assert DPMS-on
            // and relight the panel on the home screen. Without this, stopping the
            // screensaver above (or any later Klipper-driven invalidation) renders
            // a frame whose DRM commit turns the connector back ON — the
            // user-reported regression where an idle HDMI panel "comes back on at
            // the home screen".
            suppress_flush_for_sleep();
        } else {
            // The capability probe disagreed with reality; degrade to the overlay
            // rather than leaving a lit panel with no visual sleep at all.
            mechanism = SleepMechanism::SoftwareOverlay;
            create_sleep_overlay();
        }
        break;

    case SleepMechanism::HostSleep:
        // Android (#1245): no backlight sysfs and no backend blank/power-off, so
        // the only way to genuinely darken the panel is to stop asserting
        // FLAG_KEEP_SCREEN_ON and let Android's own display timeout run. Painting
        // a black overlay instead (what we used to do) left the panel fully lit
        // and blocked the device from ever sleeping. Deliberately no overlay: the
        // OS is about to power the panel, and the app gets paused with it.
        //
        // Remember the resume counter so the pause/resume round trip that follows
        // can be told apart from "still waiting for Android's timeout".
#ifdef __ANDROID__
        m_resume_seq_at_sleep = android_get_resume_seq();
#endif
        set_keep_screen_on(false);
        break;

    case SleepMechanism::SoftwareOverlay:
        // Software overlay path: do NOT call FBIOBLANK — the overlay alone is
        // sufficient and FBIOBLANK can cause a race condition on wake where the
        // framebuffer isn't ready before LVGL renders, leaving a black screen
        // even after the overlay is removed (#303).
        create_sleep_overlay();
        break;
    }
    m_last_sleep_mechanism = mechanism;

    if (m_backlight && m_backlight->is_available() && m_sleep_backlight_off) {
        m_backlight->set_brightness(0);
    }
    spdlog::info("[DisplayManager] Display sleeping ({}{}) after {}s",
                 sleep_mechanism_name(mechanism),
                 m_sleep_backlight_off ? "" : ", backlight kept on", timeout_sec);

    // Notify subscribers (camera stream, etc.) to suspend background work
    for (auto& cb : m_sleep_callbacks) {
        cb(true);
    }
}

// ============================================================================
// Host keep-screen-on (Android, #1245)
// ============================================================================

void DisplayManager::set_keep_screen_on(bool keep_on) {
#ifdef __ANDROID__
    if (m_keep_screen_on == keep_on) {
        return; // transition-guarded: don't cross JNI to re-say the same thing
    }
    m_keep_screen_on = keep_on;
    android_set_keep_screen_on(keep_on);
    spdlog::info("[DisplayManager] Android keep-screen-on: {}", keep_on);
#else
    // Nothing else runs a display timeout behind our back — we own the panel on
    // every non-Android target, so the flag has no meaning and stays asserted.
    (void)keep_on;
#endif
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
// Power-off flush suppression (#1049)
// ============================================================================

namespace {
// No-op flush used while the panel is powered off: it must still signal "ready"
// or LVGL stalls waiting for the flush to complete, but it commits nothing to the
// framebuffer — so no DRM page-flip happens to re-assert DPMS-on.
void sleep_noop_flush_cb(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* /*px*/) {
    lv_display_flush_ready(disp);
}
} // namespace

void DisplayManager::suppress_flush_for_sleep() {
    if (m_flush_suppressed_for_sleep || !m_display) {
        return;
    }
    // Stop invalidations from scheduling renders AND swap the flush callback for a
    // no-op. Pausing the refresh timer alone is insufficient: any invalidation
    // fires LV_EVENT_REFR_REQUEST, which resumes the timer (see the identical note
    // in Application's splash suppression). With the flush neutralized, even a
    // render that slips through commits nothing, so the powered-off panel can't be
    // relit by a stray page-flip (#1049).
    lv_display_enable_invalidation(m_display, false);
    m_saved_flush_cb_for_sleep = m_display->flush_cb;
    lv_display_set_flush_cb(m_display, sleep_noop_flush_cb);
    m_flush_suppressed_for_sleep = true;
    spdlog::debug("[DisplayManager] Flush suppressed while panel powered off");
}

void DisplayManager::restore_flush_after_sleep() {
    if (!m_flush_suppressed_for_sleep) {
        return;
    }
    m_flush_suppressed_for_sleep = false;
    if (m_display) {
        if (m_saved_flush_cb_for_sleep) {
            lv_display_set_flush_cb(m_display, m_saved_flush_cb_for_sleep);
        }
        lv_display_enable_invalidation(m_display, true);
    }
    m_saved_flush_cb_for_sleep = nullptr;
    spdlog::debug("[DisplayManager] Flush restored on wake");
}

// ============================================================================
// Display Sleep Management
// ============================================================================

void DisplayManager::check_display_sleep() {
#ifdef HELIX_ENABLE_SCREENSAVER
    // HELIX_SCREENSAVER_NOW — force-start screensaver immediately (for testing)
    // Values: "1" (configured type, defaults to toasters), "starfield", "pipes"
    static bool screensaver_force_checked = false;
    if (!screensaver_force_checked) {
        screensaver_force_checked = true;
        const char* env = std::getenv("HELIX_SCREENSAVER_NOW");
        if (env) {
            std::string val(env);
            ScreensaverType force_type = ScreensaverType::FLYING_TOASTERS;
            if (val == "starfield") {
                force_type = ScreensaverType::STARFIELD;
            } else if (val == "pipes") {
                force_type = ScreensaverType::PIPES_3D;
            } else {
                // "1" or any other value: use configured type, fallback to toasters
                auto configured = ScreensaverManager::configured_type();
                force_type = (configured != ScreensaverType::OFF)
                                 ? configured
                                 : ScreensaverType::FLYING_TOASTERS;
            }
            spdlog::info("[DisplayManager] HELIX_SCREENSAVER_NOW={}, forcing screensaver type {}",
                         val, static_cast<int>(force_type));
            m_display_dimmed = true;
            ScreensaverManager::instance().start(force_type);
            m_screensaver_active = true;
            return;
        }
    }
#endif

    // If sleep-while-printing is disabled, inhibit *entering* sleep/dim during
    // active prints. We still need to honor wake-from-sleep touches: the
    // display may have entered sleep BEFORE the print started, and bailing out
    // here would strand the user on a blank screen for the duration of the
    // print (debug bundle RYAQGL6C: 8 touch events, 18-minute wake delay).
    bool inhibit_sleep_entry = false;
    if (!DisplaySettingsManager::instance().get_sleep_while_printing()) {
        // Lifecycle: a user who turned off sleep-while-printing wants the
        // screen up through the pre-print homing too, which is when they are
        // most likely to be watching.
        const auto lifecycle = get_printer_state().get_print_lifecycle();
        if (job_holds_machine(lifecycle)) {
            // Reset LVGL activity timer so we don't immediately sleep when print ends
            lv_display_trigger_activity(nullptr);
            inhibit_sleep_entry = true;
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

    // Android host sleep (#1245): Android pauses the app when it powers the panel
    // down and resumes it when the panel comes back, and neither transition is a
    // touch — so the activity check below never fires and the display would stay
    // logically asleep with keep-screen-on still cleared, re-sleeping forever and
    // never resuming the sleep callbacks. This function only runs while
    // foregrounded (the run loop short-circuits on m_backgrounded), so a bumped
    // resume counter means the panel is on again.
    //
    // The awake case is the matching invariant: the host must never be left free
    // to sleep while we consider the display awake, whatever path cleared
    // m_display_sleeping. set_keep_screen_on() is transition-guarded, so an awake
    // tick costs one member compare.
    bool resumed_from_host_sleep = false;
    if (!m_display_sleeping) {
        set_keep_screen_on(true);
    }
#ifdef __ANDROID__
    else if (m_last_sleep_mechanism == SleepMechanism::HostSleep) {
        resumed_from_host_sleep =
            host_sleep_needs_wake(true, m_resume_seq_at_sleep, android_get_resume_seq());
    }
#endif

    if (m_display_sleeping) {
        // Wake via sleep_aware_read_cb (embedded) or LVGL activity detection (SDL).
        // On SDL, the sleep-aware wrapper isn't installed because it breaks SDL's
        // mouse device identification, so we fall back to LVGL activity tracking.
        if (m_wake_requested || activity_detected || resumed_from_host_sleep) {
            m_wake_requested = false;
            wake_display();
        }
    } else if (m_display_dimmed) {
        // Currently dimmed - wake on touch, or go to sleep if timeout exceeded.
        // During a screensaver preview, skip activity-based dismiss for a brief
        // grace window — otherwise the click that *launched* the preview is
        // still fresh in lv_display_get_inactive_time() and closes it instantly.
        bool dismiss_on_activity = activity_detected;
#ifdef HELIX_ENABLE_SCREENSAVER
        if (m_screensaver_is_preview) {
            constexpr uint32_t PREVIEW_GRACE_MS = 750;
            uint32_t elapsed = get_ticks() - m_preview_start_tick_ms;
            if (elapsed < PREVIEW_GRACE_MS) {
                dismiss_on_activity = false;
            }
        }
#endif
        if (m_wake_requested || dismiss_on_activity) {
            m_wake_requested = false;
            wake_display();
        } else if (!inhibit_sleep_entry && sleep_timeout_sec > 0 &&
                   inactive_ms >= sleep_timeout_ms) {
            // Transition from dimmed to sleeping
            enter_sleep(sleep_timeout_sec);
        }
    } else {
        // Currently awake - check if we should dim, start screensaver, or sleep.
        // Inhibited during prints when sleep_while_printing=false.
        if (inhibit_sleep_entry) {
            return;
        }
        bool can_dim = m_backlight && m_backlight->supports_dimming();
#ifdef HELIX_ENABLE_SCREENSAVER
        bool has_screensaver = ScreensaverManager::configured_type() != ScreensaverType::OFF;
#else
        bool has_screensaver = false;
#endif
        // Two-stage idle (#1049), both timeouts measured from the same idle clock:
        //   Dim   → dim the backlight and/or start the screensaver (intermediate)
        //   Sleep → enter full sleep / power-off (final)
        // The Sleep>=Dim ordering is guaranteed by DisplaySettingsManager's
        // coupling (now also enforced on no-backlight + screensaver devices), so
        // the dim/screensaver branch is reachable before sleep even on the
        // reporter's no-backlight Pi.
        if (sleep_timeout_sec > 0 && inactive_ms >= sleep_timeout_ms) {
            // Sleep timeout reached — go to full sleep (blank / power-off).
            enter_sleep(sleep_timeout_sec);
        } else if (m_dim_timeout_sec > 0 && inactive_ms >= dim_timeout_ms &&
                   (can_dim || has_screensaver)) {
            // Dim timeout reached — start screensaver and/or dim backlight.
            // On devices without backlight dimming, screensaver alone provides
            // the idle visual state (instead of skipping to sleep).
            m_display_dimmed = true;
#ifdef HELIX_ENABLE_SCREENSAVER
            if (!m_screensaver_active && has_screensaver) {
                // Suspend active panel lifecycle to stop widget timers (clock, etc.)
                // that would otherwise invalidate underlying UI and bleed through
                NavigationManager::instance().suspend_active();
                ScreensaverManager::instance().start(ScreensaverManager::configured_type());
                m_screensaver_active = true;
                if (m_backlight) {
                    // Screensaver needs enough brightness to see the toasters,
                    // but respect user's dim setting if it's higher
                    m_backlight->set_brightness(std::max(m_dim_brightness_percent, 50));
                }
                spdlog::info("[DisplayManager] Screensaver started after {}s inactivity",
                             m_dim_timeout_sec);
            } else
#endif
            {
                if (m_backlight) {
                    m_backlight->set_brightness(m_dim_brightness_percent);
                }
                spdlog::info("[DisplayManager] Display dimmed to {}% after {}s inactivity",
                             m_dim_brightness_percent, m_dim_timeout_sec);
            }
        }
    }
}

void DisplayManager::restore_display_output() {
    // Re-assert the host's keep-screen-on request first (#1245). Unconditional and
    // transition-guarded: if enter_sleep() handed the panel to Android we take it
    // back here, and on every other path (including all non-Android targets) this
    // is a no-op because the flag was never released.
    set_keep_screen_on(true);

    // Undo whatever enter_sleep() did to the panel output, mirroring its branches.
    // This must run BEFORE the post-wake lv_refr_now() (#303 wake-race).
    if (m_use_hardware_blank) {
        // Hardware path: unblank framebuffer (FBIOBLANK was used during sleep).
        if (m_backend) {
            m_backend->unblank_display();
        }
    } else if (m_use_power_off && m_backend) {
        // Power-off path: re-enable rendering, then power the panel back on. The
        // flush must be restored BEFORE the post-wake lv_refr_now() (in
        // wake_display) so that synchronous render actually reaches the panel.
        // A software overlay may also exist if a prior power_off() failed and fell
        // back — remove it defensively. restore_flush_after_sleep() is a no-op if
        // suppression was never engaged (overlay fallback).
        restore_flush_after_sleep();
        m_backend->power_on();
        destroy_sleep_overlay();
    } else {
        // Software path: remove the black overlay (no FBIOBLANK to undo).
        destroy_sleep_overlay();
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
    bool was_dimmed = m_display_dimmed;
    m_display_sleeping = false;
    m_display_dimmed = false;

#ifdef HELIX_ENABLE_SCREENSAVER
    bool was_preview = m_screensaver_is_preview;
    m_screensaver_is_preview = false;
    m_preview_start_tick_ms = 0;
    // Stop screensaver on wake
    if (m_screensaver_active) {
        ScreensaverManager::instance().stop();
        m_screensaver_active = false;
        // Resume active panel lifecycle to restart widget timers
        NavigationManager::instance().resume_active();
    }
#else
    constexpr bool was_preview = false;
#endif

    // Gate input if waking from full sleep (not dim)
    // This prevents the wake touch from triggering UI actions
    if (was_sleeping) {
        disable_input_briefly();

        // Restore the panel output (unblank / power-on / remove overlay) BEFORE
        // the synchronous render below so the framebuffer is ready when LVGL
        // paints — honoring the #303 black-screen-on-wake race.
        restore_display_output();

        // Force immediate full render after wake. lv_obj_invalidate() alone only
        // marks dirty regions — the actual render happens on the next timer tick,
        // which can race with framebuffer state changes and leave a black screen
        // on some hardware (#303). lv_refr_now() renders synchronously.
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);

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

    // Auto-lock: show lock screen when waking from sleep or screensaver/dim.
    // Screensaver previews are user-initiated from settings — they didn't go
    // idle, so engaging auto-lock on preview dismiss would be surprising.
    if ((was_sleeping || was_dimmed) && !was_preview &&
        helix::LockManager::instance().auto_lock_enabled() &&
        helix::LockManager::instance().has_pin()) {
        spdlog::info("[DisplayManager] Auto-lock engaged on wake");
        helix::LockManager::instance().lock();
        helix::ui::LockScreenOverlay::instance().show();
    }

    // Notify subscribers (camera stream, etc.) to resume background work
    for (auto& cb : m_sleep_callbacks) {
        cb(false);
    }
}

#ifdef HELIX_ENABLE_SCREENSAVER
void DisplayManager::preview_screensaver(int type) {
    if (m_shutting_down || m_screensaver_active) {
        return;
    }
    auto ss_type = static_cast<ScreensaverType>(type);
    if (ss_type == ScreensaverType::OFF) {
        return;
    }

    spdlog::info("[DisplayManager] Previewing screensaver type {}", type);
    // Suspend active panel so widget timers stop updating the background
    NavigationManager::instance().suspend_active();
    ScreensaverManager::instance().start(ss_type);
    // Mark display as dimmed so wake_display() runs on touch; is_preview
    // flag suppresses auto-lock on dismiss.
    m_display_dimmed = true;
    m_screensaver_active = true;
    m_screensaver_is_preview = true;
    m_preview_start_tick_ms = get_ticks();
}
#endif

void DisplayManager::ensure_display_on() {
    // Force display awake at startup regardless of previous state
    restore_flush_after_sleep(); // defensive: never start up with flush suppressed
    set_keep_screen_on(true);    // #1245: never inherit a released host sleep lock
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

    // Re-enable rendering before the framebuffer clear / final brightness below,
    // otherwise the clear is committed through the no-op flush and never reaches
    // the panel (#1049). No-op if flush was never suppressed.
    restore_flush_after_sleep();

    // If we powered the panel down (#1049), bring it back on so the next app
    // doesn't inherit a powered-off panel.
    if (m_use_power_off && m_backend) {
        m_backend->power_on();
    }

    // Clear framebuffer to black so the last rendered frame doesn't persist
    // after the process exits (SIGTERM/SIGINT graceful shutdown)
    if (m_backend) {
        m_backend->clear_framebuffer(0x00000000);
    }

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

bool DisplayManager::has_dimming_control() const {
    return m_backlight && m_backlight->supports_dimming();
}

bool DisplayManager::is_software_rotated() const {
    return m_display && m_backend && m_backend->type() == DisplayBackendType::FBDEV &&
           lv_display_get_rotation(m_display) != LV_DISPLAY_ROTATION_0;
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
// Input Gating (Wake-Only First Touch)
// ============================================================================

void DisplayManager::disable_input_briefly() {
    // Disable all pointer input devices, and cancel whatever press is already
    // in flight on each.
    //
    // lv_indev_enable(false) is a pure flag write: pr_timestamp, long_pr_sent
    // and pointer.act_obj all survive the blackout, so a finger still on the
    // glass when input comes back keeps counting toward LV_EVENT_LONG_PRESSED
    // from the ORIGINAL touch-down. On the wake touch that means a long-press
    // gesture the user never made — home-grid edit mode opening behind the lock
    // screen (#1245). lv_indev_reset() is what actually discards that state, and
    // it lands even while the device is disabled: lv_indev_read() runs the reset
    // query handler before it checks the enabled flag.
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_enable(indev, false);
            lv_indev_reset(indev, nullptr);
        }
        indev = lv_indev_get_next(indev);
    }

    // Schedule re-enable after 200ms via LVGL timer
    lv_timer_create(reenable_input_cb, 200, nullptr);

    // info-level so the wake-gate window is captured in debug bundles by
    // default — diagnoses "tapped Resume but nothing happened" reports
    // (#22) by letting us correlate user-reported tap times with the
    // 200ms blackout window.
    spdlog::info("[DisplayManager] Wake-gate engaged: input disabled for 200ms");
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

    spdlog::info("[DisplayManager] Wake-gate released: input re-enabled");
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

    // If sleeping or dimmed and touch detected, request wake.
    // During sleep: absorb the touch so it doesn't trigger UI actions.
    // During dim: let the touch pass through but still flag for wake.
    // This is necessary because LVGL only updates last_activity_time on PRESSED,
    // but evdev drains all buffered events in one read — if press+release both
    // arrive in one poll (quick tap or slow main loop), the final state is
    // RELEASED and LVGL never registers activity.
    if (data->state == LV_INDEV_STATE_PRESSED) {
        if (dm->m_display_sleeping) {
            dm->m_wake_requested = true;
            data->state = LV_INDEV_STATE_RELEASED; // Absorb - LVGL sees no press
            spdlog::info("[DisplayManager] Wake touch absorbed at ({},{}) while sleeping",
                         data->point.x, data->point.y);
        } else if (dm->m_display_dimmed) {
            dm->m_wake_requested = true;
            spdlog::info("[DisplayManager] Wake touch at ({},{}) while dim — passing through",
                         data->point.x, data->point.y);
        }
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
// DRM→fbdev Fallback
// ============================================================================

bool DisplayManager::try_drm_to_fbdev_fallback(lv_display_rotation_t rot, bool splash_active) {
    if (m_backend->type() != DisplayBackendType::DRM ||
        m_backend->supports_hardware_rotation(rot)) {
        return true; // No fallback needed
    }

    // If input devices were already created (apply_rotation runs this fallback
    // after init()), they are bound to the DRM backend we are about to free and
    // to the display we are about to delete — they must be rebuilt on the fbdev
    // backend below. At init time m_pointer is still null and init() creates the
    // input devices after this returns, so nothing to rebuild there.
    const bool had_input_devices = (m_pointer != nullptr);

    spdlog::warn("[DisplayManager] DRM lacks hardware rotation for {}°, "
                 "falling back to fbdev (flicker-free software rotation)",
                 static_cast<int>(rot) * 90);
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
                      "For DSI/EGL displays, use the kernel panel_orientation parameter "
                      "instead: add panel_orientation=right_side_up (or left_side_up, "
                      "upside_down) to /boot/firmware/cmdline.txt and remove the "
                      "\"rotate\" key from settings.json.");
        return false;
    }
    spdlog::info("[DisplayManager] Fbdev fallback succeeded at {}x{}", m_width, m_height);
    warn_fbdev_high_dpi();

    // Recreate the input devices on the new backend. lv_display_delete() only
    // detached them (their display is now NULL) and m_backend.reset() freed the
    // DRM backend their read_cb/user_data pointed into — leaving m_pointer as a
    // display-less indev referencing freed memory and the fbdev backend with no
    // input at all. Rebuild only when they already existed (post-init swap).
    if (had_input_devices) {
        rebuild_input_after_backend_swap();
    }
    return true;
}

void DisplayManager::warn_fbdev_high_dpi() {
    static constexpr int HIGH_DPI_THRESHOLD = 1920;
    if (m_width <= HIGH_DPI_THRESHOLD && m_height <= HIGH_DPI_THRESHOLD) {
        return;
    }
    spdlog::warn("[DisplayManager] Fbdev resolution {}x{} exceeds {}px on one axis. "
                 "Cannot auto-downscale in fbdev mode. Configure a lower resolution "
                 "via kernel parameters (e.g., framebuffer_width/framebuffer_height "
                 "in /boot/firmware/config.txt on Raspberry Pi) and reboot.",
                 m_width, m_height, HIGH_DPI_THRESHOLD);
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

void DisplayManager::apply_rotation(int degrees) {
    if (!m_display || !m_backend) {
        spdlog::warn("[DisplayManager] Cannot apply rotation — display not initialized");
        return;
    }
    if (degrees == 0)
        return;

#ifdef HELIX_DISPLAY_SDL
    spdlog::warn("[DisplayManager] Rotation {}° not supported on SDL backend", degrees);
#else
    int phys_w = m_width;
    int phys_h = m_height;

    lv_display_rotation_t lv_rot = degrees_to_lv_rotation(degrees);

    // DRM backend may not support hardware rotation for this angle —
    // fall back to fbdev. Note: splash_active=false since apply_rotation()
    // is only called after init() completes (splash is already managed).
    if (!try_drm_to_fbdev_fallback(lv_rot, false)) {
        spdlog::error("[DisplayManager] Cannot apply {}° rotation — DRM fallback failed", degrees);
        return;
    }

    lv_display_set_rotation(m_display, lv_rot);

    m_width = lv_display_get_horizontal_resolution(m_display);
    m_height = lv_display_get_vertical_resolution(m_display);

    m_backend->set_display_rotation(lv_rot, phys_w, phys_h);

    spdlog::info("[DisplayManager] Display rotated {}° — effective resolution: {}x{}", degrees,
                 m_width, m_height);
#endif
}

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

    // Suppress the debounced resize fanout for the whole probe. Each rotation
    // resizes the screen, and the registered theme/layout refresh runs inside
    // the lv_timer_handler() call the tap poll below makes every iteration -
    // seconds of it on a slow panel, during which no touch sample is taken.
    // The confirmed rotation is re-applied at the end and Application refreshes
    // the theme and LayoutManager once the probe returns.
    struct ResizeFanoutSuspension {
        explicit ResizeFanoutSuspension(DisplayManager* dm) : m_dm(dm) {
            m_dm->set_resize_fanout_suspended(true);
        }
        ~ResizeFanoutSuspension() {
            m_dm->set_resize_fanout_suspended(false);
        }
        ResizeFanoutSuspension(const ResizeFanoutSuspension&) = delete;
        ResizeFanoutSuspension& operator=(const ResizeFanoutSuspension&) = delete;
        DisplayManager* m_dm;
    } resize_suspension(this);

    // Latch presses on their edge instead of sampling the level. evdev drains
    // its whole fd per read and reports only the final state, so a press and
    // its release arriving between two polls would otherwise vanish. The
    // coordinate half of the latch is contact-driven-device only: an SDL mouse
    // reports motion with no button down and would latch on every wiggle.
    helix::TapLatch tap_latch(!is_sdl);

    // Sample the pointer once and feed the latch. Safe to call as often as we
    // like; each call drains whatever evdev has buffered since the last one.
    // The read callback is looked up per call, matching how the backend may
    // replace the pointer device while the probe is running.
    auto poll_pointer = [&]() {
        lv_indev_read_cb_t read_cb = m_pointer ? lv_indev_get_read_cb(m_pointer) : nullptr;
        if (!read_cb) {
            return;
        }
        lv_indev_data_t data = {};
        read_cb(m_pointer, &data);
        tap_latch.feed(data);
    };

    // Poll until the contact lifts (or the deadline passes). Returns when the
    // pointer reads RELEASED so a held finger cannot carry into the next screen.
    auto drain_until_release = [&]() {
        uint32_t release_deadline = get_ticks() + 2000; // 2s max
        while (get_ticks() < release_deadline) {
            lv_timer_handler();
            delay(10);
            lv_indev_read_cb_t read_cb = m_pointer ? lv_indev_get_read_cb(m_pointer) : nullptr;
            if (!read_cb) {
                break;
            }
            lv_indev_data_t release_data = {};
            read_cb(m_pointer, &release_data);
            if (release_data.state == LV_INDEV_STATE_RELEASED) {
                break;
            }
        }
    };

    // Lambda for mini event loop that watches for tap.
    // Returns immediately on confirmed tap (no post-tap delay).
    auto wait_for_tap = [&](int timeout_ms, lv_obj_t* countdown_lbl, SubtitleFn subtitle_fn,
                            int rot_deg) -> bool {
        uint32_t start = get_ticks();
        int last_sec = -1;

        // Drop anything latched by the previous screen, and re-baseline the
        // coordinate so the position left behind by the last tap cannot read as
        // a fresh one. A contact that is still down here (a screen that timed
        // out mid-press) is drained to its release rather than counted as a tap
        // on this screen.
        tap_latch.reset();
        poll_pointer();
        if (tap_latch.consume()) {
            spdlog::debug("[DisplayManager] Rotation probe: contact still down at {}° entry, "
                          "waiting for release",
                          rot_deg);
            drain_until_release();
            tap_latch.reset();
        }

        // A tap detected here is drained to its release before returning, so a
        // finger still down does not carry into the next screen as a phantom.
        auto accept_tap = [&]() {
            spdlog::info("[DisplayManager] Rotation probe: tap detected at {}° ({})", rot_deg,
                         tap_latch.from_collapsed_read() ? "recovered from collapsed read"
                                                         : "press observed");
            tap_latch.consume();
            drain_until_release();
            tap_latch.reset();
        };

        while (true) {
            uint32_t elapsed = get_ticks() - start;
            if (elapsed >= static_cast<uint32_t>(timeout_ms)) {
                return false;
            }

            // Sample either side of lv_timer_handler(): whatever it costs on
            // this hardware, a tap that lands during it is still seen on the
            // very next sample rather than after another full poll interval.
            poll_pointer();
            if (tap_latch.latched()) {
                accept_tap();
                return true;
            }

            lv_timer_handler();
            delay(10);

            poll_pointer();
            if (tap_latch.latched()) {
                accept_tap();
                return true;
            }

            // Update countdown label
            int remaining_sec = static_cast<int>((timeout_ms - elapsed + 999) / 1000);
            if (remaining_sec != last_sec && countdown_lbl) {
                std::string text = subtitle_fn(rot_deg, remaining_sec);
                lv_label_set_text(countdown_lbl, text.c_str());
                last_sec = remaining_sec;
            }
        }
    };

    int confirmed_rotation = -1;
    const int max_cycles = 3;
    int cycle = 0;

    // Loop until user confirms a rotation. On real hardware, the wrong rotation
    // renders unreadable text so the user can only tap the correct one.
    // Safety: give up after max_cycles full sweeps to avoid infinite loop
    // (e.g. uncalibrated resistive touchscreen that can't register taps).
    while (confirmed_rotation < 0 && cycle < max_cycles) {
        cycle++;
        for (int i = 0; i < num_rotations; i++) {
            // Apply rotation (skip on SDL — DIRECT render mode can't rotate)
            if (!is_sdl) {
                // Set the LVGL display rotation so the rendering actually
                // changes on screen, then let the backend handle any
                // hardware-specific adjustments (touch coords, etc.).
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

    // If probe timed out without confirmation, default to 0°
    if (confirmed_rotation < 0) {
        spdlog::warn("[DisplayManager] Rotation probe: no confirmation after {} cycles, "
                     "defaulting to 0°",
                     max_cycles);
        confirmed_rotation = 0;
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

    // A timer armed before the suspension began must not fan out either - the
    // rotation probe polls for taps through lv_timer_handler(), and this is the
    // callback that can sit in a multi-second theme refresh while it does.
    if (self->m_resize_fanout_suspended) {
        spdlog::debug("[DisplayManager] Resize fanout suspended - dropping pending debounce");
        lv_timer_delete(timer);
        self->m_resize_debounce_timer = nullptr;
        return;
    }

    // Refresh cached dimensions from LVGL before fanning out callbacks.
    // lv_display_set_resolution() (e.g. from the Android SDL window resize
    // path on fold/unfold) does not update m_width/m_height, so without
    // this any callback that reads dm->width()/height() would see stale
    // startup values.  Reordering also catches non-rotation resizes from
    // any future code path that calls lv_display_set_resolution directly.
    if (self->m_display) {
        self->m_width = lv_display_get_horizontal_resolution(self->m_display);
        self->m_height = lv_display_get_vertical_resolution(self->m_display);
    }

    spdlog::debug(
        "[DisplayManager] Resize debounce complete: {}x{}, calling {} registered callbacks",
        self->m_width, self->m_height, self->m_resize_callbacks.size());

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

        if (self->m_resize_fanout_suspended) {
            spdlog::debug("[DisplayManager] Screen size changed to {}x{} while resize fanout "
                          "suspended - no debounce armed",
                          width, height);
            return;
        }

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
                const int stride = lv_draw_buf_width_to_stride(w, cf);
                self->m_color_transform.apply(px_map, w, h, stride, cf);
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
                // Use the ACTUAL draw-buffer stride, not width_to_stride(area_w):
                // the DRM backend renders into dumb buffers whose pitch is aligned
                // and may exceed the area width (direct/full render mode). Reading
                // px_map with the wrong stride mis-tracks rows. Fall back to the
                // computed stride only if the active buffer can't be queried.
                lv_draw_buf_t* dbuf = lv_display_get_buf_active(d);
                f.src_stride = (dbuf && dbuf->header.stride > 0)
                                   ? static_cast<uint32_t>(dbuf->header.stride)
                                   : lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
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
    }
    spdlog::info("[DisplayManager] Color transform: gamma={:.2f}, warmth={}, tint={} (identity={})",
                 gamma, warmth, tint, m_color_transform.is_identity());
}
