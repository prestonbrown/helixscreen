// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_motion_api.h"

#include "ui_error_reporting.h"
#include "ui_notification.h"

#include "gcode_classify.h"
#include "gcode_homing.h"
#include "jog_coalescer.h"
#include "moonraker_client.h"
#include "moonraker_gcode_guards.h"
#include "moonraker_types.h"
#include "printer_state.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "hv/json.hpp"

// Local validation helpers (same as moonraker_api_internal.h but standalone)
namespace {

bool is_valid_axis(char axis) {
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));
    return upper == 'X' || upper == 'Y' || upper == 'Z' || upper == 'E';
}

bool is_safe_distance(double distance, const SafetyLimits& limits) {
    return distance >= limits.min_relative_distance_mm &&
           distance <= limits.max_relative_distance_mm;
}

bool is_safe_position(double position, const SafetyLimits& limits) {
    return position >= limits.min_absolute_position_mm &&
           position <= limits.max_absolute_position_mm;
}

bool is_safe_feedrate(double feedrate, const SafetyLimits& limits) {
    return feedrate >= limits.min_feedrate_mm_min && feedrate <= limits.max_feedrate_mm_min;
}

/**
 * Format a numeric value for G-code without ever emitting scientific notation.
 *
 * A bare `ostringstream << double` uses defaultfloat: roughly 6 significant
 * digits, switching to scientific notation for small magnitudes. Klipper
 * tolerates "X1e-17", but it is unreadable in logs and one clamp change away
 * from a genuinely odd command — clamp_jog_delta can return a real sub-micron
 * residual (predicted=199.9999995, +1, max=200 -> ~5e-7).
 *
 * Fixed notation with trailing zeros trimmed keeps ordinary values byte-identical
 * to the old defaultfloat output ("4", "0.5", "-2", "6000") while making
 * scientific output unrepresentable for anything that survives the epsilon gate.
 */
std::string format_gcode_value(double v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6) << v;
    std::string s = ss.str();
    // std::fixed always emits a decimal point for finite values (NaN/Inf are
    // rejected upstream), so the trim is unconditional.
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s;
}

bool reject_non_finite(std::initializer_list<double> values, const char* method,
                       const MoonrakerMotionAPI::ErrorCallback& on_error) {
    for (double v : values) {
        if (std::isnan(v) || std::isinf(v)) {
            spdlog::warn("[Motion API] {}: Rejecting G-code generation: "
                         "invalid value (NaN/Inf)",
                         method);
            if (on_error) {
                on_error(MoonrakerError::validation_error(method,
                                                          "Parameter contains NaN or Inf value"));
            }
            return true;
        }
    }
    return false;
}

} // namespace

// ============================================================================
// MoonrakerMotionAPI Implementation
// ============================================================================

MoonrakerMotionAPI::MoonrakerMotionAPI(helix::IMoonrakerClient& client, helix::PrinterState& state,
                                       const SafetyLimits& safety_limits)
    : client_(client), state_(state), safety_limits_(safety_limits) {}

// ============================================================================
// Motion Control Operations
// ============================================================================

void MoonrakerMotionAPI::home_axes(const std::string& axes, SuccessCallback on_success,
                                   ErrorCallback on_error) {
    // Validate axes string (empty means all, or contains only XYZE)
    if (!axes.empty()) {
        for (char axis : axes) {
            if (!is_valid_axis(axis)) {
                NOTIFY_ERROR("Invalid axis '{}' in homing command. Must be X, Y, Z, or E.", axis);
                if (on_error) {
                    MoonrakerError err = MoonrakerError::validation_error(
                        "home_axes", "Invalid axis character (must be X, Y, Z, or E)");
                    on_error(err);
                }
                return;
            }
        }
    }

    std::string gcode = generate_home_gcode(axes);
    spdlog::info("[Motion API] Homing axes: {} (G-code: {})", axes.empty() ? "all" : axes, gcode);

    execute_gcode(gcode, on_success, on_error, HOMING_TIMEOUT_MS);
}

void MoonrakerMotionAPI::move_axis(char axis, double distance, double feedrate,
                                   SuccessCallback on_success, ErrorCallback on_error) {
    // Reject NaN/Inf before any G-code generation
    if (reject_non_finite({distance, feedrate}, "move_axis", on_error)) {
        return;
    }

    // Validate axis
    if (!is_valid_axis(axis)) {
        NOTIFY_ERROR("Invalid axis '{}'. Must be X, Y, Z, or E.", axis);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "move_axis", "Invalid axis: " + std::string(1, axis) + " (must be X, Y, Z, or E)");
            on_error(err);
        }
        return;
    }

    // Validate distance is within safety limits
    if (!is_safe_distance(distance, safety_limits_)) {
        NOTIFY_ERROR("Move distance {:.1f}mm is too large. Maximum: {:.1f}mm.", std::abs(distance),
                     safety_limits_.max_relative_distance_mm);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "move_axis", "Distance " + std::to_string(distance) + "mm exceeds safety limits (" +
                                 std::to_string(safety_limits_.min_relative_distance_mm) + "-" +
                                 std::to_string(safety_limits_.max_relative_distance_mm) + "mm)");
            on_error(err);
        }
        return;
    }

    // Validate feedrate if specified (0 means use default, negative is invalid)
    if (feedrate != 0 && !is_safe_feedrate(feedrate, safety_limits_)) {
        NOTIFY_ERROR("Speed {:.0f}mm/min is too fast. Maximum: {:.0f}mm/min.", feedrate,
                     safety_limits_.max_feedrate_mm_min);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "move_axis", "Feedrate " + std::to_string(feedrate) +
                                 "mm/min exceeds safety limits (" +
                                 std::to_string(safety_limits_.min_feedrate_mm_min) + "-" +
                                 std::to_string(safety_limits_.max_feedrate_mm_min) + "mm/min)");
            on_error(err);
        }
        return;
    }

    std::string gcode = generate_move_gcode(axis, distance, feedrate);
    spdlog::info("[Motion API] Moving axis {} by {}mm (G-code: {})", axis, distance, gcode);

    execute_gcode(gcode, on_success, on_error);
}

void MoonrakerMotionAPI::move_relative(double dx, double dy, double dz, double xy_feedrate,
                                       double z_feedrate, SuccessCallback on_success,
                                       ErrorCallback on_error) {
    if (reject_non_finite({dx, dy, dz, xy_feedrate, z_feedrate}, "move_relative", on_error)) {
        return;
    }

    // Per-axis distance safety (same limits as move_axis).
    const struct {
        char axis;
        double dist;
    } deltas[] = {{'X', dx}, {'Y', dy}, {'Z', dz}};
    for (const auto& d : deltas) {
        if (d.dist != 0.0 && !is_safe_distance(d.dist, safety_limits_)) {
            NOTIFY_ERROR("Move distance {:.1f}mm is too large. Maximum: {:.1f}mm.",
                         std::abs(d.dist), safety_limits_.max_relative_distance_mm);
            if (on_error) {
                MoonrakerError err = MoonrakerError::validation_error(
                    "move_relative", "Distance " + std::to_string(d.dist) +
                                         "mm exceeds safety limits on axis " +
                                         std::string(1, d.axis));
                on_error(err);
            }
            return;
        }
    }
    for (double f : {xy_feedrate, z_feedrate}) {
        if (f != 0 && !is_safe_feedrate(f, safety_limits_)) {
            NOTIFY_ERROR("Speed {:.0f}mm/min is too fast. Maximum: {:.0f}mm/min.", f,
                         safety_limits_.max_feedrate_mm_min);
            if (on_error) {
                MoonrakerError err = MoonrakerError::validation_error(
                    "move_relative",
                    "Feedrate " + std::to_string(f) + "mm/min exceeds safety limits");
                on_error(err);
            }
            return;
        }
    }

    std::string gcode = generate_relative_move_gcode(dx, dy, dz, xy_feedrate, z_feedrate);
    if (gcode.empty()) {
        if (on_success) {
            on_success(); // nothing to do — treat as trivially complete
        }
        return;
    }
    spdlog::info("[Motion API] Relative move dx={} dy={} dz={} (G-code: {})", dx, dy, dz, gcode);
    execute_gcode(gcode, on_success, on_error);
}

void MoonrakerMotionAPI::move_to_position(char axis, double position, double feedrate,
                                          SuccessCallback on_success, ErrorCallback on_error) {
    // Reject NaN/Inf before any G-code generation
    if (reject_non_finite({position, feedrate}, "move_to_position", on_error)) {
        return;
    }

    // Validate axis
    if (!is_valid_axis(axis)) {
        NOTIFY_ERROR("Invalid axis '{}'. Must be X, Y, Z, or E.", axis);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "move_to_position", "Invalid axis character (must be X, Y, Z, or E)");
            on_error(err);
        }
        return;
    }

    // Validate position is within safety limits
    if (!is_safe_position(position, safety_limits_)) {
        NOTIFY_ERROR("Position {:.1f}mm is out of range. Valid: {:.1f}mm to {:.1f}mm.", position,
                     safety_limits_.min_absolute_position_mm,
                     safety_limits_.max_absolute_position_mm);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "move_to_position",
                "Position " + std::to_string(position) + "mm exceeds safety limits (" +
                    std::to_string(safety_limits_.min_absolute_position_mm) + "-" +
                    std::to_string(safety_limits_.max_absolute_position_mm) + "mm)");
            on_error(err);
        }
        return;
    }

    // Validate feedrate if specified (0 means use default, negative is invalid)
    if (feedrate != 0 && !is_safe_feedrate(feedrate, safety_limits_)) {
        NOTIFY_ERROR("Speed {:.0f}mm/min is too fast. Maximum: {:.0f}mm/min.", feedrate,
                     safety_limits_.max_feedrate_mm_min);
        if (on_error) {
            MoonrakerError err = MoonrakerError::validation_error(
                "move_to_position",
                "Feedrate " + std::to_string(feedrate) + "mm/min exceeds safety limits (" +
                    std::to_string(safety_limits_.min_feedrate_mm_min) + "-" +
                    std::to_string(safety_limits_.max_feedrate_mm_min) + "mm/min)");
            on_error(err);
        }
        return;
    }

    std::string gcode = generate_absolute_move_gcode(axis, position, feedrate);
    spdlog::info("[Motion API] Moving axis {} to {}mm (G-code: {})", axis, position, gcode);

    execute_gcode(gcode, on_success, on_error);
}

// ============================================================================
// G-code Generation Helpers
// ============================================================================

std::string MoonrakerMotionAPI::generate_home_gcode(const std::string& axes) {
    if (axes.empty()) {
        return "G28"; // Home all axes
    } else {
        std::ostringstream gcode;
        gcode << "G28";
        for (char axis : axes) {
            gcode << " " << static_cast<char>(std::toupper(axis));
        }
        return gcode.str();
    }
}

std::string MoonrakerMotionAPI::generate_move_gcode(char axis, double distance, double feedrate) {
    if (std::isnan(distance) || std::isinf(distance) || std::isnan(feedrate) ||
        std::isinf(feedrate)) {
        spdlog::warn("[Motion API] generate_move_gcode: Rejecting G-code generation: "
                     "invalid value (NaN/Inf)");
        return "";
    }

    std::ostringstream gcode;
    gcode << "G91\n"; // Relative positioning
    gcode << "G0 " << static_cast<char>(std::toupper(axis)) << distance;
    if (feedrate > 0) {
        gcode << " F" << feedrate;
    }
    gcode << "\nG90"; // Back to absolute positioning
    return gcode.str();
}

std::string MoonrakerMotionAPI::generate_relative_move_gcode(double dx, double dy, double dz,
                                                             double xy_feedrate,
                                                             double z_feedrate) {
    const double vals[] = {dx, dy, dz, xy_feedrate, z_feedrate};
    for (double v : vals) {
        if (std::isnan(v) || std::isinf(v)) {
            spdlog::warn("[Motion API] generate_relative_move_gcode: Rejecting G-code "
                         "generation: invalid value (NaN/Inf)");
            return "";
        }
    }
    // Gate each axis on the SAME epsilon that AxisMove::any() uses to decide
    // whether a coalesced move is worth flushing at all. An exact != 0.0 test
    // here disagreed with that gate: a cross-axis reversal nets ~1e-17 of float
    // cancellation residue onto one axis while another carries a real delta, and
    // the residue was serialized as a real term ("G0 X1e-17 Y2").
    const bool move_x = std::abs(dx) > helix::AxisMove::EPSILON_MM;
    const bool move_y = std::abs(dy) > helix::AxisMove::EPSILON_MM;
    const bool move_z = std::abs(dz) > helix::AxisMove::EPSILON_MM;
    if (!move_x && !move_y && !move_z) {
        return ""; // includes the all-zero case
    }

    std::ostringstream gcode;
    gcode << "G91";
    if (move_x || move_y) {
        gcode << "\nG0";
        if (move_x) {
            gcode << " X" << format_gcode_value(dx);
        }
        if (move_y) {
            gcode << " Y" << format_gcode_value(dy);
        }
        if (xy_feedrate > 0) {
            gcode << " F" << format_gcode_value(xy_feedrate);
        }
    }
    if (move_z) {
        gcode << "\nG0 Z" << format_gcode_value(dz);
        if (z_feedrate > 0) {
            gcode << " F" << format_gcode_value(z_feedrate);
        }
    }
    gcode << "\nG90";
    return gcode.str();
}

std::string MoonrakerMotionAPI::generate_absolute_move_gcode(char axis, double position,
                                                             double feedrate) {
    if (std::isnan(position) || std::isinf(position) || std::isnan(feedrate) ||
        std::isinf(feedrate)) {
        spdlog::warn("[Motion API] generate_absolute_move_gcode: Rejecting G-code generation: "
                     "invalid value (NaN/Inf)");
        return "";
    }

    std::ostringstream gcode;
    gcode << "G90\n"; // Absolute positioning
    gcode << "G0 " << static_cast<char>(std::toupper(axis)) << position;
    if (feedrate > 0) {
        gcode << " F" << feedrate;
    }
    return gcode.str();
}

// ============================================================================
// G-code Execution
// ============================================================================

void MoonrakerMotionAPI::execute_gcode(const std::string& gcode, SuccessCallback on_success,
                                       ErrorCallback on_error, uint32_t timeout_ms,
                                       bool caller_surfaces_errors) {
    // Built from the CALLER's own on_error, before the activity-counter wrapping
    // further down makes error_wrapper non-null. silent=true when the caller
    // provides on_error: it handles error display, so the request tracker's
    // generic RPC_ERROR toast stays quiet. surfaces_errors additionally decides
    // whether Klipper's `!!` broadcast for the same rejection dedups against the
    // caller's toast — only a callback that really shows something earns that.
    // See include/rpc_error_policy.h.
    const helix::rpc_error_policy::CallerIntent intent{/*silent=*/(on_error != nullptr),
                                                       /*surfaces_errors=*/(on_error != nullptr) &&
                                                           caller_surfaces_errors};
    const bool silent = intent.silent;

    // Refuse motion G-code (jog/home/move) unless Klipper is READY. When klippy
    // is starting up, halted, or errored, printer.gcode.script is rejected by
    // Klipper with a raw "Printer not ready"/initialization error — and during
    // STARTUP the command is instead silently queued and can fire minutes later,
    // long after the user has moved on. Motion is discretionary user input with
    // no recovery role (recovery uses dedicated RPCs: firmware_restart, etc.), so
    // block it at the boundary and surface a friendly toast. Stricter than the
    // klippy gate in MoonrakerAPI::execute_gcode, which lets STARTUP through so
    // queued recovery gcode can run — jog moves must NOT queue-and-fire-late.
    {
        const int klippy = lv_subject_get_int(state_.get_klippy_state_subject());
        if (klippy != static_cast<int>(helix::KlippyState::READY)) {
            if (!silent) {
                spdlog::warn("[Motion API] Refusing motion G-code while Klipper not ready "
                             "(state={}): '{}'",
                             klippy, gcode.substr(0, 60));
            }
            if (on_error) {
                on_error(MoonrakerError::not_ready("printer.gcode.script", "Printer is not ready"));
            }
            return;
        }
    }

    // Refuse app-initiated homing while a print is active. The Home buttons and
    // calibration/mesh auto-homes route through here; a mid-print G28 drives the
    // nozzle into the part on loadcell-Z printers (see MoonrakerAPI::execute_gcode
    // for the full collision rationale). "Active" = PRINTING or PAUSED.
    if (helix::api::reject_homing_during_active_print(gcode, state_, silent, on_error,
                                                      "[Motion API]")) {
        return;
    }

    // Refuse discretionary gcode (non-homing jog moves) while a blocking non-print
    // operation holds Klipper's single-threaded gcode lock — a jog that fires late,
    // after the user has moved on, is dangerous. This API only ever carries moves,
    // so it always refuses; the benign fan/temp/LED half of the split (queue with a
    // per-episode toast, #1108) lives in MoonrakerAPI::execute_gcode. Homing/recovery/
    // probe-control pass through. Uses the attributed predicate: self-busy from our
    // own recent jog passes (idle_timeout reports "Printing" during any move),
    // external ops still refuse.
    if (helix::is_discretionary_gcode(gcode) && state_.is_external_blocking_operation_active()) {
        if (!silent) {
            spdlog::warn("[Motion API] Refusing discretionary G-code while printer is "
                         "homing/leveling: '{}'",
                         gcode.substr(0, 60));
        }
        if (on_error) {
            on_error(MoonrakerError::not_ready("printer.gcode.script",
                                               "Printer is busy — try again in a moment"));
        }
        return;
    }

    // Transmitted VERBATIM — see moonraker_gcode_guards.h.
    json params = {{"script", gcode}};

    spdlog::trace("[Motion API] Executing G-code: {}", gcode);

    // Stamp app-initiated motion activity for discretionary (jog) gcode so the
    // busy guard can attribute the resulting idle_timeout "Printing" to us.
    // Both callbacks are wrapped: the request tracker guarantees exactly one
    // fires (success, error, or timeout), keeping the inflight count balanced.
    // NOTE: `intent` was computed from the CALLER's on_error above, before this
    // wrapping — error_wrapper is non-null whenever we stamp, so reading it here
    // would report a promise the caller never made.
    const bool stamp = helix::is_discretionary_gcode(gcode);
    helix::PrinterState* ps = &state_;
    if (stamp) {
        ps->app_motion_activity().note_sent();
    }

    std::function<void(const json&)> success_wrapper;
    if (on_success || stamp) {
        success_wrapper = [on_success, ps, stamp](json) {
            if (stamp) {
                ps->app_motion_activity().note_done();
            }
            if (on_success) {
                on_success();
            }
        };
    }
    ErrorCallback error_wrapper = on_error;
    if (stamp) {
        error_wrapper = [on_error, ps](const MoonrakerError& err) {
            ps->app_motion_activity().note_done();
            if (on_error) {
                on_error(err);
            }
        };
    }
    client_.send_jsonrpc("printer.gcode.script", params, std::move(success_wrapper),
                         std::move(error_wrapper), timeout_ms, intent.silent, intent);
}
