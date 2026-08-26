// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_safety_limits_per_heater.cpp
 * @brief #1355 - a heater's own max_temp bounds it, not one global 400C net.
 *
 * SafetyLimits::max_temperature_celsius defaults to 400 and the config parse
 * only ever RAISED it:
 *
 *   if (max_temp > safety_limits_.max_temperature_celsius) { ... }
 *
 * A printer declaring `max_temp: 290` is strictly lower than the default, so it
 * was discarded and 400 survived. The keypad offered 350C on a 290C machine and
 * the user got a Klipper rejection instead of a bound.
 *
 * The obvious narrow fix - "adopt the highest observed max_temp unconditionally"
 * - swaps one bug for a worse one. The bound is GLOBAL, shared by nozzle, bed
 * and chamber, so a configfile.settings response exposing `heater_bed` but not
 * `extruder` would seed a 120C ceiling and then reject every nozzle target.
 * Today's widen-only code cannot do that, which is why it was written that way.
 *
 * So the ceilings go per-heater, keyed by Klipper object name, and the global
 * stays a permissive sanity net that only widens. A heater the config never
 * described falls back to that net rather than to another heater's number,
 * which is the property the bed-only cases below pin.
 *
 * Sibling of test_safety_limits_negative_min_temp.cpp (#1353): same parse, other
 * end of the range. That one clamps a floor adopted verbatim; this one stops a
 * compiled-in default outranking the config.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../../src/api/moonraker_api_internal.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "../test_helpers/temperature_controller_test_access.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "moonraker_types.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "temperature_controller.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using moonraker_internal::is_safe_temperature;

namespace {

/// A 290C hotend and a 120C bed - the shape of nearly every real printer, and
/// both below the 400C default that used to win.
SafetyLimits typical_printer() {
    SafetyLimits limits;
    limits.heater_max_temp_celsius["extruder"] = 290.0;
    limits.heater_max_temp_celsius["heater_bed"] = 120.0;
    return limits;
}

} // namespace

TEST_CASE("max_temp_for returns the heater's own ceiling", "[safety_limits][1355]") {
    const SafetyLimits limits = typical_printer();
    REQUIRE(limits.max_temp_for("extruder") == 290.0);
    REQUIRE(limits.max_temp_for("heater_bed") == 120.0);
}

TEST_CASE("A ceiling BELOW the compiled default is adopted, not discarded",
          "[safety_limits][1355]") {
    const SafetyLimits limits = typical_printer();
    // The whole bug: 290 < 400, so widen-only threw it away and left 400.
    REQUIRE(limits.max_temperature_celsius == 400.0);
    REQUIRE(limits.max_temp_for("extruder") < limits.max_temperature_celsius);
}

TEST_CASE("A heater the config never described falls back to the global net",
          "[safety_limits][1355]") {
    const SafetyLimits limits = typical_printer();
    // No chamber section was seen. The sanity net answers, and nothing pretends
    // to know a ceiling the printer never stated.
    REQUIRE(limits.max_temp_for("chamber") == 400.0);
    REQUIRE(limits.max_temp_for("extruder1") == 400.0);
}

TEST_CASE("A bed-only config does not poison the nozzle's ceiling", "[safety_limits][1355]") {
    // The failure mode a single global bound would reintroduce: settings
    // exposes heater_bed and not extruder, so "highest observed wins" would
    // make 120C the ceiling for everything and reject every nozzle target.
    SafetyLimits limits;
    limits.heater_max_temp_celsius["heater_bed"] = 120.0;

    REQUIRE(limits.max_temp_for("heater_bed") == 120.0);
    REQUIRE(limits.max_temp_for("extruder") == 400.0);
    REQUIRE(is_safe_temperature(250.0, limits, "extruder"));
    REQUIRE_FALSE(is_safe_temperature(250.0, limits, "heater_bed"));
}

TEST_CASE("Multi-extruder heaters carry independent ceilings", "[safety_limits][1355]") {
    SafetyLimits limits;
    limits.heater_max_temp_celsius["extruder"] = 300.0;
    limits.heater_max_temp_celsius["extruder1"] = 500.0; // a high-temp tool
    limits.heater_max_temp_celsius["heater_bed"] = 120.0;

    REQUIRE(limits.max_temp_for("extruder") == 300.0);
    REQUIRE(limits.max_temp_for("extruder1") == 500.0);
    // 450 belongs to the high-temp tool alone.
    REQUIRE(is_safe_temperature(450.0, limits, "extruder1"));
    REQUIRE_FALSE(is_safe_temperature(450.0, limits, "extruder"));
}

TEST_CASE("An empty map leaves every heater on the compiled default", "[safety_limits][1355]") {
    // Before update_safety_limits_from_printer() has run, or on a printer whose
    // configfile.settings never arrived. Must behave exactly as it did before
    // this change - no new rejections on a printer we know nothing about.
    const SafetyLimits limits;
    REQUIRE(limits.max_temp_for("extruder") == 400.0);
    REQUIRE(limits.max_temp_for("heater_bed") == 400.0);
    REQUIRE(is_safe_temperature(399.0, limits, "extruder"));
}

TEST_CASE("is_safe_temperature bounds a target by the heater it names", "[safety_limits][1355]") {
    const SafetyLimits limits = typical_printer();

    SECTION("a target the hotend can reach passes") {
        REQUIRE(is_safe_temperature(290.0, limits, "extruder")); // inclusive
        REQUIRE(is_safe_temperature(250.0, limits, "extruder"));
    }
    SECTION("a target above the hotend's own max is refused") {
        // The reported symptom: 350 was accepted on a 290C machine because the
        // global 400 net was doing the validating.
        REQUIRE_FALSE(is_safe_temperature(350.0, limits, "extruder"));
        REQUIRE(is_safe_temperature(350.0, limits)); // ...as the old 2-arg form still does
    }
    SECTION("the floor still applies") {
        REQUIRE_FALSE(is_safe_temperature(-1.0, limits, "extruder"));
    }
}

TEST_CASE("A generic chamber heater is keyed by the name the send carries",
          "[safety_limits][1355]") {
    // TemperatureController::resolved_name(Chamber) returns the whole section
    // header, so that is what reaches set_temperature() and what the map has to
    // be keyed on. Deriving a shorter object name from it - splitting
    // "heater_generic chamber_heater" down to "chamber_heater" - would key a
    // ceiling nothing ever looks up.
    SafetyLimits limits;
    limits.heater_max_temp_celsius["heater_generic chamber_heater"] = 60.0;

    REQUIRE(limits.max_temp_for("heater_generic chamber_heater") == 60.0);
    REQUIRE_FALSE(is_safe_temperature(80.0, limits, "heater_generic chamber_heater"));
    REQUIRE(is_safe_temperature(55.0, limits, "heater_generic chamber_heater"));
}

TEST_CASE("Heater lookup is case-insensitive", "[safety_limits][1355]") {
    // Moonraker lower-cases section headers; a discovery name need not be
    // lower-case. ensure_limits() already lower-cases defensively for the same
    // reason, so the ceiling lookup must not be the one place that does not.
    SafetyLimits limits;
    limits.heater_max_temp_celsius["heater_generic chamber_heater"] = 60.0;

    REQUIRE(limits.max_temp_for("Heater_Generic Chamber_Heater") == 60.0);
    REQUIRE(limits.max_temp_for("HEATER_BED") == 400.0); // absent, so the net answers
}

// ---------------------------------------------------------------------------
// The other half of #1355: the keypad's own ceiling.
//
// FilamentPanel handed its three keypads compiled-in members - nozzle 500
// (overwritten by the global 400 net), bed 150, chamber 150 - so every bed and
// chamber keypad in the app stopped at 150 regardless of what the printer said,
// and the nozzle offered 400 on a 290C machine. ControlsPanel had already been
// asking TemperatureController the same question for its three keypads, so this
// is the panel joining it rather than a second answer.
// ---------------------------------------------------------------------------

namespace {

/// A FilamentPanel with a real TemperatureController installed where
/// get_temperature_controller() looks for it.
///
/// FilamentPanel is in the GLOBAL namespace - ui_panel_filament.h closes
/// `helix::ui` before declaring it - while its test-access struct is in
/// helix::ui. Same as test_safety_limits_negative_min_temp.cpp, which reaches
/// it as a bare name under `using namespace helix`.
struct KeypadCeilingFixture {
    MoonrakerClientMock client;
    helix::PrinterState state;
    MoonrakerAPI api;
    std::shared_ptr<helix::TemperatureController> controller;
    ::FilamentPanel panel;

    KeypadCeilingFixture()
        : client(MoonrakerClientMock::PrinterType::VORON_24), api(client, state),
          controller(std::make_shared<helix::TemperatureController>(state, &api)),
          panel(state, &api) {
        state.init_subjects(false);
        helix::PanelWidgetManager::instance().register_shared_resource(controller);
    }

    ~KeypadCeilingFixture() {
        helix::PanelWidgetManager::instance().clear_shared_resources();
        state.deinit_subjects();
    }

    float ceiling(helix::HeaterType type, int fallback) {
        return helix::ui::FilamentPanelTestAccess::keypad_max_for(panel, type, fallback);
    }
};

} // namespace

TEST_CASE("A configured heater max beats the panel's compiled-in fallback",
          "[safety_limits][1355]") {
    KeypadCeilingFixture f;
    using helix::HeaterType;
    using helix::TemperatureControllerTestAccess;

    TemperatureControllerTestAccess::set_max(*f.controller, HeaterType::Bed, 120);
    TemperatureControllerTestAccess::set_max(*f.controller, HeaterType::Chamber, 60);

    // 150 is the member the panel used to hand the keypad unconditionally.
    REQUIRE(f.ceiling(HeaterType::Bed, 150) == 120.0f);
    REQUIRE(f.ceiling(HeaterType::Chamber, 150) == 60.0f);
}

TEST_CASE("An unknown heater max leaves the keypad on the heater default",
          "[safety_limits][1355]") {
    KeypadCeilingFixture f;
    using helix::HeaterType;

    // Nothing fetched, so keypad_range() reports the heater's own default (350
    // for the nozzle) rather than the panel's 500 member or a zero.
    const float nozzle = f.ceiling(HeaterType::Nozzle, 500);
    REQUIRE(nozzle > 0.0f);
    REQUIRE(nozzle == 350.0f);
}

// ---------------------------------------------------------------------------
// The parse itself.
//
// Everything above pins the TYPE. None of it would notice
// update_safety_limits_from_printer() reverting to the widen-only form that
// caused #1355 - max_temp_for() would keep answering correctly about a map
// nothing had filled in. These drive the real parse against the mock's real
// configfile.settings, so a revert of the adoption half goes red on its own.
//
// Both sections the mock reports sit BELOW the 400C default (heater_bed is a
// hardcoded 120), which is the property that makes them a pin: widen-only
// discards a number lower than what it already holds.
// ---------------------------------------------------------------------------

namespace {

constexpr double HOTEND_MAX_TEMP = 290.0;   ///< The reported machine: below the 400 default.
constexpr double MOCK_BED_MAX_TEMP = 120.0; ///< What the mock hardcodes for heater_bed.

class ParseFixture : public LVGLTestFixture {
  public:
    ParseFixture() : mock_client_(MoonrakerClientMock::PrinterType::VORON_24) {
        state_.init_subjects(false);
        api_ = std::make_unique<MoonrakerAPI>(mock_client_, state_);
        mock_client_.set_extruder_max_temp(HOTEND_MAX_TEMP);
    }

    ~ParseFixture() override {
        // Ordered teardown, same rule as test_safety_limits_negative_min_temp.cpp:
        // drain before each owner dies so no queued callback outlives the state it
        // captured.
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
    helix::PrinterState state_;
    std::unique_ptr<MoonrakerAPI> api_;
};

} // namespace

TEST_CASE("The config parse adopts a hotend ceiling below the compiled default",
          "[safety_limits][1355]") {
    ParseFixture f;
    REQUIRE(f.run_update());

    const SafetyLimits& limits = f.api_->get_safety_limits();

    // The whole bug in one assertion: 290 < 400, and widen-only threw it away.
    REQUIRE(limits.max_temp_for("extruder") == HOTEND_MAX_TEMP);
    REQUIRE(limits.max_temp_for("heater_bed") == MOCK_BED_MAX_TEMP);
}

TEST_CASE("The parse leaves the global ceiling permissive", "[safety_limits][1355]") {
    ParseFixture f;
    REQUIRE(f.run_update());

    const SafetyLimits& limits = f.api_->get_safety_limits();

    // A 120C bed must never become the bound for callers with no heater name to
    // hand. That regression is what widen-only was written to prevent, and it
    // still has to hold now that the per-heater map does the real work.
    REQUIRE(limits.max_temperature_celsius >= 400.0);
    REQUIRE(limits.max_temp_for("chamber") >= 400.0);
}

TEST_CASE("A parsed hotend ceiling bounds what set_temperature will send",
          "[safety_limits][1355]") {
    ParseFixture f;
    REQUIRE(f.run_update());

    const SafetyLimits& limits = f.api_->get_safety_limits();

    // The reported symptom, end to end: 350 was offered and accepted on a 290C
    // machine because the global net did the validating.
    REQUIRE_FALSE(is_safe_temperature(350.0, limits, "extruder"));
    REQUIRE(is_safe_temperature(280.0, limits, "extruder"));
    // ...while the bed keeps its own, much lower, bound.
    REQUIRE_FALSE(is_safe_temperature(150.0, limits, "heater_bed"));
    REQUIRE(is_safe_temperature(100.0, limits, "heater_bed"));
}
