// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <optional>
#include <string>
#include <vector>

namespace helix {
class IMoonrakerClient;

/** @brief Z movement style override (Auto=detect from kinematics, or force) */
enum class ZMovementStyle { AUTO = 0, BED_MOVES = 1, NOZZLE_MOVES = 2 };

/** @brief Toolhead rendering style (Auto=detect from printer type, or force) */
enum class ToolheadStyle {
    AUTO = 0,
    DEFAULT = 1,
    A4T = 2,
    ANTHEAD = 3,
    JABBERWOCKY = 4,
    STEALTHBURNER = 5,
    CREALITY_K1 = 6,
    CREALITY_K2 = 7
};

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
    // PRINTER SETTINGS (owned by SettingsManager — MoonrakerClient dependency)
    // =========================================================================

    /**
     * @brief Get LED enabled state
     * @return true if LED is on
     */
    bool get_led_enabled() const;

    /**
     * @brief Set LED enabled state
     *
     * Updates subject, sends Moonraker command, and persists startup preference.
     * The LED state is saved as "LED on at start" preference.
     *
     * @param enabled true to turn on, false to turn off
     */
    void set_led_enabled(bool enabled);

    // =========================================================================
    // Z MOVEMENT STYLE (owned by SettingsManager — PrinterState dependency)
    // =========================================================================

    /** @brief Get Z movement style override (Auto/Bed Moves/Nozzle Moves) */
    ZMovementStyle get_z_movement_style() const;

    /** @brief Set Z movement style override and apply to printer state */
    void set_z_movement_style(ZMovementStyle style);

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

    /** @brief Z movement style subject (integer: 0=Auto, 1=Bed Moves, 2=Nozzle Moves) */
    lv_subject_t* subject_z_movement_style() {
        return &z_movement_style_subject_;
    }

    // =========================================================================
    // TOOLHEAD STYLE (owned by SettingsManager — appearance setting)
    // =========================================================================

    /** @brief Get toolhead rendering style */
    ToolheadStyle get_toolhead_style() const;

    /** @brief Get effective toolhead style (resolves AUTO using printer detection) */
    ToolheadStyle get_effective_toolhead_style() const;

    /** @brief Set toolhead rendering style and persist */
    void set_toolhead_style(ToolheadStyle style);

    /** @brief Get dropdown options string */
    static const char* get_toolhead_style_options();

    /** @brief Convert toolhead style to dropdown index (native styles map to 0/Auto) */
    static int toolhead_style_to_dropdown_index(ToolheadStyle style);

    /** @brief Convert dropdown index to toolhead style enum value */
    static ToolheadStyle dropdown_index_to_toolhead_style(int index);

    /** @brief Toolhead style subject (integer: 0=Auto, 1=Stealthburner, 2=A4T, 3=AntHead,
     * 4=JabberWocky) */
    lv_subject_t* subject_toolhead_style() {
        return &toolhead_style_subject_;
    }

    // =========================================================================
    // EXTRUDE/RETRACT SPEED (owned by SettingsManager — persisted)
    // =========================================================================

    /** @brief Get extrude/retract speed in mm/s (default 5, range 1-50) */
    int get_extrude_speed() const;

    /** @brief Set extrude/retract speed in mm/s (clamped 1-50, persisted) */
    void set_extrude_speed(int mm_per_sec);

    /** @brief Extrude speed subject (integer: mm/s) for UI binding */
    lv_subject_t* subject_extrude_speed() {
        return &extrude_speed_subject_;
    }

    // =========================================================================
    // QIDI BOX EJECT (owned by SettingsManager — persisted per-printer)
    // =========================================================================

    /** @brief Get QIDI Box eject distance magnitude in mm (default 878, range 100-2000) */
    int get_qidi_eject_distance() const;

    /** @brief Set QIDI Box eject distance magnitude in mm (clamped 100-2000, persisted) */
    void set_qidi_eject_distance(int mm);

    /** @brief QIDI eject distance subject (integer: mm) for UI binding */
    lv_subject_t* subject_qidi_eject_distance() {
        return &qidi_eject_distance_subject_;
    }

    /** @brief Get QIDI Box eject velocity in mm/s (default 100, range 10-300) */
    int get_qidi_eject_velocity() const;

    /** @brief Set QIDI Box eject velocity in mm/s (clamped 10-300, persisted) */
    void set_qidi_eject_velocity(int mm_per_sec);

    /** @brief QIDI eject velocity subject (integer: mm/s) for UI binding */
    lv_subject_t* subject_qidi_eject_velocity() {
        return &qidi_eject_velocity_subject_;
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

    /** @brief LED enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_led_enabled() {
        return &led_enabled_subject_;
    }

    // =========================================================================
    // PRINTER SWITCHER VISIBILITY (owned by SettingsManager — appearance)
    // =========================================================================

    /** @brief Get whether the navbar printer switcher icon is shown */
    bool get_show_printer_switcher() const;

    /** @brief Set whether the navbar printer switcher icon is shown */
    void set_show_printer_switcher(bool show);

    /** @brief Printer switcher visibility subject (integer: 0=hidden, 1=shown) */
    lv_subject_t* subject_show_printer_switcher() {
        return &show_printer_switcher_subject_;
    }

    // =========================================================================
    // WIDGET LABELS (owned by SettingsManager — appearance)
    // =========================================================================

    /** @brief Get whether icon-only widget labels are shown on the home screen */
    bool get_show_widget_labels() const;

    /** @brief Set whether icon-only widget labels are shown on the home screen */
    void set_show_widget_labels(bool show);

    /** @brief Widget label visibility subject (integer: 0=hidden, 1=shown) */
    lv_subject_t* subject_show_widget_labels() {
        return &show_widget_labels_subject_;
    }

    // =========================================================================
    // AUTO COLOR MAP (owned by SettingsManager — filament mapping)
    // =========================================================================

    /** @brief Get whether filament mapping should auto-match by color */
    bool get_auto_color_map() const;

    /** @brief Set whether filament mapping should auto-match by color */
    void set_auto_color_map(bool enabled);

    /** @brief Auto color map subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_auto_color_map() {
        return &auto_color_map_subject_;
    }

    // =========================================================================
    // AFC UNLOAD AFTER PRINT (owned by SettingsManager — per-printer AMS behavior)
    // =========================================================================

    /**
     * @brief Get whether AFC unloads filament from the toolhead after a print.
     *
     * AFC behavior depends on the user's end-of-print macros: some retract
     * filament out of the extruder, leaving the toolhead empty by design,
     * others leave it loaded. When enabled, the pre-print runout warning is
     * suppressed (an empty toolhead is expected, not a fault). Default false.
     */
    bool get_afc_unload_after_print() const;

    /** @brief Set whether AFC unloads filament from the toolhead after a print */
    void set_afc_unload_after_print(bool enabled);

    /** @brief AFC unload-after-print subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_afc_unload_after_print() {
        return &afc_unload_after_print_subject_;
    }

    /**
     * @brief Whether to show the bypass spool even when bypass is disengaged.
     *
     * AFC publishes a virtual bypass whether or not the user has one wired, so
     * the bypass node was drawn permanently — and painted with whatever the
     * external spool slot held, which read as "a spool is on bypass" on machines
     * that have no bypass at all (#1229). The node is now hidden on AFC while
     * bypass is off; enable this to keep it visible anyway. Default false.
     */
    bool get_ams_always_show_bypass_spool() const;

    /** @brief Set whether the bypass spool stays visible with bypass disengaged */
    void set_ams_always_show_bypass_spool(bool enabled);

    /** @brief Always-show-bypass-spool subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ams_always_show_bypass_spool() {
        return &ams_always_show_bypass_spool_subject_;
    }

    /**
     * @brief Keep Spoolman spool info on a slot the firmware reports as ejected.
     *
     * merge_override() arms its eject rule only on backends whose firmware
     * reports spool ids (AFC, Happy Hare): there, firmware id 0/null means
     * the spool was ejected. This setting decides whether the slot keeps its
     * Spoolman color/material metadata anyway. Default true — retention is
     * the designed behavior; disabling restores the pre-override strip.
     * Per-printer setting.
     */
    bool get_ams_keep_spool_info_on_eject() const;

    /** @brief Set whether spool info survives a firmware-reported eject */
    void set_ams_keep_spool_info_on_eject(bool enabled);

    /** @brief Keep-spool-info-on-eject subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_ams_keep_spool_info_on_eject() {
        return &ams_keep_spool_info_on_eject_subject_;
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
    bool get_ams_force_bypass_controls() const;

    /** @brief Set whether bypass controls appear despite a firmware "no bypass" */
    void set_ams_force_bypass_controls(bool enabled);

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
        return &ams_force_bypass_controls_subject_;
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
    bool get_filament_auto_cooldown() const;

    /** @brief Set whether the nozzle cools down after a filament load/unload */
    void set_filament_auto_cooldown(bool enabled);

    /** @brief Post-op cooldown subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_filament_auto_cooldown() {
        return &filament_auto_cooldown_subject_;
    }

    // =========================================================================
    // CONSOLE FILTERS (owned by SettingsManager — gcode console noise toggles)
    // =========================================================================

    /** @brief Get whether the temperature-report filter is enabled (default true) */
    bool get_console_filter_temps() const;
    /** @brief Set whether the temperature-report filter is enabled */
    void set_console_filter_temps(bool enabled);
    /** @brief Subject (0/1) for temperature-report filter */
    lv_subject_t* subject_console_filter_temps() {
        return &console_filter_temps_subject_;
    }

    /** @brief Get whether the firmware-noise filter is enabled (default true) */
    bool get_console_filter_firmware_noise() const;
    /** @brief Set whether the firmware-noise filter is enabled */
    void set_console_filter_firmware_noise(bool enabled);
    /** @brief Subject (0/1) for firmware-noise filter */
    lv_subject_t* subject_console_filter_firmware_noise() {
        return &console_filter_firmware_noise_subject_;
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
    bool get_detection_enabled() const;

    /** @brief Set spaghetti detection master toggle and persist */
    void set_detection_enabled(bool enabled);

    /** @brief Detection enabled subject (integer: 0=off, 1=on) */
    lv_subject_t* subject_detection_enabled() {
        return &detection_enabled_subject_;
    }

    /**
     * @brief Get per-source policy for the Snapmaker U1 built-in detector
     *        0=Off, 1=NotifyOnly, 2=DeferToSource (default)
     */
    int get_detection_policy_u1() const;

    /** @brief Set per-source policy for the Snapmaker U1 built-in detector (clamped 0-2) */
    void set_detection_policy_u1(int policy);

    /** @brief Detection policy subject for U1 (integer: 0=Off, 1=NotifyOnly, 2=DeferToSource) */
    lv_subject_t* subject_detection_policy_u1() {
        return &detection_policy_u1_subject_;
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

    // LVGL subjects — only those owned by SettingsManager
    lv_subject_t led_enabled_subject_;
    lv_subject_t z_movement_style_subject_;
    lv_subject_t extrude_speed_subject_;
    lv_subject_t qidi_eject_distance_subject_;
    lv_subject_t qidi_eject_velocity_subject_;
    lv_subject_t toolhead_style_subject_;
    lv_subject_t show_printer_switcher_subject_;
    lv_subject_t show_widget_labels_subject_;
    lv_subject_t auto_color_map_subject_;
    lv_subject_t afc_unload_after_print_subject_;
    lv_subject_t ams_always_show_bypass_spool_subject_;
    lv_subject_t ams_keep_spool_info_on_eject_subject_;
    lv_subject_t ams_force_bypass_controls_subject_;
    lv_subject_t filament_auto_cooldown_subject_;
    lv_subject_t console_filter_temps_subject_;
    lv_subject_t console_filter_firmware_noise_subject_;
    lv_subject_t detection_enabled_subject_;
    lv_subject_t detection_policy_u1_subject_;

    // External references
    IMoonrakerClient* moonraker_client_ = nullptr;

    // Chamber assignment settings (plain strings, no LVGL subjects needed)
    std::string chamber_heater_assignment_{"auto"};
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
