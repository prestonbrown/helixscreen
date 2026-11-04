// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <lvgl.h>
#include <string>
#include <vector>

using namespace helix;

// PID Calibration simulation
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_pid_calibrate(const std::string& gcode) {
    // Parse HEATER= parameter
    std::string heater = "extruder";
    auto heater_pos = gcode.find("HEATER=");
    if (heater_pos != std::string::npos) {
        size_t start = heater_pos + 7;
        size_t end = gcode.find(' ', start);
        heater = gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    // Parse TARGET= parameter
    int target = 200;
    auto target_pos = gcode.find("TARGET=");
    if (target_pos != std::string::npos) {
        target = std::stoi(gcode.substr(target_pos + 7));
    }

    spdlog::info("[MoonrakerClientMock] PID_CALIBRATE: heater={} target={}°C", heater, target);

    // Five Kalico PID sample lines (pid_calibrate.py format), then the result
    // in real Klipper's wording, 500ms apart.
    std::vector<std::string> lines;
    char buf[128];
    for (int cycle = 1; cycle <= 5; ++cycle) {
        float pwm = 0.5f - (cycle * 0.02f);
        float asymmetry = 0.3f - (cycle * 0.05f);
        // First two samples have n/a tolerance, then converging values
        if (cycle <= 2) {
            snprintf(buf, sizeof(buf), "sample:%d pwm:%.3f asymmetry:%.3f tolerance:n/a", cycle,
                     pwm, asymmetry);
        } else {
            float tolerance = 0.1f / cycle;
            snprintf(buf, sizeof(buf), "sample:%d pwm:%.3f asymmetry:%.3f tolerance:%.4f", cycle,
                     pwm, asymmetry, tolerance);
        }
        lines.emplace_back(buf);
    }

    float kp, ki, kd;
    if (heater == "heater_bed") {
        kp = 73.517f;
        ki = 1.132f;
        kd = 1194.093f;
    } else {
        kp = 22.865f;
        ki = 1.292f;
        kd = 101.178f;
    }
    snprintf(buf, sizeof(buf), "PID parameters: pid_Kp=%.3f pid_Ki=%.3f pid_Kd=%.3f", kp, ki, kd);
    play_console_lines(std::move(lines), 500,
                       [this, result = std::string(buf)] { dispatch_gcode_response(result); });

    return 0; // Success - results come asynchronously via gcode_response
    return std::nullopt;
}

// MPC Calibration simulation
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_mpc_calibrate(const std::string& gcode) {
    std::string heater = "extruder";
    auto heater_pos = gcode.find("HEATER=");
    if (heater_pos != std::string::npos) {
        size_t start = heater_pos + 7;
        size_t end = gcode.find(' ', start);
        heater = gcode.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    int fan_breakpoints = 3;
    auto fb_pos = gcode.find("FAN_BREAKPOINTS=");
    if (fb_pos != std::string::npos) {
        fan_breakpoints = std::stoi(gcode.substr(fb_pos + 16));
    }

    spdlog::info("[MoonrakerClientMock] MPC_CALIBRATE: heater={} fan_breakpoints={}", heater,
                 fan_breakpoints);

    // Settle, heatup, one line per fan breakpoint, then the result block,
    // 500ms apart.
    std::vector<std::string> lines = {"Waiting for heater to settle near ambient",
                                      "Performing heatup test"};
    for (int i = 1; i <= fan_breakpoints; ++i) {
        char buf[128];
        snprintf(buf, sizeof(buf), "measuring power usage with %d%% fan",
                 (i * 100) / fan_breakpoints);
        lines.emplace_back(buf);
    }
    play_console_lines(std::move(lines), 500, [this, fan_breakpoints] {
        dispatch_gcode_response("Finished MPC calibration");
        dispatch_gcode_response("block_heat_capacity=18.4321 [J/K]");
        dispatch_gcode_response("sensor_responsiveness=0.123456 [K/s/K]");
        dispatch_gcode_response("ambient_transfer=0.045678 [W/K]");
        if (fan_breakpoints > 0) {
            dispatch_gcode_response("fan_ambient_transfer=0.12, 0.18, 0.25 [W/K]");
        }
    });

    return 0; // Success - results come asynchronously via gcode_response
    return std::nullopt;
}

// SAVE_CONFIG simulation.
//
// Klipper's cmd_SAVE_CONFIG writes printer.cfg and then calls
// request_restart('restart') as its last act, so it never acks the command --
// the connection it would ack through is already going down. Moonraker fails
// the pending printer.gcode.script with 503 "Klippy Disconnected". A caller
// therefore sees a FAILED rpc for a save that succeeded, and learns the truth
// only from klippy returning READY.
//
// Both halves are modelled: the failed rpc carries the "Klippy Disconnected"
// message, and klippy actually restarts. No "ok" is dispatched, because real
// Klipper never sends one for this command.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_save_config(const std::string& gcode) {
    spdlog::info("[MoonrakerClientMock] SAVE_CONFIG - config written; restarting, and its "
                 "RPC is dropped (as on real Klipper)");
    // Write the file BEFORE restarting, in that order, because the restart
    // reloads every runtime value from the durable stores. Committing after
    // would reload the pre-save values and throw the save away.
    commit_pending_config();
    trigger_restart(/*is_firmware=*/false);
    {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_ = "Klippy Disconnected";
    }
    return 1;
    return std::nullopt;
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_bed_mesh(const std::string& gcode) {
    if (auto forced_profile = take_forced_mesh_calibration(gcode)) {
        calibrate_mock_mesh(*forced_profile);
    } else if (gcode.find("BED_MESH_CALIBRATE") != std::string::npos) {
        // Parse optional PROFILE= parameter
        std::string profile_name = "default";
        auto profile_pos = gcode.find("PROFILE=");
        if (profile_pos != std::string::npos) {
            size_t start = profile_pos + 8; // Length of "PROFILE="
            size_t end = gcode.find_first_of(" \t\n", start);
            profile_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }
        calibrate_mock_mesh(profile_name);
    } else if (gcode.find("BED_MESH_PROFILE") != std::string::npos) {
        // Parse LOAD= or SAVE= or REMOVE= parameter
        if (gcode.find("LOAD=") != std::string::npos) {
            auto load_pos = gcode.find("LOAD=");
            size_t start = load_pos + 5; // Length of "LOAD="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Check if profile exists in stored data
            auto it = stored_bed_mesh_profiles_.find(profile_name);
            if (it != stored_bed_mesh_profiles_.end()) {
                // Load stored mesh data
                active_bed_mesh_ = it->second;
                spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE LOAD: loaded profile '{}'",
                             profile_name);
                dispatch_bed_mesh_update();
            } else {
                spdlog::warn("[MoonrakerClientMock] BED_MESH_PROFILE LOAD: profile '{}' not found",
                             profile_name);
            }
        } else if (gcode.find("SAVE=") != std::string::npos) {
            auto save_pos = gcode.find("SAVE=");
            size_t start = save_pos + 5; // Length of "SAVE="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Add new profile to list if not already present
            if (std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name) ==
                bed_mesh_profiles_.end()) {
                bed_mesh_profiles_.push_back(profile_name);
            }
            // Store current mesh data under new name
            active_bed_mesh_.name = profile_name;
            stored_bed_mesh_profiles_[profile_name] = active_bed_mesh_;
            spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE SAVE: saved profile '{}'",
                         profile_name);
            dispatch_bed_mesh_update();
        } else if (gcode.find("REMOVE=") != std::string::npos) {
            auto remove_pos = gcode.find("REMOVE=");
            size_t start = remove_pos + 7; // Length of "REMOVE="
            size_t end = gcode.find_first_of(" \t\n", start);
            std::string profile_name =
                gcode.substr(start, end == std::string::npos ? end : end - start);

            // Remove profile from list and stored data
            auto it = std::find(bed_mesh_profiles_.begin(), bed_mesh_profiles_.end(), profile_name);
            if (it != bed_mesh_profiles_.end()) {
                bed_mesh_profiles_.erase(it);
                stored_bed_mesh_profiles_.erase(profile_name);
                spdlog::info("[MoonrakerClientMock] BED_MESH_PROFILE REMOVE: removed profile '{}'",
                             profile_name);
                dispatch_bed_mesh_update();
            } else {
                spdlog::warn(
                    "[MoonrakerClientMock] BED_MESH_PROFILE REMOVE: profile '{}' not found",
                    profile_name);
            }
        }
    } else if (gcode.find("BED_MESH_CLEAR") != std::string::npos) {
        // Clear the active bed mesh
        active_bed_mesh_.name = "";
        active_bed_mesh_.probed_matrix.clear();
        active_bed_mesh_.x_count = 0;
        active_bed_mesh_.y_count = 0;
        spdlog::info("[MoonrakerClientMock] BED_MESH_CLEAR: cleared active mesh");
        dispatch_bed_mesh_update();
    }
    return std::nullopt;
}

// Z offset - SET_GCODE_OFFSET Z=0.2 or SET_GCODE_OFFSET Z_ADJUST=-0.05
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_set_gcode_offset(const std::string& gcode) {
    // Parse Z parameter (absolute offset)
    auto z_pos = gcode.find(" Z=");
    if (z_pos != std::string::npos) {
        try {
            double z_offset = std::stod(gcode.substr(z_pos + 3));
            gcode_offset_z_.store(z_offset);
            spdlog::info("[MoonrakerClientMock] SET_GCODE_OFFSET Z={:.3f}", z_offset);
            dispatch_gcode_move_update();
        } catch (...) {
        }
    }

    // Parse Z_ADJUST parameter (relative adjustment)
    auto z_adj_pos = gcode.find("Z_ADJUST=");
    if (z_adj_pos != std::string::npos) {
        try {
            double adjustment = std::stod(gcode.substr(z_adj_pos + 9));
            double new_offset = gcode_offset_z_.load() + adjustment;
            gcode_offset_z_.store(new_offset);
            spdlog::info("[MoonrakerClientMock] SET_GCODE_OFFSET Z_ADJUST={:.3f} -> Z={:.3f}",
                         adjustment, new_offset);
            dispatch_gcode_move_update();
        } catch (...) {
        }
    }
    return std::nullopt;
}

// Per-tool offset - SET_TOOL_PARAMETER T=1 PARAMETER=gcode_x_offset VALUE=-0.05
// Only the three gcode_*_offset parameters are modelled; the real command
// takes any tool parameter, but nothing else is read back anywhere in the
// app. One parameter per command, as in the firmware.
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_set_tool_parameter(const std::string& gcode) {
    auto t_pos = gcode.find("T=");
    auto p_pos = gcode.find("PARAMETER=");
    auto v_pos = gcode.find("VALUE=");
    if (t_pos != std::string::npos && p_pos != std::string::npos && v_pos != std::string::npos) {
        try {
            const int tool = std::stoi(gcode.substr(t_pos + 2));
            const std::string param =
                gcode.substr(p_pos + 10, gcode.find(' ', p_pos) - (p_pos + 10));
            const double value = std::stod(gcode.substr(v_pos + 6));
            if (auto axis = tool_offset_axis(param)) {
                {
                    std::lock_guard<std::mutex> lock(tool_offsets_mutex_);
                    tool_offsets_[tool][*axis] = value;
                }
                spdlog::info("[MoonrakerClientMock] SET_TOOL_PARAMETER T={} {}={:.3f}", tool, param,
                             value);
                dispatch_tool_update(tool, *axis);
            } else {
                spdlog::debug("[MoonrakerClientMock] SET_TOOL_PARAMETER T={} {}: not modelled",
                              tool, param);
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

// Per-tool offset, durable half - SAVE_TOOL_PARAMETER T=1 PARAMETER=gcode_x_offset
//
// klipper-toolchanger's Tool.save_parameter() is configfile.set(self.name,
// name, self.params[name]): it persists whatever the tool ALREADY holds and
// takes no VALUE=. So this stages the current runtime value and changes
// nothing live - the change only lands when SAVE_CONFIG writes it out.
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_save_tool_parameter(const std::string& gcode) {
    auto t_pos = gcode.find("T=");
    auto p_pos = gcode.find("PARAMETER=");
    if (t_pos != std::string::npos && p_pos != std::string::npos) {
        try {
            const int tool = std::stoi(gcode.substr(t_pos + 2));
            const std::string param =
                gcode.substr(p_pos + 10, gcode.find(' ', p_pos) - (p_pos + 10));
            if (auto axis = tool_offset_axis(param)) {
                char value[32];
                std::snprintf(value, sizeof(value), "%.6g", tool_offset(tool, *axis));
                // Section is Klipper's config section verbatim, which for
                // [tool T1] is "tool T1" - the same key the status object
                // uses.
                stage_config_change("tool T" + std::to_string(tool), param, value);
                spdlog::info("[MoonrakerClientMock] SAVE_TOOL_PARAMETER T={} {}={} "
                             "- staged, awaiting SAVE_CONFIG",
                             tool, param, value);
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

// Input shaper calibration - SHAPER_CALIBRATE AXIS=X or AXIS=Y
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_shaper_calibrate(const std::string& gcode) {
    char axis = 'X'; // Default to X
    if (gcode.find("AXIS=Y") != std::string::npos || gcode.find("AXIS=y") != std::string::npos) {
        axis = 'Y';
    } else if (gcode.find("AXIS=X") != std::string::npos ||
               gcode.find("AXIS=x") != std::string::npos) {
        axis = 'X';
    }
    spdlog::info("[MoonrakerClientMock] SHAPER_CALIBRATE AXIS={}", axis);
    dispatch_shaper_calibrate_response(axis);
    return std::nullopt;
}

// Belt tension measurement - TEST_RESONANCES AXIS=<1,1|1,-1> OUTPUT=resonances
// NAME=<name>. Its own block: no other substring check in this chain
// matches TEST_RESONANCES, and vice versa.
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_test_resonances(const std::string& gcode) {
    dispatch_test_resonances_response(gcode);
    return std::nullopt;
}

// PROBE_CALIBRATE or Z_ENDSTOP_CALIBRATE - Start Z-offset calibration
// - PROBE_CALIBRATE: For printers with probe (BLTouch, inductive, etc.)
// - Z_ENDSTOP_CALIBRATE: For printers with only mechanical Z endstop
// Both enter manual probe mode, home if needed, and work identically
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_probe_calibrate(const std::string& gcode) {
    const bool is_probe_calibrate = gcode.find("PROBE_CALIBRATE") != std::string::npos;
    const char* cmd_name = is_probe_calibrate ? "PROBE_CALIBRATE" : "Z_ENDSTOP_CALIBRATE";

    if (manual_probe_active_.load()) {
        spdlog::warn("[MoonrakerClientMock] {}: Already in manual probe mode", cmd_name);
        return std::nullopt;
    }

    // Ensure we're homed first
    {
        std::lock_guard<std::mutex> lock(homed_axes_mutex_);
        if (homed_axes_.find("xyz") == std::string::npos) {
            // Auto-home like real Klipper would. Group the axis zeroing
            // so a concurrent snapshot read never sees a torn position.
            std::lock_guard<std::mutex> pos_lock(pos_mutex_);
            homed_axes_ = "xyz";
            pos_x_.store(0.0);
            pos_y_.store(0.0);
            pos_z_.store(0.0);
            spdlog::info("[MoonrakerClientMock] {}: Auto-homed all axes", cmd_name);
        }
    }

    // Enter manual probe mode at a starting Z height
    manual_probe_active_.store(true);
    manual_probe_z_.store(5.0); // Start 5mm above bed
    pos_z_.store(5.0);          // Sync toolhead Z

    spdlog::info("[MoonrakerClientMock] {}: Entered manual probe mode, Z={:.3f}", cmd_name,
                 manual_probe_z_.load());

    // Dispatch manual probe state change
    dispatch_manual_probe_update();
    return std::nullopt;
}

// TESTZ Z=<value> - Adjust Z position during manual probe calibration
// Z can be absolute (Z=0.1) or relative (Z=+0.1 or Z=-0.05)
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_testz(const std::string& gcode) {
    if (!manual_probe_active_.load()) {
        spdlog::warn("[MoonrakerClientMock] TESTZ: Not in manual probe mode (ignored)");
        return 0;
    }
    size_t z_pos = gcode.find("Z=");
    if (z_pos != std::string::npos) {
        std::string z_str = gcode.substr(z_pos + 2);
        try {
            // Check for relative move (+/- prefix)
            bool is_relative = (z_str[0] == '+' || z_str[0] == '-');
            double z_value = std::stod(z_str);

            double new_z;
            if (is_relative) {
                new_z = manual_probe_z_.load() + z_value;
            } else {
                new_z = z_value;
            }

            // Clamp to reasonable range (0 to 10mm above bed)
            new_z = std::clamp(new_z, -0.5, 10.0);

            manual_probe_z_.store(new_z);
            pos_z_.store(new_z); // Sync toolhead Z

            spdlog::info("[MoonrakerClientMock] TESTZ: Z={:.3f} ({}) -> new Z={:.3f}", z_value,
                         is_relative ? "relative" : "absolute", new_z);

            // Dispatch Z position update
            dispatch_manual_probe_update();
        } catch (const std::exception& e) {
            spdlog::warn("[MoonrakerClientMock] TESTZ: Failed to parse Z value: {}", e.what());
        }
    }
    return std::nullopt;
}

// ACCEPT - Accept current Z position as the calibrated offset
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_accept(const std::string&) {
    if (manual_probe_active_.load()) {
        double final_z = manual_probe_z_.load();
        manual_probe_active_.store(false);

        spdlog::info("[MoonrakerClientMock] ACCEPT: Z-offset calibration complete, offset={:.3f}mm",
                     final_z);

        // In real Klipper, this would update probe z_offset in config
        // User typically follows with SAVE_CONFIG to persist

        // Dispatch manual probe state change (is_active=false)
        dispatch_manual_probe_update();
    } else {
        spdlog::warn("[MoonrakerClientMock] ACCEPT: Not in manual probe mode");
    }
    return std::nullopt;
}

// ABORT - Cancel manual probe calibration
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_abort(const std::string&) {
    if (manual_probe_active_.load()) {
        manual_probe_active_.store(false);
        spdlog::info("[MoonrakerClientMock] ABORT: Manual probe cancelled");

        // Dispatch manual probe state change (is_active=false)
        dispatch_manual_probe_update();
    }
    return std::nullopt;
}
