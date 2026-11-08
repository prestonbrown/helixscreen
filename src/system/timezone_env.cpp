// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "timezone_env.h"

#include "spdlog/spdlog.h"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace helix::timezone_env {

namespace {

constexpr const char* kDefaultSystemProbe = "/usr/share/zoneinfo/UTC";
constexpr const char* kDefaultBundledSubdir = "assets/zoneinfo";

bool probed_ = false;
std::string active_tzdir_;

// The TZ the process inherited, recorded before the first change to it.
bool startup_tz_recorded_ = false;
bool startup_tz_set_ = false;
std::string startup_tz_;

void record_startup_tz() {
    if (startup_tz_recorded_) {
        return;
    }
    startup_tz_recorded_ = true;
    const char* tz = ::getenv("TZ");
    startup_tz_set_ = tz != nullptr;
    startup_tz_ = tz ? tz : "";
}

// Parses a zone abbreviation: three or more letters, or <...> quoted.
bool parse_tz_name(const char*& p) {
    if (*p == '<') {
        const char* close = std::strchr(p, '>');
        if (close == nullptr || close == p + 1) {
            return false;
        }
        p = close + 1;
        return true;
    }
    const char* start = p;
    while (std::isalpha(static_cast<unsigned char>(*p))) {
        ++p;
    }
    return p - start >= 3;
}

// Parses [+-]hh[:mm[:ss]] and returns it as minutes EAST of UTC (POSIX counts west).
bool parse_tz_offset(const char*& p, int& minutes_east) {
    int sign = 1;
    if (*p == '+' || *p == '-') {
        sign = (*p == '-') ? -1 : 1;
        ++p;
    }
    if (!std::isdigit(static_cast<unsigned char>(*p))) {
        return false;
    }
    int fields[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
        while (std::isdigit(static_cast<unsigned char>(*p))) {
            fields[i] = fields[i] * 10 + (*p - '0');
            ++p;
        }
        if (*p != ':') {
            break;
        }
        ++p;
    }
    minutes_east = -sign * (fields[0] * 60 + fields[1]);
    return true;
}

// Parses ",Mm.w.d[/time]" and returns the month m.
bool parse_m_rule(const char*& p, int& month) {
    if (*p != ',' || p[1] != 'M') {
        return false;
    }
    p += 2;
    char* end = nullptr;
    long m = std::strtol(p, &end, 10);
    if (end == p || *end != '.' || m < 1 || m > 12) {
        return false;
    }
    month = static_cast<int>(m);
    p = end;
    while (*p != '\0' && *p != ',') {
        ++p;
    }
    return true;
}

bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

} // namespace

const char* configure_tzdir(const char* system_probe, const char* bundled_subdir) {
    if (probed_) {
        return active_tzdir_.c_str();
    }
    probed_ = true;

    const char* probe = system_probe ? system_probe : kDefaultSystemProbe;
    const char* subdir = bundled_subdir ? bundled_subdir : kDefaultBundledSubdir;

    if (path_exists(probe)) {
        // System tzdata present — let glibc use its default TZDIR.
        return active_tzdir_.c_str();
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        spdlog::warn("[timezone_env] System zoneinfo missing and getcwd failed; "
                     "times may display as UTC");
        return active_tzdir_.c_str();
    }

    std::string bundled = std::string(cwd) + "/" + subdir;
    if (!path_exists(bundled + "/UTC")) {
        spdlog::warn("[timezone_env] System zoneinfo missing and no bundled fallback at '{}'; "
                     "times may display as UTC",
                     bundled);
        return active_tzdir_.c_str();
    }

    ::setenv("TZDIR", bundled.c_str(), 1);
    active_tzdir_ = std::move(bundled);
    spdlog::info("[timezone_env] System zoneinfo missing; using bundled TZDIR={}", active_tzdir_);
    return active_tzdir_.c_str();
}

bool posix_offsets(const char* posix_tz, int& jan_minutes_east, int& jul_minutes_east) {
    if (posix_tz == nullptr) {
        return false;
    }
    const char* p = posix_tz;
    int std_offset = 0;
    if (!parse_tz_name(p) || !parse_tz_offset(p, std_offset)) {
        return false;
    }
    if (*p == '\0') {
        jan_minutes_east = jul_minutes_east = std_offset;
        return true;
    }
    if (!parse_tz_name(p)) {
        return false;
    }
    int dst_offset = std_offset + 60;
    if (*p != ',' && *p != '\0' && !parse_tz_offset(p, dst_offset)) {
        return false;
    }
    int start_month = 0;
    int end_month = 0;
    if (!parse_m_rule(p, start_month) || !parse_m_rule(p, end_month) || *p != '\0') {
        return false;
    }
    // Summer time that starts before it ends in the calendar year is northern.
    const bool northern = start_month < end_month;
    jan_minutes_east = northern ? std_offset : dst_offset;
    jul_minutes_east = northern ? dst_offset : std_offset;
    return true;
}

void apply_system() {
    record_startup_tz();
#ifdef ESP_PLATFORM
    ::setenv("TZ", "UTC0", 1);
#else
    if (startup_tz_set_) {
        ::setenv("TZ", startup_tz_.c_str(), 1);
    } else {
        ::unsetenv("TZ");
    }
#endif
    ::tzset();
}

bool system_zone_known() {
#ifdef ESP_PLATFORM
    return false;
#else
    record_startup_tz();
    return (startup_tz_set_ && !startup_tz_.empty()) || ::access("/etc/localtime", F_OK) == 0;
#endif
}

void apply(const char* iana_id, const char* posix_tz) {
    record_startup_tz();
#ifdef ESP_PLATFORM
    // newlib reads no zoneinfo: an IANA name parses as UTC.
    if (posix_tz) {
        ::setenv("TZ", posix_tz, 1);
        ::tzset();
        return;
    }
#else
    (void)posix_tz;
#endif
    // musl ignores TZDIR, and every static device build links musl, so a zone
    // from the bundled tree is named by its absolute path. glibc reads that the same way.
    const std::string tzdir = configure_tzdir();
    const std::string tz = tzdir.empty() ? std::string(iana_id) : tzdir + "/" + iana_id;
    ::setenv("TZ", tz.c_str(), 1);
    ::tzset();
}

void reset_probe_state() {
    probed_ = false;
    active_tzdir_.clear();
    startup_tz_recorded_ = false;
    startup_tz_set_ = false;
    startup_tz_.clear();
    ::unsetenv("TZDIR");
    ::unsetenv("TZ");
    ::tzset();
}

} // namespace helix::timezone_env
