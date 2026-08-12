// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_gating.h"

namespace helix::calibration {

BeltGate evaluate_belt_gate(const BeltGateInputs& in) {
    if (!in.connected)
        return BeltGate::NOT_CONNECTED;
    if (!in.has_accelerometer)
        return BeltGate::NO_ACCELEROMETER;
    if (!in.is_corexy)
        return BeltGate::NOT_COREXY;
    if (!in.klippy_socket_reachable)
        return BeltGate::NOT_COLOCATED;
    if (!in.dsp_capable)
        return BeltGate::HARDWARE_TOO_SLOW;
    if (in.print_active)
        return BeltGate::PRINTING;
    return BeltGate::OK;
}

const char* belt_gate_message(BeltGate gate) {
    switch (gate) {
    case BeltGate::OK:
        return "Ready";
    case BeltGate::NOT_CONNECTED:
        return "Not connected to the printer";
    case BeltGate::NO_ACCELEROMETER:
        return "No accelerometer found in your Klipper config";
    case BeltGate::NOT_COREXY:
        return "Belt tuning is only available on CoreXY printers";
    case BeltGate::NOT_COLOCATED:
        return "This needs HelixScreen running on the printer itself";
    case BeltGate::HARDWARE_TOO_SLOW:
        return "This display is not fast enough to analyse belt frequencies live";
    case BeltGate::PRINTING:
        return "Wait until the print finishes";
    }
    return "Unavailable";
}

ParkTarget park_y_for_span(float target_span_mm, std::optional<float> span_offset_mm,
                           const AxisBounds& bounds) {
    ParkTarget out;
    if (!span_offset_mm.has_value() || !bounds.has_y) {
        return out;
    }
    const float y = target_span_mm - *span_offset_mm;
    if (y < bounds.y_min || y > bounds.y_max) {
        return out;
    }
    out.y_mm = y;
    out.valid = true;
    return out;
}

} // namespace helix::calibration
