// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // SubjectLifetime

#include "chamber_heater_backend.h"
#include "subject_managed_panel.h"

#include <cmath>
#include <lvgl.h>
#include <memory>
#include <optional>
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
    Off = 0,         ///< Heater 0 AND fan 0: effective target 0, nothing displayed
    Heating = 1,     ///< Heater target > 0: effective target = heater target
    Maintaining = 2, ///< Heater 0, fan > 0 and fan != resting: effective target = fan target
    Resting = 3      ///< Heater 0, fan target == its configured resting value: the standing
                     ///< auto setpoint (M141 S0 parks here; a deliberate set equal to resting
                     ///< is byte-identical). Target displayed, auto glyph instead of the check.
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
    /// Klipper's reported pressure advance K, negative until the extruder
    /// reports one.
    float pressure_advance = -1.0f;
    /// The fitted nozzle as the extruder reports it at runtime, which firmware
    /// that tracks swappable nozzles keeps outside configfile.settings: the
    /// diameter in mm (0 until reported) and the flow type as reported ("" until
    /// reported). Stock Klipper reports neither.
    double nozzle_diameter = 0.0;
    std::string nozzle_volume_type;
    std::unique_ptr<lv_subject_t> temp_subject;   ///< Decidegrees (value * 10)
    std::unique_ptr<lv_subject_t> target_subject; ///< Decidegrees
    std::unique_ptr<lv_subject_t> power_subject;  ///< Duty in whole percent, -1 until reported
    SubjectLifetime temp_lifetime;   ///< Lifetime token for temp_subject (for ObserverGuard safety)
    SubjectLifetime target_lifetime; ///< Lifetime token for target_subject
    SubjectLifetime power_lifetime;  ///< Lifetime token for power_subject
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
     * @brief Initialize extruder tracking from discovered heater objects
     *
     * Filters the heater list for extruder* names and reconciles against the
     * current set: an extruder still present keeps its subjects and lifetimes,
     * a removed one has its lifetimes flipped and subjects freed, a new one
     * gets fresh heap-allocated subjects. Display names are recomputed, and the
     * version subject bumps only when the set changed.
     *
     * @param heaters List of Moonraker heater object names
     */
    void init_extruders(const std::vector<std::string>& heaters);

    /// Re-render each extruder's translated display name ("Nozzle 2") in the
    /// current language. The extruder set is unchanged, so extruder_version is
    /// not bumped: consumers showing a name re-read it on the language switch.
    void refresh_display_names();

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

    /// Get a specific extruder's heater duty with lifetime token (whole
    /// percent, -1 until that heater reports one). Distinct from the nullary
    /// overload, which is the ACTIVE extruder's mirror.
    lv_subject_t* get_extruder_power_subject(const std::string& name, SubjectLifetime& lifetime);

    lv_subject_t* get_extruder_power_subject() {
        return &active_extruder_power_;
    }

    /// The active extruder's pressure advance K x 10000 (0.0420 is 420), -1
    /// until it reports one. Registered as "extruder_pressure_advance".
    lv_subject_t* get_active_extruder_pressure_advance_subject() {
        return &active_extruder_pa_;
    }

    /// A specific extruder's reported pressure advance K, nullopt until it
    /// reports one or when no such extruder exists.
    std::optional<float> extruder_pressure_advance(const std::string& name) const;
    lv_subject_t* get_bed_power_subject() {
        return &bed_power_;
    }
    lv_subject_t* get_chamber_power_subject() {
        return &chamber_power_;
    }
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
    /// Device unreachable on its own link (0/1). 1 only on an engaged
    /// "not connected" report — a backend with no link state leaves it 0
    /// (unknown is not offline).
    lv_subject_t* get_chamber_heater_externally_controlled_subject() {
        return &chamber_heater_externally_controlled_;
    }
    lv_subject_t* get_chamber_heater_externally_controlled_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_externally_controlled_lifetime_;
        return &chamber_heater_externally_controlled_;
    }
    lv_subject_t* get_chamber_heater_offline_subject() {
        return &chamber_heater_offline_;
    }
    lv_subject_t* get_chamber_heater_offline_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_offline_lifetime_;
        return &chamber_heater_offline_;
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
    /// Filter fan running state (-1 unknown, 0 off, 1 on) — whether the fan is
    /// actually spinning, from the backend-reported speed when there is one,
    /// from the pin otherwise. NOT the pin: the device also runs this fan on
    /// its own while heating or purging.
    lv_subject_t* get_chamber_filter_fan_on_subject() {
        return &chamber_filter_fan_on_;
    }
    lv_subject_t* get_chamber_filter_fan_on_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_on_lifetime_;
        return &chamber_filter_fan_on_;
    }
    /// Filter-fan pin request (-1 unknown, 0 off, 1 on) — what WE asked for
    /// via the output_pin; the click handler inverts this, not the running
    /// state.
    lv_subject_t* get_chamber_filter_fan_requested_subject() {
        return &chamber_filter_fan_requested_;
    }
    lv_subject_t* get_chamber_filter_fan_requested_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_requested_lifetime_;
        return &chamber_filter_fan_requested_;
    }
    /// Filter fan is device-driven (0/1): the backend reports the device
    /// running it on its own initiative (heater warmup, thermal purge), so a
    /// pin request cannot stop it.
    lv_subject_t* get_chamber_filter_fan_device_driven_subject() {
        return &chamber_filter_fan_device_driven_;
    }
    lv_subject_t* get_chamber_filter_fan_device_driven_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_filter_fan_device_driven_lifetime_;
        return &chamber_filter_fan_device_driven_;
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

    /// The heater turned down the last target we sent and has not accepted
    /// one since (0/1). Clears when the device reports nothing outstanding or
    /// the heater's target follows the requested one.
    lv_subject_t* get_chamber_heater_command_rejected_subject() {
        return &chamber_heater_command_rejected_;
    }
    lv_subject_t* get_chamber_heater_command_rejected_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_command_rejected_lifetime_;
        return &chamber_heater_command_rejected_;
    }
    /// Counts rejection events: bumps on the first and on every later one
    /// while latched, so an observer can act once per rejection. Not XML.
    lv_subject_t* get_chamber_heater_rejection_seq_subject() {
        return &chamber_heater_rejection_seq_;
    }
    lv_subject_t* get_chamber_heater_rejection_seq_subject(SubjectLifetime& lifetime) {
        lifetime = chamber_heater_rejection_seq_lifetime_;
        return &chamber_heater_rejection_seq_;
    }
    /// Translated sentence saying why the heater did not start ("" when none).
    lv_subject_t* get_chamber_heater_rejection_text_subject() {
        return &chamber_heater_rejection_text_;
    }
    /// The latched rejection, for callers that act on it (retry, prompt).
    struct ChamberCommandRejection {
        chamber::CommandRejection kind = chamber::CommandRejection::None;
        chamber::RefusalCause cause = chamber::RefusalCause::Unspecified;
        double requested_target_c = 0; ///< 0 = not reported
    };
    /// The chamber device reports its own wait on the chamber (an M191 its
    /// glue runs) holding the queue, and the temperature it waits for (°C, 0
    /// when not reported). False for a backend that reports no such wait.
    bool chamber_wait_active() const {
        return chamber_wait_active_;
    }
    double chamber_wait_target_c() const {
        return chamber_wait_active_ && !std::isnan(chamber_wait_target_c_) ? chamber_wait_target_c_
                                                                           : 0.0;
    }
    ChamberCommandRejection chamber_command_rejection() const {
        if (!chamber_rejection_latched_) {
            return {};
        }
        return {chamber_rejection_kind_, chamber_refusal_cause_,
                std::isnan(chamber_rejection_target_c_) ? 0.0 : chamber_rejection_target_c_};
    }

    /// A filament-drying cycle is running on the chamber appliance (0/1).
    lv_subject_t* get_chamber_dryer_active_subject() {
        return &chamber_dryer_active_;
    }
    /// The running cycle's readout ("41/55°C  3:12 left"), "" when idle.
    lv_subject_t* get_chamber_dryer_text_subject() {
        return &chamber_dryer_text_;
    }
    /// 1 once a status frame from the printer has reported the cycle's state, so the
    /// default 0 of `chamber_dryer_active` before that is not read as "idle".
    lv_subject_t* get_chamber_dryer_reported_subject() {
        return &chamber_dryer_reported_;
    }
    /// Minutes left in the running cycle, rounded up; 0 once the device reports none left,
    /// -1 until it reports anything.
    int chamber_dryer_remaining_min() const {
        return chamber_dryer_remaining_reported_ ? (chamber_dryer_remaining_s_ + 59) / 60 : -1;
    }
    /// The running cycle's target in C; 0 when the device reports none.
    int chamber_dryer_target_c() const {
        return chamber_dryer_target_c_;
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

    /// Death signal for every subject this state owns (the active-extruder
    /// mirrors, the version subject and the per-extruder subjects alike).
    /// Hand to observe_*() by anything that can outlive a deinit.
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
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
        chamber_dryer_running_ = false;
        chamber_dryer_remaining_s_ = 0;
        chamber_dryer_remaining_reported_ = false;
        chamber_dryer_target_c_ = 0;
        chamber_rejection_kind_ = chamber::CommandRejection::None;
        chamber_refusal_cause_ = chamber::RefusalCause::Unspecified;
        chamber_rejection_vendor_seq_ = -1;
        chamber_rejection_target_c_ = NAN;
        chamber_wait_active_ = false;
        chamber_wait_target_c_ = NAN;
        if (subjects_initialized_) {
            lv_subject_set_int(&chamber_dryer_reported_, 0);
            publish_chamber_dryer();
            clear_chamber_rejection();
        }
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
     * @brief The Klipper object the chamber temperature is read from.
     *
     * A chamber heater measures its own chamber, so discovery resolves the
     * sensor role to the heater itself and the two names agree. A sensor that
     * names a different object is therefore a deliberate assignment, and it
     * wins the reading while the heater goes on supplying the target. With no
     * heater the sensor is the only reading there is. Every chamber readout
     * and graph series resolves its source here so they cannot disagree about
     * which probe they mean.
     */
    const std::string& chamber_temperature_source() const {
        if (!chamber_sensor_name_.empty() && chamber_sensor_name_ != chamber_heater_name_) {
            return chamber_sensor_name_;
        }
        return chamber_heater_name_;
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
    SubjectManager subjects_;
    bool subjects_initialized_ = false;

    // Active extruder subjects (decidegrees: 205.3C stored as 2053)
    // These track whichever extruder is currently active, defaulting to "extruder".
    // Registered in XML as "extruder_temp"/"extruder_target" for binding compatibility.
    lv_subject_t active_extruder_temp_{};
    lv_subject_t active_extruder_target_{};
    lv_subject_t bed_temp_{};
    lv_subject_t bed_target_{};
    // Duty cycle in whole percent, -1 until a heater reports one. Klipper sends
    // 0.0-1.0 on a heater object; a temperature_fan reports a speed instead and
    // never publishes power, so -1 is a lasting state, not just a startup one.
    lv_subject_t active_extruder_power_{};
    lv_subject_t active_extruder_pa_{};
    lv_subject_t bed_power_{};
    lv_subject_t chamber_power_{};
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
    lv_subject_t chamber_heater_fault_{};                 ///< XML: 0/1
    lv_subject_t chamber_heater_inhibited_{};             ///< XML: 0/1
    lv_subject_t chamber_heater_externally_controlled_{}; ///< XML: 0/1
    lv_subject_t chamber_heater_offline_{};               ///< XML: 0/1
    lv_subject_t chamber_heater_fault_reason_text_{};     ///< XML: translated reason, "" when none
    lv_subject_t chamber_filter_fan_on_{};                ///< XML: -1 unknown / 0 / 1 (fan RUNNING)
    lv_subject_t chamber_filter_fan_requested_{};     ///< XML: -1 unknown / 0 / 1 (our pin request)
    lv_subject_t chamber_filter_fan_device_driven_{}; ///< XML: 0/1 (device runs the fan itself)
    lv_subject_t chamber_heater_element_temp_text_{}; ///< XML: display string ("--"/"106.2°C")
    lv_subject_t chamber_filter_fan_percent_text_{};  ///< XML: display string ("--"/"100%")
    char chamber_heater_fault_reason_text_buf_[64] = {};
    char chamber_heater_element_temp_text_buf_[32] = {};
    char chamber_filter_fan_percent_text_buf_[32] = {};
    lv_subject_t chamber_dryer_active_{}; ///< XML: 0/1
    lv_subject_t chamber_dryer_reported_{};
    lv_subject_t chamber_dryer_text_{}; ///< XML: countdown string, "" when idle
    char chamber_dryer_text_buf_[64] = {};
    /// Last reported drying state; frames name only what changed.
    bool chamber_dryer_running_ = false;
    int chamber_dryer_remaining_s_ = 0;
    bool chamber_dryer_remaining_reported_ = false;
    int chamber_dryer_target_c_ = 0; ///< 0 = not reported
    void publish_chamber_dryer();
    lv_subject_t chamber_heater_command_rejected_{}; ///< XML: 0/1
    lv_subject_t chamber_heater_rejection_seq_{};    ///< Internal: rejection event count
    lv_subject_t chamber_heater_rejection_text_{};   ///< XML: translated sentence
    char chamber_heater_rejection_text_buf_[160] = {};
    SubjectLifetime chamber_heater_command_rejected_lifetime_;
    SubjectLifetime chamber_heater_rejection_seq_lifetime_;
    /// Last reported rejection; frames carry only what changed.
    chamber::CommandRejection chamber_rejection_kind_ = chamber::CommandRejection::None;
    chamber::RefusalCause chamber_refusal_cause_ = chamber::RefusalCause::Unspecified;
    int chamber_rejection_vendor_seq_ = -1; ///< -1 = not reported
    double chamber_rejection_target_c_ = NAN;
    bool chamber_rejection_latched_ = false;
    bool chamber_wait_active_ = false;
    double chamber_wait_target_c_ = NAN;
    int chamber_target_at_rejection_ = 0; ///< heater target (deci) when the latch set
    void apply_chamber_rejection(const chamber::ChamberHeaterDiagnostics& d,
                                 std::string_view backend_id);
    void release_chamber_rejection_if_target_followed();
    void clear_chamber_rejection();
    SubjectLifetime chamber_heater_fault_lifetime_;
    SubjectLifetime chamber_heater_inhibited_lifetime_;
    /// The appliance drops a poll periodically and recovers within a frame or
    /// two, so asserting offline on the first connected:false flashes an alarm
    /// for a couple of seconds several times an hour. Offline is asserted only
    /// after this many consecutive reports; any connected report clears the
    /// run. Measured flap lengths on a U1 over an hour were 1, 1 and 2 polls
    /// about twenty minutes apart, so the bar sits clear of the longest seen
    /// rather than one sample above it. At the module's 2s poll this notices a
    /// real outage in about eight seconds, which costs nothing: an appliance
    /// that has actually gone is gone for minutes.
    static constexpr int kChamberOfflineConsecutiveReports = 4;
    int chamber_offline_run_ = 0;

    SubjectLifetime chamber_heater_externally_controlled_lifetime_;
    SubjectLifetime chamber_heater_offline_lifetime_;
    SubjectLifetime chamber_heater_fault_reason_text_lifetime_;
    SubjectLifetime chamber_filter_fan_on_lifetime_;
    SubjectLifetime chamber_filter_fan_requested_lifetime_;
    SubjectLifetime chamber_filter_fan_device_driven_lifetime_;
    SubjectLifetime chamber_heater_element_temp_text_lifetime_;
    SubjectLifetime chamber_filter_fan_percent_text_lifetime_;

    // Dynamic per-extruder tracking
    std::unordered_map<std::string, ExtruderInfo> extruders_;
    lv_subject_t extruder_version_{}; ///< Bumped when extruder list changes

    // Active extruder name (defaults to "extruder"). Follows the machine's
    // toolhead status; a surface that wants to show a different tool reads the
    // per-extruder subjects instead of these mirrors.
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
    int chamber_filter_fan_percent_ = -1;    ///< Last backend-reported fan speed, -1 = none yet
};

} // namespace helix
