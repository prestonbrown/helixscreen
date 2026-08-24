// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_bypass_toggle_controller.cpp
 * @brief Guard matrix for the shared bypass toggle policy.
 *
 * Run with: ./build/bin/helix-tests "[bypass-home]"
 */

#include "ui_bypass_toggle_controller.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include <memory>
#include <string_view>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

/// PrinterState helper: set print_state_enum the way a status update would.
/// Same accessor the controller reads (singleton via app_globals), so test and
/// code under test can never disagree on which PrinterState holds the state.
void seed_print_state(PrintJobState state) {
    helix::test::set_wire_state(get_printer_state(), state);
}

/// Raise a host-side pre-print phase: print_stats still reads standby, but the
/// lifecycle becomes Preparing. The wire cannot express this window at all.
void seed_preprint_phase(helix::PrintStartPhase phase) {
    get_printer_state().set_print_start_state(phase, "", 0);
    helix::ui::UpdateQueue::instance().drain();
}

/// Install a started, zero-delay mock as AmsState's primary backend and hand
/// back the raw pointer — the controller resolves its backend through
/// AmsState::instance().get_backend(), so the mock must live there, not beside
/// the fixture. Same idiom as test_ams_bypass_preflight_wiring.cpp.
class BypassToggleFixture : public LVGLTestFixture {
  public:
    AmsBackendMock* backend = nullptr;
    BypassToggleController controller;

    BypassToggleFixture() {
        // PrinterState's subjects first — AmsState's init_subjects() observers
        // the print_state_enum subject, and the controller reads it raw. Same
        // order as test_abort_manager.cpp.
        get_printer_state().init_subjects(false);
        auto& ams = AmsState::instance();
        ams.init_subjects(false);

        auto owned = std::make_unique<AmsBackendMock>(4);
        backend = owned.get();
        backend->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        REQUIRE(backend->start());
    }

    ~BypassToggleFixture() override {
        controller.cancel_pending();
        // Join any in-flight simulated op BEFORE detaching: the mock's threads
        // must not outlive the backend AmsState owns.
        if (backend) {
            backend->wait_for_operation_thread();
        }
        // Drain while the backend is still installed so queued backend-event
        // syncs do not leak into the next test.
        UpdateQueue::instance().drain();
        AmsState::instance().set_backend(nullptr);
    }
};

} // namespace

TEST_CASE("bypass toggle: refuses while printing", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    REQUIRE_FALSE(fx.backend->is_bypass_active());
    seed_print_state(PrintJobState::PRINTING);
    fx.controller.toggle();
    CHECK_FALSE(fx.backend->is_bypass_active()); // no enable happened
    CHECK_FALSE(fx.controller.pending_enable());

    seed_print_state(PrintJobState::PAUSED);
    fx.controller.toggle();
    CHECK_FALSE(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle: refuses during a host-side pre-print block", "[ams][bypass-home]") {
    // print_stats reads standby for the whole of a host-side block, so the wire
    // cannot distinguish this from idle — the tile was tappable while the
    // pre-start G-code homed and probed, and the handler agreed to drive
    // filament through a moving toolhead.
    BypassToggleFixture fx;
    REQUIRE_FALSE(fx.backend->is_bypass_active());

    seed_print_state(PrintJobState::STANDBY);
    seed_preprint_phase(helix::PrintStartPhase::BED_MESH);

    fx.controller.toggle();
    CHECK_FALSE(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());

    // Abandoning the block hands the control back — a latched refusal would be
    // worse than the bug. Unload first so this takes the DIRECT enable path, the
    // same setup the standby case below uses; the mock boots with slot 0 loaded
    // and would otherwise go down the unload-first chain.
    seed_preprint_phase(helix::PrintStartPhase::IDLE);
    REQUIRE(fx.backend->unload_active_filament().result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();

    fx.controller.toggle();
    CHECK(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle: standby allows enable/disable", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    // The mock boots with slot 0 loaded (demo appearance). Unload it first so
    // this case exercises the DIRECT enable/disable path — the unload-first
    // chain is covered by its own tests below.
    REQUIRE(fx.backend->unload_active_filament().result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();

    fx.controller.toggle();
    CHECK(fx.backend->is_bypass_active());

    fx.controller.toggle();
    CHECK_FALSE(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle chain: unload completes -> enable fires", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    // Load a slot first so the toggle takes the unload-first path. The load
    // settles on the mock's operation thread — join it, then the system info
    // snapshot inside toggle() sees filament actually loaded.
    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();

    fx.controller.toggle();
    CHECK(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());

    // The real unload also settles on the operation thread; enable_bypass()
    // refuses while the mock reports a non-IDLE action, so join before the
    // chain step.
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();

    // The chain step: UNLOADING -> IDLE.
    CHECK(fx.controller.on_ams_action_changed(AmsAction::UNLOADING, AmsAction::IDLE));
    CHECK(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());
}

TEST_CASE("bypass toggle chain: unload ERROR disarms (regression)", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();

    fx.controller.toggle();
    REQUIRE(fx.controller.pending_enable());
    fx.backend->wait_for_operation_thread();

    CHECK(fx.controller.on_ams_action_changed(AmsAction::UNLOADING, AmsAction::ERROR));
    CHECK_FALSE(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle chain: controller self-observes the ams_action subject",
          "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    // Lane loaded so toggle() takes the unload-first path (arms the chain and
    // the controller's own ams_action observer).
    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();

    fx.controller.toggle();
    REQUIRE(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());

    // Let the real mock unload settle on its operation thread so the eventual
    // enable_bypass() is not refused for a busy backend.
    fx.backend->wait_for_operation_thread();
    UpdateQueue::instance().drain();
    REQUIRE(fx.controller.pending_enable());

    // The production feed: nobody hand-drives on_ams_action_changed() — the
    // controller's own observer computes the edges from the subject, the way
    // sync_from_backend() publishes them. observe_int_sync defers the handler
    // through UpdateQueue, hence the drain between edges.
    lv_subject_t* action = AmsState::instance().get_ams_action_subject();
    lv_subject_set_int(action, static_cast<int>(AmsAction::UNLOADING));
    UpdateQueue::instance().drain(); // IDLE -> UNLOADING: arming edge, no settle
    CHECK(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());

    lv_subject_set_int(action, static_cast<int>(AmsAction::IDLE));
    UpdateQueue::instance().drain(); // UNLOADING -> IDLE: the settling edge
    CHECK(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());

    // Settled exactly once: the observer detached on settle, so a replayed
    // edge neither re-enables nor re-arms anything.
    CHECK_FALSE(fx.controller.on_ams_action_changed(AmsAction::UNLOADING, AmsAction::IDLE));
    CHECK(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle chain: event not ours is ignored", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);
    CHECK_FALSE(fx.controller.on_ams_action_changed(AmsAction::IDLE, AmsAction::LOADING));
    CHECK_FALSE(fx.controller.on_ams_action_changed(AmsAction::UNLOADING, AmsAction::IDLE));
}

// --- Tile render/gate (needs LVGL + XML registration) ---

TEST_CASE("bypass widget: gated on ams_supports_bypass", "[ams][bypass-home]") {
    LVGLUITestFixture fx; // registers XML components incl. panel_widget_bypass

    const auto* def = helix::find_widget_def("bypass");
    REQUIRE(def != nullptr);
    CHECK(def->hardware_gate_subject != nullptr);
    CHECK(std::string_view(def->hardware_gate_subject) == "ams_supports_bypass");
    // Default span 1x1, scalable to 2x1 per the registry row.
    CHECK(def->colspan == 1);
    CHECK(def->rowspan == 1);
    CHECK(def->max_colspan == 2);
    CHECK(def->max_rowspan == 1);
    // opt-in tile, like the ams row
    CHECK_FALSE(def->default_enabled);
}
