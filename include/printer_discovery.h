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
            return;
        }

        // Keep the raw list. Detection's object_exists / macro_match /
        // macro_exclude heuristics read printer_objects(), which 73 of the 94
        // database entries depend on, so a caller that parses an object list
        // and never calls set_printer_objects() would score blind against the
        // strongest signals in the database. The discovery sequence still calls
        // the setter afterwards with the same strings, which is an idempotent
        // overwrite rather than a second copy.
        printer_objects_.reserve(objects.size());
        for (const auto& obj : objects) {
            if (obj.is_string()) {
                printer_objects_.push_back(obj.template get<std::string>());
            }
        }

        // AFC_stepper names collected separately — only used as lane source when
        // no AFC_lane objects exist (Box Turtle compat). Vivid uses AFC_stepper for
        // motor components (drive/selector), not lanes.
        std::vector<std::string> afc_stepper_names;

        // Highest-confidence chamber match wins regardless of iteration order,
        // so a "chamber"-named heater always beats a "box"-named one when a
        // printer has both (e.g. QIDI Q2: real chamber heater + Qidi-Box dryer).
        //
        // The score is keyword-tier dominant with an object-TYPE tiebreak:
        // when two candidates share the same keyword (e.g. the Creality K2 Plus
        // exposes both `heater_generic chamber_heater` and a `temperature_fan
        // chamber_fan` cooling fan), a settable heater_generic must win so that
        // "heat the chamber" never routes to a fan's cooling threshold. The
        // keyword weight is scaled so type only ever breaks exact-keyword ties,
        // never promotes a weaker keyword over a stronger one.
        int best_chamber_heater_conf = 0;
        int best_chamber_sensor_conf = 0;
        int best_chamber_cooling_fan_conf = 0;

        constexpr int CHAMBER_HEATER_GENERIC_WEIGHT = 2;  // settable heater — preferred
        constexpr int CHAMBER_TEMPERATURE_FAN_WEIGHT = 1; // fan — only wins if no heater_generic

        // Promote the current object to the best chamber heater if its keyword
        // confidence (plus object-type tiebreak) exceeds the running best.
        // type_weight breaks ties between equal-keyword candidates.
        auto try_set_chamber_heater = [&](const std::string& full_name,
                                          const std::string& object_name, int type_weight) {
            int keyword = chamber_keyword_confidence(object_name);
            if (keyword == 0) {
                return; // not a chamber-named object — never a heater candidate
            }
            int conf = keyword * 10 + type_weight;
            if (conf > best_chamber_heater_conf) {
                has_chamber_heater_ = true;
                chamber_heater_name_ = full_name;
                chamber_heater_object_name_ = object_name;
                best_chamber_heater_conf = conf;
            }
        };
        auto try_set_chamber_sensor = [&](const std::string& full_name,
                                          const std::string& object_name) {
            int conf = chamber_keyword_confidence(object_name);
            if (conf > best_chamber_sensor_conf) {
                has_chamber_sensor_ = true;
                chamber_sensor_name_ = full_name;
                best_chamber_sensor_conf = conf;
            }
        };
        // Record the chamber cooling fan independent of the heater pick. A
        // temperature_fan loses the heater role to a settable heater_generic, but
        // in COOLING mode (<=40C) the K2 M141 macro parks the setpoint on this
        // fan's target — so we must remember it separately to read that target.
        auto try_set_chamber_cooling_fan = [&](const std::string& full_name,
                                               const std::string& object_name) {
            int conf = chamber_keyword_confidence(object_name);
            if (conf > best_chamber_cooling_fan_conf) {
                chamber_cooling_fan_name_ = full_name;
                best_chamber_cooling_fan_conf = conf;
            }
        };

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
                std::string heater_name = name.substr(15); // Remove "heater_generic " prefix
                try_set_chamber_heater(name, heater_name, CHAMBER_HEATER_GENERIC_WEIGHT);
            }
            // ================================================================
            // Sensors: temperature_sensor, temperature_fan (dual-purpose)
            // ================================================================
            else if (name.rfind("temperature_sensor ", 0) == 0) {
                sensors_.push_back(name);
                std::string sensor_name = name.substr(19); // Remove "temperature_sensor " prefix
                try_set_chamber_sensor(name, sensor_name);
            }
            // Temperature-controlled fans (also act as sensors). A chamber-named
            // temperature_fan is the heater equivalent — it actively drives air
            // temperature, unlike a passive temperature_sensor.
            else if (name.rfind("temperature_fan ", 0) == 0) {
                sensors_.push_back(name);
                fans_.push_back(name);                  // Also add to fans for control
                std::string fan_name = name.substr(16); // Remove "temperature_fan " prefix
                try_set_chamber_heater(name, fan_name, CHAMBER_TEMPERATURE_FAN_WEIGHT);
                try_set_chamber_cooling_fan(name, fan_name);
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
            // CFS detection (Creality Filament System).
            //
            // Both K1 and K2 series publish a `box` Klipper object when the
            // official CFS upgrade is installed, but the firmwares expose
            // different macro dialects:
            //   - K2 stock firmware: CR_BOX_PRE_OPT / CR_BOX_EXTRUDE /
            //     CR_BOX_WASTE / CR_BOX_FLUSH / CR_BOX_END_OPT, plus BOX_*
            //     envelope (BOX_SAVE_FAN, BOX_MODE_WAIT, etc.)
            //   - K1 official CFS upgrade (≥ v2.3.5.33): BOX_EXTRUDE_MATERIAL,
            //     BOX_MATERIAL_FLUSH, BOX_NOZZLE_CLEAN, BOX_CUT_MATERIAL,
            //     BOX_RETRUDE_MATERIAL_WITH_TNN — no CR_ prefix, no fan-save.
            // AmsBackendCfs picks the right dialect from PrinterDetector at
            // construction (#968).
            else if (name == "box") {
                has_mmu_ = true;
                mmu_type_ = AmsType::CFS;
                if (PrinterDetector::is_creality_k1()) {
                    spdlog::info("[PrinterDiscovery] 'box' object on K1-series printer — "
                                 "enabling CFS backend with K1 macro dialect (BOX_*).");
                }
            }
            // ACE detection (Anycubic ACE Pro). Native Anycubic GoKlipper
            // (as shipped by Rinkhals firmware) registers the status object as
            // `filament_hub` even though the config section is `[ace]`. The
            // community drivers (ValgACE/BunnyACE/DuckACE) register it as `ace`.
            // The Anycubic Kobra S1 "mainline-Python ACE fork" registers each
            // unit as `ace_instance_N` (has get_status()) and exposes NO
            // top-level `ace`/`filament_hub` object — its config is `[ace]`
            // with `ace_count`, but only `ace_instance_N` appears in
            // objects.list (#1107). Match all three forms.
            //
            // Collect EVERY matched ACE object name (the discovery sequence
            // subscribes the real object names), but only the first ACE match
            // flips has_mmu_/mmu_type_ — with multiple `ace_instance_N` objects
            // the name-collection must NOT be gated behind the !has_mmu_ guard.
            else if (name == "ace" || name == "filament_hub" ||
                     name.rfind("ace_instance", 0) == 0) {
                ace_object_names_.push_back(name);
                if (!has_mmu_) {
                    has_mmu_ = true;
                    mmu_type_ = AmsType::ACE;
                    spdlog::info(
                        "[PrinterDiscovery] Detected ACE (Anycubic ACE Pro) via '{}' object", name);
                }
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
                filament_sensor_names_.push_back(name);
            }
            // QIDI Box detection — custom Klipper extension on Plus 4 / Q2 / Max 4
            // registers `box_stepper slot<N>` per physical slot (4 per box,
            // 1-4 boxes chainable to 16 slots). Presence of any `box_stepper
            // slot*` object is the unambiguous detection signal; the per-name
            // count gives the physical slot count.
            else if (name.rfind("box_stepper slot", 0) == 0) {
                if (!has_mmu_) {
                    has_mmu_ = true;
                    mmu_type_ = AmsType::QIDI_BOX;
                }
                if (mmu_type_ == AmsType::QIDI_BOX) {
                    ++qidi_box_slot_count_;
                }
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
            else if (name == "hall_filament_width_sensor" ||
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

        // Register the filament management backend. When a real MMU (AFC, Happy
        // Hare, etc.) is present, it always wins — even on Snapmaker U1 hardware
        // that also reports filament_detect. The Snapmaker backend is a basic
        // 4-slot fallback for U1s without an aftermarket MMU. Toolchanger alone
        // only handles tool switching, not filament management.
        if (has_mmu_) {
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
            } else if (mmu_type_ == AmsType::QIDI_BOX) {
                // i18n: do not translate - product name
                detected_ams_systems_.push_back({AmsType::QIDI_BOX, "QIDI Box"});
            }
        } else if (has_snapmaker_) {
            // Native Snapmaker filament system (no aftermarket MMU)
            detected_ams_systems_.push_back({AmsType::SNAPMAKER, "Snapmaker"});
            mmu_type_ = AmsType::SNAPMAKER;
        } else if (has_tool_changer_ && !tool_names_.empty()) {
            // Standalone tool changer with no MMU — show parallel topology
            detected_ams_systems_.push_back({AmsType::TOOL_CHANGER, "Tool Changer"});
            mmu_type_ = AmsType::TOOL_CHANGER;
        }
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
    bool parse_build_volume(const nlohmann::json& settings) {
        if (!settings.is_object()) {
            return false;
        }

        BuildVolume volume{};
        auto read = [&settings](const char* section, const char* field, float& out) {
            const auto s = settings.find(section);
            if (s == settings.end() || !s->is_object()) {
                return;
            }
            const auto f = s->find(field);
            if (f != s->end() && f->is_number()) {
                out = f->get<float>();
            }
        };
        read("stepper_x", "position_min", volume.x_min);
        read("stepper_x", "position_max", volume.x_max);
        read("stepper_y", "position_min", volume.y_min);
        read("stepper_y", "position_max", volume.y_max);
        read("stepper_z", "position_max", volume.z_max);

        // An all-zero volume is worse than none: build_volume_range heuristics
        // would score it against every printer's window. Only a real extent is
        // worth storing.
        if (volume.x_max <= 0.0f && volume.y_max <= 0.0f) {
            return false;
        }

        build_volume_ = volume;
        spdlog::debug("[PrinterDiscovery] Build volume from config: X[{:.0f},{:.0f}] "
                      "Y[{:.0f},{:.0f}] Z[0,{:.0f}]",
                      volume.x_min, volume.x_max, volume.y_min, volume.y_max, volume.z_max);
        return true;
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
        ace_object_names_.clear();

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
        chamber_cooling_fan_name_.clear();
        chamber_fan_resting_deci_ = 0;
        fan_max_power_.clear();
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
        qidi_box_slot_count_ = 0;
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

    // Chamber/enclosure keyword scoring for sensor, fan, and heater object
    // names. Vendors diverge: Creality uses "chamber", Snapmaker uses "cavity",
    // Elegoo COSMOS uses literal "box", modders often use "enclosure".
    //
    // Returns 0 for no match, higher for stronger evidence. The discovery loop
    // keeps the highest-scoring match so iteration order does not decide.
    //
    // CHAMBER / ENCLOSURE / CAVITY match as substrings — these names are
    // unambiguous, and compound forms ("chamber_temp", "ENCLOSURE_top") are
    // intended as the printer chamber.
    //
    // BOX matches only as a standalone token — split on `_` and whitespace —
    // so AMS-style names like "box1_heater" / "Box1_STM32" (QIDI Box filament
    // dryer) are not mistaken for the printer chamber. The COSMOS case
    // ("temperature_sensor box") and compound forms ("box_fan") still match.
    //
    // Two modifiers refine the base tier so a real chamber TEMPERATURE sensor
    // outranks an air-quality sensor whose name merely contains a chamber
    // keyword:
    //   - Compound penalty (-1): if the keyword is not the entire name (e.g.
    //     "chamber_temp"), subtract 1. Exact "chamber" (100) then beats
    //     "chamber_temp" (99) on a tie, while both still crush weaker keywords.
    //   - Air-quality penalty (-40): if the name carries an air-quality token
    //     (TVOC/VOC/CO2/GAS/HUMIDITY/IAQ/AQI/PM25/PM10/PARTICULATE/PRESSURE),
    //     subtract 40 so "chamber_tvoc" (59) loses to a clean "chamber" (100)
    //     or even "enclosure" (90).
    // A matched score is floored at 1 so an only-candidate air-quality chamber
    // sensor is still detected rather than dropped; no keyword still returns 0.
    static int chamber_keyword_confidence(const std::string& object_name) {
        std::string upper = to_upper(object_name);

        int score = 0;
        const char* keyword = nullptr;
        if (upper.find("CHAMBER") != std::string::npos) {
            score = 100;
            keyword = "CHAMBER";
        } else if (upper.find("ENCLOSURE") != std::string::npos) {
            score = 90;
            keyword = "ENCLOSURE";
        } else if (upper.find("CAVITY") != std::string::npos) {
            score = 85;
            keyword = "CAVITY";
        } else if (has_standalone_token(upper, "BOX")) {
            score = 60;
            keyword = "BOX";
        } else {
            return 0;
        }

        // Compound penalty: prefer a name that is exactly the keyword.
        if (upper != keyword)
            score -= 1;

        // Air-quality penalty: names carrying a gas/particulate/humidity token
        // describe air quality, not chamber temperature.
        static const char* const AIR_QUALITY_TOKENS[] = {
            "TVOC", "VOC",  "CO2",  "GAS",         "HUMIDITY", "IAQ",
            "AQI",  "PM25", "PM10", "PARTICULATE", "PRESSURE"};
        for (const char* tok : AIR_QUALITY_TOKENS) {
            if (has_standalone_token(upper, tok)) {
                score -= 40;
                break;
            }
        }

        // A matched keyword always stays detectable.
        return score < 1 ? 1 : score;
    }

    // Returns true iff `token` appears in `haystack` as a complete token,
    // where tokens are separated by `_` or whitespace. Both inputs are
    // compared case-sensitively; callers upper-case beforehand.
    static bool has_standalone_token(const std::string& haystack, const std::string& token) {
        auto is_separator = [](char c) {
            return c == '_' || std::isspace(static_cast<unsigned char>(c));
        };
        size_t i = 0;
        while (i < haystack.size()) {
            while (i < haystack.size() && is_separator(haystack[i])) {
                ++i;
            }
            size_t start = i;
            while (i < haystack.size() && !is_separator(haystack[i])) {
                ++i;
            }
            if (i - start == token.size() && haystack.compare(start, token.size(), token) == 0) {
                return true;
            }
        }
        return false;
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
    std::vector<std::string> ace_object_names_; // filament_hub / ace / ace_instance_N (#1107)

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
    std::string chamber_cooling_fan_name_;   ///< Full object name of the chamber temperature_fan
                                             ///< (e.g., "temperature_fan chamber_fan"). Recorded
                                             ///< independent of the heater pick: in COOLING mode
                                             ///< the K2 M141 macro parks the setpoint on this fan's
                                             ///< target, not the heater's.
    int chamber_fan_resting_deci_ = 0;       ///< Cooling fan's configured resting/off target
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
};

} // namespace helix

// Forward declarations for init_subsystems_from_hardware (global scope)
class IMoonrakerAPI;
namespace helix {
class IMoonrakerClient;
}

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

} // namespace helix
