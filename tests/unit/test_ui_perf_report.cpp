// SPDX-License-Identifier: GPL-3.0-or-later

#include "system/debug_bundle_collector.h"
#include "ui_overlay_open_trace.h"
#include "ui_perf_report.h"

#include "hv/json.hpp"

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
}

TEST_CASE("the bundle's ui_perf section carries the provider's snapshot, and only that",
          "[overlay_open_trace][debug-bundle]") {
    helix::BundleOptions options;
    CHECK_FALSE(helix::DebugBundleCollector::collect(options).contains("ui_perf"));

    helix::UiPerfSnapshot snapshot;
    snapshot.captured = true;
    snapshot.sample.frames = 4;
    snapshot.sample.render_ns = 4 * 25'000'000ULL;
    snapshot.sample.peak_rss_kb = 4096;
    helix::nav::OverlayOpenRecord fast{"Fast", 2, 1, 10.0, 12.0, 20.0};
    helix::nav::OverlayOpenRecord slow{"Spoolman", 3, 3, 250.0, 300.0, 750.0};
    snapshot.overlays = {fast, slow};
    options.ui_perf_json = helix::ui_perf_to_json(snapshot).dump();
    const nlohmann::json perf = helix::DebugBundleCollector::collect(options)["ui_perf"];
    CHECK(perf["frames"]["drawn"] == 4);
    CHECK(perf["frames"]["render_ms_mean"] == 25.0);
    CHECK(perf["peak_rss_kb"] == 4096);
    REQUIRE(perf["overlays"].size() == 2);
    CHECK(perf["overlays"][0]["name"] == "Spoolman"); // slowest last open first
    CHECK(perf["overlays"][0]["mean_ms"] == 250.0);
    CHECK(perf["overlays"][0]["builds"] == 3);
}
