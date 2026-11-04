// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/gcode_filename.h"
#include "firmware_3mf_extract.h"

#include <string>

#include "../catch_amalgamated.hpp"

using helix::firmware_3mf::extract_name;
using helix::firmware_3mf::is_plate_extract;
using helix::gcode::has_printable_extension;
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
// firmware_3mf::is_plate_extract() - a firmware plate-numbered .3mf extract
// =============================================================================

TEST_CASE("is_plate_extract() accepts valid shadow names", "[filename_utils][qidi]") {
    REQUIRE(is_plate_extract("shadow_native_plate_1.gcode"));
    REQUIRE(is_plate_extract("shadow_native_plate_12.gcode"));
    REQUIRE(is_plate_extract("shadow_native_plate_007.gcode"));
    // Plate id need not be numeric - any non-empty middle is accepted.
    REQUIRE(is_plate_extract("shadow_native_plate_A.gcode"));
}

TEST_CASE("extract_name() names the plate G-code QIDI extracts into .temp",
          "[filename_utils][qidi]") {
    CHECK(extract_name("Benchy.gcode.3mf") == "Benchy.gcode");
    CHECK(extract_name("sub/dir/Benchy.gcode.3mf") == "Benchy.gcode");
    CHECK(extract_name("Benchy.3mf") == "Benchy.gcode");
    // QIDI tests the inner ".gcode" case-sensitively and so appends one to
    // an upper-case name.
    CHECK(extract_name("X.GCODE.3MF") == "X.GCODE.gcode");
}

TEST_CASE("is_plate_extract() requires a non-empty plate id", "[filename_utils][qidi]") {
    // Prefix directly followed by suffix leaves no plate id in between.
    REQUIRE_FALSE(is_plate_extract("shadow_native_plate_.gcode"));
}

TEST_CASE("is_plate_extract() rejects wrong prefix", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_plate_extract("native_plate_1.gcode"));
    // Prefix must be at position 0, not embedded.
    REQUIRE_FALSE(is_plate_extract("foo_shadow_native_plate_1.gcode"));
    REQUIRE_FALSE(is_plate_extract("shadow_plate_1.gcode"));
}

TEST_CASE("is_plate_extract() rejects wrong suffix", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_plate_extract("shadow_native_plate_1.gco"));
    REQUIRE_FALSE(is_plate_extract("shadow_native_plate_1.txt"));
    REQUIRE_FALSE(is_plate_extract("shadow_native_plate_1"));
    // The active .3mf name itself must not match.
    REQUIRE_FALSE(is_plate_extract("MyModel.3mf"));
}

TEST_CASE("is_plate_extract() is case-sensitive", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_plate_extract("SHADOW_NATIVE_PLATE_1.GCODE"));
    REQUIRE_FALSE(is_plate_extract("shadow_native_plate_1.GCODE"));
}

TEST_CASE("is_plate_extract() handles empty and short input", "[filename_utils][qidi]") {
    REQUIRE_FALSE(is_plate_extract(""));
    REQUIRE_FALSE(is_plate_extract(".gcode"));
    REQUIRE_FALSE(is_plate_extract("shadow_native_plate_"));
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

TEST_CASE("make_extracted_plate_path() stages a .gcode that maps back to its archive",
          "[filename_utils][identity][three_mf]") {
    using helix::gcode::make_extracted_plate_path;
    using helix::gcode::rewritten_archive_plate;
    using helix::gcode::trusted_original_path;
    for (const std::string original :
         {"X.gcode.3mf", "parts/X.gcode.3mf", "odd~p9~s/x~~.3mf", "a~p3.gcode.3mf"}) {
        const std::string staged = make_extracted_plate_path(original, 9);
        INFO(staged);
        CHECK(staged.rfind(".helix_temp/modified_", 0) == 0);
        CHECK(staged.size() > 6);
        CHECK(staged.compare(staged.size() - 6, 6, ".gcode") == 0);
        CHECK(helix::gcode::is_uploaded_rewrite_path(staged));
        CHECK(resolve_gcode_filename(staged) == original);
        CHECK(trusted_original_path(staged) == original);
        CHECK(rewritten_archive_plate(staged) == 9);
    }
    // An ordinary staged copy names no plate.
    CHECK(rewritten_archive_plate(helix::gcode::make_rewritten_gcode_path("X.gcode")) == 0);
    CHECK(rewritten_archive_plate("X.gcode") == 0);
    CHECK(rewritten_archive_plate(make_extracted_plate_path("X.gcode.3mf", 12)) == 12);
}

TEST_CASE("make_extracted_plate_path() keeps its plate suffix within NAME_MAX",
          "[filename_utils][identity][three_mf]") {
    std::string deep;
    for (int i = 0; i < 30; ++i) {
        deep += "folder_" + std::to_string(i) + "/";
    }
    const std::string staged =
        helix::gcode::make_extracted_plate_path(deep + "Long_Part.gcode.3mf", 4);
    INFO(staged);
    CHECK(staged.size() - std::string(".helix_temp/").size() <= 255);
    CHECK(staged.compare(staged.size() - 9, 9, "~p4.gcode") == 0);
    // Too long to place the archive: a bare name, which is not vouched for.
    CHECK_FALSE(helix::gcode::trusted_original_path(staged));
    CHECK(resolve_gcode_filename(staged) == "Long_Part.gcode.3mf");
    CHECK(helix::gcode::rewritten_archive_plate(staged) == 4);
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

namespace {
// The inline splits that call sites spelled out by hand before sharing the helpers.
std::string inline_basename(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string inline_dir(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
} // namespace

TEST_CASE("get_filename_basename() and gcode_dir_of() split at the last slash only",
          "[filename_utils][path_split]") {
    const std::pair<std::string, std::pair<std::string, std::string>> cases[] = {
        {"a.gcode", {"a.gcode", ""}},
        {"/a.gcode", {"a.gcode", ""}},
        {"dir/", {"", "dir"}},
        {"/", {"", ""}},
        {"a/b/c.gcode", {"c.gcode", "a/b"}},
        {"", {"", ""}},
        {"x.gcode/y.gcode", {"y.gcode", "x.gcode"}},
        {"sub.gcode/", {"", "sub.gcode"}},
        {"a\\b.gcode", {"a\\b.gcode", ""}},
    };
    for (const auto& [path, want] : cases) {
        INFO("path='" << path << "'");
        CHECK(helix::gcode::get_filename_basename(path) == want.first);
        CHECK(helix::gcode_dir_of(path) == want.second);
        CHECK(helix::gcode::get_filename_basename(path) == inline_basename(path));
        CHECK(helix::gcode_dir_of(path) == inline_dir(path));
    }
}

TEST_CASE("a visible staging root stages where dot-directory-blind Klipper can print",
          "[filename_utils][identity][staging]") {
    using helix::gcode::StagingRoot;
    const std::string rewritten =
        helix::gcode::make_rewritten_gcode_path("parts/benchy.gcode", StagingRoot::Visible);
    const std::string extracted =
        helix::gcode::make_extracted_plate_path("X.gcode.3mf", 9, StagingRoot::Visible);
    for (const auto& staged : {rewritten, extracted}) {
        INFO(staged);
        CHECK(staged.rfind("helix_staging/modified_", 0) == 0);
        CHECK(helix::gcode::is_uploaded_rewrite_path(staged));
        CHECK(helix::gcode::is_rewritten_gcode_path(staged));
    }
    CHECK(resolve_gcode_filename(rewritten) == "parts/benchy.gcode");
    CHECK(helix::gcode::trusted_original_path(rewritten) == "parts/benchy.gcode");
    CHECK(helix::gcode::trusted_original_path(extracted) == "X.gcode.3mf");
    CHECK(helix::gcode::rewritten_archive_plate(extracted) == 9);
    CHECK(helix::gcode::make_rewritten_gcode_path("a.gcode").rfind(".helix_temp/modified_", 0) ==
          0);

    // A user folder that merely ends in the name is not the staging directory.
    CHECK_FALSE(helix::gcode::is_uploaded_rewrite_path("myhelix_staging/modified_1p_a.gcode"));
    CHECK_FALSE(helix::gcode::is_uploaded_rewrite_path("x.helix_temp/modified_1p_a.gcode"));
}

namespace {
/// Names the plugin's link directory for one test and forgets it after.
struct PluginLinkDir {
    explicit PluginLinkDir(const std::string& dir) {
        helix::gcode::set_plugin_link_dir(dir);
    }
    ~PluginLinkDir() {
        helix::gcode::set_plugin_link_dir("");
    }
};
} // namespace

TEST_CASE("the plugin's link directory comes from its status, the hidden default always",
          "[filename_utils][identity][staging]") {
    // Not reported yet: only the plugin's default resolves.
    CHECK(resolve_gcode_filename(".helix_print/full/parts/benchy.gcode") == "parts/benchy.gcode");
    CHECK_FALSE(helix::gcode::is_rewritten_gcode_path("helix_print/full/parts/benchy.gcode"));
    CHECK_FALSE(helix::gcode::is_app_managed_directory("helix_print"));

    PluginLinkDir dir("helix_print/");
    CHECK(resolve_gcode_filename(".helix_print/full/parts/benchy.gcode") == "parts/benchy.gcode");
    CHECK(resolve_gcode_filename("helix_print/full/parts/benchy.gcode") == "parts/benchy.gcode");
    CHECK(helix::gcode::trusted_original_path("helix_print/full/parts/benchy.gcode") ==
          "parts/benchy.gcode");
    CHECK(helix::gcode::is_rewritten_gcode_path("helix_print/benchy.gcode"));
    CHECK_FALSE(helix::gcode::is_uploaded_rewrite_path("helix_print/benchy.gcode"));
    CHECK(helix::gcode::is_app_managed_directory("helix_print"));
}

TEST_CASE("staging follows the plugin's answer, else the printer database",
          "[filename_utils][staging]") {
    using helix::gcode::staging_root_for;
    using helix::gcode::StagingRoot;
    // The plugin present and its directory known: its visibility decides.
    CHECK(staging_root_for(true, "helix_print", false) == StagingRoot::Visible);
    CHECK(staging_root_for(true, ".helix_print", true) == StagingRoot::Hidden);
    // No plugin, or one that named no directory: the database flag.
    CHECK(staging_root_for(false, "helix_print", false) == StagingRoot::Hidden);
    CHECK(staging_root_for(false, "", true) == StagingRoot::Visible);
    CHECK(staging_root_for(true, "", true) == StagingRoot::Visible);
    CHECK(staging_root_for(true, "", false) == StagingRoot::Hidden);
}

TEST_CASE("the app's own gcodes directories are named once", "[filename_utils][staging]") {
    const auto dirs = helix::gcode::staging_dirs();
    CHECK(dirs == std::vector<std::string>{".helix_temp", "helix_staging"});
    CHECK(helix::gcode::is_app_managed_directory("helix_staging"));
    CHECK_FALSE(helix::gcode::is_app_managed_directory("helix_print")); // until the plugin says
    CHECK_FALSE(helix::gcode::is_app_managed_directory("parts"));
    CHECK_FALSE(helix::gcode::is_app_managed_directory(".helix_temp"));
}

TEST_CASE("only the root's staging directories hold the app's copies",
          "[filename_utils][staging]") {
    using helix::gcode::is_uploaded_rewrite_path;
    CHECK(is_uploaded_rewrite_path(".helix_temp/modified_1p_a.gcode"));
    CHECK(is_uploaded_rewrite_path("gcodes/.helix_temp/modified_1p_a.gcode"));
    CHECK(is_uploaded_rewrite_path("helix_staging/modified_1p_a.gcode"));
    // A user folder of that name deeper down is the user's: never deleted.
    CHECK_FALSE(is_uploaded_rewrite_path("jobs/helix_staging/modified_1p_a.gcode"));
    CHECK_FALSE(is_uploaded_rewrite_path("jobs/.helix_temp/modified_1p_a.gcode"));
    CHECK(resolve_gcode_filename("jobs/helix_staging/modified_1p_a.gcode") ==
          "jobs/helix_staging/modified_1p_a.gcode");
}
