// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "bed_drying.h"
#include "lvgl/lvgl.h"
#include "maintenance_meters.h"
#include "mount_feed_sequence.h"
#include "persisted_setting.h"
#include "subject_managed_panel.h"
#include "toolhead_style.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;

/// Jog step mode selector — defined in ui_panel_motion.h. Declared here so the
/// jog-distance accessors can take it without pulling the motion panel's UI
/// machinery into every settings consumer.
enum class JogMode;

/** @brief Z movement style override (Auto=detect from kinematics, or force) */
enum class ZMovementStyle { AUTO = 0, BED_MOVES = 1, NOZZLE_MOVES = 2 };

/// Motion panel Jog-tab arrangement. Pad is the circular two-ring pad with a
/// Z column; Arrows and ArrowsHome are arrow keys with one shared step
/// selector (home buttons beside the arrows, or in the centre of each cross);
/// AxisRows gives each axis a row of minus/plus buttons at the jog mode's two
/// distances.
enum class MotionLayout { Pad = 0, Arrows = 1, ArrowsHome = 2, AxisRows = 3 };
inline constexpr int kMotionLayoutCount = 4;

/**
 * @brief Storage layer for the user-supplied console filter lists.
 *
 * The two layers are read together (the console applies their union) but are
 * written independently, so a pattern the user only ever wants muted on one
 * machine does not follow them to the next one.
 */
enum class ConsoleFilterScope {
    Global,  ///< /console/filter_user_* — in force on every printer
    Printer, ///< /printers/<active>/console/filter_user_* — active printer only
};

/// Range of the Bed-tab clearance setting, in whole mm; shared by the
/// persisted table and the settings keypad.
inline constexpr int kBedMapClearanceMinMm = 1;
inline constexpr int kBedMapClearanceMaxMm = 50;

/**
 * @brief Application settings manager with reactive UI binding
 *
 * Coordinates persistence (Config), reactive subjects (lv_subject_t), immediate
 * effects (theme changes, Moonraker commands), and user preferences.
 *
 * Domain-specific settings are delegated to specialized managers:
 * - DisplaySettingsManager: dark mode, theme, dim, sleep, brightness, animations, etc.
 * - SystemSettingsManager: language, update channel, telemetry
 * - InputSettingsManager: scroll throw, scroll limit
 * - AudioSettingsManager: sounds, volume, UI sounds, sound theme, completion alerts
 * - SafetySettingsManager: e-stop confirmation, cancel escalation
 *
 * SettingsManager retains ownership of:
 * - LED control (depends on MoonrakerClient)
 * - Z movement style (depends on PrinterState)
 * - External spool info (depends on AMS types)
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class SettingsManager {
  public:
    /**
     * @brief Get singleton instance
     * @return Reference to global SettingsManager
     */
    static SettingsManager& instance();

    /// The static PersistedSetting table this manager is built from.
    static settings::TableView persisted_table();

    // Prevent copying
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;

    /**
     * @brief Initialize LVGL subjects
     *
     * MUST be called BEFORE creating XML components that bind to settings subjects.
     * Loads initial values from Config and registers subjects with LVGL XML system.
     * Also initializes all domain-specific managers.
     */
    void init_subjects();

    /**
     * @brief Deinitialize LVGL subjects
     *
     * Must be called before lv_deinit() to properly disconnect observers.
     * Called by StaticSubjectRegistry during application shutdown.
     */
    void deinit_subjects();

    [[nodiscard]] bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

    /**
     * @brief Set Moonraker client reference for remote commands
     *
     * Required for LED control and other printer-dependent settings.
     * Call after MoonrakerClient is initialized.
     *
     * @param client Pointer to active MoonrakerClient (can be nullptr to disable)
     */
    void set_moonraker_client(IMoonrakerClient* client);

    // =========================================================================
    // Z MOVEMENT STYLE (owned by SettingsManager — PrinterState dependency)
    // =========================================================================

    /** @brief Get Z movement style override (Auto/Bed Moves/Nozzle Moves) */
    ZMovementStyle get_z_movement_style() const {
        return static_cast<ZMovementStyle>(std::clamp(settings_.get(Key::ZMovementStyle), 0, 2));
    }

    /** @brief Set Z movement style override and apply to printer state */
    void set_z_movement_style(ZMovementStyle style);

    // =========================================================================
    // BED DRYING (prestonbrown/helixscreen#1730)
    // =========================================================================

    /// Enclosure override: Auto (printer database, else a chamber heater),
    /// Enclosed (marks a DIY enclosure) or Open.
    helix::bed_drying::EnclosureStyle get_enclosure_style() const {
        return static_cast<helix::bed_drying::EnclosureStyle>(
            std::clamp(settings_.get(Key::EnclosureStyle), 0, 2));
    }
    void set_enclosure_style(helix::bed_drying::EnclosureStyle style);
    lv_subject_t* subject_enclosure_style() {
        return settings_.subject(Key::EnclosureStyle);
    }

    /// The persisted bed-drying run; `latched` false when there is none.
    helix::bed_drying::RunRecord get_bed_drying_record() const;
    /// Written and saved at once: the latch must reach disk before any heat.
    /// False when the save failed.
    [[nodiscard]] bool set_bed_drying_record(const helix::bed_drying::RunRecord& record);
    bool clear_bed_drying_record();

    // =========================================================================
    // MAINTENANCE METERS
    // =========================================================================

    /// The maintenance meters saved in the printer section @p printer_prefix (a
    /// Config::df() value); nullopt when none was ever saved there.
    std::optional<helix::maintenance::State>
    get_maintenance_record(const std::string& printer_prefix) const;
    /// Written and saved at once. False when the save failed.
    bool set_maintenance_record(const std::string& printer_prefix,
                                const helix::maintenance::State& state);

    // =========================================================================
    // CHAMBER ASSIGNMENT (owned by SettingsManager — sensor/heater override)
    // =========================================================================

    /** @brief Get chamber heater assignment ("auto", "none", or klipper name) */
    std::string get_chamber_heater_assignment() const;

    /** @brief Set chamber heater assignment and persist */
    void set_chamber_heater_assignment(const std::string& value);

    /** @brief Get chamber sensor assignment ("auto", "none", or klipper name) */
    std::string get_chamber_sensor_assignment() const;

    /** @brief Set chamber sensor assignment and persist */
    void set_chamber_sensor_assignment(const std::string& value);

    /** @brief Get the tool-changer feeder OPEN macro ("auto" = detected default) */
    std::string get_feeder_open_macro() const;

    /** @brief Set the tool-changer feeder OPEN macro and persist */
    void set_feeder_open_macro(const std::string& value);

    /** @brief Get the tool-changer feeder CLOSE macro ("auto" = detected default) */
    std::string get_feeder_close_macro() const;

    /** @brief Set the tool-changer feeder CLOSE macro and persist */
    void set_feeder_close_macro(const std::string& value);

    /** @brief How far a mount-to-feed load purges past the nozzle, in mm */
    int get_mount_feed_purge_mm() const;

    /** @brief Set the mount-to-feed purge length (clamped to 0..kMaxPurgeMm) and persist */
    void set_mount_feed_purge_mm(int mm);

    /** @brief Get the ACE bypass ON macro ("auto" = detected default) */
    std::string get_ace_bypass_on_macro() const;

    /** @brief Set the ACE bypass ON macro and persist */
    void set_ace_bypass_on_macro(const std::string& value);

    /** @brief Get the ACE bypass OFF macro ("auto" = detected default) */
    std::string get_ace_bypass_off_macro() const;

    /** @brief Set the ACE bypass OFF macro and persist */
    void set_ace_bypass_off_macro(const std::string& value);

    /** @brief Z movement style subject (integer: 0=Auto, 1=Bed Moves, 2=Nozzle Moves) */
    lv_subject_t* subject_z_movement_style() {
        return settings_.subject(Key::ZMovementStyle);
    }

    // =========================================================================
    // TOOLHEAD STYLE (owned by SettingsManager — appearance setting)
    // =========================================================================

    /** @brief Get toolhead rendering style */
    ToolheadStyle get_toolhead_style() const {
        return static_cast<ToolheadStyle>(std::clamp(settings_.get(Key::ToolheadStyle), 0, 7));
    }

    /** @brief Get effective toolhead style (resolves AUTO using printer detection) */
    ToolheadStyle get_effective_toolhead_style() const;

    /** @brief Set toolhead rendering style and persist */
    void set_toolhead_style(ToolheadStyle style) {
        settings_.set(Key::ToolheadStyle, static_cast<int>(style));
    }

    /** @brief Get dropdown options string */
    static std::string get_toolhead_style_options();

    /** @brief Convert toolhead style to dropdown index (native styles map to 0/Auto) */
    static int toolhead_style_to_dropdown_index(ToolheadStyle style);

    /** @brief Convert dropdown index to toolhead style enum value */
    static ToolheadStyle dropdown_index_to_toolhead_style(int index);

    /** @brief Toolhead style subject (integer: 0=Auto, 1=Stealthburner, 2=A4T, 3=AntHead,
     * 4=JabberWocky) */
    lv_subject_t* subject_toolhead_style() {
        return settings_.subject(Key::ToolheadStyle);
    }

    // =========================================================================
    // EXTRUDE/RETRACT SPEED (owned by SettingsManager — persisted)
    // =========================================================================

    /** @brief Get extrude/retract speed in mm/s (default 5, range 1-50) */
    int get_extrude_speed() const {
        return settings_.get(Key::ExtrudeSpeed);
    }

    /** @brief Set extrude/retract speed in mm/s (clamped 1-50, persisted) */
    void set_extrude_speed(int mm_per_sec) {
        settings_.set(Key::ExtrudeSpeed, mm_per_sec);
    }

    /** @brief Extrude speed subject (integer: mm/s) for UI binding */
    lv_subject_t* subject_extrude_speed() {
        return settings_.subject(Key::ExtrudeSpeed);
    }

    // =========================================================================
    // JOG FEEDRATES (owned by SettingsManager — persisted per-printer)
    // =========================================================================

    /** @brief Get XY jog feedrate in mm/min (default 6000, range 60-60000) */
    int get_jog_speed_xy() const {
        return settings_.get(Key::JogSpeedXy);
    }

    /** @brief Set XY jog feedrate in mm/min (clamped 60-60000, persisted) */
    void set_jog_speed_xy(int mm_per_min) {
        settings_.set(Key::JogSpeedXy, mm_per_min);
    }

    /** @brief Get Z jog feedrate in mm/min (default 600, range 60-60000) */
    int get_jog_speed_z() const {
        return settings_.get(Key::JogSpeedZ);
    }

    /** @brief Set Z jog feedrate in mm/min (clamped 60-60000, persisted) */
    void set_jog_speed_z(int mm_per_min) {
        settings_.set(Key::JogSpeedZ, mm_per_min);
    }

    /** @brief Get whether the motion readout shows actual (live) position
     *  (default false: commanded position) */
    bool get_motion_show_actual_position() const {
        return settings_.get_bool(Key::MotionShowActualPosition);
    }

    /** @brief Set whether the motion readout shows actual (live) position (persisted) */
    void set_motion_show_actual_position(bool show) {
        settings_.set(Key::MotionShowActualPosition, show);
    }

    /** @brief Motion coordinate source subject (integer: 0=commanded, 1=actual) */
    lv_subject_t* subject_motion_show_actual_position() {
        return settings_.subject(Key::MotionShowActualPosition);
    }

    /** @brief Height in mm below which a Bed-tab move lifts Z before it
     *  travels (default 5, range kBedMapClearanceMinMm-kBedMapClearanceMaxMm) */
    int get_bed_map_clearance_mm() const {
        return settings_.get(Key::BedMapClearance);
    }

    /** @brief Set the Bed-tab clearance height in mm (clamped, persisted) */
    void set_bed_map_clearance_mm(int mm) {
        settings_.set(Key::BedMapClearance, mm);
    }

    /** @brief Motion panel Jog-tab layout (default Pad) */
    MotionLayout get_motion_layout() const {
        return static_cast<MotionLayout>(settings_.get(Key::MotionLayout));
    }

    /** @brief Set the motion panel Jog-tab layout (clamped, persisted) */
    void set_motion_layout(MotionLayout layout) {
        settings_.set(Key::MotionLayout, static_cast<int>(layout));
    }

    /** @brief Motion layout subject (integer, MotionLayout values) */
    lv_subject_t* subject_motion_layout() {
        return settings_.subject(Key::MotionLayout);
    }

    // =========================================================================
    // JOG STEP DISTANCES (owned by SettingsManager — persisted per-printer)
    // =========================================================================

    /** @brief Get the jog step distance in mm for a mode and ring
     *  (defaults 0.1/1, 1/10, 10/50; clamped 0.01-200 on read and write) */
    float get_jog_distance(JogMode mode, bool outer) const;

    /** @brief Set the jog step distance in mm (clamped 0.01-200, persisted) */
    void set_jog_distance(JogMode mode, bool outer, float mm);

    /**
     * @brief Re-read every persisted setting and the jog distance cache from Config.
     *
     * init_subjects() is one-shot for the process, so a Config replaced under it
     * (printer switch, test reset) is picked up here without rebinding observers.
     * No-op before init_subjects().
     */
    void reload_from_config();

    /** @brief Restore all six jog distances to the shipped defaults (persisted) */
    void reset_jog_distances();

    // =========================================================================
    // LANE EJECT (owned by SettingsManager — persisted per-printer)
    // =========================================================================

    /** @brief Lane eject distance magnitude in mm; 0 (the default) = the backend's own */
    int get_ams_eject_distance() const {
        return settings_.get(Key::AmsEjectDistance);
    }

    /** @brief Set the lane eject distance magnitude in mm (clamped 0-2000, persisted) */
    void set_ams_eject_distance(int mm) {
        settings_.set(Key::AmsEjectDistance, mm);
    }

    /** @brief Lane eject distance subject (integer: mm) */
    lv_subject_t* subject_ams_eject_distance() {
        return settings_.subject(Key::AmsEjectDistance);
    }

    /** @brief Lane eject velocity in mm/s; 0 (the default) = the backend's own */
    int get_ams_eject_velocity() const {
        return settings_.get(Key::AmsEjectVelocity);
    }

    /** @brief Set the lane eject velocity in mm/s (clamped 0-300, persisted) */
    void set_ams_eject_velocity(int mm_per_sec) {
        settings_.set(Key::AmsEjectVelocity, mm_per_sec);
    }

    /** @brief Lane eject velocity subject (integer: mm/s) */
    lv_subject_t* subject_ams_eject_velocity() {
        return settings_.subject(Key::AmsEjectVelocity);
    }

    // =========================================================================
    // FILAMENT SETTINGS (owned by SettingsManager — AMS types dependency)
    // =========================================================================

    /**
     * @brief Get external spool info (bypass/direct spool)
     * @return SlotInfo with external spool data, or nullopt if not set
     */
    std::optional<SlotInfo> get_external_spool_info() const;

    /**
     * @brief Set external spool info (bypass/direct spool)
     * @param info SlotInfo with filament data (slot_index forced to -2)
     */
    void set_external_spool_info(const SlotInfo& info);

    /**
     * @brief Clear external spool info (back to unassigned)
     */
    void clear_external_spool_info();

    // =========================================================================
    // SUBJECT ACCESSORS (for XML binding) — owned subjects only
    // =========================================================================

    // =========================================================================
    // PRINTER SWITCHER VISIBILITY (owned by SettingsManager — appearance)
    // =========================================================================

    /** @brief Get whether the navbar printer switcher icon is shown */
    bool get_show_printer_switcher() const {
        return settings_.get_bool(Key::ShowPrinterSwitcher);
    }

    /** @brief Set whether the navbar printer switcher icon is shown */
    void set_show_printer_switcher(bool show) {
        settings_.set(Key::ShowPrinterSwitcher, show);
    }

    /** @brief Printer switcher visibility subject (integer: 0=hidden, 1=shown) */
    lv_subject_t* subject_show_printer_switcher() {
        return settings_.subject(Key::ShowPrinterSwitcher);
    }

    /**
     * @brief Death signal for the subjects this SettingsManager owns.
     *
     * Pass to observe_*() from anything that can outlive this object's
     * deinit_subjects(): that path frees every observer node without bumping
     * the ObserverGuard invalidation epoch, so a guard without the token
     * dereferences a freed observer on its next reset().
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    // =========================================================================
    // WIDGET LABELS (owned by SettingsManager — appearance)
    // =========================================================================

    /** @brief Get whether icon-only widget labels are shown on the home screen */
    bool get_show_widget_labels() const {
        return settings_.get_bool(Key::ShowWidgetLabels);
    }

    /** @brief Set whether icon-only widget labels are shown on the home screen */
    void set_show_widget_labels(bool show) {
        settings_.set(Key::ShowWidgetLabels, show);
    }

    /** @brief Widget label visibility subject (integer: 0=hidden, 1=shown) */
    lv_subject_t* subject_show_widget_labels() {
        return settings_.subject(Key::ShowWidgetLabels);
    }

    // =========================================================================
    // AUTO COLOR MAP (owned by SettingsManager — filament mapping)
    // =========================================================================

    /** @brief Get whether filament mapping should auto-match by color */
    bool get_auto_color_map() const {
        return settings_.get_bool(Key::AutoColorMap);
    }

    /** @brief Set whether filament mapping should auto-match by color */
    void set_auto_color_map(bool enabled) {
        settings_.set(Key::AutoColorMap, enabled);
    }

    /** @brief Auto color map subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_auto_color_map() {
        return settings_.subject(Key::AutoColorMap);
    }

    // =========================================================================
    // AFC UNLOAD AFTER PRINT (owned by SettingsManager — per-printer AMS behavior)
    // =========================================================================

    /**
     * @brief Whether the filament system unloads the toolhead when a print ends,
     * on systems where that is the user's end-of-print macros' choice (AFC).
     *
     * Some end macros retract filament out of the extruder, leaving the
     * toolhead empty by design, others leave it loaded. When enabled, the
     * pre-print runout warning is suppressed (an empty toolhead is expected,
     * not a fault). Default false.
     */
    bool get_unload_after_print() const {
        return settings_.get_bool(Key::UnloadAfterPrint);
    }

    /** @brief Set whether the toolhead is unloaded when a print ends */
    void set_unload_after_print(bool enabled) {
        settings_.set(Key::UnloadAfterPrint, enabled);
    }

    /** @brief Unload-after-print subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_unload_after_print() {
        return settings_.subject(Key::UnloadAfterPrint);
    }

    // =========================================================================
    // END-OF-PRINT UNLOAD PER TOOL (per-printer; for firmware that forgets it)
    // =========================================================================

    /**
     * @brief Which tools unload their filament when a print ends, bit N = tool N.
     *
     * For firmware that takes this as a per-print option and clears it when
     * each print ends: the choice is kept here and the filament backend sends
     * it ahead of every print. Default 0 (no tool unloads).
     */
    int get_end_of_print_unload_tools() const {
        return settings_.get(Key::EndOfPrintUnloadTools);
    }

    /** @brief Replace the whole end-of-print unload mask */
    void set_end_of_print_unload_tools(int mask) {
        settings_.set(Key::EndOfPrintUnloadTools, mask);
    }

    /** @brief Whether @p tool unloads when a print ends; false outside the mask's range */
    bool get_end_of_print_unload_tool(int tool) const {
        return tool >= 0 && tool < kEndOfPrintUnloadMaxTools &&
               (get_end_of_print_unload_tools() & (1 << tool)) != 0;
    }

    /** @brief Set one tool's bit, leaving the others as stored; ignores a tool out of range */
    void set_end_of_print_unload_tool(int tool, bool unload) {
        if (tool < 0 || tool >= kEndOfPrintUnloadMaxTools) {
            return;
        }
        const int mask = get_end_of_print_unload_tools();
        set_end_of_print_unload_tools(unload ? (mask | (1 << tool)) : (mask & ~(1 << tool)));
    }

    /** @brief End-of-print unload mask subject (integer bitmask) */
    lv_subject_t* subject_end_of_print_unload_tools() {
        return settings_.subject(Key::EndOfPrintUnloadTools);
    }

    /// Tools the end-of-print unload mask can name (its row's maximum is 255).
    static constexpr int kEndOfPrintUnloadMaxTools = 8;

    /**
     * @brief Whether to show the bypass spool even when bypass is disengaged.
     *
     * AFC publishes a virtual bypass whether or not the user has one wired, so
     * the bypass node was drawn permanently — and painted with whatever the
     * external spool slot held, which read as "a spool is on bypass" on machines
     * that have no bypass at all (#1229). The node is now hidden on AFC while
     * bypass is off; enable this to keep it visible anyway. Default false.
     */
    bool get_ams_always_show_bypass_spool() const {
        return settings_.get_bool(Key::AmsAlwaysShowBypassSpool);
    }

    /** @brief Set whether the bypass spool stays visible with bypass disengaged */
    void set_ams_always_show_bypass_spool(bool enabled) {
        settings_.set(Key::AmsAlwaysShowBypassSpool, enabled);
    }

    /** @brief Always-show-bypass-spool subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ams_always_show_bypass_spool() {
        return settings_.subject(Key::AmsAlwaysShowBypassSpool);
    }

    /**
     * @brief Keep Spoolman spool info on a slot the firmware reports as ejected.
     *
     * classify_binding() arms its eject verdict only on backends whose
     * firmware reports spool ids (AFC, Happy Hare): there, firmware id 0/null
     * means the spool was ejected. This setting decides whether the lane keeps
     * its declared identity anyway. Default true: retention is the designed
     * behavior, and disabling it starts an ejected lane fresh.
     * Per-printer setting.
     */
    bool get_ams_keep_spool_info_on_eject() const {
        return settings_.get_bool(Key::AmsKeepSpoolInfoOnEject);
    }

    /** @brief Set whether spool info survives a firmware-reported eject */
    void set_ams_keep_spool_info_on_eject(bool enabled) {
        settings_.set(Key::AmsKeepSpoolInfoOnEject, enabled);
    }

    /** @brief Keep-spool-info-on-eject subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ams_keep_spool_info_on_eject() {
        return settings_.subject(Key::AmsKeepSpoolInfoOnEject);
    }

    /**
     * @brief Link a lane to the Spoolman spool its tag names when the spool is seated.
     *
     * Read by AmsState's tag-link pass, which acts only on backends whose tags
     * can name a spool (AmsBackend::tags_name_spoolman_spools()). Default on.
     * Per-printer setting.
     */
    bool get_ams_tag_links_spoolman() const {
        return settings_.get_bool(Key::AmsTagLinksSpoolman);
    }

    /** @brief Set whether a tag naming a Spoolman spool links its lane */
    void set_ams_tag_links_spoolman(bool enabled) {
        settings_.set(Key::AmsTagLinksSpoolman, enabled);
    }

    /** @brief Tag-links-Spoolman subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ams_tag_links_spoolman() {
        return settings_.subject(Key::AmsTagLinksSpoolman);
    }

    /**
     * @brief Expose the bypass controls even though the firmware reports none.
     *
     * Distinct from get_ams_always_show_bypass_spool(), which only un-suppresses
     * a node we hide ourselves on AFC. This one contradicts the firmware: Happy
     * Hare defaults [mmu_machine] has_bypass to 0 for mmu_vendor "Other", which
     * is what a Qidi Box reports, so owners who do feed filament past the unit
     * get no bypass UI at all. Safe to honour because Happy Hare's own
     * select_bypass() never consults has_bypass() — MMU_SELECT_BYPASS deselects
     * the gear steppers and reports gate -2 regardless. Default false.
     */
    bool get_ams_force_bypass_controls() const {
        return settings_.get_bool(Key::AmsForceBypassControls);
    }

    /** @brief Set whether bypass controls appear despite a firmware "no bypass" */
    void set_ams_force_bypass_controls(bool enabled) {
        settings_.set(Key::AmsForceBypassControls, enabled);
    }

    /**
     * @brief Whether bypass was declared on a system with no firmware bypass
     *        command (per-printer, backend-owned).
     *
     * Persists the user's bypass toggle for backends whose firmware cannot
     * hold that state itself — today only the stock CFS dialect (its
     * BOX_ENABLE_CFS_PRINT stand-down persists in the box, but HelixScreen's
     * declaration is ours to remember across restarts). Not a user setting;
     * no subject — read once at backend start, written on the toggle.
     */
    bool get_bypass_declared() const;
    /** @brief Persist the bypass declaration (see get_bypass_declared) */
    void set_bypass_declared(bool declared);

    /** @brief Force-bypass-controls subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ams_force_bypass_controls() {
        return settings_.subject(Key::AmsForceBypassControls);
    }

    // =========================================================================
    // POST-OP COOLDOWN (owned by SettingsManager — per-printer filament behavior)
    // =========================================================================

    /**
     * @brief Get whether the nozzle cools down after a filament load/unload.
     *
     * When enabled (default), PostOpCooldownManager turns the extruder heater
     * off `filament/cooldown_delay_seconds` after an operation completes. Some
     * filament systems — AFC, for one — implement their own post-operation
     * cooldown, so users on those need to turn ours off to avoid two
     * independent timers fighting over the heater.
     */
    bool get_filament_auto_cooldown() const {
        return settings_.get_bool(Key::FilamentAutoCooldown);
    }

    /** @brief Set whether the nozzle cools down after a filament load/unload */
    void set_filament_auto_cooldown(bool enabled) {
        settings_.set(Key::FilamentAutoCooldown, enabled);
    }

    /** @brief Post-op cooldown subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_filament_auto_cooldown() {
        return settings_.subject(Key::FilamentAutoCooldown);
    }

    /**
     * @brief Open the slot editor when a person newly inserts filament.
     *
     * Default false. Per-printer setting.
     */
    bool get_filament_auto_open_editor() const {
        return settings_.get_bool(Key::FilamentAutoOpenEditor);
    }

    /** @brief Set whether the slot editor opens on a newly detected filament insert */
    void set_filament_auto_open_editor(bool enabled) {
        settings_.set(Key::FilamentAutoOpenEditor, enabled);
    }

    /** @brief Auto-open-editor subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_filament_auto_open_editor() {
        return settings_.subject(Key::FilamentAutoOpenEditor);
    }

    // =========================================================================
    // CONSOLE FILTERS (owned by SettingsManager — gcode console noise toggles)
    // =========================================================================

    /** @brief Get whether the temperature-report filter is enabled (default true) */
    bool get_console_filter_temps() const {
        return settings_.get_bool(Key::ConsoleFilterTemps);
    }
    /** @brief Set whether the temperature-report filter is enabled */
    void set_console_filter_temps(bool enabled) {
        settings_.set(Key::ConsoleFilterTemps, enabled);
    }
    /** @brief Subject (0/1) for temperature-report filter */
    lv_subject_t* subject_console_filter_temps() {
        return settings_.subject(Key::ConsoleFilterTemps);
    }

    /** @brief Get whether the firmware-noise filter is enabled (default true) */
    bool get_console_filter_firmware_noise() const {
        return settings_.get_bool(Key::ConsoleFilterFirmwareNoise);
    }
    /** @brief Set whether the firmware-noise filter is enabled */
    void set_console_filter_firmware_noise(bool enabled) {
        settings_.set(Key::ConsoleFilterFirmwareNoise, enabled);
    }
    /** @brief Subject (0/1) for firmware-noise filter */
    lv_subject_t* subject_console_filter_firmware_noise() {
        return settings_.subject(Key::ConsoleFilterFirmwareNoise);
    }

    /**
     * @brief Get every user-supplied extra pattern that applies to the active
     *        printer's preset — the union of the global layer and the active
     *        printer's own layer, global entries first, duplicates collapsed.
     *        Each entry is a `<type>:<text>` spec (`prefix:`, `substring:`, `regex:`).
     */
    std::vector<std::string> get_console_filter_user_add() const;

    /**
     * @brief Get every user-supplied pattern to drop from the active printer's
     *        preset — the union of the global and per-printer layers, global
     *        entries first, duplicates collapsed. Each entry must match a preset
     *        spec verbatim to take effect.
     */
    std::vector<std::string> get_console_filter_user_remove() const;

    /** @brief Read one storage layer of the additive user patterns, unmerged. */
    std::vector<std::string> get_console_filter_user_add(ConsoleFilterScope scope) const;
    /** @brief Read one storage layer of the suppress-from-preset patterns, unmerged. */
    std::vector<std::string> get_console_filter_user_remove(ConsoleFilterScope scope) const;

    /**
     * @brief Replace the additive user patterns in one layer. Persists immediately.
     *        The other layer is left as it is; the console sees both.
     */
    void set_console_filter_user_add(const std::vector<std::string>& patterns,
                                     ConsoleFilterScope scope = ConsoleFilterScope::Global);
    /**
     * @brief Replace the suppress-from-preset patterns in one layer. Persists
     *        immediately. The other layer is left as it is; the console sees both.
     */
    void set_console_filter_user_remove(const std::vector<std::string>& patterns,
                                        ConsoleFilterScope scope = ConsoleFilterScope::Global);

    // =========================================================================
    // MACRO PANEL (owned by SettingsManager — per-printer hidden macro set)
    // =========================================================================

    /**
     * @brief Get the set of macro names the user has hidden from the macro panel
     *        on the active printer. Empty if never configured or malformed.
     */
    std::vector<std::string> get_hidden_macros() const;

    /** @brief Replace the hidden-macro set for the active printer. Persists immediately. */
    void set_hidden_macros(const std::vector<std::string>& names);

    /**
     * @brief Whether the hidden-macro key has ever been written for the active
     *        printer. Lets callers distinguish "never configured" (seed
     *        defaults) from "configured to an empty set" (user unhid everything).
     */
    bool hidden_macros_key_exists() const;

    // =========================================================================
    // SPAGHETTI DETECTION (owned by SettingsManager — master toggle + per-source policy)
    // =========================================================================

    /** @brief Get whether spaghetti detection is globally enabled (master toggle) */
    bool get_detection_enabled() const {
        return settings_.get_bool(Key::DetectionEnabled);
    }

    /** @brief Set spaghetti detection master toggle and persist */
    void set_detection_enabled(bool enabled) {
        settings_.set(Key::DetectionEnabled, enabled);
    }

    /** @brief Detection enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_detection_enabled() {
        return settings_.subject(Key::DetectionEnabled);
    }

    /** @brief Get whether a detection pauses the print (off = warn only) */
    bool get_detection_pause_on_detect() const {
        return settings_.get_bool(Key::DetectionPauseOnDetect);
    }

    /** @brief Set whether a detection pauses the print and persist */
    void set_detection_pause_on_detect(bool pause) {
        settings_.set(Key::DetectionPauseOnDetect, pause);
    }

    /** @brief Detection pause subject (integer: 0=warn only, 1=pause) */
    lv_subject_t* subject_detection_pause_on_detect() {
        return settings_.subject(Key::DetectionPauseOnDetect);
    }

    /**
     * @brief Whether the one-time seed from the printer's stored detection
     *        preference has run. False until a capable source's preference
     *        has been copied into the settings.
     */
    bool is_detection_seeded() const;

    /** @brief Mark the detection preference seed as done (one-time) */
    void mark_detection_seeded();

    /**
     * @brief Get per-source policy for the Snapmaker U1 built-in detector
     *        0=Off, 1=NotifyOnly, 2=DeferToSource (default)
     */
    int get_detection_policy_u1() const {
        return settings_.get(Key::DetectionPolicyU1);
    }

    /** @brief Set per-source policy for the Snapmaker U1 built-in detector (clamped 0-2) */
    void set_detection_policy_u1(int policy) {
        settings_.set(Key::DetectionPolicyU1, policy);
    }

    /** @brief Detection policy subject for U1 (integer: 0=Off, 1=NotifyOnly, 2=DeferToSource) */
    lv_subject_t* subject_detection_policy_u1() {
        return settings_.subject(Key::DetectionPolicyU1);
    }

    // =========================================================================
    // Z-OFFSET SAVE (per-printer; the decision lives in helix::zoffset)
    // =========================================================================

    /// Whether the app offers to persist the Z offset on this printer, as a
    /// helix::zoffset::SaveMode: 0=Auto, 1=Show, 2=Hide. Read it through
    /// helix::zoffset::save_enabled(), which resolves Auto.
    int get_z_offset_save_mode() const {
        return settings_.get(Key::ZOffsetSaveMode);
    }
    void set_z_offset_save_mode(int mode) {
        settings_.set(Key::ZOffsetSaveMode, mode);
    }
    lv_subject_t* subject_z_offset_save_mode() {
        return settings_.subject(Key::ZOffsetSaveMode);
    }

    // =========================================================================
    // BELT NAMES (per-printer; the names live in helix::ui::belt_path_name)
    // =========================================================================

    /// Which physical belt each Belt Tension path is, as a
    /// helix::calibration::BeltLayout (0 = not set).
    int get_belt_layout() const {
        return settings_.get(Key::BeltLayout);
    }
    void set_belt_layout(int layout) {
        settings_.set(Key::BeltLayout, layout);
    }
    lv_subject_t* subject_belt_layout() {
        return settings_.subject(Key::BeltLayout);
    }

    // =========================================================================
    // BARCODE SCANNER (owned by SettingsManager — manual device selection)
    // =========================================================================

    /** @brief Get configured scanner vendor:product ID (empty = auto-detect) */
    std::string get_scanner_device_id() const;

    /** @brief Set scanner vendor:product ID (empty = clear, auto-detect) */
    void set_scanner_device_id(const std::string& vendor_product);

    /** @brief Get configured scanner device display name */
    std::string get_scanner_device_name() const;

    /** @brief Set configured scanner device display name */
    void set_scanner_device_name(const std::string& name);

    /** @brief Get configured BT scanner MAC address (empty = none) */
    std::string get_scanner_bt_address() const;

    /** @brief Set configured BT scanner MAC address (empty = clear) */
    void set_scanner_bt_address(const std::string& address);

    /** @brief Get configured scanner keymap layout
     *
     *  Scanners produce evdev keycodes according to their internal (hardware)
     *  keyboard layout — this is a physical property of the scanner and cannot
     *  be inferred from the app language. Returns one of:
     *  "qwerty" (default, US), "qwertz" (German), "azerty" (French).
     */
    std::string get_scanner_keymap() const;

    /** @brief Set configured scanner keymap layout
     *
     *  Accepts "qwerty", "qwertz", or "azerty". Unknown values are rejected
     *  and the stored setting is left unchanged.
     */
    void set_scanner_keymap(const std::string& keymap);

  private:
    SettingsManager();
    ~SettingsManager() = default;

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    enum class Key : uint8_t {
        ZMovementStyle,
        EnclosureStyle,
        ExtrudeSpeed,
        JogSpeedXy,
        JogSpeedZ,
        MotionShowActualPosition,
        BedMapClearance,
        MotionLayout,
        AmsEjectDistance,
        AmsEjectVelocity,
        ToolheadStyle,
        ShowPrinterSwitcher,
        ShowWidgetLabels,
        AutoColorMap,
        UnloadAfterPrint,
        EndOfPrintUnloadTools,
        AmsAlwaysShowBypassSpool,
        AmsKeepSpoolInfoOnEject,
        AmsTagLinksSpoolman,
        AmsForceBypassControls,
        FilamentAutoCooldown,
        FilamentAutoOpenEditor,
        ConsoleFilterTemps,
        ConsoleFilterFirmwareNoise,
        DetectionEnabled,
        DetectionPauseOnDetect,
        DetectionPolicyU1,
        ZOffsetSaveMode,
        BeltLayout,
        COUNT
    };
    settings::PersistedSettings<Key, static_cast<size_t>(Key::COUNT)> settings_;

    // Jog step distances in mm, [static_cast<int>(JogMode)][outer]. Cached
    // config values rather than subjects: read on every jog, not widget-bound.
    // Sized for the three JogMode values; static_assert in load_jog_distances().
    float jog_distances_[3][2]{};
    void load_jog_distances();

    // External references
    IMoonrakerClient* moonraker_client_ = nullptr;

    // Chamber assignment settings (plain strings, no LVGL subjects needed)
    std::string chamber_heater_assignment_{"auto"};
    std::string feeder_open_macro_{"auto"};
    std::string feeder_close_macro_{"auto"};
    int mount_feed_purge_mm_ = helix::mount_feed::kDefaultPurgeMm; ///< Loaded in init_subjects()
    std::string ace_bypass_on_macro_{"auto"};
    std::string ace_bypass_off_macro_{"auto"};
    std::string chamber_sensor_assignment_{"auto"};

    // Scanner device selection (plain strings, no LVGL subjects needed)
    std::string scanner_device_id_;        // "vendor:product" or empty
    std::string scanner_device_name_;      // display name for UI
    std::string scanner_bt_address_;       // BT scanner MAC address or empty
    std::string scanner_keymap_{"qwerty"}; // "qwerty" | "qwertz" | "azerty"

    // State
    bool subjects_initialized_ = false;
};

} // namespace helix
