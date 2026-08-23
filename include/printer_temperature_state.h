// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "subject_managed_panel.h"

#include <lvgl.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/**
 * @brief Chamber control mode derived from the M141 heater/cooling-fan split.
 *
 * Stored as the int value of the `chamber_mode` subject. Computed WITH the
 * cooling fan's configured resting target so the "Off" state (M141 S0 resets the
 * cooling fan to its resting target, e.g. 35°C on the K2) is recognized as Off
 * rather than misread as a deliberate "Maintaining" set.
 */
enum ChamberMode {
    Off = 0,        ///< Heater 0 AND (fan 0 OR fan == resting): effective target 0
    Heating = 1,    ///< Heater target > 0: effective target = heater target
    Maintaining = 2 ///< Heater 0, fan > 0 and fan != resting: effective target = fan target
};

/**
 * @brief Per-extruder temperature data with reactive subjects
 *
 * Each extruder discovered via init_extruders() gets its own ExtruderInfo
 * with heap-allocated subjects (unique_ptr for pointer stability across rehash).
 */
struct ExtruderInfo {
    std::string name;         ///< Klipper name: "extruder", "extruder1", etc.
    std::string display_name; ///< Human-readable: "Nozzle", "Nozzle 1"
    float temperature = 0.0f; ///< Raw float for internal tracking
    float target = 0.0f;
    /// Last non-zero nozzle target (°C), latched. Survives the target cooling to 0
    /// so a filament swap can still heat the nozzle enough to purge the previous
    /// material. Reset on unload (via clear_load_latch); in-memory only.
    float last_nonzero_target = 0.0f;
    std::unique_ptr<lv_subject_t> temp_subject;   ///< Decidegrees (value * 10)
    std::unique_ptr<lv_subject_t> target_subject; ///< Decidegrees
    SubjectLifetime temp_lifetime;   ///< Lifetime token for temp_subject (for ObserverGuard safety)
    SubjectLifetime target_lifetime; ///< Lifetime token for target_subject
};

/**
 * @brief Manages temperature-related subjects for printer state
 *
 * Extracted from PrinterState as part of god class decomposition.
 * All temperatures stored in decidegrees (value * 10 for 0.1C precision).
 *
 * Supports multiple extruders via a dynamic ExtruderInfo map. The "active extruder"
 * subjects track whichever extruder is currently selected (via set_active_extruder),
 * defaulting to "extruder". XML bindings use names "extruder_temp"/"extruder_target"
 * for the active extruder subjects.
 */
class PrinterTemperatureState {
  public:
    PrinterTemperatureState() = default;
    ~PrinterTemperatureState() = default;

    // Non-copyable
    PrinterTemperatureState(const PrinterTemperatureState&) = delete;
    PrinterTemperatureState& operator=(const PrinterTemperatureState&) = delete;

    /**
     * @brief Initialize temperature subjects
     * @param register_xml If true, register subjects with LVGL XML system
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Deinitialize subjects (called by SubjectManager automatically)
     */
    void deinit_subjects();

    /**
     * @brief Update temperatures from Moonraker status JSON
     * @param status JSON object containing "extruder" and/or "heater_bed" keys
     */
    void update_from_status(const nlohmann::json& status);

    /**
     * @brief Re-register subjects with LVGL XML system
     *
     * Call this to ensure subjects are registered in LVGL's global XML registry.
     * Use when other code may have overwritten the registry (e.g., other tests
     * calling init_subjects(true) on their own PrinterState instances).
     *
     * Does NOT reinitialize subjects - only updates LVGL XML registry mappings.
     * Safe to call multiple times.
     */
    void register_xml_subjects();

    /**
     * @brief Initialize extruder tracking from discovered heater objects
     *
     * Filters the heater list for extruder* names, creates ExtruderInfo entries
     * with heap-allocated subjects, and bumps the version subject to trigger
     * UI rebuilds.
     *
     * Safe to call multiple times (cleans up previous entries first).
     *
     * @param heaters List of Moonraker heater object names
     */
    void init_extruders(const std::vector<std::string>& heaters);

    // Active extruder subjects (decidegrees: value * 10)
    // These track whichever extruder is currently active (set via set_active_extruder)
    lv_subject_t* get_active_extruder_temp_subject() {
        return &active_extruder_temp_;
    }
    lv_subject_t* get_active_extruder_target_subject() {
        return &active_extruder_target_;
    }

    // Per-extruder subject accessors (returns nullptr if not found)
    // Prefer the overloads that return SubjectLifetime when creating observers!
    lv_subject_t* get_extruder_temp_subject(const std::string& name);
    lv_subject_t* get_extruder_target_subject(const std::string& name);

    /// Get per-extruder temp subject with lifetime token (use when creating observers)
    lv_subject_t* get_extruder_temp_subject(const std::string& name, SubjectLifetime& lifetime);
    /// Get per-extruder target subject with lifetime token (use when creating observers)
    lv_subject_t* get_extruder_target_subject(const std::string& name, SubjectLifetime& lifetime);

    lv_subject_t* get_bed_temp_subject() {
        return &bed_temp_;
    }
    lv_subject_t* get_bed_target_subject() {
        return &bed_target_;
    }

    /// Get bed temp subject with lifetime token (use when creating observers)
    lv_subject_t* get_bed_temp_subject(SubjectLifetime& lifetime) {
        lifetime = bed_temp_lifetime_;
        return &bed_temp_;
    }
    /// Get bed target subject with lifetime token (use when creating observers)
    lv_subject_t* get_bed_target_subject(SubjectLifetime& lifetime) {
        lifetime = bed_target_lifetime_;
        return &bed_target_;
    }
    lv_subject_t* get_chamber_temp_subject() {
        return &chamber_temp_;
    }
    lv_subject_t* get_chamber_temp_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_temp_lifetime_;
        return &chamber_temp_;
    }
    lv_subject_t* get_chamber_target_subject() {
        return &chamber_target_;
    }
    lv_subject_t* get_chamber_target_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_target_lifetime_;
        return &chamber_target_;
    }
    /// Chamber cooling-fan target (decidegrees). In COOLING mode the K2 M141
    /// macro parks the setpoint on the temperature_fan target while the heater
    /// target stays 0; this subject surfaces that fan target so it can be
    /// combined with the heater target for display.
    lv_subject_t* get_chamber_fan_target_subject() {
        return &chamber_fan_target_;
    }
    lv_subject_t* get_chamber_fan_target_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_fan_target_lifetime_;
        return &chamber_fan_target_;
    }
    /// Effective chamber setpoint (decidegrees): the heater target when heating
    /// (target > 0), otherwise the cooling-fan target. In COOLING mode the heater
    /// target is 0 and the real M141 setpoint lives on the fan; this subject
    /// surfaces whichever is active so temp_display shows the maintain setpoint
    /// instead of "--".
    lv_subject_t* get_chamber_effective_target_subject() {
        return &chamber_effective_target_;
    }
    lv_subject_t* get_chamber_effective_target_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_effective_target_lifetime_;
        return &chamber_effective_target_;
    }
    /// Chamber control mode (ChamberMode int): Off/Heating/Maintaining. Computed
    /// WITH the cooling-fan resting target so M141 S0 (which parks the fan at its
    /// resting target, e.g. 35°C) reads as Off, not Maintaining.
    lv_subject_t* get_chamber_mode_subject() {
        return &chamber_mode_;
    }
    lv_subject_t* get_chamber_mode_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_mode_lifetime_;
        return &chamber_mode_;
    }

    // Chamber-heater diagnostics subjects (backend-provided, issue #1290).
    // Capability-gated: they only update once set_chamber_diagnostics_source()
    // has wired a backend's surfaces; absent objects in a delta status frame
    // leave the last values in place.
    /// Latched chamber-heater fault (0/1)
    lv_subject_t* get_chamber_heater_fault_subject() {
        return &chamber_heater_fault_;
    }
    lv_subject_t* get_chamber_heater_fault_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_fault_lifetime_;
        return &chamber_heater_fault_;
    }
    /// Heating inhibited by the heater's own protection (0/1)
    lv_subject_t* get_chamber_heater_inhibited_subject() {
        return &chamber_heater_inhibited_;
    }
    lv_subject_t* get_chamber_heater_inhibited_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_inhibited_lifetime_;
        return &chamber_heater_inhibited_;
    }
    /// Translated fault reason for the UI ("" when none) — derived from the
    /// backend's generic FaultReason kind; the raw vendor code never binds.
    lv_subject_t* get_chamber_heater_fault_reason_text_subject() {
        return &chamber_heater_fault_reason_text_;
    }
    lv_subject_t* get_chamber_heater_fault_reason_text_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_fault_reason_text_lifetime_;
        return &chamber_heater_fault_reason_text_;
    }
    /// Filter fan running state (-1 unknown, 0 off, 1 on)
    lv_subject_t* get_chamber_filter_fan_on_subject() {
        return &chamber_filter_fan_on_;
    }
    lv_subject_t* get_chamber_filter_fan_on_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_on_lifetime_;
        return &chamber_filter_fan_on_;
    }
    /// Heating-element temp display string ("--" unknown; "106.2°C" nominal)
    lv_subject_t* get_chamber_heater_element_temp_text_subject() {
        return &chamber_heater_element_temp_text_;
    }
    lv_subject_t* get_chamber_heater_element_temp_text_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_element_temp_text_lifetime_;
        return &chamber_heater_element_temp_text_;
    }
    /// Filter fan speed display string ("--" unknown; "100%" nominal)
    lv_subject_t* get_chamber_filter_fan_percent_text_subject() {
        return &chamber_filter_fan_percent_text_;
    }
    lv_subject_t* get_chamber_filter_fan_percent_text_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_percent_text_lifetime_;
        return &chamber_filter_fan_percent_text_;
    }
    /// Filter-fan toggle label (translated "Filter Fan: On"/"Filter Fan: Off")
    lv_subject_t* get_chamber_filter_fan_on_text_subject() {
        return &chamber_filter_fan_on_text_;
    }
    lv_subject_t* get_chamber_filter_fan_on_text_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_on_text_lifetime_;
        return &chamber_filter_fan_on_text_;
    }
    /// Filter-fan toggle icon name ("fan"/"fan_off") — drives the compact
    /// portrait card's icon button via bind_icon
    lv_subject_t* get_chamber_filter_fan_icon_subject() {
        return &chamber_filter_fan_icon_;
    }
    lv_subject_t* get_chamber_filter_fan_icon_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_icon_lifetime_;
        return &chamber_filter_fan_icon_;
    }

    /// Number of tracked extruders
    int extruder_count() const {
        return static_cast<int>(extruders_.size());
    }

    /// Access to extruder map (for UI enumeration)
    const std::unordered_map<std::string, ExtruderInfo>& extruders() const {
        return extruders_;
    }

    /// Version subject, bumped when extruder list changes (for UI rebuild triggers)
    lv_subject_t* get_extruder_version_subject() {
        return &extruder_version_;
    }

    /**
     * @brief Set the sensor name used to read chamber temperature
     * @param name Klipper sensor name (e.g., "temperature_sensor chamber")
     */
    void set_chamber_sensor_name(const std::string& name) {
        chamber_sensor_name_ = name;
    }

    /**
     * @brief Set the heater name used to control chamber temperature
     * @param name Klipper heater name (e.g., "heater_generic chamber")
     */
    void set_chamber_heater_name(const std::string& name) {
        chamber_heater_name_ = name;
    }

    /**
     * @brief Set the temperature_fan name carrying the chamber cooling setpoint
     * @param name Klipper object name (e.g., "temperature_fan chamber_fan")
     *
     * In COOLING mode the K2 M141 macro writes the setpoint to this fan's
     * `target`; surfaced via get_chamber_fan_target_subject().
     */
    void set_chamber_cooling_fan_name(const std::string& name) {
        chamber_cooling_fan_name_ = name;
    }

    /**
     * @brief Set the cooling fan's configured resting/off target (decidegrees)
     * @param deci Resting target ×10 (e.g. 350 for 35°C on the K2)
     *
     * `M141 S0` ("Off") resets the cooling fan to this configured resting target
     * with the heater at 0. Recognizing the resting value lets the chamber mode
     * report Off instead of misreading the resting fan target as a deliberate
     * "Maintaining" set. Read from configfile.settings[<fan>].target_temp at
     * discovery; defaults to 0 (pre-config-fetch), where the `fan != resting`
     * test still distinguishes a real maintain set (fan > 0) from off.
     */
    void set_chamber_fan_resting(int deci) {
        chamber_fan_resting_deci_ = deci;
    }

    /**
     * @brief Set the chamber-heater diagnostics source (backend-resolved)
     * @param backend_id Matched backend id ("" = none; see chamber_heater_backend.h)
     * @param diagnostics_object Status object carrying diagnostics ("" = none)
     * @param filter_fan_pin Binary filtration-fan output_pin ("" = none)
     *
     * Called from PrinterState's chamber resolution. Until a non-empty
     * diagnostics object is wired, diagnostics status objects are ignored.
     */
    void set_chamber_diagnostics_source(const std::string& backend_id,
                                        const std::string& diagnostics_object,
                                        const std::string& filter_fan_pin) {
        chamber_backend_id_ = backend_id;
        chamber_diagnostics_object_ = diagnostics_object;
        chamber_filter_fan_pin_ = filter_fan_pin;
    }

    /**
     * @brief Get the Klipper heater name for chamber (empty if sensor-only)
     */
    const std::string& chamber_heater_name() const {
        return chamber_heater_name_;
    }

    /**
     * @brief Get the Klipper sensor name for chamber (empty if no sensor configured)
     */
    const std::string& chamber_sensor_name() const {
        return chamber_sensor_name_;
    }

    /**
     * @brief Get the status object carrying chamber-heater diagnostics ("" = none)
     */
    const std::string& chamber_diagnostics_object() const {
        return chamber_diagnostics_object_;
    }

    /**
     * @brief Set which extruder is active (mirrors its data to active subjects)
     *
     * Validates that the name exists in the extruder map. If unknown, logs a
     * warning and keeps the previous active extruder.
     *
     * @param name Klipper extruder name (e.g., "extruder", "extruder1")
     */
    void set_active_extruder(const std::string& name);

    /**
     * @brief Get the name of the currently active extruder
     * @return Klipper name of active extruder (defaults to "extruder")
     */
    const std::string& active_extruder_name() const;

    /**
     * @brief Active extruder's latched last non-zero target (°C)
     *
     * Returns the active extruder's last_nonzero_target, or 0 if the active
     * extruder is unknown (not yet discovered). Used by the swap-preheat guard
     * so a nozzle that cooled to 0 still heats enough to purge the previous
     * material.
     */
    float get_active_extruder_last_nonzero_target() const {
        auto it = extruders_.find(active_extruder_name_);
        return it != extruders_.end() ? it->second.last_nonzero_target : 0.0f;
    }

    /**
     * @brief Clear the load latch (last non-zero target) for an extruder
     * @param extruder_name Klipper name; empty resolves to the active extruder
     *
     * Called on unload — the filament is physically pulled, so there is nothing
     * left to purge and the held temperature must not linger into the next load.
     */
    void clear_load_latch(const std::string& extruder_name = "") {
        const std::string& name = extruder_name.empty() ? active_extruder_name_ : extruder_name;
        auto it = extruders_.find(name);
        if (it != extruders_.end()) {
            it->second.last_nonzero_target = 0.0f;
        }
    }

  private:
    friend class PrinterTemperatureStateTestAccess;

    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Active extruder subjects (decidegrees: 205.3C stored as 2053)
    // These track whichever extruder is currently active, defaulting to "extruder".
    // Registered in XML as "extruder_temp"/"extruder_target" for binding compatibility.
    lv_subject_t active_extruder_temp_{};
    lv_subject_t active_extruder_target_{};
    lv_subject_t bed_temp_{};
    lv_subject_t bed_target_{};
    SubjectLifetime bed_temp_lifetime_;
    SubjectLifetime bed_target_lifetime_;
    // XML display subjects: chamber_effective_target + chamber_mode are THE canonical
    // chamber display bindings (XML-registered). chamber_target / chamber_fan_target are
    // internal synthesis inputs only — intentionally NOT XML-registered.
    lv_subject_t chamber_temp_{};
    lv_subject_t
        chamber_target_{}; ///< Internal: raw heater target (0 in Maintaining mode) — NOT XML
    lv_subject_t chamber_fan_target_{};       ///< Internal: cooling-fan target — NOT XML
    lv_subject_t chamber_effective_target_{}; ///< XML display: heater target (Heating) or fan
                                              ///< target (Maintaining) or 0 (Off)
    lv_subject_t
        chamber_mode_{}; ///< XML display: ChamberMode int Off/Heating/Maintaining (resting-aware)
    SubjectLifetime chamber_temp_lifetime_;
    SubjectLifetime chamber_target_lifetime_;
    SubjectLifetime chamber_fan_target_lifetime_;
    SubjectLifetime chamber_effective_target_lifetime_;
    SubjectLifetime chamber_mode_lifetime_;

    // Chamber-heater diagnostics (backend-provided, issue #1290). Absent
    // objects in a delta status frame = no news: subjects keep last values.
    lv_subject_t chamber_heater_fault_{};             ///< XML: 0/1
    lv_subject_t chamber_heater_inhibited_{};         ///< XML: 0/1
    lv_subject_t chamber_heater_fault_reason_text_{}; ///< XML: translated reason, "" when none
    lv_subject_t chamber_filter_fan_on_{};            ///< XML: -1 unknown / 0 / 1
    lv_subject_t chamber_heater_element_temp_text_{}; ///< XML: display string ("--"/"106.2°C")
    lv_subject_t chamber_filter_fan_percent_text_{};  ///< XML: display string ("--"/"100%")
    lv_subject_t chamber_filter_fan_on_text_{};       ///< XML: translated toggle label
    lv_subject_t chamber_filter_fan_icon_{};          ///< XML: toggle icon name ("fan"/"fan_off")
    char chamber_heater_fault_reason_text_buf_[64] = {};
    char chamber_heater_element_temp_text_buf_[32] = {};
    char chamber_filter_fan_percent_text_buf_[32] = {};
    char chamber_filter_fan_on_text_buf_[64] = {};
    char chamber_filter_fan_icon_buf_[16] = {};
    SubjectLifetime chamber_heater_fault_lifetime_;
    SubjectLifetime chamber_heater_inhibited_lifetime_;
    SubjectLifetime chamber_heater_fault_reason_text_lifetime_;
    SubjectLifetime chamber_filter_fan_on_lifetime_;
    SubjectLifetime chamber_heater_element_temp_text_lifetime_;
    SubjectLifetime chamber_filter_fan_percent_text_lifetime_;
    SubjectLifetime chamber_filter_fan_on_text_lifetime_;
    SubjectLifetime chamber_filter_fan_icon_lifetime_;

    // Dynamic per-extruder tracking
    std::unordered_map<std::string, ExtruderInfo> extruders_;
    lv_subject_t extruder_version_{}; ///< Bumped when extruder list changes

    // Active extruder name (defaults to "extruder")
    std::string active_extruder_name_ = "extruder";

    // Chamber configuration
    std::string chamber_sensor_name_; ///< Klipper sensor name (e.g., "temperature_sensor chamber")
    std::string chamber_heater_name_; ///< Klipper heater name (e.g., "heater_generic chamber"),
                                      ///< empty if sensor-only
    std::string
        chamber_cooling_fan_name_; ///< temperature_fan carrying the chamber cooling
                                   ///< setpoint (M141 target in cooling mode), empty if none
    int chamber_fan_resting_deci_ =
        0; ///< Cooling fan's configured resting/off target
           ///< (decidegrees); M141 S0 returns the fan here. 0 = unknown.

    // Chamber-heater diagnostics source (resolved in PrinterState::set_hardware)
    std::string chamber_backend_id_;         ///< Matched backend id, "" = none/generic
    std::string chamber_diagnostics_object_; ///< Status object with diagnostics, "" = none
    std::string chamber_filter_fan_pin_;     ///< Binary filter fan output_pin, "" = none
};

} // namespace helix
