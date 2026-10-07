// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * @file printer_discovery.h
 * @brief Single source of truth for all discovered printer hardware
 *
 * This class consolidates:
 * - Hardware lists (heaters, fans, sensors, leds, steppers) from MoonrakerClient
 * - Capability flags (has_qgl, has_probe, etc.) from PrinterCapabilities
 * - Macros from PrinterCapabilities
 * - AMS/MMU detection from PrinterCapabilities
 */

#include "ams_types.h"
#include "filament_database.h" // filament::DEFAULT_DIAMETER_MM
#include "openams_api.h"       // OpenAMS claims only a manager speaking its API
#include "printer_detector.h"  // For BuildVolume struct
#include "text_io.h"           // helix::text_io::to_upper

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix {

/// Describes one detected AMS/filament system
struct DetectedAmsSystem {
    AmsType type = AmsType::NONE;
    std::string name; // Human-readable: "Happy Hare", "AFC", "Tool Changer"
};

/// Which screws-tilt module a printer runs. Upstream [screws_tilt_adjust]
/// reports through SCREWS_TILT_CALCULATE console lines; the Snapmaker U1 ships
/// its own [auto_screws_tilt_adjust] whose results arrive as a status object.
enum class ScrewsTiltDialect {
    Standard,      ///< upstream [screws_tilt_adjust]
    SnapmakerAuto, ///< Snapmaker U1 [auto_screws_tilt_adjust]
};

class PrinterDiscovery {
  public:
    PrinterDiscovery() = default;

    /**
     * @brief Parse Klipper objects from printer.objects.list response
     *
     * Extracts all hardware components and capabilities from the object list.
     * This is the single entry point for hardware discovery.
     *
     * @param objects JSON array of object names from printer.objects.list
     */
    void parse_objects(const nlohmann::json& objects);

    /**
     * @brief Status fields that decide a claim the object list cannot
     *
     * Empty unless a filament system is present by name only and has to show
     * a supported API before it may claim the printer. The discovery sequence
     * queries these (a `printer.objects.query` "objects" map) before the
     * hardware callback and hands the reply to settle_status_claims().
     */
    [[nodiscard]] nlohmann::json claim_status_query() const {
        nlohmann::json query = nlohmann::json::object();
        if (has_openams_manager_ && !has_mmu_) {
            query[openams::kManagerObject] = nlohmann::json::array({"api_version", "schema"});
        }
        return query;
    }

    /**
     * @brief Finish the claims claim_status_query() left open
     *
     * @param status The `status` object of the query's reply; empty when the
     *               query failed, which settles every open claim as unclaimed.
     */
    void settle_status_claims(const nlohmann::json& status);

  private:
    /// Fill detected_ams_systems_ from the flags the scan (and any settled
    /// status claim) left behind.
    void register_detected_ams_systems();

  public:
    /**
     * @brief Parse configfile keys to detect accelerometers
     *
     * Klipper's objects/list only returns objects with get_status() methods.
     * Accelerometer modules (adxl345, lis2dw, mpu9250, resonance_tester) don't
     * have get_status() since they're on-demand calibration tools.
     * Must check configfile instead.
     *
     * @param config JSON object from configfile.config response
     */
    void parse_config_keys(const nlohmann::json& config);

    /**
     * @brief Derive the build volume from the configfile [stepper_*] sections
     *
     * The X/Y travel limits and the Z height are the most discriminating
     * evidence PrinterDetector has: 63 of the 94 database entries carry a
     * build_volume_range heuristic. configfile.settings reports the resolved
     * value for every stepper key, so this is available as soon as the
     * configfile query returns — long before the safety-limits fetch that is
     * the other writer of this field.
     *
     * Every field is presence-checked: Klipper emits nulls for keys a printer
     * does not define, and a bare .get<float>() on one throws type_error.302.
     *
     * @param settings JSON object from a configfile.settings response
     * @return true when a usable X or Y extent was found and stored
     */
    bool parse_build_volume(const nlohmann::json& settings);

    /**
     * @brief Reset all discovered hardware to initial state
     *
     * @note This clears ALL fields including printer info (hostname, versions, etc).
     *       When using parse_objects(), call printer info setters AFTER parse_objects()
     *       since it calls clear() internally.
     */
    void clear();

    // ========================================================================
    // Hardware Lists
    // ========================================================================

    [[nodiscard]] const std::vector<std::string>& heaters() const {
        return heaters_;
    }

    [[nodiscard]] const std::vector<std::string>& fans() const {
        return fans_;
    }

    [[nodiscard]] const std::vector<std::string>& load_cells() const {
        return load_cells_;
    }

    [[nodiscard]] const std::vector<std::string>& sensors() const {
        return sensors_;
    }

    [[nodiscard]] const std::vector<std::string>& leds() const {
        return leds_;
    }

    [[nodiscard]] const std::vector<std::string>& steppers() const {
        return steppers_;
    }

    // ========================================================================
    // Capability Flags
    // ========================================================================

    [[nodiscard]] bool has_qgl() const {
        return has_qgl_;
    }

    [[nodiscard]] bool has_z_tilt() const {
        return has_z_tilt_;
    }

    [[nodiscard]] bool has_bed_mesh() const {
        return has_bed_mesh_;
    }

    [[nodiscard]] bool has_probe() const {
        return has_probe_;
    }

    [[nodiscard]] bool has_heater_bed() const {
        return has_heater_bed_;
    }

    [[nodiscard]] bool has_mmu() const {
        return has_mmu_;
    }

    [[nodiscard]] bool has_snapmaker() const {
        return has_snapmaker_;
    }

    [[nodiscard]] bool has_tool_changer() const {
        return has_tool_changer_;
    }

    /// Whether the firmware ships AUTO_FEEDING_BATCH, the macro that wraps a
    /// multi-head feed with target snapshot/restore and next-head preheat.
    /// Absent on firmware before 1.6; the batch path falls back to bare
    /// AUTO_FEEDING per head when this is false.
    [[nodiscard]] bool has_auto_feeding_batch() const {
        return has_auto_feeding_batch_;
    }

    /// Whether a pin_watch dock-sensor extra is configured.
    [[nodiscard]] bool has_pin_watch() const {
        return has_pin_watch_;
    }

    /// An `AFC_unit` object is present, which only PAXX's AFC-Lite stub
    /// publishes. A plain fact about the object list; what it means for backend
    /// selection is decided alongside has_snapmaker().
    [[nodiscard]] bool has_afc_lite() const {
        return has_afc_lite_;
    }

    /// Full Klipper object name of the pin_watch section (e.g. "pin_watch io"),
    /// for subscribing to it. Empty when there is none.
    /// The [medusahc] object exists. A plain fact about the object list; what
    /// it means is helix::toolchanger_addon's business.
    [[nodiscard]] bool has_medusahc() const {
        return has_medusahc_;
    }

    /// The object's name as reported, for subscribing to it.
    [[nodiscard]] const std::string& medusahc_object_name() const {
        return medusahc_object_name_;
    }

    [[nodiscard]] const std::string& pin_watch_object_name() const {
        return pin_watch_object_name_;
    }

    [[nodiscard]] bool has_chamber_heater() const {
        return has_chamber_heater_;
    }

    [[nodiscard]] bool has_chamber_sensor() const {
        return has_chamber_sensor_;
    }

    [[nodiscard]] const std::string& chamber_sensor_name() const {
        return chamber_sensor_name_;
    }

    [[nodiscard]] const std::string& chamber_heater_name() const {
        return chamber_heater_name_;
    }

    [[nodiscard]] const std::string& chamber_heater_object_name() const {
        return chamber_heater_object_name_;
    }

    /// Matched chamber-heater backend id ("" when no chamber heater).
    [[nodiscard]] const std::string& chamber_heater_backend_id() const {
        return chamber_heater_backend_id_;
    }
    /// Diagnostics status object bound by the backend ("" when none).
    [[nodiscard]] const std::string& chamber_diagnostics_object() const {
        return chamber_diagnostics_object_;
    }
    /// Filter-fan output_pin bound by the backend ("" when none).
    [[nodiscard]] const std::string& chamber_filter_fan_pin() const {
        return chamber_filter_fan_pin_;
    }

    /// Full object name of the chamber cooling temperature_fan (empty if none).
    /// Recorded independent of the heater pick — see chamber_cooling_fan_name_.
    [[nodiscard]] const std::string& chamber_cooling_fan_name() const {
        return chamber_cooling_fan_name_;
    }

    /// Cooling fan's configured resting/off target in decidegrees (×10), or 0 if
    /// unknown. Read from configfile.settings[<fan>].target_temp during discovery.
    /// `M141 S0` resets the cooling fan to this value, so recognizing it lets the
    /// chamber report Off instead of misreading the resting target as Maintaining.
    void set_chamber_fan_resting_deci(int deci) {
        chamber_fan_resting_deci_ = deci;
    }
    [[nodiscard]] int chamber_fan_resting_deci() const {
        return chamber_fan_resting_deci_;
    }

    /// Record a fan's configured `max_power` (from configfile.settings), keyed by
    /// lowercased object name. Klipper reports `speed` scaled by this value, so
    /// PrinterFanState divides it back out to display the logical fraction
    /// (matching Mainsail). See PrinterFanState::normalize_speed().
    void set_fan_max_power(const std::string& object_name, double max_power) {
        fan_max_power_[object_name] = max_power;
    }
    [[nodiscard]] const std::unordered_map<std::string, double>& fan_max_power() const {
        return fan_max_power_;
    }

    [[nodiscard]] bool has_led() const {
        return has_led_;
    }

    [[nodiscard]] const std::vector<std::string>& led_effects() const {
        return led_effects_;
    }

    [[nodiscard]] bool has_led_effects() const {
        return has_led_effects_;
    }

    [[nodiscard]] const std::vector<std::string>& led_macros() const {
        return led_macros_;
    }

    [[nodiscard]] bool has_led_macros() const {
        return !led_macros_.empty();
    }

    [[nodiscard]] bool has_accelerometer() const {
        return has_accelerometer_;
    }

    [[nodiscard]] bool has_filament_sensors() const {
        return !filament_sensor_names_.empty();
    }

    [[nodiscard]] bool has_firmware_retraction() const {
        return has_firmware_retraction_;
    }

    [[nodiscard]] bool has_timelapse() const {
        return has_timelapse_;
    }

    [[nodiscard]] bool has_exclude_object() const {
        return has_exclude_object_;
    }

    [[nodiscard]] bool has_screws_tilt() const {
        return has_screws_tilt_;
    }

    /// Which screws-tilt module backs the capability. Standard whenever
    /// upstream [screws_tilt_adjust] is configured, alone or alongside
    /// Snapmaker's — SCREWS_TILT_CALCULATE works and is the simpler path.
    /// SnapmakerAuto only when the U1's module is the sole one. Also the
    /// answer for a printer with neither module: the dialect is meaningless
    /// until the capability exists, and must not claim the U1's.
    [[nodiscard]] ScrewsTiltDialect screws_tilt_dialect() const {
        return (has_snapmaker_auto_screws_tilt_ && !has_standard_screws_tilt_)
                   ? ScrewsTiltDialect::SnapmakerAuto
                   : ScrewsTiltDialect::Standard;
    }

    [[nodiscard]] bool has_klippain_shaketune() const {
        return has_klippain_shaketune_;
    }

    [[nodiscard]] bool has_speaker() const {
        return has_speaker_;
    }

    [[nodiscard]] bool has_fan_feedback() const {
        return has_fan_feedback_;
    }

    /**
     * @brief Check if connected printer runs Kalico (Klipper fork with MPC support)
     *
     * Detected from printer.info "app" field returning "Kalico".
     */
    [[nodiscard]] bool is_kalico() const {
        return is_kalico_;
    }

    /**
     * @brief Set Kalico detection flag
     * @param kalico true if printer.info reports app as "Kalico"
     */
    void set_is_kalico(bool kalico) {
        is_kalico_ = kalico;
    }

    [[nodiscard]] bool supports_leveling() const {
        return has_qgl() || has_z_tilt() || has_bed_mesh();
    }

    [[nodiscard]] bool supports_chamber() const {
        return has_chamber_heater() || has_chamber_sensor();
    }

    // ========================================================================
    // AMS/MMU Detection
    // ========================================================================

    [[nodiscard]] AmsType mmu_type() const {
        return mmu_type_;
    }

    /// @brief Alias for mmu_type() - compatibility with PrinterCapabilities API
    [[nodiscard]] AmsType get_mmu_type() const {
        return mmu_type_;
    }

    /// @brief Physical slot count for a detected QIDI Box (0 if none).
    ///
    /// Derived from the number of `box_stepper slot<N>` objects in
    /// printer.objects.list. Values 4/8/12/16 for 1-4 boxes chained.
    /// Returns 0 when mmu_type() != QIDI_BOX.
    [[nodiscard]] int qidi_box_slot_count() const {
        return qidi_box_slot_count_;
    }

    /// @brief All detected AMS/filament systems (may include multiple backends)
    [[nodiscard]] const std::vector<DetectedAmsSystem>& detected_ams_systems() const {
        return detected_ams_systems_;
    }

    [[nodiscard]] const std::vector<std::string>& afc_lane_names() const {
        return afc_lane_names_;
    }

    /// @brief Alias for afc_lane_names() - compatibility with PrinterCapabilities API
    [[nodiscard]] const std::vector<std::string>& get_afc_lane_names() const {
        return afc_lane_names_;
    }

    [[nodiscard]] const std::vector<std::string>& afc_hub_names() const {
        return afc_hub_names_;
    }

    /// @brief Alias for afc_hub_names() - compatibility with PrinterCapabilities API
    [[nodiscard]] const std::vector<std::string>& get_afc_hub_names() const {
        return afc_hub_names_;
    }

    [[nodiscard]] const std::vector<std::string>& afc_unit_object_names() const {
        return afc_unit_object_names_;
    }

    [[nodiscard]] const std::vector<std::string>& afc_buffer_names() const {
        return afc_buffer_names_;
    }

    [[nodiscard]] const std::vector<std::string>& tool_names() const {
        return tool_names_;
    }

    /// @brief Alias for tool_names() - compatibility with PrinterCapabilities API
    [[nodiscard]] const std::vector<std::string>& get_tool_names() const {
        return tool_names_;
    }

    [[nodiscard]] const std::vector<std::string>& filament_sensor_names() const {
        return filament_sensor_names_;
    }

    /// @brief Alias for filament_sensor_names() - compatibility with PrinterCapabilities API
    [[nodiscard]] const std::vector<std::string>& get_filament_sensor_names() const {
        return filament_sensor_names_;
    }

    [[nodiscard]] const std::vector<std::string>& width_sensor_objects() const {
        return width_sensor_objects_;
    }

    [[nodiscard]] bool has_width_sensors() const {
        return !width_sensor_objects_.empty();
    }

    [[nodiscard]] const std::vector<std::string>& mmu_encoder_names() const {
        return mmu_encoder_names_;
    }

    /// @brief Klipper object names of every detected ACE unit.
    ///
    /// One of `filament_hub` (native GoKlipper), `ace` (community drivers), or
    /// one-or-more `ace_instance_N` (Kobra S1 mainline-Python fork, #1107).
    /// The discovery sequence subscribes each of these so the ACE backend gets
    /// live status. Empty when mmu_type() != AmsType::ACE.
    [[nodiscard]] const std::vector<std::string>& ace_object_names() const {
        return ace_object_names_;
    }

    [[nodiscard]] const std::vector<std::string>& mmu_servo_names() const {
        return mmu_servo_names_;
    }

    // ========================================================================
    // Macro Detection
    // ========================================================================

    [[nodiscard]] const std::unordered_set<std::string>& macros() const {
        return macros_;
    }

    /// @brief Alias for macros() - compatibility with PrinterCapabilities API
    [[nodiscard]] const std::unordered_set<std::string>& get_macros() const {
        return macros_;
    }

    /**
     * @brief Check if a macro exists (case-insensitive)
     * @param name Macro name to check
     * @return true if the macro exists
     */
    [[nodiscard]] bool has_macro(const std::string& name) const {
        return macros_.count(helix::text_io::to_upper(name)) > 0;
    }

    /**
     * @brief The macro's name AS WRITTEN in printer.cfg, given any casing
     *
     * has_macro() is deliberately case-insensitive, because the gcode command a
     * macro registers is its uppercased alias and that is what users type. Two
     * things are NOT uppercased, though, and both bite silently:
     *
     *   - the status object key, which is the config section verbatim
     *     ("gcode_macro Tool_Offset"), so a subscription or a status lookup
     *     spelled in caps simply never matches; and
     *   - SET_GCODE_VARIABLE's MACRO= value, registered as a mux key on the
     *     config-case name (klippy/extras/gcode_macro.py registers `name`, not
     *     `self.alias`), so a capitalised MACRO= is rejected outright.
     *
     * Anything naming a macro to Klipper rather than calling it must go through
     * here. Empty when no such macro exists.
     */
    [[nodiscard]] std::string macro_config_name(const std::string& name) const {
        auto it = macro_config_names_.find(helix::text_io::to_upper(name));
        return it == macro_config_names_.end() ? std::string{} : it->second;
    }

    /// Record the macros whose bodies reach SAVE_CONFIG / FIRMWARE_RESTART and
    /// friends, directly or through other macros. Computed from
    /// configfile.settings during discovery by
    /// helix::analyze_host_restarting_macros(); stored uppercased like macros_.
    void set_host_restarting_macros(std::unordered_set<std::string> macros) {
        host_restarting_macros_ = std::move(macros);
    }

    /// Whether running this macro takes the host down with it. A name-only
    /// check misses the wrapped case (ZMOD's AUTO_FULL_BED_LEVEL reaches
    /// SAVE_CONFIG two levels down), which is why confirmations ask this.
    [[nodiscard]] bool macro_restarts_host(const std::string& name) const {
        return host_restarting_macros_.count(helix::text_io::to_upper(name)) > 0;
    }

    [[nodiscard]] const std::unordered_set<std::string>& host_restarting_macros() const {
        return host_restarting_macros_;
    }

    /**
     * @brief Read the filament diameter from [extruder] filament_diameter
     *
     * A missing, null or non-positive value leaves the current diameter.
     *
     * @param settings JSON object from a configfile.settings response
     */
    void parse_filament_diameter(const nlohmann::json& settings) {
        const auto extruder = settings.find("extruder");
        if (extruder == settings.end() || !extruder->is_object()) {
            return;
        }
        const auto d = extruder->find("filament_diameter");
        if (d != extruder->end() && d->is_number() && d->get<float>() > 0.0f) {
            filament_diameter_mm_ = d->get<float>();
        }
    }

    /// Filament diameter in mm the extruder is configured for.
    [[nodiscard]] float filament_diameter_mm() const {
        return filament_diameter_mm_;
    }

    /**
     * @brief Resolve the command that toggles a filament sensor in firmware
     *
     * A [gcode_macro SET_FILAMENT_SENSOR] wrapper must rename the builtin
     * (rename_existing), and a wrapper may treat every call as a user setting
     * and persist it. HelixScreen's toggles are temporary firmware state, not
     * user settings, so they go to the builtin under its renamed name and skip
     * the wrapper's side effects on purpose.
     *
     * @param settings JSON object from a configfile.settings response
     * @return true when a wrapper's rename_existing was found and stored
     */
    bool parse_sensor_toggle_command(const nlohmann::json& settings);

    /// The firmware command for SENSOR=<name> ENABLE=<0|1>. See
    /// parse_sensor_toggle_command().
    [[nodiscard]] std::string sensor_toggle_command() const {
        return sensor_toggle_command_.empty() ? std::string("SET_FILAMENT_SENSOR")
                                              : sensor_toggle_command_;
    }

    /// Macros reaching a command that leaves the host DOWN, from
    /// helix::analyze_host_halting_macros(); stored uppercased like macros_.
    void set_host_halting_macros(std::unordered_set<std::string> macros) {
        host_halting_macros_ = std::move(macros);
    }

    /// Does this macro reach M112/SHUTDOWN/EMERGENCY_STOP, directly or through
    /// another macro? Separate from macro_restarts_host() because the two differ
    /// in what the user is promised once the rpc comes back dropped.
    [[nodiscard]] bool macro_halts_host(const std::string& name) const {
        return host_halting_macros_.count(helix::text_io::to_upper(name)) > 0;
    }

    [[nodiscard]] const std::unordered_set<std::string>& host_halting_macros() const {
        return host_halting_macros_;
    }

    [[nodiscard]] std::string nozzle_clean_macro() const {
        return nozzle_clean_macro_;
    }

    /// @brief Alias for nozzle_clean_macro() - compatibility with PrinterCapabilities API
    [[nodiscard]] std::string get_nozzle_clean_macro() const {
        return nozzle_clean_macro_;
    }

    [[nodiscard]] std::string purge_line_macro() const {
        return purge_line_macro_;
    }

    /// @brief Alias for purge_line_macro() - compatibility with PrinterCapabilities API
    [[nodiscard]] std::string get_purge_line_macro() const {
        return purge_line_macro_;
    }

    [[nodiscard]] std::string heat_soak_macro() const {
        return heat_soak_macro_;
    }

    /// @brief Alias for heat_soak_macro() - compatibility with PrinterCapabilities API
    [[nodiscard]] std::string get_heat_soak_macro() const {
        return heat_soak_macro_;
    }

    [[nodiscard]] bool has_nozzle_clean_macro() const {
        return !nozzle_clean_macro_.empty();
    }

    [[nodiscard]] bool has_purge_line_macro() const {
        return !purge_line_macro_.empty();
    }

    [[nodiscard]] bool has_heat_soak_macro() const {
        return !heat_soak_macro_.empty();
    }

    /**
     * @brief Get detected HelixScreen helper macros
     * @return Set of HELIX_* macro names
     */
    [[nodiscard]] const std::unordered_set<std::string>& helix_macros() const {
        return helix_macros_;
    }

    /**
     * @brief Check if HelixScreen helper macros are installed
     * @return true if any HELIX_* macros were detected
     */
    [[nodiscard]] bool has_helix_macros() const {
        return !helix_macros_.empty();
    }

    /**
     * @brief Check if a specific HelixScreen helper macro exists
     * @param macro_name Full macro name (e.g., "HELIX_BED_MESH_IF_NEEDED")
     * @return true if macro was detected
     */
    [[nodiscard]] bool has_helix_macro(const std::string& macro_name) const {
        return helix_macros_.count(helix::text_io::to_upper(macro_name)) > 0;
    }

    /**
     * @brief Get total number of detected macros
     */
    [[nodiscard]] size_t macro_count() const {
        return macros_.size();
    }

    // ========================================================================
    // Printer Info (populated from server.info / printer.info)
    // ========================================================================

    /**
     * @brief Set printer hostname from printer.info
     */
    void set_hostname(const std::string& hostname) {
        hostname_ = hostname;
    }

    [[nodiscard]] const std::string& hostname() const {
        return hostname_;
    }

    /**
     * @brief Set Klipper software version from printer.info
     */
    void set_software_version(const std::string& version) {
        software_version_ = version;
    }

    [[nodiscard]] const std::string& software_version() const {
        return software_version_;
    }

    /**
     * @brief Set Moonraker version from server.info
     */
    void set_moonraker_version(const std::string& version) {
        moonraker_version_ = version;
    }

    [[nodiscard]] const std::string& moonraker_version() const {
        return moonraker_version_;
    }

    /**
     * @brief Set kinematics type from toolhead subscription
     */
    void set_kinematics(const std::string& kinematics) {
        kinematics_ = kinematics;
    }

    [[nodiscard]] const std::string& kinematics() const {
        return kinematics_;
    }

    /**
     * @brief Set build volume from bed_mesh bounds
     */
    void set_build_volume(const BuildVolume& volume) {
        build_volume_ = volume;
    }

    [[nodiscard]] const BuildVolume& build_volume() const {
        return build_volume_;
    }

    /**
     * @brief Set primary MCU chip type
     */
    void set_mcu(const std::string& mcu) {
        mcu_ = mcu;
    }

    [[nodiscard]] const std::string& mcu() const {
        return mcu_;
    }

    /**
     * @brief Set all MCU chip types (primary + secondary)
     */
    void set_mcu_list(const std::vector<std::string>& mcu_list) {
        mcu_list_ = mcu_list;
    }

    [[nodiscard]] const std::vector<std::string>& mcu_list() const {
        return mcu_list_;
    }

    /**
     * @brief Set OS distribution name from machine.system_info
     */
    void set_os_version(const std::string& os_version) {
        os_version_ = os_version;
    }

    [[nodiscard]] const std::string& os_version() const {
        return os_version_;
    }

    /**
     * @brief Set host CPU architecture from machine.system_info
     */
    void set_cpu_arch(const std::string& cpu_arch) {
        cpu_arch_ = cpu_arch;
    }

    [[nodiscard]] const std::string& cpu_arch() const {
        return cpu_arch_;
    }

    /**
     * @brief Set MCU version strings (name→version pairs)
     * e.g., {"mcu", "v0.12.0-108-..."}, {"mcu EBBCan", "v0.12.0-..."}
     */
    void set_mcu_versions(const std::vector<std::pair<std::string, std::string>>& mcu_versions) {
        mcu_versions_ = mcu_versions;
    }

    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& mcu_versions() const {
        return mcu_versions_;
    }

    /**
     * @brief Set all printer objects from Klipper
     */
    void set_printer_objects(const std::vector<std::string>& objects) {
        printer_objects_ = objects;
    }

    [[nodiscard]] const std::vector<std::string>& printer_objects() const {
        return printer_objects_;
    }

    /// True once parse_objects() consumed an objects list: the hardware lists
    /// above are then complete, and an empty one means the printer has none.
    [[nodiscard]] bool objects_reported() const {
        return objects_reported_;
    }

  private:
    // Helper: natural sort — splits on trailing digits so "lane2" < "lane10"
    static void natural_sort(std::vector<std::string>& names);

    // Helper: true when `needle` appears in `name` bounded by underscores or the
    // ends of the string. Klipper macro names are underscore-separated words, so
    // a plain substring test matches far too much: "PID" hits LED_RAPID_FLASH and
    // "HOME" hits STATUS_LED_HOMING, dropping LED macros that are perfectly valid.
    static bool contains_word(const std::string& name, const std::string& needle);

    // Helper: check if name matches any pattern
    static bool matches_any(const std::string& name, const std::vector<std::string>& patterns) {
        for (const auto& pattern : patterns) {
            if (name == pattern) {
                return true;
            }
        }
        return false;
    }

    // Hardware lists
    std::vector<std::string> heaters_;
    std::vector<std::string> fans_;
    std::vector<std::string> load_cells_;
    std::vector<std::string> sensors_;
    std::vector<std::string> leds_;
    std::vector<std::string> steppers_;

    // AMS/MMU discovery
    std::vector<std::string> afc_lane_names_;
    std::vector<std::string> afc_hub_names_;
    std::vector<std::string>
        afc_unit_object_names_; // Full names: "AFC_BoxTurtle Turtle_1", "AFC_OpenAMS AMS_1"
    std::vector<std::string> afc_buffer_names_; // Buffer suffixes: "TN", "TN1", etc.
    std::vector<std::string> tool_names_;
    std::vector<std::string> filament_sensor_names_;
    std::vector<std::string> width_sensor_objects_;
    std::vector<std::string> mmu_encoder_names_;
    std::vector<std::string> mmu_servo_names_;
    std::vector<std::string> ace_object_names_; // filament_hub / ace / ace_instance_N (#1107)

    // Macros
    std::unordered_set<std::string> macros_;
    /// UPPERCASE macro name -> the name as written in printer.cfg. See
    /// macro_config_name().
    std::unordered_map<std::string, std::string> macro_config_names_;
    std::unordered_set<std::string> host_restarting_macros_; ///< Macros that reach a host restart
    std::unordered_set<std::string> host_halting_macros_;    ///< Macros that reach a host halt
    std::string sensor_toggle_command_; ///< Empty = the SET_FILAMENT_SENSOR builtin
    std::unordered_set<std::string> helix_macros_;
    std::string nozzle_clean_macro_;
    std::string purge_line_macro_;
    std::string heat_soak_macro_;

    // Capability flags
    bool has_qgl_ = false;
    bool has_z_tilt_ = false;
    bool has_bed_mesh_ = false;
    bool has_probe_ = false;
    bool has_heater_bed_ = false;
    bool has_mmu_ = false;
    bool has_openams_manager_ = false; ///< oams_manager listed; claim settled from its status
    bool has_snapmaker_ = false;
    bool has_afc_lite_ = false;
    bool has_tool_changer_ = false;
    bool has_auto_feeding_batch_ = false;
    bool has_pin_watch_ = false;
    std::string pin_watch_object_name_;
    bool has_medusahc_ = false;
    std::string medusahc_object_name_;
    bool has_chamber_heater_ = false;
    bool has_chamber_sensor_ = false;
    std::string chamber_sensor_name_;
    std::string chamber_heater_name_;        ///< Full object name (e.g., "heater_generic chamber")
    std::string chamber_heater_object_name_; ///< Object name only (e.g., "chamber")
    std::string chamber_heater_backend_id_;  ///< "" none, "generic", appliance id
    std::string chamber_diagnostics_object_; ///< status object with diagnostics, "" none
    std::string chamber_filter_fan_pin_;     ///< binary filter fan output_pin, "" none
    std::string chamber_cooling_fan_name_;   ///< Full object name of the chamber temperature_fan
                                             ///< (e.g., "temperature_fan chamber_fan"). Recorded
                                             ///< independent of the heater pick: in COOLING mode
                                             ///< the K2 M141 macro parks the setpoint on this fan's
                                             ///< target, not the heater's.
    float filament_diameter_mm_ = filament::DEFAULT_DIAMETER_MM; ///< [extruder] filament_diameter
    int chamber_fan_resting_deci_ = 0; ///< Cooling fan's configured resting/off target
                                       ///< (decidegrees), from configfile.settings
                                       ///< target_temp. 0 = unknown. M141 S0 returns here.
    std::unordered_map<std::string, double> fan_max_power_; ///< Per-fan max_power from
                                                            ///< configfile.settings, keyed by
                                                            ///< lowercased object name. Used to
                                                            ///< normalize reported fan speeds.
    bool has_led_ = false;
    std::vector<std::string> led_effects_;
    bool has_led_effects_ = false;
    std::vector<std::string> led_macros_;
    bool has_accelerometer_ = false;
    bool has_firmware_retraction_ = false;
    bool has_timelapse_ = false;
    bool has_exclude_object_ = false;
    bool has_screws_tilt_ = false;
    bool has_standard_screws_tilt_ = false;       ///< upstream [screws_tilt_adjust]
    bool has_snapmaker_auto_screws_tilt_ = false; ///< U1 [auto_screws_tilt_adjust]
    int qidi_box_slot_count_ = 0; ///< Count of `box_stepper slot<N>` objects (QIDI Box)
    bool has_klippain_shaketune_ = false;
    bool has_speaker_ = false;
    bool has_fan_feedback_ = false;
    bool is_kalico_ = false;
    AmsType mmu_type_ = AmsType::NONE;
    std::vector<DetectedAmsSystem> detected_ams_systems_;

    // Printer info (from server.info / printer.info)
    std::string hostname_;
    std::string software_version_;
    std::string moonraker_version_;
    std::string os_version_;
    std::string cpu_arch_;
    std::string kinematics_;
    BuildVolume build_volume_;
    std::string mcu_;
    std::vector<std::string> mcu_list_;
    std::vector<std::pair<std::string, std::string>> mcu_versions_;
    std::vector<std::string> printer_objects_;
    bool objects_reported_ = false;
};

} // namespace helix

// Forward declarations for init_subsystems_from_hardware (global scope)
class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
class PrinterState;
} // namespace helix

namespace helix {

/**
 * @brief Initialize subsystems from hardware discovery
 *
 * Initializes AMS backend, filament sensor manager, and standard macros
 * based on discovered hardware.
 *
 * @param hardware Hardware discovery results
 * @param api IMoonrakerAPI instance
 * @param client MoonrakerClient instance
 */
void init_subsystems_from_hardware(const PrinterDiscovery& hardware, IMoonrakerAPI* api,
                                   IMoonrakerClient* client);

/**
 * @brief The objects TemperatureSensorManager tracks: temperature_sensor /
 *        temperature_fan / tmc objects plus every heater_generic (filament
 *        dryers, auxiliary heaters). Extruders and the bed are PrinterState's.
 */
std::vector<std::string> temperature_sensor_objects(const PrinterDiscovery& hardware);

/**
 * @brief Case-insensitive search of Klipper object names
 *
 * A leading '^' pins the pattern to the start of a name and a trailing '$'
 * to its end, so "^box$" names exactly the object "box" and not a
 * "gcode_macro BOX_UNLOAD" that contains it. Shared by detection heuristics
 * (object_exists) and probe-preparation rules.
 */
[[nodiscard]] bool has_pattern(const std::vector<std::string>& objects, const std::string& pattern);

} // namespace helix
