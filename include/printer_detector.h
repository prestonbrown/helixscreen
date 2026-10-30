// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "belt_tension_types.h"
#include "plr_profile.h"
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
namespace helix::printer_images {
struct HardwareFacts;
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
    /// Best-scoring candidate naming a DIFFERENT machine than the winner.
    /// Detection answers "which printer is this", so candidates are told apart
    /// by the machine they picture: entries that picture the same one are two
    /// spellings of a single answer - a filament-system option, or a firmware
    /// variant of the printer the user owns - and a tie between those says
    /// nothing about how well the machine was identified. Empty when no other
    /// candidate scored.
    std::string runner_up_type_name;
    int runner_up_confidence = 0; ///< Its confidence (0 if none)
    int tied_count = 0;           ///< Candidates sharing the winner's confidence, winner included

    /// Scores before the 100 ceiling, for the winner and for that runner-up.
    /// The published confidence saturates, which flattens a real lead between
    /// two strong candidates into an apparent tie, so margin() is measured on
    /// these. The bonus inside them counts identifying matches only: a
    /// corroborating match that raised it would separate look-alikes by how
    /// much corroboration one entry happens to author. Zero on a result
    /// assembled by hand rather than returned by detect(), where the published
    /// confidence is the only score there is. A separator's weight lands here
    /// and nowhere else: it is separation between look-alikes, not confidence
    /// in the identification.
    int uncapped_confidence = 0;
    int runner_up_uncapped_confidence = 0;

    /// The printers a person has to choose between. The winner comes first.
    /// When ambiguous(), every other candidate picturing a different machine
    /// that the winner does not lead by PrinterDetector::kDetectMinMargin
    /// follows, one name per machine. Only the winner when the detection
    /// separated; empty when nothing was identified.
    std::vector<std::string> contenders;

    /**
     * @brief Check if detection succeeded
     * @return true if confidence > 0, false otherwise
     */
    bool detected() const {
        return confidence > 0;
    }

    /// How far the winner leads a candidate that scored @p rival_confidence
    /// published and @p rival_uncapped before the ceiling (0 when unknown).
    int lead_over(int rival_confidence, int rival_uncapped) const {
        const int winner = uncapped_confidence > 0 ? uncapped_confidence : confidence;
        const int rival = rival_uncapped > 0 ? rival_uncapped : rival_confidence;
        return winner - rival;
    }

    /// How far the winner leads the best candidate with a different outcome.
    int margin() const {
        return lead_over(runner_up_confidence, runner_up_uncapped_confidence);
    }

    /// A detection that named a printer without separating it from an
    /// alternative the user would notice. Defined below PrinterDetector, which
    /// owns the margin floor.
    bool ambiguous() const;
};

/**
 * @brief Build volume dimensions from the configfile
 *
 * The extents are [stepper_*] travel, which covers the bed plus any overtravel
 * to a purge or nozzle-clean position. The declared bed is the size a
 * firmware's own config states for its bed, where it states one.
 */
struct BuildVolume {
    float x_min = 0.0f;
    float x_max = 0.0f;
    float y_min = 0.0f;
    float y_max = 0.0f;
    float z_max = 0.0f;          ///< Maximum Z height (if available)
    float declared_bed_x = 0.0f; ///< Bed X the firmware config declares (0 if none)
    float declared_bed_y = 0.0f; ///< Bed Y the firmware config declares (0 if none)
    /// Rectangular plate from [bed_mesh] mesh_min/mesh_max, machine space.
    /// All zero when the config declares none (no bed_mesh, or a round bed).
    float plate_x_min = 0.0f;
    float plate_x_max = 0.0f;
    float plate_y_min = 0.0f;
    float plate_y_max = 0.0f;

    /// Printable size along X and Y, for display and reporting: the declared bed
    /// where the firmware states one, else printable_axis_span() of the travel.
    float printable_x() const;
    float printable_y() const;
};

namespace helix {
/// Printable span of one travel range. A negative minimum that is small next to
/// the maximum is endstop overtravel (a Y endstop 14mm behind the bed homes to -14),
/// not room to print in, so the span starts at 0. A centre-origin range (a delta's
/// -100..100) keeps its full extent.
float printable_axis_span(float min, float max);

/// Klipper's probe x_offset/y_offset: the probe's position relative to the nozzle.
struct ProbeXYOffset {
    double x = 0.0;
    double y = 0.0;
};
} // namespace helix

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
    /// True once an objects list was parsed, so heaters/sensors/fans/leds/
    /// printer_objects are complete and an empty list means "none", not
    /// "not reported yet". Absence-scoring heuristics (led_required) only
    /// fire when this is set: an unreported list says nothing about the
    /// hardware, and scoring it would turn a slow discovery into a wrong model.
    bool objects_reported = false;
};

/**
 * @brief One row of the printer selection list, with its manufacturer
 *
 * Selection UIs group the list by manufacturer (vendor tiles). The pseudo
 * entries appended to every list ("Custom/Other", "Unknown") have no database
 * row, so their manufacturer is empty.
 */
// NAMESPACE_OK: joins this header's global-scope types (PrinterDetectionResult et al.)
struct PrinterListEntry {
    std::string name;         ///< Display name, as get_list_names() returns it
    std::string manufacturer; ///< Database `manufacturer` field; empty for pseudo-entries
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
     * @brief Image filename for a printer type, given the hardware detected on it
     *
     * The entry's first `image_variants` entry whose conditions @p facts meet,
     * else its plain `image` (printer_image_variants.h).
     *
     * @return Image filename if found, empty string if not found
     */
    static std::string get_image_for_printer(const std::string& printer_name,
                                             const helix::printer_images::HardwareFacts& facts);

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
     * Several entries can share one preset; the one marked
     * `"preset_default": true` answers for the family, and a family without a
     * marked entry answers with its first entry in database order.
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
     * for LVGL list widget. Includes an entry when `show_in_list` is true or
     * missing (default true); entries that explicitly set it `false` (the
     * non-printer addons) are excluded. Always appends "Custom/Other"
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
     * @brief Get list names plus manufacturers, filtered by kinematics
     *
     * Element i is exactly get_list_names(kinematics)[i] with its manufacturer
     * attached, so an index into one addresses the same machine in the other.
     * The pseudo-entries ("Custom/Other", "Unknown") carry an empty
     * manufacturer; every visible database machine carries its `manufacturer`
     * field verbatim.
     *
     * @param kinematics Kinematics filter. Empty = unfiltered.
     * @return Vector of {name, manufacturer} in list order
     */
    static const std::vector<PrinterListEntry>& get_list_entries(const std::string& kinematics);

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

    /**
     * @brief Does automatic tool offset calibration replace manual Z calibration?
     *
     * Looks up the `hide_manual_z_calibration` field from the printer
     * database JSON. True only for printers whose CALIBRATE_TOOL_OFFSETS
     * routine sets every tool's Z, the reference tool included, so the paper
     * test has nothing left to set. Defaults to false: klipper-toolchanger's
     * example macro measures the other tools against T0 and writes nothing
     * for T0, so the paper test stays unless the database says otherwise.
     *
     * @param printer_name Printer name (e.g., "FlashForge Creator 5 Pro")
     * @return True when the field is set to true, false otherwise
     */
    static bool hide_manual_z_calibration(const std::string& printer_name);

    /// The database's `z_offset_persists` field for @p printer_name: false when the
    /// firmware keeps a SET_GCODE_OFFSET adjustment only for the running print.
    /// True when absent or unknown. Read only for the firmware_managed strategy.
    static bool z_offset_persists(const std::string& printer_name);

    /// The database's `"tool_change_resets_z_offset": true` flag for @p printer_name:
    /// activating a tool re-applies that head's stored gcode offset absolutely,
    /// replacing any adjustment. False when absent or unknown.
    static bool tool_change_resets_z_offset(const std::string& printer_name);

    /// Look up probe type override from the printer database.
    /// @return Probe type string (e.g., "prtouch_v2"), or empty if not specified
    static std::string get_probe_type(const std::string& printer_name);

    /**
     * @brief Printer-specific bed-mesh calibration gcode template
     *
     * For a printer whose firmware mesh macro takes parameters a bare macro slot
     * cannot carry, or needs housekeeping helixscreen cannot infer. The database
     * entry may supply a template, one line or several, under
     * `calibration.bed_mesh_gcode`.
     *
     * Read through StandardMacros rather than directly: this feeds the SHIPPED
     * tier, so the user's own Settings override still outranks it and
     * resolve_macro_script() substitutes its placeholders.
     *
     * @return Template, possibly with placeholders, or empty string if the
     *         printer ships no sequence for this operation.
     */
    static std::string get_bed_mesh_calibrate_gcode(const std::string& printer_name);

    /**
     * @brief Whether the printer's shipped bed-mesh sequence prepares the machine itself
     *
     * `calibration.bed_mesh_self_prepares` in the database entry. True means the
     * sequence heats, homes and does its own probe preparation, so nothing is sent
     * ahead of it. A shipped sequence without the flag gets the app's preparation.
     *
     * @return false when the entry, its calibration object or the flag is absent.
     */
    static bool get_bed_mesh_self_prepares(const std::string& printer_name);

    /**
     * @brief Printer-specific screws-tilt sequence
     *
     * `calibration.screws_tilt_gcode`, for firmware that wraps SCREWS_TILT_CALCULATE
     * in a macro answering a different question. `{screws_tilt_builtin}` names
     * Klipper's calculation under the wrapper's rename_existing; on a printer with
     * no wrapper the sequence yields to the stock command (resolve_macro_script()).
     *
     * @return Template, possibly with placeholders, or empty string if the
     *         printer ships no sequence for this operation.
     */
    static std::string get_screws_tilt_gcode(const std::string& printer_name);

    /// `calibration.screws_tilt_self_prepares`: the shipped screws-tilt sequence
    /// does its own probe preparation. See get_bed_mesh_self_prepares().
    static bool get_screws_tilt_self_prepares(const std::string& printer_name);

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

    /**
     * @brief Measured heating rates for a printer, in seconds per degree C
     *
     * Database field `thermal_rates` maps a heater name to the rate a first
     * print should assume, e.g. `{"heater_bed": 6.0, "extruder": 0.4}`.
     * ThermalRateManager uses these in place of its build-volume guess; a rate
     * learned from this printer's own prints still wins. Only heaters in
     * ThermalRateManager::kPersistedHeaters are returned.
     *
     * @param printer_name Printer name (e.g., "Elegoo Centauri Carbon")
     * @return heater name -> s/°C; empty when the entry declares none
     */
    static std::map<std::string, float> get_thermal_rates(const std::string& printer_name);

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
     * @brief Persist a printer type the USER chose, and merge its preset.
     *
     * The deliberate counterpart to auto_detect_and_save(): detection declines
     * whenever it is not sure, so every remaining case has to be reachable by
     * hand. Shared by the wizard's printer-identify step and the Printer
     * Manager's model row so the two cannot drift.
     *
     * Applying the preset is not optional. It merges the preset's hardware /
     * expected-device lists and fan role mappings into config; skipping it
     * makes legitimate hardware surface as "new hardware" warnings on the next
     * boot (#837).
     *
     * @param config Config instance to write to
     * @param type_name Printer type name as it appears in the database
     * @param discovery Hardware discovery, used to resolve preset variants
     * @return Name of the preset applied, or empty if the type has none
     */
    static std::string apply_type_choice(helix::Config* config, const std::string& type_name,
                                         const helix::PrinterDiscovery& discovery);

    /**
     * @brief Strip heuristics data from the database to reclaim memory
     *
     * Call after detection is complete. Removes heuristic arrays from all printer
     * entries while preserving kinematics info needed for filtered list building.
     * Safe to call multiple times (no-op after first). Use reload() to restore.
     */
    static void compact_database();

    /**
     * @brief Get the native toolhead style for a printer from the database
     * @param printer_name Printer name (case-insensitive match)
     * @return Toolhead style string (e.g., "creality_k1") or empty if none
     */
    static std::string get_toolhead_style(const std::string& printer_name);

    /// The database's `"enclosed": true` flag for @p printer_name
    /// (prestonbrown/helixscreen#1730); false when absent or unknown.
    static bool is_enclosed(const std::string& printer_name);

    /// How @p printer_name tensions its belts, from the database's `"belt_tensioner"` string
    /// (`"screw"` or `"spring"`). UNKNOWN when the field is absent, empty, not a string, or
    /// any other value, and for an unknown printer.
    static helix::calibration::BeltTensionerKind
    get_belt_tensioner(const std::string& printer_name);

    /// The database's `"klipper_hides_dot_dirs": true` flag for @p printer_name:
    /// its Klipper skips dot-directories when it resolves a file to print, so a
    /// copy staged under one fails to open. False when absent or unknown.
    static bool klipper_hides_dot_dirs(const std::string& printer_name);

    /// The database's power-loss recovery `plr` block for @p printer_name;
    /// nullopt when the printer is unknown or unlisted for it (tier 0).
    static std::optional<helix::PlrDbBlock> get_plr_block(const std::string& printer_name);

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
     * @brief Resolve a stored printer type name to the name the database uses now.
     *
     * The saved type is a display name, not a stable id, so renaming an entry
     * orphans every config written under the old name: detection returns the
     * new name, the stored one never matches again, and the user is told their
     * correctly-identified printer is set up wrong. Four entries have been
     * renamed in place so far (voron_0_1, flashforge_ad5m_pro, and both
     * zerog_nebula_*), and the database is user-updatable, so this is not a
     * closed set that a one-off migration could cover.
     *
     * Entries list their former names in "aliases". A name that is a current
     * entry name is returned untouched; a name that matches some entry's alias
     * resolves to that entry's current name; anything else (a custom rig, a
     * type from a newer database) is returned unchanged.
     *
     * @param printer_name Stored type name, possibly from an older database
     * @return The current database name for the same printer, or the input
     */
    static std::string canonical_type_name(const std::string& printer_name);

    /// Minimum winning confidence before detection may persist a printer type.
    /// Mirrors HELIX_DETECT_MIN_CONFIDENCE in scripts/lib/installer/printer_seed.sh.
    static constexpr int kAutosaveMinConfidence = 85;

    /// Minimum detection confidence before the saved-vs-detected type warning
    /// may be offered. Telling a user the type they chose is wrong is a
    /// stronger claim than filling in a blank, so it is derived from the
    /// autosave bar rather than set alongside it: the two cannot drift into an
    /// order where contradicting an explicit setting is the cheaper of the two.
    static constexpr int kMismatchMinConfidence = kAutosaveMinConfidence;

    /// Minimum lead over the best candidate offering a different outcome before
    /// a detection counts as an identification rather than a guess between
    /// look-alikes. One point: refuse an exact tie, where the winner is decided
    /// by how many heuristics somebody happened to author, and accept anything
    /// that separates at all.
    ///
    /// Raising it needs the scoring saturation fixed first
    /// (prestonbrown/helixscreen#1435). While the extra-match bonus caps, whole
    /// families of entries collapse onto one number and a one-point lead is the
    /// most that correctly-identified hardware is guaranteed to show: a genuine
    /// AD5M Pro leads the plain 5M by exactly that. Any larger floor refuses
    /// printers this database identifies correctly today.
    ///
    /// Independent of the installer's HELIX_DETECT_MIN_MARGIN in
    /// scripts/lib/installer/printer_seed.sh, which chooses between two seeding depths rather than
    /// between saving and declining.
    static constexpr int kDetectMinMargin = 1;

    /**
     * @brief Why the saved-vs-detected type warning was or was not offered.
     *
     * Every non-Warn value is a silent decline in the shipped flow, so the
     * reason is the only thing a debug bundle can use to tell "detection scored
     * 68" apart from "detection returned nothing at all".
     */
    enum class MismatchDecision {
        Warn,                 ///< Offer the prompt.
        NoDetection,          ///< Detection produced no type name.
        ConfidenceTooLow,     ///< Below kMismatchMinConfidence.
        Ambiguous,            ///< Scored well, but a rival outcome scored within kDetectMinMargin.
        MatchesSavedType,     ///< Detected type equals the saved one.
        SavedTypeNotSpecific, ///< Saved type is empty, Custom/Other, or Unknown.
        AlreadyDismissed      ///< The user already answered for this saved type.
    };

    /// Human-readable tag for a MismatchDecision, for logs.
    static const char* mismatch_decision_name(MismatchDecision decision);

    /**
     * @brief Classify the saved-vs-detected type comparison.
     *
     * Warn iff detection is high-confidence (>= kMismatchMinConfidence), leads
     * its nearest rival outcome by kDetectMinMargin, contradicts a meaningful
     * saved type (not Custom/Other, Unknown, or empty), and the per-printer
     * flag does not already equal the current saved type (changing the type
     * re-arms the warning once).
     *
     * @param detected_margin PrinterDetectionResult::margin() for the detection
     *        being classified. Every caller states it: a margin left implicit
     *        is a tie that reaches the decision as a separation, and the whole
     *        Ambiguous branch goes dark without a compile error. A caller that
     *        holds no candidate field - a bare confidence from a stored
     *        verdict - passes kDetectMinMargin, asserting nothing about rivals
     *        rather than inventing an ambiguity.
     */
    static MismatchDecision classify_type_mismatch(const std::string& saved_type,
                                                   const std::string& detected_type,
                                                   int detected_confidence,
                                                   const std::string& flag_value,
                                                   int detected_margin);

    /**
     * @brief Classify a detection result against the saved type.
     *
     * The overload callers holding a whole result should use: it reads the
     * fields the decision depends on - the margin included - off the result,
     * so a caller cannot silently drop one and still compile.
     *
     * @param detected A result from detect()/auto_detect(). A result that
     *        identified nothing classifies as NoDetection.
     */
    static MismatchDecision classify_type_mismatch(const std::string& saved_type,
                                                   const PrinterDetectionResult& detected,
                                                   const std::string& flag_value);

    /// The variant fields of one database entry. An entry with "variant_of"
    /// is the same machine as the entry with that id, differing only by a
    /// firmware mod or an add-on that "variant_label" names.
    struct VariantEntry {
        std::string id;
        std::string name;
        std::string variant_of; ///< Base entry's id, empty on a base entry
        std::string variant_label;
    };

    /// How a saved type and a contradicting detected type relate.
    enum class TypeRelation {
        DifferentModel,  ///< Two machines, or a variant link that does not resolve.
        DetectedVariant, ///< Detected is a variant of the saved base entry.
        SavedVariant,    ///< Saved is a variant; the hardware looks like its base.
        SiblingVariants  ///< Both are different variants of one base.
    };

    struct TypeRelationResult {
        TypeRelation relation = TypeRelation::DifferentModel;
        std::string base;           ///< Base entry's name; empty for DifferentModel
        std::string saved_label;    ///< Saved entry's variant label, if it is a variant
        std::string detected_label; ///< Detected entry's variant label, if it is a variant
    };

    /**
     * @brief Relate a saved and a detected type through the entries' variant links.
     *
     * Pure: decides from @p entries alone. A variant whose "variant_of" names no
     * entry, or that carries no label, counts as a model of its own, so the
     * caller falls back to the plain different-model wording.
     */
    static TypeRelationResult relate_types(const std::vector<VariantEntry>& entries,
                                           const std::string& saved_type,
                                           const std::string& detected_type);

    /// relate_types() against the loaded printer database.
    static TypeRelationResult relate_types(const std::string& saved_type,
                                           const std::string& detected_type);

    /**
     * @brief Decide whether a detection result is solid enough to persist.
     *
     * A non-empty PRINTER_TYPE makes auto_detect_and_save short-circuit
     * forever, so whatever is written here is what the printer stays. Two
     * things have to hold before that is a fair thing to do:
     *
     *  - the winner scored at least kAutosaveMinConfidence. Below that bar a
     *    custom rig whose only FlashForge-ish signal was an LED named
     *    'chamber_light' scores 55 and ships as a FlashForge Adventurer 5M Pro.
     *  - the winner leads the best candidate offering a DIFFERENT outcome by
     *    kDetectMinMargin. Without it, a field of look-alikes that all score
     *    the same is resolved by heuristic count and database order, and an
     *    arbitrary pick becomes permanent.
     *
     * Declining leaves the wizard's identify step on its Custom/Other default
     * and lets a fuller discovery snapshot try again on the next hardware
     * change, which is the honest answer when the evidence does not separate
     * the candidates.
     */
    static bool meets_autosave_threshold(const PrinterDetectionResult& result);
};

inline bool PrinterDetectionResult::ambiguous() const {
    return detected() && margin() < PrinterDetector::kDetectMinMargin;
}
