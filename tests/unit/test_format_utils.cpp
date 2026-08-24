// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"

#include "format_utils.h"

#include <cstring>
#include <ctime>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::format;

// =============================================================================
// UNAVAILABLE constant
// =============================================================================

TEST_CASE("UNAVAILABLE constant is em dash", "[format_utils]") {
    CHECK(std::string(UNAVAILABLE) == "—");
}

// =============================================================================
// Percentage formatting
// =============================================================================

TEST_CASE("format_percent basic cases", "[format_utils][percent]") {
    char buf[16];

    SECTION("formats integer percentages") {
        CHECK(std::string(format_percent(0, buf, sizeof(buf))) == "0%");
        CHECK(std::string(format_percent(45, buf, sizeof(buf))) == "45%");
        CHECK(std::string(format_percent(100, buf, sizeof(buf))) == "100%");
    }

    SECTION("handles boundary values") {
        CHECK(std::string(format_percent(-5, buf, sizeof(buf))) == "-5%");
        CHECK(std::string(format_percent(255, buf, sizeof(buf))) == "255%");
    }
}

TEST_CASE("format_percent_or_unavailable", "[format_utils][percent]") {
    char buf[16];

    SECTION("returns formatted percent when available") {
        CHECK(std::string(format_percent_or_unavailable(50, true, buf, sizeof(buf))) == "50%");
    }

    SECTION("returns UNAVAILABLE when not available") {
        CHECK(std::string(format_percent_or_unavailable(50, false, buf, sizeof(buf))) == "—");
    }
}

TEST_CASE("format_percent_float with decimals", "[format_utils][percent]") {
    char buf[16];

    SECTION("formats with 0 decimals") {
        CHECK(std::string(format_percent_float(45.7, 0, buf, sizeof(buf))) == "46%");
        CHECK(std::string(format_percent_float(100.0, 0, buf, sizeof(buf))) == "100%");
    }

    SECTION("formats with 1 decimal") {
        CHECK(std::string(format_percent_float(45.5, 1, buf, sizeof(buf))) == "45.5%");
        CHECK(std::string(format_percent_float(99.9, 1, buf, sizeof(buf))) == "99.9%");
    }

    SECTION("formats with 2 decimals") {
        CHECK(std::string(format_percent_float(45.55, 2, buf, sizeof(buf))) == "45.55%");
    }
}

TEST_CASE("format_humidity from x10 value", "[format_utils][percent]") {
    char buf[16];

    SECTION("converts x10 values to whole percent") {
        CHECK(std::string(format_humidity(455, buf, sizeof(buf))) == "45%");
        CHECK(std::string(format_humidity(1000, buf, sizeof(buf))) == "100%");
        CHECK(std::string(format_humidity(0, buf, sizeof(buf))) == "0%");
    }

    SECTION("rounds correctly") {
        CHECK(std::string(format_humidity(456, buf, sizeof(buf))) == "45%");
        CHECK(std::string(format_humidity(459, buf, sizeof(buf))) == "45%");
    }
}

// =============================================================================
// Distance formatting
// =============================================================================

TEST_CASE("format_distance_mm with precision", "[format_utils][distance]") {
    char buf[32];

    SECTION("formats with specified precision") {
        CHECK(std::string(format_distance_mm(1.234, 2, buf, sizeof(buf))) == "1.23 mm");
        CHECK(std::string(format_distance_mm(0.1, 3, buf, sizeof(buf))) == "0.100 mm");
        CHECK(std::string(format_distance_mm(10.0, 0, buf, sizeof(buf))) == "10 mm");
    }

    SECTION("handles negative values") {
        CHECK(std::string(format_distance_mm(-0.5, 2, buf, sizeof(buf))) == "-0.50 mm");
    }
}

TEST_CASE("format_diameter_mm fixed 2 decimals", "[format_utils][distance]") {
    char buf[32];

    CHECK(std::string(format_diameter_mm(1.75f, buf, sizeof(buf))) == "1.75 mm");
    CHECK(std::string(format_diameter_mm(2.85f, buf, sizeof(buf))) == "2.85 mm");
    CHECK(std::string(format_diameter_mm(1.0f, buf, sizeof(buf))) == "1.00 mm");
}

// =============================================================================
// Speed formatting
// =============================================================================

TEST_CASE("format_speed_mm_s", "[format_utils][speed]") {
    char buf[32];

    CHECK(std::string(format_speed_mm_s(150.0, buf, sizeof(buf))) == "150 mm/s");
    CHECK(std::string(format_speed_mm_s(0.0, buf, sizeof(buf))) == "0 mm/s");
    CHECK(std::string(format_speed_mm_s(300.5, buf, sizeof(buf))) == "300 mm/s");
}

TEST_CASE("format_speed_mm_min", "[format_utils][speed]") {
    char buf[32];

    CHECK(std::string(format_speed_mm_min(300.0, buf, sizeof(buf))) == "300 mm/min");
    CHECK(std::string(format_speed_mm_min(0.0, buf, sizeof(buf))) == "0 mm/min");
}

// =============================================================================
// Acceleration formatting
// =============================================================================

TEST_CASE("format_accel_mm_s2", "[format_utils][accel]") {
    char buf[32];

    CHECK(std::string(format_accel_mm_s2(3000.0, buf, sizeof(buf))) == "3000 mm/s²");
    CHECK(std::string(format_accel_mm_s2(500.0, buf, sizeof(buf))) == "500 mm/s²");
    CHECK(std::string(format_accel_mm_s2(0.0, buf, sizeof(buf))) == "0 mm/s²");
}

// =============================================================================
// Frequency formatting
// =============================================================================

TEST_CASE("format_frequency_hz", "[format_utils][frequency]") {
    char buf[32];

    CHECK(std::string(format_frequency_hz(48.5, buf, sizeof(buf))) == "48.5 Hz");
    CHECK(std::string(format_frequency_hz(60.0, buf, sizeof(buf))) == "60.0 Hz");
    CHECK(std::string(format_frequency_hz(0.0, buf, sizeof(buf))) == "0.0 Hz");
}

// =============================================================================
// Buffer safety
// =============================================================================

TEST_CASE("formatters handle small buffers safely", "[format_utils][safety]") {
    char tiny[4];

    SECTION("percent truncates safely") {
        format_percent(100, tiny, sizeof(tiny));
        CHECK(tiny[sizeof(tiny) - 1] == '\0');
    }

    SECTION("distance truncates safely") {
        format_distance_mm(123.456, 2, tiny, sizeof(tiny));
        CHECK(tiny[sizeof(tiny) - 1] == '\0');
    }
}

// =============================================================================
// Duration formatting (padded)
// =============================================================================

TEST_CASE("duration_padded shows seconds under 5 minutes", "[format_utils][duration]") {
    SECTION("zero seconds") {
        CHECK(duration_padded(0) == "0s");
    }

    SECTION("negative values") {
        CHECK(duration_padded(-10) == "0s");
    }

    SECTION("under 1 minute shows seconds only") {
        CHECK(duration_padded(5) == "5s");
        CHECK(duration_padded(30) == "30s");
        CHECK(duration_padded(59) == "59s");
    }

    SECTION("1 to 4 minutes shows minutes and seconds") {
        CHECK(duration_padded(60) == "1m 00s");
        CHECK(duration_padded(90) == "1m 30s");
        CHECK(duration_padded(150) == "2m 30s");
        CHECK(duration_padded(299) == "4m 59s");
    }

    SECTION("5 minutes and above shows minutes only") {
        CHECK(duration_padded(300) == "5m");
        CHECK(duration_padded(360) == "6m");
        CHECK(duration_padded(600) == "10m");
        CHECK(duration_padded(3540) == "59m");
    }

    SECTION("hours shows hours and padded minutes") {
        CHECK(duration_padded(3600) == "1h 00m");
        CHECK(duration_padded(3660) == "1h 01m");
        CHECK(duration_padded(7200) == "2h 00m");
        CHECK(duration_padded(7830) == "2h 10m");
    }
}

// =============================================================================
// Duration remaining formatting
// =============================================================================

TEST_CASE("duration_remaining shows seconds under 5 minutes", "[format_utils][duration]") {
    SECTION("zero seconds") {
        CHECK(duration_remaining(0) == "0 min left");
    }

    SECTION("negative values") {
        CHECK(duration_remaining(-10) == "0 min left");
    }

    SECTION("under 1 minute shows 0:SS") {
        CHECK(duration_remaining(5) == "0:05 left");
        CHECK(duration_remaining(30) == "0:30 left");
        CHECK(duration_remaining(59) == "0:59 left");
    }

    SECTION("1 to 4 minutes shows M:SS") {
        CHECK(duration_remaining(60) == "1:00 left");
        CHECK(duration_remaining(90) == "1:30 left");
        CHECK(duration_remaining(150) == "2:30 left");
        CHECK(duration_remaining(299) == "4:59 left");
    }

    SECTION("5 minutes and above shows minutes") {
        CHECK(duration_remaining(300) == "5 min left");
        CHECK(duration_remaining(360) == "6 min left");
        CHECK(duration_remaining(600) == "10 min left");
    }

    SECTION("hours shows H:MM") {
        CHECK(duration_remaining(3600) == "1:00 left");
        CHECK(duration_remaining(3660) == "1:01 left");
        CHECK(duration_remaining(7200) == "2:00 left");
    }
}

// =============================================================================
// Filament length formatting
// =============================================================================

TEST_CASE("format_filament_length formats correctly", "[format_utils][filament]") {
    SECTION("sub-meter values show as mm") {
        CHECK(format_filament_length(0) == "0mm");
        CHECK(format_filament_length(1) == "1mm");
        CHECK(format_filament_length(500) == "500mm");
        CHECK(format_filament_length(999) == "999mm");
    }

    SECTION("meter-range values show as meters with 1 decimal") {
        CHECK(format_filament_length(1000) == "1.0m");
        CHECK(format_filament_length(1500) == "1.5m");
        CHECK(format_filament_length(12500) == "12.5m");
        CHECK(format_filament_length(999999) == "1000.0m");
    }

    SECTION("kilometer-range values show as km with 2 decimals") {
        CHECK(format_filament_length(1000000) == "1.00km");
        CHECK(format_filament_length(1230000) == "1.23km");
    }
}

// =============================================================================
// Short date formatting (helix::ui)
// =============================================================================

TEST_CASE("format_short_date current year omits year", "[format_utils][date]") {
    // Build a timestamp for March 10 of the current year
    time_t now = time(nullptr);
    struct tm now_tm {};
    localtime_r(&now, &now_tm);

    struct tm target_tm {};
    target_tm.tm_year = now_tm.tm_year; // current year
    target_tm.tm_mon = 2;               // March (0-based)
    target_tm.tm_mday = 10;
    target_tm.tm_hour = 12;
    time_t ts = mktime(&target_tm);

    std::string result = helix::ui::format_short_date(ts);
    // Should contain "Mar" and "10" but NOT a year suffix
    CHECK(result.find("Mar") != std::string::npos);
    CHECK(result.find("10") != std::string::npos);
    CHECK(result.find("'") == std::string::npos); // no year tick mark
}

TEST_CASE("format_short_date previous year includes year", "[format_utils][date]") {
    // Build a timestamp for June 15 of last year
    time_t now = time(nullptr);
    struct tm now_tm {};
    localtime_r(&now, &now_tm);

    struct tm target_tm {};
    target_tm.tm_year = now_tm.tm_year - 1; // previous year
    target_tm.tm_mon = 5;                   // June (0-based)
    target_tm.tm_mday = 15;
    target_tm.tm_hour = 12;
    time_t ts = mktime(&target_tm);

    std::string result = helix::ui::format_short_date(ts);
    // Should contain "Jun", "15", and a year indicator
    CHECK(result.find("Jun") != std::string::npos);
    CHECK(result.find("15") != std::string::npos);
    CHECK(result.find("'") != std::string::npos); // year tick mark present
}

TEST_CASE("format_short_date handles zero timestamp", "[format_utils][date]") {
    // Unix epoch (1970-01-01) — always a different year, should show year
    std::string result = helix::ui::format_short_date(0);
    CHECK(!result.empty());
    CHECK(result != "Unknown");
}

// =============================================================================
// ETA clock time formatting
// =============================================================================

// Helper: build a local time_t from hour:minute on 2026-03-27
static std::time_t make_ref_time(int hour, int minute) {
    struct tm t = {};
    t.tm_year = 126; // 2026
    t.tm_mon = 2;    // March
    t.tm_mday = 27;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    return mktime(&t);
}

TEST_CASE("eta_clock_time produces clock time from remaining seconds", "[format_utils][eta]") {
    SECTION("30 minutes remaining from 14:00 gives 2:30 PM") {
        auto now = make_ref_time(14, 0);
        auto eta = eta_clock_time(30 * 60, now);
        CHECK(eta == "(~2:30 PM)");
    }

    SECTION("0 seconds remaining returns empty") {
        CHECK(eta_clock_time(0, make_ref_time(14, 0)).empty());
    }

    SECTION("negative remaining returns empty") {
        CHECK(eta_clock_time(-100, make_ref_time(14, 0)).empty());
    }

    SECTION("crosses midnight — 11 hours from 14:00 is 1:00 AM") {
        auto eta = eta_clock_time(11 * 3600, make_ref_time(14, 0));
        CHECK(eta == "(~1:00 AM)");
    }

    SECTION("very short remaining — 30 seconds from 23:59") {
        auto eta = eta_clock_time(30, make_ref_time(23, 59));
        CHECK(eta == "(~11:59 PM)");
    }

    SECTION("noon produces 12:00 PM") {
        auto eta = eta_clock_time(3600, make_ref_time(11, 0));
        CHECK(eta == "(~12:00 PM)");
    }

    SECTION("midnight produces 12:00 AM") {
        auto eta = eta_clock_time(3600, make_ref_time(23, 0));
        CHECK(eta == "(~12:00 AM)");
    }

    SECTION("48 hours remaining still produces valid output") {
        auto eta = eta_clock_time(48 * 3600, make_ref_time(10, 0));
        CHECK(!eta.empty());
        CHECK(eta.front() == '(');
        CHECK(eta.back() == ')');
    }
}

// =============================================================================
// Second-resolution time formatting (helix::ui)
// =============================================================================

TEST_CASE("format_time_with_seconds matches format_time plus seconds", "[format][time]") {
    struct tm t {};
    t.tm_hour = 14;
    t.tm_min = 32;
    t.tm_sec = 6;

    const std::string with_sec = helix::ui::format_time_with_seconds(&t);
    const std::string without = helix::ui::format_time(&t);

    // Same 12H/24H shape, one extra :SS field.
    CHECK(with_sec.find(":06") != std::string::npos);
    CHECK(with_sec.size() == without.size() + 3);
}

TEST_CASE("format_time_with_seconds zero-pads seconds", "[format][time]") {
    struct tm t {};
    t.tm_hour = 9;
    t.tm_min = 5;
    t.tm_sec = 3;
    CHECK(helix::ui::format_time_with_seconds(&t).find(":03") != std::string::npos);
}

TEST_CASE("format_time_with_seconds is null-safe", "[format][time]") {
    // Matches format_time()'s null behavior, not empty() — see format_utils.h.
    CHECK(helix::ui::format_time_with_seconds(nullptr) == helix::format::UNAVAILABLE);
}

TEST_CASE("eta_clock_time formats 24-hour clock when use_24h is true", "[format_utils][eta]") {
    SECTION("30 minutes remaining from 14:00 gives 14:30") {
        auto eta = eta_clock_time(30 * 60, make_ref_time(14, 0), true);
        CHECK(eta == "(~14:30)");
    }

    SECTION("crosses midnight — 11 hours from 14:00 is 1:00") {
        auto eta = eta_clock_time(11 * 3600, make_ref_time(14, 0), true);
        CHECK(eta == "(~1:00)");
    }

    SECTION("noon produces 12:00") {
        auto eta = eta_clock_time(3600, make_ref_time(11, 0), true);
        CHECK(eta == "(~12:00)");
    }

    SECTION("midnight produces 0:00") {
        auto eta = eta_clock_time(3600, make_ref_time(23, 0), true);
        CHECK(eta == "(~0:00)");
    }

    SECTION("morning time 9:15") {
        auto eta = eta_clock_time(15 * 60, make_ref_time(9, 0), true);
        CHECK(eta == "(~9:15)");
    }
}
