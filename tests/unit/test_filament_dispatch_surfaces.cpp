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
#include "ams_state.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "filament_op_router.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "standard_macros.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::CachedMacroInfo;
using helix::MacroExecuteCallback;
using helix::MacroParamResult;
using helix::ui::AmsOperationSidebar;
using helix::ui::FilamentRunoutHandler;
using helix::ui::FilamentRunoutHandlerTestAccess;
using helix::ui::ParamPolicy;

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
        AmsState::instance().clear_backends();
        // Empty cache => MacroParamKnowledge::UNKNOWN for every macro, i.e. the
        // branch that WOULD prompt. That is deliberate: it is the only way to
        // prove ParamPolicy::Suppress actually suppresses something.
        helix::MacroParamCache::instance().clear();

        // Record prompts instead of raising a real modal — the prompt branch has
        // to be reachable in a binary with no screen.
        helix::ui::set_filament_param_prompter([this](const std::string& macro,
                                                      const CachedMacroInfo&,
                                                      MacroExecuteCallback on_execute) {
            ++prompt_count;
            prompted_macro = macro;
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
        AmsState::instance().clear_backends();
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
    void clear_filament_macros() {
        StandardMacros::instance().reset();
        REQUIRE(StandardMacros::instance().get(StandardMacroSlot::LoadFilament).is_empty());
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
    REQUIRE(AmsState::instance().get_backend() == nullptr);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);

    CHECK(prompt_count == 1);
    CHECK(prompted_macro == "LOAD_FILAMENT");
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar load with no backend and no macro falls back to raw gcode",
                 "[filament][dispatch][wiring][ams]") {
    clear_filament_macros();
    REQUIRE(AmsState::instance().get_backend() == nullptr);

    AmsOperationSidebar sidebar(state);
    sidebar.handle_load_with_preheat(0);

    CHECK(prompt_count == 0);
    CHECK(gcode_sent_containing(LOAD_FALLBACK_MARKER));
}

TEST_CASE_METHOD(DispatchSurfaceFixture,
                 "Sidebar unload with no backend reaches the configured macro",
                 "[filament][dispatch][wiring][ams]") {
    configure_filament_macros();
    REQUIRE(AmsState::instance().get_backend() == nullptr);

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
    REQUIRE(AmsState::instance().get_backend() == nullptr);

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
    REQUIRE(AmsState::instance().get_backend() == nullptr);
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
