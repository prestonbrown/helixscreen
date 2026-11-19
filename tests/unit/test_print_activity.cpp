// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "locale_formats.h"
#include "print_activity.h"

#include <cstdlib>
#include <ctime>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// POSIX rule string, so no zoneinfo files are needed.
constexpr const char* kTestZone = "EST5EDT,M3.2.0,M11.1.0";

struct TzGuard {
    std::string saved;
    bool was_set = false;
    explicit TzGuard(const char* zone) {
        if (const char* v = ::getenv("TZ")) {
            saved = v;
            was_set = true;
        }
        ::setenv("TZ", zone, 1);
        ::tzset();
    }
    ~TzGuard() {
        if (was_set)
            ::setenv("TZ", saved.c_str(), 1);
        else
            ::unsetenv("TZ");
        ::tzset();
    }
};

std::time_t local_time(int y, int mo, int d, int h, int mi = 0) {
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    return mktime(&t);
}

ActivityDayStarts synthetic_starts() {
    ActivityDayStarts s{};
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<double>(i) * 86400.0;
    return s;
}

PrintHistoryJob job(double start, double end, double print_duration, double total = 0.0) {
    PrintHistoryJob j;
    j.start_time = start;
    j.end_time = end;
    j.print_duration = print_duration;
    j.total_duration = total;
    return j;
}

} // namespace

TEST_CASE("local_day_starts bounds seven whole local days", "[print_activity]") {
    TzGuard tz(kTestZone);
    const auto starts = local_day_starts(local_time(2026, 10, 14, 15));

    CHECK(starts[6] == static_cast<double>(local_time(2026, 10, 14, 0)));
    CHECK(starts[7] == static_cast<double>(local_time(2026, 10, 15, 0)));
    CHECK(starts[0] == static_cast<double>(local_time(2026, 10, 8, 0)));
    for (double s : starts) {
        std::time_t t = static_cast<std::time_t>(s);
        struct tm lt {};
        localtime_r(&t, &lt);
        CHECK(lt.tm_hour == 0);
        CHECK(lt.tm_min == 0);
        CHECK(lt.tm_sec == 0);
    }
}

TEST_CASE("local_day_starts keeps a 25-hour DST day whole", "[print_activity]") {
    TzGuard tz(kTestZone);
    const auto starts = local_day_starts(local_time(2026, 11, 3, 12));
    const double nov1 = static_cast<double>(local_time(2026, 11, 1, 0));

    bool found = false;
    for (size_t i = 0; i < kActivityDays; ++i) {
        if (starts[i] == nov1) {
            found = true;
            CHECK(starts[i + 1] - starts[i] == 90000.0);
        }
        CHECK(starts[i + 1] > starts[i]);
    }
    CHECK(found);
}

TEST_CASE("local_day_starts keeps a 23-hour DST day whole", "[print_activity]") {
    TzGuard tz(kTestZone);
    const auto starts = local_day_starts(local_time(2026, 3, 10, 12));
    const double mar8 = static_cast<double>(local_time(2026, 3, 8, 0));

    bool found = false;
    for (size_t i = 0; i < kActivityDays; ++i) {
        if (starts[i] == mar8) {
            found = true;
            CHECK(starts[i + 1] - starts[i] == 82800.0);
        }
    }
    CHECK(found);
}

TEST_CASE("summarize_week buckets print_duration by job_timestamp", "[print_activity]") {
    const auto starts = synthetic_starts();

    SECTION("print_duration, not total_duration, lands in the end day") {
        auto w = summarize_week({job(3 * 86400 + 10, 3 * 86400 + 9000, 7200, 9000)}, starts);
        CHECK(w.hours_by_day[3] == 2.0);
        CHECK(w.week_prints == 1);
    }
    SECTION("an in-progress row counts by start_time") {
        auto w = summarize_week({job(6 * 86400 + 100, 0, 3600)}, starts);
        CHECK(w.hours_by_day[6] == 1.0);
        CHECK(w.week_prints == 1);
    }
    SECTION("a print spanning midnight lands in its end day only") {
        auto w = summarize_week({job(2 * 86400 + 86000, 3 * 86400 + 1000, 3600)}, starts);
        CHECK(w.hours_by_day[2] == 0.0);
        CHECK(w.hours_by_day[3] == 1.0);
    }
    SECTION("out-of-window and timestampless rows are skipped") {
        auto w = summarize_week({job(0, starts[0] - 1, 3600), job(0, starts[7], 3600),
                                 job(0, 0, 3600), job(0, 86400 + 5, 3600)},
                                starts);
        CHECK(w.week_prints == 1);
        CHECK(w.hours_by_day[1] == 1.0);
    }
    SECTION("negative print_duration adds nothing") {
        auto w = summarize_week({job(0, 86400 + 5, -500)}, starts);
        CHECK(w.hours_by_day[1] == 0.0);
        CHECK(w.week_prints == 1);
    }
    SECTION("week_hours is the sum of the days") {
        auto w = summarize_week({job(0, 5, 3600), job(0, 86400 + 5, 5400), job(0, 6 * 86400, 1800)},
                                starts);
        double sum = 0;
        for (double h : w.hours_by_day)
            sum += h;
        CHECK(w.week_hours == sum);
        CHECK(w.week_hours == Catch::Approx(3.0));
    }
}

TEST_CASE("summarize_week over real local days across a DST change", "[print_activity]") {
    TzGuard tz(kTestZone);
    const auto starts = local_day_starts(local_time(2026, 11, 3, 12));
    const double nov1 = static_cast<double>(local_time(2026, 11, 1, 0));

    size_t dst_day = 0;
    for (size_t i = 0; i < kActivityDays; ++i) {
        if (starts[i] == nov1)
            dst_day = i;
    }
    // A job exactly at a day's start belongs to that day; one second earlier to the day before.
    const auto w =
        summarize_week({job(0, starts[dst_day], 3600), job(0, starts[dst_day] - 1, 7200),
                        job(0, starts[dst_day + 1] - 1, 1800), job(0, starts[dst_day + 1], 5400)},
                       starts);
    CHECK(w.hours_by_day[dst_day] == Catch::Approx(1.5));
    CHECK(w.hours_by_day[dst_day - 1] == Catch::Approx(2.0));
    CHECK(w.hours_by_day[dst_day + 1] == Catch::Approx(1.5));
    double sum = 0;
    for (double h : w.hours_by_day)
        sum += h;
    CHECK(w.week_hours == sum);
    CHECK(w.week_prints == 4);
}

TEST_CASE("scale_day_bars scales to the tallest day", "[print_activity]") {
    using A = std::array<double, kActivityDays>;
    using B = std::array<int, kActivityDays>;
    CHECK(scale_day_bars(A{0, 0, 0, 0, 0, 0, 0}) == B{0, 0, 0, 0, 0, 0, 0});
    CHECK(scale_day_bars(A{1, 2, 4, 0, 0, 0, 8}) == B{13, 25, 50, 0, 0, 0, 100});
    CHECK(scale_day_bars(A{0.01, 0, 0, 0, 0, 0, 10}) == B{6, 0, 0, 0, 0, 0, 100});
}

TEST_CASE("format_print_hours", "[print_activity]") {
    CHECK(format_print_hours(0) == "0 h");
    CHECK(format_print_hours(-3) == "0 h");
    CHECK(format_print_hours(0.004) == "1 min");
    CHECK(format_print_hours(0.75) == "45 min");
    CHECK(format_print_hours(0.999) == "1 h");
    CHECK(format_print_hours(1.0) == "1 h");
    CHECK(format_print_hours(2.54) == "2.5 h");
    CHECK(format_print_hours(2.04) == "2 h");
    CHECK(format_print_hours(9.96) == "10 h");
    CHECK(format_print_hours(23.4) == "23 h");
    CHECK(format_print_hours(1204.6) == "1205 h");
}

TEST_CASE("format_print_count", "[print_activity]") {
    CHECK(format_print_count(0) == "0 prints");
    CHECK(format_print_count(1) == "1 print");
    CHECK(format_print_count(128) == "128 prints");
}

TEST_CASE("format_filament_meters", "[print_activity]") {
    CHECK(format_filament_meters(0).empty());
    CHECK(format_filament_meters(-5).empty());
    CHECK(format_filament_meters(30) == "0.1 m");
    CHECK(format_filament_meters(2540) == "2.5 m");
    CHECK(format_filament_meters(2040) == "2 m");
    CHECK(format_filament_meters(9960) == "10 m");
    CHECK(format_filament_meters(412300) == "412 m");
    CHECK(format_filament_meters(12345678) == "12346 m");
}

TEST_CASE("format_lifetime_line", "[print_activity]") {
    constexpr uint64_t k412Hours = 412 * 3600;
    CHECK(format_lifetime_line({128, k412Hours, 412300.0}, true) == "128 prints · 412 h · 412 m");
    CHECK(format_lifetime_line({128, k412Hours, 412300.0}, false) == "128 prints · 412 h");
    CHECK(format_lifetime_line({128, k412Hours, 0.0}, true) == "128 prints · 412 h");
}

TEST_CASE("weekday_initial", "[print_activity]") {
    CHECK(weekday_initial("Mon") == "M");
    CHECK(weekday_initial("mié.") == "M");
    CHECK(weekday_initial("dim.") == "D");
    CHECK(weekday_initial("пн") == "п");
    CHECK(weekday_initial("月") == "月");
    CHECK(weekday_initial("").empty());
}

TEST_CASE("format_localized_weekday_abbr reads tm_wday", "[print_activity]") {
    helix::locale::locale_set_language("en");
    struct tm t {};
    t.tm_wday = 1;
    CHECK(weekday_initial(helix::locale::format_localized_weekday_abbr(&t)) == "M");
}
