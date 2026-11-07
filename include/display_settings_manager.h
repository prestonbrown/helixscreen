// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "lvgl/lvgl.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"

#include <algorithm>
#include <memory>
#include <string>

namespace helix {

/** @brief Time display format (12-hour with AM/PM or 24-hour) */
enum class TimeFormat { HOUR_12 = 0, HOUR_24 = 1 };

/**
 * @brief Domain-specific manager for display/appearance settings
 *
 * Owns all display-related LVGL subjects and persistence:
 * - dark_mode (light/dark toggle)
 * - dark_mode_available (ephemeral, depends on theme)
 * - theme_preset (current theme index)
 * - display_dim (dim timeout in seconds)
 * - display_sleep (sleep timeout in seconds)
 * - brightness (0-100, clamped to 10-100)
 * - has_backlight (ephemeral, hardware detection)
 * - sleep_while_printing (allow sleep during prints)
 * - screensaver_print_progress (progress card on savers without their own print display)
 * - animations_enabled (UI animation toggle)
 * - bed_mesh_render_mode (Auto/3D/2D)
 * - gcode_render_mode (Auto/3D/2D/Thumbnail Only)
 * - time_format (12H/24H)
 * - printer_image (config-only, no subject)
 * - bed_mesh_show_zero_plane (config-only, no subject)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class DisplaySettingsManager {
  public:
    static DisplaySettingsManager& instance();

    /// The static PersistedSetting table this manager is built from.
    static settings::TableView persisted_table();

    // Non-copyable
    DisplaySettingsManager(const DisplaySettingsManager&) = delete;
    DisplaySettingsManager& operator=(const DisplaySettingsManager&) = delete;

    /** @brief Initialize LVGL subjects and load from Config */
    void init_subjects();

    /** @brief Deinitialize LVGL subjects (called by StaticSubjectRegistry) */
    void deinit_subjects();

    // =========================================================================
    // DARK MODE / THEME
    // =========================================================================

    /** @brief Get dark mode state */
    bool get_dark_mode() const;

    /** @brief Set dark mode state (updates subject + persists) */
    void set_dark_mode(bool enabled);

    /** @brief Check if current theme supports dark mode toggle */
    bool is_dark_mode_available() const;

    /** @brief Called when theme changes to update mode availability */
    void on_theme_changed();

    /** @brief Get current theme filename (without .json) */
    std::string get_theme_name() const;

    /** @brief Set theme by filename, marks restart pending */
    void set_theme_name(const std::string& name);

    /** @brief Get dropdown options string for discovered themes */
    std::string get_theme_options() const;

    /** @brief Get index of current theme in options list */
    int get_theme_index() const;

    /** @brief Set theme by dropdown index */
    void set_theme_by_index(int index);

    // =========================================================================
    // DISPLAY POWER / BRIGHTNESS
    // =========================================================================

    /** @brief Get display dim timeout in seconds (0 = disabled) */
    int get_display_dim_sec() const;

    /** @brief Set display dim timeout (updates subject + persists + notifies DisplayManager) */
    void set_display_dim_sec(int seconds);

    /** @brief Get display sleep timeout in seconds (0 = disabled) */
    int get_display_sleep_sec() const;

    /** @brief Set display sleep timeout (updates subject + persists) */
    void set_display_sleep_sec(int seconds);

    /** @brief Get display brightness (10-100) */
    int get_brightness() const {
        return settings_.get(Key::Brightness);
    }

    /** @brief The brightness to restore to the backlight: get_brightness() held to 10-100 */
    int user_brightness() const {
        return std::clamp(get_brightness(), 10, 100);
    }

    /**
     * @brief Apply brightness live WITHOUT persisting (clamped 10-100)
     *
     * Updates the subject and the backlight, and nothing else. Use this for the
     * per-tick handler of a slider drag: set_brightness() writes settings.json,
     * which fsyncs the file, fsyncs the directory and copies a rolling backup —
     * per drag tick that is a real stall on flash-backed hardware, and on some
     * platforms the backlight backend also forks a shell.
     *
     * Pair it with set_brightness() on release so the final value is durable.
     *
     * @return the clamped value actually applied
     */
    int preview_brightness(int percent);

    /** @brief Set display brightness (clamped 10-100, updates subject + hardware + persists) */
    void set_brightness(int percent);

    /** @brief Check if hardware backlight control is available */
    bool has_backlight_control() const;

    /** @brief Check if backlight supports continuous dimming (not binary on/off) */
    bool has_dimming_control() const;

    /**
     * @brief Whether a screensaver is enabled (type != OFF).
     *
     * Always false when HELIX_ENABLE_SCREENSAVER is undefined or 0, where the type is Off.
     */
    bool screensaver_enabled() const;

    /**
     * @brief Whether the Sleep>=Dim coupling must be enforced.
     *
     * The Display Sleep stage (power-off) must never precede the Screen Dim
     * stage. On dimming-capable devices that stage is a backlight dim; on
     * no-backlight devices an enabled screensaver IS the dim-stage visual, so
     * the coupling must hold there too (#1049). Returns true when the device can
     * dim OR a screensaver is enabled.
     */
    bool should_couple_sleep_to_dim() const;

    bool get_sleep_while_printing() const {
        return settings_.get_bool(Key::SleepWhilePrinting);
    }
    void set_sleep_while_printing(bool enabled) {
        settings_.set(Key::SleepWhilePrinting, enabled);
    }

    /// Whether savers with no print display of their own show the progress card for a running
    /// print. Paused and ended prints show it regardless.
    bool get_screensaver_print_progress() const {
        return settings_.get_bool(Key::ScreensaverPrintProgress);
    }
    void set_screensaver_print_progress(bool enabled) {
        settings_.set(Key::ScreensaverPrintProgress, enabled);
    }

    // =========================================================================
    // UI PREFERENCES
    // =========================================================================

    /** @brief Get animations enabled state */
    bool get_animations_enabled() const;

    /** @brief Set animations enabled state (updates subject + persists) */
    void set_animations_enabled(bool enabled);

    /** @brief Use Android system keyboard instead of built-in LVGL keyboard */
    bool get_use_system_keyboard() const {
        return settings_.get_bool(Key::UseSystemKeyboard);
    }
    void set_use_system_keyboard(bool enabled) {
        settings_.set(Key::UseSystemKeyboard, enabled);
    }

    /** @brief Hide the on-screen keyboard while a hardware keyboard is attached */
    bool get_hide_keyboard_with_hardware() const {
        return settings_.get_bool(Key::HideKeyboardWithHardware);
    }
    void set_hide_keyboard_with_hardware(bool enabled) {
        settings_.set(Key::HideKeyboardWithHardware, enabled);
    }

    /**
     * @brief Record whether the display backend opened a physical keyboard.
     *
     * DisplayManager calls this each time it creates its input devices, which
     * happens before init_subjects(), so the value is held here and seeds the
     * settings_hardware_keyboard_present subject. Only a backend that opened a
     * scanned keyboard device answers true: the SDL window keyboard is not one.
     */
    void set_hardware_keyboard_present(bool present);

    /** @brief Whether the display backend opened a physical keyboard */
    bool hardware_keyboard_present() const {
        return hardware_keyboard_present_;
    }

    /**
     * @brief Whether text entry goes to the hardware keyboard alone.
     *
     * True only while the preference is on AND a physical keyboard is attached;
     * the focused textarea still takes that keyboard's keys through the input
     * group, so nothing but the on-screen keyboard goes away.
     */
    bool soft_keyboard_suppressed() const;

    /** @brief Page-scroll buttons enabled (auto-injected gutter chevrons) */
    bool get_page_scroll_buttons() const {
        return settings_.get_bool(Key::PageScrollButtons);
    }
    void set_page_scroll_buttons(bool enabled) {
        settings_.set(Key::PageScrollButtons, enabled);
    }

    /** @brief Speed/flow readouts lead with mm/s and mm³/s instead of percent */
    bool get_speed_flow_physical_units() const {
        return settings_.get_bool(Key::SpeedFlowPhysicalUnits);
    }
    void set_speed_flow_physical_units(bool enabled) {
        settings_.set(Key::SpeedFlowPhysicalUnits, enabled);
    }

    /** @brief Print Status preview card shows the live camera instead of the preview */
    bool get_print_status_show_camera() const {
        return settings_.get_bool(Key::PrintStatusShowCamera);
    }
    void set_print_status_show_camera(bool enabled) {
        settings_.set(Key::PrintStatusShowCamera, enabled);
    }

    /** @brief Keep Android navigation bar onscreen (issue #908, Android only) */
    bool get_keep_navbar_visible() const {
        return settings_.get_bool(Key::KeepNavbarVisible);
    }

    /** @brief Set keep-navbar-visible preference (updates subject + persists + JNI push) */
    void set_keep_navbar_visible(bool enabled);

    /** @brief The saved Glass Effects choice; glass shows only where the gate also passes */
    bool get_glass_effects() const {
        return settings_.get_bool(Key::GlassEffects);
    }

    /** @brief Set the Glass Effects choice (updates subject + persists) */
    void set_glass_effects(bool enabled);

    /** @brief Record the glass gate's verdict for this run. Main thread; the gate only */
    void set_glass_available(bool available);

    bool get_glass_available() const;

    /**
     * @brief Get the UI scale setting as a percentage.
     *
     * DisplayMetrics::kScaleSettingAutomatic (0) means "follow the panel's
     * DPI"; anything else is an explicit percentage. Deliberately has no
     * subject: the scale is read once during display init and screens do not
     * re-tier at runtime, so a subject would advertise live reactivity that
     * does not exist. Application reads the same Config key directly, because
     * it needs the value before this manager is constructed.
     */
    int get_ui_scale_percent() const;

    /** @brief Set the UI scale setting (persists; takes effect on next start) */
    void set_ui_scale_percent(int percent);

    /** @brief Bed mesh render mode (0=Auto, 1=3D, 2=2D) */
    int get_bed_mesh_render_mode() const {
        return settings_.get(Key::BedMeshRenderMode);
    }
    void set_bed_mesh_render_mode(int mode) {
        settings_.set(Key::BedMeshRenderMode, mode);
    }

    /** @brief G-code render mode (0=Auto, 1=3D, 2=2D, 3=Thumbnail Only) */
    int get_gcode_render_mode() const {
        return settings_.get(Key::GcodeRenderMode);
    }

    /** @brief Set G-code render mode; an explicit pick also clears the GPU crash blocks */
    void set_gcode_render_mode(int mode);

    TimeFormat get_time_format() const {
        return static_cast<TimeFormat>(std::clamp(settings_.get(Key::TimeFormat), 0, 1));
    }
    void set_time_format(TimeFormat format) {
        settings_.set(Key::TimeFormat, static_cast<int>(format));
    }

    /// Whether the dropdown offers "System": the host's own zone. The ESP32 has
    /// none, so an unpicked zone there is UTC.
#ifdef ESP_PLATFORM
    static constexpr bool kSystemTimezoneAvailable = false;
#else
    static constexpr bool kSystemTimezoneAvailable = true;
#endif

    /** @brief Current timezone IANA ID (e.g., "America/New_York"), or "" while it follows the
     * system */
    std::string get_timezone() const;

    /** @brief Set timezone by IANA ID, applies via setenv/tzset, persists. "" selects System */
    void set_timezone(const std::string& iana_id);

    /** @brief Set timezone by dropdown index */
    void set_timezone_by_index(int index);

    /** @brief Get dropdown index for current timezone */
    int get_timezone_index() const;

    /** @brief Get newline-separated dropdown options string */
    static std::string get_timezone_options();

    /**
     * @brief Apply the persisted /display/timezone to the process
     *
     * An absent key follows the system zone where there is one, else UTC.
     * @return the dropdown index of the applied zone
     */
    static int apply_configured_timezone();

    /**
     * @brief The offered zone a browser-reported zone resolves to
     *
     * An offered IANA id matches itself; any other id takes the first offered
     * zone with the same 1 January and 1 July UTC offsets (minutes east).
     * @return the offered IANA id, or "" when none matches
     */
    static std::string match_timezone(const std::string& iana_id, int jan_minutes_east,
                                      int jul_minutes_east);

    /**
     * @brief Adopt a browser-reported zone unless the user has picked one
     *
     * Main thread only: it sets the timezone subject.
     */
    void adopt_timezone_if_unset(const std::string& iana_id, int jan_minutes_east,
                                 int jul_minutes_east);

    // =========================================================================
    // SCREEN ROTATION
    // =========================================================================
    //
    // /display/rotate is read exactly once, by DisplayManager::init(), and LVGL
    // screens neither resize nor re-rotate afterwards. Every setter here is
    // therefore restart-required: nothing on screen turns until the app is
    // relaunched.

    /**
     * @brief Get the saved screen rotation in degrees.
     *
     * Anything other than 90/180/270 in the config reads back as 0, matching
     * what the startup path will actually apply.
     */
    int get_display_rotation() const;

    /**
     * @brief Persist a screen rotation and pin the first-boot probe.
     *
     * Also writes /display/rotation_probed, so the interactive probe in
     * Application::run_rotation_probe_and_layout() can never re-run and
     * overwrite an explicit choice.
     *
     * @param degrees One of 0, 90, 180, 270. Anything else is rejected.
     * @return true when the applied rotation changed, i.e. a restart is needed.
     *         false for a rejected value or a re-selection of the current one.
     */
    bool set_display_rotation(int degrees);

    /** @brief Get dropdown index (0-3) for a rotation in degrees */
    static int rotation_degrees_to_index(int degrees);

    /** @brief Convert dropdown index (0-3) to rotation degrees */
    static int index_to_rotation_degrees(int index);

    /**
     * @brief Whether the running display backend honors /display/rotate.
     *
     * The SDL desktop backend renders in DIRECT mode and skips rotation
     * outright (see DisplayManager::init), so the control is inert there and
     * the row is hidden. HELIX_SHOW_ROTATION_SETTING reveals it again for
     * desktop UI work. (HELIX_FORCE_ROTATION_PROBE is not reused for this: it
     * launches the interactive probe, which takes over the screen.)
     */
    static bool rotation_setting_available();

    // =========================================================================
    // SCREENSAVER
    // =========================================================================

    /**
     * @brief Get screensaver type (a ScreensaverType value, include/screensaver_registry.h)
     *
     * Always Off (0) when HELIX_ENABLE_SCREENSAVER is undefined or 0.
     */
    int get_screensaver_type() const;

#if HELIX_ENABLE_SCREENSAVER
    /** @brief Set screensaver type (updates subject, persists its stable name) */
    void set_screensaver_type(int type);
#endif

    // =========================================================================
    // CONFIG-ONLY SETTINGS (no subjects)
    // =========================================================================

    /** @brief Get custom printer image ID (empty = auto-detect) */
    std::string get_printer_image() const;

    /** @brief Set custom printer image ID and persist. Empty = auto-detect. */
    void set_printer_image(const std::string& id);

    /** @brief Get bed mesh zero plane visibility */
    bool get_bed_mesh_show_zero_plane() const;

    // =========================================================================
    // DISPLAY DIM OPTIONS (for dropdown population)
    // =========================================================================

    /** @brief Get dropdown index for current dim seconds value */
    static int dim_seconds_to_index(int seconds);

    /** @brief Convert dropdown index to dim seconds */
    static int index_to_dim_seconds(int index);

    /// The form the dim-timeout row takes in Display settings. Without a dimmable
    /// backlight the same timeout starts the screensaver, so the row is offered
    /// under that name while one is enabled, and hidden when nothing would happen.
    /// should_couple_sleep_to_dim() holds exactly when this is not Hidden.
    enum class DimRowKind { Hidden = 0, ScreenDim = 1, ScreensaverAfter = 2 };
    static DimRowKind dim_row_kind(bool has_dimming, bool screensaver_enabled);

    // =========================================================================
    // DISPLAY SLEEP OPTIONS (for dropdown population)
    // =========================================================================

    /** @brief Get dropdown index for current sleep seconds value */
    static int sleep_seconds_to_index(int seconds);

    /** @brief Convert dropdown index to sleep seconds */
    static int index_to_sleep_seconds(int index);

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding)
    // =========================================================================

    /** @brief Dark mode subject (integer: 0=light, 1=dark) */
    lv_subject_t* subject_dark_mode() {
        return &dark_mode_subject_;
    }

    /** @brief Dark mode available subject (integer: 0=no toggle, 1=toggle enabled) */
    lv_subject_t* subject_dark_mode_available() {
        return &dark_mode_available_subject_;
    }

    /** @brief Theme preset subject (integer: preset index) */
    lv_subject_t* subject_theme_preset() {
        return &theme_preset_subject_;
    }

    /** @brief Display dim subject (integer: seconds, 0=disabled) */
    lv_subject_t* subject_display_dim() {
        return &display_dim_subject_;
    }

    /** @brief Display sleep subject (integer: seconds, 0=disabled) */
    lv_subject_t* subject_display_sleep() {
        return &display_sleep_subject_;
    }

    /** @brief Brightness subject (integer: 10-100 percent) */
    lv_subject_t* subject_brightness() {
        return settings_.subject(Key::Brightness);
    }

    /** @brief Has backlight control subject (integer: 0=no, 1=yes) */
    lv_subject_t* subject_has_backlight() {
        return &has_backlight_subject_;
    }

    /** @brief Has dimming control subject (integer: 0=binary only, 1=dimmable) */
    lv_subject_t* subject_has_dimming() {
        return &has_dimming_subject_;
    }

    /** @brief Sleep while printing subject (integer: 0=inhibit, 1=allow) */
    lv_subject_t* subject_sleep_while_printing() {
        return settings_.subject(Key::SleepWhilePrinting);
    }
    lv_subject_t* subject_screensaver_print_progress() {
        return settings_.subject(Key::ScreensaverPrintProgress);
    }

    /** @brief Animations enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_animations_enabled() {
        return &animations_enabled_subject_;
    }

    /**
     * @brief Resolve the default for the animations_enabled setting.
     *
     * Software-rotated displays (fbdev + non-zero rotation) repaint through a
     * per-frame CPU rotate that makes transition animations jerky, so the
     * default is forced off there regardless of platform tier (#986). Only a
     * default — an explicit user setting always wins (see init_subjects()).
     *
     * @param platform_supports_animations PlatformCapabilities tier result
     * @param software_rotated             DisplayManager::is_software_rotated()
     * @return the default value for animations_enabled
     */
    static bool animations_default(bool platform_supports_animations, bool software_rotated) {
        return software_rotated ? false : platform_supports_animations;
    }

    /** @brief System keyboard subject (integer: 0=built-in, 1=system) */
    lv_subject_t* subject_use_system_keyboard() {
        return settings_.subject(Key::UseSystemKeyboard);
    }

    /** @brief Page-scroll buttons subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_page_scroll_buttons() {
        return settings_.subject(Key::PageScrollButtons);
    }

    /** @brief Speed/flow readout units subject (integer: 0=percent, 1=mm/s + mm³/s) */
    lv_subject_t* subject_speed_flow_physical_units() {
        return settings_.subject(Key::SpeedFlowPhysicalUnits);
    }

    /** @brief The saved Glass Effects choice (integer: 0=flat, 1=glass) */
    lv_subject_t* subject_glass_effects() {
        return settings_.subject(Key::GlassEffects);
    }

    /** @brief Whether the glass gate passed on this board (integer: 0/1, set only by the gate) */
    lv_subject_t* subject_glass_available() {
        return &glass_available_subject_;
    }

    /** @brief Print Status preview content subject (integer: 0=preview, 1=camera) */
    lv_subject_t* subject_print_status_show_camera() {
        return settings_.subject(Key::PrintStatusShowCamera);
    }

    /** @brief Keep navbar visible subject (integer: 0=immersive, 1=always show) */
    lv_subject_t* subject_keep_navbar_visible() {
        return settings_.subject(Key::KeepNavbarVisible);
    }

    /** @brief Hide-with-hardware-keyboard subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_hide_keyboard_with_hardware() {
        return settings_.subject(Key::HideKeyboardWithHardware);
    }

    /** @brief Physical keyboard presence (integer: 0=none, 1=attached) */
    lv_subject_t* subject_hardware_keyboard_present() {
        return &hardware_keyboard_present_subject_;
    }

    /** @brief Android platform flag (integer: 0=not Android, 1=Android) */
    lv_subject_t* subject_is_android() {
        return &is_android_subject_;
    }

    /** @brief Rotation setting availability (integer: 0=backend ignores it, 1=honored) */
    lv_subject_t* subject_rotation_available() {
        return &rotation_available_subject_;
    }

    /** @brief Bed mesh render mode subject (integer: 0=auto, 1=3D, 2=2D) */
    lv_subject_t* subject_bed_mesh_render_mode() {
        return settings_.subject(Key::BedMeshRenderMode);
    }

    /** @brief G-code render mode subject (integer: 0=auto, 1=3D, 2=2D, 3=thumbnail only) */
    /**
     * @brief Death signal for the subjects this singleton owns.
     *
     * Panels that outlive a deinit_subjects() cycle and observe these settings
     * subjects — PrintStatusPanel watches subject_gcode_render_mode() to reapply
     * the viewer mode live — must pass this to observe_*(). Without it their
     * ObserverGuards keep pointers to observer nodes deinit_all() freed.
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    lv_subject_t* subject_gcode_render_mode() {
        return settings_.subject(Key::GcodeRenderMode);
    }

    /** @brief Time format subject (integer: 0=12H, 1=24H) */
    lv_subject_t* subject_time_format() {
        return settings_.subject(Key::TimeFormat);
    }

    /** @brief Timezone subject (integer: index into curated list) */
    lv_subject_t* subject_timezone() {
        return &timezone_subject_;
    }

    /**
     * @brief Screensaver type subject (a ScreensaverType value, include/screensaver_registry.h)
     *
     * Registered in every build, as Off without screensaver support, so XML that binds
     * `settings_screensaver_type` resolves and hides saver-only controls.
     */
    lv_subject_t* subject_screensaver_type() {
        return &screensaver_type_subject_;
    }

  private:
    DisplaySettingsManager();
    ~DisplaySettingsManager() = default;

    SubjectManager subjects_;

    /// Keeps the XML style engine's transition scale following
    /// animations_enabled_subject_. See init_subjects().
    ObserverGuard transition_scale_observer_;

    enum class Key : uint8_t {
        SleepWhilePrinting,
        Brightness,
        UseSystemKeyboard,
        HideKeyboardWithHardware,
        PageScrollButtons,
        SpeedFlowPhysicalUnits,
        PrintStatusShowCamera,
        KeepNavbarVisible,
        BedMeshRenderMode,
        GcodeRenderMode,
        TimeFormat,
        GlassEffects,
        ScreensaverPrintProgress,
        COUNT
    };
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;

    lv_subject_t dark_mode_subject_{};
    lv_subject_t dark_mode_available_subject_{};
    lv_subject_t theme_preset_subject_{};
    lv_subject_t display_dim_subject_{};
    lv_subject_t display_sleep_subject_{};
    lv_subject_t has_backlight_subject_{};
    lv_subject_t has_dimming_subject_{};
    lv_subject_t dim_row_kind_subject_{};
    lv_subject_t animations_enabled_subject_{};
    lv_subject_t hardware_keyboard_present_subject_{};
    lv_subject_t is_android_subject_{};
    lv_subject_t rotation_available_subject_{};
    lv_subject_t timezone_subject_{};
    lv_subject_t glass_available_subject_{};

    lv_subject_t screensaver_type_subject_{};
    lv_subject_t has_screensaver_subject_{};

    bool subjects_initialized_ = false;
    bool hardware_keyboard_present_ = false;
};

} // namespace helix
