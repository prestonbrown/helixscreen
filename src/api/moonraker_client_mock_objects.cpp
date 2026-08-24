// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock_internal.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <chrono>
#include <string>

namespace mock_internal {

static bool is_mock_kalico() {
    const char* env = std::getenv("HELIX_MOCK_KALICO");
    return env && std::string(env) == "1";
}

json get_mock_gcode_macro_config() {
    json cfg;
    cfg["gcode_macro clean_nozzle"] = {
        {"gcode", "{% set PURGE_LEN = params.PURGE_LEN|default(10)|float %}\n"
                  "{% set PURGE_TEMP = params.PURGE_TEMP|default(240)|int %}\nG1 ..."},
        {"description", "Wipe and purge the nozzle using the brush at the rear of the bed, "
                        "with configurable purge length and temperature"},
        {"variable_start_x", "265"},
        {"variable_start_y", "298"},
        {"variable_wipe_qty", "4"}};
    cfg["gcode_macro print_start"] = {
        {"gcode", "{% set BED_TEMP = params.BED_TEMP|default(60)|float %}\n"
                  "{% set EXTRUDER_TEMP = params.EXTRUDER_TEMP|default(190)|float %}\n"
                  "G28\nM190 S{BED_TEMP}"},
        {"description", "Home, heat bed and nozzle, then start printing"}};
    cfg["gcode_macro start_print"] = {
        {"gcode", "{% set BED_TEMP = params.BED_TEMP|default(60)|float %}\n"
                  "{% set EXTRUDER_TEMP = params.EXTRUDER_TEMP|default(200)|float %}\n"
                  "{% set CHAMBER_TEMP = params.CHAMBER_TEMP|default(0)|float %}\nG28"},
        {"description", "Full print start sequence: homes all axes, heats bed to target, "
                        "waits for chamber soak if specified, performs QGL and adaptive bed "
                        "mesh, then heats nozzle and primes"}};
    cfg["gcode_macro pause"] = {{"gcode", "SAVE_GCODE_STATE\nBASE_PAUSE"}};
    cfg["gcode_macro resume"] = {{"gcode", "RESTORE_GCODE_STATE\nBASE_RESUME"}};
    cfg["gcode_macro cancel_print"] = {{"gcode", "TURN_OFF_HEATERS\nCANCEL_PRINT_BASE"},
                                       {"description", "Cancel print and turn off heaters"}};
    cfg["gcode_macro end_print"] = {{"gcode", "G91\nG1 Z5\nG90\nTURN_OFF_HEATERS"}};
    cfg["gcode_macro load_filament"] = {
        {"gcode", "{% set TEMP = params.TEMP|default(220)|int %}\n"
                  "{% set SPEED = params.SPEED|default(300)|int %}\nM109 S{TEMP}"},
        {"description", "Heat nozzle and load filament"}};
    cfg["gcode_macro unload_filament"] = {{"gcode",
                                           "{% set TEMP = params.TEMP|default(220)|int %}\n"
                                           "M109 S{TEMP}\nG1 E-50 F300"}};
    return cfg;
}

json get_mock_accel_config() {
    return {{"adxl345", json::object()}, {"resonance_tester", json::object()}};
}

json get_mock_probe_config() {
    const char* probe_env = std::getenv("HELIX_MOCK_PROBE_TYPE");
    const std::string probe_type = (probe_env && probe_env[0]) ? probe_env : "cartographer";

    json cfg = json::object();
    if (probe_type == "none") {
        return cfg;
    }
    if (probe_type == "cartographer") {
        cfg["cartographer"] = {{"z_offset", "0.000"}, {"speed", "5"}};
    } else if (probe_type == "beacon") {
        cfg["beacon"] = {{"z_offset", "0.000"}, {"speed", "5"}};
    } else if (probe_type == "bltouch") {
        cfg["bltouch"] = {
            {"z_offset", "-1.850"}, {"x_offset", "-40.0"}, {"y_offset", "-10.0"}, {"speed", "5"}};
    } else if (probe_type == "loadcell") {
        // Status reports z_offset: null for this one — the config is the only
        // place the persisted offset exists.
        cfg["probe"] = {{"z_offset", "-0.185"}, {"speed", "5"}};
    } else {
        // tap, klicky, standard, ... → generic [probe]
        cfg["probe"] = {{"z_offset", "-0.250"}, {"speed", "5"}};
    }
    return cfg;
}

// Minimal Happy Hare "mmu" status for --real-ams: a static 4-gate setup with a
// mix of loaded/empty gates. gate_status is the load-bearing field — it's what
// AmsBackendHappyHare::parse_mmu_state() uses to size the slot registry and
// flip slots_.is_initialized(); the rest just refines what the AMS panel shows.
// Values proven correct against AmsBackendHappyHare's own unit test fixture
// (tests/unit/test_ams_backend_happy_hare.cpp, "v3 data with no v4 fields").
json get_mock_mmu_status() {
    return {{"gate", 2},
            {"tool", 2},
            {"filament", "Loaded"},
            {"action", "Idle"},
            {"filament_pos", 8},
            {"has_bypass", true},
            {"gate_status", {1, 0, 2, 1}},
            {"gate_color_rgb", {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}},
            {"gate_material", {"PLA", "PETG", "ABS", "TPU"}},
            {"ttg_map", {0, 1, 2, 3}},
            {"endless_spool_groups", {0, 0, 1, 1}}};
}

void register_object_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // printer.objects.list - List available printer objects
    // When Klippy is in STARTUP or ERROR state, Klipper returns JSON-RPC error -32601
    registry["printer.objects.list"] =
        [](MoonrakerClientMock* self, const json& /*params*/,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        auto klippy = self->get_klippy_state();

        // When Klippy is not ready (STARTUP/ERROR), Klipper returns Method not found
        if (klippy == MoonrakerClientMock::KlippyState::STARTUP ||
            klippy == MoonrakerClientMock::KlippyState::ERROR) {
            spdlog::debug("[MoonrakerClientMock] printer.objects.list: Klippy not ready, "
                          "returning error -32601");
            if (error_cb) {
                MoonrakerError err;
                err.code = -32601;
                err.message = "Method not found";
                error_cb(err);
            }
            return true;
        }

        // Build the object list from the mock's discovered hardware
        // This mirrors what populate_capabilities() populates
        const auto& hw = self->hardware();
        json objects = json::array();
        for (const auto& obj : hw.printer_objects()) {
            objects.push_back(obj);
        }

        spdlog::debug("[MoonrakerClientMock] printer.objects.list: returning {} objects",
                      objects.size());

        if (success_cb) {
            json response = {{"result", {{"objects", objects}}}};
            success_cb(response);
        }
        return true;
    };

    // printer.objects.query - Query printer object state
    registry["printer.objects.query"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)error_cb;
        json status_obj = json::object();

        // Check what objects are being queried
        if (params.contains("objects")) {
            auto& objects = params["objects"];

            // webhooks state (for is_printer_ready)
            if (objects.contains("webhooks")) {
                auto klippy = self->get_klippy_state();
                std::string state_str = "ready";
                switch (klippy) {
                case MoonrakerClientMock::KlippyState::STARTUP:
                    state_str = "startup";
                    break;
                case MoonrakerClientMock::KlippyState::SHUTDOWN:
                    state_str = "shutdown";
                    break;
                case MoonrakerClientMock::KlippyState::ERROR:
                    state_str = "error";
                    break;
                default:
                    break;
                }
                status_obj["webhooks"] = {{"state", state_str}};
            }

            // print_stats (for get_print_state)
            if (objects.contains("print_stats")) {
                // Use get_print_phase to derive print state string
                auto phase = self->get_print_phase();
                std::string state_str = "standby";
                switch (phase) {
                case MoonrakerClientMock::MockPrintPhase::IDLE:
                    state_str = "standby";
                    break;
                case MoonrakerClientMock::MockPrintPhase::PREHEAT:
                case MoonrakerClientMock::MockPrintPhase::PRINTING:
                    state_str = "printing";
                    break;
                case MoonrakerClientMock::MockPrintPhase::PAUSED:
                    state_str = "paused";
                    break;
                case MoonrakerClientMock::MockPrintPhase::COMPLETE:
                    state_str = "complete";
                    break;
                case MoonrakerClientMock::MockPrintPhase::CANCELLED:
                    state_str = "cancelled";
                    break;
                case MoonrakerClientMock::MockPrintPhase::ERROR:
                    state_str = "error";
                    break;
                }
                status_obj["print_stats"] = {{"state", state_str}};
            }

            // configfile (for update_safety_limits_from_printer + input shaper config)
            if (objects.contains("configfile")) {
                // Build config section with input_shaper if configured
                json config_section = {};

                // Accelerometer. The subscribe handler below and
                // populate_capabilities() both already report it; without it
                // here the query handler was the odd one out, so the discovery
                // sequence saw a printer with no accelerometer.
                config_section.merge_patch(get_mock_accel_config());

                if (self->is_input_shaper_configured()) {
                    config_section["input_shaper"] = self->build_input_shaper_config();
                }

                // Bed screws. PrinterDiscovery detects the capability from
                // `config` (the section has no get_status(), so it never appears
                // in objects/list), while the screws-tilt panel reads the thread
                // from `settings`. The mock needs both or --test can't reach the
                // panel at all.
                config_section["screws_tilt_adjust"] = {{"screw_thread", MOCK_SCREW_THREAD},
                                                        {"speed", "50"},
                                                        {"horizontal_move_z", "10"}};

                // Add LED effect configs to config section
                config_section["led_effect breathing"] = {{"leds", "neopixel:chamber_light"},
                                                          {"autostart", "false"},
                                                          {"frame_rate", "24"}};
                config_section["led_effect fire_comet"] = {
                    {"leds", "neopixel:chamber_light (1-10)"},
                    {"autostart", "false"},
                    {"frame_rate", "24"}};
                config_section["led_effect rainbow"] = {
                    {"leds", "neopixel:status_led"}, {"autostart", "false"}, {"frame_rate", "24"}};
                config_section["led_effect static_white"] = {{"leds", "neopixel:chamber_light"},
                                                             {"autostart", "false"},
                                                             {"frame_rate", "24"}};

                // Gcode macro templates for param detection testing
                config_section.merge_patch(get_mock_gcode_macro_config());

                // Probe section — where ProbeSensorManager::discover_from_config()
                // reads z_offset from on the real discovery path.
                config_section.merge_patch(get_mock_probe_config());

                // Build extruder settings based on HELIX_MOCK_KALICO env var
                json extruder_settings = {{"min_temp", 0.0},
                                          {"max_temp", self->get_extruder_max_temp()},
                                          {"min_extrude_temp", 170.0}};
                if (is_mock_kalico()) {
                    extruder_settings["control"] = "mpc";
                    extruder_settings["heater_power"] = 50.0;
                } else {
                    extruder_settings["control"] = "pid";
                    extruder_settings["pid_kp"] = 22.865;
                    extruder_settings["pid_ki"] = 1.292;
                    extruder_settings["pid_kd"] = 101.178;
                }

                // Probe-equipped printers (endstop_pin: probe:z_virtual_endstop)
                // get a JSON null here rather than a number — see
                // set_stepper_z_endstop_null().
                json stepper_z_settings = {{"position_min", 0.0}, {"position_max", MOCK_BED_Z_MAX}};
                if (self->is_stepper_z_endstop_null()) {
                    stepper_z_settings["position_endstop"] = nullptr;
                } else {
                    stepper_z_settings["position_endstop"] = 235.0;
                }

                status_obj["configfile"] = {
                    {"settings",
                     {{"printer", {{"max_velocity", 500.0}, {"max_accel", 10000.0}}},
                      {"stepper_x",
                       {{"position_min", MOCK_BED_X_MIN}, {"position_max", MOCK_BED_X_MAX}}},
                      {"stepper_y",
                       {{"position_min", MOCK_BED_Y_MIN}, {"position_max", MOCK_BED_Y_MAX}}},
                      {"stepper_z", stepper_z_settings},
                      {"extruder", extruder_settings},
                      // Resonance sweep bounds — the input shaper collector
                      // reads these to scale sweep progress to the range this
                      // printer will actually test.
                      {"resonance_tester",
                       {{"min_freq", self->get_resonance_min_freq()},
                        {"max_freq", self->get_resonance_max_freq()},
                        {"accel_per_hz", 75.0},
                        {"hz_per_sec", 1.0}}},
                      // Bed screw geometry — the screws-tilt panel reads
                      // screw_thread from here to size its level tolerance.
                      {"screws_tilt_adjust",
                       {{"screw_thread", MOCK_SCREW_THREAD},
                        {"speed", 50.0},
                        {"horizontal_move_z", 10.0}}},
                      {"heater_bed",
                       {{"min_temp", 0.0},
                        {"max_temp", 120.0},
                        {"control", "pid"},
                        {"pid_kp", 73.517},
                        {"pid_ki", 1.132},
                        {"pid_kd", 1194.093}}}}},
                    {"config", config_section}};

                // [bed_mesh] probe_count — the print-start collector's
                // entry-time query reads this to size the mesh denominator.
                if (const auto* probe_count = self->config_bed_mesh_probe_count()) {
                    status_obj["configfile"]["settings"]["bed_mesh"] = {
                        {"probe_count", json::array({probe_count->first, probe_count->second})}};
                }
            }

            // toolhead (for get_machine_limits)
            if (objects.contains("toolhead")) {
                status_obj["toolhead"] = {{"max_velocity", 500.0},
                                          {"max_accel", 10000.0},
                                          {"max_accel_to_decel", 5000.0},
                                          {"square_corner_velocity", 5.0},
                                          {"max_z_velocity", 40.0},
                                          {"max_z_accel", 1000.0},
                                          {"position", {0.0, 0.0, 0.0, 0.0}},
                                          {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                          {"axis_maximum", {235.0, 235.0, 250.0, 0.0}},
                                          {"homed_axes", "xyz"}};
            }

            // stepper_enable (for motors_enabled state - immediate response to M84)
            if (objects.contains("stepper_enable")) {
                bool enabled = self->are_motors_enabled();
                status_obj["stepper_enable"] = {{"steppers",
                                                 {{"stepper_x", enabled},
                                                  {"stepper_y", enabled},
                                                  {"stepper_z", enabled},
                                                  {"extruder", enabled}}}};
            }

            // idle_timeout (for printer activity state)
            if (objects.contains("idle_timeout")) {
                // Derive state from print phase and idle_timeout_triggered:
                // - "Printing" when active print in progress
                // - "Idle" when idle timeout has triggered
                // - "Ready" when active but not printing
                std::string idle_state;
                auto phase = self->get_print_phase();
                if (phase == MoonrakerClientMock::MockPrintPhase::PRINTING ||
                    phase == MoonrakerClientMock::MockPrintPhase::PREHEAT) {
                    idle_state = "Printing";
                } else if (self->is_idle_timeout_triggered()) {
                    idle_state = "Idle";
                } else {
                    idle_state = "Ready";
                }
                status_obj["idle_timeout"] = {{"state", idle_state}};
            }

            // mmu (Happy Hare MMU status — --real-ams)
            if (objects.contains("mmu")) {
                status_obj["mmu"] = get_mock_mmu_status();
            }

            // MCU objects (for discovery - chip type and firmware version)
            for (const auto& [key, val] : objects.items()) {
                if (key == "mcu" || key.rfind("mcu ", 0) == 0) {
                    json mcu_obj = json::object();
                    // Provide mock mcu_constants and mcu_version
                    if (key == "mcu") {
                        mcu_obj["mcu_constants"] = {{"MCU", "stm32f446xx"}};
                        mcu_obj["mcu_version"] = "v0.12.0-155-g4cfa273e";
                    } else {
                        // Secondary MCU (e.g., "mcu EBBCan")
                        mcu_obj["mcu_constants"] = {{"MCU", "stm32g0b1xx"}};
                        mcu_obj["mcu_version"] = "v0.12.0-155-g4cfa273e";
                    }
                    status_obj[key] = mcu_obj;
                }
            }
        }

        if (success_cb) {
            json response = {{"result", {{"status", status_obj}}}};
            success_cb(response);
        }
        return true;
    };

    // printer.objects.subscribe - Subscribe to printer object updates
    // Returns initial state with eventtime (subsequent updates come via notify_status_update)
    registry["printer.objects.subscribe"] =
        [](MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           std::function<void(const MoonrakerError&)> error_cb) -> bool {
        (void)error_cb;

        // Get current time as eventtime (seconds since epoch with microsecond precision)
        auto now = std::chrono::system_clock::now();
        auto epoch = now.time_since_epoch();
        double eventtime =
            std::chrono::duration_cast<std::chrono::microseconds>(epoch).count() / 1000000.0;

        json status_obj = json::object();

        // Check what objects are being subscribed to
        if (params.contains("objects")) {
            auto& objects = params["objects"];

            // webhooks state
            if (objects.contains("webhooks")) {
                auto klippy = self->get_klippy_state();
                std::string state_str = "ready";
                switch (klippy) {
                case MoonrakerClientMock::KlippyState::STARTUP:
                    state_str = "startup";
                    break;
                case MoonrakerClientMock::KlippyState::SHUTDOWN:
                    state_str = "shutdown";
                    break;
                case MoonrakerClientMock::KlippyState::ERROR:
                    state_str = "error";
                    break;
                default:
                    break;
                }
                status_obj["webhooks"] = {{"state", state_str}, {"state_message", ""}};
            }

            // print_stats
            if (objects.contains("print_stats")) {
                auto phase = self->get_print_phase();
                std::string state_str = "standby";
                switch (phase) {
                case MoonrakerClientMock::MockPrintPhase::IDLE:
                    state_str = "standby";
                    break;
                case MoonrakerClientMock::MockPrintPhase::PREHEAT:
                case MoonrakerClientMock::MockPrintPhase::PRINTING:
                    state_str = "printing";
                    break;
                case MoonrakerClientMock::MockPrintPhase::PAUSED:
                    state_str = "paused";
                    break;
                case MoonrakerClientMock::MockPrintPhase::COMPLETE:
                    state_str = "complete";
                    break;
                case MoonrakerClientMock::MockPrintPhase::CANCELLED:
                    state_str = "cancelled";
                    break;
                case MoonrakerClientMock::MockPrintPhase::ERROR:
                    state_str = "error";
                    break;
                }
                status_obj["print_stats"] = {
                    {"state", state_str},
                    {"filename", ""},
                    {"total_duration", 0.0},
                    {"print_duration", 0.0},
                    {"filament_used", 0.0},
                    {"message", ""},
                    {"info", {{"total_layer", nullptr}, {"current_layer", nullptr}}}};
            }

            // heater_bed
            if (objects.contains("heater_bed")) {
                status_obj["heater_bed"] = {{"temperature", 25.0}, {"target", 0.0}, {"power", 0.0}};
            }

            // extruder
            if (objects.contains("extruder")) {
                status_obj["extruder"] = {{"temperature", 25.0},
                                          {"target", 0.0},
                                          {"power", 0.0},
                                          {"pressure_advance", 0.04}};
            }

            // Toolchanger secondary extruders (HELIX_MOCK_AMS=toolchanger registers
            // extruder1/2/3 as heaters). Distinct initial values so each heating
            // state renders a different color in the Nozzle Temps widget:
            //   extruder1 HEATING (150 → 250), extruder2 AT-TEMP (248 → 250),
            //   extruder3 COOLING (200, no target). The contains() guard is
            //   self-limiting: these heaters only exist in toolchanger mode.
            if (objects.contains("extruder1")) {
                status_obj["extruder1"] = {{"temperature", 150.0},
                                           {"target", 250.0},
                                           {"power", 0.8},
                                           {"pressure_advance", 0.04}};
            }
            if (objects.contains("extruder2")) {
                status_obj["extruder2"] = {{"temperature", 248.0},
                                           {"target", 250.0},
                                           {"power", 0.2},
                                           {"pressure_advance", 0.04}};
            }
            if (objects.contains("extruder3")) {
                status_obj["extruder3"] = {{"temperature", 200.0},
                                           {"target", 0.0},
                                           {"power", 0.0},
                                           {"pressure_advance", 0.04}};
            }

            // toolhead
            if (objects.contains("toolhead")) {
                status_obj["toolhead"] = {{"max_velocity", 500.0},
                                          {"max_accel", 10000.0},
                                          {"max_accel_to_decel", 5000.0},
                                          {"square_corner_velocity", 5.0},
                                          {"position", {0.0, 0.0, 0.0, 0.0}},
                                          {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                          {"axis_maximum", {235.0, 235.0, 250.0, 0.0}},
                                          {"homed_axes", "xyz"},
                                          {"print_time", 0.0},
                                          {"estimated_print_time", 0.0},
                                          {"extruder", "extruder"}};
            }

            // virtual_sdcard
            if (objects.contains("virtual_sdcard")) {
                status_obj["virtual_sdcard"] = {{"file_path", ""},
                                                {"progress", 0.0},
                                                {"is_active", false},
                                                {"file_position", 0}};
            }

            // fan (part cooling fan)
            if (objects.contains("fan")) {
                status_obj["fan"] = {{"speed", 0.0}, {"rpm", nullptr}};
            }

            // Print-start / state macros — the discovery sequence subscribes to
            // these to learn when the slicer prep phase has completed and the
            // actual print has started. Emit the boolean flag fields the
            // PrintStartCollector parser expects.
            if (objects.contains("gcode_macro _START_PRINT")) {
                status_obj["gcode_macro _START_PRINT"] = {{"print_started", false}};
            }
            if (objects.contains("gcode_macro START_PRINT")) {
                status_obj["gcode_macro START_PRINT"] = {{"preparation_done", false}};
            }
            if (objects.contains("gcode_macro _HELIX_STATE")) {
                status_obj["gcode_macro _HELIX_STATE"] = {{"print_started", false}};
            }

            // fan_feedback (Creality tachometer module — fan0_speed..fan9_speed RPM).
            if (objects.contains("fan_feedback")) {
                status_obj["fan_feedback"] = {{"fan0_speed", 0.0}, {"fan1_speed", 0.0},
                                              {"fan2_speed", 0.0}, {"fan3_speed", 0.0},
                                              {"fan4_speed", 0.0}, {"fan5_speed", 0.0},
                                              {"fan6_speed", 0.0}, {"fan7_speed", 0.0},
                                              {"fan8_speed", 0.0}, {"fan9_speed", 0.0}};
            }

            // mmu (Happy Hare MMU status — --real-ams)
            if (objects.contains("mmu")) {
                status_obj["mmu"] = get_mock_mmu_status();
            }

            // Width sensors (hall_filament_width_sensor, tsl1401cl_filament_width_sensor)
            for (auto it = objects.begin(); it != objects.end(); ++it) {
                if (it.key().find("filament_width_sensor") != std::string::npos ||
                    it.key().find("fila_switch") != std::string::npos) {
                    status_obj[it.key()] = {
                        {"Diameter", 1.75}, {"Raw", 500.0}, {"is_active", true}};
                }
            }

            // Filament runout sensors — emit detection_count alongside the
            // detected/enabled flags so the FilamentSensorManager reads it.
            for (auto it = objects.begin(); it != objects.end(); ++it) {
                if (it.key().rfind("filament_switch_sensor ", 0) == 0 ||
                    it.key().rfind("filament_motion_sensor ", 0) == 0) {
                    status_obj[it.key()] = {
                        {"filament_detected", true}, {"enabled", true}, {"detection_count", 0}};
                }
            }

            // Toolchanger + per-tool objects — emit when discovered so
            // ToolState parsers can populate AmsState.
            if (objects.contains("toolchanger")) {
                status_obj["toolchanger"] = {
                    {"status", "ready"}, {"tool_number", 0}, {"tool_numbers", json::array({0})}};
            }
            for (auto it = objects.begin(); it != objects.end(); ++it) {
                if (it.key().rfind("tool ", 0) == 0) {
                    // Faithfully map each tool to its own extruder (T0→extruder,
                    // T1→extruder1, ...) when the matching heater is registered
                    // (toolchanger mode). Not read by ToolState — which maps by
                    // sorted heater index — but keeps the tool object self-consistent.
                    std::string extruder_for_tool = "extruder";
                    const std::string tool_suffix = it.key().substr(5); // after "tool "
                    if (tool_suffix.size() >= 2 && tool_suffix[0] == 'T' &&
                        std::isdigit(static_cast<unsigned char>(tool_suffix[1]))) {
                        int tool_idx = tool_suffix[1] - '0';
                        if (tool_idx > 0) {
                            std::string candidate = "extruder" + std::to_string(tool_idx);
                            if (objects.contains(candidate)) {
                                extruder_for_tool = candidate;
                            }
                        }
                    }
                    status_obj[it.key()] = {{"active", false},
                                            {"mounted", true},
                                            {"detect_state", "OK"},
                                            {"gcode_x_offset", 0.0},
                                            {"gcode_y_offset", 0.0},
                                            {"gcode_z_offset", 0.0},
                                            {"extruder", extruder_for_tool},
                                            {"fan", "fan"}};
                }
            }

            // gcode_move
            if (objects.contains("gcode_move")) {
                status_obj["gcode_move"] = {{"speed", 100.0},
                                            {"speed_factor", 1.0},
                                            {"extrude_factor", 1.0},
                                            {"absolute_coordinates", true},
                                            {"absolute_extrude", true},
                                            {"homing_origin", {0.0, 0.0, 0.0, 0.0}},
                                            {"position", {0.0, 0.0, 0.0, 0.0}},
                                            {"gcode_position", {0.0, 0.0, 0.0, 0.0}}};
            }

            // stepper_enable (for motor state)
            if (objects.contains("stepper_enable")) {
                bool enabled = self->are_motors_enabled();
                status_obj["stepper_enable"] = {{"steppers",
                                                 {{"stepper_x", enabled},
                                                  {"stepper_y", enabled},
                                                  {"stepper_z", enabled},
                                                  {"extruder", enabled}}}};
            }

            // idle_timeout (for activity state)
            if (objects.contains("idle_timeout")) {
                std::string idle_state;
                auto phase = self->get_print_phase();
                if (phase == MoonrakerClientMock::MockPrintPhase::PRINTING ||
                    phase == MoonrakerClientMock::MockPrintPhase::PREHEAT) {
                    idle_state = "Printing";
                } else if (self->is_idle_timeout_triggered()) {
                    idle_state = "Idle";
                } else {
                    idle_state = "Ready";
                }
                status_obj["idle_timeout"] = {{"state", idle_state}, {"printing_time", 0.0}};
            }

            // motion_report (for live position updates)
            if (objects.contains("motion_report")) {
                status_obj["motion_report"] = {{"live_position", {0.0, 0.0, 0.0, 0.0}},
                                               {"live_velocity", 0.0},
                                               {"live_extruder_velocity", 0.0}};
            }

            // display_status (for progress/message display)
            if (objects.contains("display_status")) {
                status_obj["display_status"] = {{"progress", 0.0}, {"message", nullptr}};
            }

            // LED effect objects (for tracking enabled state)
            for (auto it = objects.begin(); it != objects.end(); ++it) {
                if (it.key().rfind("led_effect ", 0) == 0) {
                    status_obj[it.key()] = {
                        {"enabled", false}, {"run_complete", false}, {"frame_rate", 24.0}};
                }
            }

            // configfile (printer configuration)
            if (objects.contains("configfile")) {
                // Build extruder settings based on HELIX_MOCK_KALICO env var
                json extruder_settings2 = {
                    {"min_temp", 0.0}, {"max_temp", 300.0}, {"min_extrude_temp", 170.0}};
                if (is_mock_kalico()) {
                    extruder_settings2["control"] = "mpc";
                    extruder_settings2["heater_power"] = 50.0;
                } else {
                    extruder_settings2["control"] = "pid";
                    extruder_settings2["pid_kp"] = 22.865;
                    extruder_settings2["pid_ki"] = 1.292;
                    extruder_settings2["pid_kd"] = 101.178;
                }

                status_obj["configfile"] = {
                    {"settings",
                     {{"printer", {{"max_velocity", 500.0}, {"max_accel", 10000.0}}},
                      {"stepper_x",
                       {{"position_min", MOCK_BED_X_MIN}, {"position_max", MOCK_BED_X_MAX}}},
                      {"stepper_y",
                       {{"position_min", MOCK_BED_Y_MIN}, {"position_max", MOCK_BED_Y_MAX}}},
                      {"stepper_z",
                       {{"position_min", 0.0},
                        {"position_max", MOCK_BED_Z_MAX},
                        {"position_endstop", 235.0}}},
                      {"extruder", extruder_settings2},
                      {"resonance_tester",
                       {{"min_freq", self->get_resonance_min_freq()},
                        {"max_freq", self->get_resonance_max_freq()},
                        {"accel_per_hz", 75.0},
                        {"hz_per_sec", 1.0}}},
                      {"heater_bed",
                       {{"min_temp", 0.0},
                        {"max_temp", 120.0},
                        {"control", "pid"},
                        {"pid_kp", 73.517},
                        {"pid_ki", 1.132},
                        {"pid_kd", 1194.093}}}}},
                    // config section contains raw Klipper config keys (used for sensor discovery)
                    {"config", [&]() {
                         json cfg = get_mock_accel_config();
                         if (self->is_input_shaper_configured()) {
                             cfg["input_shaper"] = self->build_input_shaper_config();
                         }
                         // LED effect configs for mock testing
                         cfg["led_effect breathing"] = {{"leds", "neopixel:chamber_light"},
                                                        {"autostart", "false"},
                                                        {"frame_rate", "24"}};
                         cfg["led_effect fire_comet"] = {{"leds", "neopixel:chamber_light (1-10)"},
                                                         {"autostart", "false"},
                                                         {"frame_rate", "24"}};
                         cfg["led_effect rainbow"] = {{"leds", "neopixel:status_led"},
                                                      {"autostart", "false"},
                                                      {"frame_rate", "24"}};
                         cfg["led_effect static_white"] = {{"leds", "neopixel:chamber_light"},
                                                           {"autostart", "false"},
                                                           {"frame_rate", "24"}};
                         // Gcode macro templates for param detection testing
                         cfg.merge_patch(get_mock_gcode_macro_config());
                         cfg.merge_patch(get_mock_probe_config());
                         return cfg;
                     }()}};
            }
        }

        if (success_cb) {
            json response = {{"result", {{"eventtime", eventtime}, {"status", status_obj}}}};
            success_cb(response);
        }
        return true;
    };
}

} // namespace mock_internal
