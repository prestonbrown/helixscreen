// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_preview_decision.cpp
 * @brief Tests for the pure preview-reconciliation decision function.
 *
 * These tests call the REAL helix::ui::decide_preview_action() (not a shadow
 * copy), so they double as the regression guard for the self-healing re-entry
 * behavior: the decision reads ACTUAL widget state (thumbnail has source, gcode
 * has geometry) rather than intent bools that can lie after destroy-on-close /
 * memory reclaim.
 *
 * Bug context: navigating away from Print Status mid-print and back left the
 * preview blank because dedup guards said "showing X" while the recreated/
 * cleared widget showed nothing. decide_preview_action() makes a blank widget
 * always reload because it compares against widget reality.
 *
 * The thumbnail and the gcode viewer track which file they hold SEPARATELY:
 * the thumbnail subject observer can advance the thumbnail's marker (even while
 * the panel is hidden) long before the deferred gcode load runs. The gcode
 * mismatch must be computed against the gcode viewer's own marker, never the
 * thumbnail's, or a stale render from the previous print is left on screen.
 */

#include "print_status_preview_decision.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::decide_preview_action;
using helix::ui::PreviewAction;

TEST_CASE("Preview decision: fresh open loads both", "[print_status][preview]") {
    // Nothing displayed yet, blank widgets, viewer wanted.
    PreviewAction a = decide_preview_action(/*thumb_displayed*/ "", /*gcode_displayed*/ "",
                                            /*desired*/ "benchy.gcode",
                                            /*thumb_src*/ false, /*gcode_content*/ false,
                                            /*want_viewer*/ true);
    REQUIRE(a.load_thumbnail);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: re-entry all valid is a no-op", "[print_status][preview]") {
    // Same file already displayed, both widgets populated → nothing to do.
    PreviewAction a = decide_preview_action("benchy.gcode", "benchy.gcode", "benchy.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true);
    REQUIRE_FALSE(a.load_thumbnail);
    REQUIRE_FALSE(a.load_gcode);
}

TEST_CASE("Preview decision: re-entry with blank thumbnail reloads thumbnail",
          "[print_status][preview]") {
    // Widget was recreated/reclaimed: displayed marker says same file, but the
    // thumbnail image source is gone. Must reload the thumbnail even though the
    // filename did not change.
    PreviewAction a = decide_preview_action("benchy.gcode", "benchy.gcode", "benchy.gcode",
                                            /*thumb_src*/ false, /*gcode_content*/ true,
                                            /*want_viewer*/ true);
    REQUIRE(a.load_thumbnail);
    REQUIRE_FALSE(a.load_gcode);
}

TEST_CASE("Preview decision: re-entry with unloaded viewer reloads gcode",
          "[print_status][preview]") {
    // Viewer geometry was cleared (memory pressure) but thumbnail survived.
    PreviewAction a = decide_preview_action("benchy.gcode", "benchy.gcode", "benchy.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ false,
                                            /*want_viewer*/ true);
    REQUIRE_FALSE(a.load_thumbnail);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: thumbnail advanced to new file must still reload stale gcode",
          "[print_status][preview]") {
    // Bug (stale 3D render): a print starts from the slicer/Mainsail while the
    // user is NOT on the Print Status panel. The thumbnail subject observer fires
    // and advances the THUMBNAIL's displayed marker to the new file, but the
    // gcode viewer still holds the PREVIOUS print's geometry — its deferred load
    // was never scheduled because the panel was inactive. On navigating back, the
    // gcode mismatch must be computed against the gcode viewer's OWN marker (still
    // the old file), not the thumbnail's (already the new file). Otherwise the
    // reload is suppressed and the previous print's render stays on screen while
    // the metadata + thumbnail correctly show the new print.
    PreviewAction a = decide_preview_action(/*thumb_displayed*/ "tires.gcode",
                                            /*gcode_displayed*/ "tower.gcode",
                                            /*desired*/ "tires.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true);
    REQUIRE_FALSE(a.load_thumbnail); // thumbnail already current
    REQUIRE(a.load_gcode);           // gcode still on the previous print → reload
}

TEST_CASE("Preview decision: gcode load is independent of current view mode",
          "[print_status][preview]") {
    // Regression: the view-mode subject is 0 (thumbnail) at print start and only
    // flips to 3D/2D AFTER the gcode loads. Gating the load on the display mode
    // deadlocks it — the gcode never downloads, so the mode never leaves
    // thumbnail, so the 3D render never appears. The decision must (re)load gcode
    // purely from want_viewer + widget reality, never from the view mode.
    PreviewAction a = decide_preview_action(/*thumb_displayed*/ "", /*gcode_displayed*/ "",
                                            /*desired*/ "benchy.gcode",
                                            /*thumb_src*/ false, /*gcode_content*/ false,
                                            /*want_viewer*/ true);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: want_viewer false suppresses gcode load", "[print_status][preview]") {
    // Lifecycle does not want the viewer (e.g. idle/terminal): only the thumbnail
    // fallback is relevant. The render-mode setting (thumbnail-only / 3D-disabled)
    // is enforced downstream in load_gcode_for_viewing(), not here.
    PreviewAction a = decide_preview_action("", "", "benchy.gcode",
                                            /*thumb_src*/ false, /*gcode_content*/ false,
                                            /*want_viewer*/ false);
    REQUIRE(a.load_thumbnail);
    REQUIRE_FALSE(a.load_gcode);
}

TEST_CASE("Preview decision: filename change reloads both", "[print_status][preview]") {
    // A different print started. Even though both widgets hold content, it is
    // the OLD file's content → reload both.
    PreviewAction a = decide_preview_action("old.gcode", "old.gcode", "new.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true);
    REQUIRE(a.load_thumbnail);
    REQUIRE(a.load_gcode);
}

TEST_CASE("Preview decision: desired empty does nothing", "[print_status][preview]") {
    // No active print → leave widgets alone regardless of their state.
    SECTION("blank widgets") {
        PreviewAction a = decide_preview_action("", "", "", false, false, true);
        REQUIRE_FALSE(a.load_thumbnail);
        REQUIRE_FALSE(a.load_gcode);
    }
    SECTION("stale content from finished print") {
        PreviewAction a = decide_preview_action("done.gcode", "done.gcode", "", true, true, true);
        REQUIRE_FALSE(a.load_thumbnail);
        REQUIRE_FALSE(a.load_gcode);
    }
}

// --- Stale geometry must leave the screen immediately -----------------------
//
// Reloading is not the same as clearing. The gcode load is deliberately
// deferred (5s while the printer is still preparing, to avoid a memory spike),
// and until it lands the viewer keeps rendering whatever it already holds. On a
// new print that is the PREVIOUS print's model, so the user watches the wrong
// object for the whole deferral. The decision therefore has to say "drop this
// now" separately from "fetch that later".

TEST_CASE("Preview decision: viewer holding another print's geometry is cleared",
          "[print_status][preview]") {
    PreviewAction a = decide_preview_action("old.gcode", "old.gcode", "new.gcode",
                                            /*thumb_src*/ true, /*gcode_content*/ true,
                                            /*want_viewer*/ true);
    REQUIRE(a.clear_gcode);
    REQUIRE(a.load_gcode); // still reloads, just not while showing the old model
}

TEST_CASE("Preview decision: clearing is not gated on wanting the viewer",
          "[print_status][preview]") {
    // want_viewer only decides whether to FETCH. Stale geometry is wrong on
    // screen either way, so gating the clear on it would leave the previous
    // print visible exactly when we had decided not to replace it.
    PreviewAction a = decide_preview_action("old.gcode", "old.gcode", "new.gcode", true, true,
                                            /*want_viewer*/ false);
    REQUIRE(a.clear_gcode);
    REQUIRE_FALSE(a.load_gcode);
}

TEST_CASE("Preview decision: current geometry is never cleared", "[print_status][preview]") {
    // The viewer already holds the desired print. Clearing here would blank a
    // correct render and force a needless reload.
    PreviewAction a =
        decide_preview_action("cur.gcode", "cur.gcode", "cur.gcode", true, true, true);
    REQUIRE_FALSE(a.clear_gcode);
}

TEST_CASE("Preview decision: an empty viewer has nothing to clear", "[print_status][preview]") {
    PreviewAction a =
        decide_preview_action("", "", "new.gcode", false, /*gcode_content*/ false, true);
    REQUIRE_FALSE(a.clear_gcode);
}

TEST_CASE("Preview decision: the finished-print freeze is preserved", "[print_status][preview]") {
    // After a print ends the display is deliberately frozen on its final frame
    // until a new print starts. desired == "" must not trigger a clear, or the
    // completed print's model vanishes out from under the user.
    PreviewAction a = decide_preview_action("done.gcode", "done.gcode", "", true, true, true);
    REQUIRE_FALSE(a.clear_gcode);
}
