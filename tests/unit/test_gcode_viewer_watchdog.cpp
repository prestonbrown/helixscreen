// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Unit tests for the gcode-viewer renderer-stall watchdog decision logic.
//
// The watchdog self-heals a dropped lv_obj_invalidate() by force-invalidating
// when the solid cache is behind the print's target layer and not advancing.
// Under a persistent EXTERNAL failure (disk full -> layer load fails -> cache
// stuck), the original logic kicked forever (4000+ kicks / ~2h in bundle
// YZQ47HQ6). watchdog_evaluate() adds a consecutive-stall cap so it gives up
// and surfaces an error instead of thrashing.

#include "ui_gcode_viewer.h"

#include "../lvgl_test_fixture.h"
#include "gcode_viewer_watchdog.h"

#include <filesystem>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::gcode_viewer::watchdog_evaluate;
using helix::gcode_viewer::WatchdogDecision;
using helix::gcode_viewer::WatchdogObservation;
using helix::test_access::gcode_viewer_set_watchdog_track;
using helix::test_access::gcode_viewer_watchdog_track;
using helix::test_access::GcodeViewerWatchdogTrack;

namespace {
constexpr int NEVER_SAMPLED = -2; // sentinel for "no previous observation"
constexpr int MAX_STALL_KICKS = 30;
bool g_load_done = false;
} // namespace

TEST_CASE("watchdog: first sample never kicks or gives up", "[gcode_viewer][watchdog]") {
    // prev_cached == -2 sentinel: we have nothing to compare against yet.
    WatchdogObservation obs{/*cached=*/-1,
                            /*target=*/855,
                            /*prev_cached=*/NEVER_SAMPLED,
                            /*prev_target=*/NEVER_SAMPLED,
                            /*stall_streak=*/0,
                            /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 0);
}

TEST_CASE("watchdog: caught-up cache idles with zero streak", "[gcode_viewer][watchdog]") {
    // cached >= target: healthy, nothing to do, streak resets even if it was high.
    WatchdogObservation obs{
        /*cached=*/855,      /*target=*/855,      /*prev_cached=*/855,
        /*prev_target=*/855, /*stall_streak=*/12, /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 0);
}

TEST_CASE("watchdog: confirmed stall kicks and increments streak", "[gcode_viewer][watchdog]") {
    // Behind target AND same (cached,target) as last tick -> confirmed stall.
    WatchdogObservation obs{/*cached=*/-1,       /*target=*/855,     /*prev_cached=*/-1,
                            /*prev_target=*/855, /*stall_streak=*/0, /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 1);
}

TEST_CASE("watchdog: progress since last tick resets the streak", "[gcode_viewer][watchdog]") {
    // cached advanced (-1 -> 40): the renderer is making progress, so even a
    // large prior streak must reset and we must NOT give up.
    WatchdogObservation obs{/*cached=*/40,       /*target=*/855,      /*prev_cached=*/-1,
                            /*prev_target=*/855, /*stall_streak=*/29, /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK_FALSE(d.give_up);
    CHECK(d.stall_streak == 0);
}

TEST_CASE("watchdog: gives up after max consecutive stalls and stops kicking",
          "[gcode_viewer][watchdog]") {
    // One more stall tick reaches the cap: stop force-invalidating and signal
    // give_up so the caller can surface the error state.
    WatchdogObservation obs{/*cached=*/-1,
                            /*target=*/855,
                            /*prev_cached=*/-1,
                            /*prev_target=*/855,
                            /*stall_streak=*/MAX_STALL_KICKS - 1,
                            /*visible=*/true};
    auto d = watchdog_evaluate(obs, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK(d.give_up);
    CHECK(d.stall_streak == MAX_STALL_KICKS);
}

TEST_CASE("watchdog: a hidden viewer never kicks or gives up however long it stalls",
          "[gcode_viewer][watchdog]") {
    // A viewer under a hidden screen is not drawn, and its cache advances only
    // while drawing, so a stall there is expected rather than a render failure.
    constexpr int HIDDEN_TICKS = MAX_STALL_KICKS + 10;
    int streak = 0;
    for (int tick = 0; tick < HIDDEN_TICKS; ++tick) {
        CAPTURE(tick);
        WatchdogObservation obs{/*cached=*/-1,           /*target=*/855,
                                /*prev_cached=*/-1,      /*prev_target=*/855,
                                /*stall_streak=*/streak, /*visible=*/false};
        const WatchdogDecision d = watchdog_evaluate(obs, MAX_STALL_KICKS);
        REQUIRE_FALSE(d.kick);
        REQUIRE_FALSE(d.give_up);
        REQUIRE(d.stall_streak == 0);
        streak = d.stall_streak;
    }
}

TEST_CASE("watchdog: a viewer shown again counts its stall from zero", "[gcode_viewer][watchdog]") {
    // One tick short of giving up when the viewer is hidden: the hidden tick
    // discards that streak instead of completing it.
    WatchdogObservation hidden{/*cached=*/-1,
                               /*target=*/855,
                               /*prev_cached=*/-1,
                               /*prev_target=*/855,
                               /*stall_streak=*/MAX_STALL_KICKS - 1,
                               /*visible=*/false};
    WatchdogDecision d = watchdog_evaluate(hidden, MAX_STALL_KICKS);
    REQUIRE_FALSE(d.give_up);
    REQUIRE(d.stall_streak == 0);

    // Visible again and still stalled: a full run of kicks before it gives up.
    int streak = d.stall_streak;
    for (int tick = 1; tick < MAX_STALL_KICKS; ++tick) {
        CAPTURE(tick);
        WatchdogObservation obs{/*cached=*/-1,           /*target=*/855,
                                /*prev_cached=*/-1,      /*prev_target=*/855,
                                /*stall_streak=*/streak, /*visible=*/true};
        d = watchdog_evaluate(obs, MAX_STALL_KICKS);
        REQUIRE(d.kick);
        REQUIRE_FALSE(d.give_up);
        REQUIRE(d.stall_streak == tick);
        streak = d.stall_streak;
    }

    WatchdogObservation last{/*cached=*/-1,           /*target=*/855,
                             /*prev_cached=*/-1,      /*prev_target=*/855,
                             /*stall_streak=*/streak, /*visible=*/true};
    d = watchdog_evaluate(last, MAX_STALL_KICKS);
    CHECK_FALSE(d.kick);
    CHECK(d.give_up);
}

// ============================================================================
// The viewer restarts the watchdog for new content
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "watchdog: a new load starts from a fresh baseline",
                 "[gcode_viewer][watchdog]") {
    std::string path;
    for (const auto& prefix : {"", "../", "../../"}) {
        std::string candidate = std::string(prefix) + "assets/test_gcodes/SimpleCuraTest.gcode";
        if (std::filesystem::exists(candidate)) {
            path = candidate;
            break;
        }
    }
    REQUIRE_FALSE(path.empty()); // run helix-tests from the repo root

    lv_obj_t* viewer = ui_gcode_viewer_create(test_screen());
    REQUIRE(viewer != nullptr);

    g_load_done = false;
    ui_gcode_viewer_set_load_callback(
        viewer, [](lv_obj_t*, void*, bool) { g_load_done = true; }, nullptr);

    // One tick short of giving up on the previous file's stall.
    gcode_viewer_set_watchdog_track(viewer, {-1, 0, MAX_STALL_KICKS - 1});

    ui_gcode_viewer_load_file(viewer, path.c_str());

    const GcodeViewerWatchdogTrack track = gcode_viewer_watchdog_track(viewer);
    CHECK(track.prev_cached == NEVER_SAMPLED);
    CHECK(track.prev_target == NEVER_SAMPLED);
    CHECK(track.stall_streak == 0);

    REQUIRE(wait_until([] { return g_load_done; }, 30000));
    ui_gcode_viewer_set_load_callback(viewer, nullptr, nullptr);
    ui_gcode_viewer_clear(viewer);
    lv_obj_delete(viewer);
    process_lvgl(50);
}

TEST_CASE_METHOD(LVGLTestFixture, "watchdog: a display paused for panel power-off is not drawn",
                 "[gcode_viewer][watchdog]") {
    // Panel power-off sleep stops redraws for the whole display while the
    // screen stays visible, so the cache cannot advance there either.
    lv_obj_t* viewer = ui_gcode_viewer_create(test_screen());
    REQUIRE(viewer != nullptr);
    lv_display_t* disp = lv_obj_get_display(viewer);
    REQUIRE(disp != nullptr);

    CHECK(helix::test_access::gcode_viewer_watchdog_can_draw(viewer));

    // Enabling is counted and the display is shared across tests, so disable
    // until it actually stops accepting redraws, then give back the same count.
    int disables = 0;
    while (lv_display_is_invalidation_enabled(disp)) {
        lv_display_enable_invalidation(disp, false);
        ++disables;
    }
    CHECK_FALSE(helix::test_access::gcode_viewer_watchdog_can_draw(viewer));
    while (disables-- > 0) {
        lv_display_enable_invalidation(disp, true);
    }

    CHECK(helix::test_access::gcode_viewer_watchdog_can_draw(viewer));

    lv_obj_delete(viewer);
    process_lvgl(50);
}
