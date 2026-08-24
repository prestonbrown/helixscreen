// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file gcode_classify.h
 * @brief Classify a G-code script as "discretionary" (safe to refuse when busy).
 *
 * Used to gate convenience commands (fan, temp, non-homing moves, LED) at the
 * send boundary while the printer is executing a blocking non-print operation
 * (G28, BED_MESH_CALIBRATE, manual probe, ...). Those commands would otherwise
 * queue behind the blocking op and time out after 60 s. Recovery, homing,
 * probe-control, and macro scripts are NEVER discretionary so they always pass.
 * See the guards in IMoonrakerAPI::execute_gcode / MoonrakerMotionAPI::execute_gcode.
 */

#pragma once

#include <cctype>
#include <sstream>
#include <string>

namespace helix {

namespace detail {

/// Uppercased first whitespace-delimited token of @p line, with any inline `;`
/// comment stripped first. Empty for a blank / whitespace-only / comment-only line.
/// Shared by every classifier below so tokenization stays identical across them.
inline std::string gcode_first_token_upper(const std::string& line) {
    std::string l = line;
    const size_t comment = l.find(';');
    if (comment != std::string::npos) {
        l.erase(comment);
    }
    const size_t start = l.find_first_not_of(" \t\r");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = l.find_first_of(" \t\r", start);
    std::string token = l.substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (char& c : token) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return token;
}

/// Coarse category of a single (already-uppercased, whole) g-code token.
/// `Other` means "not in the discretionary set" — recovery/homing/probe/macro.
enum class GcodeCat { Other, Fan, Temp, Move, Modal, Led, Pin, Tune, Message };

inline GcodeCat categorize_gcode_token(const std::string& t) {
    if (t == "M106" || t == "M107" || t == "SET_FAN_SPEED") {
        return GcodeCat::Fan;
    }
    if (t == "M104" || t == "M140" || t == "M109" || t == "M190" || t == "M141" ||
        t == "SET_HEATER_TEMPERATURE" || t == "SET_TEMPERATURE_FAN_TARGET") {
        return GcodeCat::Temp;
    }
    if (t == "G0" || t == "G1") {
        return GcodeCat::Move;
    }
    if (t == "G90" || t == "G91") {
        return GcodeCat::Modal;
    }
    // SET_LED_EFFECT drives led_effect strips — an unambiguous LED write.
    if (t == "SET_LED" || t == "SET_LED_EFFECT") {
        return GcodeCat::Led;
    }
    // SET_PIN drives ANY output_pin — an LED strip (case light, neopixel-as-pin)
    // OR a non-numeric output_pin fan (fan_gcode.h). The token alone can't tell
    // which, so it gets its own category rather than being misnamed "LED change".
    // Still safe to run late either way.
    if (t == "SET_PIN") {
        return GcodeCat::Pin;
    }
    // Speed/flow factor: pure scaling, harmless if it lands a moment late.
    if (t == "M220" || t == "M221") {
        return GcodeCat::Tune;
    }
    if (t == "M117") {
        return GcodeCat::Message;
    }
    return GcodeCat::Other;
}

} // namespace detail

/// True if @p script consists ENTIRELY of discretionary commands — convenience
/// commands that are safe to refuse while the printer is busy with a blocking
/// operation. Default-ALLOW: returns true ONLY for the known discretionary set,
/// so anything unrecognized (recovery, homing, probe control, macros) is treated
/// as important and returns false.
///
/// Discretionary command set (first whitespace-delimited token of a line,
/// case-insensitive, WHOLE-token compare):
///   - Fan:  M106, M107, SET_FAN_SPEED
///   - Temp: M104, M140, M109, M190, M141 (chamber macro),
///           SET_HEATER_TEMPERATURE, SET_TEMPERATURE_FAN_TARGET
///     (every target-setting form IMoonrakerAPI::set_temperature can emit —
///      see build_heater_gcode())
///   - Move: G0, G1  (non-homing moves)
///   - Positioning mode: G90, G91 (absolute/relative — wrap our own jog moves,
///     which are emitted as "G91\nG0 X..\nG90"; pure modal state, safe to defer)
///   - LED:  SET_LED, SET_LED_EFFECT
///   - Pin:  SET_PIN — drives ANY output_pin (an LED strip OR a non-numeric
///     output_pin fan, see fan_gcode.h); kept separate from LED so the toast
///     noun doesn't misname a fan pin as an "LED change"
///   - Tune: M220 (speed factor), M221 (flow factor)
///   - Message: M117
///
/// DELIBERATELY EXCLUDED: SET_GCODE_OFFSET. It CONTROLS a blocking op — z-offset
/// calibration sends it mid-probe — so queuing it behind that op would break
/// calibration. Same class as TESTZ/ACCEPT/ABORT. Do not add it.
///
/// Before adding any command here, confirm every caller that emits it either holds
/// no in-flight counter or passes execute_gcode's on_queued. Marking a command
/// discretionary routes it down the fire-and-forget queue path, which DROPS the RPC
/// response — a caller counting on on_success/on_error alone will wedge (#1129).
///
/// Multi-line: returns true ONLY if the script is non-empty AND every non-blank
/// command line's first token is in the discretionary set. If ANY non-blank line
/// is non-discretionary (e.g. a compound macro containing a G28 or TESTZ), the
/// whole script is NOT discretionary (returns false → allowed). Blank lines,
/// whitespace-only lines, and comment-only lines are ignored. Inline `;` comments
/// are stripped before tokenizing, so "M106 S128 ; cool" tokenizes as "M106".
///
/// Whole-token matching keeps prefixes out: "M1090", "G10", "M10", and
/// "SET_FAN_SPEED_EXTRA" all return false — a substring match would wrongly trip.
inline bool is_discretionary_gcode(const std::string& script) {
    std::istringstream lines(script);
    std::string line;
    bool saw_command = false;
    while (std::getline(lines, line)) {
        const std::string token = detail::gcode_first_token_upper(line);
        if (token.empty()) {
            continue; // blank / whitespace-only / comment-only line
        }
        if (detail::categorize_gcode_token(token) == detail::GcodeCat::Other) {
            return false; // any non-discretionary command → allow the whole script
        }
        saw_command = true;
    }
    return saw_command;
}

/// True if @p script contains a physical MOVE (G0/G1) on any line.
///
/// A discretionary script that moves the toolhead must NOT be queued behind a
/// blocking op the way fan/temp/LED can be: a jog that fires minutes late — after
/// the user has walked away — can crash the head. The busy guard uses this to keep
/// rejecting moves while it lets the benign rest queue. Whole-token, comment-aware
/// (so "G10"/"G100" do not trip). Bare G90/G91 are modal-only, not moves.
inline bool gcode_contains_move(const std::string& script) {
    std::istringstream lines(script);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string token = detail::gcode_first_token_upper(line);
        if (detail::categorize_gcode_token(token) == detail::GcodeCat::Move) {
            return true;
        }
    }
    return false;
}

/// A short human noun naming what a benign discretionary @p script changes, for
/// the "queued — runs when it's ready" toast: "temperature change", "fan change",
/// "LED change", "speed change", "flow change", "display message", or a generic
/// "change". Returns the first meaningful line's kind (modal G90/G91 wrappers and
/// unrecognized lines are skipped), so a jog-style "G91\nM106" is named for its
/// fan line. SET_PIN (GcodeCat::Pin) always falls through to the generic "change"
/// — it can't be told apart from a non-LED output_pin fan at this layer.
inline std::string discretionary_gcode_noun(const std::string& script) {
    std::istringstream lines(script);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string token = detail::gcode_first_token_upper(line);
        switch (detail::categorize_gcode_token(token)) {
        case detail::GcodeCat::Temp:
            return "temperature change";
        case detail::GcodeCat::Fan:
            return "fan change";
        case detail::GcodeCat::Led:
            return "LED change";
        case detail::GcodeCat::Tune:
            return token == "M221" ? "flow change" : "speed change";
        case detail::GcodeCat::Message:
            return "display message";
        case detail::GcodeCat::Pin:
            // SET_PIN can't be told apart from a fan pin at this layer — fall
            // through to the generic noun rather than claiming "LED change".
            break;
        case detail::GcodeCat::Move:
        case detail::GcodeCat::Modal:
        case detail::GcodeCat::Other:
            break; // keep scanning for a line that names a benign change
        }
    }
    return "change";
}

} // namespace helix
