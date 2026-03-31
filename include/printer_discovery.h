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
#include "printer_detector.h" // For BuildVolume struct

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>
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
    void parse_objects(const nlohmann::json& objects) {
        clear();

        // Validate input is an array
        if (!objects.is_array()) {
            spdlog::warn("[PrinterDiscovery] parse_objects: input is not an array");
            return;
        }

        // Debug dump: pretty-print all discovered objects
        spdlog::debug("[PrinterDiscovery] parse_objects: received {} objects", objects.size());
        if (spdlog::should_log(spdlog::level::debug)) {
            try {
                // Dump objects as a formatted list for easy reading
                std::string obj_list;
                for (const auto& obj : objects) {
                    if (obj.is_string()) {
                        obj_list += "\n  - " + obj.get<std::string>();
                    }
                }
                if (!obj_list.empty()) {
                    spdlog::debug("[PrinterDiscovery] Objects list:{}\n", obj_list);
                }
            } catch (const std::exception& e) {
                spdlog::debug("[PrinterDiscovery] Failed to format objects: {}", e.what());
            }
        }

        // AFC_stepper names collected separately — only used as lane source when
        // no AFC_lane objects exist (Box Turtle compat). Vivid uses AFC_stepper for
        // motor components (drive/selector), not lanes.
        std::vector<std::string> afc_stepper_names;

        for (const auto& obj : objects) {
            // Skip non-string elements
            if (!obj.is_string()) {
                continue;
            }
            std::string name = obj.template get<std::string>();

            // Skip empty strings
            if (name.empty()) {
                continue;
            }

            std::string upper_name = to_upper(name);

            // ================================================================
            // Steppers (stepper_x, stepper_y, stepper_z, stepper_z1, etc.)
            // ================================================================
            if (name.rfind("stepper_", 0) == 0) {
                steppers_.push_back(name);
            }
            // ================================================================
            // Heaters: extruders, heater_bed, heater_generic
            // ================================================================
            // Match "extruder", "extruder1", etc., but NOT "extruder_stepper"
            else if (name.rfind("extruder", 0) == 0 && name.rfind("extruder_stepper", 0) != 0) {
                heaters_.push_back(name);
            }
            // Heated bed
            else if (name == "heater_bed") {
                heaters_.push_back(name);
                has_heater_bed_ = true;
            }
            // Generic heaters (e.g., "heater_generic chamber")
            else if (name.rfind("heater_generic ", 0) == 0) {
                heaters_.push_back(name);
                // Check for chamber heater
                std::string heater_name = name.substr(15); // Remove "heater_generic " prefix
                if (to_upper(heater_name).find("CHAMBER") != std::string::npos) {
                    has_chamber_heater_ = true;
                    chamber_heater_name_ = name;
                    chamber_heater_object_name_ = heater_name;
                }
            }
            // ================================================================
            // Sensors: temperature_sensor, temperature_fan (dual-purpose)
            // ================================================================
            else if (name.rfind("temperature_sensor ", 0) == 0) {
                sensors_.push_back(name);
                // Check for chamber sensor
                std::string sensor_name = name.substr(19); // Remove "temperature_sensor " prefix
                if (to_upper(sensor_name).find("CHAMBER") != std::string::npos) {
                    has_chamber_sensor_ = true;
                    chamber_sensor_name_ = name;
                }
            }
            // Temperature-controlled fans (also act as sensors)
            else if (name.rfind("temperature_fan ", 0) == 0) {
                sensors_.push_back(name);
                fans_.push_back(name); // Also add to fans for control
                // Check for chamber temperature_fan
                std::string fan_name = name.substr(16); // Remove "temperature_fan " prefix
                if (to_upper(fan_name).find("CHAMBER") != std::string::npos) {
                    has_chamber_heater_ = true;
                    chamber_heater_name_ = name;
                    chamber_heater_object_name_ = fan_name;
                }
            }
            // TMC stepper drivers with built-in temperature (tmc2240, tmc5160)
            else if (name.rfind("tmc2240 ", 0) == 0 || name.rfind("tmc5160 ", 0) == 0) {
                sensors_.push_back(name);
            }
            // ================================================================
            // Fans: fan, heater_fan, fan_generic, controller_fan
            // ================================================================
            else if (name == "fan") {
                fans_.push_back(name);
            } else if (name.rfind("heater_fan ", 0) == 0) {
                fans_.push_back(name);
            } else if (name.rfind("fan_generic ", 0) == 0) {
                fans_.push_back(name);
            } else if (name.rfind("controller_fan ", 0) == 0) {
                fans_.push_back(name);
            }
            // ================================================================
            // LEDs: led_effect (must be before "led "), neopixel, dotstar, led
            // ================================================================
            // led_effect MUST be checked before "led " to avoid false match
            else if (name.rfind("led_effect ", 0) == 0) {
                led_effects_.push_back(name);
                has_led_effects_ = true;
            } else if (name.rfind("neopixel ", 0) == 0 || name == "neopixel") {
                leds_.push_back(name);
                has_led_ = true;
            } else if (name.rfind("dotstar ", 0) == 0 || name == "dotstar") {
                leds_.push_back(name);
                has_led_ = true;
            } else if (name.rfind("led ", 0) == 0) {
                leds_.push_back(name);
                has_led_ = true;
            }
            // Output pins - classify as fan, LED, or speaker based on name
            else if (name == "fan_feedback") {
                has_fan_feedback_ = true;
            } else if (name.rfind("output_pin ", 0) == 0) {
                std::string pin_name = name.substr(11); // Remove "output_pin " prefix
                std::string upper_pin = to_upper(pin_name);

                // Fan detection: name starts with "FAN" (e.g., fan0, fan1, fan2)
                if (upper_pin.rfind("FAN", 0) == 0) {
                    fans_.push_back(name);
                }
                // LED detection
                else if (upper_pin.find("LIGHT") != std::string::npos ||
                         upper_pin.find("LED") != std::string::npos ||
                         upper_pin.find("LAMP") != std::string::npos) {
                    leds_.push_back(name);
                    has_led_ = true;
                }
                // Speaker/buzzer detection for M300 support
                if (upper_pin.find("BEEPER") != std::string::npos ||
                    upper_pin.find("BUZZER") != std::string::npos ||
                    upper_pin.find("SPEAKER") != std::string::npos) {
                    has_speaker_ = true;
                }
            }
            // ================================================================
            // Capability flags
            // ================================================================
            else if (name == "quad_gantry_level") {
                has_qgl_ = true;
            } else if (name == "z_tilt") {
                has_z_tilt_ = true;
            } else if (name == "bed_mesh") {
                has_bed_mesh_ = true;
            } else if (name == "probe" || name == "bltouch" || name == "smart_effector" ||
                       name == "cartographer" || name == "beacon") {
                has_probe_ = true;
            } else if (name.rfind("probe_eddy_current ", 0) == 0) {
                has_probe_ = true;
            } else if (name == "firmware_retraction") {
                has_firmware_retraction_ = true;
            } else if (name == "timelapse") {
                has_timelapse_ = true;
            } else if (name == "exclude_object") {
                has_exclude_object_ = true;
            } else if (name == "screws_tilt_adjust") {
                has_screws_tilt_ = true;
            }
            // NOTE: screws_tilt_adjust may not appear in objects/list (no get_status()).
            // Also detected in parse_config_keys() as fallback.
            //
            // NOTE: Accelerometer detection removed from parse_objects().
            // Klipper's objects/list only returns objects with get_status() methods.
            // Accelerometers (adxl345, lis2dw, mpu9250, resonance_tester) intentionally
            // don't have get_status() since they're on-demand calibration tools.
            // Use parse_config_keys() instead to detect accelerometers from configfile.
            // ================================================================
            // MMU/AMS detection
            // ================================================================
            else if (name == "mmu") {
                has_mmu_ = true;
                mmu_type_ = AmsType::HAPPY_HARE;
            } else if (name == "AFC") {
                has_mmu_ = true;
                mmu_type_ = AmsType::AFC;
            }
            // CFS detection (Creality Filament System — K2 series)
            else if (name == "box") {
                has_mmu_ = true;
                mmu_type_ = AmsType::CFS;
            }
            // ACE detection (Anycubic ACE Pro — ValgACE/BunnyACE/DuckACE Klipper drivers)
            else if (name == "ace" && !has_mmu_) {
                has_mmu_ = true;
                mmu_type_ = AmsType::ACE;
                spdlog::info("[PrinterDiscovery] Detected ACE (Anycubic ACE Pro)");
            }
            // MMU encoder discovery (Happy Hare)
            else if (name.rfind("mmu_encoder ", 0) == 0) {
                std::string encoder_name = name.substr(12); // Remove "mmu_encoder " prefix
                if (!encoder_name.empty()) {
                    mmu_encoder_names_.push_back(encoder_name);
                }
            }
            // MMU servo discovery (Happy Hare)
            else if (name.rfind("mmu_servo ", 0) == 0) {
                std::string servo_name = name.substr(10); // Remove "mmu_servo " prefix
                if (!servo_name.empty()) {
                    mmu_servo_names_.push_back(servo_name);
                }
            }
            // AFC_stepper: may be lanes (Box Turtle) or motor components (Vivid).
            // Collected separately; only used as lanes if no AFC_lane objects exist.
            else if (name.rfind("AFC_stepper ", 0) == 0) {
                std::string stepper_name = name.substr(12); // Remove "AFC_stepper " prefix
                if (!stepper_name.empty()) {
                    afc_stepper_names.push_back(stepper_name);
                }
            }
            // AFC hub discovery
            else if (name.rfind("AFC_hub ", 0) == 0) {
                std::string hub_name = name.substr(8); // Remove "AFC_hub " prefix
                if (!hub_name.empty()) {
                    afc_hub_names_.push_back(hub_name);
                }
            }
            // AFC_lane discovery (authoritative lane source for Vivid, OpenAMS, etc.)
            else if (name.rfind("AFC_lane ", 0) == 0) {
                std::string lane_name = name.substr(9); // Remove "AFC_lane " prefix (9 chars)
                if (!lane_name.empty()) {
                    afc_lane_names_.push_back(lane_name);
                }
            }
            // AFC unit-level objects (BoxTurtle, OpenAMS, ViViD, NightOwl, etc.)
            // Any AFC_ object not matching known component prefixes is a unit type
            else if (name.rfind("AFC_", 0) == 0 && name.rfind("AFC_stepper ", 0) != 0 &&
                     name.rfind("AFC_hub ", 0) != 0 && name.rfind("AFC_extruder ", 0) != 0 &&
                     name.rfind("AFC_lane ", 0) != 0 && name.rfind("AFC_buffer ", 0) != 0 &&
                     name.rfind("AFC_led ", 0) != 0) {
                afc_unit_object_names_.push_back(name); // Store FULL name for Klipper queries
            }
            // AFC buffer objects
            else if (name.rfind("AFC_buffer ", 0) == 0) {
                std::string buffer_name = name.substr(11); // Remove "AFC_buffer " prefix (11 chars)
                if (!buffer_name.empty()) {
                    afc_buffer_names_.push_back(buffer_name);
                }
            }
            // AD5X IFS detection via ZMOD firmware sensors
            // Three sensor name patterns trigger detection:
            //   1. lessWaste plugin:  "filament_switch_sensor _ifs_port_sensor_N"
            //   2. Native ZMOD (old): "filament_motion_sensor _ifs_motion_sensor_N"
            //   3. Native ZMOD:       "filament_motion_sensor ifs_motion_sensor"
            else if (!has_mmu_ &&
                     (name.rfind("filament_switch_sensor _ifs_port_sensor_", 0) == 0 ||
                      name.rfind("filament_motion_sensor _ifs_motion_sensor_", 0) == 0 ||
                      name == "filament_motion_sensor ifs_motion_sensor")) {
                has_mmu_ = true;
                mmu_type_ = AmsType::AD5X_IFS;
            }
            // Snapmaker U1 detection — filament_detect is unique to U1 firmware
            else if (name == "filament_detect") {
                has_snapmaker_ = true;
            }
            // Tool changer detection
            else if (name == "toolchanger") {
                has_tool_changer_ = true;
            }
            // Tool object discovery
            else if (name.rfind("tool ", 0) == 0) {
                std::string tool_name = name.substr(5); // Remove "tool " prefix
                if (!tool_name.empty()) {
                    tool_names_.push_back(tool_name);
                }
            }
            // ================================================================
            // Width sensors (filament diameter measurement)
            // ================================================================
            else if (name == "hall_filament_width_sensor" || name == "hall_fila_switch" ||
                     name == "tsl1401cl_filament_width_sensor") {
                width_sensor_objects_.push_back(name);
            }
            // ================================================================
            // Filament sensors
            // ================================================================
            else if (name.rfind("filament_switch_sensor ", 0) == 0 ||
                     name.rfind("filament_motion_sensor ", 0) == 0) {
                filament_sensor_names_.push_back(name);
            }
            // ================================================================
            // Macro detection
            // ================================================================
            else if (name.rfind("gcode_macro ", 0) == 0) {
                std::string macro_name = name.substr(12); // Remove "gcode_macro " prefix
                std::string upper_macro = to_upper(macro_name);

                macros_.insert(upper_macro);

                // Check for HelixScreen helper macros
                if (upper_macro.rfind("HELIX_", 0) == 0) {
                    helix_macros_.insert(upper_macro);
                }

                // Check for Klippain Shake&Tune
                if (upper_macro == "AXES_SHAPER_CALIBRATION") {
                    has_klippain_shaketune_ = true;
                }

                // Check for common macro patterns and cache them
                if (nozzle_clean_macro_.empty()) {
                    static const std::vector<std::string> nozzle_patterns = {
                        "CLEAN_NOZZLE", "NOZZLE_WIPE", "WIPE_NOZZLE", "PURGE_NOZZLE",
                        "NOZZLE_CLEAN"};
                    if (matches_any(upper_macro, nozzle_patterns)) {
                        nozzle_clean_macro_ = macro_name;
                    }
                }

                if (purge_line_macro_.empty()) {
                    static const std::vector<std::string> purge_patterns = {
                        "PURGE_LINE", "PRIME_LINE", "INTRO_LINE", "LINE_PURGE"};
                    if (matches_any(upper_macro, purge_patterns)) {
                        purge_line_macro_ = macro_name;
                    }
                }

                if (heat_soak_macro_.empty()) {
                    static const std::vector<std::string> soak_patterns = {
                        "HEAT_SOAK", "CHAMBER_SOAK", "SOAK", "BED_SOAK"};
                    if (matches_any(upper_macro, soak_patterns)) {
                        heat_soak_macro_ = macro_name;
                    }
                }

                // LED macro auto-detection
                static const std::vector<std::string> led_keywords = {
                    "LIGHT", "LED", "LAMP", "ILLUMINAT", "BACKLIGHT", "NEON"};
                static const std::vector<std::string> led_exclusions = {
                    "PRINT_START", "PRINT_END",        "M600",       "BED_MESH",
                    "PAUSE",       "RESUME",           "CANCEL",     "HOME",
                    "QGL",         "Z_TILT",           "PROBE",      "CALIBRATE",
                    "PID",         "FIRMWARE_RESTART", "SAVE_CONFIG"};

                bool is_led_candidate = false;
                for (const auto& kw : led_keywords) {
                    if (upper_macro.find(kw) != std::string::npos) {
                        is_led_candidate = true;
                        break;
                    }
                }
                if (is_led_candidate) {
                    bool excluded = false;
                    for (const auto& ex : led_exclusions) {
                        if (upper_macro.find(ex) != std::string::npos) {
                            excluded = true;
                            break;
                        }
                    }
                    if (!excluded) {
                        led_macros_.push_back(upper_macro);
                    }
                }
            }
        }

        // AFC_stepper objects can be lanes (Box Turtle: "AFC_stepper lane0") or motor
        // components (Vivid: "AFC_stepper Vivid_1_drive"/"Vivid_1_selector").
        // When no AFC_lane objects exist, treat all steppers as lanes (pure Box Turtle).
        // When BOTH exist (e.g., Box Turtle + OpenAMS + ACE), merge stepper names
        // that look like lanes ("lane" prefix + digit) into the lane list.
        if (afc_lane_names_.empty() && !afc_stepper_names.empty()) {
            afc_lane_names_ = std::move(afc_stepper_names);
        } else if (!afc_lane_names_.empty() && !afc_stepper_names.empty()) {
            // Mixed setup: merge AFC_stepper lane names not already in AFC_lane list
            std::unordered_set<std::string> existing(afc_lane_names_.begin(),
                                                     afc_lane_names_.end());
            for (auto& name : afc_stepper_names) {
                if (name.rfind("lane", 0) == 0 && existing.find(name) == existing.end()) {
                    afc_lane_names_.push_back(std::move(name));
                }
            }
        }

        // Sort AFC lane names using natural sort (lane2 before lane10)
        if (!afc_lane_names_.empty()) {
            natural_sort(afc_lane_names_);
        }
        if (!afc_buffer_names_.empty()) {
            natural_sort(afc_buffer_names_);
        }

        // Sort tool names for consistent ordering
        if (!tool_names_.empty()) {
            std::sort(tool_names_.begin(), tool_names_.end());
        }

        // Collect all detected AMS systems
        detected_ams_systems_.clear();

        // Register the actual filament management backend. Only one AMS backend
        // can be active at a time on Klipper. When a real MMU (AFC, Happy Hare)
        // is present alongside klipper-toolchanger, prefer the MMU — toolchanger
        // just handles tool switching, not filament management.
        if (has_snapmaker_) {
            detected_ams_systems_.push_back({AmsType::SNAPMAKER, "Snapmaker"});
            mmu_type_ = AmsType::SNAPMAKER;
        } else if (has_mmu_) {
            if (mmu_type_ == AmsType::HAPPY_HARE) {
                detected_ams_systems_.push_back({AmsType::HAPPY_HARE, "Happy Hare"});
            } else if (mmu_type_ == AmsType::AFC) {
                detected_ams_systems_.push_back({AmsType::AFC, "AFC"});
            } else if (mmu_type_ == AmsType::AD5X_IFS) {
                detected_ams_systems_.push_back({AmsType::AD5X_IFS, "AD5X IFS"});
            } else if (mmu_type_ == AmsType::CFS) {
                detected_ams_systems_.push_back({AmsType::CFS, "CFS"});
            } else if (mmu_type_ == AmsType::ACE) {
                detected_ams_systems_.push_back({AmsType::ACE, "ACE"});
            }
        } else if (has_tool_changer_ && !tool_names_.empty()) {
            // Standalone tool changer with no MMU — show parallel topology
            detected_ams_systems_.push_back({AmsType::TOOL_CHANGER, "Tool Changer"});
            mmu_type_ = AmsType::TOOL_CHANGER;
        }

        // Debug summary: log detected hardware
        spdlog::debug("[PrinterDiscovery] Hardware discovery complete:");
        spdlog::debug(
            "  Heaters: {} (extruders: {}, bed: {}, generic: {})", heaters_.size(),
            std::count_if(heaters_.begin(), heaters_.end(),
                          [](const std::string& h) { return h.rfind("extruder", 0) == 0; }),
            has_heater_bed_ ? 1 : 0,
            std::count_if(heaters_.begin(), heaters_.end(),
                          [](const std::string& h) { return h.rfind("heater_generic ", 0) == 0; }));
        spdlog::debug("  Chamber: heater='{}' (detected: {}), sensor='{}' (detected: {})",
                      chamber_heater_name_, has_chamber_heater_ ? "yes" : "no",
                      chamber_sensor_name_, has_chamber_sensor_ ? "yes" : "no");
        spdlog::debug("  Sensors: {} (temp: {}, fans: {})", sensors_.size(),
                      std::count_if(sensors_.begin(), sensors_.end(),
                                    [](const std::string& s) {
                                        return s.rfind("temperature_sensor ", 0) == 0;
                                    }),
                      std::count_if(sensors_.begin(), sensors_.end(), [](const std::string& s) {
                          return s.rfind("temperature_fan ", 0) == 0;
                      }));
        spdlog::debug("  Fans: {}, LEDs: {}, Steppers: {}", fans_.size(), leds_.size(),
                      steppers_.size());
        spdlog::debug("  Filament sensors: {}, Width sensors: {}", filament_sensor_names_.size(),
                      width_sensor_objects_.size());
        spdlog::debug("  MMU: {} (type: {}), Tool changer: {}, Tools: {}", has_mmu_ ? "yes" : "no",
                      static_cast<int>(mmu_type_), has_tool_changer_ ? "yes" : "no",
                      tool_names_.size());
        spdlog::debug("  Macros: {} (Helix: {})", macros_.size(), helix_macros_.size());
    }

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
    void parse_config_keys(const nlohmann::json& config) {
        if (!config.is_object()) {
            return;
        }

        // Extract kinematics from [printer] section
        // Klipper's toolhead.kinematics status field returns null (it's an object reference),
        // so configfile.config.printer.kinematics is the reliable source
        if (config.contains("printer") && config["printer"].is_object()) {
            const auto& printer = config["printer"];
            if (printer.contains("kinematics") && printer["kinematics"].is_string()) {
                kinematics_ = printer["kinematics"].get<std::string>();
                spdlog::debug("[PrinterDiscovery] Kinematics from config: {}", kinematics_);
            }
        }

        for (const auto& [key, value] : config.items()) {
            if (key == "adxl345" || key.rfind("adxl345 ", 0) == 0 || key == "lis2dw" ||
                key.rfind("lis2dw ", 0) == 0 || key == "mpu9250" || key.rfind("mpu9250 ", 0) == 0 ||
                key == "lis3dh" || key.rfind("lis3dh ", 0) == 0 || key == "icm20948" ||
                key.rfind("icm20948 ", 0) == 0 || key == "resonance_tester") {
                has_accelerometer_ = true;
                spdlog::debug("[PrinterDiscovery] Accelerometer detected from config: {}", key);
            }

            // Beacon RevH has an onboard LIS2DW accelerometer.
            // Detect from accel-specific config fields in the [beacon] section.
            if (key == "beacon" && value.is_object()) {
                if (value.contains("accel_scale") || value.contains("accel_axes_map")) {
                    has_accelerometer_ = true;
                    spdlog::debug(
                        "[PrinterDiscovery] Beacon onboard accelerometer detected from config");
                }
            }

            // Also detect accelerometers referenced by resonance_tester
            // (e.g., accel_chip: beacon for Beacon RevH probes)
            if (key == "resonance_tester" && value.is_object()) {
                for (const auto& field : {"accel_chip", "accel_chip_x", "accel_chip_y"}) {
                    if (value.contains(field) && value[field].is_string()) {
                        const auto& chip = value[field].get<std::string>();
                        if (chip == "beacon" || chip.rfind("beacon ", 0) == 0) {
                            has_accelerometer_ = true;
                            spdlog::debug("[PrinterDiscovery] Beacon accelerometer detected via "
                                          "resonance_tester {}",
                                          field);
                        }
                    }
                }
            }

            // screws_tilt_adjust doesn't implement get_status() in Klipper,
            // so it may not appear in objects/list. Detect from configfile as fallback.
            if (key == "screws_tilt_adjust") {
                has_screws_tilt_ = true;
                spdlog::debug("[PrinterDiscovery] screws_tilt_adjust detected from config");
            }
        }
    }

    /**
     * @brief Reset all discovered hardware to initial state
     *
     * @note This clears ALL fields including printer info (hostname, versions, etc).
     *       When using parse_objects(), call printer info setters AFTER parse_objects()
     *       since it calls clear() internally.
     */
    void clear() {
        // Hardware lists
        heaters_.clear();
        fans_.clear();
        sensors_.clear();
        leds_.clear();
        steppers_.clear();

        // AMS/MMU discovery
        afc_lane_names_.clear();
        afc_hub_names_.clear();
        afc_unit_object_names_.clear();
        afc_buffer_names_.clear();
        tool_names_.clear();
        filament_sensor_names_.clear();
        width_sensor_objects_.clear();
        mmu_encoder_names_.clear();
        mmu_servo_names_.clear();

        // Macros
        macros_.clear();
        helix_macros_.clear();
        nozzle_clean_macro_.clear();
        purge_line_macro_.clear();
        heat_soak_macro_.clear();

        // Capability flags
        has_qgl_ = false;
        has_z_tilt_ = false;
        has_bed_mesh_ = false;
        has_probe_ = false;
        has_heater_bed_ = false;
        has_mmu_ = false;
        has_snapmaker_ = false;
        has_tool_changer_ = false;
        has_chamber_heater_ = false;
        has_chamber_sensor_ = false;
        chamber_sensor_name_.clear();
        chamber_heater_name_.clear();
        chamber_heater_object_name_.clear();
        has_led_ = false;
        led_effects_.clear();
        has_led_effects_ = false;
        led_macros_.clear();
        has_accelerometer_ = false;
        has_firmware_retraction_ = false;
        has_timelapse_ = false;
        has_exclude_object_ = false;
        has_screws_tilt_ = false;
        has_klippain_shaketune_ = false;
        has_speaker_ = false;
        has_fan_feedback_ = false;
        is_kalico_ = false;
        mmu_type_ = AmsType::NONE;
        detected_ams_systems_.clear();

        // Printer info
        hostname_.clear();
        software_version_.clear();
        moonraker_version_.clear();
        os_version_.clear();
        cpu_arch_.clear();
        kinematics_.clear();
        build_volume_ = BuildVolume{};
        mcu_.clear();
        mcu_list_.clear();
        mcu_versions_.clear();
        printer_objects_.clear();
    }

    // ========================================================================
    // Hardware Lists
    // ========================================================================

    [[nodiscard]] const std::vector<std::string>& heaters() const {
        return heaters_;
    }

    [[nodiscard]] const std::vector<std::string>& fans() const {
        return fans_;
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
        return macros_.count(to_upper(name)) > 0;
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
        return helix_macros_.count(to_upper(macro_name)) > 0;
    }

    /**
     * @brief Get total number of detected macros
     */
    [[nodiscard]] size_t macro_count() const {
        return macros_.size();
    }

    /**
     * @brief Get summary string for logging
     */
    [[nodiscard]] std::string summary() const;

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

  private:
    // Helper: convert string to uppercase
    static std::string to_upper(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        return result;
    }

    // Helper: natural sort — splits on trailing digits so "lane2" < "lane10"
    static void natural_sort(std::vector<std::string>& names) {
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
            // Find where trailing digits start
            auto digit_start = [](const std::string& s) -> size_t {
                size_t i = s.size();
                while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1])))
                    --i;
                return i;
            };
            size_t da = digit_start(a);
            size_t db = digit_start(b);
            std::string prefix_a = a.substr(0, da);
            std::string prefix_b = b.substr(0, db);
            if (prefix_a != prefix_b)
                return prefix_a < prefix_b;
            // Same prefix — compare numeric suffixes
            int num_a = (da < a.size()) ? std::stoi(a.substr(da)) : -1;
            int num_b = (db < b.size()) ? std::stoi(b.substr(db)) : -1;
            return num_a < num_b;
        });
    }

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

    // Macros
    std::unordered_set<std::string> macros_;
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
    bool has_snapmaker_ = false;
    bool has_tool_changer_ = false;
    bool has_chamber_heater_ = false;
    bool has_chamber_sensor_ = false;
    std::string chamber_sensor_name_;
    std::string chamber_heater_name_;        ///< Full object name (e.g., "heater_generic chamber")
    std::string chamber_heater_object_name_; ///< Object name only (e.g., "chamber")
    bool has_led_ = false;
    std::vector<std::string> led_effects_;
    bool has_led_effects_ = false;
    std::vector<std::string> led_macros_;
    bool has_accelerometer_ = false;
    bool has_firmware_retraction_ = false;
    bool has_timelapse_ = false;
    bool has_exclude_object_ = false;
    bool has_screws_tilt_ = false;
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
};

} // namespace helix

// Forward declarations for init_subsystems_from_hardware (global scope)
class MoonrakerAPI;
namespace helix {
class MoonrakerClient;
}

namespace helix {

/**
 * @brief Initialize subsystems from hardware discovery
 *
 * Initializes AMS backend, filament sensor manager, and standard macros
 * based on discovered hardware.
 *
 * @param hardware Hardware discovery results
 * @param api MoonrakerAPI instance
 * @param client MoonrakerClient instance
 */
void init_subsystems_from_hardware(const PrinterDiscovery& hardware, MoonrakerAPI* api,
                                   MoonrakerClient* client);

} // namespace helix
