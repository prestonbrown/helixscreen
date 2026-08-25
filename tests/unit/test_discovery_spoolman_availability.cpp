// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_discovery_spoolman_availability.cpp
 * @brief Discovery must not flap the Spoolman capability flag.
 *
 * K2 Plus, 2026-08-24. Every boot logged, in order:
 *
 *   [Moonraker Client] Spoolman component detected, checking status...
 *   [Moonraker Client] Spoolman status: connected=true
 *   [SpoolmanManager] Spoolman became unavailable, stopping polling
 *
 * Spoolman was connected the whole time and served 134 spools. The sequence
 * cleared the flag unconditionally at the top of every components scan and
 * relied on an unguarded async status RPC to put it back, so any rediscovery —
 * and rediscovery reruns on every notify_klippy_ready and every websocket
 * reconnect, not just the printer switch the clear was written for — produced a
 * false->true edge pair.
 *
 * That falling edge is destructive and one-way: SpoolmanManager deletes its poll
 * timer and drops the identity cache that supplies every slot's vendor and
 * material, nothing re-verifies afterwards, and the one manual recheck is itself
 * hidden behind this same flag. The printer sat unmanaged for five days.
 *
 * So the end state is not enough to assert — a flap ends correct. These pin the
 * EDGES.
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Runs the REAL discovery sequence while routing RPCs through the mock
/// registry (same seam as test_discovery_klippy_gate.cpp).
class TestDiscoveryClient : public MoonrakerClientMock {
  public:
    using MoonrakerClientMock::MoonrakerClientMock;

    void discover_printer_real() {
        MoonrakerClient::discover_printer([]() {}, [](const std::string&) {});
    }
};

/// Counts falling edges (1 -> 0) on printer_has_spoolman.
struct FallingEdgeCounter {
    int falls = 0;
    int last = -1;

    void observe(int value) {
        if (last == 1 && value == 0) {
            ++falls;
        }
        last = value;
    }
};

/// PrinterCapabilitiesState writes the flag through AsyncLifetimeGuard::defer(),
/// so it lands on a later process_pending tick, not the current one.
void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

/// The capability subjects must exist before the flag can be read back — a bare
/// LVGL fixture does not create them.
void init_printer_subjects() {
    get_printer_state().init_subjects(false);
}

} // namespace

TEST_CASE("Discovery does not flap Spoolman availability when the component is present",
          "[discovery][spoolman]") {
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_mock_spoolman_enabled(true);

    init_printer_subjects();

    // Settle into the steady state a running printer is already in.
    get_printer_state().set_spoolman_available(true);
    drain();
    REQUIRE(get_printer_state().is_spoolman_available());

    FallingEdgeCounter counter;
    auto* subject = get_printer_state().get_printer_has_spoolman_subject();
    REQUIRE(subject != nullptr);
    lv_observer_t* obs = lv_subject_add_observer(
        subject,
        [](lv_observer_t* o, lv_subject_t* s) {
            static_cast<FallingEdgeCounter*>(lv_observer_get_user_data(o))
                ->observe(lv_subject_get_int(s));
        },
        &counter);
    REQUIRE(obs != nullptr);

    // Rediscovery, exactly as notify_klippy_ready triggers it.
    client.discover_printer_real();
    drain();

    CHECK(get_printer_state().is_spoolman_available());
    CHECK(counter.falls == 0); // the whole bug, in one number

    // And again — the K2 reran this on every reconnect for five days.
    client.discover_printer_real();
    drain();

    CHECK(get_printer_state().is_spoolman_available());
    CHECK(counter.falls == 0);

    lv_observer_remove(obs);
}

TEST_CASE("Discovery still clears Spoolman availability when the component is gone",
          "[discovery][spoolman]") {
    // The clear exists so switching to a printer without Spoolman hides the row.
    // Narrowing it to the absent-component case must not lose that.
    LVGLTestFixture fixture;

    TestDiscoveryClient client;
    client.set_klippy_state(MoonrakerClientMock::KlippyState::READY);
    client.set_mock_spoolman_enabled(false);

    init_printer_subjects();

    get_printer_state().set_spoolman_available(true);
    drain();
    REQUIRE(get_printer_state().is_spoolman_available());

    client.discover_printer_real();
    drain();

    CHECK_FALSE(get_printer_state().is_spoolman_available());
}
