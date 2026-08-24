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
#include "ui_update_queue.h"

#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "printer_state.h"
#include "spoolman_manager.h"

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
};

using TA = SpoolmanManagerTestAccess;

// ============================================================================
// LVGL Init (once per translation unit, idempotent)
// ============================================================================

namespace {
struct LVGLInitializerSpoolman {
    LVGLInitializerSpoolman() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};
static LVGLInitializerSpoolman lvgl_init;
} // namespace

// ============================================================================
// Fixture — reset singleton state between tests (L053)
// ============================================================================

struct SpoolmanFixture {
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
        get_printer_state().set_spoolman_available(available);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
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
    overlay.on_deactivate();
    REQUIRE(TA::poll_refcount(mgr) == 0);
    REQUIRE_FALSE(OverlayTA::holds_poll_ref(overlay));

    // Open #2 — must not stack a second reference
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    overlay.on_deactivate();
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

    overlay.on_deactivate();
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
    overlay.on_deactivate();
    overlay.on_deactivate();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    // Same for a dismissal that follows a toggle-off
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 2);
    OverlayTA::apply_sync(overlay, false);
    REQUIRE(TA::poll_refcount(mgr) == 1);
    overlay.on_deactivate();
    REQUIRE(TA::poll_refcount(mgr) == 1);
}
