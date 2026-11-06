// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests helix::timezone_env — the TZDIR fallback that lets the app apply
// IANA timezones on devices that ship without /usr/share/zoneinfo/ (notably
// Elegoo Centauri Carbon running OpenCentauri COSMOS).

#include "test_helpers/scoped_env.h"
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
struct TimezoneScope {
    // Declared first so it is destroyed last: the probe state is reset only
    // after TZ and TZDIR are back to their original values.
    struct ProbeReset {
        ~ProbeReset() {
            ::tzset();
            helix::timezone_env::reset_probe_state();
        }
    } probe_reset;
    helix::ScopedEnv tz{"TZ"};
    helix::ScopedEnv tzdir{"TZDIR"};
    fs::path saved_cwd = fs::current_path();

    ~TimezoneScope() {
        fs::current_path(saved_cwd);
    }
};

} // namespace

TEST_CASE("timezone_env: falls back to bundled TZDIR when system zoneinfo missing",
          "[timezone_env]") {
    TimezoneScope guard;
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
    TimezoneScope guard;
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

TEST_CASE("timezone_env: a bundled zone resolves without TZDIR, as musl reads it",
          "[timezone_env][2003]") {
    TimezoneScope guard;
    helix::timezone_env::reset_probe_state();

    fs::path temp = make_temp_zoneinfo();
    fs::path fake_root = temp.parent_path() / ("helix_tz_root3_" + temp.filename().string());
    fs::create_directories(fake_root);
    fs::current_path(fake_root);
    fs::create_directory_symlink(temp, fake_root / "bundled_zi");
    const fs::path bundled = fs::canonical(fake_root) / "bundled_zi";

    (void)helix::timezone_env::configure_tzdir("/nonexistent/path/UTC", "bundled_zi");
    helix::timezone_env::apply("America/New_York");

    const char* tz = ::getenv("TZ");
    REQUIRE(tz != nullptr);
    CHECK(std::string(tz) == (bundled / "America/New_York").string());

    // musl never reads TZDIR: with it gone the zone must still resolve.
    ::unsetenv("TZDIR");
    ::tzset();

    // 2026-06-15 12:00:00 UTC is 08:00 EDT.
    std::tm utc_tm{};
    utc_tm.tm_year = 2026 - 1900;
    utc_tm.tm_mon = 5;
    utc_tm.tm_mday = 15;
    utc_tm.tm_hour = 12;
    std::time_t epoch = ::timegm(&utc_tm);
    std::tm local_tm{};
    REQUIRE(::localtime_r(&epoch, &local_tm) != nullptr);
    CHECK(local_tm.tm_hour == 8);

    fs::remove_all(temp);
    fs::remove_all(fake_root);
}

TEST_CASE("timezone_env: with system zoneinfo the zone is applied by name",
          "[timezone_env][2003]") {
    TimezoneScope guard;
    helix::timezone_env::reset_probe_state();

    std::string probe = "/usr/share/zoneinfo/UTC";
    if (!fs::exists(probe)) {
        probe = "/etc/hostname";
    }
    (void)helix::timezone_env::configure_tzdir(probe.c_str(), "assets/zoneinfo");
    helix::timezone_env::apply("America/New_York");

    const char* tz = ::getenv("TZ");
    REQUIRE(tz != nullptr);
    CHECK(std::string(tz) == "America/New_York");
}

TEST_CASE("timezone_env: leaves TZDIR untouched when system zoneinfo present", "[timezone_env]") {
    TimezoneScope guard;
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

TEST_CASE("timezone_env: posix_offsets reads January and July offsets from a POSIX rule",
          "[timezone_env]") {
    using helix::timezone_env::posix_offsets;
    int jan = 999;
    int jul = 999;

    SECTION("fixed zone") {
        REQUIRE(posix_offsets("UTC0", jan, jul));
        CHECK(jan == 0);
        CHECK(jul == 0);
        REQUIRE(posix_offsets("JST-9", jan, jul));
        CHECK(jan == 540);
        CHECK(jul == 540);
    }
    SECTION("quoted names and minute offsets") {
        REQUIRE(posix_offsets("<+0545>-5:45", jan, jul));
        CHECK(jan == 345);
        CHECK(jul == 345);
        REQUIRE(posix_offsets("<-03>3", jan, jul));
        CHECK(jan == -180);
        CHECK(jul == -180);
    }
    SECTION("northern DST is summer time in July") {
        REQUIRE(posix_offsets("EST5EDT,M3.2.0,M11.1.0", jan, jul));
        CHECK(jan == -300);
        CHECK(jul == -240);
        REQUIRE(posix_offsets("NST3:30NDT,M3.2.0,M11.1.0", jan, jul));
        CHECK(jan == -210);
        CHECK(jul == -150);
        REQUIRE(posix_offsets("GMT0BST,M3.5.0/1,M10.5.0", jan, jul));
        CHECK(jan == 0);
        CHECK(jul == 60);
    }
    SECTION("southern DST is summer time in January") {
        REQUIRE(posix_offsets("AEST-10AEDT,M10.1.0,M4.1.0/3", jan, jul));
        CHECK(jan == 660);
        CHECK(jul == 600);
        REQUIRE(posix_offsets("<-04>4<-03>,M9.1.6/24,M4.1.6/24", jan, jul));
        CHECK(jan == -180);
        CHECK(jul == -240);
    }
    SECTION("an explicit DST offset is used as written") {
        REQUIRE(posix_offsets("AAA5BBB3,M3.2.0,M11.1.0", jan, jul));
        CHECK(jan == -300);
        CHECK(jul == -180);
    }
    SECTION("malformed rules are refused") {
        CHECK_FALSE(posix_offsets(nullptr, jan, jul));
        CHECK_FALSE(posix_offsets("", jan, jul));
        CHECK_FALSE(posix_offsets("EST", jan, jul));
        CHECK_FALSE(posix_offsets("E5", jan, jul));
        CHECK_FALSE(posix_offsets("<-03", jan, jul));
        CHECK_FALSE(posix_offsets("EST5EDT,J60,J300", jan, jul));
        CHECK_FALSE(posix_offsets("EST5EDT,M3.2.0", jan, jul));
        CHECK_FALSE(posix_offsets("EST5x", jan, jul));
    }
}

TEST_CASE("timezone_env: apply_system restores the zone the process started with",
          "[timezone_env]") {
    TimezoneScope guard;

    SECTION("a TZ set before the first apply comes back") {
        helix::timezone_env::reset_probe_state();
        ::setenv("TZ", "EST5EDT", 1);
        helix::timezone_env::apply("UTC");
        REQUIRE(std::string(::getenv("TZ")) == "UTC");
        helix::timezone_env::apply_system();
        REQUIRE(::getenv("TZ") != nullptr);
        CHECK(std::string(::getenv("TZ")) == "EST5EDT");
    }
    SECTION("no TZ at startup means none after") {
        helix::timezone_env::reset_probe_state();
        helix::timezone_env::apply("UTC");
        helix::timezone_env::apply_system();
        CHECK(::getenv("TZ") == nullptr);
    }
    SECTION("apply_system before any apply leaves TZ as it is") {
        helix::timezone_env::reset_probe_state();
        ::setenv("TZ", "JST-9", 1);
        helix::timezone_env::apply_system();
        REQUIRE(::getenv("TZ") != nullptr);
        CHECK(std::string(::getenv("TZ")) == "JST-9");
    }
}
