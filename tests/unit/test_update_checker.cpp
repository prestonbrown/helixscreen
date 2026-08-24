// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_update_checker.cpp
 * @brief TDD tests for UpdateChecker service
 *
 * These tests define the expected interface and behavior of UpdateChecker
 * before implementation exists. Tests are structured to:
 *
 * 1. Run currently (version comparison, JSON parsing) - validates existing utils
 * 2. Fail to compile once update_checker.h is included - drives interface design
 * 3. Pass after full implementation - validates implementation correctness
 *
 * Test categories:
 * - Version comparison for update detection
 * - GitHub release JSON parsing
 * - Error handling (network, parse, invalid data)
 * - Status enum transitions
 */

#include "../helix_test_fixture.h"
#include "../test_helpers/live_thread_count.h"
#include "../test_helpers/update_queue_test_access.h"
#include "config.h"
#include "lvgl.h"
#include "version.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::version;
using json = nlohmann::json;

// ============================================================================
// Helper Functions for UpdateChecker Logic
// ============================================================================

namespace {

/**
 * @brief Strip 'v' or 'V' prefix from version tag
 *
 * GitHub releases use "v1.2.3" format, but version comparison needs "1.2.3"
 */
std::string strip_version_prefix(const std::string& tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) {
        return tag.substr(1);
    }
    return tag;
}

/**
 * @brief Determine if an update is available
 *
 * Returns true if latest > current (newer version available)
 * Returns false if latest <= current (up to date or ahead)
 */
bool is_update_available(const std::string& current_version, const std::string& latest_version) {
    auto current = parse_version(current_version);
    auto latest = parse_version(latest_version);

    if (!current || !latest) {
        return false; // Can't determine, assume no update
    }

    return *latest > *current;
}

/**
 * @brief Parse ReleaseInfo from GitHub API JSON response
 *
 * Expected JSON format:
 * {
 *   "tag_name": "v1.2.3",
 *   "body": "Release notes...",
 *   "published_at": "2025-01-15T10:00:00Z",
 *   "assets": [{"name": "file.tar.gz", "browser_download_url": "https://..."}]
 * }
 */
struct ParsedRelease {
    std::string version;       // Stripped version (e.g., "1.2.3")
    std::string tag_name;      // Original tag (e.g., "v1.2.3")
    std::string download_url;  // Asset download URL
    std::string release_notes; // Body markdown
    std::string published_at;  // ISO 8601 timestamp
    bool valid = false;
};

/**
 * @brief Safely get string value from JSON, handling null
 */
std::string json_string_or_empty(const json& j, const std::string& key) {
    if (!j.contains(key)) {
        return "";
    }
    const auto& val = j[key];
    if (val.is_null()) {
        return "";
    }
    if (val.is_string()) {
        return val.get<std::string>();
    }
    return "";
}

ParsedRelease parse_github_release(const std::string& json_str) {
    ParsedRelease result;

    try {
        auto j = json::parse(json_str);

        result.tag_name = json_string_or_empty(j, "tag_name");
        result.release_notes = json_string_or_empty(j, "body");
        result.published_at = json_string_or_empty(j, "published_at");

        // Strip 'v' prefix for version comparison
        result.version = strip_version_prefix(result.tag_name);

        // Find binary asset URL (look for .tar.gz)
        if (j.contains("assets") && j["assets"].is_array()) {
            for (const auto& asset : j["assets"]) {
                std::string name = asset.value("name", "");
                if (name.find(".tar.gz") != std::string::npos) {
                    result.download_url = asset.value("browser_download_url", "");
                    break;
                }
            }
        }

        // Valid if we have at least a version
        result.valid = !result.version.empty() && parse_version(result.version).has_value();

    } catch (const json::exception&) {
        result.valid = false;
    }

    return result;
}

} // anonymous namespace

// ============================================================================
// Version Comparison for Update Detection
// ============================================================================

TEST_CASE("Version comparison for update detection", "[update_checker][version]") {
    SECTION("update available when latest > current") {
        // Minor version bump
        REQUIRE(is_update_available("1.0.0", "1.1.0"));
        // Patch version bump
        REQUIRE(is_update_available("1.0.0", "1.0.1"));
        // Major version bump
        REQUIRE(is_update_available("1.0.0", "2.0.0"));
        // Multiple component differences
        REQUIRE(is_update_available("1.2.3", "1.2.4"));
        REQUIRE(is_update_available("1.2.3", "1.3.0"));
        REQUIRE(is_update_available("1.2.3", "2.0.0"));
    }

    SECTION("no update when versions are equal") {
        REQUIRE_FALSE(is_update_available("1.0.0", "1.0.0"));
        REQUIRE_FALSE(is_update_available("2.5.3", "2.5.3"));
        REQUIRE_FALSE(is_update_available("0.0.1", "0.0.1"));
    }

    SECTION("no update when current is ahead (don't downgrade)") {
        // Current is newer than remote (development build scenario)
        REQUIRE_FALSE(is_update_available("1.1.0", "1.0.0"));
        REQUIRE_FALSE(is_update_available("2.0.0", "1.9.9"));
        REQUIRE_FALSE(is_update_available("1.0.1", "1.0.0"));
    }

    SECTION("handles v prefix in version strings") {
        // parse_version already handles v prefix
        auto v1 = parse_version("v1.0.0");
        auto v2 = parse_version("1.1.0");
        REQUIRE(v1.has_value());
        REQUIRE(v2.has_value());
        REQUIRE(*v2 > *v1);
    }

    SECTION("pre-release suffix stripped for comparison") {
        // Pre-release versions should compare as their base version
        auto beta = parse_version("1.0.0-beta");
        auto release = parse_version("1.0.0");
        REQUIRE(beta.has_value());
        REQUIRE(release.has_value());
        // Both parse to 1.0.0, so they're equal
        REQUIRE(*beta == *release);
    }

    SECTION("pre-release to release is NOT an update (same base version)") {
        // v1.0.0-beta -> v1.0.0 should NOT be an update
        // (pre-release suffix is stripped, versions are equal)
        REQUIRE_FALSE(is_update_available("1.0.0-beta", "1.0.0"));
        REQUIRE_FALSE(is_update_available("1.0.0", "1.0.0-beta"));
    }

    SECTION("invalid version strings return no update") {
        REQUIRE_FALSE(is_update_available("", "1.0.0"));
        REQUIRE_FALSE(is_update_available("1.0.0", ""));
        REQUIRE_FALSE(is_update_available("invalid", "1.0.0"));
        REQUIRE_FALSE(is_update_available("1.0.0", "invalid"));
        REQUIRE_FALSE(is_update_available("", ""));
    }
}

// ============================================================================
// GitHub Release JSON Parsing
// ============================================================================

TEST_CASE("GitHub release JSON parsing", "[update_checker][json]") {
    SECTION("parses valid release JSON") {
        const char* json_str = R"({
            "tag_name": "v1.2.3",
            "body": "## What's New\n- Feature A\n- Bug fix B",
            "published_at": "2025-01-15T10:00:00Z",
            "assets": [{
                "name": "helixscreen-1.2.3.tar.gz",
                "browser_download_url": "https://github.com/prestonbrown/helixscreen/releases/download/v1.2.3/helixscreen-1.2.3.tar.gz"
            }]
        })";

        auto release = parse_github_release(json_str);

        REQUIRE(release.valid);
        REQUIRE(release.tag_name == "v1.2.3");
        REQUIRE(release.version == "1.2.3");
        REQUIRE(release.release_notes == "## What's New\n- Feature A\n- Bug fix B");
        REQUIRE(release.published_at == "2025-01-15T10:00:00Z");
        REQUIRE(release.download_url ==
                "https://github.com/prestonbrown/helixscreen/releases/download/v1.2.3/"
                "helixscreen-1.2.3.tar.gz");
    }

    SECTION("handles multiple assets, selects tar.gz") {
        const char* json_str = R"({
            "tag_name": "v2.0.0",
            "body": "Release",
            "published_at": "2025-02-01T00:00:00Z",
            "assets": [
                {"name": "source.zip", "browser_download_url": "https://example.com/source.zip"},
                {"name": "helixscreen.tar.gz", "browser_download_url": "https://example.com/helixscreen.tar.gz"},
                {"name": "debug.log", "browser_download_url": "https://example.com/debug.log"}
            ]
        })";

        auto release = parse_github_release(json_str);

        REQUIRE(release.valid);
        REQUIRE(release.download_url == "https://example.com/helixscreen.tar.gz");
    }

    SECTION("handles missing optional fields gracefully") {
        // Minimal valid JSON - only tag_name required for version
        const char* json_str = R"({
            "tag_name": "v3.0.0"
        })";

        auto release = parse_github_release(json_str);

        REQUIRE(release.valid);
        REQUIRE(release.version == "3.0.0");
        REQUIRE(release.release_notes.empty());
        REQUIRE(release.published_at.empty());
        REQUIRE(release.download_url.empty());
    }

    SECTION("handles empty assets array") {
        const char* json_str = R"({
            "tag_name": "v1.0.0",
            "body": "No binaries yet",
            "assets": []
        })";

        auto release = parse_github_release(json_str);

        REQUIRE(release.valid);
        REQUIRE(release.version == "1.0.0");
        REQUIRE(release.download_url.empty());
    }

    SECTION("handles null body field") {
        const char* json_str = R"({
            "tag_name": "v1.0.0",
            "body": null,
            "published_at": "2025-01-01T00:00:00Z"
        })";

        auto release = parse_github_release(json_str);

        REQUIRE(release.valid);
        // null should be converted to empty string by .value() default
        REQUIRE(release.release_notes.empty());
    }

    SECTION("rejects malformed JSON") {
        const char* invalid_json = R"({
            "tag_name": "v1.0.0"
            "body": "missing comma"
        })";

        auto release = parse_github_release(invalid_json);
        REQUIRE_FALSE(release.valid);
    }

    SECTION("rejects empty JSON object") {
        auto release = parse_github_release("{}");
        REQUIRE_FALSE(release.valid);
    }

    SECTION("rejects invalid tag_name") {
        const char* json_str = R"({
            "tag_name": "not-a-version"
        })";

        auto release = parse_github_release(json_str);
        REQUIRE_FALSE(release.valid);
    }

    SECTION("rejects empty string") {
        auto release = parse_github_release("");
        REQUIRE_FALSE(release.valid);
    }

    SECTION("handles version without v prefix") {
        const char* json_str = R"({
            "tag_name": "1.5.0"
        })";

        auto release = parse_github_release(json_str);

        REQUIRE(release.valid);
        REQUIRE(release.tag_name == "1.5.0");
        REQUIRE(release.version == "1.5.0");
    }
}

// ============================================================================
// Version Prefix Stripping
// ============================================================================

TEST_CASE("Version prefix stripping", "[update_checker][version]") {
    SECTION("strips lowercase v") {
        REQUIRE(strip_version_prefix("v1.2.3") == "1.2.3");
    }

    SECTION("strips uppercase V") {
        REQUIRE(strip_version_prefix("V1.2.3") == "1.2.3");
    }

    SECTION("preserves version without prefix") {
        REQUIRE(strip_version_prefix("1.2.3") == "1.2.3");
    }

    SECTION("handles empty string") {
        REQUIRE(strip_version_prefix("") == "");
    }

    SECTION("handles just v") {
        REQUIRE(strip_version_prefix("v") == "");
    }
}

// ============================================================================
// Error Handling Scenarios
// ============================================================================

TEST_CASE("Update checker error scenarios", "[update_checker][error]") {
    SECTION("empty response body") {
        auto release = parse_github_release("");
        REQUIRE_FALSE(release.valid);
    }

    SECTION("non-JSON response") {
        auto release = parse_github_release("<!DOCTYPE html><html>Error</html>");
        REQUIRE_FALSE(release.valid);
    }

    SECTION("JSON array instead of object") {
        auto release = parse_github_release("[1, 2, 3]");
        REQUIRE_FALSE(release.valid);
    }

    SECTION("deeply nested invalid structure") {
        const char* json_str = R"({
            "tag_name": {"nested": "object"}
        })";

        auto release = parse_github_release(json_str);
        REQUIRE_FALSE(release.valid);
    }
}

// ============================================================================
// UpdateChecker Interface Tests (TO BE ENABLED)
// ============================================================================
//
// These tests define the expected UpdateChecker interface. They will fail to
// compile until update_checker.h is created. Uncomment after implementation.
//

// Interface tests for UpdateChecker - now enabled

#include "ui_update_queue.h"

#include "app_globals.h"
#include "print_lifecycle_state.h"
#include "printer_state.h"
#include "system/update_checker.h"

using namespace helix;
TEST_CASE("UpdateChecker status enum values", "[update_checker][status]") {
    // Verify enum values exist and are distinct
    REQUIRE(static_cast<int>(UpdateChecker::Status::Idle) == 0);
    REQUIRE(static_cast<int>(UpdateChecker::Status::Checking) == 1);
    REQUIRE(static_cast<int>(UpdateChecker::Status::UpdateAvailable) == 2);
    REQUIRE(static_cast<int>(UpdateChecker::Status::UpToDate) == 3);
    REQUIRE(static_cast<int>(UpdateChecker::Status::Error) == 4);
}

TEST_CASE("UpdateChecker initial state", "[update_checker][init]") {
    auto& checker = UpdateChecker::instance();

    // Clear any state from previous tests
    checker.clear_cache();

    SECTION("starts in Idle state after clear") {
        REQUIRE(checker.get_status() == UpdateChecker::Status::Idle);
    }

    SECTION("no cached update after clear") {
        REQUIRE_FALSE(checker.has_update_available());
        REQUIRE_FALSE(checker.get_cached_update().has_value());
    }

    SECTION("no error message after clear") {
        REQUIRE(checker.get_error_message().empty());
    }
}

TEST_CASE("UpdateChecker ReleaseInfo struct", "[update_checker][release_info]") {
    UpdateChecker::ReleaseInfo info;

    SECTION("default construction has empty strings") {
        REQUIRE(info.version.empty());
        REQUIRE(info.tag_name.empty());
        REQUIRE(info.download_url.empty());
        REQUIRE(info.release_notes.empty());
        REQUIRE(info.published_at.empty());
    }

    SECTION("can assign values") {
        info.version = "1.2.3";
        info.tag_name = "v1.2.3";
        info.download_url = "https://example.com/release.tar.gz";
        info.release_notes = "Bug fixes";
        info.published_at = "2025-01-15T10:00:00Z";

        REQUIRE(info.version == "1.2.3");
        REQUIRE(info.tag_name == "v1.2.3");
    }
}

TEST_CASE("UpdateChecker cache behavior", "[update_checker][cache]") {
    auto& checker = UpdateChecker::instance();

    SECTION("clear_cache resets cached update") {
        checker.clear_cache();
        REQUIRE_FALSE(checker.get_cached_update().has_value());
        REQUIRE(checker.get_status() == UpdateChecker::Status::Idle);
    }
}

TEST_CASE("UpdateChecker thread safety", "[update_checker][threading]") {
    auto& checker = UpdateChecker::instance();

    SECTION("get_status is thread-safe") {
        // Should be able to call from any thread
        auto status = checker.get_status();
        (void)status; // Use the variable
    }

    SECTION("get_cached_update is thread-safe") {
        // Should return consistent snapshot
        auto cached = checker.get_cached_update();
        (void)cached;
    }

    SECTION("has_update_available is thread-safe") {
        auto has_update = checker.has_update_available();
        (void)has_update;
    }
}

TEST_CASE("UpdateChecker lifecycle", "[update_checker][lifecycle]") {
    auto& checker = UpdateChecker::instance();

    SECTION("init is idempotent") {
        REQUIRE_NOTHROW(checker.init());
        REQUIRE_NOTHROW(checker.init());
    }

    SECTION("shutdown is idempotent") {
        REQUIRE_NOTHROW(checker.shutdown());
        REQUIRE_NOTHROW(checker.shutdown());
    }
}

// [slow]: the thread-neutrality assertion below compares live_thread_count()
// before and after, and that reads /proc/self/status "Threads:", which still
// counts a thread the kernel has not finished reaping. A correctly JOINED libhv
// loop therefore lingers in the count for a moment, so under the 96-way parallel
// shard pool this reports 9 == 8 and reads as the very leak it exists to catch
// (seen twice; passes 5/5 in isolation). Keeping it out of the parallel run
// preserves what it is for - naming a regression at its source rather than as a
// crash in an unrelated test (prestonbrown/helixscreen#1212) - without the false
// positive. A condition-based wait for the count to settle would let it come
// back into the default run.
TEST_CASE("UpdateChecker callback is optional", "[update_checker][callback][slow]") {
    // Hermetic by construction: no real network. The dev channel already reads
    // its endpoint from config (/update/dev_url) and is exempt from the rate
    // limiter, so pointing it at a closed loopback port drives the full
    // check_for_updates -> do_check -> fetch_dev_release -> report_result cycle
    // on a fast, local, deterministic connection refusal. Hitting api.github.com
    // instead made this test slow, network-dependent, and — because libhv's
    // requests:: client spins event-loop threads on a successful TLS connection
    // that outlive the call — the one test in the suite that leaked threads
    // (prestonbrown/helixscreen#1212). The isolation listener's per-TEST_CASE
    // reset_config_singleton() wipes these keys again for the next test.
    auto* config = Config::get_instance();
    REQUIRE(config != nullptr);
    // Dev and Beta are gated behind /beta_features — get_channel() reports Stable
    // for either one while beta is locked, which would send this check at the real
    // stable endpoint instead of the loopback port below.
    config->set<bool>("/beta_features", true);
    config->set<int>("/update/channel", 2); // Dev
    config->set<std::string>("/update/dev_url", "http://127.0.0.1:1/");

    auto& checker = UpdateChecker::instance();
    checker.init();
    checker.clear_cache();

    const int threads_before = helix::test::live_thread_count();
    REQUIRE(threads_before > 0);

    SECTION("nullptr callback survives the whole check cycle") {
        REQUIRE_NOTHROW(checker.check_for_updates(nullptr));

        // Wait for the worker to publish a terminal status. report_result()
        // stores status_ under the mutex BEFORE deferring to the LVGL thread,
        // so this flips as soon as the check body has run. A refused loopback
        // connect lands in ~10ms; the budget only has to exceed the request's
        // own 30s timeout so a host that silently drops (rather than refuses)
        // port 1 still reaches Error instead of flaking here.
        constexpr int POLL_ITERATIONS = 8000; // 8000 * 5ms = 40s
        for (int i = 0;
             i < POLL_ITERATIONS && checker.get_status() == UpdateChecker::Status::Checking; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // Pin that the check really ran end-to-end rather than short-circuiting
        // (a future early-return in check_for_updates would otherwise silently
        // gut this test): a refused connection lands on Status::Error.
        REQUIRE(checker.get_status() == UpdateChecker::Status::Error);

        // Run the deferred completion lambda on this (main) thread. That lambda
        // is where `if (callback) callback(...)` lives — draining it is what
        // actually exercises the nullptr-callback contract. Drop that guard and
        // invoking the empty std::function throws std::bad_function_call, which
        // process_pending() swallows by design; the exception counter is what
        // makes it visible here.
        const uint32_t exceptions_before =
            helix::ui::UpdateQueueTestAccess::callback_exception_count();
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        CHECK(helix::ui::UpdateQueueTestAccess::callback_exception_count() == exceptions_before);

        checker.shutdown();

        // The check must be thread-neutral. A synchronous HTTPS request through
        // libhv's requests:: client spins up event-loop threads that outlive the
        // call, and an unjoined loop later fires on freed state and crashes an
        // unrelated test (prestonbrown/helixscreen#1212).
        CHECK(helix::test::live_thread_count() == threads_before);
    }
}

// ============================================================================
// Real-World Scenario Tests
// ============================================================================

TEST_CASE("Real-world update scenarios", "[update_checker][scenarios]") {
    SECTION("typical GitHub release response") {
        // Simulates actual GitHub API response structure
        const char* github_response = R"({
            "url": "https://api.github.com/repos/prestonbrown/helixscreen/releases/12345",
            "html_url": "https://github.com/prestonbrown/helixscreen/releases/tag/v1.5.0",
            "id": 12345,
            "tag_name": "v1.5.0",
            "target_commitish": "main",
            "name": "HelixScreen v1.5.0",
            "draft": false,
            "prerelease": false,
            "created_at": "2025-01-20T08:00:00Z",
            "published_at": "2025-01-20T10:00:00Z",
            "body": "## What's New in v1.5.0\n\n### Features\n- Auto-update support\n- Improved touch calibration\n\n### Bug Fixes\n- Fixed memory leak in thumbnail cache",
            "assets": [
                {
                    "url": "https://api.github.com/repos/prestonbrown/helixscreen/releases/assets/100",
                    "id": 100,
                    "name": "helixscreen-1.5.0-arm64.tar.gz",
                    "size": 5242880,
                    "download_count": 42,
                    "browser_download_url": "https://github.com/prestonbrown/helixscreen/releases/download/v1.5.0/helixscreen-1.5.0-arm64.tar.gz"
                },
                {
                    "url": "https://api.github.com/repos/prestonbrown/helixscreen/releases/assets/101",
                    "id": 101,
                    "name": "sha256sums.txt",
                    "size": 128,
                    "download_count": 10,
                    "browser_download_url": "https://github.com/prestonbrown/helixscreen/releases/download/v1.5.0/sha256sums.txt"
                }
            ]
        })";

        auto release = parse_github_release(github_response);

        REQUIRE(release.valid);
        REQUIRE(release.version == "1.5.0");
        REQUIRE(release.tag_name == "v1.5.0");
        REQUIRE(release.download_url.find("helixscreen-1.5.0-arm64.tar.gz") != std::string::npos);
        REQUIRE(release.release_notes.find("Auto-update support") != std::string::npos);
    }

    SECTION("update from 1.4.0 to 1.5.0") {
        const std::string current = "1.4.0";
        const std::string latest = "1.5.0";

        REQUIRE(is_update_available(current, latest));

        // Verify version comparison logic
        auto current_v = parse_version(current);
        auto latest_v = parse_version(latest);
        REQUIRE(current_v.has_value());
        REQUIRE(latest_v.has_value());
        REQUIRE(latest_v->minor == current_v->minor + 1);
    }

    SECTION("no update when running development build ahead of release") {
        // Developer might be on 1.6.0-dev while latest release is 1.5.0
        const std::string current = "1.6.0";
        const std::string latest = "1.5.0";

        REQUIRE_FALSE(is_update_available(current, latest));
    }

    SECTION("update available for security patch") {
        // Security patches bump patch version
        const std::string current = "1.5.0";
        const std::string latest = "1.5.1";

        REQUIRE(is_update_available(current, latest));
    }
}

// ============================================================================
// Edge Cases and Boundary Conditions
// ============================================================================

TEST_CASE("Version edge cases", "[update_checker][edge]") {
    SECTION("major version zero (0.x.x)") {
        // Pre-1.0 versions should still compare correctly
        REQUIRE(is_update_available("0.1.0", "0.2.0"));
        REQUIRE(is_update_available("0.9.9", "1.0.0"));
        REQUIRE_FALSE(is_update_available("0.5.0", "0.5.0"));
    }

    SECTION("large version numbers") {
        REQUIRE(is_update_available("1.0.0", "100.0.0"));
        REQUIRE(is_update_available("99.99.99", "100.0.0"));
    }

    SECTION("version with build metadata") {
        // Build metadata should be ignored
        auto v1 = parse_version("1.0.0+build.123");
        auto v2 = parse_version("1.0.0+build.456");
        REQUIRE(v1.has_value());
        REQUIRE(v2.has_value());
        REQUIRE(*v1 == *v2);
    }

    SECTION("version with pre-release and build metadata") {
        auto v = parse_version("1.0.0-beta.1+sha.abc123");
        REQUIRE(v.has_value());
        REQUIRE(v->major == 1);
        REQUIRE(v->minor == 0);
        REQUIRE(v->patch == 0);
    }
}

// ============================================================================
// LVGL Subject Integration Tests
// ============================================================================

TEST_CASE("UpdateChecker subject initialization", "[update_checker][subjects]") {
    auto& checker = UpdateChecker::instance();
    checker.clear_cache();
    checker.init();

    SECTION("all subject accessors return non-null after init") {
        REQUIRE(checker.status_subject() != nullptr);
        REQUIRE(checker.version_text_subject() != nullptr);
        REQUIRE(checker.new_version_subject() != nullptr);
    }

    SECTION("integer subjects have correct initial values") {
        REQUIRE(lv_subject_get_int(checker.status_subject()) ==
                static_cast<int>(UpdateChecker::Status::Idle));
    }

    SECTION("string subjects start empty") {
        const char* version_text = lv_subject_get_string(checker.version_text_subject());
        REQUIRE(version_text != nullptr);
        REQUIRE(std::string(version_text).empty());

        const char* new_version = lv_subject_get_string(checker.new_version_subject());
        REQUIRE(new_version != nullptr);
        REQUIRE(std::string(new_version).empty());
    }

    checker.shutdown();
}

TEST_CASE("UpdateChecker subject accessors remain stable after shutdown",
          "[update_checker][subjects]") {
    auto& checker = UpdateChecker::instance();
    checker.clear_cache();
    checker.init();

    // Verify subjects exist before shutdown
    REQUIRE(checker.status_subject() != nullptr);

    checker.shutdown();

    // Accessors return member addresses, so they remain non-null even after shutdown.
    // (The subjects themselves are deinitialized, but the pointers are stable.)
    REQUIRE(checker.status_subject() != nullptr);
    REQUIRE(checker.version_text_subject() != nullptr);
    REQUIRE(checker.new_version_subject() != nullptr);
}

TEST_CASE("JSON edge cases", "[update_checker][json][edge]") {
    SECTION("unicode in release notes") {
        const char* json_str = R"({
            "tag_name": "v1.0.0",
            "body": "Fixed emoji display \ud83d\ude80 and Chinese chars \u4e2d\u6587"
        })";

        auto release = parse_github_release(json_str);
        REQUIRE(release.valid);
        REQUIRE_FALSE(release.release_notes.empty());
    }

    SECTION("very long release notes") {
        std::string long_body(10000, 'x');
        std::string json_str = R"({"tag_name": "v1.0.0", "body": ")" + long_body + R"("})";

        auto release = parse_github_release(json_str);
        REQUIRE(release.valid);
        REQUIRE(release.release_notes.length() == 10000);
    }

    SECTION("special characters in asset names") {
        const char* json_str = R"({
            "tag_name": "v1.0.0",
            "assets": [{
                "name": "helix screen_v1.0.0_(arm64).tar.gz",
                "browser_download_url": "https://example.com/release.tar.gz"
            }]
        })";

        auto release = parse_github_release(json_str);
        REQUIRE(release.valid);
        REQUIRE_FALSE(release.download_url.empty());
    }
}

// ============================================================================
// Download Status Types and Subjects
// ============================================================================

TEST_CASE("UpdateChecker download status enum values", "[update_checker]") {
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Idle) == 0);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Confirming) == 1);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Downloading) == 2);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Verifying) == 3);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Installing) == 4);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Complete) == 5);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Error) == 6);
    REQUIRE(static_cast<int>(UpdateChecker::DownloadStatus::Restarting) == 7);
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker report_download_status transitions to Restarting",
                 "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    checker.report_download_status(UpdateChecker::DownloadStatus::Restarting, 100,
                                   "v1.0.0 installed!");
    REQUIRE(checker.get_download_status() == UpdateChecker::DownloadStatus::Restarting);
    REQUIRE(checker.get_download_progress() == 100);

    // Reset back to idle for other tests
    checker.report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
    checker.shutdown();
}

TEST_CASE("UpdateChecker download state initial values", "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    REQUIRE(checker.get_download_status() == UpdateChecker::DownloadStatus::Idle);
    REQUIRE(checker.get_download_progress() == 0);
    REQUIRE(checker.get_download_error().empty());

    checker.shutdown();
}

TEST_CASE("UpdateChecker download subjects exist after init", "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    REQUIRE(checker.download_status_subject() != nullptr);
    REQUIRE(checker.download_progress_subject() != nullptr);
    REQUIRE(checker.download_text_subject() != nullptr);

    REQUIRE(lv_subject_get_int(checker.download_status_subject()) == 0);
    REQUIRE(lv_subject_get_int(checker.download_progress_subject()) == 0);

    checker.shutdown();
}

TEST_CASE("UpdateChecker get_download_path returns valid path", "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    auto path = checker.get_download_path();
    REQUIRE(!path.empty());
    REQUIRE(path.find("helixscreen-update.tar.gz") != std::string::npos);

    checker.shutdown();
}

// Helper: assert the staging dir is NOT within-or-equal-to install_root. This
// is the load-bearing safety invariant — TMP_DIR is rm -rf'd on cleanup AND the
// installer's --update flow (release.sh) does dotfile `rm -rf` inside INSTALL_DIR
// and `mv INSTALL_DIR ...` during the atomic swap. A staging dir under
// INSTALL_DIR would be deleted/relocated out from under the extract → wiped
// install falsely reported as success, or a rollback.
static void require_outside_install_root(const std::string& staging,
                                         const std::string& install_root) {
    REQUIRE(staging != install_root);
    REQUIRE(staging.rfind(install_root + "/", 0) != 0);
}

TEST_CASE("UpdateChecker compute_update_staging_dir derives a safe subdir", "[update_checker]") {
    using UC = UpdateChecker;

    // COLLISION: download dir == install root (the common self-update case,
    // e.g. both /home/pi/helixscreen). Staging MUST relocate to a SIBLING of
    // the install dir, never a subdir of it.
    {
        const std::string tarball = "/home/pi/helixscreen/helixscreen-update.tar.gz";
        const std::string install_root = "/home/pi/helixscreen";
        auto staging = UC::compute_update_staging_dir(tarball, install_root);
        REQUIRE(staging == "/home/pi/.helix-update-staging");
        require_outside_install_root(staging, install_root);
        REQUIRE(staging.find("/.helix-update-staging") != std::string::npos);
    }

    // COLLISION with a deeper download dir INSIDE the install root: still
    // relocate to the install root's parent (sibling of the install dir).
    {
        const std::string tarball = "/home/pi/helixscreen/dl/helixscreen-update.tar.gz";
        const std::string install_root = "/home/pi/helixscreen";
        auto staging = UC::compute_update_staging_dir(tarball, install_root);
        REQUIRE(staging == "/home/pi/.helix-update-staging");
        require_outside_install_root(staging, install_root);
    }

    // NON-COLLISION: download dir on a different partition than the install
    // root. Leave the base at the download dir — it's already outside.
    {
        const std::string tarball = "/data/helixscreen-update.tar.gz";
        const std::string install_root = "/home/pi/helixscreen";
        auto staging = UC::compute_update_staging_dir(tarball, install_root);
        REQUIRE(staging == "/data/.helix-update-staging");
        require_outside_install_root(staging, install_root);
    }

    // ANCESTOR: download dir is the PARENT of the install root. Already a
    // sibling location of the install dir — must NOT be relocated.
    {
        const std::string tarball = "/home/pi/helixscreen-update.tar.gz";
        const std::string install_root = "/home/pi/helixscreen";
        auto staging = UC::compute_update_staging_dir(tarball, install_root);
        REQUIRE(staging == "/home/pi/.helix-update-staging");
        require_outside_install_root(staging, install_root);
    }

    // Empty install_root (unknown): fall back to plain dirname behaviour.
    {
        const std::string tarball = "/data/helixscreen/helixscreen-update.tar.gz";
        auto staging = UC::compute_update_staging_dir(tarball, "");
        REQUIRE(staging == "/data/helixscreen/.helix-update-staging");
        REQUIRE(staging.find("//") == std::string::npos);
    }

    // Trailing slashes on install_root must not defeat the within-or-equal
    // comparison — still a collision, still relocated.
    {
        const std::string tarball = "/home/pi/helixscreen/helixscreen-update.tar.gz";
        auto staging = UC::compute_update_staging_dir(tarball, "/home/pi/helixscreen/");
        REQUIRE(staging == "/home/pi/.helix-update-staging");
    }

    // Tarball sitting at filesystem root, unknown install root: the directory
    // is "/". Result is "/.helix-update-staging" and must NOT equal "/".
    {
        const std::string tarball = "/helixscreen-update.tar.gz";
        auto staging = UC::compute_update_staging_dir(tarball, "");
        REQUIRE(staging == "/.helix-update-staging");
        REQUIRE(staging != "/");
    }

    // Bare filename (no directory component) resolves against "." rather than
    // producing a bare "/.helix-update-staging" at the root.
    {
        const std::string tarball = "helixscreen-update.tar.gz";
        auto staging = UC::compute_update_staging_dir(tarball, "");
        REQUIRE(staging == "./.helix-update-staging");
        REQUIRE(staging != ".");
    }
}

TEST_CASE("UpdateChecker required_download_space_bytes scales with download size",
          "[update_checker]") {
    using UC = UpdateChecker;

    // Unknown size → fixed default. Bounded both ways so the constant
    // can't silently drift back up and over-block tight-rootfs devices.
    auto unknown = UC::required_download_space_bytes(0);
    REQUIRE(unknown >= 120ULL * 1024 * 1024);
    REQUIRE(unknown <= 130ULL * 1024 * 1024);

    // Tiny download → safety floor
    auto tiny = UC::required_download_space_bytes(1024);
    REQUIRE(tiny >= 50ULL * 1024 * 1024);

    // Realistic 70 MB download → 1.2x + buffer ≈ 94 MB
    auto realistic = UC::required_download_space_bytes(70ULL * 1024 * 1024);
    REQUIRE(realistic > 70ULL * 1024 * 1024);
    REQUIRE(realistic < 110ULL * 1024 * 1024);

    // Large download → scales up
    auto large = UC::required_download_space_bytes(500ULL * 1024 * 1024);
    REQUIRE(large > 500ULL * 1024 * 1024);
}

TEST_CASE("statvfs result wider than 32-bit doesn't truncate", "[update_checker]") {
    // Regression: bundle D6LPLAYP reported "178.3 MB free" across a 60 GiB
    // rootfs with 46 GB actually free, because get_available_space() computed
    // f_bavail * f_frsize in size_t (32-bit on pi32/armhf/MIPS32) and the
    // product wrapped mod 2^32. The threshold check then over-blocked an
    // update on a device with tons of disk space.
    //
    // This test locks in the multiplication semantics. The values below are
    // taken from the bundle: 11,580,559 free 4 KiB blocks on a 60 GiB ext4.
    constexpr unsigned long blocks = 11'580'559UL;
    constexpr unsigned long frsize = 4096UL;

    // Correct: widen BEFORE multiplying. ~46 GB.
    const uint64_t bytes = static_cast<uint64_t>(blocks) * static_cast<uint64_t>(frsize);
    REQUIRE(bytes == 47'433'969'664ULL);
    REQUIRE(bytes / (1024ULL * 1024ULL) > 45'000ULL); // > 45 GiB

    // What the bug looked like: 32-bit truncated product reproduces the
    // ~181 MiB the user saw in the bundle. This branch is documentary —
    // if anyone ever reintroduces a narrow cast, the helper above is the
    // contract that must hold.
    const uint32_t truncated =
        static_cast<uint32_t>(static_cast<uint32_t>(blocks) * static_cast<uint32_t>(frsize));
    REQUIRE(truncated < 200U * 1024U * 1024U);
}

TEST_CASE("UpdateChecker get_platform_asset_name format", "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    auto name = checker.get_platform_asset_name();
    REQUIRE(name.find("helixscreen-") != std::string::npos);
    REQUIRE(name.find(".zip") != std::string::npos);

    checker.shutdown();
}

TEST_CASE_METHOD(HelixTestFixture, "UpdateChecker download requires cached update",
                 "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();
    checker.clear_cache();

    // Should not crash or start download without cached update
    checker.start_download();
    REQUIRE(checker.get_download_status() == UpdateChecker::DownloadStatus::Error);

    checker.shutdown();
}

TEST_CASE("UpdateChecker cancel_download sets cancelled flag", "[update_checker]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    checker.cancel_download();
    // Verify it doesn't crash and state is not Downloading
    REQUIRE(checker.get_download_status() != UpdateChecker::DownloadStatus::Downloading);

    checker.shutdown();
}

// Drive the published print_lifecycle subject the way production does. The
// lifecycle is published by PrinterPrintState::publish_lifecycle_state(), which
// runs only from update_from_status() and set_print_start_state() — writing
// print_state_enum directly leaves it stale.
static void drive_lifecycle(PrinterState& ps, const char* wire_state, PrintStartPhase phase) {
    ps.reset_print_start_state(); // force phase to IDLE so the next raise is a new print
    ps.update_from_status(json{{"print_stats", {{"state", wire_state}}}});
    ps.set_print_start_state(phase, "", 0);
    for (int i = 0; i < 8; ++i) {
        helix::ui::UpdateQueue::instance().drain();
    }
}

namespace {

/// These two drive the PROCESS-WIDE PrinterState, which HelixTestFixture does not
/// reset. A REQUIRE that fires before a trailing restore would throw and leave
/// print_lifecycle stuck at Preparing for every later test in the shard, which
/// then fails for a reason unrelated to its own subject. The destructor runs on
/// the throw path too.
struct GlobalPrintStateFixture : public HelixTestFixture {
    ~GlobalPrintStateFixture() override {
        auto& ps = get_printer_state();
        ps.reset_print_start_state();
        ps.update_from_status(nlohmann::json{{"print_stats", {{"state", "standby"}}}});
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }
};

} // namespace

TEST_CASE_METHOD(GlobalPrintStateFixture,
                 "UpdateChecker refuses to download while a job owns the machine",
                 "[update_checker][job-holds-machine]") {
    // update_install_suppressed() returns BEFORE the print guard and touches no
    // download state, so on a firmware-managed or read-only install tree the
    // guard is unreachable and everything below would pass vacuously. Assert the
    // precondition rather than assume it.
    REQUIRE_FALSE(update_install_suppressed());

    auto& state = get_printer_state();
    state.init_subjects(false); // no-op when an earlier test already did it
    auto& checker = UpdateChecker::instance();
    checker.init();
    // Deliberately NO cached update. The print guard runs first, so its refusal
    // is the one that must be reported; nothing here can reach the download
    // thread on either side of the guard.
    checker.clear_cache();

    // Host-side pre-print block: print_stats still reads standby while the
    // lifecycle is already Preparing.
    drive_lifecycle(state, "standby", PrintStartPhase::BED_MESH);
    REQUIRE(state.get_print_lifecycle() == PrintState::Preparing);

    checker.start_download();

    CHECK(checker.get_download_status() == UpdateChecker::DownloadStatus::Error);
    // The discriminator. Reverting the guard to `job_state == PRINTING ||
    // job_state == PAUSED` does not make start_download() succeed — it makes it
    // fall through to the no-cached-update branch, which also reports Error.
    // Only the message tells the two refusals apart.
    CHECK(checker.get_download_error() == "Stop the print before installing updates");

    checker.shutdown();
    drive_lifecycle(state, "standby", PrintStartPhase::IDLE);
}

TEST_CASE_METHOD(GlobalPrintStateFixture,
                 "UpdateChecker allows a download when no job owns the machine",
                 "[update_checker][job-holds-machine]") {
    // Negative control for the test above: with the printer idle the print guard
    // must not fire, so the refusal comes from the missing cache instead. Without
    // this, a guard that refused unconditionally would satisfy both.
    REQUIRE_FALSE(update_install_suppressed());

    auto& state = get_printer_state();
    state.init_subjects(false); // no-op when an earlier test already did it
    auto& checker = UpdateChecker::instance();
    checker.init();
    checker.clear_cache();

    drive_lifecycle(state, "standby", PrintStartPhase::IDLE);
    REQUIRE(state.get_print_lifecycle() == PrintState::Idle);

    checker.start_download();

    CHECK(checker.get_download_status() == UpdateChecker::DownloadStatus::Error);
    CHECK(checker.get_download_error() == "No update information cached");

    checker.shutdown();
}

TEST_CASE("UpdateChecker platform key defaults to pi in native build",
          "[update_checker][platform]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    auto name = checker.get_platform_asset_name();
    // In native builds (no HELIX_PLATFORM_* define), defaults to "pi"
    // Asset name format: helixscreen-{platform}.zip
    REQUIRE(name == "helixscreen-pi.zip");

    checker.shutdown();
}

// ============================================================================
// Dismissed Version Tests
// ============================================================================

TEST_CASE("UpdateChecker dismissed version logic", "[update_checker][dismissed]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    // Clear any previously dismissed version
    auto* config = Config::get_instance();
    if (config) {
        config->set<std::string>("/update/dismissed_version", "");
        config->save();
    }

    SECTION("is_version_dismissed returns false when no dismissed version in config") {
        REQUIRE_FALSE(checker.is_version_dismissed("1.2.0"));
    }

    SECTION("is_version_dismissed returns true when version matches dismissed") {
        if (config) {
            config->set<std::string>("/update/dismissed_version", "1.2.0");
            config->save();
        }
        REQUIRE(checker.is_version_dismissed("1.2.0"));
    }

    SECTION("is_version_dismissed returns false for newer version than dismissed") {
        if (config) {
            config->set<std::string>("/update/dismissed_version", "1.2.0");
            config->save();
        }
        REQUIRE_FALSE(checker.is_version_dismissed("1.3.0"));
    }

    SECTION("is_version_dismissed returns true for older version than dismissed") {
        if (config) {
            config->set<std::string>("/update/dismissed_version", "1.2.0");
            config->save();
        }
        REQUIRE(checker.is_version_dismissed("1.1.0"));
    }

    SECTION("dismiss_current_version persists to config") {
        // We need a cached update for dismiss_current_version to work
        // Since we can't easily set cached_info_ without a real check,
        // test via the config path directly
        // This tests the config interaction pattern
        if (config) {
            auto dismissed = config->get<std::string>("/update/dismissed_version", "");
            // After clearing, should be empty
            REQUIRE(dismissed.empty());
        }
    }

    checker.shutdown();
}

// ============================================================================
// Auto-Check Timer Tests
// ============================================================================

TEST_CASE("UpdateChecker auto-check timer lifecycle", "[update_checker][auto_check]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    SECTION("start_auto_check creates timer (returns without crash)") {
        REQUIRE_NOTHROW(checker.start_auto_check());
        // Clean up
        checker.stop_auto_check();
    }

    SECTION("stop_auto_check cleans up timer") {
        checker.start_auto_check();
        REQUIRE_NOTHROW(checker.stop_auto_check());
    }

    SECTION("double start_auto_check is safe (idempotent)") {
        REQUIRE_NOTHROW(checker.start_auto_check());
        REQUIRE_NOTHROW(checker.start_auto_check());
        checker.stop_auto_check();
    }

    SECTION("stop_auto_check before start_auto_check is safe") {
        REQUIRE_NOTHROW(checker.stop_auto_check());
    }

    SECTION("stop_auto_check after stop_auto_check is safe") {
        checker.start_auto_check();
        REQUIRE_NOTHROW(checker.stop_auto_check());
        REQUIRE_NOTHROW(checker.stop_auto_check());
    }

    checker.shutdown();
}

TEST_CASE("UpdateChecker notification subjects exist after init", "[update_checker][auto_check]") {
    auto& checker = UpdateChecker::instance();
    checker.init();

    SECTION("release_notes_subject returns non-null") {
        REQUIRE(checker.release_notes_subject() != nullptr);
    }

    SECTION("changelog_visible_subject returns non-null") {
        REQUIRE(checker.changelog_visible_subject() != nullptr);
    }

    SECTION("changelog_visible starts at 0") {
        REQUIRE(lv_subject_get_int(checker.changelog_visible_subject()) == 0);
    }

    SECTION("release_notes starts empty") {
        const char* notes = lv_subject_get_string(checker.release_notes_subject());
        REQUIRE(notes != nullptr);
        REQUIRE(std::string(notes).empty());
    }

    checker.shutdown();
}

// ============================================================================
// Installer Resolution Tests (tarball extraction preference)
// ============================================================================

namespace {

// Helper to create a temp directory
std::string make_temp_dir(const std::string& prefix) {
    std::string tmpl = "/tmp/" + prefix + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = mkdtemp(buf.data());
    return result ? std::string(result) : "";
}

// Helper to create a file with content and optional +x permission
void create_file(const std::string& path, const std::string& content, bool executable = false) {
    std::ofstream f(path);
    f << content;
    f.close();
    if (executable) {
        chmod(path.c_str(), 0755);
    }
}

// Helper to recursively remove a directory
void remove_dir(const std::string& path) {
    std::string cmd = "rm -rf " + path;
    std::system(cmd.c_str());
}

} // anonymous namespace

TEST_CASE("find_local_installer with custom search paths", "[update_checker][installer]") {
    auto tmp = make_temp_dir("helix_test_installer");
    REQUIRE(!tmp.empty());

    SECTION("finds installer in extra search path") {
        std::string installer_path = tmp + "/install.sh";
        create_file(installer_path, "#!/bin/sh\necho test\n", true);

        auto found = UpdateChecker::find_local_installer({installer_path});
        REQUIRE(found == installer_path);
    }

    SECTION("extra search paths take priority over well-known paths") {
        std::string installer_path = tmp + "/install.sh";
        create_file(installer_path, "#!/bin/sh\necho custom\n", true);

        auto found = UpdateChecker::find_local_installer({installer_path});
        // Should find our custom path, not a well-known one
        REQUIRE(found == installer_path);
    }

    SECTION("returns empty when no installer exists") {
        // Search only in our empty temp dir — nothing executable there
        std::string nonexistent = tmp + "/nonexistent/install.sh";
        auto found = UpdateChecker::find_local_installer({nonexistent});
        // The key test: nonexistent path is NOT returned
        REQUIRE(found != nonexistent);
    }

    SECTION("skips non-executable files") {
        std::string installer_path = tmp + "/install.sh";
        create_file(installer_path, "#!/bin/sh\necho test\n", false); // NOT executable

        auto found = UpdateChecker::find_local_installer({installer_path});
        // Should not find the non-executable file
        REQUIRE(found != installer_path);
    }

    SECTION("finds first executable in multiple extra paths") {
        std::string first = tmp + "/first_install.sh";
        std::string second = tmp + "/second_install.sh";
        create_file(first, "#!/bin/sh\necho first\n", true);
        create_file(second, "#!/bin/sh\necho second\n", true);

        auto found = UpdateChecker::find_local_installer({first, second});
        REQUIRE(found == first);
    }

    SECTION("skips missing first path, finds second") {
        std::string missing = tmp + "/missing_install.sh";
        std::string present = tmp + "/present_install.sh";
        create_file(present, "#!/bin/sh\necho here\n", true);

        auto found = UpdateChecker::find_local_installer({missing, present});
        REQUIRE(found == present);
    }

    remove_dir(tmp);
}

TEST_CASE("Tarball installer extraction creates correct structure", "[update_checker][installer]") {
    // Test that a tarball containing helixscreen/install.sh can be extracted
    // and the extracted installer is usable
    auto tmp = make_temp_dir("helix_test_tarball");
    REQUIRE(!tmp.empty());

    SECTION("tarball with install.sh can be extracted") {
        // Create the directory structure: helixscreen/install.sh
        std::string inner_dir = tmp + "/helixscreen";
        mkdir(inner_dir.c_str(), 0755);
        create_file(inner_dir + "/install.sh", "#!/bin/sh\nexit 0\n", true);

        // Create tarball
        std::string tarball_path = tmp + "/test.tar.gz";
        std::string cmd = "tar czf " + tarball_path + " -C " + tmp + " helixscreen/install.sh";
        REQUIRE(std::system(cmd.c_str()) == 0);

        // Extract to a new location (simulating what do_install does)
        std::string extract_dir = tmp + "/extracted";
        mkdir(extract_dir.c_str(), 0750);

        std::string extract_cmd =
            "tar xzf " + tarball_path + " -C " + extract_dir + " helixscreen/install.sh";
        REQUIRE(std::system(extract_cmd.c_str()) == 0);

        // Verify the extracted installer exists and is readable
        std::string extracted_installer = extract_dir + "/helixscreen/install.sh";
        REQUIRE(access(extracted_installer.c_str(), R_OK) == 0);

        // Make it executable (as do_install does)
        chmod(extracted_installer.c_str(), 0755);
        REQUIRE(access(extracted_installer.c_str(), X_OK) == 0);

        // Verify content matches
        std::ifstream f(extracted_installer);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        REQUIRE(content.find("#!/bin/sh") != std::string::npos);
        REQUIRE(content.find("exit 0") != std::string::npos);
    }

    SECTION("tarball without install.sh triggers fallback") {
        // Create a tarball with only the binary, no install.sh
        std::string inner_dir = tmp + "/helixscreen/bin";
        std::string mkdir_cmd = "mkdir -p " + inner_dir;
        std::system(mkdir_cmd.c_str());
        create_file(inner_dir + "/helix-screen", "fake-binary", false);

        std::string tarball_path = tmp + "/no-installer.tar.gz";
        std::string cmd =
            "tar czf " + tarball_path + " -C " + tmp + " helixscreen/bin/helix-screen";
        REQUIRE(std::system(cmd.c_str()) == 0);

        // Try to extract install.sh — should fail
        std::string extract_dir = tmp + "/extracted2";
        mkdir(extract_dir.c_str(), 0750);

        std::string extract_cmd = "tar xzf " + tarball_path + " -C " + extract_dir +
                                  " helixscreen/install.sh 2>/dev/null";
        int ret = std::system(extract_cmd.c_str());
        // tar returns non-zero when the specified member doesn't exist
        REQUIRE(ret != 0);

        // Extracted installer should not exist
        std::string extracted_installer = extract_dir + "/helixscreen/install.sh";
        REQUIRE(access(extracted_installer.c_str(), R_OK) != 0);
    }

    remove_dir(tmp);
}

// ============================================================================
// extract_installer_from_tarball tests
//
// These tests exercise the actual production code path that was silently broken
// by the gunzip -k incompatibility on older BusyBox. They call
// UpdateChecker::extract_installer_from_tarball() directly to verify the logic
// that do_install() depends on.
// ============================================================================

namespace {

// Resolve path to tests/fixtures/update/ using __FILE__.
// __FILE__ may be absolute or relative depending on build system.
std::string get_update_fixture_dir() {
    std::string src = __FILE__;

    // Handle both absolute (".../tests/unit/...") and relative ("tests/unit/...") paths
    auto pos = src.rfind("/tests/unit/");
    if (pos != std::string::npos) {
        return src.substr(0, pos) + "/tests/fixtures/update/";
    }

    // Relative path starting with "tests/unit/"
    if (src.find("tests/unit/") == 0) {
        return "tests/fixtures/update/";
    }

    return "";
}

} // namespace

TEST_CASE("extract_installer_from_tarball: tarball with install.sh",
          "[update_checker][installer][do_install]") {
    auto tmp = make_temp_dir("helix_extract_test");
    REQUIRE(!tmp.empty());

    SECTION("extracts installer from a well-formed release tarball") {
        // Build a minimal release tarball: helixscreen/install.sh
        std::string inner = tmp + "/helixscreen";
        mkdir(inner.c_str(), 0755);
        create_file(inner + "/install.sh", "#!/bin/sh\nexit 0\n", true);

        std::string tarball = tmp + "/release.tar.gz";
        std::string cmd =
            "cd " + tmp + " && COPYFILE_DISABLE=1 tar czf release.tar.gz helixscreen/install.sh";
        REQUIRE(std::system(cmd.c_str()) == 0);

        std::string extract_dir = tmp + "/extracted";
        mkdir(extract_dir.c_str(), 0750);

        auto result = UpdateChecker::extract_installer_from_tarball(tarball, extract_dir);

        REQUIRE(!result.empty());
        REQUIRE(result.find("install.sh") != std::string::npos);
        REQUIRE(access(result.c_str(), X_OK) == 0); // must be executable after extraction
    }

    SECTION("returns empty when install.sh is absent from tarball") {
        // Tarball with only the binary — no install.sh (replicates the CC1 packaging bug)
        std::string inner = tmp + "/helixscreen/bin";
        std::string mkdircmd = "mkdir -p " + inner;
        std::system(mkdircmd.c_str());
        create_file(inner + "/helix-screen", "fake-binary", false);

        std::string tarball = tmp + "/no-installer.tar.gz";
        std::string cmd = "cd " + tmp +
                          " && COPYFILE_DISABLE=1 tar czf no-installer.tar.gz"
                          " helixscreen/bin/helix-screen";
        REQUIRE(std::system(cmd.c_str()) == 0);

        std::string extract_dir = tmp + "/extracted2";
        mkdir(extract_dir.c_str(), 0750);

        auto result = UpdateChecker::extract_installer_from_tarball(tarball, extract_dir);
        REQUIRE(result.empty()); // no installer → empty, triggers find_local_installer fallback
    }

    SECTION("returns empty when tarball does not exist") {
        std::string extract_dir = tmp + "/extracted3";
        mkdir(extract_dir.c_str(), 0750);

        auto result =
            UpdateChecker::extract_installer_from_tarball(tmp + "/nonexistent.tar.gz", extract_dir);
        REQUIRE(result.empty());
    }

    SECTION("extracted installer is chmod +x regardless of permissions in archive") {
        std::string inner = tmp + "/helixscreen";
        mkdir(inner.c_str(), 0755);
        // Create install.sh without +x — extract_installer_from_tarball must chmod it
        create_file(inner + "/install.sh", "#!/bin/sh\nexit 0\n", false);

        std::string tarball = tmp + "/no-exec.tar.gz";
        std::string cmd =
            "cd " + tmp + " && COPYFILE_DISABLE=1 tar czf no-exec.tar.gz helixscreen/install.sh";
        REQUIRE(std::system(cmd.c_str()) == 0);

        std::string extract_dir = tmp + "/extracted4";
        mkdir(extract_dir.c_str(), 0750);

        auto result = UpdateChecker::extract_installer_from_tarball(tarball, extract_dir);
        REQUIRE(!result.empty());
        REQUIRE(access(result.c_str(), X_OK) == 0); // function must have chmod +x'd it
    }

    remove_dir(tmp);
}

TEST_CASE("extract_installer_from_tarball: committed fixture tarballs",
          "[update_checker][installer][do_install]") {
    std::string fixture_dir = get_update_fixture_dir();
    REQUIRE(!fixture_dir.empty());

    SECTION("fixture WITH install.sh extracts successfully") {
        std::string tarball = fixture_dir + "helixscreen-pi-v99.0.0-test.tar.gz";
        if (access(tarball.c_str(), R_OK) != 0) {
            FAIL("Fixture file missing: " + tarball);
        }

        auto tmp = make_temp_dir("helix_fixture_ok");
        REQUIRE(!tmp.empty());

        auto result = UpdateChecker::extract_installer_from_tarball(tarball, tmp);
        REQUIRE(!result.empty());
        REQUIRE(access(result.c_str(), X_OK) == 0);

        remove_dir(tmp);
    }

    SECTION("fixture WITHOUT install.sh returns empty (replicates CC1 packaging bug)") {
        std::string tarball = fixture_dir + "helixscreen-pi-v99.0.0-no-installer.tar.gz";
        if (access(tarball.c_str(), R_OK) != 0) {
            FAIL("Fixture file missing: " + tarball);
        }

        auto tmp = make_temp_dir("helix_fixture_noinst");
        REQUIRE(!tmp.empty());

        // This is the exact failure mode CC1 users hit before the packaging fix:
        // tarball exists, install.sh is missing, do_install falls back to
        // find_local_installer() which returns "" on a fresh device → "Installer not found"
        auto result = UpdateChecker::extract_installer_from_tarball(tarball, tmp);
        REQUIRE(result.empty());

        remove_dir(tmp);
    }
}

TEST_CASE("extract_installer_from_tarball: works with empty PATH (systemd regression)",
          "[update_checker][installer][do_install][path]") {
    // Regression test for the Pi "Installer not found" bug:
    // systemd services run with a minimal PATH that may not include /usr/bin or /bin.
    // extract_installer_from_tarball must use absolute tool paths (via resolve_tool),
    // not bare names that depend on $PATH. If it uses bare names, execvp("tar", ...)
    // exits 127 → extraction fails → "Installer not found".

    std::string fixture_dir = get_update_fixture_dir();
    REQUIRE(!fixture_dir.empty());

    std::string tarball = fixture_dir + "helixscreen-pi-v99.0.0-test.tar.gz";
    if (access(tarball.c_str(), R_OK) != 0) {
        FAIL("Fixture file missing: " + tarball);
    }

    auto tmp = make_temp_dir("helix_path_test");
    REQUIRE(!tmp.empty());

    // Save and clear PATH to simulate a minimal systemd environment
    const char* original_path = std::getenv("PATH");
    setenv("PATH", "", 1); // empty PATH — bare execvp("tar",...) would fail

    auto result = UpdateChecker::extract_installer_from_tarball(tarball, tmp);

    // Restore PATH before assertions (even if they fail)
    if (original_path) {
        setenv("PATH", original_path, 1);
    } else {
        unsetenv("PATH");
    }

    remove_dir(tmp);

    // Must succeed: resolve_tool() finds tar/cp/gunzip via absolute paths
    REQUIRE(!result.empty());
}

// ============================================================================
// Platform Key & Architecture Validation
// ============================================================================

TEST_CASE("get_platform_key returns a known platform", "[update_checker][platform]") {
    std::string platform = UpdateChecker::get_platform_key();
    REQUIRE(!platform.empty());

    // Must be one of the supported platform keys. Keep in sync with the
    // release matrix in .github/workflows/release.yml and the #ifdef ladder in
    // UpdateChecker::get_platform_key(). Adding a platform without an entry
    // here — AND a matching #elif in get_platform_key — silently bricks
    // in-app updates for that platform (falls through to "pi", so the device
    // downloads the Pi tarball and ends up with missing shared libs).
    std::vector<std::string> known_platforms = {"pi", "pi32", "x86", "ad5m",  "k1",
                                                "k2", "ad5x", "cc1", "esp32", "snapmaker-u1"};
    bool found = false;
    for (const auto& p : known_platforms) {
        if (platform == p) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("get_platform_key matches compiled binary architecture",
          "[update_checker][platform][arch]") {
#ifndef __linux__
    SKIP("Linux-only: requires /proc/self/exe and ELF binary format");
#else
    // The platform key must agree with what we actually compiled as.
    // This catches the bug where uname() returned "aarch64" on a pi32 build.
    std::string platform = UpdateChecker::get_platform_key();

    // Check that our own binary's ELF class matches the platform expectation
    FILE* f = fopen("/proc/self/exe", "rb");
    REQUIRE(f != nullptr);

    unsigned char elf_header[20];
    size_t n = fread(elf_header, 1, 20, f);
    fclose(f);
    REQUIRE(n == 20);

    // Verify ELF magic
    REQUIRE(elf_header[0] == 0x7F);
    REQUIRE(elf_header[1] == 'E');
    REQUIRE(elf_header[2] == 'L');
    REQUIRE(elf_header[3] == 'F');

    uint8_t elf_class = elf_header[4]; // 1 = 32-bit, 2 = 64-bit

    if (platform == "pi32" || platform == "ad5m") {
        REQUIRE(elf_class == 1); // ELFCLASS32
    } else if (platform == "pi") {
        REQUIRE(elf_class == 2); // ELFCLASS64
    }
    // Other platforms (k1, k2, ad5x, cc1) may vary — no assertion
#endif
}

TEST_CASE("get_platform_display_name returns non-empty string for all known platforms",
          "[update_checker][platform]") {
    // Mirror the known_platforms list from "get_platform_key returns a known platform".
    // Every key that get_platform_key() can return MUST have a display name.
    // Keep in sync with platform_canonical_model in debug_bundle_collector.cpp
    // (and UpdateChecker::get_platform_display_name once centralised).
    std::vector<std::string> known_platforms = {"pi", "pi32", "x86", "ad5m",  "k1",
                                                "k2", "ad5x", "cc1", "esp32", "snapmaker-u1"};

    for (const auto& key : known_platforms) {
        INFO("platform key: " << key);
        std::string name = UpdateChecker::get_platform_display_name(key);
        REQUIRE(!name.empty());
    }
}

TEST_CASE("get_platform_display_name returns correct strings for known platforms",
          "[update_checker][platform]") {
    // Exact display name strings — changing them breaks debug bundle dashboard parsing.
    REQUIRE(UpdateChecker::get_platform_display_name("pi") == "Raspberry Pi");
    REQUIRE(UpdateChecker::get_platform_display_name("pi32") == "Raspberry Pi (32-bit)");
    REQUIRE(UpdateChecker::get_platform_display_name("x86") == "x86 Desktop");
    REQUIRE(UpdateChecker::get_platform_display_name("ad5m") == "FlashForge Adventurer 5M");
    REQUIRE(UpdateChecker::get_platform_display_name("ad5x") == "FlashForge Adventurer 5X");
    REQUIRE(UpdateChecker::get_platform_display_name("k1") == "Creality K1");
    REQUIRE(UpdateChecker::get_platform_display_name("k2") == "Creality K2 Plus");
    REQUIRE(UpdateChecker::get_platform_display_name("cc1") == "Elegoo Centauri Carbon");
    REQUIRE(UpdateChecker::get_platform_display_name("snapmaker-u1") == "Snapmaker U1");
    REQUIRE(UpdateChecker::get_platform_display_name("esp32") == "BTT K-Touch");
    // Unknown keys fall back to the key itself.
    REQUIRE(UpdateChecker::get_platform_display_name("unknown-platform") == "unknown-platform");
}

// ============================================================================
// Zip integrity verification (prestonbrown/helixscreen#993)
//
// Release archives ship as .zip, and `unzip -t` support depends on the
// firmware's BusyBox vintage. Verified on-device:
//
//   BusyBox 1.29.3 (FlashForge AD5M)    no -t: "invalid option -- 't'"
//   BusyBox 1.31.1 (Creality K1)        no -t: "invalid option -- 't'"
//   BusyBox 1.36.1 (Elegoo Centauri)    -t present and correct
//   info-zip 6.00  (Debian/Pi/desktop)  -t present and correct
//
// Using `unzip -t` as the primary check therefore failed every AD5M and K1
// update on an intact download ("Error: Corrupt download").
// verify_zip_integrity() prefers python3's zipfile.testzip(), which behaves
// identically everywhere.
//
// NOTE: the Unverifiable path (python present but built without zlib, as on the
// AD5M, or no tools at all) cannot be exercised here -- verify_zip_integrity
// resolves python3/unzip from absolute system directories, so a test cannot
// shadow them via PATH. That path is covered by the installer's bats suite
// ("python has no zlib (AD5M)") and was verified on the device itself.
// ============================================================================

namespace {

/// True when a python3 capable of building zip fixtures is on this system.
bool zip_fixture_tooling_available() {
    return std::system("python3 -c 'import zipfile, zlib' >/dev/null 2>&1") == 0;
}

/// Run a python snippet with `path` as argv[1]. Returns true on exit 0.
bool run_python_fixture(const std::string& script, const std::string& path) {
    // Fixture construction only — the code under test never uses a shell.
    std::string cmd = "python3 -c \"" + script + "\" '" + path + "' >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

/// Build a zip containing one deflated, incompressible member.
bool make_valid_zip(const std::string& path) {
    return run_python_fixture("import os,sys,zipfile;"
                              "z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED);"
                              "z.writestr('bin/helix-screen', b'\\x7fELF'+os.urandom(65536));"
                              "z.close()",
                              path);
}

/// Flip a byte in the middle of the compressed payload. The archive stays
/// structurally valid, so only a real per-entry CRC test can catch it — this is
/// exactly what BusyBox 1.36's no-op `-t` waves through.
bool corrupt_zip_payload(const std::string& path) {
    return run_python_fixture("import sys;"
                              "p=sys.argv[1];"
                              "d=bytearray(open(p,'rb').read());"
                              "d[len(d)//2]^=0xFF;"
                              "open(p,'wb').write(bytes(d))",
                              path);
}

/// Truncate the file, destroying the end-of-central-directory record.
bool truncate_zip(const std::string& path) {
    return run_python_fixture("import sys;"
                              "p=sys.argv[1];"
                              "d=open(p,'rb').read();"
                              "open(p,'wb').write(d[:len(d)//2])",
                              path);
}

std::string zip_fixture_path(const char* name) {
    return std::string("/tmp/helix_zip_fixture_") + name + ".zip";
}

} // namespace

TEST_CASE("verify_zip_integrity accepts an intact zip", "[update_checker][zip][993]") {
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto path = zip_fixture_path("good");
    REQUIRE(make_valid_zip(path));

    REQUIRE(UpdateChecker::verify_zip_integrity(path) == UpdateChecker::ZipIntegrity::Ok);
    std::remove(path.c_str());
}

TEST_CASE("verify_zip_integrity rejects a CRC-corrupt zip", "[update_checker][zip][993]") {
    // The archive is structurally intact — catching this REQUIRES a real
    // per-entry CRC test, not `unzip -t` on BusyBox 1.36.
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto path = zip_fixture_path("crc");
    REQUIRE(make_valid_zip(path));
    REQUIRE(corrupt_zip_payload(path));

    REQUIRE(UpdateChecker::verify_zip_integrity(path) == UpdateChecker::ZipIntegrity::Corrupt);
    std::remove(path.c_str());
}

TEST_CASE("verify_zip_integrity rejects a truncated zip", "[update_checker][zip][993]") {
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto path = zip_fixture_path("trunc");
    REQUIRE(make_valid_zip(path));
    REQUIRE(truncate_zip(path));

    REQUIRE(UpdateChecker::verify_zip_integrity(path) == UpdateChecker::ZipIntegrity::Corrupt);
    std::remove(path.c_str());
}

TEST_CASE("verify_zip_integrity rejects non-zip and missing files", "[update_checker][zip][993]") {
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }

    SECTION("HTML error page saved with a .zip name") {
        // What a CDN 404/504 actually leaves on disk — issue #993's original
        // "File is not a zip file" report.
        const auto path = zip_fixture_path("html");
        std::ofstream f(path);
        f << "<!DOCTYPE html><html><body>504 Gateway Timeout</body></html>";
        f.close();

        REQUIRE(UpdateChecker::verify_zip_integrity(path) == UpdateChecker::ZipIntegrity::Corrupt);
        std::remove(path.c_str());
    }

    SECTION("empty file") {
        const auto path = zip_fixture_path("empty");
        std::ofstream f(path);
        f.close();

        REQUIRE(UpdateChecker::verify_zip_integrity(path) == UpdateChecker::ZipIntegrity::Corrupt);
        std::remove(path.c_str());
    }

    SECTION("nonexistent path") {
        REQUIRE(UpdateChecker::verify_zip_integrity("/tmp/helix_zip_fixture_does_not_exist.zip") ==
                UpdateChecker::ZipIntegrity::Corrupt);
    }
}

TEST_CASE("verify_zip_integrity never reports Unverifiable when python3 exists",
          "[update_checker][zip][993]") {
    // Unverifiable must be reserved for systems with neither python3 nor unzip.
    // On such a system the caller falls back to SHA256 rather than failing the
    // update — but a normal device must always get a definitive answer.
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto path = zip_fixture_path("definitive");
    REQUIRE(make_valid_zip(path));

    REQUIRE(UpdateChecker::verify_zip_integrity(path) != UpdateChecker::ZipIntegrity::Unverifiable);
    std::remove(path.c_str());
}

// ============================================================================
// Zip member extraction / tool availability
//
// Not every platform ships `unzip`: the Creality K2's OpenWrt firmware has no
// unzip binary and no BusyBox unzip applet, only python3 with zipfile+zlib.
// Demanding unzip made every in-app update there fail before downloading.
//
// NOTE: these tests exercise whichever tool the host actually has (unzip on
// dev/CI machines). The python fallback cannot be forced here — the
// implementation resolves tools from absolute system directories, so a test
// cannot shadow them via PATH. That branch was verified on K2 hardware.
// ============================================================================

TEST_CASE("available_zip_tool finds a usable tool on this system", "[update_checker][zip]") {
    // A dev/CI host has unzip, python3, or both; None would mean zip releases
    // are uninstallable here.
    REQUIRE(UpdateChecker::available_zip_tool() != UpdateChecker::ZipTool::None);
}

TEST_CASE("extract_zip_member extracts a member's exact contents", "[update_checker][zip]") {
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto zip = zip_fixture_path("extract");
    const std::string dir = "/tmp/helix_zip_fixture_extract_dir";
    std::system(("rm -rf '" + dir + "'").c_str());
    REQUIRE(mkdir(dir.c_str(), 0750) == 0);

    // Build a zip holding a shell member with known contents.
    REQUIRE(run_python_fixture("import sys,zipfile;"
                               "z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED);"
                               "z.writestr('helixscreen/install.sh','#!/bin/sh\\necho hi\\n');"
                               "z.close()",
                               zip));

    REQUIRE(UpdateChecker::extract_zip_member(zip, dir, "helixscreen/install.sh") == 0);

    std::ifstream f(dir + "/helixscreen/install.sh");
    REQUIRE(f.good());
    std::string contents((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    REQUIRE(contents == "#!/bin/sh\necho hi\n");

    std::system(("rm -rf '" + dir + "'").c_str());
    std::remove(zip.c_str());
}

TEST_CASE("extract_zip_member leaves an extracted installer executable", "[update_checker][zip]") {
    // An install.sh or bin/helix-screen that lands without its exec bit is
    // useless — the updater runs it straight after extraction.
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto zip = zip_fixture_path("mode");
    const std::string dir = "/tmp/helix_zip_fixture_mode_dir";
    std::system(("rm -rf '" + dir + "'").c_str());
    REQUIRE(mkdir(dir.c_str(), 0750) == 0);

    // Store the member with no mode bits at all, the hostile case for the
    // python path (zipfile.extract() would leave it 0600).
    REQUIRE(run_python_fixture("import sys,zipfile;"
                               "z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED);"
                               "z.writestr('bin/helix-screen','#!/bin/sh\\nexit 0\\n');"
                               "z.close()",
                               zip));

    REQUIRE(UpdateChecker::extract_zip_member(zip, dir, "bin/helix-screen") == 0);
    REQUIRE(access((dir + "/bin/helix-screen").c_str(), X_OK) == 0);

    std::system(("rm -rf '" + dir + "'").c_str());
    std::remove(zip.c_str());
}

TEST_CASE("extract_zip_member fails for a member that isn't in the archive",
          "[update_checker][zip]") {
    if (!zip_fixture_tooling_available()) {
        SKIP("python3 with zipfile/zlib required to build zip fixtures");
    }
    const auto zip = zip_fixture_path("absent");
    const std::string dir = "/tmp/helix_zip_fixture_absent_dir";
    std::system(("rm -rf '" + dir + "'").c_str());
    REQUIRE(mkdir(dir.c_str(), 0750) == 0);
    REQUIRE(make_valid_zip(zip));

    REQUIRE(UpdateChecker::extract_zip_member(zip, dir, "no/such/member") != 0);

    std::system(("rm -rf '" + dir + "'").c_str());
    std::remove(zip.c_str());
}

// ============================================================================
// release_info.json self-repair (prestonbrown/helixscreen#993)
// ============================================================================
//
// Moonraker's type:web updater downloads the release asset named by
// release_info.json's asset_name. A missing or stale name makes Moonraker fall
// back to the alphabetically-FIRST asset on the release -- never a zip -- and
// extraction dies with "File is not a zip file". The file was written once at
// install time and never revalidated, so a bad value permanently blocked the
// very update that would have repaired it. UpdateChecker::repair_release_info()
// re-derives asset_name from the platform key at every boot.

namespace {

std::string expected_asset_name() {
    return "helixscreen-" + UpdateChecker::get_platform_key() + ".zip";
}

// Read a whole file; returns "" when it cannot be opened.
std::string read_all(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open())
        return "";
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

nlohmann::json read_json(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    return nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
}

// Make <root> look like a deployed install so repair_release_info() is willing
// to CREATE a missing release_info.json there.
void make_deployed_layout(const std::string& root) {
    REQUIRE(mkdir((root + "/bin").c_str(), 0755) == 0);
    create_file(root + "/bin/helix-screen", "#!/bin/sh\nexit 0\n", true);
}

} // anonymous namespace

TEST_CASE("repair_release_info: empty install root is a no-op", "[update_checker][release_info]") {
    // Bind-mounted layouts resolve to "". Must not crash, must not guess a path.
    REQUIRE(UpdateChecker::repair_release_info("") == UpdateChecker::ReleaseInfoRepair::Absent);
}

TEST_CASE("repair_release_info: missing file in a non-deployed tree is left alone",
          "[update_checker][release_info]") {
    // A dev build resolves its install root to the SOURCE CHECKOUT. Creating the
    // file whenever it is absent would drop an untracked release_info.json into
    // the repo on every run.
    auto tmp = make_temp_dir("helix_relinfo_nodeploy");
    REQUIRE(!tmp.empty());

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Absent);
    REQUIRE_FALSE(std::filesystem::exists(tmp + "/release_info.json"));

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: missing file in a deployed install is created",
          "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_missing");
    REQUIRE(!tmp.empty());
    make_deployed_layout(tmp);

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);

    auto j = read_json(tmp + "/release_info.json");
    REQUIRE(j.is_object());
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());
    CHECK(j["project_name"].get<std::string>() == "helixscreen");
    CHECK(j["project_owner"].get<std::string>() == "prestonbrown");
    // No prior version to preserve -- reconstructed from the running build.
    REQUIRE(j.contains("version"));
    CHECK(j["version"].get<std::string>().rfind("v", 0) == 0);

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: empty file is rewritten", "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_empty");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";
    create_file(path, "");

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);

    auto j = read_json(path);
    REQUIRE(j.is_object());
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: truncated/malformed JSON is rewritten",
          "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_malformed");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";
    // Power-cut mid-write: a real half-object, not just garbage bytes.
    create_file(path, "{\"project_name\":\"helixscreen\",\"asset_na");

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);

    auto j = read_json(path);
    REQUIRE(j.is_object());
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());
    CHECK(j["project_name"].get<std::string>() == "helixscreen");

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: valid JSON that is not an object is rewritten",
          "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_array");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";
    // Parses fine, but every field lookup on it would be a type error.
    create_file(path, "[\"helixscreen\"]");

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);

    auto j = read_json(path);
    REQUIRE(j.is_object());
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: asset_name absent is filled in, other fields preserved",
          "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_noasset");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";
    create_file(path, R"({"project_name":"helixscreen","project_owner":"prestonbrown",)"
                      R"("version":"v0.99.84","extra_key":"keep me"})");

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);

    auto j = read_json(path);
    REQUIRE(j.is_object());
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());
    CHECK(j["version"].get<std::string>() == "v0.99.84");
    CHECK(j["project_name"].get<std::string>() == "helixscreen");
    CHECK(j["project_owner"].get<std::string>() == "prestonbrown");
    // Unknown keys survive -- we repair one field, we do not reset the file.
    CHECK(j["extra_key"].get<std::string>() == "keep me");

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: asset_name for the wrong platform is corrected",
          "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_wrong");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";
    // The reported field case: a name that matches no asset on this release, so
    // Moonraker grabs ad5m.sym.zst instead.
    const std::string wrong = expected_asset_name() == "helixscreen-ad5m.zip"
                                  ? "helixscreen-k1.zip"
                                  : "helixscreen-ad5m.zip";
    create_file(path, R"({"project_name":"helixscreen","project_owner":"prestonbrown",)"
                      R"("version":"v0.99.84","asset_name":")" +
                          wrong + R"("})");

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);

    auto j = read_json(path);
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());
    CHECK(j["version"].get<std::string>() == "v0.99.84");

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: empty and non-string asset_name are both repaired",
          "[update_checker][release_info]") {
    auto tmp = make_temp_dir("helix_relinfo_badtype");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";

    SECTION("empty string") {
        create_file(path, R"({"asset_name":""})");
    }
    SECTION("null") {
        create_file(path, R"({"asset_name":null})");
    }
    SECTION("number") {
        create_file(path, R"({"asset_name":42})");
    }

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Repaired);
    auto j = read_json(path);
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: a correct file is not rewritten",
          "[update_checker][release_info]") {
    // No boot-time disk churn, and it keeps the repair log line diagnostic:
    // if it appears in a field log, something really was wrong.
    auto tmp = make_temp_dir("helix_relinfo_correct");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";

    // Deliberately pretty-printed with a trailing newline: a rewrite emits a
    // compact dump, so byte-identity alone would catch a stray write.
    const std::string original = "{\n    \"asset_name\": \"" + expected_asset_name() +
                                 "\",\n    \"project_name\": \"helixscreen\",\n"
                                 "    \"project_owner\": \"prestonbrown\",\n"
                                 "    \"version\": \"v0.99.103\"\n}\n";
    create_file(path, original);

    // Backdate the mtime an hour so any rewrite is unmistakable regardless of
    // filesystem timestamp granularity.
    const auto backdated = std::filesystem::last_write_time(path) - std::chrono::hours(1);
    std::filesystem::last_write_time(path, backdated);

    REQUIRE(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::NotNeeded);

    // Extra parens on purpose: they suppress Catch2's expression decomposition,
    // which would otherwise try to stream a file_time_type. libc++ gives that
    // clock an __int128 rep, and ostream has no unambiguous operator<< for it —
    // the macOS build fails to compile, not to assert. Comparing as a plain bool
    // keeps the check and costs only the operand values in the failure message.
    CHECK((std::filesystem::last_write_time(path) == backdated));
    CHECK(read_all(path) == original);
    // And no temp file was left lying next to it.
    CHECK_FALSE(std::filesystem::exists(path + ".tmp"));

    remove_dir(tmp);
}

TEST_CASE("repair_release_info: writing through a symlink preserves the link (#1176)",
          "[update_checker][release_info][regression]") {
    // The installer symlinks install-dir files out to printer_data, and that
    // link is the only thing keeping them alive through a Moonraker one-click
    // update (rmtree unlinks a symlink rather than following it). rename(2) onto
    // a symlink replaces THE SYMLINK -- so an atomic write that skips
    // canonicalisation silently converts the link into a regular file and
    // strands it in the doomed directory. Content assertions cannot see this:
    // the JSON round-trips perfectly either way.
    auto install = make_temp_dir("helix_relinfo_link_install");
    auto real = make_temp_dir("helix_relinfo_link_real");
    REQUIRE(!install.empty());
    REQUIRE(!real.empty());

    const std::string real_file = real + "/release_info.json";
    const std::string link_path = install + "/release_info.json";
    create_file(real_file, R"({"project_name":"helixscreen","version":"v0.99.84"})");

    std::error_code ec;
    std::filesystem::create_symlink(real_file, link_path, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::is_symlink(link_path));

    REQUIRE(UpdateChecker::repair_release_info(install) ==
            UpdateChecker::ReleaseInfoRepair::Repaired);

    // The link must survive. Without symlink resolution this is a regular file.
    CHECK(std::filesystem::is_symlink(link_path));
    // ...and the write must have landed on the far side of it, not beside it.
    auto j = read_json(real_file);
    REQUIRE(j.is_object());
    CHECK(j["asset_name"].get<std::string>() == expected_asset_name());
    CHECK(j["version"].get<std::string>() == "v0.99.84");

    CHECK_FALSE(std::filesystem::exists(link_path + ".tmp"));
    CHECK_FALSE(std::filesystem::exists(real_file + ".tmp"));

    remove_dir(install);
    remove_dir(real);
}

TEST_CASE("repair_release_info: an unwritable install dir fails softly",
          "[update_checker][release_info]") {
    // Read-only rootfs / root-owned install dir. A failed repair must never be
    // fatal -- the app boots, self-update just stays broken until the installer
    // is re-run.
    if (geteuid() == 0) {
        SKIP("running as root: directory permissions are not enforced");
    }
    auto tmp = make_temp_dir("helix_relinfo_ro");
    REQUIRE(!tmp.empty());
    const std::string path = tmp + "/release_info.json";
    const std::string original = R"({"asset_name":"helixscreen-wrong.zip"})";
    create_file(path, original);
    REQUIRE(chmod(tmp.c_str(), 0555) == 0);

    CHECK(UpdateChecker::repair_release_info(tmp) == UpdateChecker::ReleaseInfoRepair::Failed);
    // The original is left intact rather than truncated.
    CHECK(read_all(path) == original);

    REQUIRE(chmod(tmp.c_str(), 0755) == 0);
    remove_dir(tmp);
}

// ============================================================================
// Channel version relation (drives the downgrade path)
// ============================================================================
//
// compare_channel_version() replaced a strict `latest > current` test. That
// rule was correct while every install tracked one ever-advancing line, and
// wrong the moment channels became user-selectable: someone who ran the devel
// track and switched back to stable is AHEAD of the channel they now want, so
// "offer only if newer" reports "Already up to date" forever and leaves them
// with no way back short of a manual reinstall.

TEST_CASE("compare_channel_version: channel ahead is an ordinary update",
          "[update_checker][version][channel]") {
    CHECK(compare_channel_version("1.0.0", "1.1.0") == ChannelVersionRelation::Newer);
    CHECK(compare_channel_version("1.0.0", "1.0.1") == ChannelVersionRelation::Newer);
    CHECK(compare_channel_version("1.0.0", "2.0.0") == ChannelVersionRelation::Newer);
    CHECK(compare_channel_version("0.99.111", "1.0.0") == ChannelVersionRelation::Newer);
}

TEST_CASE("compare_channel_version: channel behind is reported, not swallowed",
          "[update_checker][version][channel]") {
    // The devel-to-stable switch: installed 1.1.x, stable serves 1.0.x.
    CHECK(compare_channel_version("1.1.0", "1.0.4") == ChannelVersionRelation::Older);
    CHECK(compare_channel_version("2.0.0", "1.9.9") == ChannelVersionRelation::Older);
    CHECK(compare_channel_version("1.0.1", "1.0.0") == ChannelVersionRelation::Older);
}

TEST_CASE("compare_channel_version: equal versions are Same, not Newer or Older",
          "[update_checker][version][channel]") {
    CHECK(compare_channel_version("1.0.0", "1.0.0") == ChannelVersionRelation::Same);
    CHECK(compare_channel_version("2.5.3", "2.5.3") == ChannelVersionRelation::Same);
}

TEST_CASE("compare_channel_version: unparseable versions do nothing",
          "[update_checker][version][channel]") {
    // Must not be reported as Older -- a garbled manifest would otherwise offer
    // the whole fleet a "switch" to a version that does not exist.
    CHECK(compare_channel_version("1.0.0", "") == ChannelVersionRelation::Unknown);
    CHECK(compare_channel_version("", "1.0.0") == ChannelVersionRelation::Unknown);
    CHECK(compare_channel_version("1.0.0", "not-a-version") == ChannelVersionRelation::Unknown);
}

TEST_CASE("compare_channel_version: prerelease suffixes are still discarded",
          "[update_checker][version][channel]") {
    // Pins the constraint that forced the release pipeline off suffix-derived
    // channels: Version carries only major/minor/patch, so two devel builds of
    // the same x.y.z are indistinguishable here. Releases route by the
    // RELEASE_CHANNEL file and use plain monotonic versions instead.
    CHECK(compare_channel_version("1.1.0-dev1", "1.1.0-dev2") == ChannelVersionRelation::Same);
    CHECK(compare_channel_version("1.1.0", "1.1.0-rc.1") == ChannelVersionRelation::Same);
}

TEST_CASE("ReleaseInfo::is_downgrade defaults to false", "[update_checker][channel]") {
    // Every construction site that does not explicitly mark a downgrade must
    // produce a normal update, or the install path starts asking for
    // confirmation on ordinary upgrades.
    UpdateChecker::ReleaseInfo info;
    CHECK_FALSE(info.is_downgrade);
}
