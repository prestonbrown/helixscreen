// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file hardware_validator.h
 * @brief Hardware validation layer for detecting config/discovery mismatches
 *
 * Compares settings.json expectations against Moonraker discovery results and
 * previous session state to detect missing, new, or changed hardware.
 *
 * @pattern Validation layer with persistence
 * @threading Main thread only (called from discovery callback)
 */

#pragma once

#include "json_fwd.h"

#include <optional>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class Config;
}

namespace helix {
class PrinterDiscovery;
}

/**
 * @brief Severity level for hardware validation issues
 */
enum class HardwareIssueSeverity {
    INFO,    ///< New hardware discovered (suggestion to add)
    WARNING, ///< Configured hardware missing (may be intentional)
    CRITICAL ///< Core hardware missing (extruder, heater_bed)
};

/**
 * @brief Which badge summarizes a validation result in the health headline
 *
 * Coarser than HardwareIssueSeverity. Newly discovered hardware is INFO on its
 * own row but still asks the user for attention in the headline, so INFO and
 * WARNING share one level here.
 */
enum class HardwareStatusLevel {
    OK = 0,        ///< Nothing to report
    ATTENTION = 1, ///< Hardware is missing, newly discovered, or changed
    CRITICAL = 2   ///< Core hardware missing
};

/**
 * @brief Type of hardware component
 */
enum class HardwareType {
    HEATER,          ///< Heaters (extruder, heater_bed, heater_generic)
    SENSOR,          ///< Temperature sensors (temperature_sensor, temperature_fan)
    FAN,             ///< Fans (fan, heater_fan, controller_fan, fan_generic)
    LED,             ///< LEDs (neopixel, led, dotstar)
    FILAMENT_SENSOR, ///< Filament sensors (switch, motion)
    OTHER            ///< Uncategorized
};

/**
 * @brief Convert hardware type to display string
 */
inline const char* hardware_type_to_string(HardwareType type) {
    switch (type) {
    case HardwareType::HEATER:
        return "heater";
    case HardwareType::SENSOR:
        return "sensor";
    case HardwareType::FAN:
        return "fan";
    case HardwareType::LED:
        return "led";
    case HardwareType::FILAMENT_SENSOR:
        return "filament_sensor";
    default:
        return "hardware";
    }
}

/**
 * @brief Individual hardware validation issue
 */
struct HardwareIssue {
    std::string hardware_name; ///< Full Klipper name (e.g., "heater_bed", "neopixel chamber_light")
    HardwareType hardware_type;     ///< Category of hardware
    HardwareIssueSeverity severity; ///< Issue severity level
    std::string message;            ///< Human-readable description
    bool is_optional;               ///< User marked as intentionally disconnected

    /**
     * @brief Create an issue for missing critical hardware
     */
    static HardwareIssue critical(const std::string& name, HardwareType type,
                                  const std::string& msg) {
        return {name, type, HardwareIssueSeverity::CRITICAL, msg, false};
    }

    /**
     * @brief Create an issue for missing configured hardware
     */
    static HardwareIssue warning(const std::string& name, HardwareType type, const std::string& msg,
                                 bool optional = false) {
        return {name, type, HardwareIssueSeverity::WARNING, msg, optional};
    }

    /**
     * @brief Create an issue for newly discovered hardware
     */
    static HardwareIssue info(const std::string& name, HardwareType type, const std::string& msg) {
        return {name, type, HardwareIssueSeverity::INFO, msg, false};
    }
};

/**
 * @brief Result of hardware validation with categorized issues
 */
struct HardwareValidationResult {
    /// Critical: Missing core hardware (extruder, heater_bed)
    std::vector<HardwareIssue> critical_missing;

    /// Expected: Configured in settings.json but not discovered
    std::vector<HardwareIssue> expected_missing;

    /// New: Discovered but not in config (suggest adding)
    std::vector<HardwareIssue> newly_discovered;

    /// Changed: Was present last session, now missing
    std::vector<HardwareIssue> changed_from_last_session;

    /**
     * @brief Check if any issues exist
     */
    [[nodiscard]] bool has_issues() const {
        return !critical_missing.empty() || !expected_missing.empty() ||
               !newly_discovered.empty() || !changed_from_last_session.empty();
    }

    /**
     * @brief Check if critical issues exist
     */
    [[nodiscard]] bool has_critical() const {
        return !critical_missing.empty();
    }

    /**
     * @brief Get total number of issues across all categories
     */
    [[nodiscard]] size_t total_issue_count() const {
        return critical_missing.size() + expected_missing.size() + newly_discovered.size() +
               changed_from_last_session.size();
    }

    /**
     * @brief Get the highest severity level among all issues
     */
    [[nodiscard]] HardwareIssueSeverity max_severity() const {
        if (!critical_missing.empty()) {
            return HardwareIssueSeverity::CRITICAL;
        }
        if (!expected_missing.empty() || !changed_from_last_session.empty()) {
            return HardwareIssueSeverity::WARNING;
        }
        return HardwareIssueSeverity::INFO;
    }

    /**
     * @brief Get the headline badge level for this result
     *
     * Only critical issues earn the critical badge; every other kind of issue
     * shares the attention badge.
     */
    [[nodiscard]] HardwareStatusLevel headline_level() const {
        if (!has_issues()) {
            return HardwareStatusLevel::OK;
        }
        if (has_critical()) {
            return HardwareStatusLevel::CRITICAL;
        }
        return HardwareStatusLevel::ATTENTION;
    }
};

/**
 * @brief Snapshot of hardware state for session comparison
 *
 * Stored in settings.json under "hardware_session/last_snapshot" to enable
 * detection of hardware changes between sessions.
 */
struct HardwareSnapshot {
    std::string timestamp;                     ///< ISO 8601 timestamp of when snapshot was taken
    std::vector<std::string> heaters;          ///< Discovered heater names
    std::vector<std::string> sensors;          ///< Discovered sensor names
    std::vector<std::string> fans;             ///< Discovered fan names
    std::vector<std::string> leds;             ///< Discovered LED names
    std::vector<std::string> filament_sensors; ///< Discovered filament sensor names

    /**
     * @brief Serialize snapshot to JSON
     */
    [[nodiscard]] json to_json() const;

    /**
     * @brief Deserialize snapshot from JSON
     * @param j JSON object containing snapshot data
     * @return Populated snapshot, or empty snapshot if parse fails
     */
    static HardwareSnapshot from_json(const json& j);

    /**
     * @brief Get hardware items that were in this snapshot but not in current
     * @param current Current hardware snapshot to compare against
     * @return Vector of hardware names that were removed
     */
    [[nodiscard]] std::vector<std::string> get_removed(const HardwareSnapshot& current) const;

    /**
     * @brief Get hardware items in current that weren't in this snapshot
     * @param current Current hardware snapshot to compare against
     * @return Vector of hardware names that were added
     */
    [[nodiscard]] std::vector<std::string> get_added(const HardwareSnapshot& current) const;

    /**
     * @brief Check if snapshot is empty (never populated)
     */
    [[nodiscard]] bool is_empty() const {
        return heaters.empty() && sensors.empty() && fans.empty() && leds.empty() &&
               filament_sensors.empty();
    }
};

/**
 * @brief Hardware validation layer for HelixScreen
 *
 * Compares settings.json expectations vs Moonraker discovery results.
 * Runs after on_discovery_complete_ callback.
 *
 * ## Usage:
 * @code
 * // In Application::connect_to_printer() after discovery
 * HardwareValidator validator;
 * auto result = validator.validate(
 *     config,           // settings.json expectations
 *     hardware          // PrinterDiscovery with discovered hardware
 * );
 *
 * if (result.has_issues()) {
 *     validator.notify_user(result);
 * }
 * validator.save_session_snapshot(config, hardware);
 * @endcode
 */
class HardwareValidator {
  public:
    HardwareValidator() = default;

    /**
     * @brief Perform hardware validation
     *
     * Compares:
     * 1. Critical hardware existence (extruder, heater_bed)
     * 2. Config expectations vs discovered hardware
     * 3. Previous session vs current session
     *
     * @param config Config instance with hardware settings
     * @param hardware PrinterDiscovery with discovered hardware
     * @return Validation result with categorized issues
     */
    HardwareValidationResult validate(helix::Config* config,
                                      const helix::PrinterDiscovery& hardware);

    /**
     * @brief Show persistent notification with "View Details" action
     *
     * Creates notification that appears in the notification list with an action
     * button that navigates to Hardware Health section in Settings.
     *
     * @param result Validation result to notify about
     */
    void notify_user(const HardwareValidationResult& result);

    /**
     * @brief Save current hardware state as session snapshot
     *
     * Call after successful validation to update last-known-good state.
     * Persists to settings.json under "hardware_session/last_snapshot".
     *
     * @param config Config instance to save to
     * @param hardware PrinterDiscovery with discovered hardware
     */
    void save_session_snapshot(helix::Config* config, const helix::PrinterDiscovery& hardware);

    /**
     * @brief Create snapshot from current hardware discovery state
     *
     * @param hardware PrinterDiscovery with discovered hardware
     * @return Snapshot of current hardware state
     */
    static HardwareSnapshot create_snapshot(const helix::PrinterDiscovery& hardware);

    /**
     * @brief Load previous session snapshot from config
     *
     * @param config Config instance to load from
     * @return Previous snapshot, or nullopt if none exists
     */
    static std::optional<HardwareSnapshot> load_session_snapshot(helix::Config* config);

    /**
     * @brief Check if hardware is marked as optional in config
     *
     * @param config Config instance to check
     * @param hardware_name Full hardware name (e.g., "neopixel chamber_light")
     * @return true if user marked as intentionally disconnected
     */
    static bool is_hardware_optional(helix::Config* config, const std::string& hardware_name);

    /**
     * @brief Mark hardware as optional (suppress future warnings)
     *
     * Updates settings.json and saves immediately.
     *
     * @param config Config instance to modify
     * @param hardware_name Full hardware name
     * @param optional true to mark optional, false to remove marking
     */
    static void set_hardware_optional(helix::Config* config, const std::string& hardware_name,
                                      bool optional);

    /**
     * @brief Add hardware to expected list (save to config)
     *
     * Adds newly discovered hardware to the expected hardware list in
     * settings.json so future sessions will warn if it's missing.
     *
     * @param config Config instance to modify
     * @param hardware_name Full hardware name to add
     */
    static void add_expected_hardware(helix::Config* config, const std::string& hardware_name);

    /**
     * @brief Accept everything currently discovered as expected for this printer
     *
     * Pays off a wizard hardware snapshot deferred because Klipper was down
     * during setup (#1160). Everything present at the first successful
     * discovery was there all along — the printer was simply unreachable while
     * the user ran the wizard — so reporting it as newly appeared is noise.
     *
     * Records only what validate_new_hardware() consults: fans, LEDs and
     * standalone filament sensors. Heaters are validated through their role
     * keys rather than this list. AMS/MMU capability names are deliberately
     * excluded — validate_expected_hardware() treats those as must-be-present
     * and would warn every boot if the system were later removed; only the
     * wizard's own AMS step, where the user confirms the hardware, adds them.
     *
     * Idempotent: names already present are left alone, and config is saved
     * once, only if something was added.
     *
     * @param config   Config instance to modify
     * @param hardware Discovery result to accept
     * @return Number of names added
     */
    static size_t acknowledge_discovered_hardware(helix::Config* config,
                                                  const helix::PrinterDiscovery& hardware);

  private:
    /**
     * @brief Validate critical hardware exists (extruder, heater_bed)
     *
     * Skipped entirely for `kinematics: none`. A missing item the user marked
     * optional is not reported.
     */
    void validate_critical_hardware(helix::Config* config, const helix::PrinterDiscovery& hardware,
                                    HardwareValidationResult& result);

    /**
     * @brief Validate configured hardware in settings.json exists
     */
    void validate_configured_hardware(helix::Config* config,
                                      const helix::PrinterDiscovery& hardware,
                                      HardwareValidationResult& result);

    /**
     * @brief Find hardware discovered but not in config (suggest adding)
     */
    void validate_new_hardware(helix::Config* config, const helix::PrinterDiscovery& hardware,
                               HardwareValidationResult& result);

    /**
     * @brief Compare current session against previous to find changes
     */
    void validate_session_changes(const HardwareSnapshot& previous, const HardwareSnapshot& current,
                                  helix::Config* config, HardwareValidationResult& result);

    /**
     * @brief Log the list of user-silenced (ignored) hardware at INFO level
     *
     * Called once per validation run so debug bundles capture which hardware
     * the user has silenced. The ignore list itself has no timestamp/version
     * metadata, so this log is the only visible record.
     */
    static void log_ignored_hardware(helix::Config* config);

    /**
     * @brief Check if a name is in a vector (case-insensitive)
     */
    static bool contains_name(const std::vector<std::string>& vec, const std::string& name);

    /**
     * @brief Guess hardware type from Klipper object name
     */
    static HardwareType guess_hardware_type(const std::string& name);
};
