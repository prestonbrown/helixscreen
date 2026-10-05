// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file display_backend.h
 * @brief Abstract platform-independent interface for display and input initialization
 *
 * @pattern Pure virtual interface + static create()/create_auto() factory methods
 * @threading Implementation-dependent; see concrete implementations
 *
 * @see display_backend_sdl.cpp, display_backend_fbdev.cpp
 */

#pragma once

#include "data_root_resolver.h"
#include "text_io.h"
#include "touch_calibration.h"

#include <lvgl.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/**
 * @brief Display backend types supported by HelixScreen
 */
enum class DisplayBackendType {
    SDL,   ///< SDL2 for desktop development (macOS/Linux with X11/Wayland)
    FBDEV, ///< Linux framebuffer (/dev/fb0) - works on most embedded Linux
    DRM,   ///< Linux DRM/KMS - modern display API, better for Pi
    AUTO   ///< Auto-detect best available backend
};

/**
 * @brief Result of display resolution auto-detection
 *
 * Used by detect_resolution() to return hardware-detected display dimensions.
 * Only valid for fbdev/DRM backends; SDL always returns invalid.
 */
struct DetectedResolution {
    int width = 0;
    int height = 0;
    bool valid = false;
};

/**
 * @brief Convert DisplayBackendType to string for logging
 */
inline const char* display_backend_type_to_string(DisplayBackendType type) {
    switch (type) {
    case DisplayBackendType::SDL:
        return "SDL";
    case DisplayBackendType::FBDEV:
        return "Framebuffer";
    case DisplayBackendType::DRM:
        return "DRM/KMS";
    case DisplayBackendType::AUTO:
        return "Auto";
    default:
        return "Unknown";
    }
}

namespace helix::ui {

/**
 * @brief Short lowercase name of the running display path, for keys and fingerprints
 *
 * "sdl", "fbdev", "drm", or "egl" for DRM rendering through EGL/OpenGL ES.
 */
inline const char* display_backend_key(DisplayBackendType type, bool gpu_accelerated) {
    switch (type) {
    case DisplayBackendType::SDL:
        return "sdl";
    case DisplayBackendType::FBDEV:
        return "fbdev";
    case DisplayBackendType::DRM:
        return gpu_accelerated ? "egl" : "drm";
    case DisplayBackendType::AUTO:
        return "auto";
    }
    return "unknown";
}

} // namespace helix::ui

/**
 * @brief Convert rotation degrees to LVGL rotation enum
 *
 * Maps user-facing degree values (0, 90, 180, 270) to LVGL's
 * LV_DISPLAY_ROTATION_* constants. Invalid values default to 0.
 *
 * @param degrees Rotation in degrees (0, 90, 180, 270)
 * @return LVGL rotation enum value
 */
inline lv_display_rotation_t degrees_to_lv_rotation(int degrees) {
    switch (degrees) {
    case 90:
        return LV_DISPLAY_ROTATION_90;
    case 180:
        return LV_DISPLAY_ROTATION_180;
    case 270:
        return LV_DISPLAY_ROTATION_270;
    default:
        return LV_DISPLAY_ROTATION_0;
    }
}

/**
 * @brief Detect panel orientation from kernel cmdline
 *
 * Parses /proc/cmdline for video=*:panel_orientation=* to determine if the
 * physical panel is mounted rotated. Works on any Linux regardless of display
 * backend. No spdlog dependency so it's safe for splash binary.
 *
 * @return Rotation in degrees (0, 90, 180, 270), or -1 if not detected
 */
inline int detect_panel_orientation_from_cmdline() {
    const std::optional<std::string> first = helix::text_io::read_first_line("/proc/cmdline");
    if (!first) {
        return -1;
    }
    const std::string& line = *first;

    const std::string needle = "panel_orientation=";
    auto pos = line.find(needle);
    if (pos == std::string::npos) {
        return -1;
    }

    auto val_start = pos + needle.size();
    auto val_end = line.find_first_of(" \t\n", val_start);
    std::string orientation = line.substr(val_start, val_end - val_start);

    if (orientation == "normal")
        return 0;
    if (orientation == "upside_down")
        return 180;
    if (orientation == "left_side_up")
        return 90;
    if (orientation == "right_side_up")
        return 270;

    return -1;
}

namespace helix {

/**
 * @brief The first `"key": <value>` in the settings files, as the raw value token
 *
 * Searches settings.json in the config dir, then the legacy helixconfig.json
 * locations, and returns the alphanumeric token after the colon (digits, `true`,
 * `false`). The first occurrence anywhere in a file whose token `accept` takes
 * wins, at any nesting depth, so a nested `"key": null` does not hide a real value
 * later in the file. A plain scan, not a JSON parse: the splash and watchdog read
 * their settings before anything else runs and link no regex code.
 *
 * @param from If non-null, receives the path the value came from.
 * @param accept If non-null, occurrences whose token it rejects are skipped.
 */
inline std::optional<std::string> read_settings_scalar(std::string_view key,
                                                       std::string* from = nullptr,
                                                       bool (*accept)(std::string_view) = nullptr) {
    const std::string paths[] = {helix::writable_path("settings.json"),
                                 helix::writable_path("helixconfig.json"), "helixconfig.json",
                                 "/opt/helixscreen/helixconfig.json"};
    const std::string quoted = "\"" + std::string(key) + "\"";
    auto is_space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    auto is_token = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };

    for (const auto& path : paths) {
        const std::optional<std::string> content = helix::text_io::read_file(path);
        if (!content) {
            continue;
        }
        const std::string_view text(*content);
        for (size_t at = text.find(quoted); at != std::string_view::npos;
             at = text.find(quoted, at + 1)) {
            size_t i = at + quoted.size();
            while (i < text.size() && is_space(text[i]))
                ++i;
            if (i >= text.size() || text[i] != ':')
                continue;
            ++i;
            while (i < text.size() && is_space(text[i]))
                ++i;
            size_t end = i;
            while (end < text.size() && is_token(text[end]))
                ++end;
            if (end == i || (accept && !accept(text.substr(i, end - i))))
                continue;
            if (from) {
                *from = path;
            }
            return std::string(text.substr(i, end - i));
        }
    }
    return std::nullopt;
}

/// The first occurrence of `key` holding a non-negative integer.
inline std::optional<int> read_settings_int(std::string_view key, std::string* from = nullptr) {
    const auto token = read_settings_scalar(key, from, [](std::string_view t) {
        return helix::text_io::parse_int<int>(t).has_value();
    });
    return token ? helix::text_io::parse_int<int>(*token) : std::nullopt;
}

/// The first occurrence of `key` holding `true` or `false`.
inline std::optional<bool> read_settings_bool(std::string_view key, std::string* from = nullptr) {
    const auto token = read_settings_scalar(
        key, from, [](std::string_view t) { return t == "true" || t == "false"; });
    return token ? std::optional<bool>(*token == "true") : std::nullopt;
}

} // namespace helix

/**
 * @brief Read display rotation from settings.json
 *
 * Used by watchdog and splash binaries which don't use the full Config system.
 *
 * @param default_value Fallback rotation in degrees when no setting exists
 * @return Rotation in degrees (0, 90, 180, 270); 0 for an invalid value
 */
inline int read_config_rotation(int default_value = 0) {
    const auto parsed = helix::read_settings_int("rotate");
    if (!parsed) {
        return default_value;
    }
    const int rotation = *parsed;
    return (rotation == 90 || rotation == 180 || rotation == 270) ? rotation : 0;
}

namespace helix {

/**
 * @brief Rotation for the splash and the watchdog's crash dialog
 *
 * The CLI value, else the configured one, else the kernel's panel_orientation
 * (informational only: the kernel does not rotate the framebuffer itself).
 */
inline int standalone_rotation(int cli_rotation) {
    const int rotation = cli_rotation != 0 ? cli_rotation : read_config_rotation(0);
    if (rotation != 0) {
        return rotation;
    }
    const int kernel = detect_panel_orientation_from_cmdline();
    return kernel > 0 ? kernel : 0;
}

/**
 * @brief Rotation DisplayManager::init() applies at startup
 *
 * The requested rotation, else the kernel's panel_orientation when the
 * first-boot rotation probe owns this boot. It must be applied in init(),
 * before the input devices exist: the backends gate the stored touch range and
 * calibration on the display's rotation from create_input_pointer()
 * (prestonbrown/helixscreen#1428).
 *
 * @param requested CLI rotation in degrees (0 = none requested)
 * @param probe_wanted Whether the first-boot rotation probe owns this boot
 * @param kernel_orientation DisplayBackend::detect_panel_orientation(), -1 if absent
 */
inline int startup_rotation(int requested, bool probe_wanted, int kernel_orientation) {
    if (requested != 0 || !probe_wanted) {
        return requested;
    }
    return kernel_orientation > 0 ? kernel_orientation : 0;
}

} // namespace helix

/**
 * @brief Abstract display backend interface
 *
 * Provides platform-agnostic display and input initialization.
 * Follows the same factory pattern as WifiBackend.
 *
 * Lifecycle:
 * 1. Factory creates backend via DisplayBackend::create() or create_auto()
 * 2. Call create_display() to initialize display hardware
 * 3. Call create_input_pointer() to initialize touch/mouse input
 * 4. Optionally call create_input_keyboard() for keyboard support
 * 5. Backend is destroyed when unique_ptr goes out of scope
 *
 * Thread safety: Backend creation and destruction should be done from
 * the main thread. Display operations are typically single-threaded.
 */
class DisplayBackend {
  public:
    virtual ~DisplayBackend() {
        if (s_active == this) {
            s_active = nullptr;
        }
    }

    /**
     * @brief What angle is the picture actually presented at, in degrees?
     *
     * The backend that applied the rotation is the only thing that knows what
     * it did with the angle it was handed: told LVGL, gave it to a scanout
     * plane, or failed and left the panel upright. Overriding this is how a
     * backend that does not route rotation through LVGL keeps the touch
     * pipeline honest, since LVGL's own rotation stops describing the picture
     * the moment something else owns it.
     *
     * @param disp Display to query, or nullptr for the default display
     * @return 0, 90, 180 or 270
     */
    virtual int applied_rotation_degrees(lv_display_t* disp = nullptr) const {
        return lvgl_rotation_degrees(disp);
    }

    /**
     * @brief The backend currently driving the display, or nullptr
     *
     * Maintained by construction and destruction, so a backend swap keeps it
     * right with nothing to remember at the call site. Display setup and
     * teardown are main-thread only, which is what makes a plain pointer
     * sufficient here.
     */
    static DisplayBackend* active() {
        return s_active;
    }

    /**
     * @brief The angle LVGL believes it is rendering at, in degrees
     *
     * The default answer for `applied_rotation_degrees()` and the fallback
     * when no backend is live, such as the watchdog's crash dialog.
     */
    static int lvgl_rotation_degrees(lv_display_t* disp = nullptr) {
        lv_display_t* target = disp ? disp : lv_display_get_default();
        return target ? static_cast<int>(lv_display_get_rotation(target)) * 90 : 0;
    }

    /// Gesture thresholds for an evdev multi-touch pointer. ROTATE is pushed out of
    /// reach so PINCH and two-finger pan are the only two-finger gestures that win.
    static void configure_touch_gestures(lv_indev_t* indev);

    // ========================================================================
    // Display Creation
    // ========================================================================

    /**
     * @brief Initialize the display
     *
     * Creates the LVGL display object for this backend. This allocates
     * display buffers and initializes the underlying display hardware.
     *
     * @param width Display width in pixels
     * @param height Display height in pixels
     * @return LVGL display object, or nullptr on failure
     */
    virtual lv_display_t* create_display(int width, int height) = 0;

    // ========================================================================
    // Input Device Creation
    // ========================================================================

    /**
     * @brief Create pointer input device (mouse/touchscreen)
     *
     * Initializes the primary input device for the display.
     * For desktop: mouse input via SDL
     * For embedded: touchscreen via evdev
     *
     * @return LVGL input device, or nullptr on failure
     */
    virtual lv_indev_t* create_input_pointer() = 0;

    /**
     * @brief Create keyboard input device (optional)
     *
     * Not all backends support keyboard input. Returns nullptr
     * if keyboard is not available or not applicable.
     *
     * @return LVGL input device, or nullptr if not supported
     */
    virtual lv_indev_t* create_input_keyboard() {
        return nullptr;
    }

    /**
     * @brief Whether create_input_keyboard() opened a physical keyboard device.
     *
     * A backend whose keyboard indev exists without one (the SDL window
     * keyboard) answers false, so the indev pointer is never a presence test.
     */
    virtual bool has_hardware_keyboard() const {
        return false;
    }

    // ========================================================================
    // Backend Information
    // ========================================================================

    /**
     * @brief Get the backend type
     */
    virtual DisplayBackendType type() const = 0;

    /**
     * @brief Get backend name for logging/display
     */
    virtual const char* name() const = 0;

    /**
     * @brief Check if this backend is available on the current system
     *
     * For SDL: checks if display can be opened
     * For FBDEV: checks if /dev/fb0 exists and is accessible
     * For DRM: checks if /dev/dri/card0 exists and is accessible
     *
     * @return true if backend can be used
     */
    virtual bool is_available() const = 0;

    /**
     * @brief Detect the native display resolution from hardware
     *
     * Queries the display hardware for its native resolution. This allows
     * auto-configuration without requiring explicit CLI size arguments.
     *
     * For FBDEV: queries FBIOGET_VSCREENINFO for xres/yres
     * For DRM: queries the connector's preferred mode
     * For SDL: returns invalid (desktop uses presets/CLI)
     *
     * @return DetectedResolution with valid=true if detection succeeded
     */
    virtual DetectedResolution detect_resolution() const {
        return {}; // Default: not supported
    }

    /**
     * @brief Check if the display is still active/owned by this process
     *
     * Used by the splash screen to detect when the main app takes over
     * the display. For framebuffer/DRM backends, this checks if another
     * process has opened the display device.
     *
     * @return true if display is still active, false if taken over
     */
    virtual bool is_active() const {
        return true;
    }

    /**
     * @brief Clear the entire framebuffer to a solid color
     *
     * Used by splash screen to wipe any pre-existing content (like Linux
     * console text) before rendering the UI. This writes directly to the
     * framebuffer, bypassing LVGL's dirty region tracking.
     *
     * Must be called AFTER create_display() and before any LVGL rendering.
     *
     * @param color 32-bit ARGB color (0xAARRGGBB format, use 0xFF for full opacity)
     * @return true if framebuffer was cleared, false on error or not supported
     */
    virtual bool clear_framebuffer(uint32_t color) {
        (void)color;
        return false; // Not supported by default
    }

    /**
     * @brief Unblank the display and reset pan position
     *
     * Explicitly enables the display backlight and resets the framebuffer
     * pan position to (0,0). This is essential on some embedded systems
     * (like AD5M) where the display may be blanked by other processes
     * during boot.
     *
     * Uses standard Linux framebuffer ioctls:
     * - FBIOBLANK with FB_BLANK_UNBLANK to enable display
     * - FBIOPAN_DISPLAY with yoffset=0 to reset pan position
     *
     * Should be called early in startup, before or after create_display().
     *
     * @return true if unblank succeeded, false on error or not supported
     */
    virtual bool unblank_display() {
        return false; // Not supported by default
    }

    /**
     * @brief Report whether this backend can really power the panel down.
     *
     * Distinct from blank_display(): blanking (FB_BLANK_NORMAL) merely stops the
     * scan-out, while power-off (FB_BLANK_POWERDOWN / DPMS off) signals the panel
     * to drop its backlight/transceiver. Used for HDMI/fbdev devices that have no
     * sysfs/ioctl backlight (#1049) — when true, idle entry powers the panel off
     * instead of painting a software black overlay.
     *
     * @return true if power_off()/power_on() perform a real hardware power transition
     */
    virtual bool supports_power_off() const {
        return false; // Not supported by default
    }

    /**
     * @brief Power the panel down (fbdev FB_BLANK_POWERDOWN / DRM DPMS off).
     *
     * Only meaningful when supports_power_off() returns true. The counterpart
     * power_on() MUST be called before the next render after wake to honor the
     * #303 wake-race (framebuffer not ready before LVGL renders → black screen).
     *
     * @return true if the power-down succeeded
     */
    virtual bool power_off() {
        return false; // Not supported by default
    }

    /**
     * @brief Power the panel back on (counterpart to power_off()).
     *
     * @return true if the power-on succeeded
     */
    virtual bool power_on() {
        return false; // Not supported by default
    }

    /**
     * @brief Tell the backend that an external splash process owns the framebuffer.
     *
     * When set, create_display() skips FBIOBLANK and other ioctls that would
     * disrupt the splash image.
     */
    virtual void set_splash_active(bool active) {
        (void)active;
    }

    /**
     * @brief Tell the backend the user explicitly requested a size via -s.
     *
     * When true, backends that can detect a native resolution (fbdev, DRM) log
     * warnings and enqueue toasts if the requested resolution cannot be honored.
     * Must be called before create_display(). Backends without a notion of a
     * native resolution (SDL) ignore this.
     */
    virtual void set_size_was_explicit(bool explicit_size) {
        (void)explicit_size;
    }

    /**
     * @brief Whether GPU-accelerated rendering (EGL/OpenGL ES) is active.
     *
     * Only the DRM backend can drive an EGL/GLES path; all other backends
     * render on the CPU and return false.
     *
     * @return true if hardware-accelerated rendering is in use
     */
    virtual bool is_gpu_accelerated() const {
        return false;
    }

    /**
     * @brief Make the next presented frame carry every pixel, not only the flushed areas.
     *
     * A backend that sends the GPU only the areas LVGL flushes keeps what it last
     * received everywhere else. That goes stale when the image changes without passing
     * through the backend's flush: frames rendered while a no-op flush callback stood in
     * for it, or a color transform that rewrites pixels on their way to it. Backends that
     * present whole frames have nothing to do.
     */
    virtual void request_full_upload() {}

    /**
     * @brief Apply a rotation, and decide what LVGL is told about it
     *
     * The backend is the only writer of the display's rotation. This default
     * hands the angle to LVGL, which rotates on the flush path and transforms
     * pointer input to match, so a backend with nothing special to do inherits
     * working rotation by saying nothing.
     *
     * A backend that rotates by some other means - a scanout plane - overrides
     * this and reports the result through applied_rotation_degrees(), because
     * LVGL's own rotation stops describing the panel once something else owns
     * it (prestonbrown/helixscreen#1275).
     *
     * @param disp Display to rotate
     * @param rot LVGL rotation enum
     * @param phys_w Native panel width (pre-rotation)
     * @param phys_h Native panel height (pre-rotation)
     */
    virtual void set_display_rotation(lv_display_t* disp, lv_display_rotation_t rot, int phys_w,
                                      int phys_h) {
        (void)phys_w;
        (void)phys_h;
        if (disp != nullptr) {
            lv_display_set_rotation(disp, rot);
        }
    }

    /**
     * @brief Check if hardware can rotate without software fallback
     *
     * Returns true if the backend can handle the requested rotation
     * without CPU-based pixel manipulation. Used by DisplayManager
     * to decide whether to fall back to a different backend.
     *
     * @param rot Requested rotation
     * @return true if hardware rotation is supported, false if software needed
     */
    virtual bool supports_hardware_rotation(lv_display_rotation_t rot) const {
        (void)rot;
        return true; // Most backends handle rotation natively
    }

    /**
     * @brief Detect panel orientation from DRM connector properties
     *
     * Queries the kernel's DRM connector "panel orientation" property to
     * determine if the physical panel is mounted rotated. Works even on
     * fbdev backends since DRM is still present in the kernel.
     *
     * @return Rotation in degrees (0, 90, 180, 270), or -1 if not detected
     */
    static int detect_panel_orientation();

    /**
     * @brief Blank the display (turn off backlight via framebuffer ioctl)
     *
     * Blanks the display using the FBIOBLANK ioctl with FB_BLANK_NORMAL.
     * This is the counterpart to unblank_display() and should be called
     * when putting the display to sleep.
     *
     * @return true if blank succeeded, false on error or not supported
     */
    virtual bool blank_display() {
        return false; // Not supported by default
    }

    // ========================================================================
    // Touch Calibration
    // ========================================================================

    /**
     * @brief Apply new touch calibration coefficients
     * @return true if calibration was applied
     */
    virtual bool set_calibration(const helix::TouchCalibration& cal) {
        (void)cal;
        return false;
    }

    /**
     * @brief Get current touch calibration
     * @return Current calibration (valid=false if none)
     */
    virtual helix::TouchCalibration get_calibration() const {
        return helix::TouchCalibration{};
    }

    /**
     * @brief Check if the detected touch device needs calibration
     * @return true if calibration wizard should be offered
     */
    virtual bool needs_touch_calibration() const {
        return false;
    }

    /**
     * @brief Check whether the manual calibration entry point should be offered
     *
     * Separate from needs_touch_calibration(), which means "auto-fire the wizard
     * on first boot". This one means "a human can reach the wizard from
     * Settings", and is true for any real touch device. See
     * helix::device_supports_calibration() for why the two must not share a
     * flag (prestonbrown/helixscreen#1259).
     *
     * @return true if the Settings entry point should be reachable
     */
    virtual bool supports_touch_calibration() const {
        return false;
    }

    /**
     * @brief Temporarily disable affine calibration for recalibration
     */
    virtual void disable_affine_calibration() {}

    /**
     * @brief Re-enable affine calibration after recalibration
     */
    virtual void enable_affine_calibration() {}

    /**
     * @brief Discard the stored calibration, leaving the device uncalibrated
     *
     * Stronger than disable_affine_calibration(), which only stops the stored
     * matrix from being applied: this forgets it, so enable_affine_calibration()
     * cannot resurrect it. Used to put a never-calibrated device back the way an
     * aborted calibration session found it.
     */
    virtual void clear_calibration() {}

    /**
     * @brief Re-program the evdev linear stage's ABS range and axis swap
     *
     * The stage that runs BEFORE the affine matrix: lv_evdev swaps the axes, then
     * scales the driver-declared ABS range onto the display, then clamps. That
     * clamp is lossy, so a range the panel does not actually emit cannot be
     * repaired by any affine on top - the three-point calibration solves for this
     * range instead (prestonbrown/helixscreen#1259, #1276).
     *
     * min > max on an axis inverts it, which is deliberate and supported.
     *
     * @param source What the diagnostics record as supplying the range: Stored
     *               for a solved one, or the original source when a calibration
     *               session puts the pre-session range back.
     * @return true if the backend re-programmed the device; false on backends
     *         with no evdev stage, where the caller must keep the affine-only path
     */
    virtual bool apply_touch_range(bool swap_axes, int min_x, int min_y, int max_x, int max_y,
                                   helix::TouchRangeSource source) {
        (void)source;
        (void)swap_axes;
        (void)min_x;
        (void)min_y;
        (void)max_x;
        (void)max_y;
        return false;
    }

    // ========================================================================
    // Factory Methods
    // ========================================================================

    /**
     * @brief Create a specific backend type
     *
     * @param type Backend type to create
     * @return Backend instance, or nullptr if type not available/compiled
     */
    static std::unique_ptr<DisplayBackend> create(DisplayBackendType type);

    /**
     * @brief Auto-detect and create the best available backend
     *
     * Detection order (first available wins):
     * 1. Check HELIX_DISPLAY_BACKEND environment variable override
     * 2. DRM (if compiled and /dev/dri/card0 accessible)
     * 3. Framebuffer (if compiled and /dev/fb0 accessible)
     * 4. SDL (fallback for desktop)
     *
     * @return Backend instance, or nullptr if no backend available
     */
    static std::unique_ptr<DisplayBackend> create_auto();

    /**
     * @brief Convenience: auto-detect and create backend
     *
     * Same as create_auto(), provided for simpler calling code.
     */
    static std::unique_ptr<DisplayBackend> create() {
        return create_auto();
    }

    /**
     * @brief Whether a failed create_display() should be retried on fbdev
     *
     * The in-process fallback decision DisplayManager::init() makes after
     * create_display() returns: retry only when there is no display AND the
     * backend that just failed was not already fbdev (fbdev is the last resort —
     * falling back to itself would loop).
     *
     * A static predicate rather than an inline condition so it is reachable from
     * a unit test. init() itself initializes LVGL and opens real devices, so the
     * only alternative is a test that re-types the condition and can never go red.
     *
     * @param backend The backend whose create_display() just returned
     * @param display What create_display() returned (nullptr on failure)
     * @return true if the fbdev fallback should be attempted
     */
    static bool should_try_fbdev_fallback(const DisplayBackend* backend,
                                          const lv_display_t* display) {
        return display == nullptr && backend != nullptr &&
               backend->type() != DisplayBackendType::FBDEV;
    }

  protected:
    DisplayBackend() {
        s_active = this;
    }

  private:
    inline static DisplayBackend* s_active = nullptr;
};

/**
 * @brief Rotation the display is actually running at, in degrees
 *
 * The single source for "is the display rotated right now?" - the question
 * the touch pipeline asks in two places that must agree: the calibration
 * solver deciding whether an evdev range fit is safe to compute, and the
 * backends deciding whether a stored range fit is safe to program
 * (prestonbrown/helixscreen#1394).
 *
 * Asks the live backend rather than `/display/rotate`, because the config key
 * is the REQUEST and the two disagree in both directions:
 *
 *  - `--rotate` / HELIX_DISPLAY_ROTATION with no `/display/rotate` key: the
 *    display is rotated and the key reads 0.
 *  - A DRM→fbdev rotation fallback that fails (DSI/EGL), or any rotation
 *    asked for on SDL: the key is non-zero and the display is NOT rotated -
 *    DisplayManager logs "Continuing without rotation" and leaves it at 0.
 *
 * A gate reading the key gets one of those wrong; two gates reading
 * different sources disagree with each other.
 *
 * Ordering: DisplayManager::init() applies every startup rotation (CLI, env,
 * config, and a first-boot kernel panel_orientation) before it creates the
 * input devices, so a backend asking this from create_input_pointer() already
 * sees the final state. The interactive first-boot probe is the one path that
 * rotates after input exists; it goes through the backend too, so this answer
 * stays current, but a gate already evaluated in create_input_pointer() does
 * not re-run.
 *
 * @param disp Display to query, or nullptr for the default display
 * @return 0, 90, 180 or 270; 0 when there is no display
 */
inline int display_rotation_degrees(lv_display_t* disp = nullptr) {
    if (const DisplayBackend* backend = DisplayBackend::active()) {
        return backend->applied_rotation_degrees(disp);
    }
    return DisplayBackend::lvgl_rotation_degrees(disp);
}

/**
 * @brief Is the display actually rotated right now?
 *
 * @see display_rotation_degrees() for why this asks the backend, not the
 *      config key
 * @param disp Display to query, or nullptr for the default display
 * @return true when the display is at 90, 180 or 270 degrees
 */
inline bool display_is_rotated(lv_display_t* disp = nullptr) {
    return display_rotation_degrees(disp) != 0;
}

// ============================================================================
// Backend-Specific Headers (conditionally included)
// ============================================================================

// These are only available when the corresponding backend is compiled in.
// Check with #ifdef HELIX_DISPLAY_SDL etc.

#ifdef HELIX_DISPLAY_SDL
#include "display_backend_sdl.h"
#endif

#ifdef HELIX_DISPLAY_FBDEV
#include "display_backend_fbdev.h"
#endif

#ifdef HELIX_DISPLAY_DRM
#include "display_backend_drm.h"
#endif
