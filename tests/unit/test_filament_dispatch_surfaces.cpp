// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_dispatch_surfaces.cpp
 * @brief The other two dispatch surfaces on the shared three-tier ladder.
 *
 * Run with: ./build/bin/helix-tests "[filament][dispatch][wiring]"
 *
 * test_filament_op_dispatch.cpp pins the decision and
 * test_filament_panel_dispatch_wiring.cpp pins what FilamentPanel does with it.
 * This file pins the two surfaces that never had the ladder at all:
 *
 *   - AmsOperationSidebar used to `return` silently when there was no AMS
 *     backend. It now falls through to the user's LOAD_FILAMENT macro and then
 *     to raw gcode, like every other surface.
 *   - FilamentRunoutHandler used to navigate the user to the Filament panel for
 *     a load — out from under the live runout dialog. It now dispatches in
 *     place, and never raises a parameter modal on top of that dialog.
 *
 * The third thing pinned here is the lifetime hazard the sidebar's new tier 2
 * introduces. MacroParamModal stores its on_execute_ callback and does NOT clear
 * it on dismiss — only the next show_for_*() overwrites it — while the sidebar
 * is destroyed whenever the AMS panel closes. "Open the param modal, close the
 * AMS panel, press Run" therefore reaches a freed sidebar unless the callback is
 * guarded. The guard is observable: token.defer() counts every skipped callback
 * by tag through helix::async_lifetime, so the test can assert the skip
 * happened rather than hoping ASAN notices.
 */

#include "ui_ams_sidebar.h"
#include "ui_filament_runout_handler.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/filament_runout_handler_test_access.h"
#include "../test_helpers/lane_material_backend.h"
#include "../test_helpers/load_filament_expression_default.h"
#include "ams_state.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "filament_op_router.h"
#include "macro_executor.h"
#include "macro_param_cache.h"
#include "macro_param_defaults.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

#include "../catch_amalgamated.hpp"

using helix::CachedMacroInfo;
using helix::MacroExecuteCallback;
using helix::MacroParamResult;
using helix::ui::AmsOperationSidebar;
using helix::ui::FilamentMacroOp;
using helix::ui::FilamentRunoutHandler;
using helix::ui::FilamentRunoutHandlerTestAccess;
using helix::ui::ParamPolicy;
using ParamValues = std::map<std::string, std::string>;

namespace {

/// Gcode the tier-3 load fallback must contain. execute_gcode() annotates the
/// script, so tests match on a substring rather than the whole string.
constexpr const char* LOAD_FALLBACK_MARKER = "G1 E56";

class DispatchSurfaceFixture : public LVGLTestFixture {
  public:
    DispatchSurfaceFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        mock_client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);

        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());

        // No AMS backend by default — the shape both surfaces used to refuse.
        helix::AmsState::instance().clear_backends();
        // Empty cache => MacroParamKnowledge::UNKNOWN for every macro, i.e. the
        // branch that WOULD prompt. That is deliberate: it is the only way to
        // prove ParamPolicy::Suppress actually suppresses something.
        helix::MacroParamCache::instance().clear();

        // Record prompts instead of raising a real modal — the prompt branch has
        // to be reachable in a binary with no screen.
        helix::ui::set_filament_param_prompter(
            [this](const std::string& macro, const CachedMacroInfo&, const ParamValues& prefill,
                   MacroExecuteCallback on_execute) {
                ++prompt_count;
                prompted_macro = macro;
                prompted_prefill = prefill;
                pending_execute = std::move(on_execute);
            });

        // Start each test with an empty skip-counter window.
        helix::async_lifetime::take_snapshot();
    }

    ~DispatchSurfaceFixture() override {
        helix::ui::set_filament_param_prompter({});
        set_moonraker_api(previous_api_);
        StandardMacros::instance().reset();
        helix::MacroParamCache::instance().clear();
        helix::AmsState::instance().clear_backends();
        helix::ui::UpdateQueue::instance().drain();
        helix::async_lifetime::take_snapshot();
        mock_client.stop_temperature_simulation();
        mock_client.disconnect();
        api.reset();
    }

    /// Give StandardMacros a real LOAD_FILAMENT / UNLOAD_FILAMENT to resolve.
    void configure_filament_macros() {
        helix::PrinterDiscovery hardware;
        nlohmann::json objects = {"extruder", "gcode_macro LOAD_FILAMENT",
                                  "gcode_macro UNLOAD_FILAMENT"};
        hardware.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(hardware);
        REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
    }

    /// No macro configured anywhere — forces the tier-3 raw gcode fallback.
    ///
    /// reset() alone does not produce that state. It clears detected_macro only;
    /// configured_macro and fallback_macro survive it by design, and
    /// FALLBACK_MACROS seeds UnloadFilament with HELIX_UNLOAD_FILAMENT in the
    /// constructor. A fresh singleton therefore reaches the unload cases with the
    /// slot already non-empty, plan_unload() answers Macro instead of RawGcode,
    /// and the raw-gcode assertion fails. It only passed because an earlier test
    /// in the same process had run init() against hardware without that macro,
    /// which is what clears a fallback — so the case was pinned by execution
    /// order rather than by its own setup.
    ///
    /// init() against bare hardware empties all three sources: it restores the
    /// fallback from the table and then drops it (this printer defines no helix
    /// macros), and validate_configured() demotes anything load_from_config()
    /// picked up into missing_macro.
    void clear_filament_macros() {
        helix::PrinterDiscovery bare;
        nlohmann::json objects = {"extruder"};
        bare.parse_objects(objects);
        StandardMacros::instance().reset();
        StandardMacros::instance().init(bare);
        // Every slot these tests dispatch against, not just LoadFilament: its
        // table entry is "" so it is the one slot that cannot catch this.
        REQUIRE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
        REQUIRE(StandardMacros::instance().get(StandardMacroSlot::UnloadFilament).is_empty());
        REQUIRE(StandardMacros::instance().get(StandardMacroSlot::Purge).is_empty());
    }

    /// Seed MacroParamCache with gcode_macro bodies. populate_from_configfile()
    /// replaces the cache, so every macro a test needs goes in one call.
    static void cache_macros(std::initializer_list<std::pair<const char*, const char*>> macros) {
        nlohmann::json config;
        std::unordered_set<std::string> names;
        for (const auto& [name, gcode] : macros) {
            config[std::string("gcode_macro ") + name]["gcode"] = gcode;
            names.insert(name);
        }
        helix::MacroParamCache::instance().populate_from_configfile(config, names);
    }

    /// The extruder target Klipper reports, in degrees.
    void set_extruder_target(double degrees) {
        state.init_extruders({"extruder"});
        state.update_from_status({{"extruder", {{"target", degrees}}}});
    }

    [[nodiscard]] bool gcode_sent_containing(const std::string& needle) const {
        for (const auto& script : mock_client.gcode_script_history()) {
            if (script.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// Callbacks dropped by an expired LifetimeToken since the last drain,
    /// counting ONLY the sidebar's own macro dispatch tags.
    ///
    /// take_snapshot().total is process-global: every AsyncLifetimeGuard in the
    /// binary feeds it, including background workers still in flight from an
    /// earlier test (AmsBackendAd5xIfs's 500ms zcolor debounce is the known
    /// one). Under load those land inside this test's measurement window and
    /// the total reads 5 where the sidebar contributed 1. Filtering to the tag
    /// under test is what makes the assertion deterministic — and it is a
    /// stronger claim than the total was, because it also pins WHICH producer
    /// skipped rather than accepting any skip as proof.
    [[nodiscard]] static uint64_t drain_skip_total() {
        uint64_t n = 0;
        for (const auto& e : helix::async_lifetime::take_snapshot().entries) {
            if (e.tag == "AmsOperationSidebar::load_macro" ||
                e.tag == "AmsOperationSidebar::unload_macro") {
                n += e.count;
            }
        }
        return n;
    }

    MoonrakerClientMock mock_client;
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;

    int prompt_count = 0;
    std::string prompted_macro;
    ParamValues prompted_prefill;
    MacroExecuteCallback pending_execute;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

// =============================================================================
// The params policy — the constraint the runout dialog exists to enforce
// =============================================================================

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "ParamPolicy::Suppress runs a params-taking macro with no parameters",
                 "[filament][dispatch][wiring][params]") {
    bool ran = false;
    MacroParamResult seen;
    const bool prompted = helix::ui::dispatch_filament_macro("LOAD_FILAMENT", ParamPolicy::Suppress,
                                                             [&](const MacroParamResult& r) {
                                                                 ran = true;
                                                                 seen = r;
                                                             });

    CHECK_FALSE(prompted);
    CHECK(prompt_count == 0);
    CHECK(ran);
    CHECK(seen.params.empty());
    CHECK(seen.variables.empty());
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "ParamPolicy::Prompt defers the run until the prompt is answered",
                 "[filament][dispatch][wiring][params]") {
    // Guard rail on the test above: the same macro under the other policy MUST
    // prompt, or "Suppress suppresses" would be asserting nothing.
    bool ran = false;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt, [&](const MacroParamResult&) { ran = true; });

    CHECK(prompted);
    CHECK(prompt_count == 1);
    CHECK(prompted_macro == "LOAD_FILAMENT");
    CHECK_FALSE(ran); // waits for the user

    REQUIRE(pending_execute);
    pending_execute({});
    CHECK(ran);
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "A macro known to take no parameters never prompts under either policy",
                 "[filament][dispatch][wiring][params]") {
    nlohmann::json config;
    config["gcode_macro LOAD_FILAMENT"]["gcode"] = "G1 E50 F300";
    helix::MacroParamCache::instance().populate_from_configfile(config, {"LOAD_FILAMENT"});

    int runs = 0;
    CHECK_FALSE(helix::ui::dispatch_filament_macro("LOAD_FILAMENT", ParamPolicy::Prompt,
                                                   [&](const MacroParamResult&) { ++runs; }));
    CHECK(prompt_count == 0);
    CHECK(runs == 1);
}

// =============================================================================
// AmsOperationSidebar — the surface that used to return silently
// =============================================================================

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load with no backend reaches the configured macro",
                 "[filament][dispatch][wiring][ams]") {
    // Before the router this was a bare `if (!backend) return;` — the button
    // did nothing at all on a printer with no AMS.
    configure_filament_macros();
    REQUIRE(helix::AmsState::instance().get_backend() == nullptr);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);

    CHECK(prompt_count == 1);
    CHECK(prompted_macro == "LOAD_FILAMENT");
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load with no backend and no macro falls back to raw gcode",
                 "[filament][dispatch][wiring][ams]") {
    clear_filament_macros();
    REQUIRE(helix::AmsState::instance().get_backend() == nullptr);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing(LOAD_FALLBACK_MARKER));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar unload with no backend reaches the configured macro",
                 "[filament][dispatch][wiring][ams]") {
    configure_filament_macros();
    REQUIRE(helix::AmsState::instance().get_backend() == nullptr);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_unload(1);

    CHECK(prompt_count == 1);
    CHECK(prompted_macro == "UNLOAD_FILAMENT");
}

// =============================================================================
// The lifetime hazard tier 2 introduces on a mortal surface
// =============================================================================

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "A param-modal Run press after the sidebar is destroyed dispatches nothing",
                 "[filament][dispatch][wiring][ams][threading]") {
    configure_filament_macros();

    auto sidebar = std::make_unique<AmsOperationSidebar>(state);
    sidebar->handle_load_with_preheat(0);
    REQUIRE(prompt_count == 1);
    REQUIRE(pending_execute);

    // AmsPanel::clear_panel_reference() -> sidebar_.reset(). The modal still
    // holds the callback: dismissing it does not clear on_execute_.
    sidebar.reset();

    REQUIRE(drain_skip_total() == 0); // fresh window
    pending_execute({});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(drain_skip_total() == 1);
    CHECK_FALSE(gcode_sent_containing("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "A param-modal Run press while the sidebar is alive does dispatch",
                 "[filament][dispatch][wiring][ams][threading]") {
    // The other half of the contract: the guard must not swallow the normal
    // case, or the test above would pass against a callback that never runs.
    configure_filament_macros();

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);
    REQUIRE(pending_execute);

    REQUIRE(drain_skip_total() == 0);
    pending_execute({});
    helix::ui::UpdateQueue::instance().drain();

    CHECK(drain_skip_total() == 0);
    CHECK(gcode_sent_containing("LOAD_FILAMENT"));
}

// =============================================================================
// FilamentRunoutHandler — the surface that used to navigate away
// =============================================================================

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Runout load with no backend dispatches the macro instead of navigating",
                 "[filament][dispatch][wiring][runout]") {
    configure_filament_macros();
    REQUIRE(helix::AmsState::instance().get_backend() == nullptr);

    const helix::PanelId before = NavigationManager::instance().get_active();

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(gcode_sent_containing("LOAD_FILAMENT"));
    // The old fallback was set_active(PanelId::Filament), which tears the
    // guidance dialog out from under the user mid-runout.
    CHECK(NavigationManager::instance().get_active() == before);
}

TEST_CASE_METHOD(DispatchSurfaceFixture, "Runout load never prompts for macro parameters",
                 "[filament][dispatch][wiring][runout][params]") {
    // The macro's params are UNKNOWN here, which is exactly the state that makes
    // the Filament panel raise MacroParamModal. Stacking that on top of the live
    // runout dialog — whose own observers keep firing underneath — is the thing
    // ParamPolicy::Suppress exists to prevent.
    configure_filament_macros();

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Runout load with no backend and no macro falls back to raw gcode",
                 "[filament][dispatch][wiring][runout]") {
    clear_filament_macros();

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_load(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing(LOAD_FALLBACK_MARKER));
}

// -----------------------------------------------------------------------------
// Runout Unload / Purge — the two buttons the ladder conversion missed
// -----------------------------------------------------------------------------
//
// Only Load was converted. set_on_unload_filament and set_on_purge still called
// StandardMacros::execute() straight: no backend tier, no raw-gcode fallback,
// and a "Unload macro not configured" warning on a printer whose AMS backend
// would have handled it — or whose bowden a plain retract would have cleared.
//
// Mutation check: point dispatch_unload() back at StandardMacros::execute() and
// the raw-gcode case fails (nothing is sent at all); do the same for
// dispatch_purge() and its fallback case fails.

TEST_CASE_METHOD(DispatchSurfaceFixture, "Runout unload with no backend dispatches the macro",
                 "[filament][dispatch][wiring][runout]") {
    configure_filament_macros();
    REQUIRE(helix::AmsState::instance().get_backend() == nullptr);
    REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::UnloadFilament).is_empty());

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(gcode_sent_containing("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture, "Runout unload never prompts for macro parameters",
                 "[filament][dispatch][wiring][runout][params]") {
    // Same ParamPolicy::Suppress contract as Load: a MacroParamModal raised from
    // the runout dialog would stack on a live modal whose observers keep firing.
    configure_filament_macros();

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("UNLOAD_FILAMENT"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Runout unload with no backend and no macro falls back to raw gcode",
                 "[filament][dispatch][wiring][runout]") {
    // The old code warned "Unload macro not configured" and did nothing. The
    // tier-3 unload fallback is tip-shape then an 80mm retract.
    clear_filament_macros();

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_unload(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("G1 E-80"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture, "Runout purge routes through the shared macro tier",
                 "[filament][dispatch][wiring][runout]") {
    helix::PrinterDiscovery hardware;
    nlohmann::json objects = {"extruder", "gcode_macro PURGE"};
    hardware.parse_objects(objects);
    StandardMacros::instance().reset();
    StandardMacros::instance().init(hardware);
    REQUIRE_FALSE(StandardMacros::instance().get(StandardMacroSlot::Purge).is_empty());

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_purge(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0); // ParamPolicy::Suppress, even though params are UNKNOWN
    CHECK(gcode_sent_containing("PURGE"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture, "Runout purge with no macro falls back to raw gcode",
                 "[filament][dispatch][wiring][runout]") {
    // There is no plan_purge() — no backend exposes a purge entry point — so the
    // ladder here is macro then raw extrude. The old code warned and did nothing.
    clear_filament_macros();

    FilamentRunoutHandler handler(api.get());
    FilamentRunoutHandlerTestAccess::dispatch_purge(handler);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("G1 E50"));
}

// =============================================================================
// Saved parameter defaults on the shared dispatch
// =============================================================================

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "dispatch_filament_macro runs a saved ask-off record with no prompt",
                 "[filament][dispatch][wiring][macro]") {
    cache_macros({{"LOAD_FILAMENT", "{% set TEMP = params.TEMP|default(200)|int %}\n"
                                    "{% set SPEED = params.SPEED|default(50)|int %}\nG1 E10"}});
    helix::MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}};
    record.ask_for_params = false;
    helix::MacroParamDefaults::instance().set("LOAD_FILAMENT", record);

    MacroParamResult received;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt,
        [&received](const MacroParamResult& result) { received = result; });

    CHECK_FALSE(prompted);
    CHECK(prompt_count == 0);
    // SPEED has no saved value, so it falls back to the macro's own default.
    CHECK(received.params == ParamValues({{"TEMP", "210"}}));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "dispatch_filament_macro prefills the prompt with a saved ask-on record",
                 "[filament][dispatch][wiring][macro]") {
    cache_macros({{"LOAD_FILAMENT", "{% set TEMP = params.TEMP|default(200)|int %}\n"
                                    "{% set SPEED = params.SPEED|default(50)|int %}\nG1 E10"}});
    helix::MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}};
    helix::MacroParamDefaults::instance().set("LOAD_FILAMENT", record);

    MacroParamResult received;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt,
        [&received](const MacroParamResult& result) { received = result; });

    // A saved set never completes the run on its own - ask is on, so the
    // prompt still appears, carrying the saved value as its prefill.
    CHECK(prompted);
    CHECK(prompt_count == 1);
    CHECK(prompted_prefill == ParamValues({{"TEMP", "210"}}));
    CHECK(received.params.empty());
}

TEST_CASE_METHOD(DispatchSurfaceFixture, "a suppressed surface still sends the saved values",
                 "[filament][dispatch][wiring][macro]") {
    cache_macros({{"LOAD_FILAMENT", "{% set TEMP = params.TEMP|default(200)|int %}\nG1 E10"}});
    helix::MacroParamDefaultRecord record; // ask on: the record's default
    record.values = {{"TEMP", "210"}};
    helix::MacroParamDefaults::instance().set("LOAD_FILAMENT", record);

    MacroParamResult received;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Suppress,
        [&received](const MacroParamResult& result) { received = result; });

    CHECK_FALSE(prompted);
    CHECK(prompt_count == 0);
    CHECK(received.params == ParamValues({{"TEMP", "210"}}));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "caller-known values outrank the saved record on a prompted dispatch",
                 "[filament][dispatch][wiring][macro]") {
    cache_macros({{"LOAD_FILAMENT", "{% set TEMP = params.TEMP|default(200)|int %}\n"
                                    "{% set SPEED = params.SPEED|default(50)|int %}\nG1 E10"}});
    helix::MacroParamDefaultRecord record;
    record.values = {{"TEMP", "210"}, {"SPEED", "60"}};
    helix::MacroParamDefaults::instance().set("LOAD_FILAMENT", record);

    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt, [](const MacroParamResult&) {}, {{"TEMP", "220"}});

    // Known covers TEMP but not SPEED, so the prompt still appears - with the
    // surface's live temperature over the record's, and SPEED from the record.
    CHECK(prompted);
    CHECK(prompted_prefill == ParamValues({{"TEMP", "220"}, {"SPEED", "60"}}));
}

// =============================================================================
// Values the surface already knows: prefilled, and sent without a prompt when
// they cover every parameter the macro takes
// =============================================================================

namespace {

/// The EXTRUDER_TEMP a load macro is offered, or empty when it is offered none.
std::string offered_load_temp(int target_c, std::optional<int> material_c, int min_extrude_c,
                              int max_nozzle_c = 300) {
    const ParamValues values = helix::ui::nozzle_temp_prefill(
        FilamentMacroOp::Load, target_c, material_c, min_extrude_c, max_nozzle_c);
    auto it = values.find("EXTRUDER_TEMP");
    return it == values.end() ? std::string{} : it->second;
}

} // namespace

TEST_CASE("nozzle_temp_prefill offers load and unload macros EXTRUDER_TEMP, NOZZLE_TEMP and TEMP",
          "[filament][params][prefill]") {
    const ParamValues expected{{"EXTRUDER_TEMP", "260"}, {"NOZZLE_TEMP", "260"}, {"TEMP", "260"}};

    CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Load, 260, 220, 170, 300) == expected);
    CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Unload, 260, 220, 170, 300) == expected);
}

TEST_CASE("nozzle_temp_prefill offers a purge macro its temperature only as PURGE_TEMP",
          "[filament][params][prefill]") {
    CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Purge, 260, 220, 170, 300) ==
          ParamValues{{"PURGE_TEMP", "260"}});
}

TEST_CASE("nozzle_temp_prefill offers the hotter of the live target and the material",
          "[filament][params][prefill]") {
    CHECK(offered_load_temp(260, 240, 180) == "260");
    CHECK(offered_load_temp(200, 240, 180) == "240");
    CHECK(offered_load_temp(0, 220, 170) == "220");
}

TEST_CASE("nozzle_temp_prefill never offers a temperature below the extrusion minimum",
          "[filament][params][prefill]") {
    SECTION("a standby target with no material offers nothing") {
        CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Load, 140, std::nullopt, 180, 300)
                  .empty());
    }
    SECTION("a standby target gives way to a material above the minimum") {
        CHECK(offered_load_temp(140, 240, 180) == "240");
    }
    SECTION("a material below the minimum offers nothing") {
        CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Purge, 0, 170, 180, 300).empty());
    }
    SECTION("the minimum itself is not offered, one degree above it is") {
        // M109 returns within a degree of its target and Klipper compares its
        // smoothed temperature with the minimum, so a nozzle heated to exactly the
        // minimum can still be refused extrusion.
        CHECK(offered_load_temp(180, std::nullopt, 180).empty());
        CHECK(offered_load_temp(181, std::nullopt, 180) == "181");
    }
}

TEST_CASE("nozzle_temp_prefill never offers a temperature above the hotend's maximum",
          "[filament][params][prefill]") {
    SECTION("a material hotter than the hotend allows offers nothing") {
        CHECK(offered_load_temp(0, 290, 180, 280).empty());
        CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Purge, 0, 290, 180, 280).empty());
    }
    SECTION("the maximum itself is offered") {
        CHECK(offered_load_temp(0, 280, 180, 280) == "280");
    }
}

TEST_CASE("nozzle_temp_prefill knows nothing with the heater off and no material",
          "[filament][params][prefill]") {
    CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Load, 0, std::nullopt, 170, 300).empty());
    // A printer reporting no minimum still has no temperature to offer.
    CHECK(helix::ui::nozzle_temp_prefill(FilamentMacroOp::Load, 0, 0, 0, 300).empty());
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "A macro whose every parameter is known runs with those values and no prompt",
                 "[filament][dispatch][wiring][params][prefill]") {
    cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});

    bool ran = false;
    MacroParamResult seen;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt,
        [&](const MacroParamResult& r) {
            ran = true;
            seen = r;
        },
        ParamValues{{"EXTRUDER_TEMP", "260"}, {"PURGE_TEMP", "260"}});

    CHECK_FALSE(prompted);
    CHECK(prompt_count == 0);
    REQUIRE(ran);
    // Only what the macro reads is sent: PURGE_TEMP is not one of its parameters.
    CHECK(seen.params == ParamValues{{"EXTRUDER_TEMP", "260"}});
    CHECK(helix::build_macro_gcode("LOAD_FILAMENT", seen) == "LOAD_FILAMENT EXTRUDER_TEMP=260");
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "A parameter nothing fills still prompts, with the known ones prefilled",
                 "[filament][dispatch][wiring][params][prefill]") {
    cache_macros({{"LOAD_FILAMENT", "{% set t = params.EXTRUDER_TEMP|default(220)|int %}\n"
                                    "{% set l = params.LENGTH|default(100)|float %}\n"
                                    "M109 S{t}\nG1 E{l} F300"}});

    bool ran = false;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt, [&](const MacroParamResult&) { ran = true; },
        ParamValues{{"EXTRUDER_TEMP", "260"}});

    CHECK(prompted);
    CHECK(prompt_count == 1);
    CHECK_FALSE(ran);
    CHECK(prompted_prefill == ParamValues{{"EXTRUDER_TEMP", "260"}});
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "With nothing known a params-taking macro prompts with nothing prefilled",
                 "[filament][dispatch][wiring][params][prefill]") {
    cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});

    bool ran = false;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt, [&](const MacroParamResult&) { ran = true; }, {});

    CHECK(prompted);
    CHECK(prompt_count == 1);
    CHECK_FALSE(ran);
    CHECK(prompted_prefill.empty());
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Known values never skip the prompt for a macro whose parameters are unknown",
                 "[filament][dispatch][wiring][params][prefill]") {
    // Empty cache: nothing says the macro reads EXTRUDER_TEMP, so sending it
    // unasked would be a guess.
    bool ran = false;
    const bool prompted = helix::ui::dispatch_filament_macro(
        "LOAD_FILAMENT", ParamPolicy::Prompt, [&](const MacroParamResult&) { ran = true; },
        ParamValues{{"EXTRUDER_TEMP", "260"}});

    CHECK(prompted);
    CHECK(prompt_count == 1);
    CHECK_FALSE(ran);
    // Nor is it typed into the free-text prompt, which reads no parameter names.
    CHECK(prompted_prefill.empty());
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load sends the live nozzle target to a temperature-only macro",
                 "[filament][dispatch][wiring][ams][prefill]") {
    configure_filament_macros();
    cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    set_extruder_target(260.0);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("LOAD_FILAMENT EXTRUDER_TEMP=260"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar unload sends the live nozzle target to a temperature-only macro",
                 "[filament][dispatch][wiring][ams][prefill]") {
    configure_filament_macros();
    cache_macros({{"UNLOAD_FILAMENT",
                   "{% set t = params.TEMP|default(200)|int %}\nM109 S{t}\nG1 E-60 F600"}});
    set_extruder_target(250.0);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_unload(1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("UNLOAD_FILAMENT TEMP=250"));
}

namespace {

/// AmsState as a sidebar test needs it: subjects up, no external spool, and
/// optionally a backend, all undone at scope exit.
struct AmsScope {
    explicit AmsScope(std::unique_ptr<helix::AmsBackend> backend = nullptr) {
        helix::AmsState::instance().init_subjects(true);
        helix::AmsState::instance().clear_external_spool_info();
        if (backend) {
            helix::AmsState::instance().set_backend(std::move(backend));
        }
    }
    ~AmsScope() {
        helix::AmsState::instance().set_backend(nullptr);
        helix::AmsState::instance().deinit_subjects();
    }
    AmsScope(const AmsScope&) = delete;
    AmsScope& operator=(const AmsScope&) = delete;
};

} // namespace

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load with the heater off sends the lane's material temperature",
                 "[filament][dispatch][wiring][ams][prefill]") {
    configure_filament_macros();
    cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    set_extruder_target(0.0);
    AmsScope ams(std::make_unique<helix::test::LaneMaterialBackend>(/*lane=*/1, 240));

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing("LOAD_FILAMENT EXTRUDER_TEMP=240"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load never sends a live target below the extrusion minimum",
                 "[filament][dispatch][wiring][ams][prefill]") {
    configure_filament_macros();
    cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    auto limits = api->get_safety_limits();
    limits.min_extrude_temp_celsius = 180.0;
    api->set_safety_limits(limits);
    // A paused printer holds its nozzle at a standby temperature below the minimum.
    set_extruder_target(140.0);
    AmsScope ams;

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 1);
    CHECK(prompted_prefill.empty());
    CHECK_FALSE(gcode_sent_containing("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load never sends a material temperature above the hotend's maximum",
                 "[filament][dispatch][wiring][ams][prefill]") {
    configure_filament_macros();
    cache_macros({{"LOAD_FILAMENT", helix::test::LOAD_FILAMENT_EXPRESSION_DEFAULT}});
    auto limits = api->get_safety_limits();
    limits.set_max_temp_for("extruder", 280.0);
    api->set_safety_limits(limits);
    set_extruder_target(0.0);
    // The lane's material recommends more than this hotend is configured for.
    AmsScope ams(std::make_unique<helix::test::LaneMaterialBackend>(/*lane=*/1, 290));

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(1);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(prompt_count == 1);
    CHECK(prompted_prefill.empty());
    CHECK_FALSE(gcode_sent_containing("LOAD_FILAMENT"));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar unload with no printer connection offers its macro no temperature",
                 "[filament][dispatch][wiring][ams][prefill]") {
    // Unload dispatches its macro inline, so its prefill is read with no printer
    // connection to take the extrusion limits from.
    configure_filament_macros();
    cache_macros({{"UNLOAD_FILAMENT",
                   "{% set t = params.TEMP|default(200)|int %}\nM109 S{t}\nG1 E-60 F600"}});
    set_extruder_target(260.0);
    AmsScope ams;
    set_moonraker_api(nullptr);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_unload(1);

    CHECK(prompt_count == 1);
    CHECK(prompted_prefill.empty());
}
