// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_open_trace.h"

#include "../catch_amalgamated.hpp"

using helix::nav::OverlayOpenTracker;

namespace {
int g_root_a = 0;
int g_root_b = 0;
} // namespace

TEST_CASE("OverlayOpenTracker times an open from request to the first frame after its push",
          "[overlay_open_trace]") {
    OverlayOpenTracker t;
    t.requested(&g_root_a, "Spoolman", true, 100.0);
    CHECK(t.awaiting_frame());

    // The push is queued: a frame drawn before it runs is still the old screen.
    t.frame_drawn(110.0);
    REQUIRE(t.records().size() == 1);
    CHECK(t.records()[0].opens == 0);

    t.pushed(&g_root_a);
    t.frame_drawn(145.0);
    CHECK_FALSE(t.awaiting_frame());
    const auto& r = t.records()[0];
    CHECK(r.name == "Spoolman");
    CHECK(r.opens == 1);
    CHECK(r.builds == 1);
    CHECK(r.last_ms == Catch::Approx(45.0));

    // A reopen on the retained tree, faster: max keeps the first, total sums both.
    t.requested(&g_root_a, "Spoolman", false, 1000.0);
    t.pushed(&g_root_a);
    t.frame_drawn(1020.0);
    CHECK(r.opens == 2);
    CHECK(r.builds == 1);
    CHECK(r.last_ms == Catch::Approx(20.0));
    CHECK(r.max_ms == Catch::Approx(45.0));
    CHECK(r.total_ms == Catch::Approx(65.0));
}

TEST_CASE("OverlayOpenTracker edge cases", "[overlay_open_trace]") {
    OverlayOpenTracker t;

    SECTION("two overlays stacked in one pass both end at the frame that shows them") {
        t.requested(&g_root_a, "A", true, 0.0);
        t.requested(&g_root_b, "B", true, 5.0);
        t.pushed(&g_root_a);
        t.pushed(&g_root_b);
        t.frame_drawn(30.0);
        REQUIRE(t.records().size() == 2);
        CHECK(t.records()[0].last_ms == Catch::Approx(30.0));
        CHECK(t.records()[1].last_ms == Catch::Approx(25.0));
    }

    SECTION("a second request for the same root replaces the first") {
        t.requested(&g_root_a, "A", true, 0.0);
        t.requested(&g_root_a, "A", false, 50.0);
        t.pushed(&g_root_a);
        t.frame_drawn(60.0);
        CHECK(t.records()[0].opens == 1);
        CHECK(t.records()[0].builds == 1);
        CHECK(t.records()[0].last_ms == Catch::Approx(10.0));
    }

    SECTION("an open with no frame for too long goes stale, pushed or not") {
        t.requested(&g_root_a, "A", true, 0.0);
        t.requested(&g_root_b, "B", true, 0.0);
        t.pushed(&g_root_b); // a push that ran while the display slept
        t.frame_drawn(OverlayOpenTracker::kStaleMs + 1.0);
        CHECK_FALSE(t.awaiting_frame());
        CHECK(t.records()[0].opens == 0);
        CHECK(t.records()[1].opens == 0);
    }

    SECTION("a push for a root nobody requested is ignored") {
        t.pushed(&g_root_a);
        t.frame_drawn(10.0);
        CHECK(t.records().empty());
    }

    SECTION("more requests than pending slots drop the oldest") {
        int roots[OverlayOpenTracker::kMaxPending + 1] = {};
        for (auto& root : roots) {
            t.requested(&root, "X", true, 0.0);
            t.pushed(&root);
        }
        t.frame_drawn(10.0);
        CHECK(t.records()[0].opens == OverlayOpenTracker::kMaxPending);
        CHECK(t.records()[0].builds == OverlayOpenTracker::kMaxPending + 1);
    }

    SECTION("a null name is recorded rather than dereferenced") {
        t.requested(&g_root_a, nullptr, true, 0.0);
        t.pushed(&g_root_a);
        t.frame_drawn(1.0);
        CHECK(t.records()[0].name == "?");
    }
}
