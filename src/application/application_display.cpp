// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file application_display.cpp
 * @brief Application display phases: DisplayManager init, UI scale/DPI, resize
 *        handling, theme, assets, first-boot rotation probe and layout.
 */

#include "ui_icon_loader.h"
#include "ui_nav_manager.h"
#include "ui_unit_art.h"

#include "application.h"
#include "asleep_status_wiring.h"
#include "asset_manager.h"
#include "board_identity.h"
#include "cjk_filename_glyphs.h"
#include "config.h"
#include "core/data_root_resolver.h"
#include "display_backend.h"
#include "display_manager.h"
#include "display_metrics.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "input_settings_manager.h"
#include "layout_manager.h"
#include "lvgl.h"
#include "lvgl_log_handler.h"
#include "memory_monitor.h"
#include "moonraker_manager.h"
#include "rotation_probe.h"
#include "runtime_config.h"
#include "splash_screen.h"
#include "theme_manager.h"
#include "xml_registration.h"

#include <spdlog/spdlog.h>

#ifdef HELIX_DISPLAY_SDL
#include <SDL.h>
#endif

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unistd.h>

using namespace helix;

namespace {

// The --dpi override and the stored UI Scale setting as read at startup. The
// resize path reuses them, so a setting that "applies after restart" does not
// sneak in on the first fold.
int s_user_override_dpi = 0;
int s_scale_setting = helix::DisplayMetrics::kScaleSettingAutomatic;

/// Measure the drawing surface and decide the UI scale from it.
///
/// Android is the one platform whose reported DPI can be trusted: OEMs must
/// declare it and SDL reads it straight from DisplayMetrics. Every Linux
/// target is excluded on purpose: a survey of the eight test devices found the
/// reported physical size wrong or absent on six, including a Pi that reports
/// the official 7" panel's exact active area for a ~115mm screen.
helix::ScaleDecision decide_display_scale(lv_display_t* display) {
    std::optional<double> native_dpi;
    int native_w = 0, native_h = 0;
    const int logical_w = lv_display_get_horizontal_resolution(display);
    const int logical_h = lv_display_get_vertical_resolution(display);
#ifdef __ANDROID__
    float ddpi = 0, hdpi = 0, vdpi = 0;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0 && ddpi > 0) {
        native_dpi = static_cast<double>(ddpi);
    }
    // The SDL driver draws at an integer fraction of the native resolution and
    // upscales, so the DPI is reported per native pixel while the UI lays out
    // in logical ones.
    if (auto* renderer = static_cast<SDL_Renderer*>(lv_sdl_window_get_renderer(display))) {
        SDL_GetRendererOutputSize(renderer, &native_w, &native_h);
    }
    spdlog::info("[Application] Android display DPI: diagonal={:.0f} h={:.0f} v={:.0f}; "
                 "{}x{} logical, {}x{} native",
                 ddpi, hdpi, vdpi, logical_w, logical_h, native_w, native_h);
#endif
    return helix::DisplayMetrics::decide_scale(s_user_override_dpi, native_dpi, native_w, native_h,
                                               logical_w, logical_h, helix::board::current(),
                                               s_scale_setting);
}

/// Make @p d the process-wide scale: design tokens and fonts read it the next
/// time theme_manager_refresh_layout_constants() resolves them, and LVGL's own
/// chrome follows the display DPI.
void apply_display_scale(lv_display_t* display, const helix::ScaleDecision& d) {
    helix::DisplayMetrics::set_auto_scale(d.auto_scale);
    helix::DisplayMetrics::set_active_scale(d.ui_scale);
    lv_display_set_dpi(display, d.lvgl_dpi);
    spdlog::info("[Application] Display metrics: dpi={:.0f} (source={}) → ui_scale={:.3f} "
                 "(auto={:.3f}, setting={}), lvgl_dpi={}",
                 d.resolved.dpi, helix::dpi_source_name(d.resolved.source), d.ui_scale,
                 d.auto_scale,
                 s_scale_setting == helix::DisplayMetrics::kScaleSettingAutomatic
                     ? std::string("automatic")
                     : std::to_string(s_scale_setting) + "%",
                 d.lvgl_dpi);
}

} // namespace

bool Application::init_display() {
#ifdef HELIX_DISPLAY_SDL
    // Set window position environment variables
    if (m_args.display_num >= 0) {
        char display_str[32];
        snprintf(display_str, sizeof(display_str), "%d", m_args.display_num);
        setenv("HELIX_SDL_DISPLAY", display_str, 1);
    }
    if (m_args.x_pos >= 0 && m_args.y_pos >= 0) {
        char x_str[32], y_str[32];
        snprintf(x_str, sizeof(x_str), "%d", m_args.x_pos);
        snprintf(y_str, sizeof(y_str), "%d", m_args.y_pos);
        setenv("HELIX_SDL_XPOS", x_str, 1);
        setenv("HELIX_SDL_YPOS", y_str, 1);
    }
#endif

    m_display = std::make_unique<DisplayManager>();
    DisplayManager::Config config;
    config.width = m_screen_width;
    config.height = m_screen_height;
    // The first-boot orientation is applied by display init, as the configured
    // rotation, so it is in place before the backends create their input devices
    // and gate the stored touch range on it (#1428): the kernel's
    // panel_orientation here, the board's mount once init knows the framebuffer.
    m_rotation_probe_wanted = rotation_probe_wanted();
    const int kernel_orientation =
        m_rotation_probe_wanted ? DisplayBackend::detect_panel_orientation() : -1;
    config.rotation =
        helix::startup_rotation(m_args.rotation, m_rotation_probe_wanted, kernel_orientation);
    config.first_boot_probe = m_rotation_probe_wanted;
    config.kernel_orientation = kernel_orientation;
    config.size_was_explicit = m_args.size_was_explicit;

    // Get scroll config from settings.json
    config.scroll_throw =
        m_config->get<int>("/input/scroll_throw", InputSettingsManager::kDefaultScrollThrow);
    config.scroll_limit = m_config->get<int>("/input/scroll_limit", 10);

    // Allow headless/VNC operation without a touchscreen
    const char* req_ptr = std::getenv("HELIX_REQUIRE_POINTER");
    if (req_ptr && (std::string(req_ptr) == "0" || std::string(req_ptr) == "false")) {
        config.require_pointer = false;
        spdlog::info("[Application] Pointer input not required (HELIX_REQUIRE_POINTER={})",
                     req_ptr);
    }

    // Tell DisplayManager to skip framebuffer ioctls (FBIOBLANK, FBIOPAN_DISPLAY)
    // when splash is active — the splash process already owns and configured the display.
    config.splash_active = (get_runtime_config()->splash_pid > 0);

    if (!m_display->init(config)) {
        spdlog::error("[Application] Display initialization failed");
        return false;
    }

    // Update screen dimensions from what the display actually resolved to.
    // DisplayManager::init() handles config rotation AND kernel panel orientation
    // auto-detection (with DRM→fbdev fallback), so dimensions already reflect
    // any rotation applied.
    m_screen_width = m_display->width();
    m_screen_height = m_display->height();

    // Reconnect the WebSocket when the display wakes from sleep. The app
    // background/foreground path (on_enter_foreground) already force-reconnects,
    // but on Android the SDL background/foreground event pair is unreliable for
    // the display-off/on round trip — the Activity may not get a clean
    // onPause/onResume, so m_backgrounded never flips and the reconnect is
    // skipped. This sleep callback closes that gap (#1245).
    //
    // Android ONLY, deliberately. force_reconnect() tears the socket down and
    // rebuilds it synchronously on whatever thread calls it, and this callback
    // runs on the UI thread. The Linux fbdev/DRM fleet never backgrounds the
    // process on display sleep — the connection stays up and the health timer
    // keeps running — so there is nothing to re-establish on wake, and running
    // the teardown anyway only exposes the main loop to blocking inside it.
    //
    // Registered here, alongside the DisplayManager that owns the callback list,
    // rather than in m_session.connect_moonraker(): that runs again on every printer
    // switch, and register_sleep_callback() only appends — there is no
    // unregister — so each switch would stack another copy and fire one extra
    // force_reconnect() per wake. init_display() runs once per process, and the
    // captured `this` owns m_display, so the callback list cannot outlive it.
    // m_session.moonraker() is read lazily at wake time and need not exist yet.
#ifdef __ANDROID__
    m_display->register_sleep_callback([this](bool sleeping) {
        if (!sleeping && m_session.moonraker() && m_session.moonraker()->client()) {
            // Debounce: on_enter_foreground() may have already called
            // force_reconnect for the same wake event. Skip if it ran
            // within the last 5 seconds — the second call would bump
            // the connection generation and make the first discovery's
            // subscription stale (#1245).
            auto now = std::chrono::steady_clock::now();
            if (now - m_last_force_reconnect < std::chrono::seconds(5)) {
                spdlog::debug("[Application] Display woke — skipping reconnect (debounced, "
                              "on_enter_foreground ran recently)");
                return;
            }
            spdlog::info("[Application] Display woke — reconnecting WebSocket");
            m_last_force_reconnect = now;
            m_session.moonraker()->client()->force_reconnect();
        }
    });
#endif

    helix::wire_asleep_status(*m_display, [this] { return m_session.moonraker().get(); });
    // Turning spools in the unit art stop while the panel is dark and resume on wake.
    m_display->register_sleep_callback(
        [](bool sleeping) { helix::ui::unit_art_set_display_asleep(sleeping); });

    // Register LVGL log handler AFTER lv_init() (called inside display->init())
    // Must be after lv_init() because it resets global state and clears callbacks
    helix::logging::register_lvgl_log_handler();

    // Always set DPI explicitly. LVGL's lv_display_create() initializes dpi to
    // LV_DPI_DEF (160), but the fbdev/DRM drivers will OVERWRITE it from the
    // kernel's reported physical screen size (FBIOGET_VSCREENINFO width/height
    // in mm, or DRM connector mmWidth). When the driver reports BOGUS physical
    // dimensions — observed on BTT CB1 / sun4i-drmdrmfb, which reports the
    // BTT HDMI5 5" panel (real ≈109mm × 65mm) as 890mm × 500mm (≈35"×20",
    // off by ~8×) — that computes to dpi≈23, and LV_DPX_CALC(23, 10) clamps
    // PAD_SMALL to 1px via
    // its MAX(.., 1) safeguard. Result: dropdown / input padding visually
    // disappears. Forcing dpi here (after the driver has had its chance) makes
    // the UI immune to lying kernel drivers.
    // Resolve the panel's physical DPI and turn it into a UI scale factor. The
    // scale is flat (exactly 1.0) through 225 DPI, which covers every shipping
    // printer measured, so this is an identity everywhere except phone-class
    // panels.
    //
    // The stored UI Scale setting wins over the measurement, which is the
    // whole point of exposing it: a panel we scale wrongly, or a user who
    // simply wants larger type, has a way out that does not need a rebuild.
    // Config rather than DisplaySettingsManager because this runs inside
    // init_display(), before that manager's subjects exist; both read the
    // same key. `--dpi` stays upstream of this, overriding the measurement
    // that Automatic then follows.
    //
    // What LVGL gets is LV_DPI_DEF scaled by the SAME factor, never a
    // kernel-derived number. LVGL turns its DPI into padding via LV_DPX_CALC,
    // so this makes its internal chrome (and the four lv_dpx() call sites in
    // our own code) grow in step with the design tokens theme_manager scales.
    // The two paths are disjoint, so they add rather than compound. At scale
    // 1.0 this is exactly LV_DPI_DEF, which is what keeps a lying kernel
    // driver from reaching the UI at all (a sun4i-drm CB1 reports 23 DPI and
    // would collapse padding to 1px via LV_DPX's MAX(..,1) floor).
    s_user_override_dpi = m_args.dpi;
    s_scale_setting = Config::get_instance()->get<int>(
        "/display/ui_scale_percent", helix::DisplayMetrics::kScaleSettingAutomatic);
    const int32_t pre_set_dpi = lv_display_get_dpi(m_display->display());
    const helix::ScaleDecision scale = decide_display_scale(m_display->display());
    apply_display_scale(m_display->display(), scale);
    if (pre_set_dpi < 50 && m_args.dpi == 0) {
        spdlog::warn("[Application] Display reported dpi={} before set — backend lost LV_DPI_DEF "
                     "between create and theme init. Fix-forward applied (forced to {}).",
                     pre_set_dpi, scale.lvgl_dpi);
    }

    // Get active screen
    m_screen = lv_screen_active();

    // Set window icon
    ui_set_window_icon(m_display->display());

    // Initialize resize handler
    m_display->init_resize_handler(m_screen);

    // Refresh theme tokens (nav_width, overlay widths, spacing) and the
    // LayoutManager state on every debounced resize.  Without this hook,
    // theme_manager_refresh_layout_constants() only fires once at startup
    // (after the rotation probe) and a runtime size change — e.g. fold/
    // unfold on a Samsung Fold or Flip — leaves the ui_breakpoint subject
    // and overlay widths stuck at startup values, and home-screen widgets
    // do not reflow (#941).  Captureless lambda — must use singletons.
    m_display->register_resize_callback([]() {
        auto* dm = DisplayManager::instance();
        if (!dm)
            return;
        lv_display_t* disp = dm->display();
        if (!disp)
            return;

        const int w = dm->width();
        const int h = dm->height();
        auto& layout = helix::LayoutManager::instance();

        // LayoutManager first: theme_manager_refresh_layout_constants() now
        // derives ui_is_portrait from LayoutManager::type() (override-aware),
        // so the type must reflect the new geometry before refresh reads it.
        // #1255.
        // A foldable changes the native-to-logical ratio, and with it the
        // DPI of the logical pixel, so the scale is decided again before the
        // tokens and fonts that read it are refreshed.
        const helix::ScaleDecision scale = decide_display_scale(disp);
        const bool scale_moved = scale.ui_scale != helix::DisplayMetrics::active_scale();
        if (scale_moved) {
            apply_display_scale(disp, scale);
        }

        const helix::LayoutType layout_before = layout.type();
        layout.init(w, h);
        const bool tokens_moved = theme_manager_refresh_layout_constants(disp);
        helix::refresh_cjk_filename_glyphs();
        // Built widgets keep the tokens, fonts and layout variant they were built
        // with. Only a change to one of those rebuilds them, so a window drag
        // rebuilds at a tier crossing rather than at every debounced step (#1214).
        const bool rebuild = tokens_moved || scale_moved || layout.type() != layout_before;
        // Components register with the tokens and layout variant of the moment;
        // idle ones register again at the new geometry on their next use. A
        // rebuild registers the live ones again too, then rebuilds the views.
        if (rebuild) {
            helix::reregister_xml_components_for_rebuild();
            NavigationManager::instance().rebuild_for_layout_change();
        } else {
            helix::unregister_idle_xml_components();
        }

        // Overlays cache their root widget across show/hide cycles, so the
        // width applied at push time goes stale when the canvas changes size
        // (e.g., Android Keep Navigation Bar pins a side bar that insets the
        // LVGL surface). NavigationManager remembers each live overlay's
        // resolved width class, so this re-derives the pixel width from the
        // constants just refreshed above (#941, #1178).
        NavigationManager::instance().reapply_overlay_widths();

        spdlog::info("[Application] Resize: refreshed theme + layout for {}x{} ({}){}", w, h,
                     layout.name(), rebuild ? ", views rebuilt" : "");
    });

    // Tips are NOT loaded here. TipsManager::get_instance() parses the database
    // on first use instead, so a session that never displays the tips widget
    // never pays the 105 KB parse or keeps its cache resident.

    spdlog::debug("[Application] Display initialized");
    helix::MemoryMonitor::log_now("after_display_init");

    // Initialize splash screen manager for deferred exit, re-enabling rendering in the main
    // loop when the splash exits
    m_splash_manager.start(get_runtime_config()->splash_pid);

    m_splash_manager.begin_render_suppression(get_runtime_config()->splash_pid);

    return true;
}

bool Application::init_theme() {
    // Determine theme mode
    bool dark_mode;
    if (m_args.dark_mode_cli >= 0) {
        dark_mode = (m_args.dark_mode_cli == 1);
    } else {
        dark_mode = m_config->get<bool>("/dark_mode", true);
    }

    // Register globals.xml first (required for theme constants, fonts, spacing tokens)
    // Note: fonts must be registered before this (done in init_assets phase)
    lv_result_t globals_result = lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/globals.xml").c_str());
    if (globals_result != LV_RESULT_OK) {
        spdlog::error("[Application] FATAL: Failed to load globals.xml - "
                      "all XML constants (fonts, colors, spacing) will be missing. "
                      "Check working directory and verify ui_xml/globals.xml exists.");
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            spdlog::error("[Application] Current working directory: {}", cwd);
        }
        return false;
    }

    // Initialize theme
    theme_manager_init(m_display->display(), dark_mode);

    // Apply background color to screen
    theme_manager_apply_bg_color(m_screen, "screen_bg", LV_PART_MAIN);

    // Show LVGL splash screen only when no external splash process is running.
    // On embedded targets, helix-splash provides visual coverage during startup;
    // showing the internal splash too causes a visible double-splash.
    if (!get_runtime_config()->should_skip_splash() && get_runtime_config()->splash_pid <= 0) {
        helix::show_splash_screen(m_screen_width, m_screen_height);
    }

    spdlog::debug("[Application] Theme initialized (dark={})", dark_mode);
    return true;
}

bool Application::init_assets() {
    AssetManager::register_all();

    // TJPGD (built-in JPEG decoder) is auto-initialized by LVGL when LV_USE_TJPGD=1
    spdlog::debug("[Application] Assets registered");
    helix::MemoryMonitor::log_now("after_fonts_loaded");
    return true;
}

bool Application::rotation_probe_wanted() const {
    // Run rotation probe on first boot if no rotation is configured.
    // Skip if: CLI rotation set, env var set, already probed, or config already
    // has a /display/rotate key (even if 0 — means user already configured it).
    // HELIX_FORCE_ROTATION_PROBE=1 bypasses all guards (for testing on SDL).
    if (helix::rotation_probe_forced()) {
        spdlog::info("[Application] Rotation probe forced via HELIX_FORCE_ROTATION_PROBE");
        return true;
    }
#if defined(HELIX_DISPLAY_FBDEV) || defined(HELIX_DISPLAY_DRM)
    if (m_args.rotation == 0 && !std::getenv("HELIX_DISPLAY_ROTATION")) {
        if (!helix::rotation_probe_unanswered(*m_config)) {
            spdlog::info("[Application] Rotation probe skipped: probed={}, has_rotate_key={}",
                         m_config->get<bool>("/display/rotation_probed", false),
                         m_config->exists("/display/rotate"));
            return false;
        }
        return true;
    }
    spdlog::info("[Application] Rotation probe skipped: cli_rotation={}, env={}", m_args.rotation,
                 std::getenv("HELIX_DISPLAY_ROTATION") ? std::getenv("HELIX_DISPLAY_ROTATION")
                                                       : "unset");
    return false;
#else
    spdlog::debug("[Application] Rotation probe skipped: not embedded build");
    return false;
#endif
}

void Application::run_rotation_probe_and_layout() {
    {
        const bool should_probe = m_rotation_probe_wanted;

        if (should_probe) {
            int32_t pre_w = m_screen_width;
            int32_t pre_h = m_screen_height;

            // A known orientation first: the kernel's panel_orientation (which
            // is informational, the kernel does NOT rotate the framebuffer) or
            // the board's mount. init_display() has already applied it, so only
            // the config write remains.
            const int known_orientation = m_display->first_boot_orientation();
            if (known_orientation >= 0) {
                // Orientation detected (0=Normal, 90, 180, 270).
                if (known_orientation > 0) {
                    spdlog::info("[Application] Auto-detected panel orientation: {}° — "
                                 "applied at display init, saving to config",
                                 known_orientation);
                } else {
                    spdlog::info("[Application] Auto-detected panel orientation: Normal (0°) — "
                                 "no rotation needed, saving to config");
                }
                m_config->set("/display/rotate", known_orientation);
                m_config->set("/display/rotation_probed", true);
                m_config->save();
            } else {
                // known_orientation == -1: not known, run interactive probe.
                // Dismiss splash first — the probe renders full-screen UI to the
                // framebuffer, which is invisible while the splash process is
                // painting over it and the flush callback is suppressed.
                m_splash_manager.dismiss_for_rotation_probe(m_display.get());
                m_display->run_rotation_probe();
                m_screen_width = m_display->width();
                m_screen_height = m_display->height();
            }

            // Rotation changed screen dimensions — refresh theme layout constants
            // (nav_width, overlay widths, spacing tokens) that were calculated
            // during Phase 6 with pre-rotation dimensions.
            if (m_screen_width != pre_w || m_screen_height != pre_h) {
                theme_manager_refresh_layout_constants(m_display->display());
            }
        }
    }

    // Initialize layout manager (after display dimensions are known)
    auto& layout_mgr = helix::LayoutManager::instance();
    if (!m_args.layout.empty() && m_args.layout != "auto") {
        layout_mgr.set_override(m_args.layout);
    } else {
        std::string config_layout = m_config->get<std::string>("/display/layout", "auto");
        if (config_layout != "auto") {
            layout_mgr.set_override(config_layout);
        }
    }
    layout_mgr.init(m_screen_width, m_screen_height);
    // LayoutManager just resolved any --layout override. Republish
    // ui_is_portrait from it so XML visual decisions match the C++ ones; the
    // startup seed (theme_manager_init) and the rotation-probe refresh both ran
    // before this point and could only see detect_layout_type(). #1255.
    theme_manager_refresh_orientation();
    spdlog::info("[Application] Layout: {} ({})", layout_mgr.name(),
                 layout_mgr.is_standard() ? "default" : "override");
}
