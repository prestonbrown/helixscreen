// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::gcode::is_native_3mf_shadow;
using helix::gcode::resolve_gcode_filename;
using helix::gcode::thumbnail_source_describes;

// =============================================================================
// is_native_3mf_shadow() - QIDI native-3MF shadow G-code detection
// =============================================================================

TEST_CASE("is_native_3mf_shadow() accepts valid shadow names", "[filename_utils][qidi]") {
    REQUIRE(is_native_3mf_shadow("shadow_native_plate_1.gcode"));
    REQUIRE(is_native_3mf_shadow("shadow_native_plate_12.gcode"));
    REQUIRE(is_native_3mf_shadow("shadow_native_plate_007.gcode"));
    // Plate id need not be numeric - any non-empty middle is accepted.
    REQUIRE(is_native_3mf_shadow("shadow_native_plate_A.gcode"));
}

TEST_CASE("is_native_3mf_shadow() requires a non-empty plate id", "[filename_utils][qidi]") {
    // Prefix directly followed by suffix leaves no plate id in between.
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_native_plate_.gcode"));
}

TEST_CASE("is_native_3mf_shadow() rejects wrong prefix", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_native_3mf_shadow("native_plate_1.gcode"));
    // Prefix must be at position 0, not embedded.
    REQUIRE_FALSE(is_native_3mf_shadow("foo_shadow_native_plate_1.gcode"));
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_plate_1.gcode"));
}

TEST_CASE("is_native_3mf_shadow() rejects wrong suffix", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_native_plate_1.gco"));
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_native_plate_1.txt"));
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_native_plate_1"));
    // The active .3mf name itself must not match.
    REQUIRE_FALSE(is_native_3mf_shadow("MyModel.3mf"));
}

TEST_CASE("is_native_3mf_shadow() is case-sensitive", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_native_3mf_shadow("SHADOW_NATIVE_PLATE_1.GCODE"));
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_native_plate_1.GCODE"));
}

TEST_CASE("is_native_3mf_shadow() handles empty and short input", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_native_3mf_shadow(""));
    REQUIRE_FALSE(is_native_3mf_shadow(".gcode"));
    REQUIRE_FALSE(is_native_3mf_shadow("shadow_native_plate_"));
}

// =============================================================================
// resolve_gcode_filename() - rewritten temp path -> original
// =============================================================================
//
// This is the shared primitive under every "which print is this?" decision in
// ActivePrintMediaManager and PrintStatusPanel, and until now it had no direct
// coverage - only incidental exercise through those two suites. Both sides must
// agree on its answer or the panel's identity comparison against the manager's
// published stamp fails silently, with no retry (prestonbrown/helixscreen#1339).

TEST_CASE("resolve_gcode_filename() extracts the original from each rewrite prefix",
          "[filename_utils][identity]") {
    CHECK(resolve_gcode_filename(".helix_temp/modified_1748_Widget.gcode") == "Widget.gcode");
    CHECK(resolve_gcode_filename("x/gcode_mod/mod_123_Model.gcode") == "Model.gcode");
    CHECK(resolve_gcode_filename("/tmp/helixscreen_mod_123_Model.gcode") == "Model.gcode");
}

TEST_CASE("resolve_gcode_filename() finds the prefix anywhere in the path",
          "[filename_utils][identity]") {
    // print_stats reports the path relative to the gcodes root, so the marker is
    // not at position 0. Moonraker's own listing shows `gcodes/.helix_temp`.
    CHECK(resolve_gcode_filename("gcodes/.helix_temp/modified_1748_Widget.gcode") ==
          "Widget.gcode");
}

TEST_CASE("resolve_gcode_filename() keeps underscores inside the original name",
          "[filename_utils][identity]") {
    // Only the FIRST underscore after the prefix separates the timestamp; the
    // rest belong to the user's filename. Splitting on the last one would
    // truncate every name with an underscore in it.
    CHECK(resolve_gcode_filename(".helix_temp/modified_9876543210_My_Cool_Print.gcode") ==
          "My_Cool_Print.gcode");
}

TEST_CASE("resolve_gcode_filename() returns unrecognised input unchanged",
          "[filename_utils][identity]") {
    CHECK(resolve_gcode_filename("plain.gcode") == "plain.gcode");
    CHECK(resolve_gcode_filename("sub/dir/plain.gcode") == "sub/dir/plain.gcode");
    CHECK(resolve_gcode_filename("") == "");
}

TEST_CASE("resolve_gcode_filename() leaves a malformed rewrite alone",
          "[filename_utils][identity]") {
    // No underscore after the prefix: there is no timestamp to strip, so there
    // is no original to recover. Returning a truncated guess here would name a
    // file that does not exist and every metadata lookup would 404.
    CHECK(resolve_gcode_filename(".helix_temp/modified_mine.gcode") ==
          ".helix_temp/modified_mine.gcode");
    // Trailing separator leaves an empty remainder.
    CHECK(resolve_gcode_filename(".helix_temp/modified_123_") == ".helix_temp/modified_123_");
}

// =============================================================================
// thumbnail_source_describes() - may this override still name this print?
// =============================================================================

TEST_CASE("thumbnail_source_describes() accepts an exact match", "[filename_utils][identity]") {
    CHECK(thumbnail_source_describes("Widget.gcode", "Widget.gcode"));
}

TEST_CASE("thumbnail_source_describes() accepts the original behind a rewrite",
          "[filename_utils][identity]") {
    CHECK(thumbnail_source_describes(".helix_temp/modified_1748_Widget.gcode", "Widget.gcode"));
}

TEST_CASE("thumbnail_source_describes() matches on basename across directories",
          "[filename_utils][identity]") {
    // print_stats may report a path while the override holds a bare name (or the
    // reverse), so equality alone would retire a source that still describes the
    // print.
    CHECK(thumbnail_source_describes("sub/dir/Widget.gcode", "Widget.gcode"));
    CHECK(thumbnail_source_describes("Widget.gcode", "other/dir/Widget.gcode"));
}

TEST_CASE("thumbnail_source_describes() accepts ANY rewritten path regardless of source",
          "[filename_utils][identity]") {
    // Deliberate and load-bearing, not a loose comparison: only this app produces
    // a rewritten path, so one always belongs to a print we started, whose
    // preparing epoch set the override being held. Reprint replays whatever
    // print_stats last reported, which for a modified print is the temp name -
    // and the original may not be recoverable from the string at all. Tightening
    // this to a real comparison retires the override mid-print and puts the
    // previous print's image back on screen.
    CHECK(thumbnail_source_describes(".helix_temp/modified_1748_Widget.gcode",
                                     "SomethingElse.gcode"));
    CHECK(thumbnail_source_describes(".helix_temp/modified_mine.gcode", "Unrecoverable.gcode"));
}

TEST_CASE("thumbnail_source_describes() rejects an unrelated print", "[filename_utils][identity]") {
    // The case the retirement check exists for: print A's override must not
    // survive into print B, or B resolves its media through A and the panel
    // never registers that a new print began.
    CHECK_FALSE(thumbnail_source_describes("printB.gcode", "printA.gcode"));
    CHECK_FALSE(thumbnail_source_describes("dir/printB.gcode", "other/printA.gcode"));
}
