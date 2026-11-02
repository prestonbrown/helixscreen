// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_activity.h"

#include "i18n_plural.h"
#include "lvgl.h"
#include "text_io.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix {

namespace {

constexpr double kSecondsPerHour = 3600.0;

/// One decimal below 10, whole number from 10 up; ".0" is dropped. A value
/// that rounds up to 10.0 takes the whole-number form.
std::string format_compact_number(double value, double min_positive) {
    if (value < 10.0) {
        double tenths = std::round(value * 10.0) / 10.0;
        if (tenths < 10.0) {
            tenths = std::max(tenths, min_positive);
            if (tenths == std::floor(tenths)) {
                return fmt::format("{}", static_cast<int>(tenths));
            }
            return fmt::format("{:.1f}", tenths);
        }
    }
    return fmt::format("{}", static_cast<long long>(std::llround(value)));
}

} // namespace

ActivityDayStarts local_day_starts(std::time_t now) {
    struct tm base {};
    localtime_r(&now, &base);
    base.tm_hour = 0;
    base.tm_min = 0;
    base.tm_sec = 0;

    ActivityDayStarts starts{};
    for (int i = 0; i <= kActivityDays; ++i) {
        struct tm day = base;
        day.tm_mday += i - (kActivityDays - 1);
        day.tm_isdst = -1;
        starts[static_cast<size_t>(i)] = static_cast<double>(mktime(&day));
    }
    return starts;
}

WeekActivity summarize_week(const std::vector<PrintHistoryJob>& jobs,
                            const ActivityDayStarts& starts) {
    WeekActivity week;
    for (const auto& job : jobs) {
        const double t = job_timestamp(job);
        if (t <= 0) {
            continue;
        }
        for (size_t i = 0; i < kActivityDays; ++i) {
            if (starts[i] <= t && t < starts[i + 1]) {
                week.hours_by_day[i] += std::max(0.0, job.print_duration) / kSecondsPerHour;
                ++week.week_prints;
                break;
            }
        }
    }
    for (double h : week.hours_by_day) {
        week.week_hours += h;
    }
    return week;
}

std::array<int, kActivityDays> scale_day_bars(const std::array<double, kActivityDays>& hours) {
    std::array<int, kActivityDays> bars{};
    const double max_hours = *std::max_element(hours.begin(), hours.end());
    if (!(max_hours > 0.0)) {
        return bars;
    }
    for (size_t i = 0; i < hours.size(); ++i) {
        if (!(hours[i] > 0.0)) {
            continue;
        }
        const int pct = static_cast<int>(std::lround(100.0 * hours[i] / max_hours));
        bars[i] = std::max(pct, kMinVisibleBarPct);
    }
    return bars;
}

std::string format_print_hours(double hours) {
    if (!std::isfinite(hours) || hours <= 0.0) {
        return fmt::format(lv_tr("{} h"), 0);
    }
    if (hours < 1.0) {
        const long minutes = std::max(1L, std::lround(hours * 60.0));
        if (minutes >= 60) {
            return fmt::format(lv_tr("{} h"), 1);
        }
        return fmt::format(lv_tr("{} min"), minutes);
    }
    return fmt::format(lv_tr("{} h"), format_compact_number(hours, 0.0));
}

std::string format_print_count(uint64_t prints) {
    return fmt::format(fmt::runtime(ui::tr_plural("1 print", "{} prints", prints)), prints);
}

std::string format_filament_meters(double mm) {
    if (!std::isfinite(mm) || mm <= 0.0) {
        return {};
    }
    return fmt::format(lv_tr("{} m"), format_compact_number(mm / 1000.0, 0.1));
}

std::string format_lifetime_line(const PrintHistoryTotals& totals, bool with_filament) {
    std::string line = format_print_count(totals.total_jobs) + " · " +
                       format_print_hours(static_cast<double>(totals.total_time) / kSecondsPerHour);
    if (with_filament) {
        const std::string length = format_filament_meters(totals.total_filament_used);
        if (!length.empty()) {
            line += " · " + length;
        }
    }
    return line;
}

std::string weekday_initial(std::string_view abbreviation) {
    return text_io::to_upper(text_io::utf8_first_codepoint(abbreviation));
}

} // namespace helix
