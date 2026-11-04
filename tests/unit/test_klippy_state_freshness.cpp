// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_klippy_state_freshness.cpp
 * @brief Klippy state must not be overwritten by a stale webhooks snapshot.
 *
 * Klippy state is fed by two paths that are NOT ordered relative to each other:
 * live `notify_status_update` frames off the WebSocket, and the discovery
 * subscription response — captured on a background thread, carried through the
 * rest of discovery, then replayed through
 * `MoonrakerClient::dispatch_status_update()`. Without a freshness guard the
 * replay is last-write-wins, so a snapshot captured while Klipper was still READY
 * resurrects READY after a SHUTDOWN: nav buttons re-enable, the recovery dialog
 * auto-dismisses, and the gcode guards re-open against a dead printer.
 *
 * Two independent signals gate the write:
 *
 *  - Provenance, STATED not inferred. `dispatch_status_update(status, true)`
 *    stamps kCachedSnapshotMarker on the synthetic notification. A zero eventtime
 *    is NOT a usable proxy: MoonrakerClientMock drives its simulated shutdown and
 *    recovery through the very same untimestamped dispatch, and those are current
 *    truth for their session, not a replay.
 *  - Klipper's eventtime, for genuine out-of-order arrival between the two queues.
 *    It is monotonic-clock derived, so it survives a Klipper restart and only
 *    rewinds on a host reboot — which necessarily drops the WebSocket, hence
 *    reset_klippy_state_freshness() on disconnect.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/moonraker_client_test_access.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "i_moonraker_client.h"
#include "moonraker_client.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

nlohmann::json webhooks_status(const char* klippy_state, const char* message = nullptr) {
    nlohmann::json webhooks = {{"state", klippy_state}};
    if (message) {
        webhooks["state_message"] = message;
    }
    return nlohmann::json{{"webhooks", webhooks}};
}

/// What MoonrakerManager's notification drain does with each frame.
/// What MoonrakerManager's drain does with a frame it queued during @p epoch.
void apply_notification(PrinterState& state, const nlohmann::json& notification, uint64_t epoch) {
    if (auto frame = parse_status_notification(notification)) {
        state.update_from_status(*frame->status, frame->eventtime, frame->from_cached_snapshot,
                                 epoch);
    }
}

void apply_notification(PrinterState& state, const nlohmann::json& notification) {
    apply_notification(state, notification, state.network_state().klippy_epoch());
}

class KlippyFreshnessFixture : public LVGLTestFixture {
  public:
    KlippyFreshnessFixture() {
        state.init_subjects(false);
        // Wire the client's fan-out to PrinterState the way MoonrakerManager does,
        // so the cached-snapshot marker is exercised end to end rather than faked.
        client.register_notify_update(
            [this](const nlohmann::json& n) { apply_notification(state, n); });
    }

    ~KlippyFreshnessFixture() override {
        helix::ui::UpdateQueue::instance().drain();
    }

    /// A live Klipper frame: real eventtime, no replay marker.
    void live(const char* klippy_state, double eventtime, const char* message = nullptr) {
        nlohmann::json notification = {
            {"method", "notify_status_update"},
            {"params", nlohmann::json::array({webhooks_status(klippy_state, message), eventtime})}};
        apply_notification(state, notification);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The discovery subscription snapshot, replayed at the end of discovery.
    void replay(const char* klippy_state, const char* message = nullptr) {
        client.dispatch_status_update(webhooks_status(klippy_state, message),
                                      /*from_cached_snapshot=*/true);
        helix::ui::UpdateQueue::instance().drain();
    }

    /// A synthetic-but-current dispatch — what MoonrakerClientMock does when it
    /// simulates a shutdown or a recovery. Untimestamped, but NOT a replay.
    void synthetic_live(const char* klippy_state, const char* message = nullptr) {
        client.dispatch_status_update(webhooks_status(klippy_state, message));
        helix::ui::UpdateQueue::instance().drain();
    }

    KlippyState klippy() {
        return static_cast<KlippyState>(
            lv_subject_get_int(state.network_state().get_klippy_state_subject()));
    }

    MoonrakerClient client;
    PrinterState state;
};

} // namespace

// ============================================================================
// T1 — the reported bug: the replayed discovery snapshot must not resurrect
// READY over a live SHUTDOWN.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: cached-snapshot replay cannot overwrite live state",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0);
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    replay("ready");
    CHECK(klippy() == KlippyState::SHUTDOWN);
}

// ============================================================================
// T2 — out-of-order live frames: an older eventtime loses.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: older eventtime is ignored",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0);
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    live("ready", 90.0);
    CHECK(klippy() == KlippyState::SHUTDOWN);
}

// ============================================================================
// T3 — forward progress still applies. The guard must not wedge the state.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: newer eventtime applies",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0);
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    live("ready", 110.0);
    CHECK(klippy() == KlippyState::READY);
}

// ============================================================================
// T4 — cold seed: with nothing live yet, even the replayed snapshot must seed
// the state. That is the normal cold-start ordering.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: cold seed from replay applies",
                 "[core][klippy][freshness]") {
    // Subject defaults to SHUTDOWN, so READY is an observable change.
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    replay("ready");
    CHECK(klippy() == KlippyState::READY);
}

// ============================================================================
// T5 — reconnect: a host reboot rewinds the monotonic clock, so the watermark
// must not survive the disconnect.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: reset re-arms cold seeding",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0);
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    replay("ready");
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    // Post-reboot Klipper restarts its clock, so the next session's frames carry
    // eventtimes far below the old watermark.
    state.network_state().reset_klippy_state_freshness();
    helix::ui::UpdateQueue::instance().drain();
    live("ready", 3.0);
    CHECK(klippy() == KlippyState::READY);
}

// A reconnect whose Klippy stayed up sends no new webhooks.state: the
// resubscribe snapshot is the only report the new session gets.
TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: after a reset the resubscribe snapshot alone recovers",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0);
    replay("ready");
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    state.network_state().reset_klippy_state_freshness();
    replay("ready");
    CHECK(klippy() == KlippyState::READY);
}

// ============================================================================
// T6 — the state_message rides in the same blob, so a replayed snapshot's
// message is equally stale and must not overwrite the live one.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: stale state_message is dropped",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0, "MCU 'mcu' shutdown: Timer too close");
    REQUIRE(klippy() == KlippyState::SHUTDOWN);
    REQUIRE(state.network_state().get_klippy_state_message() ==
            "MCU 'mcu' shutdown: Timer too close");

    replay("ready", "");
    CHECK(klippy() == KlippyState::SHUTDOWN);
    CHECK(state.network_state().get_klippy_state_message() ==
          "MCU 'mcu' shutdown: Timer too close");
}

// ============================================================================
// T6b — notify_klippy_shutdown / _ready reach PrinterState through
// set_klippy_state*, not through a status blob. Those are authoritative and must
// latch "live seen" so a later replay cannot undo them.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: set_klippy_state_sync outranks a later replay",
                 "[core][klippy][freshness]") {
    state.set_klippy_state_sync(KlippyState::ERROR);
    REQUIRE(klippy() == KlippyState::ERROR);

    replay("ready");
    CHECK(klippy() == KlippyState::ERROR);
}

// ============================================================================
// T8 — the regression the provenance flag exists to prevent. An UNFLAGGED
// dispatch_status_update (the mock's simulated shutdown / recovery, and every
// other synthetic dispatch) is current truth and must still apply after a live
// state has landed, even though it carries no eventtime.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: unflagged synthetic dispatch is live and still applies",
                 "[core][klippy][freshness]") {
    live("ready", 100.0);
    REQUIRE(klippy() == KlippyState::READY);

    synthetic_live("shutdown", "Mock shutdown");
    CHECK(klippy() == KlippyState::SHUTDOWN);
    CHECK(state.network_state().get_klippy_state_message() == "Mock shutdown");

    synthetic_live("ready");
    CHECK(klippy() == KlippyState::READY);
}

// ============================================================================
// The overload the production drain calls, without the envelope in between.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: update_from_status honours eventtime and provenance",
                 "[core][klippy][freshness]") {
    state.update_from_status(webhooks_status("shutdown"), 100.0);
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    state.update_from_status(webhooks_status("ready"), 0.0, /*from_cached_snapshot=*/true);
    CHECK(klippy() == KlippyState::SHUTDOWN);

    state.update_from_status(webhooks_status("ready"), 90.0);
    CHECK(klippy() == KlippyState::SHUTDOWN);

    state.update_from_status(webhooks_status("ready"), 101.0);
    CHECK(klippy() == KlippyState::READY);
}

// ============================================================================
// The reset and the status frames reach PrinterState through different queues:
// frames through MoonrakerManager's notification queue, deferred UI work through
// the UpdateQueue, which a panel rebuild or splash can hold frozen. A reset that
// waited in the UpdateQueue would land after the next session's frames and wipe
// their watermark.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: a reconnect while the UI queue is frozen keeps the new "
                 "session's state",
                 "[core][klippy][freshness]") {
    live("ready", 100.0);
    REQUIRE(klippy() == KlippyState::READY);

    {
        helix::ui::UpdateQueue::ScopedFreeze freeze(helix::ui::UpdateQueue::instance());
        state.network_state().reset_klippy_state_freshness(); // the link drops
        live("shutdown", 5.0);                                // the next session's clock restarted
        CHECK(klippy() == KlippyState::SHUTDOWN);
    }
    helix::ui::UpdateQueue::instance().drain();

    replay("ready");
    CHECK(klippy() == KlippyState::SHUTDOWN);
}

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: notify_klippy_* outranks a snapshot already queued",
                 "[core][klippy][freshness]") {
    helix::ui::UpdateQueue::ScopedFreeze freeze(helix::ui::UpdateQueue::instance());
    state.set_klippy_state(KlippyState::SHUTDOWN); // WebSocket thread
    replay("ready");                               // drained before the UI queue
    CHECK(klippy() != KlippyState::READY);
}

// ============================================================================
// A frame queued before the link dropped is drained after the reset. Klipper's
// clock restarts on a host reboot, so its eventtime must not become the next
// session's watermark.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: a frame queued before a reset cannot block the next session",
                 "[core][klippy][freshness]") {
    live("ready", 100.0);
    const uint64_t old_epoch = state.network_state().klippy_epoch();
    const nlohmann::json queued = {
        {"method", "notify_status_update"},
        {"params", nlohmann::json::array({webhooks_status("ready"), 150.0})}};

    state.network_state()
        .reset_klippy_state_freshness(); // the link drops; `queued` is still in the queue
    apply_notification(state, queued, old_epoch);
    helix::ui::UpdateQueue::instance().drain();

    live("shutdown", 5.0); // the rebooted host's clock restarted
    CHECK(klippy() == KlippyState::SHUTDOWN);

    replay("ready"); // the new session's discovery snapshot cannot undo it
    CHECK(klippy() == KlippyState::SHUTDOWN);
}

TEST_CASE("Klippy freshness: an auto-reconnect leaves a queued old frame behind",
          "[core][klippy][freshness]") {
    // libhv reconnects without connect(), so connection_generation() cannot
    // tell the sessions apart. The close handler's reset has to.
    LVGLTestFixture lvgl;
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::reset(ps);
    ps.init_subjects(false);
    MoonrakerClient client;
    const auto klippy = [&ps] {
        return static_cast<KlippyState>(
            lv_subject_get_int(ps.network_state().get_klippy_state_subject()));
    };

    apply_notification(ps, {{"method", "notify_status_update"},
                            {"params", nlohmann::json::array({webhooks_status("ready"), 100.0})}});
    const uint64_t old_epoch = ps.network_state().klippy_epoch();
    const nlohmann::json queued = {
        {"method", "notify_status_update"},
        {"params", nlohmann::json::array({webhooks_status("ready"), 150.0})}};

    const uint64_t generation = client.connection_generation();
    MoonrakerClientTestAccess::fire_ws_close(client);
    REQUIRE(client.connection_generation() == generation);

    apply_notification(ps, queued, old_epoch);
    apply_notification(ps, {{"method", "notify_status_update"},
                            {"params", nlohmann::json::array({webhooks_status("shutdown"), 5.0})}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(klippy() == KlippyState::SHUTDOWN);

    PrinterStateTestAccess::reset(ps);
}

// ============================================================================
// A status frame's fan-out runs observers synchronously. One that calls back into
// PrinterState must not wedge the frame that notified it.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture,
                 "Klippy freshness: an observer may call back into PrinterState mid-frame",
                 "[core][klippy][freshness]") {
    auto callback = [](lv_observer_t* observer, lv_subject_t*) {
        auto* self = static_cast<KlippyFreshnessFixture*>(lv_observer_get_user_data(observer));
        self->state.network_state().reset_klippy_state_freshness();
        self->state.set_klippy_state_sync(KlippyState::SHUTDOWN);
    };
    lv_observer_t* observer =
        lv_subject_add_observer(state.network_state().get_klippy_state_subject(), callback, this);

    live("ready", 100.0);
    CHECK(klippy() == KlippyState::SHUTDOWN);

    lv_observer_remove(observer);
}

// ============================================================================
// set_klippy_state_if_unseeded: printer.info may seed, never override. Its
// response can land after the WebSocket has already reported a newer state.
// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: printer.info seeds but never overrides",
                 "[core][klippy][freshness]") {
    SECTION("seeds when nothing live has arrived") {
        state.set_klippy_state_if_unseeded(KlippyState::ERROR);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(klippy() == KlippyState::ERROR);
    }

    SECTION("no-ops once a live state has landed") {
        live("shutdown", 100.0);
        REQUIRE(klippy() == KlippyState::SHUTDOWN);

        state.set_klippy_state_if_unseeded(KlippyState::READY);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(klippy() == KlippyState::SHUTDOWN);
    }

    SECTION("a seed carries its reason") {
        state.set_klippy_state_if_unseeded(KlippyState::ERROR, "key298: rpi MCU is shutdown");
        helix::ui::UpdateQueue::instance().drain();
        CHECK(klippy() == KlippyState::ERROR);
        CHECK(state.network_state().get_klippy_fault_message() == "key298: rpi MCU is shutdown");
    }

    SECTION("a refused seed leaves the live reason alone") {
        live("shutdown", 100.0);
        const std::string live_reason = state.network_state().get_klippy_state_message();

        state.set_klippy_state_if_unseeded(KlippyState::ERROR, "stale reason");
        helix::ui::UpdateQueue::instance().drain();
        CHECK(state.network_state().get_klippy_state_message() == live_reason);
    }
}

// ============================================================================

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: unknown webhooks state changes nothing",
                 "[core][klippy][freshness]") {
    live("shutdown", 100.0);
    REQUIRE(klippy() == KlippyState::SHUTDOWN);

    live("wedged", 110.0);
    CHECK(klippy() == KlippyState::SHUTDOWN);

    // ...and it must not poison the watermark: a real state at a newer eventtime
    // still applies.
    live("ready", 120.0);
    CHECK(klippy() == KlippyState::READY);
}

// ============================================================================
// T8 — the regression the provenance flag exists to prevent. An UNFLAGGED
// dispatch_status_update (the mock's simulated shutdown / recovery, and every
// other synthetic dispatch) is current truth and must still apply after a live
// state has landed, even though it carries no eventtime.
// ============================================================================
// Null-safety: Moonraker may send webhooks.state as JSON null
// ============================================================================
//
// `webhooks: {"state": null}` is a shape Moonraker really emits, and every
// assertion above feeds a well-formed string, so nothing here exercised it.
// The stale branch reads the state purely to name it in its debug line, and
// nlohmann's .value() throws type_error.302 on a null — inside the status
// parse, on the one path that only runs when the printer is already degraded.
// A throw there takes out the frame that was trying to report the problem.

TEST_CASE_METHOD(KlippyFreshnessFixture, "Klippy freshness: null webhooks.state never throws",
                 "[core][klippy][freshness]") {
    auto null_state_frame = [] {
        nlohmann::json webhooks = nlohmann::json::object();
        webhooks["state"] = nullptr;
        webhooks["state_message"] = nullptr;
        return nlohmann::json{{"webhooks", webhooks}};
    };

    // Both sections drive update_from_status() DIRECTLY rather than through
    // dispatch_status_update(), which defers the parse onto the UpdateQueue, so a
    // throw would escape during drain() — outside any REQUIRE_NOTHROW here, and
    // swallowed by the queue. This calls the parse on the test's own stack.

    SECTION("on a stale replay — the branch that formats the state into a log line") {
        live("shutdown", 100.0);
        REQUIRE(klippy() == KlippyState::SHUTDOWN);

        // stale == true here: cached snapshot arriving after a live state landed.
        REQUIRE_NOTHROW(state.update_from_status(null_state_frame(), 0.0,
                                                 /*from_cached_snapshot=*/true));
        CHECK(klippy() == KlippyState::SHUTDOWN);
    }

    SECTION("on a live frame — the parse must skip it, not guess") {
        live("shutdown", 100.0);
        REQUIRE(klippy() == KlippyState::SHUTDOWN);

        REQUIRE_NOTHROW(state.update_from_status(null_state_frame(), 110.0,
                                                 /*from_cached_snapshot=*/false));
        CHECK(klippy() == KlippyState::SHUTDOWN);
    }
}
