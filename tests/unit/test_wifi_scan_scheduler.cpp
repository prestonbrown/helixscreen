// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/wifi_scan_scheduler.h"

#include "../catch_amalgamated.hpp"

using helix::wifi::ScanScheduler;

// ScanScheduler is a pure state machine — no LVGL, no timers, no backend —
// so these tests exercise it directly with no fixture needed.

TEST_CASE("ScanScheduler starts ready to trigger", "[wifi][scan_scheduler]") {
    ScanScheduler sched;
    REQUIRE(sched.should_trigger());
    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.next_interval_ms() == ScanScheduler::BASE_INTERVAL_MS);
}

TEST_CASE("ScanScheduler blocks a second trigger while a scan is outstanding",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;
    sched.on_scan_started();
    REQUIRE_FALSE(sched.should_trigger());

    // Still outstanding — a second check must also refuse.
    REQUIRE_FALSE(sched.should_trigger());

    sched.on_scan_complete(3, false);
    REQUIRE(sched.should_trigger());
}

TEST_CASE("ScanScheduler grows the interval 10s -> 20s -> 30s and caps", "[wifi][scan_scheduler]") {
    ScanScheduler sched;
    REQUIRE(sched.next_interval_ms() == 10000);

    // Baseline reading — nothing to compare against yet, stays at base.
    sched.on_scan_started();
    sched.on_scan_complete(5, false);
    REQUIRE(sched.next_interval_ms() == 10000);

    // First repeat of the same count.
    sched.on_scan_started();
    sched.on_scan_complete(5, false);
    REQUIRE(sched.next_interval_ms() == 20000);

    // Second repeat.
    sched.on_scan_started();
    sched.on_scan_complete(5, false);
    REQUIRE(sched.next_interval_ms() == 30000);

    // Third repeat — capped, does not exceed MAX_INTERVAL_MS.
    sched.on_scan_started();
    sched.on_scan_complete(5, false);
    REQUIRE(sched.next_interval_ms() == ScanScheduler::MAX_INTERVAL_MS);
    REQUIRE(sched.next_interval_ms() == 30000);
}

TEST_CASE("ScanScheduler suppresses after an unchanged count twice while connected",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    // Baseline.
    sched.on_scan_started();
    sched.on_scan_complete(4, true);
    REQUIRE_FALSE(sched.suppressed());

    // First unchanged repeat — not suppressed yet.
    sched.on_scan_started();
    sched.on_scan_complete(4, true);
    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.should_trigger());

    // Second unchanged repeat — now suppressed.
    sched.on_scan_started();
    sched.on_scan_complete(4, true);
    REQUIRE(sched.suppressed());
    REQUIRE_FALSE(sched.should_trigger());
}

TEST_CASE("ScanScheduler does not suppress an unchanged count while disconnected",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    sched.on_scan_started();
    sched.on_scan_complete(4, false);
    sched.on_scan_started();
    sched.on_scan_complete(4, false);
    sched.on_scan_started();
    sched.on_scan_complete(4, false);

    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.should_trigger());
}

TEST_CASE("ScanScheduler on_user_refresh clears suppression and resets the interval",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    // Drive it into suppression.
    sched.on_scan_started();
    sched.on_scan_complete(7, true);
    sched.on_scan_started();
    sched.on_scan_complete(7, true);
    sched.on_scan_started();
    sched.on_scan_complete(7, true);
    REQUIRE(sched.suppressed());
    REQUIRE(sched.next_interval_ms() == 30000);

    sched.on_user_refresh();
    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.next_interval_ms() == ScanScheduler::BASE_INTERVAL_MS);
    REQUIRE(sched.should_trigger());
}

TEST_CASE("ScanScheduler on_disconnected clears suppression and resets the interval",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    sched.on_scan_started();
    sched.on_scan_complete(2, true);
    sched.on_scan_started();
    sched.on_scan_complete(2, true);
    sched.on_scan_started();
    sched.on_scan_complete(2, true);
    REQUIRE(sched.suppressed());

    sched.on_disconnected();
    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.next_interval_ms() == ScanScheduler::BASE_INTERVAL_MS);
    REQUIRE(sched.should_trigger());
}

TEST_CASE("ScanScheduler resets the interval on a changed count without suppressing",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    // Baseline + one repeat to grow the interval, but not enough to suppress.
    sched.on_scan_started();
    sched.on_scan_complete(1, true);
    sched.on_scan_started();
    sched.on_scan_complete(1, true);
    REQUIRE(sched.next_interval_ms() == 20000);
    REQUIRE_FALSE(sched.suppressed());

    // A changed count resets the interval...
    sched.on_scan_started();
    sched.on_scan_complete(9, true);
    REQUIRE(sched.next_interval_ms() == ScanScheduler::BASE_INTERVAL_MS);
    // ...and does not suppress.
    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.should_trigger());
}

// A failed scan trigger (or a trigger that succeeded but whose results
// couldn't be fetched) carries no information about whether the network
// environment is stable. Folding it into on_scan_complete(0, connected)
// made repeated failures indistinguishable from repeated genuinely-unchanged
// scans, so three failed triggers while connected would permanently
// suppress scanning — exactly backwards for a broken control socket, which
// is precisely the case a user is troubleshooting on the network settings
// page. on_scan_failed() must never drive suppression or the backoff.

TEST_CASE("ScanScheduler on_scan_failed clears only the outstanding flag",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;
    sched.on_scan_started();
    REQUIRE_FALSE(sched.should_trigger());

    sched.on_scan_failed();
    REQUIRE(sched.should_trigger());
    REQUIRE_FALSE(sched.suppressed());
    REQUIRE(sched.next_interval_ms() == ScanScheduler::BASE_INTERVAL_MS);
}

TEST_CASE("ScanScheduler never suppresses from repeated on_scan_failed() while connected",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    // Many consecutive failed attempts while connected — a wedged control
    // socket on an AP the station is still associated to. This must not
    // ever look like "results have gone stable" and must not permanently
    // stop scanning.
    for (int i = 0; i < 10; ++i) {
        sched.on_scan_started();
        sched.on_scan_failed();
        REQUIRE_FALSE(sched.suppressed());
        REQUIRE(sched.should_trigger());
    }

    // The interval must not have grown either — a failure isn't evidence
    // the environment is stable, so it must not feed the backoff.
    REQUIRE(sched.next_interval_ms() == ScanScheduler::BASE_INTERVAL_MS);
}

TEST_CASE("ScanScheduler on_scan_failed does not advance unchanged_streak_",
          "[wifi][scan_scheduler]") {
    ScanScheduler sched;

    // One real unchanged result (streak=1, not yet suppressed)...
    sched.on_scan_started();
    sched.on_scan_complete(4, true);
    sched.on_scan_started();
    sched.on_scan_complete(4, true);
    REQUIRE_FALSE(sched.suppressed());

    // ...then failures interleaved — if on_scan_failed() advanced the streak
    // (e.g. by comparing against last_count_ as if 0 results were seen),
    // this would tip it over into suppression. It must not.
    sched.on_scan_started();
    sched.on_scan_failed();
    sched.on_scan_started();
    sched.on_scan_failed();
    sched.on_scan_started();
    sched.on_scan_failed();
    REQUIRE_FALSE(sched.suppressed());

    // A real unchanged result picks the streak back up right where it left
    // off (this one tips it to suppression), proving the failures in
    // between were true no-ops rather than resetting anything either.
    sched.on_scan_started();
    sched.on_scan_complete(4, true);
    REQUIRE(sched.suppressed());
}
