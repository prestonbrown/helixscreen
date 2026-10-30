// SPDX-License-Identifier: GPL-3.0-or-later

// Out-of-line PrinterDiscovery members: the object-list scan and config parsers.

#include "ace_vocabulary.h"         // ace::kInstanceObjectPrefix
#include "chamber_heater_backend.h" // chamber::match, chamber::keyword_confidence
#include "display_numbering.h"      // helix::ui::tool_label
#include "json_utils.h"
#include "klipper_extruder_naming.h" // count_extruder_names
#include "macro_patterns.h"
#include "printer_discovery.h"
#include "text_io.h"
#include "toolchanger_addon.h" // zmod_c5_present

#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace helix {

void PrinterDiscovery::parse_objects(const nlohmann::json& objects) {
    clear();

    // Validate input is an array
    if (!objects.is_array()) {
        return;
    }
    objects_reported_ = true;

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

    constexpr int kChamberHeaterGenericWeight = 2;  // settable heater — preferred
    constexpr int kChamberTemperatureFanWeight = 1; // fan — only wins if no heater_generic

    // Promote the current object to the best chamber heater if its keyword
    // confidence (plus object-type tiebreak) exceeds the running best.
    // type_weight breaks ties between equal-keyword candidates.
    auto try_set_chamber_heater = [&](const std::string& full_name, const std::string& object_name,
                                      int type_weight) {
        // Registry first: appliance backends (dragonbreath, panda_breath)
        // claim their names at 95; generic carries the keyword tiers.
        chamber::MatchResult m = chamber::match(object_name);
        if (m.confidence == 0) {
            return; // not a chamber-named object — never a heater candidate
        }
        int conf = m.confidence * 10 + type_weight;
        if (conf > best_chamber_heater_conf) {
            has_chamber_heater_ = true;
            chamber_heater_name_ = full_name;
            chamber_heater_object_name_ = object_name;
            best_chamber_heater_conf = conf;
            chamber_heater_backend_id_ = std::string(m.backend->id());
            chamber_diagnostics_object_ = std::string(m.backend->diagnostics_object());
            chamber_filter_fan_pin_ = std::string(m.backend->filter_fan_pin());
        }
    };
    // Promote the current object to the best chamber sensor on keyword
    // confidence, keeping the first-listed object on a tie. Only passive
    // temperature_sensor objects reach here: anything that drives air
    // temperature scores at least as well in match() and takes the heater
    // slot instead, and the post-pass below then releases the sensor pick
    // entirely. Keywords only — appliance backends score their names in
    // match() and never claim the sensor slot.
    auto try_set_chamber_sensor = [&](const std::string& full_name,
                                      const std::string& object_name) {
        int keyword_conf = chamber::keyword_confidence(object_name);
        if (keyword_conf == 0) {
            return; // not a chamber-named object — never a sensor candidate
        }
        if (keyword_conf > best_chamber_sensor_conf) {
            has_chamber_sensor_ = true;
            chamber_sensor_name_ = full_name;
            best_chamber_sensor_conf = keyword_conf;
        }
    };
    // Record the chamber cooling fan independent of the heater pick. A
    // temperature_fan loses the heater role to a settable heater_generic, but
    // in COOLING mode (<=40C) the K2 M141 macro parks the setpoint on this
    // fan's target — so we must remember it separately to read that target.
    auto try_set_chamber_cooling_fan = [&](const std::string& full_name,
                                           const std::string& object_name) {
        int conf = chamber::keyword_confidence(object_name);
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

        std::string upper_name = helix::text_io::to_upper(name);

        // One classification per object. The bucket lists are rebuilt from it
        // after the loop; the switch records what a kind implies beyond that.
        const hw::ObjectClass cls = hw::classify_object(name);
        objects_.push_back({name, cls});
        switch (cls.kind) {
        case hw::ObjectKind::Heater:
            if (name == "heater_bed") {
                has_heater_bed_ = true;
            }
            break;
        case hw::ObjectKind::GenericHeater:
            try_set_chamber_heater(name, cls.instance, kChamberHeaterGenericWeight);
            break;
        case hw::ObjectKind::TemperatureSensor:
            try_set_chamber_sensor(name, cls.instance);
            break;
        // A chamber-named temperature_fan is the heater equivalent: it actively
        // drives air temperature, unlike a passive temperature_sensor.
        case hw::ObjectKind::TemperatureFan:
            try_set_chamber_heater(name, cls.instance, kChamberTemperatureFanWeight);
            try_set_chamber_cooling_fan(name, cls.instance);
            break;
        case hw::ObjectKind::Led:
        case hw::ObjectKind::OutputPinLed:
            has_led_ = true;
            break;
        case hw::ObjectKind::LedEffect:
            has_led_effects_ = true;
            break;
        case hw::ObjectKind::Speaker:
            has_speaker_ = true;
            break;
        case hw::ObjectKind::FanFeedback:
            has_fan_feedback_ = true;
            break;
        case hw::ObjectKind::Probe:
            has_probe_ = true;
            break;
        // AD5X IFS detection via ZMOD firmware sensors. Three sensor names
        // trigger it:
        //   1. lessWaste plugin:  "filament_switch_sensor _ifs_port_sensor_N"
        //   2. Native ZMOD (old): "filament_motion_sensor _ifs_motion_sensor_N"
        //   3. Native ZMOD:       "filament_motion_sensor ifs_motion_sensor"
        case hw::ObjectKind::FilamentSwitch:
        case hw::ObjectKind::FilamentMotion:
            if (!has_mmu_ && (name.rfind("filament_switch_sensor _ifs_port_sensor_", 0) == 0 ||
                              name.rfind("filament_motion_sensor _ifs_motion_sensor_", 0) == 0 ||
                              name == "filament_motion_sensor ifs_motion_sensor")) {
                has_mmu_ = true;
                mmu_type_ = AmsType::AD5X_IFS;
            }
            break;
        default:
            break;
        }
        if (cls.kind != hw::ObjectKind::Unknown) {
            continue;
        }

        // ================================================================
        // Capability flags
        // ================================================================
        if (name == "quad_gantry_level") {
            has_qgl_ = true;
        } else if (name == "z_tilt") {
            has_z_tilt_ = true;
        } else if (name == "bed_mesh") {
            has_bed_mesh_ = true;
        } else if (name == "firmware_retraction") {
            has_firmware_retraction_ = true;
        } else if (name == "timelapse") {
            has_timelapse_ = true;
        } else if (name == "exclude_object") {
            has_exclude_object_ = true;
        } else if (name == "screws_tilt_adjust") {
            has_screws_tilt_ = true;
            has_standard_screws_tilt_ = true;
        }
        // Snapmaker U1's own module: same capability, but its results
        // arrive as a status object (target_z, base_point1..4, probe_step),
        // not SCREWS_TILT_CALCULATE console lines. Unlike the upstream
        // section it DOES publish get_status(), so objects/list — this
        // path — is the primary detection; parse_config_keys() is the
        // fallback for a printer whose object list does not carry it.
        else if (name == "auto_screws_tilt_adjust") {
            has_screws_tilt_ = true;
            has_snapmaker_auto_screws_tilt_ = true;
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
        // Z-Mod's afclite extra for the Creator 5 line registers the `AFC`
        // object above through its own name; recorded separately so backend
        // selection can tell the shim from real Happy Hare AFC, which never
        // publishes this object.
        else if (name == "zmod_afclite") {
            has_zmod_afclite_ = true;
        }
        // klipper_openams. The name alone cannot claim the printer: a
        // manager that predates its versioned API publishes only
        // current_group, and AFC drives OpenAMS hardware without this
        // object. settle_status_claims() decides once the status is read.
        else if (name == openams::kManagerObject) {
            has_openams_manager_ = true;
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
        // The ACEPRO driver (github.com/Kobra-S1/ACEPRO) registers each unit
        // as `ace_instance_N` (has get_status()) beside a manager `ace`
        // object; a build whose manager does not appear in objects.list
        // still shows its `ace_instance_N` objects (#1107). Match all three
        // forms.
        //
        // Collect EVERY matched ACE object name (the discovery sequence
        // subscribes the real object names), but only the first ACE match
        // flips has_mmu_/mmu_type_ — with multiple `ace_instance_N` objects
        // the name-collection must NOT be gated behind the !has_mmu_ guard.
        else if (name == "ace" || name == "filament_hub" ||
                 name.rfind(helix::ace::kInstanceObjectPrefix, 0) == 0) {
            ace_object_names_.push_back(name);
            if (!has_mmu_) {
                has_mmu_ = true;
                mmu_type_ = AmsType::ACE;
                spdlog::info("[PrinterDiscovery] Detected ACE (Anycubic ACE Pro) via '{}' object",
                             name);
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
        // AFC extruder discovery (owns the <name>_tool_start/_tool_end sensors)
        else if (name.rfind("AFC_extruder ", 0) == 0) {
            std::string extruder_name = name.substr(13); // Remove "AFC_extruder " prefix
            if (!extruder_name.empty()) {
                afc_extruder_names_.push_back(extruder_name);
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
            // A literal `AFC_unit` is PAXX's AFC-Lite stub. Real AFC's
            // AFC_unit.py is a base class with no load_config_prefix, so
            // every unit that exists registers its own hardware type
            // (AFC_BoxTurtle, AFC_OpenAMS, AFC_HTLF, ...) and none can
            // publish this name.
            if (name.rfind("AFC_unit ", 0) == 0) {
                has_afc_lite_ = true;
            }
        }
        // AFC buffer objects
        else if (name.rfind("AFC_buffer ", 0) == 0) {
            std::string buffer_name = name.substr(11); // Remove "AFC_buffer " prefix (11 chars)
            if (!buffer_name.empty()) {
                afc_buffer_names_.push_back(buffer_name);
            }
        }
        // Standalone IFS module (the Forge-X drop-in): its sensors register under
        // stock filament_switch_sensor names, so no sensor pattern is needed and
        // these two names are not sensors. The module's objects, on their own: an [ifs] section
        // with no sensors configured, or [ifs_materials] on a machine whose
        // IFS board is unplugged (deliberately readable without the board -
        // the UI should still show the slot registry and report
        // not-connected). Either name alone is unambiguous: neither is a
        // stock Klipper object and no other known firmware defines them.
        else if (!has_mmu_ && (name == "ifs" || name == "ifs_materials")) {
            has_mmu_ = true;
            mmu_type_ = AmsType::AD5X_IFS;
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
            coexisting_systems_.push_back(AmsType::TOOL_CHANGER);
        }
        // pin_watch: a dock-sensor extra (not stock Klipper). Recorded as a
        // plain object fact, like toolchanger above. What it MEANS is
        // helix::toolchanger_addon's business, not this catalog's.
        // Deliberately does NOT touch has_mmu_/mmu_type_: those pick which
        // AMS backend gets built, and writing one here would swap out a
        // working backend on every printer that happens to run pin_watch.
        else if (name == "pin_watch" || name.rfind("pin_watch ", 0) == 0) {
            has_pin_watch_ = true;
            pin_watch_object_name_ = name;
        }
        // [medusahc]: a hotend changer's own extra, recorded as the same
        // kind of plain object fact as pin_watch above. It ships alongside
        // pin_watch, with or without klipper-toolchanger. Matched exactly -
        // [medusahc_calibrate] is a sibling object, not this one.
        else if (name == "medusahc" || name.rfind("medusahc ", 0) == 0) {
            has_medusahc_ = true;
            medusahc_object_name_ = name;
        }
        // [indx]: Bondtech INDX's toolboard host module (mainline
        // indx_klipper lists it; Kalico's may not, see parse_config_keys()).
        else if (name == "indx") {
            has_indx_ = true;
        } else if (name == "save_variables") {
            has_save_variables_ = true;
        } else if (name == "spoollink") {
            has_spoollink_ = true;
        }
        // Tool object discovery
        else if (name.rfind("tool ", 0) == 0) {
            std::string tool_name = name.substr(5); // Remove "tool " prefix
            if (!tool_name.empty()) {
                tool_names_.push_back(tool_name);
            }
        }
        // ================================================================
        // Macro detection
        // ================================================================
        else if (name.rfind("gcode_macro ", 0) == 0) {
            std::string macro_name = name.substr(12); // Remove "gcode_macro " prefix
            std::string upper_macro = helix::text_io::to_upper(macro_name);

            macros_.insert(upper_macro);
            // Klipper keeps the CONFIG case for the status object key
            // ("gcode_macro Tool_Offset") and for SET_GCODE_VARIABLE's
            // MACRO= mux key, while the callable command is the uppercased
            // alias. Code that has to name the object or write a variable
            // therefore cannot use the uppercased key we match on - see
            // macro_config_name().
            macro_config_names_.emplace(upper_macro, macro_name);

            // Check for HelixScreen helper macros
            if (upper_macro.rfind("HELIX_", 0) == 0) {
                helix_macros_.insert(upper_macro);
            }

            // Check for Klippain Shake&Tune
            if (upper_macro == "AXES_SHAPER_CALIBRATION") {
                has_klippain_shaketune_ = true;
            }

            // An M300 macro is the printer's tone command and the direct
            // proof it answers M300 gcode. Some buzzer setups have no
            // output_pin object at all (Z-Mod's AD5X config shells out to
            // a buzzer helper from an M300 macro), so the output_pin-based
            // speaker detection in the branch above never fires there.
            // Stronger signal than a beeper-named pin, too: a printer
            // defining the macro cannot answer M300 with
            // "Unknown command", which is the feedback loop the M300
            // backend's lazy install guards against.
            if (upper_macro == "M300") {
                has_speaker_ = true;
            }

            // AUTO_FEEDING_BATCH wraps a multi-head feed with target
            // snapshot/restore and next-head preheat. Absent on firmware
            // before 1.6, so the batch path falls back to bare
            // AUTO_FEEDING per head when this is false.
            if (upper_macro == macro_patterns::AUTO_FEEDING_BATCH) {
                has_auto_feeding_batch_ = true;
            }

            // Check for common macro patterns and cache them
            if (nozzle_clean_macro_.empty()) {
                // Shared with StandardMacros' CleanNozzle slot — see
                // include/macro_patterns.h.
                if (matches_any(upper_macro, macro_patterns::clean_nozzle())) {
                    nozzle_clean_macro_ = macro_name;
                }
            }

            if (purge_line_macro_.empty()) {
                static const std::vector<std::string> purge_patterns = {"PURGE_LINE", "PRIME_LINE",
                                                                        "INTRO_LINE", "LINE_PURGE"};
                if (matches_any(upper_macro, purge_patterns)) {
                    purge_line_macro_ = macro_name;
                }
            }

            if (heat_soak_macro_.empty()) {
                static const std::vector<std::string> soak_patterns = {"HEAT_SOAK", "CHAMBER_SOAK",
                                                                       "SOAK", "BED_SOAK"};
                if (matches_any(upper_macro, soak_patterns)) {
                    heat_soak_macro_ = macro_name;
                }
            }

            // LED macro auto-detection
            static const std::vector<std::string> led_keywords = {"LIGHT",     "LED",       "LAMP",
                                                                  "ILLUMINAT", "BACKLIGHT", "NEON"};
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
                // Exclusions match on whole underscore-delimited words. The
                // keyword test above stays a substring match on purpose (it
                // has to catch LIGHTS, LIGHTING, ILLUMINATE), but an
                // exclusion that fires on a fragment throws away real LED
                // macros -- LED_RAPID_FLASH is not a PID macro.
                bool excluded = false;
                for (const auto& ex : led_exclusions) {
                    if (contains_word(upper_macro, ex)) {
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

    rebuild_buckets();

    // AFC_stepper objects can be lanes (Box Turtle: "AFC_stepper lane0") or motor
    // components (Vivid: "AFC_stepper Vivid_1_drive"/"Vivid_1_selector").
    // When no AFC_lane objects exist, treat all steppers as lanes (pure Box Turtle).
    // When BOTH exist (e.g., Box Turtle + OpenAMS + ACE), merge stepper names
    // that look like lanes ("lane" prefix + digit) into the lane list.
    if (afc_lane_names_.empty() && !afc_stepper_names.empty()) {
        afc_lane_names_ = std::move(afc_stepper_names);
    } else if (!afc_lane_names_.empty() && !afc_stepper_names.empty()) {
        // Mixed setup: merge AFC_stepper lane names not already in AFC_lane list
        std::unordered_set<std::string> existing(afc_lane_names_.begin(), afc_lane_names_.end());
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
    if (!afc_extruder_names_.empty()) {
        natural_sort(afc_extruder_names_);
    }

    // Name order until the toolchanger's status says how Klipper numbers them
    // (order_tools_by_number). Natural, so T10 follows T9 rather than T1.
    if (!tool_names_.empty()) {
        natural_sort(tool_names_);
    }

    // A chamber heater measures its own chamber, so it supplies the reading
    // and there is no separate chamber sensor to name. Objects are
    // classified in one pass and the picks cannot consult each other, so a
    // chamber-named probe may have taken the sensor role before any heater
    // was seen. Release it: the role is a suppression flag in the graph and
    // the sensor lists, so a probe left holding it is hidden in order to
    // stand in for a reading it does not supply. An assignment naming a
    // probe still outranks the heater downstream.
    if (has_chamber_heater_ && has_chamber_sensor_) {
        spdlog::info("[PrinterDiscovery] Chamber heater '{}' supplies the chamber "
                     "temperature; '{}' keeps its own sensor role.",
                     chamber_heater_name_, chamber_sensor_name_);
        chamber_sensor_name_.clear();
        has_chamber_sensor_ = false;
    }

    // [tool N] objects come from klipper-toolchanger, so a machine that does
    // not run it has none: a hotend changer driven by its own extra, or a
    // plain multi-extruder printer whose T<n> macros are the whole story.
    // What is left to count is the hot ends, and one heater and extruder
    // motor per tool is exactly what those machines are. The names are the
    // G-code tool numbers their own T<n> commands use.
    //
    // is_extruder_name() is the same predicate ToolState::init_tools() maps
    // these heaters through, so the count here can never disagree with the
    // tools it builds. It rejects extruder_stepper, which is how a mixing
    // hotend stays one tool.
    //
    // Never overwrites real tool objects - a klipper-toolchanger name is
    // arbitrary, and ASSIGN_TOOL can remap it.
    const std::size_t extruder_heater_count = helix::count_extruder_names(heaters_);
    if (tool_names_.empty() && extruder_heater_count > 1) {
        for (std::size_t i = 0; i < extruder_heater_count; ++i) {
            tool_names_.push_back(helix::ui::tool_label(static_cast<int>(i)));
        }
    }

    // multiACE hangs ACE Pro units off a Snapmaker U1's four toolheads and
    // registers a plain `ace` object, so it matches ACE detection above and
    // outranks the U1 fallback below. Its slots live per unit under
    // `aces[]`, not in the top-level `slots` array AmsBackendAce requires,
    // so an ACE backend would attach and read nothing while the Snapmaker
    // backend that drives those toolheads stayed suppressed. objects.list
    // carries names without status, so the discriminator is co-presence:
    // filament_detect is published by U1 firmware alone, and no ACE stack
    // this chain matches runs on an unmodded U1. Yielding the printer back
    // costs the ACE-specific affordances and keeps filament management
    // (prestonbrown/helixscreen#1426). The object names stay recorded: the
    // hardware really does carry them, and nothing subscribes them once the
    // type is no longer ACE.
    if (has_mmu_ && mmu_type_ == AmsType::ACE && has_snapmaker_) {
        has_mmu_ = false;
        spdlog::info("[PrinterDiscovery] ACE object alongside filament_detect: multiACE on a "
                     "Snapmaker U1. Its slot shape is unreadable, so the Snapmaker backend "
                     "keeps the printer.");
    }

    // PAXX's AFC-Lite is a status-only stub that impersonates AFC so Fluidd
    // and Mainsail will draw their AFC panel for a U1's four extruders. It
    // reports no extruders and no hubs, so the unit infers as HUB and the
    // path draws one nozzle behind a hub for a four-toolhead machine. Its
    // every operation wraps the U1's own AUTO_FEEDING, which the Snapmaker
    // backend already drives, so yielding costs nothing the stub provided
    // and restores the four toolheads. Scoped to co-presence with
    // filament_detect for the same reason the ACE rule above is: a bare
    // AFC_unit on some other machine is displacing nothing better.
    if (has_mmu_ && mmu_type_ == AmsType::AFC && has_afc_lite_ && has_snapmaker_) {
        has_mmu_ = false;
        spdlog::info("[PrinterDiscovery] AFC_unit alongside filament_detect: PAXX AFC-Lite on "
                     "a Snapmaker U1. It reports no extruders, so the Snapmaker backend keeps "
                     "the printer and its four toolheads.");
    }

    // Z-Mod's afclite shim impersonates AFC so Fluidd and Mainsail draw an AFC
    // panel for the Creator 5's four heads. It reports no extruders and no
    // hubs, so an AFC backend infers a hub topology for heads that are each
    // their own path, and most of the AFC op macros do not exist on this
    // firmware. The changer fingerprint is what names the machine (the AD5X
    // Z-Mod publishes zmod_color but has no carriage grab buttons), so the
    // toolchanger backend keeps the printer. Scoped to that fingerprint for the
    // same reason the PAXX rule is scoped to filament_detect: a bare
    // zmod_afclite on some other machine is displacing nothing better.
    if (has_mmu_ && mmu_type_ == AmsType::AFC && has_zmod_afclite_ &&
        toolchanger_addon::zmod_c5_present(*this)) {
        has_mmu_ = false;
        spdlog::info("[PrinterDiscovery] zmod_afclite alongside the Creator 5 changer "
                     "fingerprint: Z-Mod's AFC shim on a head changer. The toolchanger "
                     "backend keeps the printer and its four toolheads.");
    }

    register_detected_ams_systems();
}

namespace {

bool kind_in(hw::ObjectKind kind, const std::vector<hw::ObjectKind>& kinds) {
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

} // namespace

std::vector<std::string>
PrinterDiscovery::objects_of(const std::vector<hw::ObjectKind>& kinds) const {
    std::vector<std::string> out;
    for (const auto& o : objects_) {
        if (kind_in(o.cls.kind, kinds)) {
            out.push_back(o.name);
        }
    }
    return out;
}

std::vector<std::pair<std::string, hw::ObjectKind>> PrinterDiscovery::object_kinds() const {
    std::vector<std::pair<std::string, hw::ObjectKind>> out;
    out.reserve(objects_.size());
    for (const auto& o : objects_) {
        out.emplace_back(o.name, o.cls.kind);
    }
    return out;
}

std::vector<std::string>
PrinterDiscovery::sections_of(const std::vector<hw::ObjectKind>& kinds) const {
    std::vector<std::string> out;
    for (const auto& s : sections_) {
        if (kind_in(s.cls.kind, kinds)) {
            out.push_back(s.name);
        }
    }
    return out;
}

const nlohmann::json* PrinterDiscovery::section(std::string_view name) const {
    for (const auto& s : sections_) {
        if (s.name == name) {
            return &s.body;
        }
    }
    return nullptr;
}

void PrinterDiscovery::rebuild_buckets() {
    using hw::ObjectKind;
    heaters_ = objects_of({ObjectKind::Heater, ObjectKind::GenericHeater});
    fans_ =
        objects_of({ObjectKind::Fan, ObjectKind::HeaterFan, ObjectKind::ControllerFan,
                    ObjectKind::GenericFan, ObjectKind::TemperatureFan, ObjectKind::OutputPinFan});
    sensors_ = objects_of({ObjectKind::TemperatureSensor, ObjectKind::TemperatureFan,
                           ObjectKind::DriverTemperature, ObjectKind::HumidityChip});
    leds_ = objects_of({ObjectKind::Led, ObjectKind::OutputPinLed});
    led_effects_ = objects_of({ObjectKind::LedEffect});
    load_cells_ = objects_of({ObjectKind::LoadCell});
    steppers_ = objects_of({ObjectKind::Stepper});
    width_sensor_objects_ = objects_of({ObjectKind::WidthSensor});
    filament_sensor_names_ = objects_of({ObjectKind::FilamentSwitch, ObjectKind::FilamentMotion});
}

void PrinterDiscovery::settle_status_claims(const nlohmann::json& status) {
    if (!has_openams_manager_ || has_mmu_) {
        return;
    }
    auto manager = status.is_object() ? status.find(openams::kManagerObject) : status.end();
    if (manager == status.end() || !openams::api_supported(*manager)) {
        spdlog::info("[PrinterDiscovery] oams_manager publishes no supported OpenAMS UI API; "
                     "not claiming the printer for OpenAMS");
        return;
    }
    has_mmu_ = true;
    mmu_type_ = AmsType::OPENAMS;
    register_detected_ams_systems();
}

void PrinterDiscovery::register_detected_ams_systems() {
    detected_ams_systems_.clear();

    // Register the filament management backend. When a real MMU (AFC, Happy
    // Hare, etc.) is present, it always wins — even on Snapmaker U1 hardware
    // that also reports filament_detect. The Snapmaker backend is a basic
    // 4-slot fallback for U1s without an aftermarket MMU, and for a U1 whose
    // MMU is one we cannot read. Toolchanger alone only handles tool
    // switching, not filament management.
    if (has_mmu_) {
        if (mmu_type_ != AmsType::NONE) {
            detected_ams_systems_.push_back({mmu_type_, ams_type_to_string(mmu_type_)});
        }
    } else if (has_snapmaker_) {
        // Native Snapmaker filament system (no aftermarket MMU)
        mmu_type_ = AmsType::SNAPMAKER;
        detected_ams_systems_.push_back({mmu_type_, ams_type_to_string(mmu_type_)});
    } else if (has_indx_) {
        // Docked passive nozzles on one extruder: never the tool-changer arm
        // below, which needs several extruders or klipper-toolchanger.
        mmu_type_ = AmsType::INDX;
        detected_ams_systems_.push_back({mmu_type_, ams_type_to_string(mmu_type_)});
    } else if (!tool_names_.empty() && (has_tool_changer_ || tool_names_.size() > 1)) {
        // More than one hot end, and no filament system managing them:
        // parallel topology, one slot per tool. Registering it is what gives
        // these printers slots, per-tool spool identity and the filament
        // panel's tool selector instead of the single-extruder UI
        // (prestonbrown/helixscreen#1350). klipper-toolchanger is a changer
        // on its own word even with a single tool declared.
        // Still last in the chain, so a real MMU always keeps its backend.
        mmu_type_ = AmsType::TOOL_CHANGER;
        detected_ams_systems_.push_back({mmu_type_, ams_type_to_string(mmu_type_)});
    }
}

void PrinterDiscovery::parse_config_keys(const nlohmann::json& config) {
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

    sections_.clear();
    for (const auto& [key, value] : config.items()) {
        const hw::ObjectClass cls = hw::classify_section(key, value);
        if (cls.kind != hw::ObjectKind::Unknown) {
            sections_.push_back({key, cls, value});
        }

        // screws_tilt_adjust doesn't implement get_status() in Klipper,
        // so it may not appear in objects/list. Detect from configfile as fallback.
        // The Snapmaker U1's auto_screws_tilt_adjust is the same story. The
        // dialect is derived from both flags at read time, so key iteration
        // order cannot decide a printer that configures the two sections.
        if (key == "screws_tilt_adjust") {
            has_screws_tilt_ = true;
            has_standard_screws_tilt_ = true;
            spdlog::debug("[PrinterDiscovery] screws_tilt_adjust detected from config");
        } else if (key == "auto_screws_tilt_adjust") {
            has_screws_tilt_ = true;
            has_snapmaker_auto_screws_tilt_ = true;
            spdlog::debug("[PrinterDiscovery] auto_screws_tilt_adjust detected from config");
        }
    }

    has_accelerometer_ = !sections_of({hw::ObjectKind::Accelerometer}).empty();

    if (config.contains("spoollink")) {
        has_spoollink_ = true;
    }
    // [indx] on Kalico may have no get_status(), so the configfile section is
    // the only sign of it, and it arrives after parse_objects() registered the
    // filament systems.
    if (!has_indx_ && config.contains("indx")) {
        has_indx_ = true;
        spdlog::debug("[PrinterDiscovery] INDX detected from config");
        register_detected_ams_systems();
    }
}

bool PrinterDiscovery::parse_build_volume(const nlohmann::json& settings) {
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

    // The bed size a firmware's own config declares, where it declares one.
    // Travel above also covers overtravel to a purge or nozzle-clean
    // position, so it cannot stand for the bed. One row per firmware; a
    // bed is declared only when both axes read as positive numbers. Macro
    // variables reach configfile.settings as strings ("350").
    struct DeclaredBedSource {
        const char* section;
        const char* x_key;
        const char* y_key;
    };
    static constexpr DeclaredBedSource kDeclaredBedSources[] = {
        // Creality K2 series
        {"gcode_macro product_param", "variable_bed_size_x", "variable_bed_size_y"},
    };
    auto read_size = [](const nlohmann::json& section, const char* key, float& out) {
        const auto v = section.find(key);
        if (v == section.end()) {
            return false;
        }
        if (v->is_number()) {
            out = v->get<float>();
            return out > 0.0f;
        }
        if (!v->is_string()) {
            return false;
        }
        const std::string& text = v->get_ref<const std::string&>();
        char* end = nullptr;
        out = std::strtof(text.c_str(), &end);
        return !text.empty() && end == text.c_str() + text.size() && out > 0.0f;
    };
    for (const auto& source : kDeclaredBedSources) {
        const auto s = settings.find(source.section);
        float x = 0.0f;
        float y = 0.0f;
        if (s != settings.end() && s->is_object() && read_size(*s, source.x_key, x) &&
            read_size(*s, source.y_key, y)) {
            volume.declared_bed_x = x;
            volume.declared_bed_y = y;
            break;
        }
    }

    // The probed area is the plate the head can safely reach; travel
    // beyond it may hold tool docks or a purge bucket.
    const auto mesh = settings.find("bed_mesh");
    if (mesh != settings.end() && mesh->is_object()) {
        const auto lo = mesh->find("mesh_min");
        const auto hi = mesh->find("mesh_max");
        auto is_xy = [](const nlohmann::json& v) {
            return v.is_array() && v.size() >= 2 && v[0].is_number() && v[1].is_number();
        };
        if (lo != mesh->end() && hi != mesh->end() && is_xy(*lo) && is_xy(*hi)) {
            volume.plate_x_min = (*lo)[0].get<float>();
            volume.plate_y_min = (*lo)[1].get<float>();
            volume.plate_x_max = (*hi)[0].get<float>();
            volume.plate_y_max = (*hi)[1].get<float>();
        }
    }

    // An all-zero volume is worse than none: build_volume_range heuristics
    // would score it against every printer's window. Only a real extent is
    // worth storing.
    if (volume.x_max <= 0.0f && volume.y_max <= 0.0f) {
        return false;
    }

    build_volume_ = volume;
    spdlog::debug("[PrinterDiscovery] Build volume from config: X[{:.0f},{:.0f}] "
                  "Y[{:.0f},{:.0f}] Z[0,{:.0f}] declared bed [{:.0f},{:.0f}]",
                  volume.x_min, volume.x_max, volume.y_min, volume.y_max, volume.z_max,
                  volume.declared_bed_x, volume.declared_bed_y);
    return true;
}

void PrinterDiscovery::parse_probe_xy_offset(const nlohmann::json& settings) {
    probe_xy_offset_.reset();
    if (!settings.is_object()) {
        return;
    }
    // Klipper allows one Z probe, so the first probe section present is the
    // one PROBE_CALIBRATE uses. Section names arrive lowercased. The name alone
    // decides: a RevH [beacon] also carries its onboard accelerometer, which its
    // body classifies as, and it is still the probe.
    static const nlohmann::json kNameOnly = nlohmann::json::object();
    const nlohmann::json* section = nullptr;
    for (const auto& [name, value] : settings.items()) {
        if (value.is_object() &&
            hw::classify_section(name, kNameOnly).kind == hw::ObjectKind::Probe) {
            section = &value;
            break;
        }
    }
    if (!section) {
        return;
    }
    ProbeXYOffset offset;
    const auto x = section->find("x_offset");
    if (x != section->end() && x->is_number()) {
        offset.x = x->get<double>();
    }
    const auto y = section->find("y_offset");
    if (y != section->end() && y->is_number()) {
        offset.y = y->get<double>();
    }
    probe_xy_offset_ = offset;
}

void PrinterDiscovery::apply_printer_info(const nlohmann::json& result) {
    software_version_ = json_util::safe_string(result, "software_version", "unknown");
    if (json_util::safe_string(result, "app") == "Kalico") {
        is_kalico_ = true;
    }
}

void PrinterDiscovery::clear() {
    objects_.clear();
    sections_.clear();

    // Hardware lists
    heaters_.clear();
    fans_.clear();
    load_cells_.clear();
    sensors_.clear();
    leds_.clear();
    steppers_.clear();

    // AMS/MMU discovery
    afc_lane_names_.clear();
    afc_hub_names_.clear();
    afc_unit_object_names_.clear();
    afc_buffer_names_.clear();
    afc_extruder_names_.clear();
    tool_names_.clear();
    filament_sensor_names_.clear();
    width_sensor_objects_.clear();
    mmu_encoder_names_.clear();
    mmu_servo_names_.clear();
    ace_object_names_.clear();

    // Macros
    macros_.clear();
    macro_config_names_.clear();
    host_restarting_macros_.clear();
    host_halting_macros_.clear();
    led_driving_macros_.clear();
    skip_wrappable_.clear();
    skip_active_.clear();
    sensor_toggle_command_.clear();
    screws_tilt_builtin_.clear();
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
    has_openams_manager_ = false;
    has_snapmaker_ = false;
    has_afc_lite_ = false;
    has_zmod_afclite_ = false;
    has_tool_changer_ = false;
    coexisting_systems_.clear();
    has_pin_watch_ = false;
    pin_watch_object_name_.clear();
    has_medusahc_ = false;
    medusahc_object_name_.clear();
    has_indx_ = false;
    has_save_variables_ = false;
    has_spoollink_ = false;
    has_chamber_heater_ = false;
    has_chamber_sensor_ = false;
    chamber_sensor_name_.clear();
    chamber_heater_name_.clear();
    chamber_heater_object_name_.clear();
    chamber_heater_backend_id_.clear();
    chamber_diagnostics_object_.clear();
    chamber_filter_fan_pin_.clear();
    chamber_cooling_fan_name_.clear();
    chamber_fan_resting_deci_ = 0;
    filament_diameter_mm_ = filament::kDefaultDiameterMm;
    nozzle_diameter_mm_ = kDefaultNozzleDiameterMm;
    fit_machine_.reset();
    leveling_probe_points_.clear();
    probe_xy_offset_.reset();
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
    has_standard_screws_tilt_ = false;
    has_snapmaker_auto_screws_tilt_ = false;
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
    objects_reported_ = false;
}

namespace {

/// The name a [gcode_macro <section>] wrapper gave the builtin it replaced
/// (rename_existing), or empty when nothing wraps it. @p section is the
/// lowercase configfile key.
std::string renamed_builtin(const nlohmann::json& settings, const char* section) {
    const auto wrapper = settings.find(section);
    if (wrapper == settings.end() || !wrapper->is_object()) {
        return {};
    }
    const auto renamed = wrapper->find("rename_existing");
    if (renamed == wrapper->end() || !renamed->is_string()) {
        return {};
    }
    return renamed->get<std::string>();
}

} // namespace

bool PrinterDiscovery::parse_sensor_toggle_command(const nlohmann::json& settings) {
    std::string builtin = renamed_builtin(settings, "gcode_macro set_filament_sensor");
    if (builtin.empty()) {
        return false;
    }
    sensor_toggle_command_ = std::move(builtin);
    return true;
}

bool PrinterDiscovery::parse_screws_tilt_builtin(const nlohmann::json& settings) {
    screws_tilt_builtin_ = renamed_builtin(settings, "gcode_macro screws_tilt_calculate");
    return !screws_tilt_builtin_.empty();
}

void PrinterDiscovery::order_tools_by_number(const nlohmann::json& toolchanger_status) {
    if (!toolchanger_status.is_object()) {
        return;
    }
    const auto listed = toolchanger_status.find("tool_names");
    if (listed == toolchanger_status.end() || !listed->is_array()) {
        return;
    }

    // klipper-toolchanger names each tool by its config section, "tool Left".
    static constexpr std::string_view kSectionPrefix = "tool ";
    std::vector<std::string> ordered;
    ordered.reserve(tool_names_.size());
    for (const auto& entry : *listed) {
        if (!entry.is_string()) {
            continue;
        }
        std::string name = entry.get<std::string>();
        if (name.rfind(kSectionPrefix, 0) == 0) {
            name.erase(0, kSectionPrefix.size());
        }
        const bool discovered =
            std::find(tool_names_.begin(), tool_names_.end(), name) != tool_names_.end();
        if (discovered && std::find(ordered.begin(), ordered.end(), name) == ordered.end()) {
            ordered.push_back(std::move(name));
        }
    }
    for (const auto& name : tool_names_) {
        if (std::find(ordered.begin(), ordered.end(), name) == ordered.end()) {
            ordered.push_back(name);
        }
    }

    if (ordered != tool_names_) {
        std::string joined;
        for (const auto& name : ordered) {
            joined += joined.empty() ? name : ", " + name;
        }
        spdlog::info("[PrinterDiscovery] Tools ordered by Klipper tool number: {}", joined);
        tool_names_ = std::move(ordered);
    }
}

void PrinterDiscovery::natural_sort(std::vector<std::string>& names) {
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
        // A suffix too long for an int sorts after every one that fits.
        int num_a = (da < a.size())
                        ? helix::text_io::parse_leading<int>(a.substr(da)).value_or(INT_MAX)
                        : -1;
        int num_b = (db < b.size())
                        ? helix::text_io::parse_leading<int>(b.substr(db)).value_or(INT_MAX)
                        : -1;
        return num_a < num_b;
    });
}

bool PrinterDiscovery::contains_word(const std::string& name, const std::string& needle) {
    if (needle.empty() || needle.size() > name.size()) {
        return false;
    }
    for (size_t pos = name.find(needle); pos != std::string::npos;
         pos = name.find(needle, pos + 1)) {
        const size_t end = pos + needle.size();
        const bool left_ok = (pos == 0) || (name[pos - 1] == '_');
        const bool right_ok = (end == name.size()) || (name[end] == '_');
        if (left_ok && right_ok) {
            return true;
        }
    }
    return false;
}

} // namespace helix
