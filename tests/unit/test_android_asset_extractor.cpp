// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_android_asset_extractor.cpp
 * @brief Unit tests for the Android asset extraction logic
 *
 * Tests the platform-agnostic extract_assets_if_needed() function using
 * temporary directories. The function copies assets from a source directory
 * to a target directory, with a VERSION marker file for cache invalidation.
 */

#include "android_asset_extractor.h"
#include "core/text_io.h"
#include "test_helpers/unique_temp_dir.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

// The extraction-gate policy lives in android_asset_extractor.cpp outside the
// __ANDROID__ block precisely so it can be exercised here. It is not in the
// public header — that carries the two entry points, and these are internals
// under test, not API. Declared by hand so the header stays the API surface.
namespace helix {
std::string trim_build_stamp(std::string stamp);
std::string resolve_asset_build_stamp(const std::string& apk_stamp,
                                      const std::string& fallback_version);
bool asset_extraction_can_skip(const std::string& resolved_stamp, const std::string& disk_stamp);
bool asset_extraction_succeeded(int dirs_requested, int dirs_extracted, int file_failures);
bool is_non_shippable_config_file(const std::string& filename);
bool asset_dir_is_user_layer(const std::string& asset_path);
int remove_compiled_font_sources(const std::string& fonts_dir);
} // namespace helix

// ============================================================================
// RAII temp directory helper
// ============================================================================

namespace {
class TempDir {
  public:
    TempDir(const std::string& prefix) {
        path_ = fs::temp_directory_path() / (prefix + "_" + helix::test::unique_suffix());
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    // Non-copyable
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const {
        return path_;
    }
    std::string str() const {
        return path_.string();
    }

  private:
    fs::path path_;
};
} // namespace

// Helper to write a file with content
static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::trunc);
    ofs << content;
}

// ============================================================================
// Extraction Tests
// ============================================================================

TEST_CASE("Extracts files from source to target directory", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    // Remove target so extractor creates it
    fs::remove_all(target.path());

    // Create source files
    write_file(source.path() / "config.json", R"({"key": "value"})");
    write_file(source.path() / "ui_xml" / "main.xml", "<root/>");

    auto result = extract_assets_if_needed(source.str(), target.str(), "1.0.0");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(fs::exists(target.path() / "config.json"));
    REQUIRE(fs::exists(target.path() / "ui_xml" / "main.xml"));
    REQUIRE(helix::text_io::read_first_line(target.path() / "config.json").value_or("") ==
            R"({"key": "value"})");
    REQUIRE(helix::text_io::read_first_line(target.path() / "ui_xml" / "main.xml").value_or("") ==
            "<root/>");
}

TEST_CASE("Skips extraction if VERSION marker matches current version", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    // Create source files
    write_file(source.path() / "data.txt", "original");

    // Pre-populate target with matching version
    write_file(target.path() / "VERSION", "2.0.0");
    write_file(target.path() / "data.txt", "old content");

    auto result = extract_assets_if_needed(source.str(), target.str(), "2.0.0");

    REQUIRE(result == AssetExtractionResult::ALREADY_CURRENT);
    // Target content should be unchanged (old content, not re-extracted)
    REQUIRE(helix::text_io::read_first_line(target.path() / "data.txt").value_or("") ==
            "old content");
}

TEST_CASE("Re-extracts if VERSION marker differs", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    // Create source files with new content
    write_file(source.path() / "data.txt", "new content");

    // Pre-populate target with old version
    write_file(target.path() / "VERSION", "1.0.0");
    write_file(target.path() / "data.txt", "old content");

    auto result = extract_assets_if_needed(source.str(), target.str(), "2.0.0");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(helix::text_io::read_first_line(target.path() / "data.txt").value_or("") ==
            "new content");
    REQUIRE(helix::text_io::read_first_line(target.path() / "VERSION").value_or("") == "2.0.0");
}

TEST_CASE("Creates target directory if it does not exist", "[android][asset]") {
    TempDir source("asset_src");
    TempDir parent("asset_parent");

    // Target is a subdirectory that does not exist yet
    fs::path target_path = parent.path() / "nested" / "target";

    write_file(source.path() / "file.txt", "hello");

    auto result = extract_assets_if_needed(source.str(), target_path.string(), "1.0.0");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(fs::exists(target_path / "file.txt"));
    REQUIRE(helix::text_io::read_first_line(target_path / "file.txt").value_or("") == "hello");
}

TEST_CASE("Missing VERSION marker triggers re-extraction", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    // Create source and target with content but no VERSION file
    write_file(source.path() / "data.txt", "fresh");
    write_file(target.path() / "data.txt", "stale");

    // No VERSION file in target = treat as needing extraction
    auto result = extract_assets_if_needed(source.str(), target.str(), "1.0.0");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(helix::text_io::read_first_line(target.path() / "data.txt").value_or("") == "fresh");
    REQUIRE(helix::text_io::read_first_line(target.path() / "VERSION").value_or("") == "1.0.0");
}

TEST_CASE("Writes correct version marker after extraction", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    write_file(source.path() / "dummy.txt", "x");

    auto result = extract_assets_if_needed(source.str(), target.str(), "3.14.159");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(helix::text_io::read_first_line(target.path() / "VERSION").value_or("") == "3.14.159");
}

TEST_CASE("Preserves directory structure during extraction", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    // Remove target so it is freshly created
    fs::remove_all(target.path());

    // Create nested structure
    write_file(source.path() / "a" / "b" / "c.txt", "deep");
    write_file(source.path() / "a" / "sibling.txt", "side");
    write_file(source.path() / "top.txt", "top");

    auto result = extract_assets_if_needed(source.str(), target.str(), "1.0.0");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(fs::exists(target.path() / "a" / "b" / "c.txt"));
    REQUIRE(fs::exists(target.path() / "a" / "sibling.txt"));
    REQUIRE(fs::exists(target.path() / "top.txt"));
    REQUIRE(helix::text_io::read_first_line(target.path() / "a" / "b" / "c.txt").value_or("") ==
            "deep");
}

TEST_CASE("Returns FAILED when source directory does not exist", "[android][asset]") {
    TempDir target("asset_tgt");

    auto result = extract_assets_if_needed("/nonexistent/source/dir", target.str(), "1.0.0");

    REQUIRE(result == AssetExtractionResult::FAILED);
}

// ============================================================================
// BUILD_STAMP gate — the APK extraction path's skip/retry policy
// ============================================================================

TEST_CASE("Build stamp is trimmed of trailing whitespace", "[android][asset][stamp]") {
    CHECK(trim_build_stamp("1754923312345\n") == "1754923312345");
    CHECK(trim_build_stamp("1754923312345\r\n") == "1754923312345");
    CHECK(trim_build_stamp("1754923312345 ") == "1754923312345");
    CHECK(trim_build_stamp("1754923312345") == "1754923312345");
    CHECK(trim_build_stamp("\n\n") == "");
    CHECK(trim_build_stamp("") == "");
    // Leading whitespace is not part of a stamp value and must survive as-is,
    // so a genuinely different stamp never compares equal to a trimmed one.
    CHECK(trim_build_stamp(" 123") == " 123");
}

TEST_CASE("A present build stamp is used verbatim", "[android][asset][stamp]") {
    CHECK(resolve_asset_build_stamp("1754923312345\n", "0.99.105") == "1754923312345");
}

TEST_CASE("A missing build stamp falls back to the release version", "[android][asset][stamp]") {
    // An empty stamp can never equal the disk stamp, so without a fallback the
    // skip branch is unreachable and every cold start re-extracts the tree.
    const std::string resolved = resolve_asset_build_stamp("", "0.99.105");
    CHECK(resolved == "version:0.99.105");
    CHECK_FALSE(resolved.empty());

    // Whitespace-only and short-read (empty) stamps take the same path.
    CHECK(resolve_asset_build_stamp("\n", "0.99.105") == "version:0.99.105");
    CHECK(resolve_asset_build_stamp("  \r\n", "0.99.105\n") == "version:0.99.105");
}

TEST_CASE("The version fallback self-heals across launches", "[android][asset][stamp]") {
    // First launch: nothing on disk, so the gate must extract...
    const std::string resolved = resolve_asset_build_stamp("", "0.99.105");
    CHECK_FALSE(asset_extraction_can_skip(resolved, ""));

    // ...and the stamp it records makes the second launch skip.
    CHECK(asset_extraction_can_skip(resolved, resolved));

    // A version bump still forces a fresh extraction.
    CHECK_FALSE(asset_extraction_can_skip(resolve_asset_build_stamp("", "0.99.106"), resolved));

    // The fallback can never collide with a real millisecond build stamp.
    CHECK_FALSE(asset_extraction_can_skip(resolved, "1754923312345"));
}

TEST_CASE("An empty resolved stamp never allows a skip", "[android][asset][stamp]") {
    // Defensive: two empty stamps compare equal as strings, which would skip
    // extraction on the strength of knowing nothing at all.
    CHECK_FALSE(asset_extraction_can_skip("", ""));
}

TEST_CASE("Matching stamps skip extraction", "[android][asset][stamp]") {
    CHECK(asset_extraction_can_skip("1754923312345", "1754923312345"));
    CHECK_FALSE(asset_extraction_can_skip("1754923312345", "1754923300000"));
}

// ============================================================================
// Extraction success gate — when the stamp may be recorded
// ============================================================================

TEST_CASE("A clean extraction may record its stamp", "[android][asset][stamp]") {
    CHECK(asset_extraction_succeeded(12, 12, 0));
}

TEST_CASE("A partial extraction must not record its stamp", "[android][asset][stamp]") {
    // ENOSPC halfway through: files failed to write. Recording the stamp here
    // pins every later launch to a truncated asset tree.
    CHECK_FALSE(asset_extraction_succeeded(12, 12, 1));
    CHECK_FALSE(asset_extraction_succeeded(12, 12, 300));

    // A directory that could not be created or enumerated.
    CHECK_FALSE(asset_extraction_succeeded(12, 11, 0));
    CHECK_FALSE(asset_extraction_succeeded(12, 0, 0));

    // Both at once.
    CHECK_FALSE(asset_extraction_succeeded(12, 8, 4));
}

TEST_CASE("An empty manifest is not a successful extraction", "[android][asset][stamp]") {
    CHECK_FALSE(asset_extraction_succeeded(0, 0, 0));
}

// ============================================================================
// config/ shipping policy — nothing user-owned leaves the package
// ============================================================================

TEST_CASE("User config is never extracted from the package", "[android][asset][config]") {
    // A packaged settings.json would carry the packager's moonraker_host and a
    // completed setup wizard onto every install.
    CHECK(is_non_shippable_config_file("settings.json"));
    CHECK(is_non_shippable_config_file("settings-test.json"));
    CHECK(is_non_shippable_config_file("helixconfig.json"));
    CHECK(is_non_shippable_config_file("helixconfig-test.json"));
    CHECK(is_non_shippable_config_file("helixconfig.json.voronv2"));
}

TEST_CASE("Runtime artifacts are never extracted from the package", "[android][asset][config]") {
    CHECK(is_non_shippable_config_file("crash.txt"));
    CHECK(is_non_shippable_config_file("crash_1.txt"));
    CHECK(is_non_shippable_config_file("crash_history.json"));
    CHECK(is_non_shippable_config_file(".crash_restart_count"));
    CHECK(is_non_shippable_config_file("tool_spools.json"));
    CHECK(is_non_shippable_config_file("user_filaments.json"));
    CHECK(is_non_shippable_config_file("filament_slot_overrides.json"));
    CHECK(is_non_shippable_config_file("telemetry_device.json"));
    CHECK(is_non_shippable_config_file("telemetry_queue.json"));
    CHECK(is_non_shippable_config_file(".helix-screen.lock"));
}

TEST_CASE("Shipped config seeds still extract", "[android][asset][config]") {
    // The predicate gates the whole of config/ — over-matching would silently
    // strip the service units, udev rules and templates the package exists to
    // deliver.
    CHECK_FALSE(is_non_shippable_config_file("settings.json.template"));
    CHECK_FALSE(is_non_shippable_config_file("helixscreen.env"));
    CHECK_FALSE(is_non_shippable_config_file("helixscreen.service"));
    CHECK_FALSE(is_non_shippable_config_file("helixscreen.init"));
    CHECK_FALSE(is_non_shippable_config_file("helixscreen-update.service"));
    CHECK_FALSE(is_non_shippable_config_file("99-helixscreen-backlight.rules"));
    CHECK_FALSE(is_non_shippable_config_file("refresh-service-units.sh"));
    CHECK_FALSE(is_non_shippable_config_file("printer_database.json"));
    CHECK_FALSE(is_non_shippable_config_file("printing_tips.json"));
    CHECK_FALSE(is_non_shippable_config_file("default_layout.json"));
}

// ============================================================================
// User config layer: an update may seed it, never rewrite it
// ============================================================================

TEST_CASE("config/ and everything under it is the user layer", "[android][asset][config]") {
    CHECK(asset_dir_is_user_layer("config"));
    CHECK(asset_dir_is_user_layer("config/themes"));
    CHECK(asset_dir_is_user_layer("config/printer_database.d"));
}

TEST_CASE("Shipped trees outside config/ are rewritten on update", "[android][asset][config]") {
    // The read-only seeds find_readable() falls back to live under assets/config/,
    // so a new release's copies must replace the old ones.
    CHECK_FALSE(asset_dir_is_user_layer("assets/config"));
    CHECK_FALSE(asset_dir_is_user_layer("assets/config/themes/defaults"));
    CHECK_FALSE(asset_dir_is_user_layer("ui_xml"));
    CHECK_FALSE(asset_dir_is_user_layer("configs"));
    CHECK_FALSE(asset_dir_is_user_layer(""));
}

// ============================================================================
// App-private storage layout
// ============================================================================

static bool is_under(const std::string& path, const std::string& dir) {
    return path == dir || path.rfind(dir + "/", 0) == 0;
}

TEST_CASE("Assets extract into data/ under the app's files dir", "[android][asset][backup]") {
    const AndroidStorageLayout layout =
        android_storage_layout("/data/user/0/org.helixscreen.app/files");
    CHECK(layout.data_dir == "/data/user/0/org.helixscreen.app/files/data");
}

TEST_CASE("Settings backups live in app storage outside the extracted tree",
          "[android][asset][backup]") {
    const std::string files = "/data/user/0/org.helixscreen.app/files";
    const AndroidStorageLayout layout = android_storage_layout(files);

    // Writable without root, and kept across an app update.
    CHECK(is_under(layout.backup_dir, files));
    CHECK(is_under(layout.backup_fallback_dir, files));
    // An extraction rewrites and prunes under data_dir; a backup there would not
    // survive the update it exists for.
    CHECK_FALSE(is_under(layout.backup_dir, layout.data_dir));
    CHECK_FALSE(is_under(layout.backup_fallback_dir, layout.data_dir));
    CHECK(layout.backup_dir != layout.backup_fallback_dir);
}

TEST_CASE("A trailing slash on the files dir does not leak into the layout",
          "[android][asset][backup]") {
    const AndroidStorageLayout layout =
        android_storage_layout("/data/data/org.helixscreen.app/files/");
    CHECK(layout.data_dir == "/data/data/org.helixscreen.app/files/data");
    CHECK(layout.backup_dir == "/data/data/org.helixscreen.app/files/state");
    CHECK(layout.backup_fallback_dir == "/data/data/org.helixscreen.app/files/.helixscreen");
}

// ============================================================================
// Auto Backup rules cover exactly the user's state in that layout
// ============================================================================

namespace {
using Rule = std::pair<std::string, std::string>; // {"include"|"exclude", path}

/// Every <include>/<exclude> under @p section (the whole file when empty).
std::set<Rule> backup_rules(const std::string& xml_path, const std::string& section = "") {
    const std::string xml = helix::text_io::read_file(xml_path).value_or("");
    REQUIRE_FALSE(xml.empty());
    size_t begin = 0;
    size_t end = xml.size();
    if (!section.empty()) {
        begin = xml.find("<" + section + ">");
        REQUIRE(begin != std::string::npos);
        end = xml.find("</" + section + ">", begin);
        REQUIRE(end != std::string::npos);
    }
    std::set<Rule> rules;
    for (const char* kind : {"include", "exclude"}) {
        const std::string open = std::string("<") + kind + " domain=\"file\" path=\"";
        for (size_t at = xml.find(open, begin); at != std::string::npos && at < end;
             at = xml.find(open, at + 1)) {
            const size_t path_start = at + open.size();
            rules.insert({kind, xml.substr(path_start, xml.find('"', path_start) - path_start)});
        }
    }
    return rules;
}

std::set<std::string> paths_of(const std::set<Rule>& rules, const std::string& kind) {
    std::set<std::string> out;
    for (const Rule& r : rules) {
        if (r.first == kind)
            out.insert(r.second);
    }
    return out;
}
} // namespace

TEST_CASE("Auto Backup includes the config layer and the settings backups",
          "[android][asset][backup]") {
    const std::string files = "/files";
    const AndroidStorageLayout layout = android_storage_layout(files);
    const auto relative = [&](const std::string& p) { return p.substr(files.size() + 1) + "/"; };
    const std::set<std::string> expected = {relative(layout.data_dir + "/config"),
                                            relative(layout.backup_dir)};

    const std::string res = "android/app/src/main/res/xml/";
    const std::set<Rule> legacy = backup_rules(res + "backup_rules.xml");
    CHECK(paths_of(legacy, "include") == expected);
    // Every exclusion narrows an include; one outside them would exclude nothing.
    for (const std::string& ex : paths_of(legacy, "exclude")) {
        CAPTURE(ex);
        CHECK(ex.rfind(relative(layout.data_dir + "/config"), 0) == 0);
    }
    CHECK(paths_of(legacy, "exclude").count("data/config/telemetry_device.json") == 1);
    CHECK(paths_of(legacy, "exclude").count("data/config/.helix-screen.lock") == 1);
    // FileConfigStorage::preserve_corrupt() keeps a broken document as <config>.corrupt;
    // restoring one onto another device brings back nothing usable.
    CHECK(paths_of(legacy, "exclude").count("data/config/settings.json.corrupt") == 1);
    CHECK(paths_of(legacy, "exclude").count("data/config/settings-test.json.corrupt") == 1);

    // Android 12+ reads the other file; both of its sections must say the same.
    for (const char* section : {"cloud-backup", "device-transfer"}) {
        CAPTURE(section);
        CHECK(backup_rules(res + "data_extraction_rules.xml", section) == legacy);
    }
}

// ============================================================================
// Font C sources left by an earlier install are reclaimed
// ============================================================================

TEST_CASE("Only font C sources are removed from an extracted fonts dir",
          "[android][asset][fonts]") {
    TempDir tmp("fonts_reclaim");
    const fs::path fonts = tmp.path() / "fonts";
    write_file(fonts / "noto_sans_16.c", "static const uint8_t glyphs[] = {0};");
    write_file(fonts / "mdi_icons_32.c", "x");
    write_file(fonts / "NotoSans-Regular.ttf", "ttf");
    write_file(fonts / "cjk" / "noto_sans_cjk_16.bin", "bin");
    write_file(fonts / "custom_icons" / "spoolman.svg", "svg");

    CHECK(remove_compiled_font_sources(fonts.string()) == 2);
    CHECK_FALSE(fs::exists(fonts / "noto_sans_16.c"));
    CHECK_FALSE(fs::exists(fonts / "mdi_icons_32.c"));
    // CjkFontManager reads the .bin faces at runtime; nothing else here goes.
    CHECK(fs::exists(fonts / "cjk" / "noto_sans_cjk_16.bin"));
    CHECK(fs::exists(fonts / "NotoSans-Regular.ttf"));
    CHECK(fs::exists(fonts / "custom_icons" / "spoolman.svg"));
}

TEST_CASE("A missing fonts dir removes nothing", "[android][asset][fonts]") {
    TempDir tmp("fonts_missing");
    CHECK(remove_compiled_font_sources((tmp.path() / "absent").string()) == 0);
}
