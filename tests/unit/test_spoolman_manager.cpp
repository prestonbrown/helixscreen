// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spoolman_manager.cpp
 * @brief Unit tests for SpoolmanManager singleton
 *
 * Tests refcount-based polling, circuit breaker state, and spoolman
 * availability gating. Does not require a real MoonrakerAPI — exercises
 * the internal state machine via the SpoolmanManagerTestAccess friend class.
 */

#include "ui_spoolman_overlay.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ad5x_ifs_test_access.h"
#include "../test_helpers/log_capture.h"
#include "../test_helpers/registered_backend.h"
#include "../test_helpers/seeded_override.h"
#include "../test_helpers/tool_state_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_mock.h"
#include "ams_backend_toolchanger.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "spoolman_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

// ============================================================================
// TestAccess — friend class for private member inspection (L065: no test
// methods on the class itself)
// ============================================================================

class SpoolmanManagerTestAccess {
  public:
    static int poll_refcount(SpoolmanManager& m) {
        return m.poll_refcount_;
    }
    /// nullptr until something both wants polling and can be served.
    static lv_timer_t* poll_timer(SpoolmanManager& m) {
        return m.poll_timer_;
    }
    /// Bind the availability observer for this test; init_subjects() is
    /// idempotent, so a prior call would otherwise leave it unbound here.
    static void rewire_subjects(SpoolmanManager& m) {
        {
            std::lock_guard<std::recursive_mutex> lock(m.mutex_);
            m.print_state_observer_.reset();
            m.spoolman_availability_observer_.reset();
            m.initialized_ = false;
        }
        m.init_subjects();
    }
    static bool cb_open(SpoolmanManager& m) {
        return m.cb_open_;
    }
    static int consecutive_failures(SpoolmanManager& m) {
        return m.consecutive_failures_;
    }

    static void reset(SpoolmanManager& m) {
        // Delete any active timer to avoid leaks between tests
        if (m.poll_timer_ && lv_is_initialized()) {
            lv_timer_delete(m.poll_timer_);
            m.poll_timer_ = nullptr;
        }
        m.poll_refcount_ = 0;
        m.last_refresh_ms_ = 0;
        m.consecutive_failures_ = 0;
        m.cb_tripped_at_ms_ = 0;
        m.cb_open_ = false;
        m.unavailable_notified_ = false;
        m.api_ = nullptr;
    }

    static void set_consecutive_failures(SpoolmanManager& m, int count) {
        m.consecutive_failures_ = count;
    }

    static void set_cb_open(SpoolmanManager& m, bool open) {
        m.cb_open_ = open;
        if (open) {
            m.cb_tripped_at_ms_ = lv_tick_get();
        }
    }

    /// refresh_spoolman_weights() debounces itself; a case that fetches twice
    /// has to step past it.
    static void clear_debounce(SpoolmanManager& m) {
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        m.last_refresh_ms_ = 0;
    }

    /// An id another case left unresolvable is never fetched, and a shutdown
    /// flag another file's teardown latched no-ops every queued answer.
    static void reset_identity(SpoolmanManager& m) {
        SpoolmanManager::s_shutdown_flag.store(false, std::memory_order_release);
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        m.identity_cache_.clear();
        m.identity_unresolvable_.clear();
    }
};

using TA = SpoolmanManagerTestAccess;

// ============================================================================
// LVGL Init (once per translation unit, idempotent)
// ============================================================================

// ============================================================================
// Fixture — reset singleton state between tests (L053)
// ============================================================================

struct SpoolmanFixture : public LVGLTestFixture {
    static bool queue_initialized;

    SpoolmanFixture() {
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }
        TA::reset(SpoolmanManager::instance());
        get_printer_state().init_subjects(false);
    }

    ~SpoolmanFixture() {
        TA::reset(SpoolmanManager::instance());
    }

    /// Set spoolman availability and drain the update queue so the subject
    /// value is visible synchronously (set_spoolman_available uses queue_update).
    void set_spoolman_available(bool available) {
        get_printer_state().capabilities_state().set_spoolman_available(available);
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

bool SpoolmanFixture::queue_initialized = false;

// ============================================================================
// Polling Refcount Tests
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: start increments refcount", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    REQUIRE(TA::poll_refcount(mgr) == 0);

    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 2);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: stop decrements refcount", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    mgr.start_spoolman_polling();
    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 2);

    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: multiple starts and stops balance",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    mgr.start_spoolman_polling();
    mgr.start_spoolman_polling();
    mgr.start_spoolman_polling();

    mgr.stop_spoolman_polling();
    mgr.stop_spoolman_polling();
    mgr.stop_spoolman_polling();

    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: stop below zero clamps at 0", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    // Stop without any prior start
    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);

    // Multiple excess stops
    mgr.stop_spoolman_polling();
    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: start after full stop restarts cleanly",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    mgr.start_spoolman_polling();
    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);

    // Restart — refcount goes from 0 back to 1
    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);
}

// ============================================================================
// Circuit Breaker Tests
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: reset clears all circuit breaker state",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    // Dirty up the state
    TA::set_consecutive_failures(mgr, 5);
    TA::set_cb_open(mgr, true);

    REQUIRE(TA::cb_open(mgr) == true);
    REQUIRE(TA::consecutive_failures(mgr) == 5);

    // Full reset
    TA::reset(mgr);

    REQUIRE(TA::cb_open(mgr) == false);
    REQUIRE(TA::consecutive_failures(mgr) == 0);
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: set_api resets circuit breaker", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    TA::set_consecutive_failures(mgr, 3);
    TA::set_cb_open(mgr, true);

    // set_api(nullptr) calls reset_circuit_breaker internally
    mgr.set_api(nullptr);

    REQUIRE(TA::consecutive_failures(mgr) == 0);
    REQUIRE(TA::cb_open(mgr) == false);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: set_consecutive_failures updates count",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    TA::set_consecutive_failures(mgr, 3);
    REQUIRE(TA::consecutive_failures(mgr) == 3);

    TA::set_consecutive_failures(mgr, 0);
    REQUIRE(TA::consecutive_failures(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: set_cb_open toggles circuit breaker",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    REQUIRE(TA::cb_open(mgr) == false);

    TA::set_cb_open(mgr, true);
    REQUIRE(TA::cb_open(mgr) == true);

    TA::set_cb_open(mgr, false);
    REQUIRE(TA::cb_open(mgr) == false);
}

// ============================================================================
// Spoolman Availability Gating
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture,
                 "SpoolmanManager: start_polling defers until spoolman is available",
                 "[spoolman]") {
    // Wanting to poll and being able to poll arrive in either order, and at boot
    // it is always want-first: panels activate synchronously inside init_ui()
    // while set_spoolman_available() is still sitting in the UpdateQueue. The
    // request is therefore recorded and acted on later, never discarded.
    auto& mgr = SpoolmanManager::instance();
    TA::rewire_subjects(mgr);

    SECTION("a start while unavailable is remembered, not dropped") {
        set_spoolman_available(false);

        mgr.start_spoolman_polling();
        CHECK(TA::poll_refcount(mgr) == 1);    // the wish survives
        CHECK(TA::poll_timer(mgr) == nullptr); // but nothing polls yet
    }

    SECTION("start polls immediately when spoolman is already available") {
        set_spoolman_available(true);

        mgr.start_spoolman_polling();
        CHECK(TA::poll_refcount(mgr) == 1);
        CHECK(TA::poll_timer(mgr) != nullptr);
    }

    SECTION("availability arriving later arms the deferred request on its own") {
        set_spoolman_available(false);
        mgr.start_spoolman_polling();
        REQUIRE(TA::poll_timer(mgr) == nullptr);

        // No second start_spoolman_polling() here on purpose: in production
        // nothing makes that call, which is why the poll never armed at boot.
        set_spoolman_available(true);

        CHECK(TA::poll_refcount(mgr) == 1);
        CHECK(TA::poll_timer(mgr) != nullptr);
    }

    SECTION("losing spoolman stops the timer but keeps the request") {
        set_spoolman_available(true);
        mgr.start_spoolman_polling();
        REQUIRE(TA::poll_timer(mgr) != nullptr);

        set_spoolman_available(false);
        CHECK(TA::poll_timer(mgr) == nullptr);
        // The panel is still up and still wants polling, so a Spoolman that
        // comes back must resume without it having to ask again.
        CHECK(TA::poll_refcount(mgr) == 1);

        set_spoolman_available(true);
        CHECK(TA::poll_timer(mgr) != nullptr);
    }
}

// ============================================================================
// refresh without API
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture,
                 "SpoolmanManager: refresh_spoolman_weights returns early without API",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    // No API set — should return without crash
    REQUIRE_NOTHROW(mgr.refresh_spoolman_weights());
}

// ============================================================================
// SpoolmanOverlay poll-reference discipline (#1159)
// ============================================================================
//
// The overlay applies the persisted sync_enabled setting on every open, and the
// apply took an unmatched poll reference each time — refcount climbed forever and
// the poll timer could never be deleted. These tests pin the ownership contract:
// at most one reference per overlay instance, given back on dismissal.
//
// apply_sync() itself is a lambda inside the async load_from_database() chain, so
// the tests drive set_poll_ref() — the single helper that lambda now calls — plus
// the real public on_deactivate().

class SpoolmanOverlayTestAccess {
  public:
    /// Stand-in for load_from_database()'s apply_sync(enabled)
    static void apply_sync(helix::ui::SpoolmanOverlay& o, bool enabled) {
        o.set_poll_ref(enabled);
    }
    static bool holds_poll_ref(const helix::ui::SpoolmanOverlay& o) {
        return o.holds_poll_ref_;
    }
};

using OverlayTA = SpoolmanOverlayTestAccess;

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanOverlay: repeated opens do not leak poll references",
                 "[spoolman][overlay]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    helix::ui::SpoolmanOverlay overlay;

    // Open #1 — sync enabled, one reference taken
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);
    REQUIRE(OverlayTA::holds_poll_ref(overlay));

    // Dismissal gives it back
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 0);
    REQUIRE_FALSE(OverlayTA::holds_poll_ref(overlay));

    // Open #2 — must not stack a second reference
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanOverlay: repeated sync apply takes one reference",
                 "[spoolman][overlay]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    helix::ui::SpoolmanOverlay overlay;

    // load_from_database()'s key-fallback chain can apply the value more than
    // once per open (new key -> legacy key -> default)
    OverlayTA::apply_sync(overlay, true);
    OverlayTA::apply_sync(overlay, true);
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture,
                 "SpoolmanOverlay: release never steals another holder's reference",
                 "[spoolman][overlay]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    // A panel (HomePanel/AmsPanel/SpoolmanPanel) holds its own reference
    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    helix::ui::SpoolmanOverlay overlay;

    // Overlay opened with sync disabled — it never took a reference, so applying
    // "disabled" and dismissing must leave the panel's reference alone
    OverlayTA::apply_sync(overlay, false);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    // Same for a dismissal that follows a toggle-off
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 2);
    OverlayTA::apply_sync(overlay, false);
    REQUIRE(TA::poll_refcount(mgr) == 1);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 1);
}

// ============================================================================
// The lane's Spoolman record follows every fetch (prestonbrown/helixscreen#1653)
// ============================================================================

using helix::Ad5xIfsTestAccess;
using helix::AmsBackend;
using helix::AmsBackendAd5xIfs;
using helix::AmsBackendMock;
using helix::AmsState;
using helix::SlotInfo;

namespace {

/// A backend that records every slot it is asked to repaint.
class RepaintRecordingBackend : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;

    void repaint_slot_from_lane(int slot_index) override {
        repainted.push_back(slot_index);
        AmsBackendMock::repaint_slot_from_lane(slot_index);
    }

    [[nodiscard]] long repaints_of(int slot_index) const {
        return std::count(repainted.begin(), repainted.end(), slot_index);
    }

    std::vector<int> repainted;
};

/// A colour no backend paints, written into a slot's colour subject so that
/// only a resync can take it back out.
constexpr int kUnsyncedColor = 0x123456;

} // namespace

/// A mock Moonraker behind the manager, and an identity cache no earlier case
/// has touched.
struct SpoolmanLaneFixture : SpoolmanFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;

    SpoolmanLaneFixture() : api(client, get_printer_state()) {
        // Lane records and the external spool binding are process-wide; a case
        // that starts with the previous case's filings reads a filing that
        // reports "unchanged" and skips the work under test.
        helix::ams::reset_lane_sources();
        AmsState::instance().init_subjects(true);
        TA::reset_identity(SpoolmanManager::instance());
        set_spoolman_available(true);
        SpoolmanManager::instance().set_api(&api);
    }

    ~SpoolmanLaneFixture() {
        SpoolmanManager::instance().set_api(nullptr);
        drain();
        TA::reset_identity(SpoolmanManager::instance());
    }

    /// What the server holds for spool @p id. A case states every field it
    /// asserts on here rather than resting on the mock's seed inventory.
    SpoolInfo& server_spool(int id) {
        auto& spools = client.spoolman_mock().get_mock_spools();
        auto it = std::find_if(spools.begin(), spools.end(),
                               [id](const SpoolInfo& s) { return s.id == id; });
        REQUIRE(it != spools.end());
        return *it;
    }

    void remove_server_spool(int id) {
        auto& spools = client.spoolman_mock().get_mock_spools();
        spools.erase(std::remove_if(spools.begin(), spools.end(),
                                    [id](const SpoolInfo& s) { return s.id == id; }),
                     spools.end());
    }

    /// Fetch every linked slot. The mock answers inside the call, and the
    /// answer reaches a lane only when the update queue drains.
    static void fetch() {
        TA::clear_debounce(SpoolmanManager::instance());
        SpoolmanManager::instance().refresh_spoolman_weights();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void poll() {
        fetch();
        drain();
    }

    static void link(AmsBackend& backend, int slot, int spool_id) {
        SlotInfo info = backend.get_slot_info(slot);
        info.spoolman_id = spool_id;
        REQUIRE(backend.sync_external_identity(slot, info).success());
    }
};

namespace {

void state_polymaker_pla(SpoolInfo& spool) {
    spool.vendor = "Polymaker";
    spool.vendor_id = 7;
    spool.material = "PLA";
    spool.filament_name = "PolyTerra Charcoal";
    spool.color_hex = "1A1A2E";
    spool.initial_weight_g = 1000.0;
    spool.remaining_weight_g = 850.0;
}

} // namespace

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a fetch files the spool as the lane's Spoolman record",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    poll();

    const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
    REQUIRE(record.has_value());
    CHECK(record->spoolman_id == 1);
    CHECK(record->spoolman_vendor_id == 7);
    CHECK(record->brand == "Polymaker");
    CHECK(record->material == "PLA");
    CHECK(record->spool_name == "PolyTerra Charcoal");
    CHECK(record->color_rgb == 0x1A1A2EU);
    CHECK(record->total_weight_g == 1000.0F);
    CHECK(record->remaining_weight_g == 850.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a spool edited on the server reaches its lane on the next fetch",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    poll();
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
    // The id is already known, so nothing about this spool is new to the
    // identity cache when the edited record arrives.
    REQUIRE(SpoolmanManager::find_identity(1).has_value());

    server_spool(1).material = "PETG";
    server_spool(1).color_hex = "FF5500";
    poll();

    const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
    REQUIRE(record.has_value());
    CHECK(record->material == "PETG");
    CHECK(record->color_rgb == 0xFF5500U);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager refetches one spool while polling is debounced",
                 "[spoolman][lane][1653]") {
    // The debounce paces the whole-inventory poll. A deliberate read of one
    // spool answers a question the user just asked, so it steps past it.
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    // The debounce clock is the LVGL tick, and a zero tick reads as "never
    // refreshed". Move it off zero so the poll below arms the debounce the
    // targeted read has to step past.
    lv_tick_inc(1000);

    poll();
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman->material == "PLA");

    server_spool(1).material = "PETG";

    // The poll that just ran is holding the debounce down, so a whole-inventory
    // refresh at this moment reaches no slot. Without this the case would pass
    // on a refetch the debounce had swallowed.
    SpoolmanManager::instance().refresh_spoolman_weights();
    drain();
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman->material == "PLA");

    SpoolmanManager::refresh_spool(1);
    drain();

    CHECK(helix::ams::lane_sources(backend.lane(0)).spoolman->material == "PETG");
}

TEST_CASE_METHOD(
    SpoolmanLaneFixture,
    "SpoolmanManager: a slot re-bound while its fetch was in flight takes nothing from it",
    "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    link(*backend, 1, 2);
    state_polymaker_pla(server_spool(1));
    state_polymaker_pla(server_spool(2));

    // The mock answers inside the fetch and the answer waits in the update
    // queue, which is the window a real round trip leaves open.
    fetch();
    link(*backend, 0, 3);
    drain();

    // Both answers ran: slot 1's was filed, and slot 0's reached the identity
    // cache, which it fills ahead of the binding check.
    REQUIRE(helix::ams::lane_sources(backend.lane(1)).spoolman.has_value());
    REQUIRE(SpoolmanManager::find_identity(1).has_value());
    CHECK_FALSE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
}

TEST_CASE_METHOD(SpoolmanLaneFixture, "SpoolmanManager: the weights a fetch files on a lane",
                 "[spoolman][lane][1653]") {
    SECTION("a backend that keeps its own remaining weight still gets Spoolman's") {
        helix::test::RegisteredBackend<AmsBackendMock> backend(2);
        link(*backend, 0, 1);
        state_polymaker_pla(server_spool(1));

        poll();

        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->total_weight_g == 1000.0F);
        CHECK(record->remaining_weight_g == 850.0F);
    }

    SECTION("a spool Spoolman holds no weight for states no weight") {
        // Spoolman serves both weights as null when neither the spool nor its
        // filament has one, and the parser reads null as zero.
        helix::test::RegisteredBackend<AmsBackendMock> backend(2);
        link(*backend, 0, 1);
        SpoolInfo& spool = server_spool(1);
        state_polymaker_pla(spool);
        spool.initial_weight_g = 0.0;
        spool.remaining_weight_g = 0.0;

        poll();

        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->brand == "Polymaker");
        CHECK_FALSE(record->total_weight_g.has_value());
        CHECK_FALSE(record->remaining_weight_g.has_value());
    }
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a fetch files the spool's filament definition id",
                 "[spoolman][lane][1632]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    SECTION("a spool whose filament definition Spoolman knows files it") {
        server_spool(1).filament_id = 55;

        poll();

        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->spoolman_filament_id == 55);
    }

    SECTION("a spool with no filament definition files none") {
        REQUIRE(server_spool(1).filament_id == 0);

        poll();

        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK_FALSE(record->spoolman_filament_id.has_value());
    }
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a linked lane shows Spoolman's weight over the meter's",
                 "[spoolman][lane][1632]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    // The firmware meter files its own number on every frame, before and after
    // each poll: ingest replaces a source's record whole, so a poll that wrote
    // Metered and a meter that writes Metered are two writers of one source.
    const auto file_metered = [&backend](float remaining) {
        helix::ams::Observation metered(helix::ams::ObservationSource::Metered);
        metered.remaining_weight_g = remaining;
        metered.total_weight_g = 1000.0F;
        helix::ams::ingest(backend.lane(0), metered);
    };

    file_metered(400.0F);
    poll();
    file_metered(390.0F);

    const auto shown = helix::ams::resolve(helix::ams::lane_sources(backend.lane(0)));
    CHECK(shown.remaining_weight_g == 850.0F);
    CHECK(shown.total_weight_g == 1000.0F);

    // The server's number moves and the meter keeps filing its own; the lane
    // keeps showing the server's rather than flipping between the two.
    server_spool(1).remaining_weight_g = 700.0;
    poll();
    file_metered(380.0F);

    CHECK(helix::ams::resolve(helix::ams::lane_sources(backend.lane(0))).remaining_weight_g ==
          700.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: an unlinked lane's weight stays the meter's",
                 "[spoolman][lane][1632]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    // The mock seeds every slot with a spoolman id; an unlinked lane has to be
    // made one on purpose.
    link(*backend, 0, 0);
    helix::ams::Observation metered(helix::ams::ObservationSource::Metered);
    metered.remaining_weight_g = 123.0F;
    metered.total_weight_g = 1000.0F;
    helix::ams::ingest(backend.lane(0), metered);

    poll();

    CHECK_FALSE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
    const auto shown = helix::ams::resolve(helix::ams::lane_sources(backend.lane(0)));
    CHECK(shown.remaining_weight_g == 123.0F);
    CHECK(shown.total_weight_g == 1000.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a linked external spool's weight files on the bypass lane",
                 "[spoolman][lane][1632]") {
    // No AMS backend: the external spool is the only lane this poll touches.
    SlotInfo ext;
    ext.spoolman_id = 1;
    ext.material = "PLA";
    ext.remaining_weight_g = 400.0F; // the meter's count, in the raw store
    ext.total_weight_g = 1000.0F;
    AmsState::instance().set_external_spool_info(ext);
    state_polymaker_pla(server_spool(1));

    poll();

    CHECK(helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value());
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->remaining_weight_g == 850.0F);
    CHECK(shown->total_weight_g == 1000.0F);

    // The meter goes on counting in the raw store; the display does not flip
    // to its number between polls.
    SlotInfo metered_raw = *shown;
    metered_raw.remaining_weight_g = 390.0F;
    AmsState::instance().set_external_spool_info_in_memory(metered_raw);
    poll();

    shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->remaining_weight_g == 850.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a targeted refresh re-reads a linked external spool",
                 "[spoolman][lane][1632]") {
    SlotInfo ext;
    ext.spoolman_id = 1;
    ext.material = "PLA";
    ext.remaining_weight_g = 400.0F;
    ext.total_weight_g = 1000.0F;
    AmsState::instance().set_external_spool_info(ext);
    state_polymaker_pla(server_spool(1));

    lv_tick_inc(1000);
    poll();
    REQUIRE(AmsState::instance().get_external_spool_info()->remaining_weight_g == 850.0F);

    // The user's weight edit reached the server.
    server_spool(1).remaining_weight_g = 90.0;

    // The poll that just ran holds the debounce down, so only the targeted
    // read can carry the server's new number to the lane.
    SpoolmanManager::instance().refresh_spoolman_weights();
    drain();
    REQUIRE(AmsState::instance().get_external_spool_info()->remaining_weight_g == 850.0F);

    SpoolmanManager::refresh_spool(1);
    drain();

    CHECK(AmsState::instance().get_external_spool_info()->remaining_weight_g == 90.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: an external spool fetch moves the stored binding with it",
                 "[spoolman][lane][1632]") {
    SlotInfo ext;
    ext.spoolman_id = 1;
    ext.material = "PLA";
    ext.remaining_weight_g = 400.0F; // the link-time stub
    ext.total_weight_g = 1000.0F;
    AmsState::instance().set_external_spool_info(ext);
    state_polymaker_pla(server_spool(1));

    poll();

    // The raw record is what a restart (or a server that stays down) reads
    // before any poll answers: it carries the fetched weight and identity,
    // not the link-time stub.
    auto raw = AmsState::instance().raw_external_spool_info();
    REQUIRE(raw.has_value());
    CHECK(raw->remaining_weight_g == 850.0F);
    CHECK(raw->brand == "Polymaker");
    CHECK(raw->material == "PLA");
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a spool Spoolman denies takes the bypass lane's record with it",
                 "[spoolman][lane][1632]") {
    SlotInfo ext;
    ext.spoolman_id = 1;
    ext.material = "PLA";
    ext.remaining_weight_g = 400.0F;
    ext.total_weight_g = 1000.0F;
    AmsState::instance().set_external_spool_info(ext);
    state_polymaker_pla(server_spool(1));

    poll();
    REQUIRE(helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value());

    remove_server_spool(1);
    poll();

    CHECK_FALSE(helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value());
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    // The first poll moved the stored binding to the fetched weight, so the
    // display falls back to that, not to the link-time stub.
    CHECK(shown->remaining_weight_g == 850.0F);
    CHECK(shown->brand == "Polymaker");
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a denied external spool keeps its freshest identity",
                 "[spoolman][lane][1632]") {
    SlotInfo ext;
    ext.spoolman_id = 1;
    ext.material = "PLA"; // the link-time stub
    ext.remaining_weight_g = 400.0F;
    ext.total_weight_g = 1000.0F;
    AmsState::instance().set_external_spool_info(ext);
    state_polymaker_pla(server_spool(1));

    poll();

    // The server edits the spool to PETG and the next poll carries that; the
    // delete that follows must keep the PETG view, not revert to the stub.
    server_spool(1).material = "PETG";
    poll();
    REQUIRE(helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value());

    remove_server_spool(1);
    poll();

    auto sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    CHECK_FALSE(sources.spoolman.has_value());
    // A delete is bookkeeping, not a spool change: what the resolved view
    // carried goes back on the lane as remembered, the way a denied slot's
    // does.
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->material == "PETG");
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->material == "PETG");
    CHECK(shown->brand == "Polymaker");
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a denial in flight does not touch a newer binding",
                 "[spoolman][lane][1632]") {
    SlotInfo ext;
    ext.spoolman_id = 1;
    ext.material = "PLA";
    ext.remaining_weight_g = 400.0F;
    ext.total_weight_g = 1000.0F;
    AmsState::instance().set_external_spool_info(ext);
    state_polymaker_pla(server_spool(1));

    poll();
    REQUIRE(helix::ams::lane_sources(helix::ams::kBypassLaneId).spoolman.has_value());

    remove_server_spool(1);
    fetch(); // the mock answers inside the call; the denial is queued, not run

    // Another client rebinds the bypass to spool 2 and its own fetch files
    // before the denial drains.
    SlotInfo ext2;
    ext2.spoolman_id = 2;
    ext2.material = "ABS";
    ext2.remaining_weight_g = 600.0F;
    ext2.total_weight_g = 750.0F;
    AmsState::instance().set_external_spool_info(ext2);
    SpoolInfo& s2 = server_spool(2);
    s2.vendor = "Prusa";
    s2.material = "ABS";
    s2.initial_weight_g = 750.0;
    s2.remaining_weight_g = 600.0;
    REQUIRE(SpoolmanManager::file_spool_on_lane(helix::ams::kBypassLaneId, s2));

    drain(); // the denial for spool 1 runs now

    auto sources = helix::ams::lane_sources(helix::ams::kBypassLaneId);
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 2);
    CHECK_FALSE(sources.remembered.has_value());
    auto shown = AmsState::instance().get_external_spool_info();
    REQUIRE(shown.has_value());
    CHECK(shown->spoolman_id == 2);
    CHECK(shown->remaining_weight_g == 600.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a linked lane's catalog pick survives a fetch of its spool",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    // What a backend's start reloads: the identity the spool had when it was
    // last fetched, beside the product the person picked for it.
    helix::ams::FilamentSlotOverride stored;
    stored.spoolman_id = 1;
    stored.brand = "Stored Brand";
    stored.material = "PETG";
    stored.color_rgb = 0xFF0000;
    stored.color_set = true;
    stored.catalog_id = "polymaker-polyterra-pla-charcoal";
    stored.product_name = "PolyTerra PLA Charcoal";
    helix::test::file_override_as_lane_records(*backend, 0, stored);

    SpoolmanManager::file_spool_on_lane(backend.lane(0), server_spool(1));

    const auto shown = helix::ams::resolve(helix::ams::lane_sources(backend.lane(0)));
    CHECK(shown.catalog_id == "polymaker-polyterra-pla-charcoal");
    CHECK(shown.product_name == "PolyTerra PLA Charcoal");
    CHECK(shown.brand == "Polymaker");
    CHECK(shown.material == "PLA");
    CHECK(shown.color_rgb == 0x1A1A2EU);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a field the server stops stating stops outranking the lane's "
                 "other sources",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    SpoolInfo& spool = server_spool(1);
    state_polymaker_pla(spool);

    helix::ams::Observation firmware(helix::ams::ObservationSource::VendorCache);
    firmware.brand = "Firmware Brand";
    firmware.spool_name = "Firmware Name";
    helix::ams::ingest(backend.lane(0), firmware);

    poll();
    REQUIRE(helix::ams::resolve(helix::ams::lane_sources(backend.lane(0))).brand == "Polymaker");

    // The vendor is removed on the server and the filament loses its name.
    // An empty field is Spoolman saying nothing about it, not a blank brand.
    spool.vendor.clear();
    spool.vendor_id = 0;
    spool.filament_name.clear();
    poll();

    const auto sources = helix::ams::lane_sources(backend.lane(0));
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->material == "PLA");
    CHECK_FALSE(sources.spoolman->brand.has_value());
    CHECK_FALSE(sources.spoolman->spoolman_vendor_id.has_value());
    CHECK_FALSE(sources.spoolman->spool_name.has_value());

    const auto shown = helix::ams::resolve(sources);
    CHECK(shown.brand == "Firmware Brand");
    CHECK(shown.spool_name == "Firmware Name");
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a spool Spoolman denies is re-filed as remembered; an "
                 "unreachable Spoolman leaves the record standing",
                 "[spoolman][lane][1653][1672]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    link(*backend, 1, 2);

    // The record a backend's start filed from its stored override.
    helix::ams::Observation cached(helix::ams::ObservationSource::Spoolman);
    cached.spoolman_id = 1;
    cached.brand = "Cached Brand";
    helix::ams::ingest(backend.lane(0), cached);
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());

    SECTION("not found stops tracking the spool and keeps what the slot showed") {
        // A caching backend's struct carries the identity its frames painted
        // while the record stood; paint the mock's slot the same way.
        SlotInfo painted = backend->get_slot_info(0);
        painted.brand = "Cached Brand";
        painted.material = "PLA";
        REQUIRE(backend->sync_external_identity(0, painted).success());

        remove_server_spool(1);
        poll();

        REQUIRE(SpoolmanManager::is_identity_unresolvable(1));
        const helix::ams::LaneSources sources = helix::ams::lane_sources(backend.lane(0));
        CHECK_FALSE(sources.spoolman.has_value());
        // The lane shows what is loaded: the identity the record supplied is
        // re-filed as remembered, the way an unlink that kept it files it.
        REQUIRE(sources.remembered.has_value());
        CHECK(sources.remembered->brand == "Cached Brand");
        CHECK(helix::ams::resolve(sources).brand == "Cached Brand");
    }

    SECTION("not found for a slot re-bound meanwhile leaves it alone") {
        remove_server_spool(1);
        fetch();
        link(*backend, 0, 3);
        drain();

        REQUIRE(SpoolmanManager::is_identity_unresolvable(1));
        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->brand == "Cached Brand");
    }

    SECTION("an unreachable Spoolman leaves it standing") {
        client.set_mock_spoolman_enabled(false);
        poll();

        // Both linked slots' fetches failed, and each failure was counted.
        REQUIRE(TA::consecutive_failures(SpoolmanManager::instance()) == 2);
        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->brand == "Cached Brand");
    }
}

// ============================================================================
// A backend that serves its slots from a cache shows a filing at once
// (prestonbrown/helixscreen#1653)
// ============================================================================

namespace {

/// An AD5X port whose firmware reports a PLA spool in 898989, so the lane's
/// vendor cache states a colour of its own.
void seat_firmware_spool(AmsBackendAd5xIfs& ad5x) {
    Ad5xIfsTestAccess::set_port_presence(ad5x, 0, true);
    Ad5xIfsTestAccess::set_material(ad5x, 0, "PLA");
    Ad5xIfsTestAccess::set_color(ad5x, 0, "898989");
}

/// The firmware restating the port unchanged, which re-reads it and paints the
/// cached slot from the lane as every frame does.
void restate_firmware_spool(AmsBackendAd5xIfs& ad5x) {
    Ad5xIfsTestAccess::set_color(ad5x, 0, "898989");
}

} // namespace

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: an AD5X slot shows a spool edited on the server as soon as its "
                 "fetch lands",
                 "[spoolman][lane][ad5x_ifs][1653]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> ad5x(nullptr, nullptr);
    seat_firmware_spool(*ad5x);
    link(*ad5x, 0, 1);
    state_polymaker_pla(server_spool(1));
    poll();
    restate_firmware_spool(*ad5x);
    REQUIRE(ad5x->get_slot_info(0).color_rgb == 0x1A1A2EU);

    drain();
    lv_subject_t* shown = AmsState::instance().get_slot_color_subject(0);
    REQUIRE(shown != nullptr);
    lv_subject_set_int(shown, kUnsyncedColor);

    // The weights stay put, so the answer changes nothing on the slot but what
    // the lane now resolves, and no backend event resyncs the subject.
    server_spool(1).color_hex = "FF5500";
    poll();

    CHECK(ad5x->get_slot_info(0).color_rgb == 0xFF5500U);
    CHECK(lv_subject_get_int(shown) == 0xFF5500);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: an AD5X slot stops showing a spool Spoolman denies as soon as "
                 "the answer lands",
                 "[spoolman][lane][ad5x_ifs][1653]") {
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> ad5x(nullptr, nullptr);
    seat_firmware_spool(*ad5x);
    link(*ad5x, 0, 1);

    // The record a backend's start filed from its stored override, painted by
    // the next frame.
    helix::ams::Observation cached(helix::ams::ObservationSource::Spoolman);
    cached.spoolman_id = 1;
    cached.color_rgb = 0x1A1A2EU;
    helix::ams::ingest(ad5x.lane(0), cached);
    restate_firmware_spool(*ad5x);
    REQUIRE(ad5x->get_slot_info(0).color_rgb == 0x1A1A2EU);

    drain();
    lv_subject_t* shown = AmsState::instance().get_slot_color_subject(0);
    REQUIRE(shown != nullptr);
    lv_subject_set_int(shown, kUnsyncedColor);

    remove_server_spool(1);
    poll();

    REQUIRE_FALSE(helix::ams::lane_sources(ad5x.lane(0)).spoolman.has_value());
    // Colour, because the firmware states it too. A cached slot keeps a field no
    // remaining source states (#1672), so such a field cannot show a repaint.
    CHECK(ad5x->get_slot_info(0).color_rgb == 0x898989U);
    CHECK(lv_subject_get_int(shown) == 0x898989);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a fetch that changes nothing on the lane neither repaints nor "
                 "resyncs the slot",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<RepaintRecordingBackend> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));
    poll();
    REQUIRE(backend->repaints_of(0) == 1);

    drain();
    lv_subject_t* shown = AmsState::instance().get_slot_color_subject(0);
    REQUIRE(shown != nullptr);
    lv_subject_set_int(shown, kUnsyncedColor);
    // Only an answer clears the failure count, so a cleared count shows the
    // second fetch was answered.
    TA::set_consecutive_failures(SpoolmanManager::instance(), 1);

    poll();

    REQUIRE(TA::consecutive_failures(SpoolmanManager::instance()) == 0);
    CHECK(backend->repaints_of(0) == 1);
    CHECK(lv_subject_get_int(shown) == kUnsyncedColor);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a spool Spoolman denies on a lane holding no record of it "
                 "neither repaints nor resyncs the slot",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<RepaintRecordingBackend> backend(2);
    link(*backend, 0, 1);
    REQUIRE_FALSE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());

    drain();
    lv_subject_t* shown = AmsState::instance().get_slot_color_subject(0);
    REQUIRE(shown != nullptr);
    lv_subject_set_int(shown, kUnsyncedColor);

    remove_server_spool(1);
    poll();

    // The denial was answered and reached a slot still bound to the spool.
    REQUIRE(SpoolmanManager::is_identity_unresolvable(1));
    REQUIRE(backend->get_slot_info(0).spoolman_id == 1);
    CHECK(backend->repaints_of(0) == 0);
    CHECK(lv_subject_get_int(shown) == kUnsyncedColor);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a weight the poll files on a tool changer reaches the slot's "
                 "subjects in the same pass",
                 "[spoolman][toolchanger][slot_refresh]") {
    helix::test::RegisteredBackend<helix::AmsBackendToolChanger> backend(nullptr, nullptr);
    backend->set_discovered_tools({"T0", "T1"});
    AmsState& ams = AmsState::instance();
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    // A fetch that changes the lane's Spoolman record repaints and resyncs the
    // slot on its own. The poll this case pins finds the record unchanged.
    poll();
    REQUIRE(backend->get_slot_info(0).remaining_weight_g == 850.0F);

    // The slot's weight moves away from Spoolman's in memory alone, and the
    // subjects show it.
    SlotInfo moved = backend->get_slot_info(0);
    moved.remaining_weight_g = 1000.0F;
    REQUIRE(backend->sync_external_identity(0, moved).success());
    ams.sync_from_backend();
    drain();
    REQUIRE(lv_subject_get_int(ams.get_slot_fill_subject(0)) == 100);

    // The server's number moves. The mock answers inside the fetch. One pass of
    // the update queue runs that answer and nothing the answer queues in turn,
    // so what the subjects show afterwards is the poll's own refresh.
    server_spool(1).remaining_weight_g = 700.0;
    fetch();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(backend->get_slot_info(0).remaining_weight_g == 700.0F);

    CHECK(lv_subject_get_int(ams.get_slot_fill_subject(0)) == 70);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: weight polls on a linked tool changer slot leave the saved "
                 "spool assignments alone",
                 "[spoolman][toolchanger][slot_refresh][tool-state]") {
    // AmsState hands ToolState the API it saves through. A mock of its own counts
    // those saves and nothing the Spoolman fetches do.
    MoonrakerAPIMock tool_api(client, get_printer_state());
    struct AmsApiScope {
        ~AmsApiScope() {
            AmsState::instance().set_moonraker_api(nullptr);
        }
    } ams_api_scope;
    AmsState::instance().set_moonraker_api(&tool_api);

    // tool_spools.json lands in a directory this case owns.
    struct SpoolJsonScope {
        std::string previous_dir = helix::ToolState::instance().get_config_dir();
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("helix-spool-json-" + std::to_string(::getpid()));
        ~SpoolJsonScope() {
            helix::ToolState::instance().deinit_subjects();
            helix::ToolState::instance().set_config_dir(previous_dir);
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } spool_json;

    auto& ts = helix::ToolState::instance();
    ts.set_config_dir(spool_json.dir.string());
    ts.deinit_subjects();
    ts.init_subjects(false);
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolchanger", "tool T0", "tool T1", "extruder",
                                            "extruder1", "heater_bed", "gcode_move"}));
    ts.init_tools(hw);
    REQUIRE(ts.tool_count() == 2);

    helix::test::RegisteredBackend<helix::AmsBackendToolChanger> backend(nullptr, nullptr);
    backend->set_discovered_tools({"T0", "T1"});
    AmsState& ams = AmsState::instance();
    REQUIRE_FALSE(backend->has_firmware_spool_persistence());
    REQUIRE(backend->get_slot_info(0).mapped_tool == 0);

    // Link tool 0's slot and let the assignment reach ToolState and its store.
    link(*backend, 0, 1);
    ams.sync_from_backend();
    drain();
    REQUIRE(ts.tools()[0].spoolman_id == 1);
    REQUIRE(client.call_count("server.database.post_item") > 0); // the full sync's own save
    state_polymaker_pla(server_spool(1));
    poll();
    REQUIRE_FALSE(helix::ToolStateTestAccess::spool_dirty(ts));
    const int saves_after_link = client.call_count("server.database.post_item");
    REQUIRE(saves_after_link > 0);

    // A print draining the spool: each poll finds less on the server.
    for (int tick = 1; tick <= 3; ++tick) {
        const double remaining = 850.0 - 10.0 * tick;
        server_spool(1).remaining_weight_g = remaining;
        poll();
        // No full sync runs in this loop, and only update_slot writes the
        // fill subject outside one, so each poll reached update_slot.
        REQUIRE(backend->get_slot_info(0).remaining_weight_g == static_cast<float>(remaining));
        REQUIRE(lv_subject_get_int(ams.get_slot_fill_subject(0)) ==
                backend->get_slot_info(0).display_fill_pct());
    }

    CHECK(client.call_count("server.database.post_item") == saves_after_link);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "a changed Spoolman identity amends the linked lane's stored record",
                 "[spoolman][lane][1653]") {
    // Without the amend the lane shows the server's identity while the record a
    // restart reloads still holds the old one.
    helix::test::RegisteredBackend<AmsBackendAd5xIfs> backend(&api, nullptr);
    helix::ams::FilamentSlotOverride stored;
    stored.spoolman_id = 1;
    stored.brand = "Polymaker";
    stored.material = "PLA";
    stored.color_rgb = 0xBCBCBC;
    stored.color_set = true;
    stored.declared = helix::ams::declared_fields_from_names(nlohmann::json::array({"color_rgb"}));
    Ad5xIfsTestAccess::seed_override(*backend, 0, stored);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));
    poll();

    // The spool is edited on the server.
    server_spool(1).material = "PETG";
    server_spool(1).vendor = "Sunlu";
    poll();

    const auto record = Ad5xIfsTestAccess::get_override(*backend, 0);
    REQUIRE(record.has_value());
    CHECK(record->material == "PETG");
    CHECK(record->brand == "Sunlu");
    // The colour is the user's, and the server does not get to move it.
    CHECK(record->color_rgb == 0xBCBCBCu);
    CHECK(helix::ams::declares_color(*record));

    // What a restart would reload now resolves to what the lane shows.
    const helix::ams::LaneSources reload = helix::ams::sources_from_record(
        *record, helix::ams::to_lane_data_record(0, *record), helix::ams::LegacyLockKeys::LaneData);
    CHECK(helix::ams::resolve(reload).material == std::string("PETG"));
    CHECK(helix::ams::resolve(reload).brand == std::string("Sunlu"));
    CHECK(helix::ams::resolve(reload).color_rgb == 0xBCBCBCu);
}

// A Spoolman fetch that times out is an ordinary network failure: the error
// callback reports it and nothing more. In particular no exception may reach
// the UpdateQueue drain — a throw there means some queued callback blew up,
// which reads in the log as an error with no producer.
TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a failed external-spool fetch reaches no queued callback",
                 "[spoolman]") {
    SlotInfo ext;
    ext.spoolman_id = 135;
    ext.material = "PLA";
    AmsState::instance().set_external_spool_info_in_memory(ext);

    const auto exceptions_before = helix::ui::UpdateQueueTestAccess::callback_exception_count();
    client.set_mock_spoolman_enabled(false);

    helix::TextLogCapture capture;
    poll();

    // The error path ran and reported where the fetch failed.
    CHECK(capture.contains("Failed to fetch external spool Spoolman #135"));
    // And it threw nothing into the drain.
    CHECK(helix::ui::UpdateQueueTestAccess::callback_exception_count() == exceptions_before);

    AmsState::instance().clear_external_spool_info();
}
