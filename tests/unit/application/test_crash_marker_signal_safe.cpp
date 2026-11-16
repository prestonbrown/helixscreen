// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_crash_marker_signal_safe.cpp
 * @brief Tests for the async-signal-safe crash-restart marker clear.
 *
 * Application::run() appends a timestamp to .crash_restart_count on every start
 * and refuses to boot once three of them land inside 120s. It is cleared by
 * Application::shutdown() — which the SIGTERM handler deliberately skips, since
 * it does a bare _exit(0). Without its own clear, every supervisor stop would
 * leave its timestamp behind.
 *
 * On the Elegoo CC1, COSMOS's resonance-calibration macro stops and starts the
 * UI through gui-switcher (SIGTERM by pidfile). Three cycles inside the window
 * would make HelixScreen refuse to boot with "Crash loop detected".
 *
 * The handler must stay async-signal-safe, so it may not call
 * std::filesystem::remove or construct the path. clear_crash_marker_signal_safe()
 * is unlink(2) over a buffer that cache_crash_marker_path_for_signal() filled on
 * the main thread at startup. These tests exercise that pair directly; the
 * handler's one-line call to it cannot be unit-tested because the handler ends
 * in _exit(0).
 */

#include "../../../include/process_guards.h"
#include "../../test_helpers/scoped_runtime_config.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../../catch_amalgamated.hpp"

namespace fs = std::filesystem;

namespace {

/// A unique scratch marker path under the system temp dir.
std::string temp_marker_path(const char* tag) {
    return (fs::temp_directory_path() / (std::string("helix_crash_marker_test_") + tag)).string();
}

void write_marker(const std::string& path) {
    std::ofstream out(path, std::ios::trunc);
    out << "1770000000\n1770000001\n";
}

} // namespace

TEST_CASE("clear_crash_marker_signal_safe deletes the cached marker", "[application][crash_loop]") {
    const std::string path = temp_marker_path("clear");
    write_marker(path);
    REQUIRE(fs::exists(path));

    REQUIRE(helix::cache_crash_marker_path_for_signal(path));
    helix::clear_crash_marker_signal_safe();

    // This is the whole point: a supervisor SIGTERM must not leave a start
    // timestamp behind to be counted toward the next boot's crash-loop total.
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("clear_crash_marker_signal_safe is idempotent and tolerates a missing marker",
          "[application][crash_loop]") {
    const std::string path = temp_marker_path("missing");
    fs::remove(path);
    REQUIRE(helix::cache_crash_marker_path_for_signal(path));

    // ENOENT is the normal case (test mode never writes a marker). Must not
    // throw, abort, or otherwise misbehave inside a signal handler.
    helix::clear_crash_marker_signal_safe();
    helix::clear_crash_marker_signal_safe();
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("caching re-points the clear at the newest path", "[application][crash_loop]") {
    const std::string first = temp_marker_path("first");
    const std::string second = temp_marker_path("second");
    write_marker(first);
    write_marker(second);

    REQUIRE(helix::cache_crash_marker_path_for_signal(first));
    REQUIRE(helix::cache_crash_marker_path_for_signal(second));
    helix::clear_crash_marker_signal_safe();

    // Only the most recently cached path is touched — proves the clear reads
    // the buffer rather than, say, globbing or remembering the first call.
    CHECK(fs::exists(first));
    CHECK_FALSE(fs::exists(second));

    fs::remove(first);
}

TEST_CASE("an unusable path is rejected and disarms the clear", "[application][crash_loop]") {
    const std::string real = temp_marker_path("guarded");
    write_marker(real);
    REQUIRE(helix::cache_crash_marker_path_for_signal(real));

    SECTION("empty path") {
        CHECK_FALSE(helix::cache_crash_marker_path_for_signal(""));
    }

    SECTION("path longer than the static buffer") {
        // Must be rejected rather than truncated: a truncated path could name a
        // real, different file and unlink(2) would happily delete it.
        CHECK_FALSE(helix::cache_crash_marker_path_for_signal(std::string(8192, 'x')));
    }

    // Rejection disarms the clear entirely rather than leaving the previous
    // path armed under a new, wrong name.
    helix::clear_crash_marker_signal_safe();
    CHECK(fs::exists(real));

    fs::remove(real);
}

TEST_CASE("a start after three recent ones is a crash loop and clears the marker",
          "[application][crash_loop]") {
    ScopedRuntimeConfig scoped_config; // a detected loop arms safe mode globally
    const std::string path = temp_marker_path("loop");
    fs::remove(path);
    constexpr long long kNow = 1770000000;

    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow));
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 10));
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 20));
    REQUIRE(fs::exists(path));
    CHECK(helix::record_start_and_check_crash_loop(path, kNow + 30));
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("starts older than the window do not count toward a crash loop",
          "[application][crash_loop]") {
    const std::string path = temp_marker_path("window");
    fs::remove(path);
    constexpr long long kNow = 1770000000;

    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow));
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 10));
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 20));
    // 200s later all three have aged out of the 120s window.
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 200));
    fs::remove(path);
}

TEST_CASE("a crash loop boots this run in safe mode", "[application][crash_loop]") {
    ScopedRuntimeConfig scoped_config;
    get_runtime_config()->crash_loop_safe_mode = false;
    const std::string path = temp_marker_path("safe_mode");
    fs::remove(path);
    constexpr long long kNow = 1770000000;

    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow));
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 10));
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 20));
    CHECK_FALSE(get_runtime_config()->crash_loop_safe_mode);

    REQUIRE(helix::record_start_and_check_crash_loop(path, kNow + 30));
    CHECK(get_runtime_config()->crash_loop_safe_mode);

    // The flag lives only in this process. The next start sees an empty
    // marker, so a fresh process stays in normal mode.
    get_runtime_config()->crash_loop_safe_mode = false;
    CHECK_FALSE(helix::record_start_and_check_crash_loop(path, kNow + 40));
    CHECK_FALSE(get_runtime_config()->crash_loop_safe_mode);
    fs::remove(path);
}

namespace {

/// A directory holding a marker that the process cannot delete: write
/// permission on the directory is what unlink(2) needs, and a read-only
/// filesystem fails the same call with EROFS.
struct UnremovableMarker {
    fs::path dir;
    std::string path;

    explicit UnremovableMarker(const char* tag) {
        dir = fs::temp_directory_path() / (std::string("helix_crash_marker_ro_") + tag);
        // A run that died mid-test leaves the directory read-only behind it.
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        path = (dir / ".crash_restart_count").string();
        std::ofstream(path) << "1770000000\n1770000010\n1770000020\n";
        fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    }
    ~UnremovableMarker() {
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
    }
    /// Root ignores directory permissions, so the setup proves nothing there.
    bool effective() const {
        return ::access(dir.c_str(), W_OK) != 0;
    }
};

} // namespace

TEST_CASE("clearing a marker that cannot be removed does not throw", "[application][crash_loop]") {
    UnremovableMarker marker("clear");
    if (!marker.effective()) {
        SKIP("running as root: directory permissions do not stop unlink");
    }
    CHECK_NOTHROW(helix::clear_crash_marker(marker.path));
    CHECK(fs::exists(marker.path));

    // A marker that was never written is the common clean-shutdown case.
    CHECK_NOTHROW(helix::clear_crash_marker((marker.dir / "absent").string()));
}

TEST_CASE("a crash loop whose marker cannot be removed still boots in safe mode",
          "[application][crash_loop]") {
    ScopedRuntimeConfig scoped_config;
    UnremovableMarker marker("loop");
    if (!marker.effective()) {
        SKIP("running as root: directory permissions do not stop unlink");
    }
    bool loop = false;
    REQUIRE_NOTHROW(loop = helix::record_start_and_check_crash_loop(marker.path, 1770000030));
    CHECK(loop);
}
