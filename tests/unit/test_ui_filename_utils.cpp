// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_filename_utils.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::gcode::has_printable_extension;
using helix::gcode::is_native_3mf_shadow;
using helix::gcode::qidi_3mf_extract_name;
using helix::gcode::resolve_gcode_filename;
using helix::gcode::thumbnail_source_describes;

// =============================================================================
// has_printable_extension() - the one printable-extension list
// =============================================================================
//
// The Moonraker file list, the USB stick scanner and the display-name stripper
// all decide "is this a printable file" through this predicate. A FAT stick
// mounted without long-filename support hands the scanner 8.3 upper-case names
// like 3DBENC~1.GCO, so case-insensitivity and the short extensions are not
// cosmetic.

TEST_CASE("has_printable_extension() accepts every printable extension",
          "[filename_utils][printable]") {
    REQUIRE(has_printable_extension("benchy.gcode"));
    REQUIRE(has_printable_extension("benchy.gco"));
    REQUIRE(has_printable_extension("benchy.g"));
    REQUIRE(has_printable_extension("plate.3mf"));
}

TEST_CASE("has_printable_extension() is case-insensitive", "[filename_utils][printable]") {
    // 8.3 short name a no-LFN FAT mount produces for 3DBenchy.gcode
    REQUIRE(has_printable_extension("3DBENC~1.GCO"));
    REQUIRE(has_printable_extension("BENCHY.GCODE"));
    REQUIRE(has_printable_extension("Job.G"));
    REQUIRE(has_printable_extension("PLATE.3MF"));
}

TEST_CASE("has_printable_extension() accepts names shorter than six characters",
          "[filename_utils][printable]") {
    REQUIRE(has_printable_extension("a.g"));
    REQUIRE(has_printable_extension("a.gco"));
}

TEST_CASE("has_printable_extension() rejects non-printable and malformed names",
          "[filename_utils][printable]") {
    REQUIRE_FALSE(has_printable_extension("notes.txt"));
    REQUIRE_FALSE(has_printable_extension("firmware.bin"));
    // Klipper cannot execute these, so listing them would offer a file that
    // fails the moment it is selected.
    REQUIRE_FALSE(has_printable_extension("job.bgcode"));
    REQUIRE_FALSE(has_printable_extension("package.ufp"));
    REQUIRE_FALSE(has_printable_extension(""));
    REQUIRE_FALSE(has_printable_extension("gcode"));
    // A name that is nothing but the extension is a hidden dotfile.
    REQUIRE_FALSE(has_printable_extension(".gcode"));
    REQUIRE_FALSE(has_printable_extension(".g"));
}

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

TEST_CASE("qidi_3mf_extract_name() names the plate G-code QIDI extracts into .temp",
          "[filename_utils][qidi]") {
    CHECK(qidi_3mf_extract_name("Benchy.gcode.3mf") == "Benchy.gcode");
    CHECK(qidi_3mf_extract_name("sub/dir/Benchy.gcode.3mf") == "Benchy.gcode");
    CHECK(qidi_3mf_extract_name("Benchy.3mf") == "Benchy.gcode");
    // QIDI tests the inner ".gcode" case-sensitively and so appends one to
    // an upper-case name.
    CHECK(qidi_3mf_extract_name("X.GCODE.3MF") == "X.GCODE.gcode");
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

TEST_CASE("make_rewritten_gcode_path() keeps the original's whole path recoverable",
          "[filename_utils][identity][reprint]") {
    for (const std::string original :
         {"benchy.gcode", "parts/benchy.gcode", "a/b/My_Part.gcode", "odd~s~~name/x~.gcode"}) {
        const std::string staged = helix::gcode::make_rewritten_gcode_path(original);
        INFO(staged);
        // One flat file in the staging directory: the cleanups list it there.
        CHECK(staged.rfind(".helix_temp/modified_", 0) == 0);
        CHECK(staged.find('/', std::string(".helix_temp/").size()) == std::string::npos);
        CHECK(helix::gcode::is_uploaded_rewrite_path(staged));
        CHECK(resolve_gcode_filename(staged) == original);
        CHECK(resolve_gcode_filename("gcodes/" + staged) == original);
    }
}

TEST_CASE("resolve_gcode_filename() decodes a staged subfolder path",
          "[filename_utils][identity][reprint]") {
    CHECK(resolve_gcode_filename(".helix_temp/modified_1748p_parts~sbenchy.gcode") ==
          "parts/benchy.gcode");
    // Without the path marker the name is a bare filename, taken literally.
    CHECK(resolve_gcode_filename(".helix_temp/modified_1748_a~sb.gcode") == "a~sb.gcode");
}

TEST_CASE("resolve_gcode_filename() unwraps a HelixPrint plugin symlink path",
          "[filename_utils][identity][reprint]") {
    CHECK(resolve_gcode_filename(".helix_print/parts/benchy.gcode") == "parts/benchy.gcode");
    CHECK(resolve_gcode_filename(".helix_print/benchy.gcode") == "benchy.gcode");
    // Only the plugin's own directory, as the leading segment.
    CHECK(resolve_gcode_filename("my.helix_print/b.gcode") == "my.helix_print/b.gcode");
    CHECK_FALSE(helix::gcode::is_rewritten_gcode_path("models/.helix_print/full/x.gcode"));
    CHECK(resolve_gcode_filename("models/.helix_print/full/x.gcode") ==
          "models/.helix_print/full/x.gcode");
    CHECK(helix::gcode::trusted_original_path("models/.helix_print/full/x.gcode") ==
          "models/.helix_print/full/x.gcode");
    CHECK(resolve_gcode_filename(".helix_print/") == ".helix_print/");

    // A plugin-started print is ours, but its symlink is the plugin's to remove.
    CHECK(helix::gcode::is_rewritten_gcode_path(".helix_print/parts/benchy.gcode"));
    CHECK_FALSE(helix::gcode::is_uploaded_rewrite_path(".helix_print/parts/benchy.gcode"));
    CHECK_FALSE(helix::gcode::is_rewritten_gcode_path("my.helix_print/b.gcode"));
}

TEST_CASE("trusted_original_path() answers only for names that place the original",
          "[filename_utils][identity][reprint]") {
    using helix::gcode::trusted_original_path;
    // Not a rewrite: the path names itself.
    CHECK(trusted_original_path("parts/benchy.gcode") == "parts/benchy.gcode");
    // Whole-path forms.
    CHECK(trusted_original_path(helix::gcode::make_rewritten_gcode_path("parts/benchy.gcode")) ==
          "parts/benchy.gcode");
    CHECK(trusted_original_path(".helix_print/full/parts/benchy.gcode") == "parts/benchy.gcode");
    CHECK(trusted_original_path(".helix_print/full/benchy.gcode") == "benchy.gcode");
    // Bare-filename forms could be a same-named file in any folder.
    CHECK_FALSE(trusted_original_path(".helix_temp/modified_1748_benchy.gcode"));
    CHECK_FALSE(trusted_original_path(".helix_print/benchy.gcode"));
    CHECK_FALSE(trusted_original_path("x/gcode_mod/mod_1_benchy.gcode"));
    CHECK_FALSE(trusted_original_path(".helix_temp/modified_mine.gcode"));
    // resolve_gcode_filename() still guesses, for display and lookups.
    CHECK(resolve_gcode_filename(".helix_print/benchy.gcode") == "benchy.gcode");
    CHECK(resolve_gcode_filename(".helix_print/full/parts/benchy.gcode") == "parts/benchy.gcode");
}

TEST_CASE("make_rewritten_gcode_path() keeps a staged name within NAME_MAX",
          "[filename_utils][identity][reprint]") {
    std::string deep;
    for (int i = 0; i < 30; ++i) {
        deep += "folder_" + std::to_string(i) + "/";
    }
    const std::string original = deep + "benchy.gcode";
    const std::string staged = helix::gcode::make_rewritten_gcode_path(original);
    INFO(staged);
    const std::string name = staged.substr(std::string(".helix_temp/").size());
    CHECK(name.size() <= 255);
    CHECK(name.find('/') == std::string::npos);
    CHECK(helix::gcode::is_uploaded_rewrite_path(staged));
    // Too long to carry the path, so it cannot vouch for one.
    CHECK_FALSE(helix::gcode::trusted_original_path(staged));
    CHECK(resolve_gcode_filename(staged) == "benchy.gcode");

    // A cut that falls inside a multibyte character moves to the next one.
    for (int pad = 0; pad < 3; ++pad) {
        std::string cjk;
        for (int i = 0; i < 90; ++i) {
            cjk += "\xE6\xA8\xA1"; // 模, three bytes
        }
        const std::string staged_cjk = helix::gcode::make_rewritten_gcode_path(
            cjk + std::string(static_cast<size_t>(pad), 'a') + ".gcode");
        const std::string cut = staged_cjk.substr(std::string(".helix_temp/modified_").size());
        const std::string kept = cut.substr(cut.find('_') + 1);
        INFO("pad " << pad);
        CHECK(staged_cjk.size() - std::string(".helix_temp/").size() <= 255);
        REQUIRE_FALSE(kept.empty());
        CHECK((static_cast<unsigned char>(kept[0]) & 0xC0) != 0x80);
        CHECK(kept.substr(kept.size() - 6) == ".gcode");
    }

    const std::string long_name = std::string(250, 'x') + ".gcode";
    const std::string staged_long = helix::gcode::make_rewritten_gcode_path(long_name);
    CHECK(staged_long.size() - std::string(".helix_temp/").size() <= 255);
    CHECK(staged_long.substr(staged_long.size() - 6) == ".gcode");
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

TEST_CASE("is_3mf() matches a .3mf suffix in any case", "[filename_utils][qidi_3mf]") {
    using helix::gcode::is_3mf;
    CHECK(is_3mf("Foo (PETG).gcode.3mf"));
    CHECK(is_3mf("dir/Model.3MF"));
    CHECK_FALSE(is_3mf(".3mf"));
    CHECK_FALSE(is_3mf("Foo.gcode"));
    CHECK_FALSE(is_3mf("shadow_native_plate_1.gcode"));
    CHECK_FALSE(is_3mf("3mf"));
    CHECK_FALSE(is_3mf("foo3mf"));
    CHECK_FALSE(is_3mf("Foo.3mf.gcode"));
    CHECK_FALSE(is_3mf(""));
}

TEST_CASE("strip_gcode_extension() removes a whole .gcode.3mf", "[filename_utils][qidi_3mf]") {
    using helix::gcode::get_display_filename;
    using helix::gcode::strip_gcode_extension;
    CHECK(strip_gcode_extension("Foo (PETG).gcode.3mf") == "Foo (PETG)");
    CHECK(strip_gcode_extension("Foo.GCODE.3MF") == "Foo");
    CHECK(strip_gcode_extension("Foo.gco.3mf") == "Foo");
    CHECK(strip_gcode_extension("Model.3mf") == "Model");
    CHECK(strip_gcode_extension("Model.3mf.3mf") == "Model.3mf");
    CHECK(strip_gcode_extension(".3mf") == ".3mf");
    CHECK(strip_gcode_extension("Foo.gcode") == "Foo");
    CHECK(get_display_filename("dir/Foo (PETG).gcode.3mf") == "Foo (PETG)");
}
