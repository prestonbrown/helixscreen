// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pre_print_option.h"

#include <map>
#include <string>
#include <vector>

// Forward declarations for auto_detect_and_save()
namespace helix {
class Config;
}
namespace helix {
class PrinterDiscovery;
}

/**
 * @brief Printer auto-detection result with confidence and reasoning
 */
struct PrinterDetectionResult {
    std::string type_name; ///< Printer type name (e.g., "FlashForge AD5M Pro", "Voron 2.4")
    int confidence;      ///< Confidence score 0-100 (≥70 = high confidence, <70 = low confidence)
    std::string reason;  ///< Human-readable detection reasoning
    int match_count = 1; ///< Number of matching heuristics (for combined scoring)
    int best_single_confidence = 0; ///< Highest individual heuristic confidence (tiebreaker)
    std::string
        preset; ///< Platform preset name from DB (e.g., "k1", "snapmaker_u1"), empty if none
    std::string
        runner_up_type_name; ///< 2nd-place candidate model name (for margin gating), empty if none
    int runner_up_confidence = 0; ///< 2nd-place candidate confidence (0 if none)

    /**
     * @brief Check if detection succeeded
     * @return true if confidence > 0, false otherwise
     */
    bool detected() const {
        return confidence > 0;
    }
};

/**
 * @brief Build volume dimensions from bed_mesh configuration
 */
struct BuildVolume {
    float x_min = 0.0f;
    float x_max = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
    float z_max = 0.0f; ///< Maximum Z height (if available)
};

/**
 * @brief Printer hardware discovery data
 *
 * Aggregates hardware information from Moonraker for detection analysis.
 */
struct PrinterHardwareData {
    std::vector<std::string> heaters{};         ///< Controllable heaters (extruders, bed, etc.)
    std::vector<std::string> sensors{};         ///< Read-only temperature sensors
    std::vector<std::string> fans{};            ///< All fan types
    std::vector<std::string> leds{};            ///< LED outputs
    std::string hostname{};                     ///< Printer hostname from printer.info
    std::vector<std::string> printer_objects{}; ///< Full list of Klipper objects from objects/list
    std::vector<std::string> steppers{}; ///< Stepper motor names (stepper_x, stepper_z, etc.)
    std::string kinematics{};            ///< Kinematics type (corexy, cartesian, delta, etc.)
    std::string mcu{};                   ///< Primary MCU chip type (e.g., "stm32h723xx", "rp2040")
    std::vector<std::string> mcu_list{}; ///< All MCU chips (primary + secondary, CAN toolheads)
    BuildVolume build_volume{};          ///< Build volume dimensions from bed_mesh
    std::string cpu_arch{};              ///< Host CPU architecture (e.g., "ARMv7", "MIPS")
};

/**
 * @brief Printer auto-detection using hardware fingerprints
 *
 * Data-driven printer detection system that loads heuristics from JSON database.
 * Analyzes hardware discovery data to identify printer models based on
 * distinctive patterns found in real printers (FlashForge AD5M Pro, Voron V2, etc.).
 *
 * This class is completely independent of UI code and printer type lists.
 * It returns printer type names as strings, which the caller can map to their
 * own data structures (e.g., UI dropdowns, config values).
 *
 * Detection heuristics are defined in config/printer_database.json, allowing
 * new printer types to be added without recompilation.
 *
 * **Contract**: Returned type_name strings are loaded from printer_database.json.
 * The detector dynamically builds list options from the database, making it
 * fully data-driven with no hardcoded printer lists.
 */
class PrinterDetector {
  public:
    /**
     * @brief Detect printer type from hardware data
     *
     * Loads heuristics from config/printer_database.json and executes pattern matching
     * rules to identify printer model. Supports multiple heuristic types:
     * - sensor_match: Pattern matching on sensors array
     * - fan_match: Pattern matching on fans array
     * - hostname_match: Pattern matching on printer hostname
     * - fan_combo: Multiple fan patterns must all be present
     *
     * Returns the printer with highest confidence match, or empty result if
     * no distinctive fingerprints detected.
     *
     * @param hardware Hardware discovery data from Moonraker
     * @return Detection result with type name, confidence, and reasoning
     */
    static PrinterDetectionResult detect(const PrinterHardwareData& hardware);

    /**
     * @brief Get image filename for a printer type
     *
     * Looks up the image field from the printer database JSON.
     * Returns just the filename (e.g., "voron-v2.png"), not the full path.
     *
     * @param printer_name Printer name (e.g., "Voron 2.4", "FlashForge Adventurer 5M")
     * @return Image filename if found, empty string if not found
     */
    static std::string get_image_for_printer(const std::string& printer_name);

    /**
     * @brief Get image filename for a printer by ID
     *
     * Looks up the image field from the printer database JSON using the printer ID.
     * Returns just the filename (e.g., "voron-v2.png"), not the full path.
     *
     * @param printer_id Printer ID (e.g., "voron_2_4", "flashforge_adventurer_5m")
     * @return Image filename if found, empty string if not found
     */
    static std::string get_image_for_printer_id(const std::string& printer_id);

    /**
     * @brief Get console filter pattern preset for a printer
     *
     * Looks up the `console_filter_patterns` array from the database entry for
     * the given printer name (matched against the entry's `name` field, then
     * fall-through to `id`). Each pattern is a serialized
     * `<type>:<text>` string (`prefix:`, `substring:`, `regex:`) consumed by
     * `helix::ui::ConsoleFilterEngine`. Returns an empty vector when the
     * printer is not in the database or has no preset.
     *
     * @param printer_name Printer name or ID
     * @return Pattern specs in load order; empty if no preset.
     */
    static std::vector<std::string> get_console_filter_patterns(const std::string& printer_name);

    /**
     * @brief Get printer display name for a platform preset
     *
     * Looks up the database entry whose `preset` field matches and returns its
     * `name` (e.g. preset "ad5x" → "FlashForge Adventurer 5X"). Used to populate
     * the per-printer `type` field when a platform preset is applied from the
     * installer, so the home panel can resolve the correct printer image.
     *
     * @param preset_name Platform preset name (e.g., "ad5x", "k1", "cc1")
     * @return Matching printer name, empty string if no entry matches
     */
    static std::string get_name_for_preset(const std::string& preset_name);

    /**
     * @brief Look up the preset name for a printer display name
     *
     * Reverse of @ref get_name_for_preset. Used by the wizard's manual
     * printer-type selection step to resolve the user's pick to a preset
     * file before applying it.
     *
     * @param printer_name Printer display name (e.g. "FlashForge Adventurer 5M Pro")
     * @return Matching preset name, empty string if no entry matches
     */
    static std::string get_preset_for_name(const std::string& printer_name);

    /**
     * @brief Apply a preset to the active printer with firmware-variant detection
     *
     * Applies the given base preset, but first checks the discovery's printer-object
     * list for firmware-variant signatures (e.g. `fan_generic fanM106` → `_zmod`
     * variant). When a variant exists for the firmware fingerprint, applies that
     * variant instead and writes its name as the active preset.
     *
     * Used by both the auto-detect path and the wizard manual-pick path so they
     * stay in sync on variant resolution.
     *
     * @param config Active config (must be non-null)
     * @param preset Base preset name (e.g. "ad5m_pro")
     * @param discovery Printer discovery used for variant detection
     * @return Preset name actually applied (may be a variant), empty if neither
     *         the base nor a variant could be applied
     */
    static std::string apply_preset_with_variants(helix::Config* config, const std::string& preset,
                                                  const helix::PrinterDiscovery& discovery);

    /**
     * @brief Build list options string from database
     *
     * Dynamically builds a newline-separated string of printer names suitable
     * for LVGL list widget. Only includes entries with `show_in_list: true`
     * (defaults to true if field is missing). Always appends "Custom/Other"
     * and "Unknown" at the end.
     *
     * The string is cached after first build for performance.
     *
     * @return Newline-separated printer names for lv_list_set_options()
     */
    static const std::string& get_list_options();

    /**
     * @brief Get list of printer names from database
     *
     * Returns a vector of all printer names that should appear in the list.
     * Useful for index lookups and iteration.
     *
     * @return Vector of printer names (includes Custom/Other and Unknown)
     */
    static const std::vector<std::string>& get_list_names();

    /**
     * @brief Find index of a printer name in the list
     *
     * @param printer_name Name to search for
     * @return Index if found, or index of "Unknown" if not found
     */
    static int find_list_index(const std::string& printer_name);

    /**
     * @brief Get printer name at list index
     *
     * @param index Roller index (0-based)
     * @return Printer name, or "Unknown" if index out of bounds
     */
    static std::string get_list_name_at(int index);

    /**
     * @brief Get the index of "Unknown" in the list
     *
     * @return Index of the Unknown entry (last entry)
     */
    static int get_unknown_list_index();

    // =========================================================================
    // Kinematics-Filtered List API
    // =========================================================================

    /**
     * @brief Get list options filtered by kinematics type
     *
     * @param kinematics Kinematics filter (e.g., "delta", "corexy"). Empty = unfiltered.
     * @return Newline-separated printer names matching the kinematics
     */
    static const std::string& get_list_options(const std::string& kinematics);

    /**
     * @brief Get list names filtered by kinematics type
     *
     * @param kinematics Kinematics filter. Empty = unfiltered.
     * @return Vector of printer names matching the kinematics
     */
    static const std::vector<std::string>& get_list_names(const std::string& kinematics);

    /**
     * @brief Find index of a printer name in the filtered list
     *
     * @param printer_name Name to search for
     * @param kinematics Kinematics filter. Empty = unfiltered.
     * @return Index if found, or index of "Unknown" if not found
     */
    static int find_list_index(const std::string& printer_name, const std::string& kinematics);

    /**
     * @brief Get printer name at index in the filtered list
     *
     * @param index List index (0-based)
     * @param kinematics Kinematics filter. Empty = unfiltered.
     * @return Printer name, or "Unknown" if index out of bounds
     */
    static std::string get_list_name_at(int index, const std::string& kinematics);

    /**
     * @brief Get the index of "Unknown" in the filtered list
     *
     * @param kinematics Kinematics filter. Empty = unfiltered.
     * @return Index of the Unknown entry (last entry)
     */
    static int get_unknown_list_index(const std::string& kinematics);

    /**
     * @brief Get the pre-print option set for a printer
     *
     * Looks up the `pre_print_options` field from the printer database JSON for
     * the specified printer and returns the parsed `PrePrintOptionSet`. This is
     * the printer-agnostic abstraction used by the print-detail panel (Phase 2
     * of the pre-print-options-framework refactor).
     *
     * @param printer_name Printer name (e.g., "FlashForge Adventurer 5M Pro")
     * @return Option set, or empty set if not found
     */
    static PrePrintOptionSet get_pre_print_option_set(const std::string& printer_name);

    /**
     * @brief Get Z-offset calibration strategy for a printer
     *
     * Looks up the z_offset_calibration_strategy field from the printer database JSON.
     * Returns empty string if not specified (caller should auto-detect).
     *
     * @param printer_name Printer name (e.g., "FlashForge Adventurer 5M Pro")
     * @return Strategy string ("probe_calibrate", "firmware_managed", "endstop"), or empty string
     */
    static std::string get_z_offset_calibration_strategy(const std::string& printer_name);

    /// Look up probe type override from the printer database.
    /// @return Probe type string (e.g., "prtouch_v2"), or empty if not specified
    static std::string get_probe_type(const std::string& printer_name);

    /**
     * @brief Printer-specific bed-mesh calibration gcode template
     *
     * Some printers need pre-probe housekeeping that helixscreen can't express
     * with a single macro slot — notably the Elegoo Centauri Carbon whose
     * mainline-Klipper `[load_cell_probe]` requires `LOAD_CELL_SAVE_TARE` and
     * a nozzle-wipe wrapper before `BED_MESH_CALIBRATE`. The database entry
     * may supply a multi-line template under `calibration.bed_mesh_gcode`;
     * the Bed Mesh panel substitutes `{profile}` with the temporary profile
     * name and sends the whole block as a single gcode script.
     *
     * @return Template with `{profile}` placeholder, or empty string if the
     *         printer has no override (callers fall back to `StandardMacros`).
     */
    static std::string get_bed_mesh_calibrate_gcode(const std::string& printer_name);

    /**
     * @brief Belt-span offset in mm for a printer model, or a negative value
     *
     * Free belt span at gantry position Y is about `Y + offset`. Geometry, so
     * it is per-model, and it is measured rather than derived. A negative
     * return means this model has no measured value: the caller must fall back
     * to span-independent A-vs-B matching and suppress the absolute frequency
     * target rather than guessing.
     */
    static double get_belt_span_offset_mm(const std::string& printer_name);

    /**
     * @brief Get print start profile name for a printer
     *
     * Looks up the print_start_profile field from the printer database JSON
     * for the specified printer. This determines which JSON profile to load
     * for PRINT_START phase detection.
     *
     * @param printer_name Printer name (e.g., "FlashForge Adventurer 5M Pro")
     * @return Profile name (e.g., "forge_x"), or empty string if not specified
     */
    static std::string get_print_start_profile(const std::string& printer_name);

    /**
     * @brief First-print pre-print phase durations for a printer
     *
     * PreprintPredictor's generic defaults assume a fully-featured macro flow
     * (homing, QGL, Z-tilt, bed mesh, cleaning, purging) and sum to ~260s,
     * which is wildly pessimistic for printers whose slicer start-gcode only
     * heats and homes (e.g. Elegoo CC1 loads a stored bed-mesh profile and
     * has a near-empty PRINT_START).
     *
     * Database field `print_start_default_phases` is an object mapping phase
     * name → seconds, e.g. `{"HOMING": 25}`. When present it REPLACES the
     * generic defaults for the first-print ETA; history still wins once a
     * print has completed and the predictor has real timings.
     *
     * Phase name keys match the PrintStartPhase enum in printer_state.h:
     * HOMING, QGL, Z_TILT, BED_MESH, CLEANING, PURGING. Unknown names are
     * ignored (with a warning).
     *
     * @param printer_name Printer name (e.g., "Elegoo Centauri Carbon")
     * @return phase_enum_int → seconds; empty map falls back to generic defaults.
     */
    static std::map<int, int> get_print_start_default_phases(const std::string& printer_name);

    // =========================================================================
    // User Extensions API
    // =========================================================================

    /**
     * @brief Load status for debugging and settings UI
     */
    struct LoadStatus {
        bool loaded;                           ///< True if database loaded successfully
        int total_printers;                    ///< Total enabled printers
        int user_overrides;                    ///< Number of bundled printers overridden by user
        int user_additions;                    ///< Number of new printers added by user
        std::vector<std::string> loaded_files; ///< Files loaded (bundled + extensions)
        std::vector<std::string> load_errors;  ///< Non-fatal errors encountered
    };

    /**
     * @brief Reload printer database and extensions
     *
     * Clears all caches and reloads from disk. Useful for development/testing
     * after modifying extension files.
     */
    static void reload();

    /**
     * @brief Get load status for debugging/settings UI
     *
     * Returns information about what was loaded, including any errors
     * encountered in user extension files.
     *
     * @return LoadStatus with details about loaded files and errors
     */
    static LoadStatus get_load_status();

    /**
     * @brief Auto-detect printer type from discovery data
     *
     * Convenience wrapper that builds PrinterHardwareData from PrinterDiscovery
     * and runs detection. Use this instead of manually building hardware data.
     *
     * @param discovery Hardware discovery data from Moonraker
     * @return Detection result with type name, confidence, and reasoning
     */
    static PrinterDetectionResult auto_detect(const helix::PrinterDiscovery& discovery);

    /**
     * @brief Auto-detect printer type and save to config if not already set
     *
     * Called during Moonraker discovery completion. If printer.type is empty,
     * runs detection and saves the result to config. Also updates PrinterState
     * so the home panel gets the correct image and capabilities.
     *
     * @param discovery Hardware discovery data from Moonraker
     * @param config Config instance to check/save printer type
     * @return true if detection ran and found a match, false if skipped or no match
     */
    static bool auto_detect_and_save(const helix::PrinterDiscovery& discovery,
                                     helix::Config* config);

    /**
     * @brief Strip heuristics data from the database to reclaim memory
     *
     * Call after detection is complete. Removes heuristic arrays from all printer
     * entries while preserving kinematics info needed for filtered list building.
     * Safe to call multiple times (no-op after first). Use reload() to restore.
     */
    static void compact_database();

    /**
     * @brief Check if the configured printer type is a Voron variant
     *
     * Reads the printer type from config and does a case-insensitive check
     * for "voron". Used to select Stealthburner toolhead rendering in the
     * filament path canvas.
     *
     * @return true if printer type contains "Voron" (case-insensitive)
     */
    static bool is_voron_printer();

    /**
     * @brief Check if the detected printer is a PrintersForAnts (PFA) model
     *
     * Used to select AntHead toolhead rendering when toolhead style is Auto.
     *
     * @return true if printer type contains "PFA" (case-insensitive)
     */
    static bool is_pfa_printer();

    /**
     * @brief Get the native toolhead style for a printer from the database
     * @param printer_name Printer name (case-insensitive match)
     * @return Toolhead style string (e.g., "creality_k1") or empty if none
     */
    static std::string get_toolhead_style(const std::string& printer_name);

    /**
     * @brief Check if connected printer is a Creality K1 series
     * @return true if printer type contains both "creality" and "k1"
     */
    static bool is_creality_k1();

    /**
     * @brief Check if connected printer is a Creality K2 series
     * @return true if printer type contains both "creality" and "k2"
     */
    static bool is_creality_k2();

    /**
     * @brief Check if connected printer is a Creality Hi
     * @return true if printer type contains both "creality" and "hi"
     *
     * The Hi is a Cartesian bedslinger whose optional CFS uses the
     * non-prefixed BOX_* macro dialect (same family as the K1, not the K2
     * CR_BOX_* primitives). AmsBackendCfs keys its macro variant off this.
     */
    static bool is_creality_hi();

    /**
     * @brief Declared physical tightening direction for bed screws
     *
     * Printer database entries may set `"screws_tilt_direction": "cw"` or
     * `"ccw"` to declare which rotation, from the user's physical viewpoint,
     * tightens a bed screw on that printer. This is an explicit override —
     * independent of whatever Klipper's `screw_thread` is set to — that
     * describes the printer's own geometry.
     *
     * Klipper's own output direction (CW/CCW) is computed using its
     * configured `screw_thread`. When the DB override disagrees with
     * Klipper's default CW-M* semantics (value "ccw"), HelixScreen flips
     * CW↔CCW in the displayed direction so the UI matches the printer's
     * physical reality.
     *
     * Used to override vendors whose shipped `screw_thread` config disagrees
     * with the physical screw response (e.g. FlashForge Adventurer 5M family:
     * ships CW-M4 but the physical response requires CCW-M4 semantics).
     *
     * @return "cw", "ccw", or empty string if not declared (treat as "cw")
     */
    static std::string screws_tilt_direction_override();
};
