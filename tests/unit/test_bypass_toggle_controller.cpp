// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_bypass_toggle_controller.cpp
 * @brief Guard matrix for the shared bypass toggle policy.
 *
 * Run with: ./build/bin/helix-tests "[bypass-home]"
 */

#include "ui_bypass_toggle_controller.h"
#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

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

/// Run the production wake path to completion. Two drains, because it is two
/// hops: the queued AmsState::on_backend_event bodies sync the backend and bump
/// ams_data_revision, and the observers that notification fires are themselves
/// deferred back onto the queue by observe_int_sync.
void settle_backend_events() {
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();
}

/// Records every value ams_action publishes, so a test can assert which edges
/// were actually observable instead of assuming the one it wants was.
class ActionRecorder {
  public:
    explicit ActionRecorder(lv_subject_t* subject) {
        // Immediate rather than deferred: recording a published value must not
        // itself be reordered against the drains the tests count on. It only
        // appends, so it touches no observer lifecycle.
        guard_ = helix::ui::observe_int_immediate<ActionRecorder>(
            subject, this,
            [](ActionRecorder* self, int value) {
                self->seen_.push_back(static_cast<AmsAction>(value));
            },
            AmsState::instance().get_subjects_lifetime());
    }

    [[nodiscard]] bool saw(AmsAction action) const {
        return std::find(seen_.begin(), seen_.end(), action) != seen_.end();
    }

  private:
    std::vector<AmsAction> seen_;
    ObserverGuard guard_;
};

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

    // No hand-driven chain step: the unload settles on the operation thread and
    // the backend events it emitted carry the chain the rest of the way.
    fx.backend->wait_for_operation_thread();
    settle_backend_events();

    CHECK(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());
}

TEST_CASE("bypass toggle chain: an unload publishing no action edge still enables bypass",
          "[ams][bypass-home][1512]") {
    // The production sequence: no status sync lands while the unload is in
    // flight, so the next one re-reads a backend that is already idle again and
    // publishes ams_action IDLE -> IDLE, which notifies nobody. A chain hung off
    // an UNLOADING -> IDLE edge waits for an edge nothing ever emits.
    //
    // Built by detaching event delivery rather than by racing the operation
    // thread: with AmsState unsubscribed, the whole unload provably happens
    // between two syncs instead of probably happening between them.
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    settle_backend_events();

    ActionRecorder actions(AmsState::instance().get_ams_action_subject());

    // Long enough that the drain below lands while the unload is still running,
    // so the observer's subscribe-time notification cannot settle the chain on
    // its own and the wake feed is the only thing left that can.
    fx.backend->set_operation_delay(80);
    fx.backend->set_event_callback(nullptr);

    fx.controller.toggle();
    REQUIRE(fx.controller.pending_enable());

    settle_backend_events();
    REQUIRE(fx.controller.pending_enable()); // still unloading, nothing to settle

    fx.backend->wait_for_operation_thread(); // the unload lands, unobserved

    // One status sync over a backend that finished while nobody was looking.
    // This is what AmsState::on_backend_event() does: sync, then bump the
    // revision. The sync publishes nothing - the action it reads is the action
    // already on the subject - so the revision bump is the whole signal.
    AmsState::instance().sync_from_backend();
    lv_subject_t* revision = AmsState::instance().get_ams_data_revision_subject();
    lv_subject_set_int(revision, lv_subject_get_int(revision) + 1);
    settle_backend_events();

    // The precondition this case exists for. Without it the test degrades into
    // the happy path above the moment the mock's timing shifts.
    CHECK_FALSE(actions.saw(AmsAction::UNLOADING));
    CHECK(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());
}

TEST_CASE("bypass toggle chain: enables whether or not a sync lands mid-unload",
          "[ams][bypass-home][1512]") {
    // The same request at two sampling rates. At delay 0 the unload is over
    // before the first drain; a slower one is still running when that drain
    // syncs, which is the only case an edge-chained controller could settle.
    // Both must reach bypass.
    const int delay_ms = GENERATE(0, 60);

    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    settle_backend_events();

    fx.backend->set_operation_delay(delay_ms);
    fx.controller.toggle();
    REQUIRE(fx.controller.pending_enable());

    // Sample first, join second - at delay 0 this drain lands after the unload,
    // at delay 60 while it is still in flight.
    settle_backend_events();
    fx.backend->wait_for_operation_thread();
    settle_backend_events();

    CHECK(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());
}

TEST_CASE("bypass toggle chain: unload ERROR disarms (regression)", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    settle_backend_events();

    fx.controller.toggle();
    REQUIRE(fx.controller.pending_enable());
    // Join without draining: the chain is still armed, and the backend can be
    // put into ERROR before any sync gets to settle it.
    fx.backend->wait_for_operation_thread();
    fx.backend->simulate_error(AmsResult::FILAMENT_JAM);

    settle_backend_events();
    CHECK_FALSE(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle chain: controller self-observes ams_data_revision", "[ams][bypass-home]") {
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);

    // Lane loaded so toggle() takes the unload-first path (arms the chain and
    // the controller's own revision observer).
    REQUIRE(fx.backend->load_filament(0).result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    settle_backend_events();

    // The unload has to still be running when the bump below lands, or the
    // controller settles on it and the "not yet" assertion becomes a coin toss
    // decided by the operation thread - the same timing dependence this whole
    // change is about. A delay makes it a fact of the test.
    fx.backend->set_operation_delay(80);

    fx.controller.toggle();
    REQUIRE(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());

    // A revision bump while the lane is still loaded is not a settle: the
    // controller re-reads the backend and finds the unload it asked for
    // outstanding. Bumped by hand here, the way a mid-unload sync would.
    lv_subject_t* revision = AmsState::instance().get_ams_data_revision_subject();
    lv_subject_set_int(revision, lv_subject_get_int(revision) + 1);
    settle_backend_events();
    CHECK(fx.controller.pending_enable());
    CHECK_FALSE(fx.backend->is_bypass_active());

    // Nobody hand-drives the chain step: the unload lands, the backend events
    // it emitted bump the revision, and the controller's own observer settles.
    fx.backend->wait_for_operation_thread();
    settle_backend_events();
    CHECK(fx.backend->is_bypass_active());
    CHECK_FALSE(fx.controller.pending_enable());

    // Settled exactly once: the observer came off the subject, and a replayed
    // bump neither re-enables nor re-arms anything.
    lv_subject_set_int(revision, lv_subject_get_int(revision) + 1);
    settle_backend_events();
    CHECK_FALSE(fx.controller.poll_pending_engage());
    CHECK(fx.backend->is_bypass_active());
}

TEST_CASE("bypass toggle chain: a poll with nothing armed is inert", "[ams][bypass-home]") {
    // Every backend event reaches every armed controller, so the ones with no
    // chain of their own must not read the enable out of somebody else's unload.
    BypassToggleFixture fx;
    seed_print_state(PrintJobState::STANDBY);
    CHECK_FALSE(fx.controller.poll_pending_engage());
    CHECK_FALSE(fx.backend->is_bypass_active());

    REQUIRE(fx.backend->unload_active_filament().result == AmsResult::SUCCESS);
    fx.backend->wait_for_operation_thread();
    settle_backend_events();
    CHECK_FALSE(fx.controller.poll_pending_engage());
    CHECK_FALSE(fx.backend->is_bypass_active());
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
