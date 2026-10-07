// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../tests/mocks/mock_printer_state.h"
#include "moonraker_client_mock.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace helix;

namespace {

bool has_token(const std::string& gcode, const char* token) {
    return gcode.find(token) != std::string::npos;
}

// The bare command, optionally followed by arguments after a space.
bool is_command(const std::string& gcode, const std::string& command) {
    return gcode == command || gcode.find(command + " ") == 0;
}

// Real Klipper uppercases only the leading command word before dispatch
// (M117, m117, and M117 all route to the same handler) - it never touches
// anything after that word. Mirror that here: every branch below matches on
// the command name via gcode.find(...)/gcode == ..., so normalizing just the
// token up to the first whitespace lets lowercase/mixed-case commands (as
// typed in the console, or emitted by some slicers) match the same way they
// would on real hardware, without mangling M117 message text, filenames
// (SDCARD_PRINT_FILE FILENAME=...), or any other argument payload.
std::string normalize_gcode_command_case(const std::string& raw) {
    size_t token_end = raw.find_first_of(" \t");
    std::string result = raw;
    size_t end = (token_end == std::string::npos) ? raw.size() : token_end;
    for (size_t i = 0; i < end; ++i) {
        result[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[i])));
    }
    return result;
}

} // namespace

// Standalone IFS module commands (HELIX_MOCK_AMS=ifs-module): matched on the
// command token exactly — a bare T<n> must not be confused with a temperature
// parameter elsewhere in a line.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_ifs_module(const std::string& gcode) {
    const size_t ifs_token_end = gcode.find_first_of(" \t");
    if (apply_ifs_module_gcode(gcode.substr(0, ifs_token_end), gcode)) {
        return 0;
    }
    return std::nullopt;
}

// MedusaHC commands. They match the COMMAND TOKEN exactly, not find() anywhere
// in the line, so the bare OPEN/CLOSE feeder macros cannot be confused with a
// substring of anything else. Only armed in the MedusaHC mock modes.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_medusa(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    auto tool_param = [&]() -> int {
        const size_t t = gcode.find("T=");
        if (t == std::string::npos) {
            return -1;
        }
        try {
            return std::stoi(gcode.substr(t + 2));
        } catch (...) {
            return -1;
        }
    };

    if (cmd == "OPEN" || cmd == "MHC_OPEN") {
        medusa_feeder_open_.store(true);
        spdlog::info("[MoonrakerClientMock] MedusaHC feeder opened ({})", cmd);
        return 0;
    }
    if (cmd == "CLOSE" || cmd == "MHC_CLOSE") {
        medusa_feeder_open_.store(false);
        spdlog::info("[MoonrakerClientMock] MedusaHC feeder closed ({})", cmd);
        return 0;
    }
    if (cmd == "SELECT_TOOL") {
        start_medusa_swap(tool_param());
        return 0;
    }
    if (cmd == "UNSELECT_TOOL" || cmd == "DROP_TOOL") {
        start_medusa_swap(-1);
        return 0;
    }
    // Bare T<n>: what the fork registers when it runs the swap itself.
    if (cmd.size() >= 2 && cmd[0] == 'T' &&
        std::all_of(cmd.begin() + 1, cmd.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        start_medusa_swap(std::stoi(cmd.substr(1)));
        return 0;
    }
    return std::nullopt;
}

// CFS calibration commands (HELIX_MOCK_AMS=cfs), matched on the command token
// exactly. BOX_FIND_CUT_POS and BOX_CUSTOM_COMMAND are CFS-only vocabulary, and
// the chute jog script (BOX_CUSTOM_COMMAND aside) falls through to the generic
// G91/G0 simulation.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_cfs(const std::string& gcode) {
    const size_t token_end = gcode.find_first_of(" \t");
    const std::string cmd = gcode.substr(0, token_end);
    if (cmd == "BOX_FIND_CUT_POS") {
        spdlog::info("[MoonrakerClientMock] CFS cutter calibration sweep (mock)");
        simulate_cfs_find_cut_pos();
        return 0;
    }
    if (cmd == "BOX_CUSTOM_COMMAND" && apply_cfs_box_custom_command(gcode)) {
        return 0;
    }
    return std::nullopt;
}

// Snapmaker U1 feeder commands. Unconditional: AUTO_FEEDING is U1-only
// vocabulary, so no mock printer mode needs to arm it, and no other printer's
// script can contain the token.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_u1_feeding(const std::string& gcode) {
    if (apply_u1_feeding_gcode(gcode)) {
        return 0;
    }
    return std::nullopt;
}

// Heater and pin targets: M104/M109 S<temp>, M140/M190 S<temp>,
// SET_HEATER_TEMPERATURE HEATER=<name> TARGET=<temp>, SET_TEMPERATURE_FAN_TARGET,
// SET_PIN and the Panda Breath drying pair. The first rule of the chain that
// matches a line handles it.
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_heater_temperature(const std::string& gcode) {
    double target = 0.0;
    size_t target_pos = gcode.find("TARGET=");
    if (target_pos != std::string::npos) {
        target = std::stod(gcode.substr(target_pos + 7));
    }

    if (gcode.find("HEATER=extruder") != std::string::npos) {
        set_extruder_target(target);
        reset_idle_timeout();
        spdlog::info("[MoonrakerClientMock] Extruder target set to {}°C", target);
        dispatch_status_update({{"extruder", {{"target", target}}}});
    } else if (gcode.find("HEATER=heater_bed") != std::string::npos) {
        set_bed_target(target);
        reset_idle_timeout();
        spdlog::info("[MoonrakerClientMock] Bed target set to {}°C", target);
        dispatch_status_update({{"heater_bed", {{"target", target}}}});
    } else if (gcode.find("HEATER=heater_generic ") != std::string::npos) {
        // Reject invalid format: Klipper expects bare object name, not "heater_generic chamber"
        spdlog::error(
            "[MoonrakerClientMock] Invalid SET_HEATER_TEMPERATURE: HEATER must use bare object "
            "name (e.g. HEATER=chamber), not prefixed type (HEATER=heater_generic chamber)");
        return 1;
    } else {
        // Chamber heater, matched by the BARE object name the resolved
        // chamber heater uses — "HEATER=chamber" for keyword heaters,
        // "HEATER=dragonbreath" for backend-named ones (a hard-coded
        // "chamber" comparison silently ignores those).
        const std::string bare = chamber_heater_bare_name();
        if (!bare.empty() && gcode.find("HEATER=" + bare) != std::string::npos) {
            set_chamber_target(target);
            reset_idle_timeout();
            spdlog::info("[MoonrakerClientMock] Chamber target set to {}°C", target);
            auto key = chamber_heater_status_key();
            if (!key.empty())
                dispatch_status_update({{key, {{"target", target}}}});
        } else if (size_t pos = gcode.find("HEATER="); pos != std::string::npos) {
            const size_t start = pos + 7;
            const std::string heater = gcode.substr(start, gcode.find(' ', start) - start);
            if (set_aux_heater_target(heater, target)) {
                spdlog::info("[MoonrakerClientMock] Heater {} target set to {}°C", heater, target);
                dispatch_status_update({{"heater_generic " + heater, {{"target", target}}}});
            }
        }
    }
    return std::nullopt;
}

// Check for SET_TEMPERATURE_FAN_TARGET (temperature_fan chamber heaters)
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_temperature_fan_target(const std::string& gcode) {
    double target = 0.0;
    size_t target_pos = gcode.find("TARGET=");
    if (target_pos != std::string::npos) {
        target = std::stod(gcode.substr(target_pos + 7));
    }
    set_chamber_target(target);
    reset_idle_timeout();
    spdlog::info("[MoonrakerClientMock] Chamber (temperature_fan) target set to {}°C", target);
    auto key = chamber_heater_status_key();
    if (!key.empty())
        dispatch_status_update({{key, {{"target", target}}}});
    return std::nullopt;
}

// VENDOR_OK: the stock Panda Breath binding's drying commands, simulated
// for the panda_breath mock shape (chamber_heater_backend_panda_breath.cpp
// builds them).
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_panda_dry_start(const std::string& gcode) {
    auto int_param = [&gcode](const char* key, int fallback) {
        const size_t pos = gcode.find(key);
        if (pos == std::string::npos) {
            return fallback;
        }
        try {
            return std::stoi(gcode.substr(pos + std::strlen(key)));
        } catch (const std::exception&) {
            return fallback;
        }
    };
    chamber_dry_temp_.store(int_param("TEMP=", 55));
    chamber_dry_hours_.store(std::clamp(int_param("HOURS=", 6), 1, 12));
    chamber_dry_start_.store(mock_sim_time_.load());
    chamber_drying_.store(true);
    spdlog::info("[MoonrakerClientMock] Chamber drying started: {}C for {}h",
                 chamber_dry_temp_.load(), chamber_dry_hours_.load());
    return std::nullopt;
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_panda_dry_stop(const std::string&) {
    chamber_drying_.store(false);
    spdlog::info("[MoonrakerClientMock] Chamber drying stopped");
    return std::nullopt;
}

// Check for SET_PIN (chamber filter fan: SET_PIN PIN=dragonbreath_filter VALUE=1)
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_set_pin(const std::string& gcode) {
    const std::string pin_obj = chamber_filter_pin_object();
    if (!pin_obj.empty()) {
        const std::string bare_pin = pin_obj.substr(11); // strip "output_pin "
        if (gcode.find("PIN=" + bare_pin) != std::string::npos) {
            double value = 0.0;
            size_t value_pos = gcode.find("VALUE=");
            if (value_pos != std::string::npos) {
                value = std::stod(gcode.substr(value_pos + 6));
            }
            chamber_filter_value_.store(value);
            reset_idle_timeout();
            spdlog::info("[MoonrakerClientMock] Chamber filter pin {} set to {}", bare_pin, value);
            dispatch_status_update({{pin_obj, {{"value", value}}}});
        }
    }
    return std::nullopt;
}

// Check for M-code style temperature commands
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_extruder_target_mcode(const std::string& gcode) {
    size_t s_pos = gcode.find('S');
    if (s_pos != std::string::npos) {
        double target = std::stod(gcode.substr(s_pos + 1));
        set_extruder_target(target);
        reset_idle_timeout();
        spdlog::info("[MoonrakerClientMock] Extruder target set to {}°C (M-code)", target);
        dispatch_status_update({{"extruder", {{"target", target}}}});
    }
    return std::nullopt;
}

MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_bed_target_mcode(const std::string& gcode) {
    size_t s_pos = gcode.find('S');
    if (s_pos != std::string::npos) {
        double target = std::stod(gcode.substr(s_pos + 1));
        set_bed_target(target);
        reset_idle_timeout();
        spdlog::info("[MoonrakerClientMock] Bed target set to {}°C (M-code)", target);
        dispatch_status_update({{"heater_bed", {{"target", target}}}});
    }
    return std::nullopt;
}

// M117 <message> - Set display message (LCD message on real printers).
// Bare M117 (or M117 followed only by whitespace) clears the message.
// Klipper strips exactly one leading space after the command, so
// "M117 hello" -> "hello" but "M117  hello" (two spaces) -> " hello".
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_display_message(const std::string& gcode) {
    std::string text = gcode.substr(4);
    if (!text.empty() && text.front() == ' ') {
        text.erase(0, 1);
    }
    if (text.find_first_not_of(" \t") == std::string::npos) {
        text.clear();
    }
    {
        std::lock_guard<std::mutex> lock(display_message_mutex_);
        display_message_ = text;
        display_message_set_ = true;
    }
    spdlog::info("[MoonrakerClientMock] Display message set to \"{}\" (M117)", text);
    dispatch_status_update({{"display_status", {{"message", text}}}});
    return 0;
    return std::nullopt;
}

// SAVE_GCODE_STATE captures the positioning mode for the matching
// RESTORE_GCODE_STATE. Handled before the G90/G91 parse so a script
// containing both SAVE and G91 (the CFS chute jog form) saves the mode
// that G91 is about to change. One level deep: no shipped script nests
// SAVE/RESTORE pairs.
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_save_gcode_state(const std::string& gcode) {
    saved_gcode_relative_.store(relative_mode_.load());
    return std::nullopt;
}

// Parse motion mode commands (G90/G91)
// G90 - Absolute positioning mode
// G91 - Relative positioning mode
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_absolute_mode(const std::string&) {
    relative_mode_.store(false);
    spdlog::info("[MoonrakerClientMock] Set absolute positioning mode (G90)");
    return std::nullopt;
}

MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_relative_mode(const std::string&) {
    relative_mode_.store(true);
    spdlog::info("[MoonrakerClientMock] Set relative positioning mode (G91)");
    return std::nullopt;
}

// M84 - Disable stepper motors (clears homed_axes + updates stepper_enable)
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_disable_motors(const std::string&) {
    motors_enabled_.store(false);
    {
        std::lock_guard<std::mutex> lock(homed_axes_mutex_);
        homed_axes_.clear();
    }
    spdlog::info("[MoonrakerClientMock] Motors disabled (M84/M18), homed_axes cleared");
    // Dispatch toolhead with cleared homed_axes (primary motor state indicator)
    // and stepper_enable state change (fallback for printers that report it)
    json status = {{"toolhead", {{"homed_axes", ""}}},
                   {"stepper_enable",
                    {{"steppers",
                      {{"stepper_x", false},
                       {"stepper_y", false},
                       {"stepper_z", false},
                       {"extruder", false}}}}}};
    dispatch_status_update(status);
    return std::nullopt;
}

// Parse homing command (G28)
// G28 - Home all axes
// G28 X - Home X axis only
// G28 Y - Home Y axis only
// G28 Z - Home Z axis only
// G28 X Y - Home X and Y axes
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_home(const std::string& gcode) {
    // Re-enable motors when homing
    motors_enabled_.store(true);
    // Check if specific axes are mentioned after G28
    // Need to look after the G28 to avoid false matches
    size_t g28_pos = gcode.find("G28");
    std::string after_g28 = gcode.substr(g28_pos + 3);

    // Check for specific axis letters (case insensitive search)
    bool has_x =
        after_g28.find('X') != std::string::npos || after_g28.find('x') != std::string::npos;
    bool has_y =
        after_g28.find('Y') != std::string::npos || after_g28.find('y') != std::string::npos;
    bool has_z =
        after_g28.find('Z') != std::string::npos || after_g28.find('z') != std::string::npos;

    // If no specific axis mentioned, home all
    bool home_all = !has_x && !has_y && !has_z;

    std::string homed;
    {
        std::lock_guard<std::mutex> lock(homed_axes_mutex_);
        // Group all axis zeroing so a concurrent snapshot read never sees a
        // partially-homed position. Lock order is homed_axes_mutex_ ->
        // pos_mutex_ (pos_mutex_ is a leaf).
        std::lock_guard<std::mutex> pos_lock(pos_mutex_);

        if (home_all) {
            homed_axes_ = "xyz";
            pos_x_.store(0.0);
            pos_y_.store(0.0);
            pos_z_.store(0.0);
            spdlog::info("[MoonrakerClientMock] Homed all axes (G28), homed_axes='xyz'");
        } else {
            if (has_x) {
                if (homed_axes_.find('x') == std::string::npos) {
                    homed_axes_ += 'x';
                }
                pos_x_.store(0.0);
            }
            if (has_y) {
                if (homed_axes_.find('y') == std::string::npos) {
                    homed_axes_ += 'y';
                }
                pos_y_.store(0.0);
            }
            if (has_z) {
                if (homed_axes_.find('z') == std::string::npos) {
                    homed_axes_ += 'z';
                }
                pos_z_.store(0.0);
            }
            spdlog::info("[MoonrakerClientMock] Homed axes: X={} Y={} Z={}, homed_axes='{}'", has_x,
                         has_y, has_z, homed_axes_);
        }
        homed = homed_axes_;
    }
    reset_idle_timeout();
    dispatch_status_update({{"toolhead", {{"homed_axes", homed}}}});
    return std::nullopt;
}

// Parse movement commands (G0/G1)
// G0 X100 Y50 Z10 - Rapid move
// G1 X100 Y50 Z10 E5 F3000 - Linear move (E and F ignored for now)
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_move(const std::string& gcode) {
    // Re-enable motors when moving
    motors_enabled_.store(true);
    bool is_relative = relative_mode_.load();

    // Position limits (typical Voron 2.4 350mm config)
    // Z allows slight negative for probe calibration
    constexpr double X_MIN = 0.0, X_MAX = 350.0;
    constexpr double Y_MIN = 0.0, Y_MAX = 350.0;
    constexpr double Z_MIN = -0.5, Z_MAX = 340.0;

    // Helper lambda to parse axis value from gcode string
    auto parse_axis = [&gcode](char axis) -> std::pair<bool, double> {
        // Look for the axis letter followed by a number
        size_t pos = gcode.find(axis);
        if (pos == std::string::npos) {
            // Try lowercase
            pos = gcode.find(static_cast<char>(axis + 32));
        }
        if (pos != std::string::npos && pos + 1 < gcode.length()) {
            // Skip any spaces after the axis letter
            size_t value_start = pos + 1;
            while (value_start < gcode.length() && gcode[value_start] == ' ') {
                value_start++;
            }
            if (value_start < gcode.length()) {
                try {
                    double value = std::stod(gcode.substr(value_start));
                    return {true, value};
                } catch (...) {
                    // Parse error, ignore this axis
                }
            }
        }
        return {false, 0.0};
    };

    auto [has_x, x_val] = parse_axis('X');
    auto [has_y, y_val] = parse_axis('Y');
    auto [has_z, z_val] = parse_axis('Z');

    // Calculate target positions
    double target_x = has_x ? (is_relative ? pos_x_.load() + x_val : x_val) : pos_x_.load();
    double target_y = has_y ? (is_relative ? pos_y_.load() + y_val : y_val) : pos_y_.load();
    double target_z = has_z ? (is_relative ? pos_z_.load() + z_val : z_val) : pos_z_.load();

    // Check limits (like real Klipper)
    bool out_of_range = false;
    std::string error_detail;
    if (target_x < X_MIN || target_x > X_MAX) {
        error_detail = "Move out of range: X=" + std::to_string(target_x);
        out_of_range = true;
    } else if (target_y < Y_MIN || target_y > Y_MAX) {
        error_detail = "Move out of range: Y=" + std::to_string(target_y);
        out_of_range = true;
    } else if (target_z < Z_MIN || target_z > Z_MAX) {
        error_detail = "Move out of range: Z=" + std::to_string(target_z);
        out_of_range = true;
    }

    if (out_of_range) {
        // The two channels carry the SAME rejection in different shapes, and
        // real Moonraker is the spec for both:
        //   - broadcast gcode-response stream: `!!`-prefixed (error_classify
        //     strips the prefix before the router sees it);
        //   - JSON-RPC error `message`: no prefix at all.
        // The latch therefore holds the unprefixed text: a "!!" message there
        // could never match the broadcast channel and would defeat the
        // cross-source toast dedup.
        const std::string broadcast_line = "!! " + error_detail;
        dispatch_gcode_response(broadcast_line);
        spdlog::warn("[MoonrakerClientMock] Move rejected - {}", broadcast_line);
        // Store error for RPC handler to return proper error response (like real Moonraker)
        {
            std::lock_guard<std::mutex> lock(gcode_error_mutex_);
            last_gcode_error_ = error_detail;
        }
    } else {
        // Apply the move as a group so the background simulation loop never
        // broadcasts a torn snapshot (e.g. X/Y updated but Z still stale).
        {
            std::lock_guard<std::mutex> pos_lock(pos_mutex_);
            if (has_x)
                pos_x_.store(target_x);
            if (has_y)
                pos_y_.store(target_y);
            if (has_z)
                pos_z_.store(target_z);
        }

        if (has_x || has_y || has_z) {
            double sx, sy, sz;
            read_position_snapshot(sx, sy, sz);
            spdlog::debug("[MoonrakerClientMock] Move {} X={} Y={} Z={} (mode={})",
                          gcode.find("G0") != std::string::npos ? "G0" : "G1", sx, sy, sz,
                          is_relative ? "relative" : "absolute");
            // Reset idle timeout when moving
            reset_idle_timeout();
            // Dispatch immediate position update (matches real Moonraker):
            // live_position is in gcode space (includes the z offset),
            // toolhead.position is not.
            dispatch_status_update(
                {{"toolhead", {{"position", {sx, sy, sz, 0.0}}}},
                 {"motion_report",
                  {{"live_position", {sx, sy, sz + gcode_offset_z_.load(), 0.0}}}}});
        }
    }
    return std::nullopt;
}

// The matching half of SAVE_GCODE_STATE above: after the move block so a
// jog script (SAVE, G91, G0, M400, RESTORE) restores the mode it started
// in, the way Klipper's template wrapper does.
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_restore_gcode_state(const std::string& gcode) {
    relative_mode_.store(saved_gcode_relative_.load());
    return std::nullopt;
}

// Parse Tn tool change commands (T0, T1, T2, ...)
// Klipper maps Tn to ACTIVATE_EXTRUDER for multi-extruder setups;
// toolchanger plugins override Tn with physical tool change logic.
// The mock simulates the result: dispatch toolhead.extruder status update.
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_tool_change(const std::string& gcode) {
    try {
        int tool_index = std::stoi(gcode.substr(1));
        // Build extruder name: T0 -> "extruder", T1 -> "extruder1", T2 -> "extruder2", ...
        std::string extruder_name =
            (tool_index == 0) ? "extruder" : ("extruder" + std::to_string(tool_index));
        spdlog::info("[MoonrakerClientMock] Tool change {} -> active extruder: {}", gcode,
                     extruder_name);

        // Dispatch toolhead.extruder update (same as real Klipper)
        json status = {{"toolhead", {{"extruder", extruder_name}}}};
        dispatch_status_update(status);
    } catch (...) {
        spdlog::warn("[MoonrakerClientMock] Failed to parse tool index from: {}", gcode);
    }
    return std::nullopt;
}

// Parse print job commands (delegate to unified internal handlers)
// SDCARD_PRINT_FILE FILENAME=xxx - Start printing a file
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_sdcard_print_file(const std::string& gcode) {
    size_t filename_pos = gcode.find("FILENAME=");
    if (filename_pos != std::string::npos) {
        size_t start = filename_pos + 9;
        std::string filename;
        if (start < gcode.size() && gcode[start] == '"') {
            // Quoted value, as Klipper's shlex tokenizer expects whenever
            // the name contains spaces (Moonraker and the PLR resume path
            // both always quote). Runs to the closing quote; the quotes
            // themselves are not part of the name.
            size_t end = gcode.find('"', start + 1);
            filename = (end != std::string::npos) ? gcode.substr(start + 1, end - start - 1)
                                                  : gcode.substr(start + 1);
        } else {
            // Bare value: ends at the next whitespace.
            size_t end = gcode.find(' ', start);
            filename =
                (end != std::string::npos) ? gcode.substr(start, end - start) : gcode.substr(start);
        }

        // Use unified internal handler
        start_print_internal(filename);
    }
    return std::nullopt;
}

// PAUSE - Pause current print
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_pause(const std::string&) {
    pause_print_internal();
    return std::nullopt;
}

// RESUME - Resume paused print
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_resume(const std::string&) {
    resume_print_internal();
    return std::nullopt;
}

// CANCEL_PRINT - Cancel current print
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_cancel_print(const std::string&) {
    cancel_print_internal();
    return std::nullopt;
}

// M112 - Emergency stop
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_emergency_stop(const std::string&) {
    emergency_stop_internal();
    return std::nullopt;
}

// Fan control - M106/M107/SET_FAN_SPEED
// M106 P0 S128 - Set fan index 0 to 50% (S is 0-255, P is fan index)
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_fan_m106(const std::string& gcode) {
    int fan_index = 0;
    int speed_value = 0;

    // Parse P parameter (fan index)
    auto p_pos = gcode.find('P');
    if (p_pos != std::string::npos && p_pos + 1 < gcode.length()) {
        try {
            fan_index = std::stoi(gcode.substr(p_pos + 1));
        } catch (...) {
        }
    }

    // Parse S parameter (speed 0-255)
    auto s_pos = gcode.find('S');
    if (s_pos != std::string::npos && s_pos + 1 < gcode.length()) {
        try {
            speed_value = std::stoi(gcode.substr(s_pos + 1));
            speed_value = std::clamp(speed_value, 0, 255);
        } catch (...) {
        }
    }

    // Convert to normalized speed (0.0-1.0)
    double normalized_speed = speed_value / 255.0;

    // Fan index 0 = "fan", index 1+ = "fan1", "fan2", etc.
    std::string fan_name = (fan_index == 0) ? "fan" : ("fan" + std::to_string(fan_index));
    set_fan_speed_internal(fan_name, normalized_speed);

    spdlog::trace("[MoonrakerClientMock] M106 P{} S{} -> {} speed={:.2f}", fan_index, speed_value,
                  fan_name, normalized_speed);
    return std::nullopt;
}

// M107 - Turn off fan
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_fan_m107(const std::string& gcode) {
    int fan_index = 0;

    auto p_pos = gcode.find('P');
    if (p_pos != std::string::npos && p_pos + 1 < gcode.length()) {
        try {
            fan_index = std::stoi(gcode.substr(p_pos + 1));
        } catch (...) {
        }
    }

    std::string fan_name = (fan_index == 0) ? "fan" : ("fan" + std::to_string(fan_index));
    set_fan_speed_internal(fan_name, 0.0);

    spdlog::info("[MoonrakerClientMock] M107 P{} -> {} off", fan_index, fan_name);
    return std::nullopt;
}

// SET_FAN_SPEED - Klipper extended fan control
// SET_FAN_SPEED FAN=nevermore SPEED=0.5
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_set_fan_speed(const std::string& gcode) {
    std::string fan_name;
    double speed = 0.0;

    // Parse FAN parameter
    auto fan_pos = gcode.find("FAN=");
    if (fan_pos != std::string::npos) {
        size_t start = fan_pos + 4;
        size_t end = gcode.find_first_of(" \t\n", start);
        fan_name = gcode.substr(start, end == std::string::npos ? end : end - start);
    }

    // Parse SPEED parameter (0.0-1.0)
    auto speed_pos = gcode.find("SPEED=");
    if (speed_pos != std::string::npos) {
        try {
            speed = std::stod(gcode.substr(speed_pos + 6));
            speed = std::clamp(speed, 0.0, 1.0);
        } catch (...) {
        }
    }

    if (!fan_name.empty()) {
        // Try to find matching fan in discovered fans list
        std::string full_fan_name = find_fan_by_suffix(fan_name);
        if (!full_fan_name.empty()) {
            set_fan_speed_internal(full_fan_name, speed);
            spdlog::info("[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f}", full_fan_name,
                         speed);
        } else {
            // Use short name if no match found
            set_fan_speed_internal(fan_name, speed);
            spdlog::info("[MoonrakerClientMock] SET_FAN_SPEED FAN={} SPEED={:.2f} (unmatched fan)",
                         fan_name, speed);
        }
    }
    return std::nullopt;
}

// LED control - SET_LED LED=<name> RED=<0-1> GREEN=<0-1> BLUE=<0-1> [WHITE=<0-1>]
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_set_led(const std::string& gcode) {
    // Parse LED name
    std::string led_name;
    auto led_pos = gcode.find("LED=");
    if (led_pos != std::string::npos) {
        size_t start = led_pos + 4;
        // Handle quoted LED names: LED="name" or LED=name
        if (start < gcode.size() && gcode[start] == '"') {
            start++;
            size_t end = gcode.find('"', start);
            if (end != std::string::npos) {
                led_name = gcode.substr(start, end - start);
            }
        } else {
            size_t end = gcode.find_first_of(" \t\n", start);
            led_name = gcode.substr(start, end == std::string::npos ? end : end - start);
        }
    }

    // Parse color values (default to 0)
    auto parse_color = [&gcode](const std::string& param) -> double {
        auto pos = gcode.find(param + "=");
        if (pos != std::string::npos) {
            size_t start = pos + param.length() + 1;
            try {
                return std::clamp(std::stod(gcode.substr(start)), 0.0, 1.0);
            } catch (...) {
                return 0.0;
            }
        }
        return 0.0;
    };

    double red = parse_color("RED");
    double green = parse_color("GREEN");
    double blue = parse_color("BLUE");
    double white = parse_color("WHITE");

    // Find matching LED in our list (need to match by suffix since command uses short name)
    std::string full_led_name;
    for (const auto& led : discovery_.leds()) {
        // Match if LED name ends with the command's led_name
        // e.g., "neopixel chamber_light" matches "chamber_light"
        if (led.length() >= led_name.length()) {
            size_t suffix_start = led.length() - led_name.length();
            if (led.substr(suffix_start) == led_name) {
                full_led_name = led;
                break;
            }
        }
    }

    if (!full_led_name.empty()) {
        // Update LED state
        {
            std::lock_guard<std::mutex> lock(led_mutex_);
            led_states_[full_led_name] = LedColor{red, green, blue, white};
        }

        spdlog::info("[MoonrakerClientMock] SET_LED: {} R={:.2f} G={:.2f} B={:.2f} W={:.2f}",
                     full_led_name, red, green, blue, white);

        // Dispatch LED state update notification (like real Moonraker would)
        json led_status;
        {
            std::lock_guard<std::mutex> lock(led_mutex_);
            for (const auto& [name, color] : led_states_) {
                led_status[name] = {
                    {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
            }
        }
        dispatch_status_update(led_status);
    } else {
        spdlog::warn("[MoonrakerClientMock] SET_LED: unknown LED '{}'", led_name);
    }
    return std::nullopt;
}

MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_exclude_object(const std::string& gcode) {
    // Parse NAME parameter
    size_t name_pos = gcode.find("NAME=");
    if (name_pos != std::string::npos) {
        size_t start = name_pos + 5;
        std::string object_name;

        // Handle quoted names (NAME="Part With Spaces")
        if (start < gcode.length() && gcode[start] == '"') {
            size_t end_quote = gcode.find('"', start + 1);
            if (end_quote != std::string::npos) {
                object_name = gcode.substr(start + 1, end_quote - start - 1);
            }
        } else {
            // Unquoted name (ends at space or end of string)
            size_t end = gcode.find_first_of(" \t\n", start);
            object_name =
                (end != std::string::npos) ? gcode.substr(start, end - start) : gcode.substr(start);
        }

        if (!object_name.empty()) {
            // Update shared state if available
            if (mock_state_) {
                mock_state_->add_excluded_object(object_name);
            }
            // Also update local state for backward compatibility
            {
                std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                excluded_objects_.insert(object_name);
            }
            spdlog::info("[MoonrakerClientMock] EXCLUDE_OBJECT: '{}' added to exclusion list",
                         object_name);

            // Dispatch status update (like real Klipper would via WebSocket)
            // Use local excluded_objects_ (always up-to-date) rather than mock_state_
            // which is only available in test fixtures
            {
                json excluded_array = json::array();
                {
                    std::lock_guard<std::mutex> lock(excluded_objects_mutex_);
                    for (const auto& obj : excluded_objects_) {
                        excluded_array.push_back(obj);
                    }
                }
                json eo_status = {
                    {"exclude_object",
                     {{"excluded_objects", excluded_array}, {"current_object", nullptr}}}};
                dispatch_status_update(eo_status);
            }
        }
    } else {
        spdlog::warn("[MoonrakerClientMock] EXCLUDE_OBJECT without NAME parameter ignored");
    }
    return std::nullopt;
}

// EXCLUDE_OBJECT_DEFINE - Register objects for the print
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_exclude_object_define(const std::string& gcode) {
    size_t name_pos = gcode.find("NAME=");
    if (name_pos != std::string::npos) {
        size_t start = name_pos + 5;
        std::string object_name;
        if (start < gcode.size() && gcode[start] == '"') {
            size_t end = gcode.find('"', start + 1);
            if (end != std::string::npos) {
                object_name = gcode.substr(start + 1, end - start - 1);
            }
        } else {
            size_t end = gcode.find_first_of(" \t\r\n", start);
            object_name = gcode.substr(start, end - start);
        }
        if (!object_name.empty() && mock_state_) {
            mock_state_->add_object_name(object_name);
            spdlog::debug("[MoonrakerClientMock] EXCLUDE_OBJECT_DEFINE: registered '{}'",
                          object_name);
        }
    }
    return std::nullopt;
}

// SET_LED_EFFECT EFFECT=<name> [STOP=1] - Enable one LED effect, or stop one
MoonrakerClientMock::GcodeResult
MoonrakerClientMock::gcode_set_led_effect(const std::string& gcode) {
    size_t effect_pos = gcode.find("EFFECT=");
    if (effect_pos != std::string::npos) {
        size_t start = effect_pos + 7;
        size_t end = gcode.find_first_of(" \t\r\n", start);
        std::string effect_name = gcode.substr(start, end - start);
        std::string full_name = "led_effect " + effect_name;

        // klipper-led_effect's STOP=1: stop just the named effect. The
        // others keep running and no color change is implied.
        size_t stop_pos = gcode.find("STOP=");
        if (stop_pos != std::string::npos) {
            size_t vstart = stop_pos + 5;
            size_t vend = gcode.find_first_of(" \t\r\n", vstart);
            // klipper-led_effect reads STOP with get_int: any nonzero
            // value stops the effect
            if (std::atoi(gcode.substr(vstart, vend - vstart).c_str()) != 0) {
                spdlog::info("[MoonrakerClientMock] SET_LED_EFFECT: stopping '{}'", effect_name);
                {
                    std::lock_guard<std::mutex> lock(led_mutex_);
                    enabled_led_effects_.erase(full_name);
                }
                dispatch_status_update(json{{full_name, {{"enabled", false}}}});
                return std::nullopt;
            }
        }

        spdlog::info("[MoonrakerClientMock] SET_LED_EFFECT: enabling '{}'", effect_name);

        // Build status update: enable the target effect, disable all others
        json effect_status = json::object();

        // Known mock effects
        const std::vector<std::string> known_effects = {
            "led_effect breathing", "led_effect fire_comet", "led_effect rainbow",
            "led_effect static_white"};

        for (const auto& name : known_effects) {
            bool should_enable = (name == full_name);
            effect_status[name] = {{"enabled", should_enable}};
        }

        // A plugin's effect name is a free string; the frame must name
        // the enabled effect even when it is not a built-in.
        effect_status[full_name] = {{"enabled", true}};

        // The handler is exclusive: enabling one effect makes it the
        // only one running.
        {
            std::lock_guard<std::mutex> lock(led_mutex_);
            enabled_led_effects_ = {full_name};
        }

        // Simulate LED color output: each effect has a characteristic color
        // In real Klipper, led_effect continuously updates the neopixel color_data
        struct EffectColor {
            double r, g, b, w;
        };
        static const std::unordered_map<std::string, EffectColor> effect_colors = {
            {"breathing", {0.6, 0.6, 1.0, 0.0}},    // Soft blue-white pulse
            {"fire_comet", {1.0, 0.3, 0.0, 0.0}},   // Orange/fire
            {"rainbow", {0.5, 0.0, 1.0, 0.0}},      // Purple (mid-rainbow)
            {"static_white", {1.0, 1.0, 1.0, 0.0}}, // Pure white
        };

        auto color_it = effect_colors.find(effect_name);
        if (color_it != effect_colors.end()) {
            const auto& c = color_it->second;
            // Update internal LED state and dispatch color_data for all LED strips
            {
                std::lock_guard<std::mutex> lock(led_mutex_);
                for (auto& [name, color] : led_states_) {
                    color = LedColor{c.r, c.g, c.b, c.w};
                }
            }
            json led_status;
            {
                std::lock_guard<std::mutex> lock(led_mutex_);
                for (const auto& [name, color] : led_states_) {
                    led_status[name] = {
                        {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
                }
            }
            // Merge LED color updates into the effect status dispatch
            effect_status.update(led_status);
        }

        dispatch_status_update(effect_status);
    }
    return std::nullopt;
}

// STOP_LED_EFFECTS - Disable all LED effects
MoonrakerClientMock::GcodeResult MoonrakerClientMock::gcode_stop_led_effects(const std::string&) {
    spdlog::info("[MoonrakerClientMock] STOP_LED_EFFECTS: disabling all effects");

    json effect_status = json::object();
    const std::vector<std::string> known_effects = {"led_effect breathing", "led_effect fire_comet",
                                                    "led_effect rainbow",
                                                    "led_effect static_white"};

    for (const auto& name : known_effects) {
        effect_status[name] = {{"enabled", false}};
    }

    // Turn LEDs off when effects stop
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        enabled_led_effects_.clear();
        for (auto& [name, color] : led_states_) {
            color = LedColor{0.0, 0.0, 0.0, 0.0};
        }
    }
    json led_status;
    {
        std::lock_guard<std::mutex> lock(led_mutex_);
        for (const auto& [name, color] : led_states_) {
            led_status[name] = {
                {"color_data", json::array({{color.r, color.g, color.b, color.w}})}};
        }
    }
    effect_status.update(led_status);

    dispatch_status_update(effect_status);
    return std::nullopt;
}

// Order is behaviour. Matching is by substring, not by command name, so one
// line can reach several handlers (SET_LED_EFFECT also matches SET_LED) and an
// earlier handler can end the script first (M117 returns before G28/M84 can see
// its message text). The vendor handlers come first because they match the
// command token exactly and must claim bare words such as OPEN and T<n> before
// the substring checks can.
int MoonrakerClientMock::gcode_script(const std::string& raw_gcode) {
    spdlog::trace("[MoonrakerClientMock] Mock gcode_script: {}", raw_gcode);

    // Record for test inspection (ordered history of every script handled).
    // Uses the raw, un-normalized text so tests/logs see exactly what was sent.
    record_gcode_script(raw_gcode);

    // Normalize the command token only (see normalize_gcode_command_case above).
    // Every check below sees this copy.
    const std::string g = normalize_gcode_command_case(raw_gcode);

    // Clear previous error at start
    {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        last_gcode_error_.clear();
    }

    // A handler that returns a code ends the script; nullopt lets the checks
    // after it see the same line.
    GcodeResult r;

    if (is_mock_ifs_module() && (r = gcode_ifs_module(g))) {
        return *r;
    }
    if (is_mock_medusahc() && (r = gcode_medusa(g))) {
        return *r;
    }
    if (printer_type_ == PrinterType::FLASHFORGE_CREATOR5_ZMOD && (r = gcode_zmod(g))) {
        return *r;
    }
    if (is_mock_cfs() && (r = gcode_cfs(g))) {
        return *r;
    }
    if ((r = gcode_u1_feeding(g))) {
        return *r;
    }

    // Heater and pin targets: the first match wins.
    if (has_token(g, "SET_HEATER_TEMPERATURE")) {
        r = gcode_heater_temperature(g);
    } else if (has_token(g, "SET_TEMPERATURE_FAN_TARGET")) {
        r = gcode_temperature_fan_target(g);
    } else if (has_token(g, "PANDA_BREATH_DRY_START")) {
        r = gcode_panda_dry_start(g);
    } else if (has_token(g, "PANDA_BREATH_DRY_STOP")) {
        r = gcode_panda_dry_stop(g);
    } else if (has_token(g, "SET_PIN")) {
        r = gcode_set_pin(g);
    } else if (has_token(g, "M104") || has_token(g, "M109")) {
        r = gcode_extruder_target_mcode(g);
    } else if (has_token(g, "M140") || has_token(g, "M190")) {
        r = gcode_bed_target_mcode(g);
    }
    if (r) {
        return *r;
    }

    if (is_command(g, "M117") && (r = gcode_display_message(g))) {
        return *r;
    }
    if (has_token(g, "SAVE_GCODE_STATE")) {
        gcode_save_gcode_state(g);
    }
    if (has_token(g, "G90")) {
        gcode_absolute_mode(g);
    } else if (has_token(g, "G91")) {
        gcode_relative_mode(g);
    }
    if (has_token(g, "M84") || has_token(g, "M18")) {
        gcode_disable_motors(g);
    }
    if (has_token(g, "G28")) {
        gcode_home(g);
    }
    if (has_token(g, "G0") || has_token(g, "G1")) {
        gcode_move(g);
    }
    if (has_token(g, "RESTORE_GCODE_STATE")) {
        gcode_restore_gcode_state(g);
    }
    if (g.length() >= 2 && g[0] == 'T' && std::isdigit(g[1])) {
        gcode_tool_change(g);
    }

    if (has_token(g, "SDCARD_PRINT_FILE")) {
        gcode_sdcard_print_file(g);
    } else if (is_command(g, "PAUSE")) {
        gcode_pause(g);
    } else if (is_command(g, "RESUME")) {
        gcode_resume(g);
    } else if (is_command(g, "CANCEL_PRINT")) {
        gcode_cancel_print(g);
    } else if (has_token(g, "M112")) {
        gcode_emergency_stop(g);
    }

    if (has_token(g, "M106")) {
        gcode_fan_m106(g);
    } else if (has_token(g, "M107")) {
        gcode_fan_m107(g);
    } else if (has_token(g, "SET_FAN_SPEED")) {
        gcode_set_fan_speed(g);
    }

    if (has_token(g, "G92") && has_token(g, "E")) {
        spdlog::warn("[MoonrakerClientMock] STUB: G92 E (set extruder position) NOT IMPLEMENTED");
    }
    if ((has_token(g, "G0") || has_token(g, "G1")) && has_token(g, "E")) {
        spdlog::debug("[MoonrakerClientMock] Note: Extrusion (E parameter) ignored in G0/G1");
    }

    if (has_token(g, "PID_CALIBRATE") && (r = gcode_pid_calibrate(g))) {
        return *r;
    }
    if (has_token(g, "MPC_CALIBRATE") && (r = gcode_mpc_calibrate(g))) {
        return *r;
    }
    if (has_token(g, "SAVE_CONFIG") && (r = gcode_save_config(g))) {
        return *r;
    }
    // Takes any line: a forced mesh calibration matches on its own text.
    gcode_bed_mesh(g);
    if (has_token(g, "SET_GCODE_OFFSET")) {
        gcode_set_gcode_offset(g);
    }
    if (has_token(g, "SET_TOOL_PARAMETER")) {
        gcode_set_tool_parameter(g);
    }
    if (has_token(g, "SAVE_TOOL_PARAMETER")) {
        gcode_save_tool_parameter(g);
    }
    if (has_token(g, "SHAPER_CALIBRATE")) {
        gcode_shaper_calibrate(g);
    }
    if (has_token(g, "TEST_RESONANCES")) {
        gcode_test_resonances(g);
    }
    if (has_token(g, "SET_INPUT_SHAPER")) {
        spdlog::info("[MoonrakerClientMock] SET_INPUT_SHAPER: {}", g);
    }
    if (has_token(g, "MEASURE_AXES_NOISE")) {
        spdlog::info("[MoonrakerClientMock] MEASURE_AXES_NOISE");
        dispatch_measure_axes_noise_response();
    }
    if (has_token(g, "SET_PRESSURE_ADVANCE")) {
        spdlog::warn("[MoonrakerClientMock] STUB: SET_PRESSURE_ADVANCE NOT IMPLEMENTED");
    }
    if (has_token(g, "SET_LED")) {
        gcode_set_led(g);
    }

    if (has_token(g, "FIRMWARE_RESTART")) {
        trigger_restart(/*is_firmware=*/true);
    } else if (has_token(g, "RESTART") && !has_token(g, "FIRMWARE")) {
        trigger_restart(/*is_firmware=*/false);
    }

    if (has_token(g, "PROBE_CALIBRATE") || has_token(g, "Z_ENDSTOP_CALIBRATE")) {
        gcode_probe_calibrate(g);
    }
    if (has_token(g, "TESTZ") && (r = gcode_testz(g))) {
        return *r;
    }
    if (is_command(g, "ACCEPT")) {
        gcode_accept(g);
    }
    if (is_command(g, "ABORT")) {
        gcode_abort(g);
    }
    if (has_token(g, "EXCLUDE_OBJECT") && !has_token(g, "EXCLUDE_OBJECT_DEFINE") &&
        !has_token(g, "EXCLUDE_OBJECT_START") && !has_token(g, "EXCLUDE_OBJECT_END")) {
        gcode_exclude_object(g);
    }
    if (has_token(g, "EXCLUDE_OBJECT_DEFINE")) {
        gcode_exclude_object_define(g);
    }
    if (has_token(g, "SET_LED_EFFECT")) {
        gcode_set_led_effect(g);
    }
    if (has_token(g, "STOP_LED_EFFECTS")) {
        gcode_stop_led_effects(g);
    }

    if (has_token(g, "QUAD_GANTRY_LEVEL")) {
        spdlog::warn("[MoonrakerClientMock] STUB: QUAD_GANTRY_LEVEL NOT IMPLEMENTED");
    } else if (has_token(g, "Z_TILT_ADJUST")) {
        spdlog::warn("[MoonrakerClientMock] STUB: Z_TILT_ADJUST NOT IMPLEMENTED");
    }
    if (has_token(g, "PROBE") && !has_token(g, "BED_MESH") && !has_token(g, "PROBE_CALIBRATE")) {
        spdlog::warn("[MoonrakerClientMock] STUB: PROBE command not fully implemented");
    }

    // Return error code if any error occurred (like real Moonraker)
    {
        std::lock_guard<std::mutex> lock(gcode_error_mutex_);
        if (!last_gcode_error_.empty()) {
            return 1; // Error - call get_last_gcode_error() for message
        }
    }
    return 0; // Success
}
