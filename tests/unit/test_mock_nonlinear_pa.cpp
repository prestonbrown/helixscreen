// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_mock_nonlinear_pa.cpp
 * @brief The mock's nonlinear pressure advance (HELIX_MOCK_PA_MODEL) and the
 *        firmware PA test tower (HELIX_MOCK_PA_TEST).
 */

#include "../test_helpers/scoped_env.h"
#include "moonraker_client_mock.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;
using Phase = MoonrakerClientMock::MockPrintPhase;

namespace {

/// The extruder objects of every status frame the mock dispatched.
struct ExtruderFrames {
    explicit ExtruderFrames(MoonrakerClientMock& mock) {
        mock.register_notify_update([this](const json& n) {
            if (n.contains("params") && n["params"].is_array() && !n["params"].empty() &&
                n["params"][0].is_object() && n["params"][0].contains("extruder")) {
                frames.push_back(n["params"][0]["extruder"]);
            }
        });
    }
    std::vector<json> frames;
};

} // namespace

TEST_CASE("Mock nonlinear PA: tanh reports linear_advance, MODEL switches", "[pa_mock]") {
    helix::ScopedEnv model("HELIX_MOCK_PA_MODEL", "tanh");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    ExtruderFrames seen(mock);

    REQUIRE(mock.gcode_script("SET_PRESSURE_ADVANCE ADVANCE=0.03 OFFSET=0.01") == 0);
    REQUIRE_FALSE(seen.frames.empty());
    const json& frame = seen.frames.back();
    CHECK(frame["pressure_advance_model"] == "tanh");
    CHECK(frame["linear_advance"] == Catch::Approx(0.03));
    CHECK(frame["nonlinear_offset"] == Catch::Approx(0.01));
    CHECK_FALSE(frame.contains("pressure_advance"));

    REQUIRE(mock.gcode_script("SET_PRESSURE_ADVANCE MODEL=recipr") == 0);
    CHECK(seen.frames.back()["pressure_advance_model"] == "recipr");

    CHECK(mock.gcode_script("SET_PRESSURE_ADVANCE MODEL=bogus") != 0);
    CHECK(mock.get_last_gcode_error() == "Invalid MODEL='bogus' choice");

    mock.gcode_script("SET_PRESSURE_ADVANCE MODEL=linear");
    CHECK(seen.frames.back()["pressure_advance_model"] == "linear");
    CHECK(seen.frames.back().contains("pressure_advance"));
    CHECK_FALSE(seen.frames.back().contains("linear_advance"));
}

TEST_CASE("Mock PA tower: refuses a mismatched target, sweeps OFFSET and completes", "[pa_mock]") {
    helix::ScopedEnv model("HELIX_MOCK_PA_MODEL", "tanh");
    helix::ScopedEnv tower("HELIX_MOCK_PA_TEST", "1");
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    ExtruderFrames seen(mock);

    mock.gcode_script("M104 S200");
    CHECK(mock.gcode_script("PRINT_PA_TOWER NOZZLE=0.4 TARGET_TEMP=210") != 0);
    CHECK(mock.get_last_gcode_error().find("does not match") != std::string::npos);
    CHECK(mock.get_print_phase() == Phase::IDLE);

    mock.gcode_script("M104 S210");
    REQUIRE(
        mock.gcode_script(
            "TUNING_TOWER COMMAND=SET_PRESSURE_ADVANCE PARAMETER=OFFSET START=0 FACTOR=0.001") ==
        0);
    seen.frames.clear();
    REQUIRE(mock.gcode_script("PRINT_PA_TOWER NOZZLE=0.4 TARGET_TEMP=210") == 0);
    CHECK(mock.get_print_phase() == Phase::PRINTING);

    for (int i = 0; i < 1000 && mock.get_print_phase() == Phase::PRINTING; ++i) {
        mock.step_printing(1.0);
    }
    CHECK(mock.get_print_phase() == Phase::COMPLETE);

    std::vector<double> offsets;
    for (const auto& f : seen.frames) {
        if (f.contains("nonlinear_offset")) {
            offsets.push_back(f["nonlinear_offset"].get<double>());
        }
    }
    REQUIRE(offsets.size() > 10);
    CHECK(offsets.back() == Catch::Approx(0.05).margin(1e-6));
    for (size_t i = 1; i < offsets.size(); ++i) {
        CHECK(offsets[i] >= offsets[i - 1]);
    }
    CHECK(offsets.back() > offsets.front());
}

TEST_CASE("Mock PA tower: unknown without HELIX_MOCK_PA_TEST", "[pa_mock]") {
    MoonrakerClientMock mock(MoonrakerClientMock::PrinterType::VORON_24);
    CHECK(mock.gcode_script("PRINT_PA_TOWER NOZZLE=0.4 TARGET_TEMP=210") != 0);
    CHECK(mock.get_last_gcode_error() == "Unknown command:\"PRINT_PA_TOWER\"");
}
