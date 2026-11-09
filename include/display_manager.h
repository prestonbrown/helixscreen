// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_coalesced_timer.h"
#include "ui_timer_guard.h"

#include "backlight_backend.h"
#include "color_transform.h"
#include "display_backend.h"
#include "display_sleep_controller.h"
#include "indev_delete_watch.h"
#include "refresh_timing.h"
#include "remote_screen_manager.h"
#include "scroll_click_guard.h"
#include "touch_calibration.h"
#include "touch_calibration_session.h"
#include "touch_calibration_wrapper.h"
#include "wake_touch_latch.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <vector>

/**
 * @brief Manages LVGL display initialization and lifecycle
 *
 * Encapsulates display backend creation, LVGL initialization, and input device
 * setup. Extracted from main.cpp init_lvgl() to enable isolated testing and
 * cleaner application startup.
 *
 * Lifecycle:
 * 1. Create DisplayManager instance
 * 2. Call init() with desired configuration
 * 3. Use display(), pointer_input(), keyboard_input() as needed
 * 4. Call shutdown() or let destructor clean up
 *
 * Thread safety: All methods should be called from the main thread.
 *
 * @code
 * DisplayManager display_mgr;
 * DisplayManager::Config config;
 * config.width = 800;
 * config.height = 480;
 *
 * if (!display_mgr.init(config)) {
 *     spdlog::error("Failed to initialize display");
 *     return 1;
 * }
 *
 * // Use display_mgr.display() for LVGL operations
 * // ...
 *
 * display_mgr.shutdown();
 * @endcode
 */
#if HELIX_ENABLE_SCREENSAVER
namespace helix::ui {
struct SaverHost;
}
#endif

class DisplayManager : public helix::ICalibrationSink {
  public:
    /**
     * @brief Display configuration options
     */
    struct Config {
        int width = 0;                  ///< Display width in pixels (0 = auto-detect)
        int height = 0;                 ///< Display height in pixels (0 = auto-detect)
        int rotation = 0;               ///< Display rotation in degrees (0, 90, 180, 270)
        int scroll_throw = 25;          ///< Scroll momentum decay (1-99, higher = faster decay)
        int scroll_limit = 10;          ///< Pixels before scrolling starts
        bool require_pointer = true;    ///< Fail init if no pointer device (embedded only)
        bool splash_active = false;     ///< External splash owns framebuffer — skip unblank/pan
        bool size_was_explicit = false; ///< True if width/height came from user's -s flag
        bool first_boot_probe = false;  ///< The first-boot rotation probe owns this boot
        int kernel_orientation = -1;    ///< Kernel panel_orientation, -1 if absent
    };

    DisplayManager();
    ~DisplayManager();

    // Non-copyable, non-movable (owns unique resources)
    DisplayManager(const DisplayManager&) = delete;
    DisplayManager& operator=(const DisplayManager&) = delete;
    DisplayManager(DisplayManager&&) = delete;
    DisplayManager& operator=(DisplayManager&&) = delete;

    /**
     * @brief Get the current DisplayManager instance
     *
     * Returns the most recently initialized DisplayManager. Typically there is
     * only one instance owned by Application. Returns nullptr if none exists.
     *
     * @return Pointer to current instance, or nullptr
     */
    static DisplayManager* instance();

    /**
     * @brief Initialize LVGL and display backend
     *
     * Creates display backend (auto-detected), initializes LVGL,
     * creates display and input devices.
     *
     * @param config Display configuration
     * @return true on success, false on failure (logs error details)
     */
    bool init(const Config& config);

    /**
     * @brief Shutdown display and release resources
     *
     * Safe to call multiple times. Called automatically by destructor.
     */
    void shutdown();

    /**
     * @brief Check if display is initialized
     * @return true if init() succeeded and shutdown() not called
     */
    bool is_initialized() const {
        return m_initialized;
    }

    /**
     * @brief Get LVGL display object
     * @return Display pointer, or nullptr if not initialized
     */
    lv_display_t* display() const {
        return m_display;
    }

    /**
     * @brief Get pointer input device (mouse/touch)
     * @return Input device pointer, or nullptr if not available
     */
    lv_indev_t* pointer_input() const {
        return m_pointer;
    }

    /// Pixels of travel after which the post-scroll click guard counts a touch
    /// as a scroll. Follows the live scroll_limit setting.
    void set_scroll_guard_limit(int px) {
        m_scroll_guard.scroll_limit_px = px;
    }

    /**
     * @brief Get keyboard input device
     * @return Input device pointer, or nullptr if not available
     */
    lv_indev_t* keyboard_input() const {
        return m_keyboard;
    }

    /**
     * @brief Get display backend
     * @return Backend pointer, or nullptr if not initialized
     */
    DisplayBackend* backend() const {
        return m_backend.get();
    }

    /**
     * @brief True if any remote-screen sink (e.g. the U1 fb0 mirror) is active.
     *
     * Lets callers gate remote-only work (such as forcing a full fb0 repaint at
     * splash handoff) so devices without a remote sink pay nothing.
     */
    bool remote_screen_active() const {
        return m_remote_screen.any_active();
    }

    /// The remote-screen sink set, for attaching the built-in screen server's sink.
    helix::RemoteScreenManager& remote_screen() {
        return m_remote_screen;
    }

    /**
     * @brief Get current display width
     *
     * Read from the display, never cached: a backend may settle rotation itself
     * (a DRM plane taking 90/270 un-swaps LVGL's resolution) and a window resize
     * changes it underneath us, so any copy taken earlier can be stale.
     *
     * @return Width in pixels, or 0 if not initialized
     */
    int width() const {
        return m_display ? lv_display_get_horizontal_resolution(m_display) : m_width;
    }

    /**
     * @brief Get current display height
     * @return Height in pixels, or 0 if not initialized
     */
    int height() const {
        return m_display ? lv_display_get_vertical_resolution(m_display) : m_height;
    }

    /// On a first boot (Config::first_boot_probe), the orientation init() applied
    /// without asking: the kernel's panel_orientation, else the board's panel mount.
    /// -1 when neither applies and the interactive probe has to run.
    int first_boot_orientation() const {
        return m_first_boot_orientation;
    }

    // ========================================================================
    // Display Sleep Management
    // ========================================================================

    /**
     * @brief Check inactivity and trigger display sleep if timeout exceeded
     *
     * Call this from the main event loop. Uses LVGL's built-in inactivity
     * tracking (lv_display_get_inactive_time) and the configured sleep timeout.
     *
     * Sleep states:
     * - Awake: Full brightness
     * - Dimmed: Reduced brightness after dim timeout
     * - Sleeping: Backlight off after sleep timeout, first touch only wakes
     */
    void check_display_sleep();

    /**
     * @brief Manually wake the display
     *
     * Restores brightness to saved level. When waking from full sleep (not dim),
     * input is disabled for 200ms so the wake touch doesn't trigger UI actions.
     */
    void wake_display();

    /**
     * @brief Force display ON at startup, at the saved brightness
     *
     * Clears any sleep or dim state a previous instance left behind and applies
     * DisplaySettingsManager's brightness, so it must run after that manager's
     * subjects exist (after PrinterSession::init_core_subjects()).
     */
    void ensure_display_on();

    /**
     * @brief Set dim timeout for immediate effect
     *
     * Called by SettingsManager when user changes dim timeout setting.
     *
     * @param seconds Dim timeout (0 to disable)
     */
    void set_dim_timeout(int seconds);

    /**
     * @brief Restore display to usable state on shutdown
     *
     * Called during app cleanup to ensure display is awake before exiting.
     * Prevents next app from starting with a black screen.
     */
    void restore_display_on_shutdown();

    /**
     * @brief Stop LVGL flushing for the rest of the process
     *
     * For a reboot or shutdown of this host that has already been dispatched:
     * a framebuffer clear made after this holds until init stops us instead of
     * being painted over on the next frame. No wake or touch undoes it.
     */
    void hold_flush_for_power_off();

#if HELIX_ENABLE_SCREENSAVER
    /**
     * @brief Start a screensaver immediately for preview/testing
     *
     * Used by the settings UI to let users audition the selected screensaver
     * without waiting for the dim timeout. The next touch dismisses it via
     * wake_display(); auto-lock is suppressed since this is a manual preview,
     * not an idle timeout.
     *
     * No-op if type is OFF or a screensaver is already active.
     *
     * @param type Screensaver type to preview
     */
    void preview_screensaver(int type);

    /**
     * @brief What the screensaver gate needs from this app
     *
     * The print callback asks job_holds_machine() of the published print lifecycle; the
     * display path is `backend`'s display_backend_key(), or "unknown" without a backend.
     */
    static helix::ui::SaverHost screensaver_host(const DisplayBackend* backend);
#endif

    /**
     * @brief Check if display is currently sleeping
     * @return true if backlight is off due to inactivity
     */
    bool is_display_sleeping() const {
        return m_sleep.is_sleeping();
    }

    /// Refresh pacing read from the environment by init(); the main loop sleeps by it.
    const helix::RefreshTiming& refresh_timing() const {
        return m_refresh_timing;
    }

    /**
     * @brief Register callback for display sleep/wake transitions
     *
     * Callbacks are invoked with true on sleep entry, false on wake.
     * Used by camera stream and other background tasks to suspend
     * work while the display is off, reducing idle CPU usage.
     *
     * @param cb Callback receiving true=sleep, false=wake
     */
    void register_sleep_callback(std::function<void(bool sleeping)> cb) {
        m_sleep.register_callback(std::move(cb));
    }

    /// What runs before the first frame after a sleep (DisplaySleepHost::settle_for_wake_frame).
    void set_wake_frame_settle(std::function<void()> settle) {
        m_settle_for_wake_frame = std::move(settle);
    }

    /**
     * @brief Check if display is currently dimmed
     * @return true if backlight is at reduced brightness
     */
    bool is_display_dimmed() const {
        return m_sleep.is_dimmed();
    }

    /// A pointer that does not pass through the sleep-aware wrapper reports a press. True
    /// means it is the wake touch, which the caller hides from LVGL until it lifts.
    bool note_wake_press(int x, int y) {
        return m_sleep.note_press(x, y);
    }

    /**
     * @brief Set backlight brightness directly
     * @param percent Brightness 0-100 (clamped to 10-100 minimum)
     */
    void set_backlight_brightness(int percent);

    /**
     * @brief Check if hardware backlight control is available
     * @return true if brightness can be controlled
     */
    bool has_backlight_control() const;

    /**
     * @brief Check if backlight supports continuous dimming
     *
     * Binary backlights (GPIO, max_brightness=1) can only be on/off.
     * When false, the brightness slider is hidden and the dim-before-sleep
     * transition is skipped (Awake → Sleeping directly).
     *
     * @return true if brightness can be smoothly adjusted
     */
    bool has_dimming_control() const;

    /**
     * @brief Whether the framebuffer is being rotated in software
     *
     * True when the active backend is fbdev and a non-zero rotation is applied,
     * so every flush pays a per-frame CPU rotate over the dirty rect (boards
     * without a hardware rotation plane). Used to lower the animations default
     * on those panels, where transitions are otherwise jerky (#986).
     *
     * @return true if rotation is done in software on the fbdev backend
     */
    bool is_software_rotated() const;

    /**
     * @brief Check if hardware blanking is used for display sleep
     *
     * When true, sleep uses FBIOBLANK + backlight off (AD5M/Allwinner).
     * When false, sleep uses a software black overlay (safe for all displays).
     * Determined by backlight backend capability or config override.
     *
     * @return true if using hardware blank, false if using software overlay
     */
    bool uses_hardware_blank() const {
        return m_sleep.uses_hardware_blank();
    }

    /**
     * @brief Update gamma + warmth for the framebuffer color transform.
     *
     * Rebuilds per-channel LUTs and applies them in the flush hook.
     * Identity transform (gamma=1, warmth=0) skips the transform entirely.
     */
    void set_color_transform(float gamma, int warmth, int tint);

    /**
     * @brief Put back the flush callback a no-op stood in for.
     *
     * Frames rendered meanwhile never reached the backend, so it is asked to present the
     * whole next frame (DisplayBackend::request_full_upload()). No-op without a display
     * or a callback.
     */
    void restore_flush_cb(lv_display_flush_cb_t flush_cb);

    /** @brief Access the color transform (read-only — used by flush hook). */
    const helix::ColorTransform& color_transform() const {
        return m_color_transform;
    }

    // ========================================================================
    // Touch Calibration
    // ========================================================================

    /**
     * @brief Apply touch calibration at runtime
     *
     * Called by calibration wizard after user accepts calibration.
     * Immediately applies the affine transform to touch input without
     * requiring a restart.
     *
     * @param cal Valid calibration coefficients
     * @return true if applied successfully, false if backend doesn't support
     *         calibration or validation failed
     */
    bool apply_touch_calibration(const helix::TouchCalibration& cal);

    /**
     * @brief Get current touch calibration from backend
     *
     * Used to backup calibration before applying a new one.
     *
     * @return Current calibration, or invalid calibration if not calibrated/not fbdev
     */
    helix::TouchCalibration get_current_calibration() const;

    /**
     * @brief Check if the touch device needs calibration
     *
     * USB HID touchscreens (HDMI displays) report mapped coordinates natively
     * and don't need calibration. Only resistive/platform touchscreens do.
     *
     * @return true if calibration wizard should be offered
     */
    bool needs_touch_calibration() const;

    /**
     * @brief Check whether the manual calibration entry point should be offered
     *
     * True for any real touch panel. needs_touch_calibration() answers a
     * narrower question — "auto-fire the wizard on first boot" — and its
     * name/range heuristic cannot see an orientation mismatch, so it must not
     * gate the manual path (prestonbrown/helixscreen#1259).
     *
     * @return true if the Settings entry point should be reachable
     */
    bool supports_touch_calibration() const;

    /**
     * @brief Temporarily disable affine calibration for recalibration
     *
     * During recalibration, touch coordinates must be raw (pre-affine) to avoid
     * the feedback loop where transformed coordinates are fed back into calibration.
     * Safe no-op on non-fbdev backends.
     */
    void disable_affine_calibration();

    /**
     * @brief Re-enable affine calibration after recalibration
     *
     * Restores the stored calibration transform. Safe no-op on non-fbdev backends.
     */
    void enable_affine_calibration();

    // ICalibrationSink — thin adapters so TouchCalibrationSession can drive the
    // backup/restore dance against DisplayManager (and against a fake in tests).
    helix::TouchCalibration current_calibration() const override {
        return get_current_calibration();
    }
    bool apply_calibration(const helix::TouchCalibration& cal) override {
        return apply_touch_calibration(cal);
    }
    void disable_affine() override {
        disable_affine_calibration();
    }
    void enable_affine() override {
        enable_affine_calibration();
    }
    void clear_calibration() override {
        if (m_backend) {
            m_backend->clear_calibration();
        }
    }
    bool apply_touch_range(bool swap_axes, int min_x, int min_y, int max_x, int max_y,
                           helix::TouchRangeSource source) override {
        return m_backend &&
               m_backend->apply_touch_range(swap_axes, min_x, min_y, max_x, max_y, source);
    }
    helix::LiveTouchRange current_touch_range() const override;

    /**
     * @brief Mark whether a touch-calibration UI (overlay or wizard) is on screen
     *
     * The debug-touches ripple is suppressed while this is true: the calibration
     * draws its own ripple, and during point capture affine is disabled so the
     * global debug ripple would render raw (Y-inverted) coordinates (#943).
     */
    void set_touch_calibration_active(bool active) {
        m_touch_calibration_active = active;
    }
    /// ICalibrationSink spelling of set_touch_calibration_active(). Also holds the
    /// evdev range still for the capture (helix::set_touch_capture_active).
    void set_capture_active(bool active) override {
        set_touch_calibration_active(active);
        helix::set_touch_capture_active(active);
    }
    bool is_touch_calibration_active() const {
        return m_touch_calibration_active;
    }

    /**
     * @brief Run rotation probe on first boot (fbdev only)
     *
     * Cycles through 0°, 90°, 180°, 270° rotations showing "Tap anywhere
     * if you can read this" for 5 seconds each. Two-tap confirmation prevents
     * accidental selection. Saves result to config and returns.
     *
     * Only called when no rotation is configured and the probe hasn't run before.
     * No-op on SDL/DRM backends.
     */
    void run_rotation_probe();

    // ========================================================================
    // Static Timing Functions (portable across platforms)
    // ========================================================================

    /**
     * @brief Get current tick count in milliseconds
     *
     * Uses SDL_GetTicks() on desktop, clock_gettime() on embedded.
     *
     * @return Milliseconds since some fixed point (wraps at ~49 days)
     */
    static uint32_t get_ticks();

    /**
     * @brief Delay for specified milliseconds
     *
     * Uses SDL_Delay() on desktop, nanosleep() on embedded.
     *
     * @param ms Milliseconds to delay
     */
    static void delay(uint32_t ms);

    // ========================================================================
    // Window Resize Handler (Desktop/SDL)
    // ========================================================================

    /**
     * @brief Callback type for resize notifications
     */
    using ResizeCallback = void (*)();

    /**
     * @brief Initialize resize handler on the given screen
     *
     * Sets up SIZE_CHANGED event listener with debouncing. Call once during
     * application startup after the screen is created; a second call moves the
     * listener to the new screen. shutdown() and the destructor remove it.
     *
     * @param screen Root screen object to monitor
     */
    void init_resize_handler(lv_obj_t* screen);

    /**
     * @brief Register callback for resize events
     *
     * Callbacks are invoked after 250ms debounce to avoid excessive
     * redraws during continuous resize operations.
     *
     * @param callback Function to call when resize completes
     */
    void register_resize_callback(ResizeCallback callback);

    /**
     * @brief Suspend/resume the debounced resize-callback fanout
     *
     * While suspended, a SIZE_CHANGED on the monitored screen arms no debounce
     * timer and an already-armed one expires without fanning out. The rotation
     * probe holds this for its whole run: every rotation it tests would
     * otherwise fire the registered callbacks, and the theme layout refresh
     * among them takes seconds on a slow panel (measured 2.9s on a K1C) inside
     * the lv_timer_handler() call that the probe's tap poll loop makes on every
     * iteration. The probe re-applies the confirmed rotation when it finishes
     * and Application::run_rotation_probe_and_layout() refreshes the theme and
     * the LayoutManager after it returns, so the suppressed intermediate
     * refreshes are not needed.
     *
     * @param suspended True to suspend fanout, false to resume
     */
    void set_resize_fanout_suspended(bool suspended);

    /**
     * @brief Whether the debounced resize-callback fanout is suspended
     */
    bool resize_fanout_suspended() const {
        return m_resize_fanout_suspended;
    }

  private:
    /// Re-apply the user's brightness 20 s after startup on boards whose stock firmware
    /// dims the panel behind our back (board::Info::firmware_dims_backlight_after_boot).
    void schedule_brightness_reassert();

    // Test-only seam (#1049): grants the test harness access to the private
    // sleep/wake/power-off members so the idle paths can be exercised without a
    // full init(). See tests/test_helpers/display_manager_test_access.h.
    friend class DisplayManagerTestAccess;

    /// Rotates the display through the backend. width()/height() then read whatever it settled on.
    /// Rotation from the config, the environment and, on a first boot, the board's
    /// panel mount, applied to the display. init() calls it after creating the
    /// display and before the input devices.
    void apply_startup_rotation(const Config& config);
    void settle_display_rotation(lv_display_rotation_t rot, int phys_w, int phys_h);

    bool m_initialized = false;
    bool m_shutting_down = false;
    /// Unrotated size requested of the backend, which may create the display at another size;
    /// width()/height() report the display's actual size.
    int m_width = 0;
    int m_height = 0;
    int m_first_boot_orientation = -1;
    bool m_size_was_explicit = false;

    std::unique_ptr<DisplayBackend> m_backend;
    lv_display_t* m_display = nullptr;
    lv_indev_t* m_pointer = nullptr;
    lv_indev_t* m_keyboard = nullptr;
    lv_group_t* m_input_group = nullptr;
    // Clears m_pointer/m_keyboard the moment LVGL deletes the indev underneath
    // them - lv_evdev on an unplugged panel's ENODEV, or lv_deinit() at
    // shutdown - so a later swap or read never reaches a freed device through
    // either member.
    helix::IndevDeleteWatch m_indev_delete_watch;

    // Backlight control
    std::unique_ptr<BacklightBackend> m_backlight;

    // Per-channel framebuffer color correction (gamma + warmth)
    helix::ColorTransform m_color_transform;
    lv_display_flush_cb_t m_original_flush_cb_for_color = nullptr;
    void install_color_transform_hook();

    // Remote-screen frame mirror (fb0 sink on the Snapmaker U1). Fed from the
    // flush hook per dirty area; a cheap early-out when no sinks are attached.
    helix::RemoteScreenManager m_remote_screen;

    // Refresh pacing from the environment, parsed once by init() and applied after its
    // input devices exist, since new devices start at LVGL's default read period.
    helix::RefreshTiming m_refresh_timing;

    std::function<void()> m_settle_for_wake_frame;

    // Dim, screensaver, sleep and wake. Reads m_backend, m_backlight, m_display and
    // m_settle_for_wake_frame through references, so it must be declared after them.
    helix::DisplaySleepController m_sleep;

    bool m_touch_calibration_active = false; // suppresses the debug-touches ripple (#943)
    // Original pointer read callback (before sleep-aware wrapper)
    lv_indev_read_cb_t m_original_pointer_read_cb = nullptr;

    // Runs inside sleep_aware_read_cb, so only where that wrapper is installed
    helix::ScrollClickGuard m_scroll_guard;
    // The wrapped pointer's wake touch; sleep_aware_read_cb wraps m_pointer alone
    helix::WakeTouchLatch m_wake_latch;

    // Resize handler state
    std::vector<ResizeCallback> m_resize_callbacks;
    bool m_resize_fanout_suspended = false;
    static constexpr uint32_t kResizeDebounceMs = 250;
    // Trailing-edge: every SIZE_CHANGED pushes the fan-out kResizeDebounceMs past it.
    // Its callback dereferences this manager; the member cancels it on destruction.
    helix::ui::CoalescedTimer m_resize_debounce{kResizeDebounceMs};
    // The object carrying resize_event_cb with this manager as its user data;
    // cleared when that object is deleted.
    lv_obj_t* m_resize_screen = nullptr;
    // Its callback dereferences this manager, so it must not outlive it.
    helix::ui::LvglTimerGuard m_brightness_reassert_timer;
    // Armed while the pointer device is gone, retrying the backend's reopen
    // every kPointerReopenPeriodMs (#2069). Its callback dereferences this
    // manager, so it must not outlive it.
    helix::ui::LvglTimerGuard m_pointer_reopen_timer;
    static constexpr uint32_t kPointerReopenPeriodMs = 1000;
    // The scroll setup init() gave the pointer, for a reopened one to get the same.
    int m_scroll_throw = 0;
    int m_scroll_limit = 0;

    static void resize_event_cb(lv_event_t* e);
    void on_screen_size_changed(lv_obj_t* screen);
    void fan_out_resize();
    /// Cancels the pending debounce and detaches resize_event_cb from m_resize_screen.
    /// Its SIZE_CHANGED and DELETE registrations both go.
    void remove_resize_handler();

    /**
     * @brief Configure scroll behavior on pointer device
     */
    void configure_scroll(int scroll_throw, int scroll_limit);

    /// Applies scroll, long-press, the sleep-aware wrapper and the scroll guard to
    /// a freshly created m_pointer, from finish_input_setup().
    void configure_pointer(int scroll_throw, int scroll_limit);

    /// Registers m_pointer/m_keyboard with m_indev_delete_watch, right after
    /// the device is created.
    void watch_pointer();
    void watch_keyboard();

    /// m_indev_delete_watch's callback for m_pointer: starts the reopen retry,
    /// except while shutting down, when lv_deinit() is deleting every indev.
    static void on_pointer_deleted(void* ctx, const lv_indev_t* indev);
    /// One reopen attempt; on success adopts the device with init()'s pointer
    /// setup and stops the retry. Named so a test can run it directly.
    static void pointer_reopen_tick(lv_timer_t* t);

    /**
     * @brief Create the debug-touch ripple timer
     *
     * No-op (returns nullptr) when m_pointer is null. The timer runs
     * unconditionally once created; RuntimeConfig::debug_touches() is checked
     * inside so the Settings toggle takes effect without a restart. Reads the
     * pointer through pointer_input() on every tick rather than a copy
     * captured at creation time, so an unplug (which clears m_pointer) is
     * seen here too.
     *
     * @return The created timer, so a caller can force it ready (lv_timer_ready())
     */
    lv_timer_t* install_debug_touch_timer();

    /// The debug-touch timer's tick, named so a test can call it directly
    /// instead of only through lv_timer_handler().
    static void debug_touch_tick(lv_timer_t* t);

    /// The single assignment behind instance(): init() publishes this manager
    /// through it, shutdown() clears it. Named so both call sites read as the
    /// same action instead of a bare pointer write repeated twice.
    static void set_active_instance(DisplayManager* dm);

    /// The input setup init() runs once m_pointer exists: configures the pointer,
    /// creates the keyboard, then applies m_refresh_timing to the timers both
    /// devices just created.
    void finish_input_setup(int scroll_throw, int scroll_limit);

    /// Create the backend's keyboard indev, report whether it is a physical
    /// keyboard to DisplaySettingsManager, and watch and group it.
    void create_keyboard_input();

    /**
     * @brief Set up keyboard input group
     */
    void setup_keyboard_group();

    /**
     * @brief End the press LVGL is tracking on every pointer, until each one lifts
     *
     * Runs on wake, so the touch that woke the display cannot click, long-press or
     * scroll on a pointer the sleep-aware wrapper does not cover.
     */
    static void cancel_pointer_presses();

    /**
     * @brief Sleep-aware input wrapper callback
     *
     * Wraps original read callback to absorb touches when sleeping.
     * Sets m_wake_requested flag and returns RELEASED state, preventing
     * UI events from firing while the display wakes.
     */
    static void sleep_aware_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

    /**
     * @brief Install sleep-aware wrapper on pointer input device
     *
     * Called during init() to wrap the backend's read callback.
     */
    void install_sleep_aware_input_wrapper();

    /**
     * @brief Fall back from DRM to fbdev if hardware rotation is unsupported
     *
     * Checks if current DRM backend supports the requested rotation.
     * If not, destroys the DRM display and recreates on fbdev.
     *
     * @param rot Requested LVGL rotation
     * @param splash_active Whether splash process owns framebuffer
     * @return true if display is usable (no fallback needed, or fallback succeeded)
     */
    bool try_drm_to_fbdev_fallback(lv_display_rotation_t rot, bool splash_active);

    /**
     * @brief Warn if fbdev resolution exceeds the high-DPI threshold
     *
     * Logs a warning and enqueues a toast because fbdev cannot switch
     * modes at runtime — the user must lower the framebuffer resolution
     * via kernel parameters and reboot.
     */
    void warn_fbdev_high_dpi();
};
