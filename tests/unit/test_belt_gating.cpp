// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_gating.h"
#include "printer_detector.h"

#include "../catch_amalgamated.hpp"

using helix::AxisBounds;
using helix::calibration::belt_gate_message;
using helix::calibration::BeltGate;
using helix::calibration::BeltGateInputs;
using helix::calibration::evaluate_belt_gate;
using helix::calibration::park_x_center;
using helix::calibration::park_y_for_span;
using helix::calibration::TARGET_SPAN_MM;

namespace {

/// Every gate satisfied - the shape the panel sees on a healthy reference Voron.
BeltGateInputs all_clear() {
    BeltGateInputs in;
    in.connected = true;
    in.has_accelerometer = true;
    in.is_corexy = true;
    in.klippy_socket_reachable = true;
    in.dsp_capable = true;
    in.print_active = false;
    return in;
}

AxisBounds voron_300() {
    AxisBounds b;
    b.y_min = 0.0f;
    b.y_max = 300.0f;
    b.has_y = true;
    return b;
}

} // namespace

TEST_CASE("a healthy co-located CoreXY passes every gate", "[belt][gating]") {
    CHECK(evaluate_belt_gate(all_clear()) == BeltGate::OK);
}

TEST_CASE("each blocker is reported", "[belt][gating]") {
    auto in = all_clear();
    in.connected = false;
    CHECK(evaluate_belt_gate(in) == BeltGate::NOT_CONNECTED);

    in = all_clear();
    in.has_accelerometer = false;
    CHECK(evaluate_belt_gate(in) == BeltGate::NO_ACCELEROMETER);

    in = all_clear();
    in.is_corexy = false;
    CHECK(evaluate_belt_gate(in) == BeltGate::NOT_COREXY);

    in = all_clear();
    in.klippy_socket_reachable = false;
    CHECK(evaluate_belt_gate(in) == BeltGate::NOT_COLOCATED);

    in = all_clear();
    in.dsp_capable = false;
    CHECK(evaluate_belt_gate(in) == BeltGate::HARDWARE_TOO_SLOW);

    in = all_clear();
    in.print_active = true;
    CHECK(evaluate_belt_gate(in) == BeltGate::PRINTING);
}

TEST_CASE("permanent blockers outrank an active print", "[belt][gating]") {
    // A bed slinger mid-print must not be told to wait for the print to end -
    // the feature will never work there, and "wait" is a promise we cannot keep.
    auto in = all_clear();
    in.is_corexy = false;
    in.print_active = true;
    CHECK(evaluate_belt_gate(in) == BeltGate::NOT_COREXY);

    in = all_clear();
    in.has_accelerometer = false;
    in.print_active = true;
    CHECK(evaluate_belt_gate(in) == BeltGate::NO_ACCELEROMETER);
}

TEST_CASE("disconnection outranks everything", "[belt][gating]") {
    BeltGateInputs in{}; // all false, including connected
    CHECK(evaluate_belt_gate(in) == BeltGate::NOT_CONNECTED);
}

TEST_CASE("every gate has a non-empty message", "[belt][gating]") {
    for (auto g :
         {BeltGate::OK, BeltGate::NOT_CONNECTED, BeltGate::NO_ACCELEROMETER, BeltGate::NOT_COREXY,
          BeltGate::NOT_COLOCATED, BeltGate::HARDWARE_TOO_SLOW, BeltGate::PRINTING}) {
        const char* m = belt_gate_message(g);
        REQUIRE(m != nullptr);
        CHECK(m[0] != '\0');
    }
}

TEST_CASE("gate inputs assembled at panel entry reflect a fresh connection",
          "[belt][gating][panel]") {
    // At panel entry the accelerometer subject may still be 0 - discovery
    // populates it from configfile.config, which arrives after connect. A gate
    // that only ever ran against a settled value would pass here and then let
    // a user press Start on a printer with no accelerometer.
    BeltGateInputs in;
    in.connected = true;
    in.has_accelerometer = false; // not yet discovered
    in.is_corexy = true;
    in.klippy_socket_reachable = true;
    in.dsp_capable = true;
    in.print_active = false;
    CHECK(evaluate_belt_gate(in) == BeltGate::NO_ACCELEROMETER);
}

TEST_CASE("park_y_for_span reproduces the measured reference geometry", "[belt][gating][span]") {
    // Measured on the reference Voron 2.4 300mm: Y115 produced a 151 mm span,
    // so the offset is 35 mm and a 150 mm span wants Y115.
    const auto p = park_y_for_span(TARGET_SPAN_MM, 35.0f, voron_300());
    REQUIRE(p.valid);
    CHECK(p.y_mm == Catch::Approx(115.0f));
}

TEST_CASE("park_y_for_span refuses when the model has no measured offset", "[belt][gating][span]") {
    // Guessing costs about 7 Hz of target error per 10 mm. The panel must fall
    // back to A-vs-B matching, which is span-independent.
    const auto p = park_y_for_span(TARGET_SPAN_MM, std::nullopt, voron_300());
    CHECK_FALSE(p.valid);
}

TEST_CASE("park_y_for_span refuses a target outside the machine", "[belt][gating][span]") {
    AxisBounds tiny = voron_300();
    tiny.y_max = 100.0f; // a 120mm-bed printer cannot reach Y115
    CHECK_FALSE(park_y_for_span(TARGET_SPAN_MM, 35.0f, tiny).valid);

    AxisBounds shifted = voron_300();
    shifted.y_min = 130.0f;
    CHECK_FALSE(park_y_for_span(TARGET_SPAN_MM, 35.0f, shifted).valid);
}

TEST_CASE("park_y_for_span refuses when Y bounds are unknown", "[belt][gating][span]") {
    // has_y distinguishes "not sent yet" from a real zero. Treating an unsent
    // bound as 0 would compute a park target against a machine we cannot size.
    AxisBounds unknown;
    unknown.has_y = false;
    unknown.y_max = 0.0f;
    CHECK_FALSE(park_y_for_span(TARGET_SPAN_MM, 35.0f, unknown).valid);
}

TEST_CASE("park_x_center is the midpoint of the X envelope", "[belt][gating][span]") {
    AxisBounds b;
    b.x_min = 0.0f;
    b.x_max = 300.0f;
    b.has_x = true;
    const auto x = park_x_center(b);
    REQUIRE(x.has_value());
    CHECK(*x == Catch::Approx(150.0f));
}

TEST_CASE("park_x_center handles an X envelope that does not start at zero",
          "[belt][gating][span]") {
    AxisBounds b;
    b.x_min = -20.0f;
    b.x_max = 280.0f;
    b.has_x = true;
    const auto x = park_x_center(b);
    REQUIRE(x.has_value());
    CHECK(*x == Catch::Approx(130.0f));
}

TEST_CASE("park_x_center refuses when X bounds are unknown", "[belt][gating][span]") {
    // Reproduces the reference-machine bug directly: has_x is false until the
    // first axis-bounds subscription update arrives, and treating an unsent
    // bound as 0 would park at X=0 on a cold start - worse than not moving.
    AxisBounds unknown;
    unknown.has_x = false;
    unknown.x_min = 0.0f;
    unknown.x_max = 0.0f;
    CHECK_FALSE(park_x_center(unknown).has_value());
}

TEST_CASE("belt span offset is negative for a model with no measured value",
          "[belt][gating][span]") {
    CHECK(PrinterDetector::get_belt_span_offset_mm("No Such Printer 9000") < 0.0);
}

TEST_CASE("belt span offset is the measured value for the Voron 2.4", "[belt][gating][span]") {
    CHECK(PrinterDetector::get_belt_span_offset_mm("Voron 2.4") == Catch::Approx(35.0));
}
