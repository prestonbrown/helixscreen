// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/ui/wizard_step_registry.h"
#include "../helix_test_fixture.h"
#include "static_panel_registry.h"
#include "wizard_step.h"
#include "wizard_step_logic.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::wizard::StepId;

// ----------------------------------------------------------------------------
// Id-based skip-vector helpers. The wizard navigates a StepId registry; these
// build the full 13-entry vector and flip named steps off.
// ----------------------------------------------------------------------------
static std::vector<helix::StepSkip> full_vec() {
    std::vector<helix::StepSkip> v;
    for (int i = 0; i < helix::wizard::STEP_COUNT; ++i)
        v.push_back({static_cast<StepId>(i), false});
    return v;
}

static void skip_step(std::vector<helix::StepSkip>& v, StepId s) {
    for (auto& e : v)
        if (e.id == s)
            e.skipped = true;
}

// ============================================================================
// Default vector (no skips) — baseline behavior
// ============================================================================

TEST_CASE("Default: all 13 steps shown", "[wizard][step_logic]") {
    REQUIRE(helix::wizard_visible_count(full_vec()) == 13);
}

TEST_CASE("Default: display step numbering is 1-based sequential", "[wizard][step_logic]") {
    auto v = full_vec();
    for (int i = 0; i < 13; ++i) {
        REQUIRE(helix::wizard_display_number(static_cast<StepId>(i), v) == i + 1);
    }
}

TEST_CASE("Default: next walks all steps", "[wizard][step_logic]") {
    auto v = full_vec();
    for (int i = 0; i < 12; ++i) {
        REQUIRE(helix::wizard_next(static_cast<StepId>(i), v) == static_cast<StepId>(i + 1));
    }
    REQUIRE(helix::wizard_is_last(StepId::Telemetry, v));
}

TEST_CASE("Default: prev walks all steps backward", "[wizard][step_logic]") {
    auto v = full_vec();
    for (int i = 12; i > 0; --i) {
        REQUIRE(helix::wizard_prev(static_cast<StepId>(i), v) == static_cast<StepId>(i - 1));
    }
    REQUIRE_FALSE(helix::wizard_prev(StepId::TouchCalibration, v).has_value());
}

// ============================================================================
// Preset mode: skip hardware steps
// ============================================================================

TEST_CASE("Preset mode: skip hardware steps", "[wizard][step_logic][preset]") {
    auto v = full_vec();
    skip_step(v, StepId::Wifi);
    skip_step(v, StepId::PrinterIdentify);
    skip_step(v, StepId::HeaterSelect);
    skip_step(v, StepId::FanSelect);
    skip_step(v, StepId::AmsIdentify);
    skip_step(v, StepId::LedSelect);
    skip_step(v, StepId::FilamentSensor);
    skip_step(v, StepId::InputShaper);
    skip_step(v, StepId::Summary);
    // telemetry NOT skipped (shown in preset mode)

    // Steps shown: TouchCalibration, Language, Connection, Telemetry = 4
    REQUIRE(helix::wizard_visible_count(v) == 4);
    REQUIRE(helix::wizard_display_number(StepId::TouchCalibration, v) == 1);
    REQUIRE(helix::wizard_display_number(StepId::Language, v) == 2);
    REQUIRE(helix::wizard_display_number(StepId::Connection, v) == 3);
    REQUIRE(helix::wizard_display_number(StepId::Telemetry, v) == 4);
}

TEST_CASE("Preset mode: next skips hardware", "[wizard][step_logic][preset]") {
    auto v = full_vec();
    skip_step(v, StepId::Wifi);
    skip_step(v, StepId::PrinterIdentify);
    skip_step(v, StepId::HeaterSelect);
    skip_step(v, StepId::FanSelect);
    skip_step(v, StepId::AmsIdentify);
    skip_step(v, StepId::LedSelect);
    skip_step(v, StepId::FilamentSensor);
    skip_step(v, StepId::InputShaper);
    skip_step(v, StepId::Summary);

    REQUIRE(helix::wizard_next(StepId::TouchCalibration, v) == StepId::Language);
    REQUIRE(helix::wizard_next(StepId::Language, v) == StepId::Connection);
    REQUIRE(helix::wizard_next(StepId::Connection, v) == StepId::Telemetry);
    REQUIRE(helix::wizard_is_last(StepId::Telemetry, v));
}

TEST_CASE("Normal mode: telemetry skipped by default", "[wizard][step_logic]") {
    auto v = full_vec();
    skip_step(v, StepId::Telemetry);

    // 12 steps shown (telemetry skipped)
    REQUIRE(helix::wizard_visible_count(v) == 12);
    REQUIRE(helix::wizard_is_last(StepId::Summary, v)); // Summary is last
}

TEST_CASE("Preset mode: prev works", "[wizard][step_logic][preset]") {
    auto v = full_vec();
    skip_step(v, StepId::Wifi);
    skip_step(v, StepId::PrinterIdentify);
    skip_step(v, StepId::HeaterSelect);
    skip_step(v, StepId::FanSelect);
    skip_step(v, StepId::AmsIdentify);
    skip_step(v, StepId::LedSelect);
    skip_step(v, StepId::FilamentSensor);
    skip_step(v, StepId::InputShaper);
    skip_step(v, StepId::Summary);

    REQUIRE(helix::wizard_prev(StepId::Telemetry, v) == StepId::Connection);
    REQUIRE(helix::wizard_prev(StepId::Connection, v) == StepId::Language);
    REQUIRE(helix::wizard_prev(StepId::Language, v) == StepId::TouchCalibration);
    REQUIRE_FALSE(helix::wizard_prev(StepId::TouchCalibration, v).has_value());
}

TEST_CASE("Preset mode: connection also skipped", "[wizard][step_logic][preset]") {
    auto v = full_vec();
    skip_step(v, StepId::Wifi);
    skip_step(v, StepId::Connection); // auto-validated
    skip_step(v, StepId::PrinterIdentify);
    skip_step(v, StepId::HeaterSelect);
    skip_step(v, StepId::FanSelect);
    skip_step(v, StepId::AmsIdentify);
    skip_step(v, StepId::LedSelect);
    skip_step(v, StepId::FilamentSensor);
    skip_step(v, StepId::InputShaper);
    skip_step(v, StepId::Summary);
    // telemetry NOT skipped

    // Steps: TouchCalibration, Language, Telemetry = 3
    REQUIRE(helix::wizard_visible_count(v) == 3);
    // lang -> telemetry (skip conn + all hw)
    REQUIRE(helix::wizard_next(StepId::Language, v) == StepId::Telemetry);
}

// ============================================================================
// Preset skip policy (wizard_preset_plan) — single source of truth for when a
// preset lets the wizard skip hardware steps, decoupled from the first-run
// telemetry fast path.
// ============================================================================

TEST_CASE("Preset plan: no preset means no preset-driven skips", "[wizard][step_logic][preset]") {
    auto plan = helix::wizard_preset_plan(/*has_preset=*/false, /*printer_count=*/1);
    REQUIRE_FALSE(plan.skip_hardware);
    REQUIRE_FALSE(plan.first_run);

    // No preset, even on a multi-printer config, still skips nothing preset-driven.
    auto plan2 = helix::wizard_preset_plan(false, 3);
    REQUIRE_FALSE(plan2.skip_hardware);
    REQUIRE_FALSE(plan2.first_run);
}

TEST_CASE("Preset plan: first printer with preset is the first-run fast path",
          "[wizard][step_logic][preset]") {
    auto plan = helix::wizard_preset_plan(/*has_preset=*/true, /*printer_count=*/1);
    REQUIRE(plan.skip_hardware);
    REQUIRE(plan.first_run); // skips summary + shows telemetry opt-in

    // A zero count (config not yet listing the active printer) is treated as
    // first-run, not as "many printers".
    auto plan0 = helix::wizard_preset_plan(true, 0);
    REQUIRE(plan0.skip_hardware);
    REQUIRE(plan0.first_run);
}

TEST_CASE("Preset plan: SECOND printer with preset skips hardware but NOT first-run",
          "[wizard][step_logic][preset][regression]") {
    // Regression: a known printer added as the 2nd+ printer (e.g. a Creality K2
    // Plus next to an existing Voron) was force-marched through manual heater/fan/
    // sensor mapping even though its preset fully configured the hardware. The
    // hardware pickers must be skipped for any printer with a preset; only the
    // one-time telemetry fast path stays gated to the first printer.
    auto plan = helix::wizard_preset_plan(/*has_preset=*/true, /*printer_count=*/2);
    REQUIRE(plan.skip_hardware);   // <-- the fix: redundant hardware steps are skipped
    REQUIRE_FALSE(plan.first_run); // <-- telemetry must NOT re-fire; summary stays shown

    auto plan5 = helix::wizard_preset_plan(true, 5);
    REQUIRE(plan5.skip_hardware);
    REQUIRE_FALSE(plan5.first_run);
}

TEST_CASE("Preset plan: secondary-printer flags navigate connection -> summary -> done",
          "[wizard][step_logic][preset]") {
    // Build the skip vector a secondary preset printer produces and confirm the
    // wizard walks straight to the summary, then finishes (no telemetry).
    auto plan = helix::wizard_preset_plan(true, 2);
    REQUIRE(plan.skip_hardware);
    REQUIRE_FALSE(plan.first_run);

    auto v = full_vec();
    // first three steps are skipped for any subsequent printer
    skip_step(v, StepId::TouchCalibration);
    skip_step(v, StepId::Language);
    skip_step(v, StepId::Wifi);
    // preset covers hardware (PrinterIdentify..InputShaper)
    if (plan.skip_hardware) {
        skip_step(v, StepId::PrinterIdentify);
        skip_step(v, StepId::HeaterSelect);
        skip_step(v, StepId::FanSelect);
        skip_step(v, StepId::AmsIdentify);
        skip_step(v, StepId::LedSelect);
        skip_step(v, StepId::FilamentSensor);
        skip_step(v, StepId::InputShaper);
    }
    // not first-run: summary shown, telemetry skipped
    if (plan.first_run)
        skip_step(v, StepId::Summary);
    else
        skip_step(v, StepId::Telemetry);

    REQUIRE(helix::wizard_next(StepId::Connection, v) == StepId::Summary); // connection -> summary
    REQUIRE(helix::wizard_is_last(StepId::Summary, v)); // summary -> done (telemetry skipped)
    REQUIRE(helix::wizard_visible_count(v) == 2);       // connection + summary
}

// ============================================================================
// Id-based pure navigation over the step registry (StepId + StepSkip vector).
// ============================================================================

static std::vector<helix::StepSkip> all_visible() {
    std::vector<helix::StepSkip> v;
    for (int i = 0; i < helix::wizard::STEP_COUNT; ++i)
        v.push_back({static_cast<StepId>(i), false});
    return v;
}

TEST_CASE("id-nav: next walks visible steps", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    REQUIRE(helix::wizard_next(StepId::Connection, v) == StepId::PrinterIdentify);
    REQUIRE(helix::wizard_visible_count(v) == 13);
}

TEST_CASE("id-nav: non-contiguous skips are honored", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    for (auto& s : v)
        if (s.id == StepId::HeaterSelect || s.id == StepId::AmsIdentify ||
            s.id == StepId::InputShaper)
            s.skipped = true;
    REQUIRE(helix::wizard_next(StepId::PrinterIdentify, v) == StepId::FanSelect);
    REQUIRE(helix::wizard_next(StepId::FanSelect, v) == StepId::LedSelect);
    REQUIRE(helix::wizard_next(StepId::FilamentSensor, v) == StepId::Summary);
    REQUIRE(helix::wizard_visible_count(v) == 10);
}

TEST_CASE("id-nav: last visible step reports done", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    for (auto& s : v)
        if (s.id == StepId::Telemetry)
            s.skipped = true;
    REQUIRE(helix::wizard_is_last(StepId::Summary, v));
    REQUIRE_FALSE(helix::wizard_next(StepId::Summary, v).has_value());
}

TEST_CASE("id-nav: display number counts visible predecessors", "[wizard][step_logic][idnav]") {
    auto v = all_visible();
    for (auto& s : v)
        if (s.id == StepId::TouchCalibration || s.id == StepId::Language)
            s.skipped = true;
    REQUIRE(helix::wizard_display_number(StepId::Wifi, v) == 1);
    REQUIRE(helix::wizard_display_number(StepId::Connection, v) == 2);
}

// ============================================================================
// Registry preset-skip regression tests (Task 20). These drive the live step
// singletons through step_by_id()->should_skip(ctx) and assert that the PRESET
// branch dominates: when preset.skip_hardware is set, the hardware steps skip
// regardless of any live hardware state they would otherwise consult.
//
// Context is built directly (NOT via build_context(), which needs a live app):
// only the fields the preset policy reads are populated.
// ============================================================================

static helix::wizard::StepContext preset_ctx(bool has_preset, int printers) {
    helix::wizard::StepContext c;
    c.preset = helix::wizard_preset_plan(has_preset, printers);
    c.is_subsequent_printer = printers > 1;
    return c;
}

TEST_CASE("registry: subsequent preset printer skips hardware, shows summary, no telemetry",
          "[wizard][step_logic][regression]") {
    auto ctx = preset_ctx(/*has_preset=*/true, /*printers=*/2);
    auto skip = [&](StepId id) { return helix::wizard::step_by_id(id)->should_skip(ctx); };
    REQUIRE(skip(StepId::PrinterIdentify));
    REQUIRE(skip(StepId::HeaterSelect));
    REQUIRE(skip(StepId::FanSelect));
    REQUIRE(skip(StepId::AmsIdentify));
    REQUIRE(skip(StepId::LedSelect));
    REQUIRE(skip(StepId::FilamentSensor));
    REQUIRE(skip(StepId::InputShaper));
    REQUIRE_FALSE(skip(StepId::Summary)); // shown for subsequent printer
    REQUIRE(skip(StepId::Telemetry));     // never re-prompt
}

TEST_CASE("registry: first-run preset printer skips summary, shows telemetry",
          "[wizard][step_logic][regression]") {
    auto ctx = preset_ctx(/*has_preset=*/true, /*printers=*/1);
    REQUIRE(helix::wizard::step_by_id(StepId::Summary)->should_skip(ctx));
    REQUIRE_FALSE(helix::wizard::step_by_id(StepId::Telemetry)->should_skip(ctx));
}

TEST_CASE_METHOD(HelixTestFixture,
                 "registry: step_by_id stays valid after panel teardown (3rd-printer UAF)",
                 "[wizard][step_logic][regression]") {
    // First wizard session: fetch a step and exercise its vtable.
    helix::wizard::Step* before = helix::wizard::step_by_id(StepId::HeaterSelect);
    REQUIRE(before != nullptr);
    REQUIRE(before->id() == StepId::HeaterSelect);

    // Simulate the wizard tearing down between printer adds: StaticPanelRegistry
    // frees the lazily-created step singletons. The old registry cached raw Step*
    // in a static vector, so the NEXT fetch returned dangling pointers and a
    // virtual call crashed (SIGSEGV adding a 3rd printer). The registry must now
    // hand back a freshly recreated, live step.
    StaticPanelRegistry::instance().destroy_all();

    helix::wizard::Step* after = helix::wizard::step_by_id(StepId::HeaterSelect);
    REQUIRE(after != nullptr);
    REQUIRE(after->id() == StepId::HeaterSelect); // virtual call on a live object
    REQUIRE(std::string(after->component_name()) == "wizard_heater_select");
}

// ============================================================================
// Preset marker authority (wizard_preset_is_authoritative)
//
// The top-level "preset" marker is written the moment printer-identify's
// cleanup() runs — which is on Back as well as Next. Because has_preset() also
// collapses printer-identify, every hardware picker AND the summary, an
// interrupted run (crash, power cut, user quits) used to come back with all of
// those steps gone, pinned to a pick the user never confirmed and with no
// in-app route to the hardware pages. These pin the three states apart.
// ============================================================================

TEST_CASE("Preset authority: no marker is never authoritative", "[wizard][step_logic][preset]") {
    for (bool provisional : {false, true}) {
        for (bool completed : {false, true}) {
            for (bool session : {false, true}) {
                REQUIRE_FALSE(helix::wizard_preset_is_authoritative(
                    /*preset_marker=*/false, provisional, completed, session));
            }
        }
    }
}

TEST_CASE("Preset authority: install-time seeded preset keeps the fast path",
          "[wizard][step_logic][preset]") {
    // Installer/factory image wrote the preset before the wizard ever ran, so
    // nothing marked it provisional. It must collapse the hardware steps on the
    // very first boot — that fast path is the whole point of shipping presets.
    REQUIRE(helix::wizard_preset_is_authoritative(/*preset_marker=*/true, /*provisional=*/false,
                                                  /*wizard_completed=*/false,
                                                  /*applied_this_session=*/false));
}

TEST_CASE("Preset authority: completed wizard settles a provisional marker",
          "[wizard][step_logic][preset]") {
    // wizard_completed flipping is the promotion. Adding a 2nd printer later,
    // in a fresh process, must still skip the redundant hardware pickers.
    REQUIRE(helix::wizard_preset_is_authoritative(/*preset_marker=*/true, /*provisional=*/true,
                                                  /*wizard_completed=*/true,
                                                  /*applied_this_session=*/false));
}

TEST_CASE("Preset authority: interrupted wizard does NOT collapse the remaining steps",
          "[wizard][step_logic][preset][regression]") {
    // The defect: identify wrote the preset, the wizard never finished, the box
    // rebooted. Fresh process => applied_this_session is false. Skipping here
    // silently ate PrinterIdentify, Heater, Fan, AMS, LED, Filament, InputShaper
    // and Summary with no way back.
    REQUIRE_FALSE(helix::wizard_preset_is_authoritative(/*preset_marker=*/true,
                                                        /*provisional=*/true,
                                                        /*wizard_completed=*/false,
                                                        /*applied_this_session=*/false));

    // ...and the steps really do come back.
    auto ctx = preset_ctx(/*has_preset=*/false, /*printers=*/1);
    REQUIRE_FALSE(helix::wizard::step_by_id(StepId::PrinterIdentify)->should_skip(ctx));
    REQUIRE_FALSE(helix::wizard::step_by_id(StepId::HeaterSelect)->should_skip(ctx));
    REQUIRE_FALSE(helix::wizard::step_by_id(StepId::FanSelect)->should_skip(ctx));
}

TEST_CASE("Preset authority: the live run still collapses steps after identify",
          "[wizard][step_logic][preset]") {
    // Same process as the identify cleanup that wrote the marker: the wizard's
    // post-identify redirect must keep working exactly as before.
    REQUIRE(helix::wizard_preset_is_authoritative(/*preset_marker=*/true, /*provisional=*/true,
                                                  /*wizard_completed=*/false,
                                                  /*applied_this_session=*/true));
}

TEST_CASE("Preset authority: session flag is off until identify applies a preset",
          "[wizard][step_logic][preset]") {
    helix::wizard_reset_preset_session_state();
    REQUIRE_FALSE(helix::wizard_preset_applied_this_session());

    helix::wizard_mark_preset_applied_this_session();
    REQUIRE(helix::wizard_preset_applied_this_session());

    // Leave the process-global as other tests expect to find it.
    helix::wizard_reset_preset_session_state();
    REQUIRE_FALSE(helix::wizard_preset_applied_this_session());
}
