// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests helix::timezone_env — the TZDIR fallback that lets the app apply
// IANA timezones on devices that ship without /usr/share/zoneinfo/ (notably
// Elegoo Centauri Carbon running OpenCentauri COSMOS).

#include "test_helpers/unique_temp_dir.h"
#include "timezone_env.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;

namespace {

// Copy a single /usr/share/zoneinfo/<zone> TZif file into dest/<zone>.
// Test requires the host (dev box / CI) to have glibc tzdata installed.
void stage_zone(const fs::path& dest_root, const std::string& zone) {
    fs::path src = fs::path("/usr/share/zoneinfo") / zone;
    fs::path dst = dest_root / zone;
    REQUIRE(fs::exists(src));
    fs::create_directories(dst.parent_path());
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

// Build a temp zoneinfo dir containing exactly UTC + America/New_York.
fs::path make_temp_zoneinfo() {
    fs::path root = helix::test::unique_temp_dir("helix_tz_test");
    fs::create_directories(root);
    stage_zone(root, "UTC");
    stage_zone(root, "America/New_York");
    return root;
}

// Snapshot/restore TZ + TZDIR + working directory across tests. The fallback
// inspects getcwd() when building the bundled path.
struct EnvGuard {
    std::string saved_tz;
    std::string saved_tzdir;
    fs::path saved_cwd;
    bool tz_was_set = false;
    bool tzdir_was_set = false;

    EnvGuard() {
        if (const char* v = ::getenv("TZ")) {
            saved_tz = v;
            tz_was_set = true;
        }
        if (const char* v = ::getenv("TZDIR")) {
            saved_tzdir = v;
            tzdir_was_set = true;
        }
        saved_cwd = fs::current_path();
    }
    ~EnvGuard() {
        fs::current_path(saved_cwd);
        if (tz_was_set)
            ::setenv("TZ", saved_tz.c_str(), 1);
        else
            ::unsetenv("TZ");
        if (tzdir_was_set)
            ::setenv("TZDIR", saved_tzdir.c_str(), 1);
        else
            ::unsetenv("TZDIR");
        ::tzset();
        helix::timezone_env::reset_probe_state();
    }
};

} // namespace

TEST_CASE("timezone_env: falls back to bundled TZDIR when system zoneinfo missing",
          "[timezone_env]") {
    EnvGuard guard;
    helix::timezone_env::reset_probe_state();

    fs::path temp = make_temp_zoneinfo();
    fs::path fake_root = temp.parent_path() / ("helix_tz_root_" + temp.filename().string());
    fs::create_directories(fake_root);
    fs::current_path(fake_root);
    fs::create_directory_symlink(temp, fake_root / "bundled_zi");

    // configure_tzdir() uses getcwd() which resolves symlinks (macOS:
    // /var/folders → /private/var/folders). Compare against the canonicalized
    // path so the test holds on both Linux (no-op) and macOS.
    const fs::path canonical_root = fs::canonical(fake_root);

    const std::string nonexistent_probe = "/nonexistent/helix/zoneinfo/UTC";
    const char* result =
        helix::timezone_env::configure_tzdir(nonexistent_probe.c_str(), "bundled_zi");

    REQUIRE(result != nullptr);
    REQUIRE(std::string(result) == (canonical_root / "bundled_zi").string());

    const char* tzdir_env = ::getenv("TZDIR");
    REQUIRE(tzdir_env != nullptr);
    REQUIRE(std::string(tzdir_env) == (canonical_root / "bundled_zi").string());

    fs::remove_all(temp);
    fs::remove_all(fake_root);
}

TEST_CASE("timezone_env: apply() with bundled TZDIR yields correct localtime offset",
          "[timezone_env]") {
    EnvGuard guard;
    helix::timezone_env::reset_probe_state();

    fs::path temp = make_temp_zoneinfo();
    fs::path fake_root = temp.parent_path() / ("helix_tz_root2_" + temp.filename().string());
    fs::create_directories(fake_root);
    fs::current_path(fake_root);
    fs::create_directory_symlink(temp, fake_root / "bundled_zi");

    (void)helix::timezone_env::configure_tzdir("/nonexistent/path/UTC", "bundled_zi");
    helix::timezone_env::apply("America/New_York");

    // 2026-06-15 12:00:00 UTC is 08:00 EDT (DST active, UTC-4).
    // Using a summer date avoids DST edge cases.
    std::tm utc_tm{};
    utc_tm.tm_year = 2026 - 1900;
    utc_tm.tm_mon = 5; // June (0-indexed)
    utc_tm.tm_mday = 15;
    utc_tm.tm_hour = 12;
    utc_tm.tm_min = 0;
    utc_tm.tm_sec = 0;
    std::time_t epoch = ::timegm(&utc_tm);

    std::tm local_tm{};
    REQUIRE(::localtime_r(&epoch, &local_tm) != nullptr);

    CHECK(local_tm.tm_hour == 8);
    CHECK(local_tm.tm_mday == 15);
    CHECK(local_tm.tm_mon == 5);
    CHECK(local_tm.tm_year == 2026 - 1900);

    helix::timezone_env::apply("UTC");
    REQUIRE(::localtime_r(&epoch, &local_tm) != nullptr);
    CHECK(local_tm.tm_hour == 12);

    fs::remove_all(temp);
    fs::remove_all(fake_root);
}

TEST_CASE("timezone_env: leaves TZDIR untouched when system zoneinfo present", "[timezone_env]") {
    EnvGuard guard;
    helix::timezone_env::reset_probe_state();
    ::unsetenv("TZDIR");

    // Use a probe path that definitely exists (the system's own zoneinfo if
    // present, else any existing file).
    std::string probe = "/usr/share/zoneinfo/UTC";
    if (!fs::exists(probe)) {
        probe = "/etc/hostname"; // Fallback: any existing file suffices for the probe
    }

    const char* result = helix::timezone_env::configure_tzdir(probe.c_str(), "assets/zoneinfo");
    REQUIRE(result != nullptr);
    CHECK(std::string(result).empty());
    CHECK(::getenv("TZDIR") == nullptr);
}
