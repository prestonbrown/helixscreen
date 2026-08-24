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

#include <filesystem>
#include <fstream>
#include <string>

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
} // namespace helix

// ============================================================================
// RAII temp directory helper
// ============================================================================

class TempDir {
  public:
    TempDir(const std::string& prefix) {
        path_ = fs::temp_directory_path() / (prefix + "_" + std::to_string(counter_++));
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
    static inline int counter_ = 0;
};

// Helper to write a file with content
static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::trunc);
    ofs << content;
}

// Helper to read a file's content
static std::string read_file(const fs::path& path) {
    std::ifstream ifs(path);
    std::string content;
    std::getline(ifs, content);
    return content;
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
    REQUIRE(read_file(target.path() / "config.json") == R"({"key": "value"})");
    REQUIRE(read_file(target.path() / "ui_xml" / "main.xml") == "<root/>");
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
    REQUIRE(read_file(target.path() / "data.txt") == "old content");
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
    REQUIRE(read_file(target.path() / "data.txt") == "new content");
    REQUIRE(read_file(target.path() / "VERSION") == "2.0.0");
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
    REQUIRE(read_file(target_path / "file.txt") == "hello");
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
    REQUIRE(read_file(target.path() / "data.txt") == "fresh");
    REQUIRE(read_file(target.path() / "VERSION") == "1.0.0");
}

TEST_CASE("Writes correct version marker after extraction", "[android][asset]") {
    TempDir source("asset_src");
    TempDir target("asset_tgt");

    write_file(source.path() / "dummy.txt", "x");

    auto result = extract_assets_if_needed(source.str(), target.str(), "3.14.159");

    REQUIRE(result == AssetExtractionResult::EXTRACTED);
    REQUIRE(read_file(target.path() / "VERSION") == "3.14.159");
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
    REQUIRE(read_file(target.path() / "a" / "b" / "c.txt") == "deep");
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
