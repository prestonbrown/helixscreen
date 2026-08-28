// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "printer_motion_state.h"

#include <optional>

namespace helix::calibration {

/// Why the live belt tuner cannot run, or OK if it can.
enum class BeltGate {
    OK,
    NOT_CONNECTED,     ///< no printer connection or klippy is not ready
    NO_ACCELEROMETER,  ///< no adxl345/lis2dw/... section in printer.cfg
    NOT_COREXY,        ///< the A/B belt-path model does not apply
    NOT_COLOCATED,     ///< klippy's UDS socket is not reachable from here
    HARDWARE_TOO_SLOW, ///< the DSP probe failed - see belt_dsp_probe.h
    PRINTING,          ///< a print job owns the toolhead
};

struct BeltGateInputs {
    bool connected = false;
    bool has_accelerometer = false;
    bool is_corexy = false;
    bool klippy_socket_reachable = false;
    bool dsp_capable = false;
    bool print_active = false;
};

/**
 * @brief Decide whether the live tuner can run
 *
 * Permanent blockers are reported before transient ones. A bed slinger that is
 * also mid-print reports NOT_COREXY, not PRINTING - telling that user to wait
 * for the print to finish promises something that will never arrive.
 */
BeltGate evaluate_belt_gate(const BeltGateInputs& in);

/// Untranslated explanatory text for a gate. Never null, never empty.
const char* belt_gate_message(BeltGate gate);

/// Voron's documented reference span, idler centre to idler centre. At correct
/// tension a span this long rings at 110 Hz.
inline constexpr float TARGET_SPAN_MM = 150.0f;

struct ParkTarget {
    float y_mm = 0.0f;
    bool valid = false;
};

/**
 * @brief Gantry Y that produces the requested free span
 *
 * On the reference Voron 2.4 300mm, span in mm is about Y + 35, measured with a
 * tape at two positions (Y100 -> 135 mm, Y115 -> 151 mm). The offset is
 * geometry, so it is a per-model constant, and it is validated on exactly one
 * machine.
 *
 * @param target_span_mm Free span wanted, normally TARGET_SPAN_MM
 * @param span_offset_mm The model's measured offset, or nullopt if unknown.
 *        nullopt yields an invalid target on purpose: a guessed offset skews
 *        the absolute frequency target by about 7 Hz per 10 mm of error, and
 *        the caller should fall back to span-independent A-vs-B matching.
 * @param bounds The machine's kinematic envelope
 * @return valid == false if the offset is unknown, Y bounds are unknown, or the
 *         computed Y falls outside the envelope
 */
ParkTarget park_y_for_span(float target_span_mm, std::optional<float> span_offset_mm,
                           const AxisBounds& bounds);

/**
 * @brief Gantry X that centres the toolhead in its kinematic envelope
 *
 * Y sets the free span on a CoreXY (see park_y_for_span()); X sets neither
 * span. But a park that only moves Y leaves the toolhead wherever it
 * happened to be, and near either end of the X envelope that is directly
 * over the belt the user is being asked to pluck - observed on the
 * reference machine, parked at X=285 on a 300 mm bed with the hint pointing
 * at the front-right belt. Centring X is a second positioning move, exactly
 * as cheap as the Y one.
 *
 * @param bounds The machine's kinematic envelope
 * @return nullopt if X bounds are not known yet - has_x is false until the
 *         first axis-bounds subscription update arrives, and a park at that
 *         point must skip the move rather than guess X=0.
 */
std::optional<float> park_x_center(const AxisBounds& bounds);

} // namespace helix::calibration
