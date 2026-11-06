// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "printer_discovery.h"

#include <map>
#include <string>

/**
 * @brief Override state for a printer capability
 *
 * Three-state logic for capability overrides in settings.json:
 * - AUTO: Use auto-detected capability from PrinterCapabilities
 * - ENABLE: Force capability to be available (regardless of detection)
 * - DISABLE: Force capability to be unavailable (regardless of detection)
 */
enum class OverrideState {
    AUTO,    ///< Use auto-detected value from PrinterCapabilities
    ENABLE,  ///< Force capability ON (user knows better than auto-detection)
    DISABLE, ///< Force capability OFF (user wants to hide option)
};

/**
 * @brief Capability names used in config and override system
 *
 * These string constants map to settings.json keys under capability_overrides.
 */
namespace capability {
constexpr const char* BED_MESH = "bed_mesh";
constexpr const char* QGL = "qgl";
constexpr const char* Z_TILT = "z_tilt";
constexpr const char* NOZZLE_CLEAN = "nozzle_clean";
constexpr const char* HEAT_SOAK = "heat_soak";
constexpr const char* CHAMBER = "chamber";
constexpr const char* SPEAKER = "speaker";
// CONST_NAME_OK: matches the namespace's existing capability names
constexpr const char* LED = "led"; // NAMESPACE_OK: joins the capability:: name table
} // namespace capability

/**
 * @brief Wrapper around PrinterCapabilities with user-configurable overrides
 *
 * Provides a three-state override system for printer capabilities. Users can
 * force-enable features that weren't auto-detected (e.g., heat soak without
 * chamber heater) or force-disable features they don't want to see.
 *
 * ## Config Format (settings.json)
 *
 * ```json
 * {
 *   "capability_overrides": {
 *     "bed_leveling": "auto",
 *     "qgl": "auto",
 *     "z_tilt": "auto",
 *     "nozzle_clean": "enable",
 *     "heat_soak": "enable",
 *     "chamber": "disable"
 *   }
 * }
 * ```
 *
 * ## Usage
 *
 * ```cpp
 * CapabilityOverrides overrides;
 * overrides.load_from_config();
 * overrides.set_hardware(hardware);
 *
 * // Check effective capability (with overrides applied)
 * if (overrides.is_available(capability::BED_MESH)) {
 *     // Show bed leveling option
 * }
 * ```
 *
 * @see PrinterDiscovery for auto-detection logic
 */
class CapabilityOverrides {
  public:
    CapabilityOverrides() = default;

    // Copyable and movable
    CapabilityOverrides(const CapabilityOverrides&) = default;
    CapabilityOverrides& operator=(const CapabilityOverrides&) = default;
    CapabilityOverrides(CapabilityOverrides&&) = default;
    CapabilityOverrides& operator=(CapabilityOverrides&&) = default;
    ~CapabilityOverrides() = default;

    /**
     * @brief Load overrides from settings.json
     *
     * Reads the capability_overrides section and populates the override map.
     * Missing keys default to AUTO (use auto-detection).
     */
    void load_from_config();

    /**
     * @brief Set the underlying hardware discovery for AUTO lookups
     *
     * @param hardware Discovered printer hardware
     */
    void set_hardware(const helix::PrinterDiscovery& hardware);

    /**
     * @brief Get override state for a capability
     *
     * @param name Capability name (use capability:: constants)
     * @return Override state (AUTO if not configured)
     */
    [[nodiscard]] OverrideState get_override(const std::string& name) const;

    /**
     * @brief Set override for a capability (in memory only)
     *
     * @param name Capability name
     * @param state Override state to set
     */
    void set_override(const std::string& name, OverrideState state);

    /**
     * @brief Check if capability is effectively available
     *
     * Applies override logic:
     * - ENABLE: always returns true
     * - DISABLE: always returns false
     * - AUTO: returns auto-detected value from PrinterCapabilities
     *
     * @param name Capability name
     * @return true if capability should be shown/available
     */
    [[nodiscard]] bool is_available(const std::string& name) const;

    /**
     * @brief Whether the user turned a capability on that detection did not find
     *
     * True only for an ENABLE override on a capability auto-detection reports
     * absent. ENABLE on a detected capability changes nothing, so it is not
     * "forced".
     */
    [[nodiscard]] bool is_forced_on(const std::string& name) const;

    /**
     * @brief Check bed mesh availability (with overrides)
     */
    [[nodiscard]] bool has_bed_mesh() const {
        return is_available(capability::BED_MESH);
    }

    /**
     * @brief Check QGL availability (with overrides)
     */
    [[nodiscard]] bool has_qgl() const {
        return is_available(capability::QGL);
    }

    /**
     * @brief Check Z-tilt availability (with overrides)
     */
    [[nodiscard]] bool has_z_tilt() const {
        return is_available(capability::Z_TILT);
    }

    /**
     * @brief Check nozzle clean availability (with overrides)
     */
    [[nodiscard]] bool has_nozzle_clean() const {
        return is_available(capability::NOZZLE_CLEAN);
    }

    /**
     * @brief Check heat soak availability (with overrides)
     */
    [[nodiscard]] bool has_heat_soak() const {
        return is_available(capability::HEAT_SOAK);
    }

    /**
     * @brief Check chamber availability (with overrides)
     */
    [[nodiscard]] bool has_chamber() const {
        return is_available(capability::CHAMBER);
    }

    /**
     * @brief Check speaker availability (with overrides)
     */
    [[nodiscard]] bool has_speaker() const {
        return is_available(capability::SPEAKER);
    }

    /**
     * @brief Check controllable light availability (with overrides)
     */
    [[nodiscard]] bool has_led() const {
        return is_available(capability::LED);
    }

    /**
     * @brief Whether the led capability is turned off, so every light control hides
     *
     * Only DISABLE hides lights the LED controller found (WLED, macro lights);
     * AUTO leaves them to detection.
     */
    [[nodiscard]] bool lights_disabled() const {
        return get_override(capability::LED) == OverrideState::DISABLE;
    }

    /**
     * @brief Check if the printer is running Kalico firmware
     *
     * Delegates to the stored PrinterDiscovery's is_kalico() flag,
     * which is set during the discovery sequence when Kalico-specific
     * objects are detected.
     */
    [[nodiscard]] bool is_kalico() const {
        return hardware_set_ && hardware_.is_kalico();
    }

    /**
     * @brief Whether the firmware accepts the HEATER_INTERRUPT gcode command
     *
     * Today only Kalico registers it; this is the one place that mapping lives.
     */
    [[nodiscard]] bool supports_heater_interrupt() const {
        return is_kalico();
    }

    /**
     * @brief Whether the firmware offers MPC heater control and the control-type query
     *
     * Today only Kalico does; this is the one place that mapping lives.
     */
    [[nodiscard]] bool supports_mpc_calibration() const {
        return is_kalico();
    }

    /**
     * @brief Save current overrides to settings.json
     *
     * Persists in-memory override changes to disk.
     *
     * @return true if save succeeded
     */
    bool save_to_config();

    /**
     * @brief Save one capability's override to settings.json
     *
     * Writes only @p name, so the other capabilities keep whatever settings.json
     * holds for them (absent means auto).
     *
     * @return true if save succeeded
     */
    bool save_override_to_config(const std::string& name);

    /**
     * @brief Get summary string for logging
     */
    [[nodiscard]] std::string summary() const;

    /**
     * @brief Parse override state from string
     *
     * @param str "auto", "enable", or "disable" (case-insensitive)
     * @return Parsed state, defaults to AUTO for invalid input
     */
    static OverrideState parse_state(const std::string& str);

    /**
     * @brief Convert override state to string
     */
    static std::string state_to_string(OverrideState state);

  private:
    /**
     * @brief Get auto-detected capability value
     */
    [[nodiscard]] bool get_auto_value(const std::string& name) const;

    std::map<std::string, OverrideState> overrides_;
    helix::PrinterDiscovery hardware_;
    bool hardware_set_ = false;
};
