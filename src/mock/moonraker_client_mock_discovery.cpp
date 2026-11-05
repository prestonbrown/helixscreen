// SPDX-License-Identifier: GPL-3.0-or-later

// What the mock publishes for discovery: the printer objects, capabilities and
// hardware lists of the persona in effect, adjusted by the HELIX_MOCK_* knobs.
// The simulation thread may already be running when these run and iterates the
// same lists, so every writer here holds discovery_mutex_.

#include "chamber_heater_backend.h"
#include "macro_param_cache.h"
#include "mock_persona.h"
#include "moonraker_client_mock.h"
#include "moonraker_client_mock_internal.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace helix;

namespace {

/// Registry consult: is this object a chamber HEATER (heater_generic /
/// temperature_fan whose bare name a chamber-heater backend claims)?
/// Replaces the old find("chamber") scans — backend-named heaters like
/// "heater_generic dragonbreath" contain no "chamber" at all.
bool is_chamber_heater_object(const std::string& obj) {
    std::string name;
    if (obj.rfind("heater_generic ", 0) == 0) {
        name = obj.substr(15);
    } else if (obj.rfind("temperature_fan ", 0) == 0) {
        name = obj.substr(16);
    } else {
        return false;
    }
    return chamber::match(name).confidence > 0;
}

/// Does any registered backend expose this exact bare object as its
/// diagnostics status object? Used by the HELIX_MOCK_OBJECTS tokenizer to
/// recognize a standalone diagnostics token ("dragonbreath") that follows a
/// completed chamber heater instead of appending to it.
bool is_registered_diagnostics_object(const std::string& token) {
    for (const auto* backend : chamber::registry()) {
        if (!backend->diagnostics_object().empty() && backend->diagnostics_object() == token) {
            return true;
        }
    }
    return false;
}

/// Replaces @p out with the names up to the first empty entry of @p names.
template <std::size_t N>
void assign_names(std::vector<std::string>& out, const std::array<std::string_view, N>& names) {
    out.clear();
    for (const std::string_view name : names) {
        if (name.empty()) {
            break;
        }
        out.emplace_back(name);
    }
}

} // namespace

void MoonrakerClientMock::update_cached_chamber_key() {
    cached_chamber_status_key_.clear();
    for (const auto& h : discovery_.heaters()) {
        if (h == "heater_bed" || h.rfind("extruder", 0) == 0) {
            continue;
        }
        // Registry consult (not find("chamber")) so backend-named heaters
        // ("heater_generic dragonbreath") win the chamber status key too.
        if (is_chamber_heater_object(h)) {
            cached_chamber_status_key_ = h;
            return;
        }
    }
}

// PRECONDITION: caller holds discovery_mutex_. Reached only from
// populate_capabilities() and rebuild_hardware_from_lists(), both of which take it.
// Locking here instead would self-deadlock — discovery_mutex_ is not recursive.
void MoonrakerClientMock::override_chamber_heater(const std::string& heater_obj) {
    auto& heaters = discovery_.heaters();
    // Registry-matched erase: a later "heater_generic chamber" replaces a
    // dragonbreath heater and vice versa (find("chamber") would miss the
    // backend-named ones entirely).
    heaters.erase(std::remove_if(heaters.begin(), heaters.end(),
                                 [](const std::string& h) { return is_chamber_heater_object(h); }),
                  heaters.end());
    heaters.push_back(heater_obj);

    // temperature_fan needs to be in sensors list for periodic status updates
    if (heater_obj.rfind("temperature_fan ", 0) == 0) {
        discovery_.sensors().push_back(heater_obj);
    }

    update_cached_chamber_key();
}

void MoonrakerClientMock::populate_capabilities() {
    // Held for the whole body: the simulation thread may already be running (connect()
    // starts it before discover_printer() calls this) and iterates these same lists.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    // Create mock Klipper object list for capabilities parsing
    json mock_objects = json::array();

    // Every persona inherits the default objects below unless its descriptor omits them.
    const auto persona = helix::mock::descriptor(printer_type_);
    auto inherits = [&persona](helix::mock::DefaultObjects object) {
        return (persona.omit & object) == 0;
    };
    using namespace helix::mock::default_object;

    // Add common objects
    mock_objects.push_back("heater_bed");
    mock_objects.push_back("extruder");
    // The second hub lane feeds a second extruder.
    if (is_mock_openams() && (openams_two_lane_units() || openams_fleet_lane_units())) {
        mock_objects.push_back("extruder1");
    }
    mock_objects.push_back("bed_mesh");

    // Add capabilities for UI testing (speaker for M300, firmware retraction for G10/G11)
    mock_objects.push_back("output_pin beeper");   // Triggers has_speaker_ capability
    mock_objects.push_back("firmware_retraction"); // Triggers has_firmware_retraction_ capability

    // Chamber temperature sensor for UI testing
    if (inherits(CHAMBER_SENSOR)) {
        mock_objects.push_back("temperature_sensor chamber");
    }

    // HELIX_MOCK_OBJECTS: space-separated list of additional Klipper objects to add
    // e.g., HELIX_MOCK_OBJECTS="temperature_fan chamber" to test temperature_fan chamber
    // heaters, or the dragonbreath trio:
    //   "heater_generic dragonbreath dragonbreath output_pin dragonbreath_filter"
    // Runs BEFORE the discovery-list snapshot below on purpose: a chamber
    // heater from the env REPLACES the default-profile one
    // (override_chamber_heater), and a stale "heater_generic chamber" left in
    // the snapshot would outscore a backend-named heater in parse_objects
    // (keyword 100 beats the appliance backends' 95).
    const char* mock_obj_env = std::getenv("HELIX_MOCK_OBJECTS");
    if (mock_obj_env && mock_obj_env[0]) {
        std::istringstream iss(mock_obj_env);
        std::string token;
        std::string current_obj;
        auto flush_object = [&]() {
            mock_objects.push_back(current_obj);
            spdlog::info("[MoonrakerClientMock] Added mock object: {}", current_obj);
            // A chamber heater from the env replaces the default-profile
            // chamber heater so the registry pick — and the mock's
            // chamber-status-key cache — resolve to the env-specified one.
            if (is_chamber_heater_object(current_obj)) {
                override_chamber_heater(current_obj);
            }
        };
        while (iss >> token) {
            // Accumulate tokens: "temperature_fan" + "chamber" → "temperature_fan chamber"
            if (!current_obj.empty()) {
                // A type prefix always starts a new object...
                bool is_prefix =
                    (token.rfind("heater_generic", 0) == 0 ||
                     token.rfind("temperature_fan", 0) == 0 ||
                     token.rfind("temperature_sensor", 0) == 0 ||
                     token.rfind("output_pin", 0) == 0 || token.rfind("gcode_button", 0) == 0);
                // ...and so does a bare backend diagnostics object
                // ("dragonbreath") following a COMPLETED chamber heater —
                // without this the second "dragonbreath" would append to
                // "heater_generic dragonbreath" instead of standing alone.
                bool is_standalone_diagnostics = current_obj.find(' ') != std::string::npos &&
                                                 is_chamber_heater_object(current_obj) &&
                                                 is_registered_diagnostics_object(token);
                if (is_prefix || is_standalone_diagnostics) {
                    // Flush previous object
                    flush_object();
                    current_obj = token;
                } else {
                    current_obj += " " + token;
                }
            } else {
                current_obj = token;
            }
        }
        if (!current_obj.empty()) {
            flush_object();
        }
    }

    // Add hardware objects from populated lists
    for (const auto& heater : discovery_.heaters()) {
        // Skip if already added (heater_bed, extruder)
        if (heater != "heater_bed" && heater != "extruder") {
            mock_objects.push_back(heater);
        }
    }
    for (const auto& fan : discovery_.fans()) {
        mock_objects.push_back(fan);
    }
    for (const auto& sensor : discovery_.sensors()) {
        // Only include sensors with proper prefixes — skip bare heater names
        // (e.g., "extruder", "heater_bed") which are already in heaters list
        if (sensor.rfind("temperature_sensor ", 0) == 0 ||
            sensor.rfind("temperature_fan ", 0) == 0 || sensor.rfind("tmc2240 ", 0) == 0 ||
            sensor.rfind("tmc2209 ", 0) == 0 || sensor.rfind("tmc5160 ", 0) == 0) {
            mock_objects.push_back(sensor);
        }
    }
    for (const auto& led : discovery_.leds()) {
        mock_objects.push_back(led);
    }
    // Additional objects set via set_additional_objects() for capability testing
    for (const auto& obj : additional_objects_) {
        mock_objects.push_back(obj);
    }

    // HELIX_MOCK_STEPPER_RESONANCE: the stepper_resonance_tester extra on any
    // persona, with the stock STEPPER_RESONANCE_FACTORY_CALIBRATE wrapper ("1")
    // or the bare extra alone ("bare").
    if (const char* resonance = std::getenv("HELIX_MOCK_STEPPER_RESONANCE");
        resonance && *resonance && std::string_view(resonance) != "0") {
        mock_objects.push_back("stepper_resonance_tester");
        if (std::string_view(resonance) != "bare") {
            mock_objects.push_back("gcode_macro STEPPER_RESONANCE_FACTORY_CALIBRATE");
        }
    }

    // Add printer-specific objects
    switch (printer_type_) {
    case PrinterType::VORON_24:
        mock_objects.push_back("quad_gantry_level");
        mock_objects.push_back("gcode_macro CLEAN_NOZZLE");
        mock_objects.push_back("gcode_macro PRINT_START");
        break;
    case PrinterType::VORON_TRIDENT:
        mock_objects.push_back("z_tilt");
        mock_objects.push_back("gcode_macro CLEAN_NOZZLE");
        mock_objects.push_back("gcode_macro PRINT_START");
        break;
    case PrinterType::FLASHFORGE_CREATOR5:
        // Creator 5 Pro fingerprint. The database entry keys off [ff_toolchange]
        // (99%) and the head-dock grab sensor (95%); see the
        // flashforge_creator_5_pro heuristics in printer_database.json. Without
        // these in printer.objects.list the persona would be detected as a
        // generic CoreXY.
        mock_objects.push_back("ff_toolchange");
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("gcode_button extruder_grab" + std::to_string(i));
        }
        // The chamber heater object separates the Pro from the heater-free
        // Creator 5, so a persona modelling the Pro must publish it.
        mock_objects.push_back("heater_generic chamber_heater");
        mock_objects.push_back("gcode_macro TOOLCHANGE_PARK");
        mock_objects.push_back("gcode_macro BED_MESH_CALIBRATE");
        // The enclosure's door and top-cover switches.
        mock_objects.push_back("gcode_button frontDoor");
        mock_objects.push_back("gcode_button topDoor");
        break;
    case PrinterType::FLASHFORGE_CREATOR5_ZMOD:
        // Z-Mod's own objects. Its carriage buttons are 1-based, unlike the
        // Reforge persona's grab0..3 above; zmod_c5_detect in
        // toolchanger_addon keys off exactly these names.
        mock_objects.push_back("zmod");
        mock_objects.push_back("zmod_color");
        mock_objects.push_back("save_variables");
        // Z-Mod 1.7.4's afclite shim: the AFC-shaped objects it registers so
        // Fluidd/Mainsail draw an AFC panel for the C5's heads (names from
        // field bundle 5WSPTZBL). Discovery must yield the AFC claim these
        // would otherwise raise to the tool changer.
        mock_objects.push_back("zmod_afclite");
        mock_objects.push_back("AFC");
        mock_objects.push_back("AFC_unit C5");
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("AFC_lane E" + std::to_string(i));
            mock_objects.push_back("filament_switch_sensor fd_ex" + std::to_string(i));
            mock_objects.push_back("filament_motion_sensor fm_ex" + std::to_string(i));
        }
        for (int i = 1; i <= 4; ++i) {
            mock_objects.push_back("gcode_button extruder_pos" + std::to_string(i));
            mock_objects.push_back("gcode_button extruder_grab" + std::to_string(i));
        }
        // Same Pro-separating chamber heater as the Reforge persona above.
        mock_objects.push_back("heater_generic chamber_heater");
        mock_objects.push_back("gcode_button frontDoor");
        mock_objects.push_back("gcode_button topDoor");
        // c5pro_generic.cfg's calibration macros.
        mock_objects.push_back("gcode_macro CALIBRATE_EXTRUDERS");
        mock_objects.push_back("gcode_macro CALIBRATE_VFA");
        mock_objects.push_back("gcode_macro AIR_CIRCULATION_INTERNAL");
        mock_objects.push_back("gcode_macro AIR_CIRCULATION_EXTERNAL");
        mock_objects.push_back("gcode_macro AIR_CIRCULATION_STOP");
        break;
    case PrinterType::ELEGOO_CC1:
        // The CC1 capture's own objects. _COSMOS_SETTINGS and ELEGOO_PURGE are
        // what name the machine; its hostname and M191 match an enclosed QIDI too.
        for (const char* obj : {"configfile",
                                "print_stats",
                                "virtual_sdcard",
                                "pause_resume",
                                "display_status",
                                "screws_tilt_adjust",
                                "gcode_macro _COSMOS_SETTINGS",
                                "gcode_macro SAVE_CONFIG",
                                "gcode_macro _KAMP_Settings",
                                "gcode_macro LINE_PURGE",
                                "gcode_macro ELEGOO_PURGE",
                                "gcode_macro SMART_PARK",
                                "gcode_macro PRINT_START",
                                "gcode_macro M191",
                                "gcode_macro PRINT_END",
                                "gcode_macro CLEAN_NOZZLE",
                                "gcode_macro MOVE_TO_TRAY",
                                "gcode_macro CUT_FILAMENT",
                                "gcode_macro LOADCELL_Z_HOME",
                                "gcode_macro CALIBRATE_Z_OFFSET",
                                "gcode_macro _UPDATE_COSMOS"}) {
            mock_objects.push_back(obj);
        }
        break;
    case PrinterType::FLASHFORGE_AD5X:
        // The stock AD5X's public IFS macro, which names the machine. None of
        // the IFS module's own objects: those make discovery stand up the
        // production AD5X IFS backend, and this persona runs the mock IFS.
        mock_objects.push_back("gcode_macro SET_EXTRUDER_SLOT");
        break;
    case PrinterType::CREALITY_K2_PLUS:
        // The K2 Plus capture's identifying objects. `box` rides on is_mock_cfs().
        for (const char* obj :
             {"motor_control", "fan_feedback", "load_ai", "filament_rack",
              "output_pin extruder_fan", "output_pin power", "output_pin ptc_power",
              "temperature_sensor mcu_temp", "heater_fan chamber_heater_fan"}) {
            mock_objects.push_back(obj);
        }
        break;
    case PrinterType::SNAPMAKER_U1:
        // The U1 capture's identifying objects. Not `filament_detect`, which would
        // make discovery stand up the production Snapmaker backend; this persona
        // runs the mock one.
        for (const char* obj :
             {"tool", "fm175xx_reader", "tmc2240 stepper_x", "purifier", "camera",
              "defect_detection", "gcode_macro FILAMENT_DT_UPDATE", "gcode_macro FILAMENT_DT_QUERY",
              "gcode_macro EXTRUDER_OFFSET_ACTION_PROBE_CALIBRATE_ALL",
              "gcode_macro _EXTRUDER_OFFSET_ACTION_PRESTART",
              "gcode_macro _EXTRUDER_OFFSET_ACTION_EXIT"}) {
            mock_objects.push_back(obj);
        }
        // The per-head hooks the offset calibration steps run.
        for (int i = 0; i < 4; ++i) {
            for (const char* step :
                 {"HEAT", "AUTO_CLEAN", "MANUAL_CLEAN", "WAIT_COOL", "PROBE_CALIBRATE"}) {
                mock_objects.push_back(
                    fmt::format("gcode_macro _EXTRUDER_OFFSET_ACTION_{}_T{}", step, i));
            }
        }
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("filament_motion_sensor e" + std::to_string(i) + "_filament");
        }
        break;
    default:
        // Other printers may not have these features
        break;
    }

    // Add LED effects (klipper-led_effect plugin objects)
    if (inherits(kLedEffects)) {
        mock_objects.push_back("led_effect breathing");
        mock_objects.push_back("led_effect fire_comet");
        mock_objects.push_back("led_effect rainbow");
        mock_objects.push_back("led_effect static_white");
    }

    // [exclude_object] — present on modern Klipper/Kalico configs. Trips
    // has_exclude_object_ in PrinterDiscovery so capability-gated features
    // (e.g. adaptive bed mesh) light up under --test.
    mock_objects.push_back("exclude_object");

    // Add common macros for all printer types (for testing macro panel)
    mock_objects.push_back("gcode_macro START_PRINT");
    mock_objects.push_back("gcode_macro END_PRINT");
    mock_objects.push_back("gcode_macro PAUSE");
    mock_objects.push_back("gcode_macro RESUME");
    mock_objects.push_back("gcode_macro CANCEL_PRINT");
    mock_objects.push_back("gcode_macro LOAD_FILAMENT");
    mock_objects.push_back("gcode_macro UNLOAD_FILAMENT");
    mock_objects.push_back("gcode_macro BED_MESH_CALIBRATE");
    mock_objects.push_back("gcode_macro G28");           // Home all
    mock_objects.push_back("gcode_macro M600");          // Filament change
    mock_objects.push_back("gcode_macro _SYSTEM_MACRO"); // System macro (hidden by default)
    append_layer_pause_objects(mock_objects);
    for (const auto& name : helix::sim::mock_padded_macro_names()) {
        mock_objects.push_back("gcode_macro " + name);
    }

    // Add LED-related macros (auto-detected by printer_discovery via LED keywords)
    if (inherits(kLedEffects)) {
        mock_objects.push_back("gcode_macro LIGHTS_ON");
        mock_objects.push_back("gcode_macro LIGHTS_OFF");
        mock_objects.push_back("gcode_macro LIGHTS_TOGGLE");
        mock_objects.push_back("gcode_macro LED_PARTY");
        mock_objects.push_back("gcode_macro LED_NIGHTLIGHT");
    }

    // MCU objects for discovery
    mock_objects.push_back("mcu");
    if (inherits(kEbbCanMcu)) {
        mock_objects.push_back("mcu EBBCan");
    }

    // Humidity sensors (BME280/HTU21D for enclosure monitoring)
    if (inherits(kBme280Chamber)) {
        mock_objects.push_back("bme280 chamber");
    }
    if (inherits(kHtu21dDryer)) {
        mock_objects.push_back("htu21d dryer");
    }

    // Width sensor (filament diameter measurement via Hall effect sensor)
    if (inherits(kWidthSensor)) {
        mock_objects.push_back("hall_filament_width_sensor");
    }

    // Moonraker plugins
    mock_objects.push_back("timelapse"); // Moonraker-Timelapse plugin

    // MMU/AMS system - Happy Hare uses "mmu" object name.
    // Suppressed in the MedusaHC modes, the standalone IFS module mode, the OpenAMS
    // mode and any persona that omits it: "mmu" detects Happy Hare (priority over every
    // other filament system) and would stand the wrong backend up.
    if (mmu_enabled_ && !is_mock_medusahc() && !is_mock_ifs_module() && !is_mock_openams() &&
        !is_mock_indx() && inherits(kHappyHareMmu)) {
        mock_objects.push_back("mmu");
    }

    // Probe objects: HELIX_MOCK_PROBE_TYPE, else the persona's own probe
    // (mock_internal::mock_probe_type).
    const json probe_status = helix::sim::mock_probe_status(printer_type_);
    for (auto it = probe_status.begin(); it != probe_status.end(); ++it) {
        mock_objects.push_back(it.key());
    }
    spdlog::debug("[MoonrakerClientMock] Mock probe: {}",
                  mock_internal::mock_probe_type(printer_type_));

    // Filament sensors (common setup: runout sensor at spool holder)
    // Check HELIX_MOCK_FILAMENT_SENSORS env var for custom sensor names
    // Default: single switch sensor named "runout_sensor"
    const char* sensor_env = std::getenv("HELIX_MOCK_FILAMENT_SENSORS");
    if (sensor_env && std::string(sensor_env) == "none") {
        // Explicitly disabled
        spdlog::debug("[MoonrakerClientMock] Filament sensors disabled via env var");
    } else if (sensor_env) {
        // Custom sensor list (comma-separated, e.g., "switch:fsensor,motion:encoder")
        std::string sensors_str(sensor_env);
        size_t pos = 0;
        while ((pos = sensors_str.find(',')) != std::string::npos || !sensors_str.empty()) {
            std::string token =
                (pos != std::string::npos) ? sensors_str.substr(0, pos) : sensors_str;
            size_t colon = token.find(':');
            if (colon != std::string::npos) {
                std::string type = token.substr(0, colon);
                std::string name = token.substr(colon + 1);
                if (type == "switch") {
                    mock_objects.push_back("filament_switch_sensor " + name);
                } else if (type == "motion") {
                    mock_objects.push_back("filament_motion_sensor " + name);
                }
            }
            if (pos == std::string::npos)
                break;
            sensors_str.erase(0, pos + 1);
        }
        spdlog::debug("[MoonrakerClientMock] Custom filament sensors from env: {}", sensor_env);
    } else if (printer_type_ == PrinterType::FLASHFORGE_CREATOR5 ||
               printer_type_ == PrinterType::FLASHFORGE_CREATOR5_ZMOD) {
        // One runout switch per head, named as in assets/config/presets/creator5_pro.json.
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("filament_switch_sensor fd_ex" + std::to_string(i));
        }
        // Z-Mod pairs each switch with a motion sensor.
        if (printer_type_ == PrinterType::FLASHFORGE_CREATOR5_ZMOD) {
            for (int i = 0; i < 4; ++i) {
                mock_objects.push_back("filament_motion_sensor fm_ex" + std::to_string(i));
            }
        }
        spdlog::debug("[MoonrakerClientMock] Creator 5 filament sensors: fd_ex0..fd_ex3");
    } else if (printer_type_ == PrinterType::ELEGOO_CC1) {
        // The chassis runout switch, named as in assets/config/presets/cc1.json.
        mock_objects.push_back("filament_switch_sensor filament_sensor");
    } else if (printer_type_ == PrinterType::FLASHFORGE_AD5X) {
        // The toolhead switch, named as in assets/config/presets/ad5x.json.
        mock_objects.push_back("filament_switch_sensor head_switch_sensor");
    } else if (printer_type_ == PrinterType::CREALITY_K2_PLUS) {
        // The toolhead switch, named as in assets/config/presets/k2.json.
        mock_objects.push_back("filament_switch_sensor filament_sensor");
    } else if (inherits(kRunoutSensor)) {
        // Default: one switch sensor (typical Voron setup)
        mock_objects.push_back("filament_switch_sensor runout_sensor");
        spdlog::debug(
            "[MoonrakerClientMock] Default filament sensor: filament_switch_sensor runout_sensor");
    }

    // Toolchanger mock mode (HELIX_MOCK_AMS=toolchanger): push the toolchanger +
    // per-tool discovery objects so PrinterDiscovery sets has_tool_changer() and
    // tool_names() = {T0..T3}. The 4 extruder heaters (extruder + extruder1/2/3)
    // are already flowing in via the discovery_.heaters() loop above. Tool naming
    // uses "tool TN" to match the established test convention (test_hardware_validator).
    if (is_mock_toolchanger()) {
        mock_objects.push_back("toolchanger");
        // The object that marks a firmware able to measure pressure advance
        // (the U1's flow calibrator depends on it), so the PA calibration
        // screen is reachable in --test. Stock Klipper has none.
        mock_objects.push_back("filament_parameters");
        for (int i = 0; i < 4; ++i) {
            mock_objects.push_back("tool T" + std::to_string(i));
        }
        // klipper-toolchanger's example config: the nozzle-contact probe and
        // the all-in-one calibration macro it drives. The macro's presence is
        // what makes the Tool Offsets calibration screen reachable.
        mock_objects.push_back("tools_calibrate");
        mock_objects.push_back("gcode_macro CALIBRATE_TOOL_OFFSETS");
    }

    // MedusaHC mock mode (HELIX_MOCK_AMS=medusahc[-ktc|-tn]): a hotend changer
    // bolted onto klipper-toolchanger. Unlike the toolchanger mode above this
    // does NOT build an AmsBackendMock - try_create_mock() declines these values
    // - so the objects pushed here are what real discovery sees, and the
    // production AmsBackendToolChanger + toolchanger_addon path runs against
    // them. Each variant emits the object set its configuration actually has,
    // so toolchanger_addon's detection and swap command routing are exercised
    // at runtime and not only in the unit tests.
    // See docs/devel/FILAMENT_BACKEND_MEDUSAHC.md § "Two shipping configurations".
    if (const MedusaVariant variant = mock_medusa_variant(); variant != MedusaVariant::NONE) {
        // The controller reads the docks through [pin_watch io].
        mock_objects.push_back("pin_watch io");
        if (variant == MedusaVariant::CONTROLLER_KTC) {
            // The optional klipper-toolchanger (pin_watch sync_toolchanger: 1),
            // configured as the README's minimal [toolchanger] + [tool Tn].
            mock_objects.push_back("toolchanger");
            for (int i = 0; i < medusa_tool_count(); ++i) {
                mock_objects.push_back("tool T" + std::to_string(i));
            }
        }
        // The tools are named from the extruder heaters (one hot end per
        // extruder) where no [tool N] exists. Each T<n> macro runs SET T=<n>,
        // which runs MHC_SET; klipper-toolchanger keeps these when it is there.
        for (int i = 0; i < medusa_tool_count(); ++i) {
            mock_objects.push_back("gcode_macro T" + std::to_string(i));
        }
        mock_objects.push_back("medusahc");
        // The Irbis3D controller registers MHC_* as native commands, which
        // printer.objects.list never shows; its MHC_macros.cfg wraps them in
        // these macros, and MHC_variables.cfg holds its state in the `_` ones.
        for (const char* macro :
             {"SET", "_DROP", "DROP_TOOL", "OPEN", "CLOSE", "CLEAN", "_ERROR", "_TOOL_OFFSET_T",
              "_LAYER_SET", "_TOOL_CFG", "_GLOBAL_STATE", "_TOOL_OFFSET"}) {
            mock_objects.push_back(std::string("gcode_macro ") + macro);
        }
        if (std::find(mock_objects.begin(), mock_objects.end(), "save_variables") ==
            mock_objects.end()) {
            mock_objects.push_back("save_variables");
        }
        spdlog::info("[MoonrakerClientMock] MedusaHC mock: variant={}",
                     variant == MedusaVariant::CONTROLLER       ? "controller"
                     : variant == MedusaVariant::CONTROLLER_KTC ? "controller-ktc"
                                                                : "controller-tn");
    }

    // multiACE mock hardware (HELIX_MOCK_AMS=multiace): the U1's own feed
    // objects plus the plain `ace` object multiACE registers, so real
    // discovery stands the PRODUCTION AmsBackendMultiAce up against the frames
    // the simulation serves below (same rule as the MedusaHC modes:
    // try_create_mock() declines this value). The U1 persona deliberately
    // omits `filament_detect` so the plain mock backend runs; this mode puts
    // it back, so pair it with HELIX_MOCK_PRINTER=snapmaker_u1.
    if (is_mock_multiace()) {
        for (const char* obj : {"filament_detect", "filament_feed left", "filament_feed right",
                                "print_task_config", "machine_state_manager", "ace"}) {
            mock_objects.push_back(obj);
        }
        read_multiace_knobs();
    }

    // Standalone IFS module mock mode (HELIX_MOCK_AMS=ifs-module): the
    // module's own objects plus its stock-named sensors, so real discovery
    // sets AmsType::AD5X_IFS and the production AmsBackendAd5xIfs runs against
    // the frames the simulation loop pushes. try_create_mock() declines this
    // value (it matches none of its spellings), exactly like the MedusaHC
    // modes above. save_variables is pushed too — the module's ifs_loaded
    // record rides it and the backend always subscribes it.
    if (is_mock_ifs_module()) {
        mock_objects.push_back("ifs");
        mock_objects.push_back("ifs_materials");
        mock_objects.push_back("save_variables");
        for (int i = 1; i <= 4; ++i) {
            mock_objects.push_back("filament_switch_sensor lane" + std::to_string(i));
        }
        mock_objects.push_back("filament_switch_sensor toolhead");
        spdlog::info("[MoonrakerClientMock] Standalone IFS module mock: ifs + ifs_materials + "
                     "4 lane sensors + toolhead");
    }

    // CFS mock mode (HELIX_MOCK_AMS=cfs): the stock K1 `box` status object the
    // production AmsBackendCfs subscribes and parses. try_create_mock()
    // declines this value, so real discovery plus the real backend run; pair
    // with HELIX_MOCK_PRINTER=k1 to latch the K1 stock dialect.
    if (is_mock_cfs()) {
        mock_objects.push_back("box");
        spdlog::info("[MoonrakerClientMock] CFS mock: box status object");
    }

    // OpenAMS mock mode (HELIX_MOCK_AMS=openams): the `oams_manager` status
    // object the production AmsBackendOpenAms claims through discovery.
    // try_create_mock() declines this value so the real backend runs.
    if (is_mock_openams()) {
        mock_objects.push_back("oams_manager");
        spdlog::info("[MoonrakerClientMock] OpenAMS mock: oams_manager status object");
    }

    // INDX mock hardware (HELIX_MOCK_AMS=indx): Bondtech's macro objects plus
    // the toolboard module, so real discovery builds AmsBackendIndx (the
    // MedusaHC rule again: try_create_mock() declines this value).
    if (is_mock_indx()) {
        read_indx_knobs();
        append_indx_objects(mock_objects);
    }

    append_skip_wrapper_objects(mock_objects);

    // Parse objects into hardware discovery (unified hardware access)
    discovery_.modify_hardware([&](PrinterDiscovery& hw) { hw.parse_objects(mock_objects); });

    // Mock accelerometer configuration for input shaper wizard testing
    // Real Klipper doesn't expose accelerometers in objects list (no get_status()),
    // so we simulate what parse_config_keys() would find from configfile.config
    json mock_config = mock_internal::get_mock_accel_config();
    // Bed screws — same story as the accelerometers: screws_tilt_adjust has no
    // get_status(), so Klipper never lists it and the capability is detected
    // from configfile.config. Without this the whole mock screws-tilt state
    // machine is unreachable in --test.
    mock_config["screws_tilt_adjust"] = {{"screw_thread", mock_internal::kMockScrewThread},
                                         {"speed", "50"},
                                         {"horizontal_move_z", "10"}};
    // Provide kinematics so bed_moves detection works
    mock_config["printer"] = {{"kinematics", kinematics()}};
    // Add gcode_macro entries for param detection (shared with configfile.config response)
    mock_config.merge_patch(mock_internal::get_mock_gcode_macro_config());
    mock_config.merge_patch(skip_wrapper_sections());
    // Probe section — shared with the configfile.config query/subscribe responses
    // so all three payloads describe the same probe.
    mock_config.merge_patch(mock_internal::get_mock_probe_config(printer_type_));
    // Stepper travel limits, matching the configfile.settings the query and
    // subscribe handlers report. The real discovery sequence derives the build
    // volume from these before detection runs, so the mock has to carry them or
    // --test detects against an empty volume the live path no longer sees.
    const auto axis_max = persona.axis_max;
    const auto axis_min = persona.axis_min;
    mock_config["stepper_x"] = {{"position_min", axis_min.x}, {"position_max", axis_max.x}};
    mock_config["stepper_y"] = {{"position_min", axis_min.y}, {"position_max", axis_max.y}};
    mock_config["stepper_z"] = {{"position_min", axis_min.z}, {"position_max", axis_max.z}};
    for (const auto& [name, settings] : extra_config_settings_.items()) {
        mock_config[name] = settings;
    }
    if (is_mock_indx()) {
        mock_config.merge_patch(indx_config_sections());
    }
    mock_config.merge_patch(medusa_config_sections());

    std::unordered_set<std::string> macros_snapshot;
    discovery_.modify_hardware([&](PrinterDiscovery& hw) {
        hw.parse_config_keys(mock_config);
        hw.parse_build_volume(mock_config);
        hw.parse_probe_xy_offset(mock_config);
        macros_snapshot = hw.macros();
    });

    // Populate macro param cache from mock config (same as real discovery sequence)
    helix::MacroParamCache::instance().populate_from_configfile(mock_config, macros_snapshot);

    spdlog::debug("[MoonrakerClientMock] Mock config: adxl345, resonance_tester, kinematics={}",
                  kinematics());

    // Populate printer objects for hardware discovery. Klipper lists each object
    // once; a persona's own lists can repeat an inherited default (a chamber
    // sensor, a chamber heater, a macro), so keep only the first.
    std::vector<std::string> all_objects;
    std::unordered_set<std::string> listed;
    for (const auto& obj : mock_objects) {
        std::string name = obj.get<std::string>();
        if (listed.insert(name).second) {
            all_objects.push_back(std::move(name));
        }
    }
    discovery_.modify_hardware([&](PrinterDiscovery& hw) { hw.set_printer_objects(all_objects); });
    update_cached_chamber_key();

    // Set mock MCU version data (after parse_objects which clears everything)
    const bool ebb_can = inherits(kEbbCanMcu);
    discovery_.modify_hardware([ebb_can](PrinterDiscovery& hw) {
        hw.set_mcu("stm32f446xx");
        if (ebb_can) {
            hw.set_mcu_list({"stm32f446xx", "stm32g0b1xx"});
            hw.set_mcu_versions(
                {{"mcu", "v0.12.0-155-g4cfa273e"}, {"mcu EBBCan", "v0.12.0-155-g4cfa273e"}});
        } else {
            hw.set_mcu_list({"stm32f446xx"});
            hw.set_mcu_versions({{"mcu", "v0.12.0-155-g4cfa273e"}});
        }
    });

    // Also populate filament_sensors vector for subscription (same as real parse_objects)
    discovery_.filament_sensors().clear();
    for (const auto& obj : mock_objects) {
        std::string name = obj.get<std::string>();
        if (name.rfind("filament_switch_sensor ", 0) == 0 ||
            name.rfind("filament_motion_sensor ", 0) == 0) {
            discovery_.filament_sensors().push_back(name);
        }
    }

    spdlog::debug("[MoonrakerClientMock] Hardware populated: {} macros, {} filament sensors",
                  discovery_.hardware().macros().size(), discovery_.filament_sensors().size());
}

void MoonrakerClientMock::rebuild_hardware_from_lists() {
    // See populate_capabilities(). The set_* helpers release their own lock before
    // calling this, so taking it here is a fresh acquisition, not a nested one.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    // Lightweight re-parse: build objects array from current discovery lists only.
    // Used by test helpers (set_heaters, set_fans, etc.) that need to update hardware()
    // without adding hardcoded common objects from populate_capabilities().

    // Apply additional_objects overrides to discovery lists
    // (e.g., temperature_fan chamber replacing heater_generic dragonbreath —
    // registry-matched, so backend-named heaters swap both ways)
    for (const auto& obj : additional_objects_) {
        if (is_chamber_heater_object(obj)) {
            override_chamber_heater(obj);
        }
    }

    // Build objects array from the corrected lists
    json objects = json::array();

    for (const auto& h : discovery_.heaters()) {
        objects.push_back(h);
    }
    for (const auto& f : discovery_.fans()) {
        objects.push_back(f);
    }
    for (const auto& s : discovery_.sensors()) {
        if (s.rfind("temperature_sensor ", 0) == 0 || s.rfind("temperature_fan ", 0) == 0 ||
            s.rfind("tmc2240 ", 0) == 0 || s.rfind("tmc2209 ", 0) == 0 ||
            s.rfind("tmc5160 ", 0) == 0) {
            objects.push_back(s);
        }
    }
    for (const auto& l : discovery_.leds()) {
        objects.push_back(l);
    }
    for (const auto& fs : discovery_.filament_sensors()) {
        objects.push_back(fs);
    }
    for (const auto& obj : additional_objects_) {
        objects.push_back(obj);
    }

    // Chamber-backend surfaces (bare diagnostics object, filter-fan pin) are
    // not derivable from the discovery lists — parse_objects() classifies
    // neither into one. Preserve the ones a previous populate materialized
    // (and only while the backend's heater itself survived the rebuild), or
    // the chamber status helpers go silent after set_heaters()/set_fans().
    {
        const auto hw_prev = discovery_.hardware();
        const auto* backend = chamber::backend_by_id(hw_prev.chamber_heater_backend_id());
        const std::string& heater = hw_prev.chamber_heater_name();
        if (backend && !heater.empty() &&
            std::find(objects.begin(), objects.end(), json(heater)) != objects.end()) {
            const auto& prev = hw_prev.printer_objects();
            for (const std::string& key : {std::string(backend->diagnostics_object()),
                                           std::string(backend->filter_fan_pin())}) {
                if (!key.empty() && std::find(prev.begin(), prev.end(), key) != prev.end() &&
                    std::find(objects.begin(), objects.end(), json(key)) == objects.end()) {
                    objects.push_back(key);
                }
            }
        }
    }

    discovery_.modify_hardware([&](PrinterDiscovery& hw) {
        hw.parse_objects(objects);
        // parse_objects() clears printer_objects_; repopulate from the same
        // array the way populate_capabilities() does (and the real discovery
        // sequence does at moonraker_discovery_sequence.cpp), or
        // objects.list and the chamber-backend status helpers go dark after
        // a rebuild.
        std::vector<std::string> all_objects;
        all_objects.reserve(objects.size());
        for (const auto& obj : objects) {
            all_objects.push_back(obj.get<std::string>());
        }
        hw.set_printer_objects(all_objects);
    });
    update_cached_chamber_key();
}

void MoonrakerClientMock::discover_printer(
    helix::SharedCallback<void()> on_complete,
    helix::SharedCallback<void(const std::string& reason)> on_error) {
    // The sequence finishes inside the locked call, but its outcome is
    // delivered after the lock is released: a caller's callback may read or
    // rebuild the very lists the lock protects. The base class keeps the
    // completion callback for force_reconnect(), so a later call goes straight
    // through.
    struct Outcome {
        std::atomic<bool> deferring{true};
        bool completed = false;
        std::optional<std::string> failure;
    };
    auto outcome = std::make_shared<Outcome>();
    {
        std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);
        MoonrakerClient::discover_printer(
            [outcome, on_complete]() {
                if (outcome->deferring) {
                    outcome->completed = true;
                } else if (on_complete) {
                    on_complete();
                }
            },
            [outcome, on_error](const std::string& reason) {
                if (outcome->deferring) {
                    outcome->failure = reason;
                } else if (on_error) {
                    on_error(reason);
                }
            });
    }
    outcome->deferring = false;
    if (outcome->completed && on_complete) {
        on_complete();
    } else if (outcome->failure && on_error) {
        on_error(*outcome->failure);
    }
}

void MoonrakerClientMock::populate_hardware() {
    // See populate_capabilities(). The clear()/assign below are exactly the writes
    // that freed string buffers out from under the simulation thread.
    std::lock_guard<std::mutex> discovery_lock(discovery_mutex_);

    const helix::mock::PersonaHardware& hardware = *helix::mock::descriptor(printer_type_).hardware;
    assign_names(discovery_.heaters(), hardware.heaters);
    assign_names(discovery_.sensors(), hardware.sensors);
    assign_names(discovery_.fans(), hardware.fans);
    assign_names(discovery_.leds(), hardware.leds);

    // Toolchanger mock mode (HELIX_MOCK_AMS=toolchanger): emulate a real Klipper
    // toolchanger with 4 distinct extruder heaters so ToolState::init_tools()
    // maps each of the 4 tools to its own extruder and the Nozzle Temps widget
    // renders the full 4-row multi-extruder layout. The bare "extruder" is
    // already present from the printer-type switch above; add extruder1/2/3.
    // Gated entirely on is_mock_toolchanger() so other AMS modes are unaffected.
    // MedusaHC joins this: a hotend changer is one hot end (and so one extruder
    // heater) per tool, which is also how PrinterDiscovery names its tools when
    // the controller-tn variant ships no [tool N] objects.
    if (is_mock_toolchanger() || is_mock_medusahc()) {
        const int tools = is_mock_medusahc() ? medusa_tool_count() : 4;
        for (int t = 1; t < tools; ++t) {
            const std::string ext = "extruder" + std::to_string(t);
            // Klipper exposes secondary extruders as both a heater and a sensor
            // under the bare name (matching the MULTI_EXTRUDER case convention).
            if (std::find(discovery_.heaters().begin(), discovery_.heaters().end(), ext) ==
                discovery_.heaters().end()) {
                discovery_.heaters().push_back(ext);
            }
            if (std::find(discovery_.sensors().begin(), discovery_.sensors().end(), ext) ==
                discovery_.sensors().end()) {
                discovery_.sensors().push_back(ext);
            }
        }
        spdlog::debug("[MoonrakerClientMock] {} mock: registered {} extruders",
                      is_mock_medusahc() ? "MedusaHC" : "Toolchanger", tools);
    }

    // Initialize LED states (all off by default)
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        led_states_.clear();
        for (const auto& led : discovery_.leds()) {
            if (led.rfind("output_pin ", 0) == 0)
                continue; // output_pin uses {value:} not color_data
            led_states_[led] = LedColor{0.0, 0.0, 0.0, 0.0};
        }
    }

    spdlog::trace("[MoonrakerClientMock] Populated hardware:");
    for (const auto& h : discovery_.heaters())
        spdlog::trace("  Heater: {}", h);
    for (const auto& s : discovery_.sensors())
        spdlog::trace("  Sensor: {}", s);
    for (const auto& f : discovery_.fans())
        spdlog::trace("  Fan: {}", f);
    for (const auto& l : discovery_.leds())
        spdlog::trace("  LED: {}", l);
}
