// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_safety_limits_negative_min_temp.cpp
 * @brief #1353 - Klipper's negative min_temp / min_extrude_temp are clamped at 0.
 *
 * Klipper's `min_temp` is a sensor sanity floor. A printer with an unmounted or
 * open thermistor - the reported case was a six-tool toolchanger with no tools
 * fitted - is legitimately configured `min_temp: -100` so Klipper tolerates the
 * open reads. HelixScreen adopted that number verbatim into SafetyLimits, where
 * it means something else entirely, and it came out the other end as:
 *
 *   - "Heat to at least -100°C for filament operations" on the filament panel;
 *   - a -100°C lower bound in is_safe_temperature(), so negative heater targets
 *     passed validation and were sent to the printer.
 *
 * The fix clamps both floors at 0 where the value is adopted, on the type
 * itself (SafetyLimits::clamp_temperature_floors), so every reader inherits it.
 *
 * NOT in scope, deliberately: cold extrusion being permitted at all. A low
 * min_extrude_temp making is_extrusion_safe() permanently true is known and
 * accepted (see ui_panel_filament.cpp's StatusBranch comment, and the opt-in
 * bypass shipped by #978). These tests pin the clamp, nothing about the gate.
 *
 * The pin is split so a revert of EITHER half goes red: the helper tests cover
 * the invariant's body, the parse and set_safety_limits tests cover the two
 * call sites that must apply it.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../../src/api/moonraker_api_internal.h"
#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "printer_state.h"
#include "tool_state.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The reporter's configuration (bundle 6QWNVZY5): every extruder carries
/// min_temp: -100 so Klipper tolerates the open thermistor on an empty dock.
constexpr double REPORTED_MIN_TEMP = -100.0;

/// Raised above the 400.0 SafetyLimits default so the parse produces an
/// observable change on the ceiling too - the signal that the temperature loop
/// ran at all, rather than the floors reading 0 because nothing was parsed.
constexpr double PROBE_EXTRUDER_MAX_TEMP = 500.0;

/// Drives the real update_safety_limits_from_printer() against a mock whose
/// configfile.settings can carry a negative floor.
class NegativeMinTempFixture : public LVGLTestFixture {
  public:
    NegativeMinTempFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
        mock_client_.set_extruder_max_temp(PROBE_EXTRUDER_MAX_TEMP);
    }

    ~NegativeMinTempFixture() override {
        // Ordered teardown, same rule as test_safety_limits_null_endstop.cpp:
        // drain before each owner dies so no queued callback outlives the state
        // it captured, or the SIGSEGV lands in whatever test constructs a
        // fixture next.
        helix::ui::UpdateQueue::instance().drain();
        api_.reset();
        helix::ui::UpdateQueue::instance().drain();
        state_.deinit_subjects();
    }

    /// Run the config parse to completion. Returns true if a callback fired.
    bool run_update() {
        std::atomic<bool> done{false};
        api_->update_safety_limits_from_printer([&]() { done.store(true); },
                                                [&](const MoonrakerError&) { done.store(true); });
        for (int i = 0; i < 200 && !done.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return done.load();
    }

    MoonrakerClientMock mock_client_;
    PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
};

/// A real FilamentPanel built from real XML - the safety warning text is a
/// subject the panel writes, so nothing short of the panel proves what a user
/// would read off the screen.
struct SafetyTextHarness {
    explicit SafetyTextHarness(LVGLUITestFixture& f) : fx(f) {
        REQUIRE(fx.api() != nullptr);
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);
    }

    /// What the user actually reads under the load/unload buttons.
    std::string safety_text() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "filament_safety_warning_text");
        REQUIRE(s != nullptr);
        const char* v = lv_subject_get_string(s);
        return v ? v : "";
    }

    LVGLUITestFixture& fx;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;
};

} // namespace

// ============================================================================
// The invariant itself - pure, no LVGL, no network
// ============================================================================

TEST_CASE("SafetyLimits: negative temperature floors clamp to zero", "[1353][safety_limits]") {
    SafetyLimits limits;
    limits.min_temperature_celsius = REPORTED_MIN_TEMP;
    limits.min_extrude_temp_celsius = REPORTED_MIN_TEMP;

    limits.clamp_temperature_floors();

    CHECK(limits.min_temperature_celsius == Catch::Approx(0.0));
    CHECK(limits.min_extrude_temp_celsius == Catch::Approx(0.0));
}

TEST_CASE("SafetyLimits: a configured positive floor survives the clamp", "[1353][safety_limits]") {
    // The clamp must be a floor, not a reset - a printer that really does
    // require 190°C before it will extrude keeps that number.
    SafetyLimits limits;
    limits.min_temperature_celsius = 5.0;
    limits.min_extrude_temp_celsius = 190.0;
    limits.max_temperature_celsius = PROBE_EXTRUDER_MAX_TEMP;

    limits.clamp_temperature_floors();

    CHECK(limits.min_temperature_celsius == Catch::Approx(5.0));
    CHECK(limits.min_extrude_temp_celsius == Catch::Approx(190.0));
    // The ceiling is a permissive sanity net and is deliberately untouched.
    CHECK(limits.max_temperature_celsius == Catch::Approx(PROBE_EXTRUDER_MAX_TEMP));
}

TEST_CASE("SafetyLimits: clamping is idempotent", "[1353][safety_limits]") {
    // update_safety_limits_from_printer() re-runs on every discovery completion,
    // including reconnects, so the clamp is applied to already-clamped values.
    SafetyLimits limits;
    limits.min_temperature_celsius = REPORTED_MIN_TEMP;
    limits.min_extrude_temp_celsius = REPORTED_MIN_TEMP;

    limits.clamp_temperature_floors();
    const SafetyLimits once = limits;
    limits.clamp_temperature_floors();

    CHECK(limits.min_temperature_celsius == Catch::Approx(once.min_temperature_celsius));
    CHECK(limits.min_extrude_temp_celsius == Catch::Approx(once.min_extrude_temp_celsius));
}

// ============================================================================
// Call site 1: the Klipper config parse
// ============================================================================

TEST_CASE_METHOD(NegativeMinTempFixture,
                 "Klipper's negative min_temp is not adopted into SafetyLimits",
                 "[1353][safety_limits]") {
    mock_client_.set_extruder_min_temp(REPORTED_MIN_TEMP);
    mock_client_.set_extruder_min_extrude_temp(REPORTED_MIN_TEMP);

    REQUIRE(run_update());

    const SafetyLimits& limits = api_->get_safety_limits();

    // Proves the temperature loop ran at all - without this the floors could
    // read 0 simply because nothing was parsed.
    REQUIRE(limits.max_temperature_celsius == Catch::Approx(PROBE_EXTRUDER_MAX_TEMP));

    // The regression. Before the fix these were -100.
    CHECK(limits.min_temperature_celsius == Catch::Approx(0.0));
    CHECK(limits.min_extrude_temp_celsius == Catch::Approx(0.0));
}

TEST_CASE_METHOD(NegativeMinTempFixture,
                 "A negative configured min_temp leaves negative targets unsendable",
                 "[1353][safety_limits]") {
    mock_client_.set_extruder_min_temp(REPORTED_MIN_TEMP);
    mock_client_.set_extruder_min_extrude_temp(REPORTED_MIN_TEMP);
    REQUIRE(run_update());

    const SafetyLimits& limits = api_->get_safety_limits();

    // is_safe_temperature() is the gate set_temperature() runs before it emits
    // any G-code. With the raw -100 floor adopted, every one of these passed.
    CHECK(moonraker_internal::is_safe_temperature(-100.0, limits) == false);
    CHECK(moonraker_internal::is_safe_temperature(-50.0, limits) == false);
    CHECK(moonraker_internal::is_safe_temperature(-0.5, limits) == false);
    // ...and a real target still gets through.
    CHECK(moonraker_internal::is_safe_temperature(215.0, limits) == true);
}

TEST_CASE_METHOD(NegativeMinTempFixture, "set_temperature rejects a negative target",
                 "[1353][safety_limits]") {
    mock_client_.set_extruder_min_temp(REPORTED_MIN_TEMP);
    mock_client_.set_extruder_min_extrude_temp(REPORTED_MIN_TEMP);
    REQUIRE(run_update());

    // The wiring, not just the predicate: validation is synchronous and runs
    // ahead of any G-code generation, so the error callback fires inline.
    bool rejected = false;
    api_->set_temperature(
        "extruder", -50.0, []() {}, [&](const MoonrakerError&) { rejected = true; });
    CHECK(rejected);

    // Control: the same path accepts a sane target, so the assertion above is
    // measuring the floor and not a broken harness.
    bool sane_rejected = false;
    api_->set_temperature(
        "extruder", 215.0, []() {}, [&](const MoonrakerError&) { sane_rejected = true; });
    CHECK_FALSE(sane_rejected);

    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(NegativeMinTempFixture, "A positive configured min_extrude_temp is preserved",
                 "[1353][safety_limits]") {
    // The clamp must not flatten a printer that genuinely wants a high floor.
    mock_client_.set_extruder_min_temp(0.0);
    mock_client_.set_extruder_min_extrude_temp(190.0);

    REQUIRE(run_update());

    CHECK(api_->get_safety_limits().min_extrude_temp_celsius == Catch::Approx(190.0));
}

// ============================================================================
// Call site 2: explicit limits - and what the user ends up reading
// ============================================================================

TEST_CASE_METHOD(LVGLUITestFixture,
                 "FilamentPanel never asks the user to heat to a negative temperature",
                 "[1353][filament][panel]") {
    SafetyTextHarness h(*this);

    // Push the reporter's numbers through the API the way discovery does, then
    // hand the panel the ints application.cpp casts them to.
    SafetyLimits limits;
    limits.min_temperature_celsius = REPORTED_MIN_TEMP;
    limits.min_extrude_temp_celsius = REPORTED_MIN_TEMP;
    api()->set_safety_limits(limits);

    const SafetyLimits& adopted = api()->get_safety_limits();
    h.panel->set_limits(static_cast<int>(adopted.min_temperature_celsius),
                        static_cast<int>(adopted.max_temperature_celsius),
                        static_cast<int>(adopted.min_extrude_temp_celsius));
    process_lvgl(10);

    const std::string text = h.safety_text();
    REQUIRE_FALSE(text.empty());

    // Before the fix this read "Heat to at least -100°C for filament operations".
    // Assert on the sign rather than the whole sentence so a reworded or
    // translated string does not fail this test for the wrong reason.
    CHECK(text.find("-100") == std::string::npos);
    CHECK(text.find('-') == std::string::npos);
    CHECK(text.find("0°C") != std::string::npos);
}
