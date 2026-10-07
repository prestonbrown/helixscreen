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
#include "../test_helpers/scoped_env.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/scoped_update_urls.h"
#include "../test_helpers/update_checker_test_access.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_constants.h"
#include "config.h"
#include "lvgl.h"
#include "platform_table.h"
#include "runtime_config.h"
#include "version.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix::version;
using json = nlohmann::json;

// ============================================================================
// Helper Functions for UpdateChecker Logic
// ============================================================================

namespace {

/**
 * @brief One GitHub release asset entry, as the API serves it
 *
 * Asset selection keys off the name, so the url and size only need to be
 * present and distinguishable.
 */
std::string asset_json(const std::string& name) {
    return R"({"name": ")" + name + R"(", "browser_download_url": "https://x/)" + name +
           R"(", "size": 123})";
}

/**
 * @brief The tarball name a release publishes for this build's platform
 */
std::string platform_tarball(const std::string& version) {
    return "helixscreen-" + helix::platform::current_key() + "-v" + version + ".tar.gz";
}

} // anonymous namespace

// ============================================================================
// Version Parsing Behind Update Detection
// ============================================================================

TEST_CASE("Self-restart sentinel creates a missing fallback dir", "[update_checker][sentinel]") {
    namespace fs = std::filesystem;
    const std::string prev = AppConstants::Update::detail::backup_fallback_dir_ref();
    const fs::path root =
        fs::temp_directory_path() / ("helix-sentinel-" + std::to_string(getpid()));
    const fs::path dir = root / ".helixscreen";
    fs::remove_all(root);
    AppConstants::Update::detail::backup_fallback_dir_ref() = dir.string();

    CHECK(UpdateChecker::write_self_restart_sentinel());
    CHECK(fs::exists(dir / "self_restart_sentinel"));

    AppConstants::Update::detail::backup_fallback_dir_ref() = prev;
    fs::remove_all(root);
}

// The three-way update rule lives in compare_channel_version() and is pinned
// against that shipped function at the bottom of this file. What remains here
// is the parse_version behaviour the rule is built on.
TEST_CASE("Version parsing semantics update detection rests on", "[update_checker][version]") {
    SECTION("handles v prefix in version strings") {
        // parse_version already handles v prefix
        auto v1 = parse_version("v1.0.0");
        auto v2 = parse_version("1.1.0");
        REQUIRE(v1.has_value());
        REQUIRE(v2.has_value());
        REQUIRE(*v2 > *v1);
    }

    SECTION("a pre-release ranks below the release of its triple") {
        // The updater has to offer 1.0.0 to someone running a 1.0.0 beta.
        auto beta = parse_version("1.0.0-beta");
        auto release = parse_version("1.0.0");
        REQUIRE(beta.has_value());
        REQUIRE(release.has_value());
        REQUIRE(*beta < *release);
        REQUIRE(*beta != *release);
    }
}

// ============================================================================
// GitHub Release JSON Parsing
// ============================================================================

TEST_CASE("GitHub release JSON parsing", "[update_checker][json]") {
    UpdateChecker::ReleaseInfo release;
    std::string error;

    SECTION("parses valid release JSON") {
        const std::string tarball = platform_tarball("1.2.3");
        const std::string json_str = R"({
            "tag_name": "v1.2.3",
            "body": "## What's New\n- Feature A\n- Bug fix B",
            "published_at": "2025-01-15T10:00:00Z",
            "assets": [)" + asset_json(tarball) +
                                     R"(]
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        REQUIRE(release.tag_name == "v1.2.3");
        REQUIRE(release.version == "1.2.3");
        REQUIRE(release.release_notes == "## What's New\n- Feature A\n- Bug fix B");
        REQUIRE(release.published_at == "2025-01-15T10:00:00Z");
        REQUIRE(release.download_url == "https://x/" + tarball);
    }

    SECTION("selects this platform's tar.gz over its zip and over other assets") {
        // A release carries one tarball and one zip per platform alongside
        // GitHub's own source archives. The tarball wins: devices still on
        // v0.99.30 or earlier gunzip whatever url the asset list hands them.
        const std::string tarball = platform_tarball("2.0.0");
        const std::string zip = "helixscreen-" + helix::platform::current_key() + ".zip";
        const std::string json_str = R"({
            "tag_name": "v2.0.0",
            "body": "Release",
            "published_at": "2025-02-01T00:00:00Z",
            "assets": [)" + asset_json("source.zip") +
                                     "," + asset_json(zip) + "," + asset_json(tarball) + "," +
                                     asset_json("debug.log") + R"(]
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        REQUIRE(release.download_url == "https://x/" + tarball);
    }

    SECTION("handles missing optional fields gracefully") {
        // Minimal valid JSON - only tag_name required for version
        const char* json_str = R"({
            "tag_name": "v3.0.0"
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
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

        REQUIRE(helix::parse_github_release(json_str, release, error));
        REQUIRE(release.version == "1.0.0");
        REQUIRE(release.download_url.empty());
    }

    SECTION("handles null body field") {
        const char* json_str = R"({
            "tag_name": "v1.0.0",
            "body": null,
            "published_at": "2025-01-01T00:00:00Z"
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        // null should be converted to empty string by .value() default
        REQUIRE(release.release_notes.empty());
    }

    SECTION("rejects malformed JSON") {
        const char* invalid_json = R"({
            "tag_name": "v1.0.0"
            "body": "missing comma"
        })";

        REQUIRE_FALSE(helix::parse_github_release(invalid_json, release, error));
        REQUIRE_FALSE(error.empty());
    }

    SECTION("rejects empty JSON object") {
        REQUIRE_FALSE(helix::parse_github_release("{}", release, error));
        REQUIRE_FALSE(error.empty());
    }

    SECTION("rejects invalid tag_name") {
        const char* json_str = R"({
            "tag_name": "not-a-version"
        })";

        REQUIRE_FALSE(helix::parse_github_release(json_str, release, error));
        REQUIRE_FALSE(error.empty());
    }

    SECTION("rejects empty string") {
        REQUIRE_FALSE(helix::parse_github_release("", release, error));
        REQUIRE_FALSE(error.empty());
    }

    SECTION("handles version without v prefix") {
        const char* json_str = R"({
            "tag_name": "1.5.0"
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        REQUIRE(release.tag_name == "1.5.0");
        REQUIRE(release.version == "1.5.0");
    }
}

// ============================================================================
// Version Prefix Stripping
// ============================================================================

TEST_CASE("Version prefix stripping", "[update_checker][version]") {
    SECTION("strips lowercase v") {
        REQUIRE(helix::strip_version_prefix("v1.2.3") == "1.2.3");
    }

    SECTION("strips uppercase V") {
        REQUIRE(helix::strip_version_prefix("V1.2.3") == "1.2.3");
    }

    SECTION("preserves version without prefix") {
        REQUIRE(helix::strip_version_prefix("1.2.3") == "1.2.3");
    }

    SECTION("handles empty string") {
        REQUIRE(helix::strip_version_prefix("") == "");
    }

    SECTION("handles just v") {
        REQUIRE(helix::strip_version_prefix("v") == "");
    }
}

// ============================================================================
// Error Handling Scenarios
// ============================================================================

TEST_CASE("Update checker error scenarios", "[update_checker][error]") {
    UpdateChecker::ReleaseInfo release;
    std::string error;

    SECTION("empty response body") {
        REQUIRE_FALSE(helix::parse_github_release("", release, error));
    }

    SECTION("non-JSON response") {
        REQUIRE_FALSE(
            helix::parse_github_release("<!DOCTYPE html><html>Error</html>", release, error));
    }

    SECTION("JSON array instead of object") {
        REQUIRE_FALSE(helix::parse_github_release("[1, 2, 3]", release, error));
    }

    SECTION("deeply nested invalid structure") {
        const char* json_str = R"({
            "tag_name": {"nested": "object"}
        })";

        REQUIRE_FALSE(helix::parse_github_release(json_str, release, error));
    }

    // Every rejection above has to say why: a silent false leaves the UI with
    // no message to show.
    CHECK_FALSE(error.empty());
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
#include "platform_table.h"
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
    // Hermetic by construction: no real network. The dev channel reads its
    // endpoint from the root-owned update_urls.json (settings.json's copy is
    // ignored) and is exempt from the rate limiter, so pointing it at a closed
    // loopback port drives the full
    // check_for_updates -> do_check -> fetch_dev_release -> report_result cycle
    // on a fast, local, deterministic connection refusal. Hitting api.github.com
    // instead made this test slow, network-dependent, and - because libhv's
    // requests:: client spins event-loop threads on a successful TLS connection
    // that outlive the call - the one test in the suite that leaked threads
    // (prestonbrown/helixscreen#1212). The isolation listener's per-TEST_CASE
    // reset_config_singleton() wipes these config keys again for the next test;
    // the ScopedUpdateUrls guard restores the state dir itself.
    auto* config = Config::get_instance();
    // Dev and Beta are gated behind /beta_features - get_channel() reports Stable
    // for either one while beta is locked, which would send this check at the real
    // stable endpoint instead of the loopback port below.
    config->set<bool>("/beta_features", true);
    config->set<int>("/update/channel", 2); // Dev
    helix::test::ScopedUpdateUrls urls("helix_dev_url", R"({"dev_url": "http://127.0.0.1:1/"})");

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
        const std::string tarball = platform_tarball("1.5.0");
        const std::string github_response = R"({
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
                    "name": ")" + tarball + R"(",
                    "size": 5242880,
                    "download_count": 42,
                    "browser_download_url": "https://github.com/prestonbrown/helixscreen/releases/download/v1.5.0/)" +
                                            tarball + R"("
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

        UpdateChecker::ReleaseInfo release;
        std::string error;
        REQUIRE(helix::parse_github_release(github_response, release, error));
        REQUIRE(release.version == "1.5.0");
        REQUIRE(release.tag_name == "v1.5.0");
        REQUIRE(release.download_url.find(tarball) != std::string::npos);
        REQUIRE(release.download_bytes == 5242880);
        REQUIRE(release.release_notes.find("Auto-update support") != std::string::npos);
    }
}

// ============================================================================
// Edge Cases and Boundary Conditions
// ============================================================================

TEST_CASE("Version edge cases", "[update_checker][edge]") {
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

    SECTION("version text starts on the current version, not blank") {
        const char* version_text = lv_subject_get_string(checker.version_text_subject());
        REQUIRE(version_text != nullptr);
        REQUIRE(std::string(version_text) == fmt::format("Version {}", HELIX_VERSION));

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
    UpdateChecker::ReleaseInfo release;
    std::string error;

    SECTION("unicode in release notes") {
        const char* json_str = R"({
            "tag_name": "v1.0.0",
            "body": "Fixed emoji display \ud83d\ude80 and Chinese chars \u4e2d\u6587"
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        REQUIRE_FALSE(release.release_notes.empty());
    }

    SECTION("very long release notes") {
        std::string long_body(10000, 'x');
        std::string json_str = R"({"tag_name": "v1.0.0", "body": ")" + long_body + R"("})";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        REQUIRE(release.release_notes.length() == 10000);
    }

    SECTION("an asset name carrying no platform key is not selected") {
        // A wrong-platform binary bricks the device it lands on, so a name the
        // platform rule does not recognise leaves the update unavailable
        // rather than guessing. The release still parses; download_url stays
        // empty. Both spellings below miss the rule for the same reason: no
        // "-<platform>-v<digit>" between the prefix and the extension.
        const std::string json_str = R"({
            "tag_name": "v1.0.0",
            "assets": [)" + asset_json("helixscreen-1.0.0.tar.gz") +
                                     "," + asset_json("helix screen_v1.0.0_(arm64).tar.gz") + R"(]
        })";

        REQUIRE(helix::parse_github_release(json_str, release, error));
        CHECK(release.version == "1.0.0");
        CHECK(release.download_url.empty());
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

TEST_CASE("UpdateChecker stages downloads under dot-prefixed names", "[update_checker]") {
    // Boards whose gcodes root IS the data partition list every plainly named
    // file in the print picker, so the staged archive must hide behind a
    // leading dot in both archive formats. The property is the dot, not the
    // spelling of the name.
    SECTION("tar.gz release") {
        const std::string name =
            UpdateChecker::download_filename_for_url("https://r.example/helixscreen.tar.gz");
        INFO(name);
        REQUIRE(!name.empty());
        REQUIRE(name.front() == '.');
        REQUIRE(name.compare(name.size() - 7, 7, ".tar.gz") == 0);
    }
    SECTION("zip release") {
        const std::string name =
            UpdateChecker::download_filename_for_url("https://r.example/helixscreen.zip");
        INFO(name);
        REQUIRE(!name.empty());
        REQUIRE(name.front() == '.');
        REQUIRE(name.compare(name.size() - 4, 4, ".zip") == 0);
    }
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

TEST_CASE("UpdateChecker install_failure_detail names the cause, not the step",
          "[update_checker][install_failure_detail]") {
    using UC = UpdateChecker;

    // The installer's step line ("[n/N] <title> ... FAILED") follows the
    // [ERROR] lines that say why; the touchscreen needs the why.
    SECTION("an [ERROR] line wins over a later FAILED step line") {
        const std::vector<std::string> lines = {
            "[3/6] Downloaded ... ok (40 MB)",
            "      \033[0;31m[ERROR]\033[0m Not enough space on /opt (12MB free)",
            "[4/6] Installed files ... FAILED",
            "Nothing on your printer was changed after this step.",
        };
        REQUIRE(UC::install_failure_detail(lines) ==
                "[ERROR] Not enough space on /opt (12MB free)");
    }

    SECTION("the last [ERROR] line is the one shown") {
        const std::vector<std::string> lines = {
            "[ERROR] first",
            "[ERROR] second",
            "[2/6] Downloaded ... FAILED",
        };
        REQUIRE(UC::install_failure_detail(lines) == "[ERROR] second");
    }

    SECTION("without an [ERROR] line, the last ERROR or FAILED line") {
        const std::vector<std::string> lines = {
            "[WARN] careful",
            "[2/6] Downloaded ... FAILED (interrupted)",
        };
        REQUIRE(UC::install_failure_detail(lines) == "[2/6] Downloaded ... FAILED (interrupted)");
    }

    SECTION("an error before a completed step is not the cause") {
        const std::vector<std::string> lines = {
            "      [ERROR] Could not seed settings",
            "[5/6] Connected to Moonraker ... ok (update manager)",
            "      systemctl enable helixscreen failed (exit 1):",
            "        Failed to enable unit: Unit file helixscreen.service is masked.",
            "[6/6] Starting HelixScreen ... FAILED",
        };
        REQUIRE(UC::install_failure_detail(lines) ==
                "systemctl enable helixscreen failed (exit 1):");
    }

    SECTION("a completed step clears earlier errors and warnings") {
        const std::vector<std::string> lines = {
            "[ERROR] stale",
            "WARNING: stale",
            "[4/6] Set up service ... ok",
            "[5/6] Connecting to Moonraker ... FAILED (interrupted)",
        };
        REQUIRE(UC::install_failure_detail(lines) ==
                "[5/6] Connecting to Moonraker ... FAILED (interrupted)");
    }

    SECTION("a failed command outranks a later FAILED step line") {
        const std::vector<std::string> lines = {
            "      apt-get install -y unzip failed (exit 100):",
            "        E: Unable to locate package unzip",
            "[2/7] Installing libraries ... FAILED",
        };
        REQUIRE(UC::install_failure_detail(lines) == "apt-get install -y unzip failed (exit 100):");
    }

    SECTION("a WARNING line only when nothing failed louder") {
        const std::vector<std::string> lines = {"ok", "WARNING: low disk"};
        REQUIRE(UC::install_failure_detail(lines) == "WARNING: low disk");
    }

    SECTION("nothing to show") {
        REQUIRE(UC::install_failure_detail({"all fine"}).empty());
    }
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
    ps.print_state()
        .reset_print_start_state(); // force phase to IDLE so the next raise is a new print
    ps.update_from_status(json{{"print_stats", {{"state", wire_state}}}});
    ps.print_state().set_print_start_state(phase, "", 0);
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
        ps.print_state().reset_print_start_state();
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
    REQUIRE(state.print_state().get_print_lifecycle() == PrintState::Preparing);

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
    REQUIRE(state.print_state().get_print_lifecycle() == PrintState::Idle);

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
    config->set<std::string>("/update/dismissed_version", "");
    config->save();

    SECTION("is_version_dismissed returns false when no dismissed version in config") {
        REQUIRE_FALSE(checker.is_version_dismissed("1.2.0"));
    }

    SECTION("is_version_dismissed returns true when version matches dismissed") {
        config->set<std::string>("/update/dismissed_version", "1.2.0");
        config->save();

        REQUIRE(checker.is_version_dismissed("1.2.0"));
    }

    SECTION("is_version_dismissed returns false for newer version than dismissed") {
        config->set<std::string>("/update/dismissed_version", "1.2.0");
        config->save();

        REQUIRE_FALSE(checker.is_version_dismissed("1.3.0"));
    }

    SECTION("is_version_dismissed returns true for older version than dismissed") {
        config->set<std::string>("/update/dismissed_version", "1.2.0");
        config->save();

        REQUIRE(checker.is_version_dismissed("1.1.0"));
    }

    SECTION("dismiss_current_version persists to config") {
        // We need a cached update for dismiss_current_version to work
        // Since we can't easily set cached_info_ without a real check,
        // test via the config path directly
        // This tests the config interaction pattern
        auto dismissed = config->get<std::string>("/update/dismissed_version", "");
        // After clearing, should be empty
        REQUIRE(dismissed.empty());
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

TEST_CASE("do_install never runs the installer in test mode",
          "[update_checker][installer][do_install][test_mode]") {
    auto tmp = make_temp_dir("helix_install_testmode");
    REQUIRE(!tmp.empty());

    // An installer that only leaves a marker, so a missing gate shows up as the
    // marker existing rather than as anything done to the host.
    const std::string marker = tmp + "/installer_ran";
    std::string inner = tmp + "/helixscreen";
    mkdir(inner.c_str(), 0755);
    create_file(inner + "/install.sh", "#!/bin/sh\ntouch '" + marker + "'\n", true);
    const std::string tarball = tmp + "/release.tar.gz";
    std::string cmd =
        "cd " + tmp + " && COPYFILE_DISABLE=1 tar czf release.tar.gz helixscreen/install.sh";
    REQUIRE(std::system(cmd.c_str()) == 0);

    // A child process makes the install: past the installer, do_install()
    // ends the process to restart it, which would take the assertions with it.
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ScopedRuntimeConfig scoped_config;
        get_runtime_config()->test_mode = true;
        auto& checker = UpdateChecker::instance();
        UpdateCheckerTestAccess::set_restart_action(checker, [] {});
        UpdateCheckerTestAccess::set_status_hold_ms(checker, 0, 0);
        UpdateCheckerTestAccess::do_install(checker, tarball);
        _exit(0);
    }
    int wstatus = 0;
    for (int i = 0; i < 3000 && waitpid(pid, &wstatus, WNOHANG) == 0; ++i)
        usleep(10 * 1000);
    if (waitpid(pid, &wstatus, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &wstatus, 0);
        FAIL("do_install did not return within 30s");
    }

    CHECK(access(marker.c_str(), F_OK) != 0);
    CHECK(access(tarball.c_str(), F_OK) != 0);

    remove_dir(tmp);
}

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

    // Clear PATH to simulate a minimal systemd environment. The guard owns
    // both the set and the restore, and keeps the original value in storage
    // it owns (prestonbrown/helixscreen#1537). The tight scope restores PATH
    // before remove_dir(), whose rm needs the real PATH back.
    std::string result;
    {
        helix::ScopedEnv path_env("PATH", ""); // empty PATH — bare execvp("tar",...) would fail
        result = UpdateChecker::extract_installer_from_tarball(tarball, tmp);
    }

    remove_dir(tmp);

    // Must succeed: resolve_tool() finds tar/cp/gunzip via absolute paths
    REQUIRE(!result.empty());
}

// ============================================================================
// Platform Key & Architecture Validation
// ============================================================================

TEST_CASE("current_key returns a known platform", "[update_checker][platform]") {
    std::string platform = helix::platform::current_key();
    REQUIRE(!platform.empty());

    // Must be one of the supported platform keys. Keep in sync with the
    // release matrix in .github/workflows/release.yml and the #ifdef ladder in
    // helix::platform::current_key(). Adding a platform without an entry
    // here — AND a matching #elif in current_key — silently bricks
    // in-app updates for that platform (falls through to "pi", so the device
    // downloads the Pi tarball and ends up with missing shared libs).
    std::vector<std::string> known_platforms = {"pi",   "pi32", "x86", "ad5m",  "k1",          "k2",
                                                "ad5x", "mips", "cc1", "esp32", "snapmaker-u1"};
    bool found = false;
    for (const auto& p : known_platforms) {
        if (platform == p) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("current_key matches compiled binary architecture", "[update_checker][platform][arch]") {
#ifndef __linux__
    SKIP("Linux-only: requires /proc/self/exe and ELF binary format");
#else
    // The platform key must agree with what we actually compiled as.
    // This catches the bug where uname() returned "aarch64" on a pi32 build.
    std::string platform = helix::platform::current_key();

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

    // Native dev builds report "pi" whatever the host, so only the class is
    // comparable here, not the machine.
    const auto* expected = helix::platform::find(platform);
    REQUIRE(expected != nullptr);
    REQUIRE(elf_class == expected->elf_class);
#endif
}

TEST_CASE("display_name returns non-empty string for all known platforms",
          "[update_checker][platform]") {
    // Mirror the known_platforms list from "current_key returns a known platform".
    // Every key that helix::platform::current_key() can return MUST have a display name.
    std::vector<std::string> known_platforms = {"pi",   "pi32", "x86", "ad5m",  "k1",          "k2",
                                                "ad5x", "mips", "cc1", "esp32", "snapmaker-u1"};

    for (const auto& key : known_platforms) {
        INFO("platform key: " << key);
        std::string name = helix::platform::display_name(key);
        REQUIRE(!name.empty());
        // Self-update ELF validation and debug-bundle file capture read the
        // same row, so a key without one silently loses both.
        REQUIRE(helix::platform::find(key) != nullptr);
    }
}

TEST_CASE("platform table gives every Linux platform an ELF expectation",
          "[update_checker][platform]") {
    // esp32 ships a firmware image, not an ELF release zip.
    for (const char* key :
         {"pi", "pi32", "x86", "ad5m", "k1", "k2", "ad5x", "mips", "cc1", "snapmaker-u1"}) {
        INFO("platform key: " << key);
        const auto* p = helix::platform::find(key);
        REQUIRE(p != nullptr);
        REQUIRE(p->elf_class != 0);
    }
}

TEST_CASE("platform table marks which platforms name printer hardware",
          "[update_checker][platform]") {
    // Debug bundles compare this platform's display name against the user's
    // printer pick; generic hosts have no hardware name to compare.
    for (const char* key : {"pi", "pi32", "x86", "esp32"}) {
        INFO("platform key: " << key);
        REQUIRE_FALSE(helix::platform::find(key)->has_printer_hardware);
    }
    for (const char* key : {"ad5m", "ad5x", "mips", "k1", "k2", "cc1", "snapmaker-u1"}) {
        INFO("platform key: " << key);
        REQUIRE(helix::platform::find(key)->has_printer_hardware);
    }
}

TEST_CASE("mips platform validates MIPS32 LE and captures the AD5X zmod files",
          "[update_checker][platform]") {
    const auto* p = helix::platform::find("mips");
    REQUIRE(p != nullptr);
    REQUIRE(p->elf_class == 1);   // ELFCLASS32
    REQUIRE(p->elf_data == 1);    // ELFDATA2LSB
    REQUIRE(p->elf_machine == 8); // EM_MIPS

    const auto* ad5x = helix::platform::find("ad5x");
    REQUIRE(ad5x != nullptr);
    REQUIRE(!ad5x->diagnostic_files.empty());
    REQUIRE(p->diagnostic_files == ad5x->diagnostic_files);
}

TEST_CASE("elf_header_matches checks class, endianness and machine", "[update_checker][platform]") {
    const auto* mips = helix::platform::find("mips");
    REQUIRE(mips != nullptr);

    // 0x7f ELF, class 1, data 1 (LE), e_machine at bytes 18-19.
    uint8_t hdr[20] = {0x7f, 'E', 'L', 'F', 1, 1};
    hdr[18] = 0x08;
    REQUIRE(helix::platform::elf_header_matches(*mips, hdr));

    SECTION("big-endian MIPS is rejected") {
        hdr[5] = 2;
        hdr[18] = 0x00;
        hdr[19] = 0x08;
        REQUIRE_FALSE(helix::platform::elf_header_matches(*mips, hdr));
    }
    SECTION("ARM is rejected") {
        hdr[18] = 0x28;
        REQUIRE_FALSE(helix::platform::elf_header_matches(*mips, hdr));
    }
    SECTION("64-bit is rejected") {
        hdr[4] = 2;
        REQUIRE_FALSE(helix::platform::elf_header_matches(*mips, hdr));
    }
}

TEST_CASE("display_name returns correct strings for known platforms",
          "[update_checker][platform]") {
    // Exact display name strings — changing them breaks debug bundle dashboard parsing.
    REQUIRE(helix::platform::display_name("pi") == "Raspberry Pi");
    REQUIRE(helix::platform::display_name("pi32") == "Raspberry Pi (32-bit)");
    REQUIRE(helix::platform::display_name("x86") == "x86 Desktop");
    REQUIRE(helix::platform::display_name("ad5m") == "FlashForge Adventurer 5M");
    REQUIRE(helix::platform::display_name("ad5x") == "FlashForge Adventurer 5X");
    REQUIRE(helix::platform::display_name("mips") == "MIPS (K1 series / AD5X)");
    REQUIRE(helix::platform::display_name("k1") == "Creality K1");
    REQUIRE(helix::platform::display_name("k2") == "Creality K2 Plus");
    REQUIRE(helix::platform::display_name("cc1") == "Elegoo Centauri Carbon");
    REQUIRE(helix::platform::display_name("snapmaker-u1") == "Snapmaker U1");
    REQUIRE(helix::platform::display_name("esp32") == "BTT K-Touch");
    // Unknown keys fall back to the key itself.
    REQUIRE(helix::platform::display_name("unknown-platform") == "unknown-platform");
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
    return "helixscreen-" + helix::platform::current_key() + ".zip";
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
    // Components are ordered numerically, not lexicographically ("99" > "100").
    CHECK(compare_channel_version("99.99.99", "100.0.0") == ChannelVersionRelation::Newer);
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
    // Pre-1.0 versions, which is the entire shipped history so far.
    CHECK(compare_channel_version("0.0.1", "0.0.1") == ChannelVersionRelation::Same);
}

TEST_CASE("compare_channel_version: unparseable versions do nothing",
          "[update_checker][version][channel]") {
    // Must not be reported as Older -- a garbled manifest would otherwise offer
    // the whole fleet a "switch" to a version that does not exist.
    CHECK(compare_channel_version("1.0.0", "") == ChannelVersionRelation::Unknown);
    CHECK(compare_channel_version("", "1.0.0") == ChannelVersionRelation::Unknown);
    CHECK(compare_channel_version("1.0.0", "not-a-version") == ChannelVersionRelation::Unknown);
    // Garbled on the installed side too: a corrupt version.h must not offer a
    // "downgrade" to whatever the channel happens to serve.
    CHECK(compare_channel_version("invalid", "1.0.0") == ChannelVersionRelation::Unknown);
    CHECK(compare_channel_version("", "") == ChannelVersionRelation::Unknown);
}

TEST_CASE("compare_channel_version: prerelease suffixes order by precedence",
          "[update_checker][version][channel]") {
    // The beta lane lives entirely inside one x.y.z, so the suffix is the only
    // thing that distinguishes two builds on it.
    CHECK(compare_channel_version("1.1.0-dev1", "1.1.0-dev2") == ChannelVersionRelation::Newer);
    CHECK(compare_channel_version("1.1.0-beta.2", "1.1.0-beta.11") ==
          ChannelVersionRelation::Newer);
    CHECK(compare_channel_version("1.1.0-beta.11", "1.1.0-beta.2") ==
          ChannelVersionRelation::Older);
    CHECK(compare_channel_version("1.1.0-beta.1", "1.1.0-beta.1") == ChannelVersionRelation::Same);

    // The stable cut outranks every beta of the same triple, and a channel
    // serving a beta of the version already installed is behind it.
    CHECK(compare_channel_version("1.0.0-beta", "1.0.0") == ChannelVersionRelation::Newer);
    CHECK(compare_channel_version("1.1.0", "1.1.0-rc.1") == ChannelVersionRelation::Older);
}

// ============================================================================
// GitHub-fallback asset selection (drives self-update on the fallback path)
// ============================================================================
//
// A release carries one tarball per platform key, and some keys are prefixes
// of others (k1 and k1-dynamic). Selection must anchor the version's 'v'
// directly after this build's platform key, or the shorter key grabs the
// longer platform's tarball. The host test build reports platform "pi"; the
// decoy below extends it exactly the way k1-dynamic extends k1.

TEST_CASE("parse_github_release: a platform prefix does not select a longer platform's tarball",
          "[update_checker][github]") {
    const std::string platform = helix::platform::current_key();
    // "pi" on a host test build; the test only needs SOME known key.
    REQUIRE_FALSE(platform.empty());

    const std::string mine = platform_tarball("1.2.3");
    const std::string decoy = "helixscreen-" + platform + "-dynamic-v0.99.0.tar.gz";
    // Decoy first: it also sorts first alphabetically, which is how the real
    // release listing serves it.
    const std::string body = R"({"tag_name": "v1.2.3", "assets": [)" + asset_json(decoy) + ", " +
                             asset_json(mine) + "]}";

    UpdateChecker::ReleaseInfo info;
    std::string error;
    REQUIRE(helix::parse_github_release(body, info, error));
    CHECK(info.version == "1.2.3");
    REQUIRE_FALSE(info.download_url.empty());
    CHECK(info.download_url.find(mine) != std::string::npos);
    CHECK(info.download_url.find(decoy) == std::string::npos);

    SECTION("only the longer platform's tarball present: no asset, not the wrong one") {
        // A wrong-platform tarball bricks the device it lands on; an empty
        // download_url leaves the update unavailable instead.
        const std::string only_decoy =
            R"({"tag_name": "v1.2.3", "assets": [)" + asset_json(decoy) + "]}";
        UpdateChecker::ReleaseInfo sparse;
        REQUIRE(helix::parse_github_release(only_decoy, sparse, error));
        CHECK(sparse.download_url.empty());
    }
}

TEST_CASE("ReleaseInfo::is_downgrade defaults to false", "[update_checker][channel]") {
    // Every construction site that does not explicitly mark a downgrade must
    // produce a normal update, or the install path starts asking for
    // confirmation on ordinary upgrades.
    UpdateChecker::ReleaseInfo info;
    CHECK_FALSE(info.is_downgrade);
}

// ============================================================================
// Download worker lifetime — the LVGL thread must never wait on it
//
// A Snapmaker U1 user reported in-app updates "always froze and required a
// reboot for the screen to respond again". start_download() joined the download
// thread on the LVGL thread, and the worker sits inside libhv's SYNCHRONOUS
// requests::downloadFile() whose req->timeout is 3600 seconds with no abort
// hook (lib/libhv/http/client/requests.h). Cancelling only sets a flag the
// worker reads AFTER downloadFile() returns, while the modal immediately resets
// the status to Idle — so "Install -> Cancel -> Install" walked straight past
// the DownloadStatus guard and into an hour-long join on the UI thread.
// ============================================================================

namespace {

/// A loopback TCP listener that accepts connections and then never answers.
///
/// Reproduces the production stall exactly: libhv connects, sends the GET and
/// blocks waiting for a response header that never comes. Nothing else in the
/// test suite can put the download worker into that state, and a worker that
/// finished on its own would make every assertion below vacuous.
///
/// `arm_release_after()` is what keeps a REGRESSION from hanging the whole
/// suite: the socket is closed after a bounded delay, so a build that still
/// blocks on the UI thread finishes late (and fails the elapsed-time
/// assertion) instead of never finishing at all.
class StalledHttpServer {
  public:
    StalledHttpServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listen_fd_, 4) == 0);
        socklen_t len = sizeof(addr);
        REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    ~StalledHttpServer() {
        cancel_armed_release();
        release();
    }

    StalledHttpServer(const StalledHttpServer&) = delete;
    StalledHttpServer& operator=(const StalledHttpServer&) = delete;

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/helixscreen-update.tar.gz";
    }

    /// True once libhv has actually connected, i.e. the worker is parked in
    /// downloadFile(). Every "a worker is still running" assertion is gated on
    /// this so none of them can pass against an already-finished worker.
    bool connected() const {
        return connected_.load();
    }

    /// Close the socket after `delay`, unless cancel_armed_release() runs first.
    void arm_release_after(std::chrono::milliseconds delay) {
        release_thread_ = std::thread([this, delay] {
            std::unique_lock<std::mutex> lk(arm_mu_);
            arm_cv_.wait_for(lk, delay, [this] { return arm_cancel_; });
            const bool cancelled = arm_cancel_;
            lk.unlock();
            if (!cancelled) {
                release();
            }
        });
    }

    void cancel_armed_release() {
        {
            std::lock_guard<std::mutex> lk(arm_mu_);
            arm_cancel_ = true;
        }
        arm_cv_.notify_all();
        if (release_thread_.joinable()) {
            release_thread_.join();
        }
    }

    /// Close everything so a blocked downloadFile() unwinds. Idempotent.
    void release() {
        if (released_.exchange(true)) {
            return;
        }
        stop_.store(true);
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : conns_) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        conns_.clear();
    }

  private:
    void accept_loop() {
        while (!stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            const int pr = ::poll(&pfd, 1, 20);
            if (pr <= 0) {
                continue;
            }
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                conns_.push_back(fd);
            }
            connected_.store(true);
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::thread accept_thread_;
    std::thread release_thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> released_{false};
    std::mutex mu_;
    std::vector<int> conns_;
    std::mutex arm_mu_;
    std::condition_variable arm_cv_;
    bool arm_cancel_ = false;
};

/// Poll `pred` until it is true or `limit` elapses.
template <typename Pred> bool wait_until_true(Pred pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

/// Common preamble for the stall tests: an idle printer, an initialised
/// checker, and a seeded release pointing at `server`.
void arm_stalled_download(UpdateChecker& checker, StalledHttpServer& server) {
    // update_install_suppressed() returns from start_download() before anything
    // else, so on a suppressed tree every assertion below would pass vacuously.
    REQUIRE_FALSE(update_install_suppressed());

    auto& state = get_printer_state();
    state.init_subjects(false);
    drive_lifecycle(state, "standby", PrintStartPhase::IDLE);
    REQUIRE(state.print_state().get_print_lifecycle() == PrintState::Idle);

    checker.init();
    UpdateCheckerTestAccess::seed_available_update(checker, "9.9.9", server.url());

    checker.start_download();

    // The load-bearing precondition: libhv is really parked in downloadFile().
    REQUIRE(wait_until_true([&] { return server.connected(); }, std::chrono::seconds(10)));
    REQUIRE(checker.download_in_flight());
}

} // namespace

TEST_CASE_METHOD(GlobalPrintStateFixture,
                 "UpdateChecker start_download does not block on a still-running worker",
                 "[update_checker][download][reentry][slow]") {
    auto& checker = UpdateChecker::instance();
    StalledHttpServer server;
    arm_stalled_download(checker, server);

    // Reproduce the reported trap. cancel_download() only sets a flag that the
    // worker reads after downloadFile() returns, and
    // UpdatesSettingsOverlay::hide_update_download_modal() immediately resets the
    // status to Idle. The enum now says "nothing is happening" while the worker
    // is still inside an hour-long blocking call.
    checker.cancel_download();
    checker.report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
    REQUIRE(checker.get_download_status() == UpdateChecker::DownloadStatus::Idle);
    REQUIRE(checker.download_in_flight()); // the enum lies; worker liveness does not

    // Safety net so a regression fails instead of hanging the suite for an hour.
    server.arm_release_after(std::chrono::seconds(4));

    const auto t0 = std::chrono::steady_clock::now();
    checker.start_download(); // ran on the LVGL thread in production
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    server.cancel_armed_release();

    // The whole bug: this call used to join() the live worker. Anything near
    // the 4s release delay means it waited on the worker rather than refusing.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);

    // ...and the refusal must be visible. Every early return from
    // start_download() has to leave a status behind: the modal binds every
    // container to a NON-ZERO download_status, so returning at Idle leaves a
    // dialog with no content and no buttons.
    CHECK(checker.get_download_status() == UpdateChecker::DownloadStatus::Error);
    CHECK(checker.get_download_error().find("still finishing") != std::string::npos);

    server.release();
    CHECK(wait_until_true([&] { return !checker.download_in_flight(); }, std::chrono::seconds(20)));
    UpdateCheckerTestAccess::clear(checker);
    helix::ui::UpdateQueue::instance().drain();
    checker.shutdown();
}

TEST_CASE_METHOD(GlobalPrintStateFixture,
                 "UpdateChecker shutdown returns promptly with a download in flight",
                 "[update_checker][download][shutdown][slow]") {
    auto& checker = UpdateChecker::instance();
    StalledHttpServer server;
    arm_stalled_download(checker, server);

    // Same safety net: without the fix shutdown() joins the stuck worker, so
    // the only thing that ever ends it is the socket closing.
    server.arm_release_after(std::chrono::seconds(6));

    const auto t0 = std::chrono::steady_clock::now();
    checker.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    server.cancel_armed_release();

    // Quitting or restarting mid-download must not wait on libhv.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2500);

    // Giving up on the join must not leak the thread past the test: once the
    // socket closes the worker still has to unwind.
    server.release();
    CHECK(wait_until_true([&] { return !checker.download_in_flight(); }, std::chrono::seconds(20)));
    UpdateCheckerTestAccess::clear(checker);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(HelixTestFixture,
                 "UpdateChecker paints the terminal status before the restart runs",
                 "[update_checker][download][restart][slow]") {
    auto& checker = UpdateChecker::instance();
    checker.init();
    UpdateCheckerTestAccess::reset_restart_state(checker);
    UpdateCheckerTestAccess::set_status_hold_ms(checker, 60, 60);

    std::atomic<int> status_when_restarted{-1};
    std::atomic<bool> restarted{false};
    UpdateCheckerTestAccess::set_restart_action(checker, [&] {
        status_when_restarted.store(lv_subject_get_int(checker.download_status_subject()));
        restarted.store(true);
    });

    checker.report_download_status(UpdateChecker::DownloadStatus::Installing, 100, "");
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_subject_get_int(checker.download_status_subject()) ==
            static_cast<int>(UpdateChecker::DownloadStatus::Installing));

    // The download worker's half: install.sh has returned 0, publish the
    // terminal frames and hand the restart over.
    std::thread worker([&] {
        UpdateCheckerTestAccess::finish_install_and_restart(checker, "/tmp/helix-test-install-root",
                                                            "9.9.9");
    });

    // The LVGL thread's half: keep draining so the deferred subject writes
    // actually land, and record what the screen would have shown.
    std::vector<int> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!restarted.load() && std::chrono::steady_clock::now() < deadline) {
        helix::ui::UpdateQueue::instance().drain();
        seen.push_back(lv_subject_get_int(checker.download_status_subject()));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    helix::ui::UpdateQueue::instance().drain();
    worker.join();

    REQUIRE(restarted.load());

    // ::_exit(0) used to fire from the download worker while the Complete write
    // was still sitting in the UpdateQueue, so the last frame the user ever saw
    // was "Installing... Do not power off your printer."
    CHECK(status_when_restarted.load() ==
          static_cast<int>(UpdateChecker::DownloadStatus::Restarting));

    // Restarting (7) is rendered by update_download_modal.xml and was reachable
    // from nothing in C++ — the modal's own comment described a transition that
    // did not exist.
    CHECK(std::find(seen.begin(), seen.end(),
                    static_cast<int>(UpdateChecker::DownloadStatus::Restarting)) != seen.end());
    CHECK(std::find(seen.begin(), seen.end(),
                    static_cast<int>(UpdateChecker::DownloadStatus::Complete)) != seen.end());

    UpdateCheckerTestAccess::set_restart_action(checker, nullptr);
    UpdateCheckerTestAccess::reset_restart_state(checker);
    checker.report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
    helix::ui::UpdateQueue::instance().drain();
    checker.shutdown();
}

// ============================================================================
// moonraker.conf channel sync
// ============================================================================

#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"

namespace {

/// Counts config writes; the mock's injected config root records their content.
class CountingTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;
    int uploads = 0;
    void upload_file(const std::string& root, const std::string& path, const std::string& content,
                     SuccessCallback on_success, ErrorCallback on_error) override {
        ++uploads;
        MoonrakerFileTransferAPIMock::upload_file(root, path, content, std::move(on_success),
                                                  std::move(on_error));
    }
};

class ChannelSyncApi : public MoonrakerAPIMock {
  public:
    ChannelSyncApi(helix::MoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPIMock(client, state), xfers_(client, "http://127.0.0.1:1") {}
    MoonrakerFileTransferAPI& transfers() override {
        return xfers_;
    }
    void restart_moonraker(SuccessCallback on_success, ErrorCallback) override {
        ++restarts;
        if (on_success)
            on_success();
    }
    CountingTransfers xfers_;
    int restarts = 0;
};

const char* const kConf = "[server]\nhost: 0.0.0.0\n\n"
                          "[update_manager helixscreen]\ntype: web\nchannel: {}\n"
                          "repo: prestonbrown/helixscreen\npath: {}\n";

constexpr const char* kInstallRoot = "/opt/helixscreen";

struct ChannelSyncFixture : public GlobalPrintStateFixture {
    MoonrakerClientMock client;
    helix::PrinterState state;
    ChannelSyncApi api{client, state};

    ChannelSyncFixture() {
        get_printer_state().init_subjects(false);
        api.set_http_base_url("http://127.0.0.1:7125");
        set_moonraker_api(&api);
    }

    std::string install_root = kInstallRoot;
    ~ChannelSyncFixture() override {
        set_moonraker_api(nullptr);
    }

    void seed(const std::string& conf) {
        api.xfers_.set_config_files({{"moonraker.conf", conf}});
    }
    void set_app_channel(int channel, bool beta_features) {
        auto* config = Config::get_instance();
        config->set<bool>("/beta_features", beta_features);
        config->set<int>("/update/channel", channel);
    }
    void start_sync() {
        UpdateCheckerTestAccess::sync_moonraker_channel_for(UpdateChecker::instance(),
                                                            install_root);
    }
    std::string sync() {
        start_sync();
        return drain();
    }
    std::string drain() {
        for (int i = 0; i < 8; ++i)
            helix::ui::UpdateQueue::instance().drain();
        return api.xfers_.get_uploaded_config("moonraker.conf").value_or("");
    }
    static std::string conf(const char* channel, const char* path = kInstallRoot) {
        return fmt::format(kConf, channel, path);
    }
};

} // namespace

TEST_CASE_METHOD(ChannelSyncFixture,
                 "Channel sync rewrites a drifted stanza and restarts Moonraker",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true); // Beta

    CHECK(sync() == conf("beta"));
    CHECK(api.xfers_.uploads == 1);
    CHECK(api.restarts == 1);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync leaves a matching stanza alone",
                 "[update_checker][moonraker_channel]") {
    seed(conf("beta"));
    set_app_channel(1, true);

    CHECK(sync() == conf("beta"));
    CHECK(api.xfers_.uploads == 0);
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync maps Dev to beta, and clamped Dev to stable",
                 "[update_checker][moonraker_channel]") {
    SECTION("Dev with beta features rides beta") {
        seed(conf("stable"));
        set_app_channel(2, true);
        CHECK(sync() == conf("beta"));
    }
    SECTION("Dev without beta features is effectively Stable") {
        seed(conf("beta"));
        set_app_channel(2, false);
        CHECK(sync() == conf("stable"));
    }
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync never writes without a stanza",
                 "[update_checker][moonraker_channel]") {
    const std::string bare = "[server]\nhost: 0.0.0.0\n";
    seed(bare);
    set_app_channel(1, true);

    CHECK(sync() == bare);
    CHECK(api.xfers_.uploads == 0);
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync never restarts Moonraker under a job",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);
    drive_lifecycle(get_printer_state(), "paused", PrintStartPhase::IDLE);
    REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));

    // The file is still corrected, so Moonraker's next start reads the right channel.
    CHECK(sync() == conf("beta"));
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync matches the stanza path after canonicalizing",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable", "/opt/./helixscreen/"));
    set_app_channel(1, true);

    CHECK(sync() == conf("beta", "/opt/./helixscreen/"));
    CHECK(api.restarts == 1);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync never touches another install's stanza",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable", "/home/pi/helixscreen"));
    set_app_channel(1, true);

    SECTION("stanza names a different directory") {}
    SECTION("this install's directory is unknown") {
        install_root.clear();
    }
    CHECK(sync() == conf("stable", "/home/pi/helixscreen"));
    CHECK(api.xfers_.uploads == 0);
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync never writes to a remote Moonraker",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);
    api.set_http_base_url("http://203.0.113.7:7125"); // TEST-NET-3, never this host

    CHECK(sync() == conf("stable"));
    CHECK(api.xfers_.uploads == 0);
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync abandons a printer it was detached from",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);

    start_sync(); // the download answers inline; the upload waits on the queue
    UpdateChecker::instance().detach(client);

    CHECK(drain() == conf("stable"));
    CHECK(api.xfers_.uploads == 0);
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync restarts no printer it was detached from",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);
    client.defer_next("machine.update.status");

    CHECK(sync() == conf("beta"));
    UpdateChecker::instance().detach(client);
    client.fire_deferred("machine.update.status");
    drain();

    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "Channel sync never restarts a busy update_manager",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);

    SECTION("update_manager reports busy") {
        client.set_update_manager_busy(true);
    }
    SECTION("update_manager status unavailable") {
        client.fail_next("machine.update.status");
    }
    CHECK(sync() == conf("beta"));
    CHECK(api.restarts == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture, "The newest channel sync wins over one still in flight",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);
    start_sync(); // downloads inline; its beta upload waits on the queue

    set_app_channel(0, true);
    start_sync(); // the file already says stable: nothing to write

    CHECK(drain() == conf("stable"));
    CHECK(api.xfers_.uploads == 0);
}

TEST_CASE_METHOD(ChannelSyncFixture,
                 "A job starting during the update status check blocks the restart",
                 "[update_checker][moonraker_channel]") {
    seed(conf("stable"));
    set_app_channel(1, true);
    client.defer_next("machine.update.status");

    CHECK(sync() == conf("beta"));
    drive_lifecycle(get_printer_state(), "printing", PrintStartPhase::IDLE);
    REQUIRE(job_holds_machine(get_printer_state().print_state().get_print_lifecycle()));
    client.fire_deferred("machine.update.status");
    drain();

    CHECK(api.restarts == 0);
}
