// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "lvgl/lvgl.h"
#include "subject_managed_panel.h"

#include <optional>
#include <string>

namespace helix {
class MoonrakerClient;

/** @brief Z movement style override (Auto=detect from kinematics, or force) */
enum class ZMovementStyle { AUTO = 0, BED_MOVES = 1, NOZZLE_MOVES = 2 };

/** @brief Toolhead rendering style (Auto=detect from printer type, or force) */
enum class ToolheadStyle { AUTO = 0, DEFAULT = 1, STEALTHBURNER = 2, A4T = 3 };

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
    void set_moonraker_client(MoonrakerClient* client);

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

    /** @brief Get dropdown options string "Auto\nBed Moves\nNozzle Moves" */
    static const char* get_z_movement_style_options();

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

    /** @brief Toolhead style subject (integer: 0=Auto, 1=Default, 2=Stealthburner, 3=A4T) */
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

  private:
    SettingsManager();
    ~SettingsManager() = default;

    // Subject manager for RAII cleanup
    SubjectManager subjects_;

    // LVGL subjects — only those owned by SettingsManager
    lv_subject_t led_enabled_subject_;
    lv_subject_t z_movement_style_subject_;
    lv_subject_t extrude_speed_subject_;
    lv_subject_t toolhead_style_subject_;

    // External references
    MoonrakerClient* moonraker_client_ = nullptr;

    // State
    bool subjects_initialized_ = false;
};

} // namespace helix
