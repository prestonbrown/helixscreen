// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Happy Hare device settings: the DeviceActions the Device settings overlay renders,
// and the G-code each one sends.

#include "ams_backend_happy_hare.h"
#include "ams_device_action_value.h"
#include "happy_hare_status_parse.h"
#include "hh_defaults.h"
#include "translation_loader.h" // TR_NOOP

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix {

namespace {

/// The unit capability a device action needs.
std::optional<happy_hare::UnitFeature> feature_for_action(std::string_view id) {
    using happy_hare::UnitFeature;
    if (id == "servo_buzz" || id == "servo_up" || id == "servo_move" || id == "servo_down")
        return UnitFeature::Servo;
    if (id == "selector_speed")
        return UnitFeature::SelectorSpeed;
    if (id == "calibrate_encoder" || id == "calibrate_gates" || id == "clog_detection")
        return UnitFeature::Encoder;
    if (id == "sync_to_extruder")
        return UnitFeature::SyncToExtruder;
    if (id == "gear_from_buffer_speed")
        return UnitFeature::FilamentBuffer;
    return std::nullopt;
}

} // namespace

// ============================================================================
// Device Management
// ============================================================================

std::optional<std::string> AmsBackendHappyHare::clog_detection_mode_gcode(int mode,
                                                                          float det_length) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // v4 refuses the encoder mode on a unit with no encoder.
    const auto unit = test_config_unit_locked("clog_detection");
    if (!unit) {
        return std::nullopt;
    }
    std::string cmd = fmt::format("MMU_TEST_CONFIG {}={}",
                                  happy_hare::param_name("clog_detection", machine_layout_), mode);
    if (mode == 1 && det_length > 0) {
        cmd += fmt::format(" {}={:.1f}",
                           happy_hare::param_name("detection_length", machine_layout_), det_length);
    }
    return cmd + unit_suffix_locked(*unit);
}

std::optional<float> AmsBackendHappyHare::clog_detection_length_setting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_defaults_.detection_length;
}

std::vector<helix::printer::DeviceSection> AmsBackendHappyHare::get_device_sections() const {
    return helix::printer::hh_default_sections();
}

std::vector<helix::printer::DeviceAction> AmsBackendHappyHare::get_device_actions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    using namespace helix::printer;
    auto actions = hh_default_actions();

    // Status-backed, so it applies before the configfile has loaded.
    if (system_info_.spoolman_mode == SpoolmanMode::OFF) {
        for (auto& a : actions) {
            if (a.id == "spoolman_refresh") {
                a.enabled = false;
                a.disable_reason = TR_NOOP("Spoolman support is off in Happy Hare");
            }
        }
    }

    // Settings whose value comes from the configfile (and is written back with
    // MMU_TEST_CONFIG) wait for it to load; live commands do not.
    const bool config_loaded = config_defaults_.loaded;
    auto config_backed = [](const DeviceAction& a) {
        return a.type == ActionType::SLIDER || a.id == "sync_to_extruder" ||
               a.id == "clog_detection";
    };
    if (!config_loaded) {
        for (auto& a : actions) {
            if (config_backed(a)) {
                a.enabled = false;
                a.disable_reason = TR_NOOP("Loading configuration...");
            }
        }
    }

    // Helper: effective float value (user override > config default)
    auto eff_f = [&](const std::optional<float>& ovr, float def) -> double {
        return static_cast<double>(ovr.value_or(def));
    };
    auto eff_i = [&](const std::optional<int>& ovr, int def) -> int { return ovr.value_or(def); };

    // Overlay effective values onto actions
    for (auto& a : actions) {
        // Clog detection's value is the encoder mode the mmu status reports,
        // which needs no config; only its write waits.
        if (!config_loaded && config_backed(a) && a.id != "clog_detection") {
            continue;
        }
        // --- Speed sliders ---
        if (a.id == "gear_from_buffer_speed") {
            a.current_value = eff_f(user_overrides_.gear_from_buffer_speed,
                                    config_defaults_.gear_from_buffer_speed);
        } else if (a.id == "gear_from_spool_speed") {
            a.current_value = eff_f(user_overrides_.gear_from_spool_speed,
                                    config_defaults_.gear_from_spool_speed);
        } else if (a.id == "gear_unload_speed") {
            a.current_value =
                eff_f(user_overrides_.gear_unload_speed, config_defaults_.gear_unload_speed);
        } else if (a.id == "selector_speed") {
            a.current_value =
                eff_f(user_overrides_.selector_move_speed, config_defaults_.selector_move_speed);
        } else if (a.id == "extruder_load_speed") {
            a.current_value =
                eff_f(user_overrides_.extruder_load_speed, config_defaults_.extruder_load_speed);
        } else if (a.id == "extruder_unload_speed") {
            a.current_value = eff_f(user_overrides_.extruder_unload_speed,
                                    config_defaults_.extruder_unload_speed);
        }
        // --- Toolhead sliders ---
        else if (a.id == "toolhead_sensor_to_nozzle") {
            a.current_value = eff_f(user_overrides_.toolhead_sensor_to_nozzle,
                                    config_defaults_.toolhead_sensor_to_nozzle);
        } else if (a.id == "toolhead_extruder_to_nozzle") {
            a.current_value = eff_f(user_overrides_.toolhead_extruder_to_nozzle,
                                    config_defaults_.toolhead_extruder_to_nozzle);
        } else if (a.id == "toolhead_entry_to_extruder") {
            a.current_value = eff_f(user_overrides_.toolhead_entry_to_extruder,
                                    config_defaults_.toolhead_entry_to_extruder);
        } else if (a.id == "toolhead_ooze_reduction") {
            a.current_value = eff_f(user_overrides_.toolhead_ooze_reduction,
                                    config_defaults_.toolhead_ooze_reduction);
        }
        // --- Accessories ---
        else if (a.id == "espooler_mode") {
            // Status-backed: use live value if available
            if (!espooler_active_.empty()) {
                a.current_value = espooler_active_;
            }
        } else if (a.id == "clog_detection") {
            // The mmu status reports the encoder's mode; the configfile's is
            // what the MMU runs before its first frame.
            const int mode = flowguard_encoder_mode_ >= 0 ? flowguard_encoder_mode_
                                                          : config_defaults_.clog_detection;
            // Map int to display string
            switch (mode) {
            case 1:
                a.current_value = std::string("Manual");
                break;
            case 2:
                a.current_value = std::string("Auto");
                break;
            default:
                a.current_value = std::string("Off");
                break;
            }
        } else if (a.id == "sync_to_extruder") {
            int val = eff_i(user_overrides_.sync_to_extruder, config_defaults_.sync_to_extruder);
            a.current_value = (val != 0);
        }
        // --- Setup ---
        else if (a.id == "led_mode") {
            // Status-backed: use live LED exit_effect if available
            if (!led_exit_effect_.empty()) {
                a.current_value = led_exit_effect_;
            }
        }
    }

    // --- Hardware filtering: an action no unit's hardware takes is disabled ---
    for (auto& a : actions) {
        if (const auto feature = feature_for_action(a.id); feature && !unit_with_locked(*feature)) {
            a.enabled = false;
            switch (*feature) {
            case happy_hare::UnitFeature::Servo:
                a.disable_reason = TR_NOOP("No unit has a servo");
                break;
            case happy_hare::UnitFeature::SelectorSpeed:
                a.disable_reason = TR_NOOP("No unit has a moving selector");
                break;
            case happy_hare::UnitFeature::Encoder:
                a.disable_reason = TR_NOOP("No unit has an encoder");
                break;
            case happy_hare::UnitFeature::SyncToExtruder:
                a.disable_reason = TR_NOOP("Every unit keeps its filament gripped");
                break;
            case happy_hare::UnitFeature::FilamentBuffer:
                a.disable_reason = TR_NOOP("No unit has a filament buffer");
                break;
            }
        } else if ((a.id == "toolhead_sensor_to_nozzle" || a.id == "toolhead_entry_to_extruder") &&
                   !test_config_unit_locked(a.id)) {
            a.enabled = false;
            a.disable_reason = TR_NOOP("No sensor fitted for this distance");
        }
    }

    return actions;
}

AmsError AmsBackendHappyHare::do_execute_device_action(const std::string& action_id,
                                                       const std::any& value) {
    spdlog::info("[AMS HappyHare] Executing device action: {}", action_id);

    const auto get_action_range = [](const std::string& id) {
        return helix::printer::hh_default_action_range(id);
    };

    // --- Simple button actions (no value required) ---
    // `per_unit`: v4 resolves the unit from UNIT= (MMU_SERVO, MMU_CALIBRATE_GATE
    // ALL=1) or infers it from a selected gate (MMU_TEST_GRIP), so these name
    // a unit: the selected one, or for an action only some units' hardware
    // takes, the first unit that has it. The calibrations that act on the
    // selected gate's unit read no UNIT.
    struct ButtonAction {
        const char* id;
        const char* gcode;
        bool per_unit;
    };
    // clang-format off
    static const ButtonAction button_actions[] = {
        {"calibrate_bowden",    "MMU_CALIBRATE_BOWDEN",            false},
        {"calibrate_encoder",   "MMU_CALIBRATE_ENCODER",           false},
        {"calibrate_gear",      "MMU_CALIBRATE_GEAR",              false},
        {"calibrate_gates",     "MMU_CALIBRATE_GATES",             true},
        {"test_grip",           "MMU_TEST_GRIP",                   true},
        {"test_load",           "MMU_TEST_LOAD",                   false},
        {"test_move",           "MMU_TEST_MOVE",                   false},
        {"servo_buzz",          "MMU_SERVO",                       true},
        {"servo_up",            "MMU_SERVO POS=up",                true},
        {"servo_move",          "MMU_SERVO POS=move",              true},
        {"servo_down",          "MMU_SERVO POS=down",              true},
        {"reset_servo_counter", "MMU_STATS COUNTER=servo RESET=1", false},
        {"reset_blade_counter", "MMU_STATS COUNTER=cutter RESET=1", false},
    };
    // clang-format on
    for (const auto& action : button_actions) {
        if (action_id != action.id) {
            continue;
        }
        std::string cmd = action.gcode;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // v4 registers only MMU_CALIBRATE_GATE; MMU_CALIBRATE_GATES is a
            // config macro alias that sends no UNIT.
            if (action_id == "calibrate_gates" && is_v4_locked()) {
                cmd = "MMU_CALIBRATE_GATE ALL=1";
            }
            if (action.per_unit) {
                std::optional<int> unit = active_unit_locked();
                if (const auto feature = feature_for_action(action_id)) {
                    unit = unit_with_locked(*feature);
                }
                if (!unit) {
                    return AmsErrorHelper::not_supported(action_id);
                }
                cmd += unit_suffix_locked(*unit);
            }
        }
        return execute_gcode(cmd);
    }

    // Extruder-only moves drive the toolhead extruder, and HH puts no print
    // check on MMU_LOAD/MMU_UNLOAD; an EXTRUDER_ONLY unload forms a tip and
    // retracts over the part, so both are moves_machine and the base refuses
    // them during a print. HH heats the nozzle itself.
    if (action_id == "load_extruder" || action_id == "unload_extruder") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            AmsError precondition = check_preconditions();
            if (!precondition) {
                return precondition;
            }
        }
        return execute_gcode(action_id == "load_extruder" ? "MMU_LOAD EXTRUDER_ONLY=1"
                                                          : "MMU_UNLOAD EXTRUDER_ONLY=1");
    }

    // HH refuses MMU_SPOOLMAN outright while its spoolman_support is off.
    if (action_id == "spoolman_refresh") {
        if (!manages_active_spool()) {
            return AmsErrorHelper::not_supported("Spoolman support is off in Happy Hare");
        }
        return execute_gcode("MMU_SPOOLMAN REFRESH=1");
    }

    // --- LED mode dropdown ---
    if (action_id == "led_mode") {
        auto [mode, err] = printer::device_action_value<std::string>(value, "LED mode");
        if (!err)
            return err;
        return execute_gcode("MMU_LED EXIT_EFFECT=" + mode);
    }

    // MMU_TEST_CONFIG <param>=<value>, the parameter named as this install
    // spells it.
    auto test_config = [this](const char* key, const std::string& value) {
        std::optional<std::string> cmd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmd = test_config_command_locked(key, value);
        }
        if (!cmd) {
            return AmsErrorHelper::not_supported(std::string("Happy Hare parameter ") + key);
        }
        return execute_gcode(*cmd);
    };

    // --- Speed sliders (integer formatting) ---
    // clang-format off
    static const std::pair<const char*, const char*> speed_params[] = {
        {"gear_from_buffer_speed", "gear_from_buffer_speed"},
        {"gear_from_spool_speed",  "gear_from_spool_speed"},
        {"gear_unload_speed",      "gear_unload_speed"},
        {"selector_speed",         "selector_move_speed"},
        {"extruder_load_speed",    "extruder_load_speed"},
        {"extruder_unload_speed",  "extruder_unload_speed"},
    };
    // clang-format on
    for (const auto& [id, key] : speed_params) {
        if (action_id == id) {
            auto [speed, err] = printer::device_action_value<double>(value, "speed");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(id);
            speed = std::clamp(speed, static_cast<double>(lo), static_cast<double>(hi));
            auto result = test_config(key, fmt::format("{:.0f}", speed));
            if (result.success()) {
                save_override(key, static_cast<float>(speed));
            }
            return result;
        }
    }

    // --- Toolhead distance sliders (one decimal place) ---
    static const char* const toolhead_params[] = {
        "toolhead_sensor_to_nozzle",
        "toolhead_extruder_to_nozzle",
        "toolhead_entry_to_extruder",
        "toolhead_ooze_reduction",
    };
    for (const char* key : toolhead_params) {
        if (action_id == key) {
            auto [dist, err] = printer::device_action_value<double>(value, "distance");
            if (!err)
                return err;
            auto [lo, hi] = get_action_range(key);
            dist = std::clamp(dist, static_cast<double>(lo), static_cast<double>(hi));
            auto result = test_config(key, fmt::format("{:.1f}", dist));
            if (result.success()) {
                save_override(action_id, static_cast<float>(dist));
            }
            return result;
        }
    }

    // --- sync_to_extruder toggle ---
    if (action_id == "sync_to_extruder") {
        auto [enable, err] = printer::device_action_value<bool>(value, "sync state");
        if (!err)
            return err;
        int val = enable ? 1 : 0;
        auto result = test_config("sync_to_extruder", std::to_string(val));
        if (result.success()) {
            save_override(action_id, val);
        }
        return result;
    }

    // --- eSpooler mode dropdown ---
    if (action_id == "espooler_mode") {
        auto [mode, err] = printer::device_action_value<std::string>(value, "eSpooler mode");
        if (!err)
            return err;
        return execute_gcode("MMU_ESPOOLER OPERATION=" + mode);
    }

    // --- Clog detection dropdown ---
    if (action_id == "clog_detection") {
        auto [mode_str, err] =
            printer::device_action_value<std::string>(value, "clog detection mode");
        if (!err)
            return err;
        int mode_int = 0;
        if (mode_str == "Manual")
            mode_int = 1;
        else if (mode_str == "Auto")
            mode_int = 2;
        // The mmu status reports the mode back, so nothing is kept here.
        return test_config("clog_detection", std::to_string(mode_int));
    }

    // --- Runtime gear-motor sync (live action, distinct from config sync_to_extruder) ---
    if (action_id == "gear_sync") {
        auto [enable, err] = printer::device_action_value<bool>(value, "gear sync state");
        if (!err)
            return err;
        return execute_gcode(enable ? "MMU_SYNC_GEAR_MOTOR SYNC=1" : "MMU_SYNC_GEAR_MOTOR SYNC=0");
    }

    // --- Motors toggle ---
    if (action_id == "motors_toggle") {
        auto [enable, err] = printer::device_action_value<bool>(value, "motor state");
        if (!err)
            return err;
        std::string cmd = enable ? "MMU_MOTORS_ON" : "MMU_MOTORS_OFF";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmd += unit_suffix_locked(kAllUnits);
        }
        return execute_gcode(cmd);
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}

} // namespace helix
