// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "format_utils.h"

#include "core/text_io.h"
#include "translation_loader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace helix::format {

// =============================================================================
// Percentage Formatting
// =============================================================================

char* format_percent(int percent, char* buf, size_t size) {
    std::snprintf(buf, size, "%d%%", percent);
    return buf;
}

const char* format_fan_speed(int speed_pct, char* buf, size_t size) {
    if (speed_pct == 0) {
        return "Off";
    }
    format_percent(speed_pct, buf, size);
    return buf;
}

char* format_percent_or_unavailable(int percent, bool available, char* buf, size_t size) {
    if (available) {
        return format_percent(percent, buf, size);
    }
    std::snprintf(buf, size, "%s", UNAVAILABLE);
    return buf;
}

char* format_percent_float(double percent, int decimals, char* buf, size_t size) {
    std::snprintf(buf, size, "%.*f%%", decimals, percent);
    return buf;
}

char* format_humidity(int humidity_x10, char* buf, size_t size) {
    // Divide by 10 to get whole percent (truncate, don't round)
    return format_percent(humidity_x10 / 10, buf, size);
}

// =============================================================================
// Distance/Length Formatting
// =============================================================================

char* format_distance_mm(double mm, int precision, char* buf, size_t size) {
    std::snprintf(buf, size, "%.*f mm", precision, mm);
    return buf;
}

char* format_diameter_mm(float mm, char* buf, size_t size) {
    std::snprintf(buf, size, "%.2f mm", static_cast<double>(mm));
    return buf;
}

// =============================================================================
// Speed Formatting
// =============================================================================

char* format_speed_mm_s(double speed, char* buf, size_t size) {
    std::snprintf(buf, size, "%.0f mm/s", speed);
    return buf;
}

char* format_speed_mm_s_float(double speed, int decimals, char* buf, size_t size) {
    if (decimals < 0)
        decimals = 0;
    std::snprintf(buf, size, "%.*f mm/s", decimals, speed);
    return buf;
}

char* format_speed_mm_min(double speed, char* buf, size_t size) {
    std::snprintf(buf, size, "%.0f mm/min", speed);
    return buf;
}

// =============================================================================
// Acceleration Formatting
// =============================================================================

char* format_accel_mm_s2(double accel, char* buf, size_t size) {
    std::snprintf(buf, size, "%.0f mm/s²", accel);
    return buf;
}

// =============================================================================
// Frequency Formatting
// =============================================================================

char* format_frequency_hz(double hz, char* buf, size_t size) {
    std::snprintf(buf, size, "%.1f Hz", hz);
    return buf;
}

// =============================================================================
// Duration Formatting
// =============================================================================

namespace {

Translator s_translator = nullptr;

/// A unit format in the current language; the format itself until a translator is set.
const char* tr(const char* format) {
    return s_translator ? s_translator(format) : format;
}

} // namespace

void set_translator(Translator translator) {
    s_translator = translator;
}

std::string duration(int total_seconds) {
    char buf[64];
    duration_to_buffer(buf, sizeof(buf), total_seconds);
    return std::string(buf);
}

std::string duration_remaining(int total_seconds) {
    const int clamped = total_seconds > 0 ? total_seconds : 0;
    const int hours = clamped / 3600;
    const int minutes = (clamped % 3600) / 60;
    const int seconds = clamped % 60;

    char buf[64];
    if (clamped == 0) {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%d min left")), 0);
    } else if (hours > 0) {
        // H:MM for longer durations
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%d:%02d left")), hours, minutes);
    } else if (clamped < 300) {
        // Under 5 minutes: M:SS for precision
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%d:%02d left")), minutes, seconds);
    } else {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%d min left")), minutes > 0 ? minutes : 1);
    }
    return std::string(buf);
}

std::string duration_from_minutes(int total_minutes) {
    const int clamped = total_minutes > 0 ? total_minutes : 0;
    const int hours = clamped / 60;
    const int minutes = clamped % 60;

    char buf[64];
    if (hours == 0) {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%d min")), minutes);
    } else if (minutes == 0) {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%dh")), hours);
    } else {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%dh %dm")), hours, minutes);
    }
    return std::string(buf);
}

size_t duration_to_buffer(char* buf, size_t buf_size, int total_seconds) {
    if (buf == nullptr || buf_size == 0) {
        return 0;
    }

    const int clamped = total_seconds > 0 ? total_seconds : 0;
    const int hours = clamped / 3600;
    const int minutes = (clamped % 3600) / 60;
    const int seconds = clamped % 60;

    int written = 0;
    if (hours == 0 && minutes == 0) {
        written = std::snprintf(buf, buf_size, tr(TR_NOOP("%ds")), seconds);
    } else if (hours == 0) {
        // Under 1 hour: minutes only, no seconds
        written = std::snprintf(buf, buf_size, tr(TR_NOOP("%dm")), minutes);
    } else if (minutes == 0) {
        written = std::snprintf(buf, buf_size, tr(TR_NOOP("%dh")), hours);
    } else {
        written = std::snprintf(buf, buf_size, tr(TR_NOOP("%dh %dm")), hours, minutes);
    }
    return written > 0 ? static_cast<size_t>(written) : 0;
}

std::string duration_padded(int total_seconds) {
    const int clamped = total_seconds > 0 ? total_seconds : 0;
    const int hours = clamped / 3600;
    const int minutes = (clamped % 3600) / 60;
    const int seconds = clamped % 60;

    char buf[64];
    if (hours == 0 && minutes > 0 && clamped < 300) {
        // Under 5 minutes: minutes and seconds for precision
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%dm %02ds")), minutes, seconds);
    } else if (hours == 0 && minutes == 0) {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%ds")), seconds);
    } else if (hours == 0) {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%dm")), minutes);
    } else {
        std::snprintf(buf, sizeof(buf), tr(TR_NOOP("%dh %02dm")), hours, minutes);
    }
    return std::string(buf);
}

std::string format_filament_length(double mm) {
    char buf[32];
    if (mm < 1000) {
        std::snprintf(buf, sizeof(buf), "%.0fmm", mm);
    } else if (mm < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1fm", mm / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fkm", mm / 1000000.0);
    }
    return std::string(buf);
}

// =============================================================================
// Clock Time Formatting
// =============================================================================

bool parse_http_date(const char* value, std::time_t& epoch_s) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (value == nullptr) {
        return false;
    }
    namespace tio = helix::text_io;
    std::string_view rest = value;
    // "<Day>, DD Mon YYYY HH:MM:SS GMT", nothing after the zone.
    size_t letters = 0;
    while (letters < rest.size() && ((rest[letters] >= 'A' && rest[letters] <= 'Z') ||
                                     (rest[letters] >= 'a' && rest[letters] <= 'z'))) {
        ++letters;
    }
    if (letters == 0) {
        return false;
    }
    rest.remove_prefix(letters);
    if (!tio::consume_literal(rest, ", ")) {
        return false;
    }
    const auto day_v = tio::scan_number<int>(rest, 10, 2);
    const auto mon_v = tio::scan_word(rest, 3);
    const auto year_v = tio::scan_number<int>(rest, 10, 4);
    const auto hour_v = tio::scan_number<int>(rest, 10, 2);
    if (!day_v || !mon_v || !year_v || !hour_v || !tio::consume_literal(rest, ":")) {
        return false;
    }
    const auto minute_v = tio::scan_number<int>(rest, 10, 2);
    if (!minute_v || !tio::consume_literal(rest, ":")) {
        return false;
    }
    const auto second_v = tio::scan_number<int>(rest, 10, 2);
    const auto zone_v = tio::scan_word(rest, 3);
    if (!second_v || !zone_v || !rest.empty() || *zone_v != "GMT") {
        return false;
    }
    const int day = *day_v, year = *year_v, hour = *hour_v, minute = *minute_v, second = *second_v;
    const std::string_view mon = *mon_v;
    int month = 0;
    while (month < 12 && mon != kMonths[month]) {
        ++month;
    }
    if (month == 12 || year < 1970 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return false;
    }
    static const int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int month_days = kDaysInMonth[month] + ((month == 1 && leap) ? 1 : 0);
    if (day < 1 || day > month_days) {
        return false;
    }
    // Days since the epoch for a proleptic Gregorian date (Hinnant's days_from_civil):
    // newlib has no timegm(), and mktime() would apply the local zone.
    const int y = year - (month < 2 ? 1 : 0); // March-based year
    const int era = y / 400;
    const int yoe = y - era * 400;
    const int mp = (month + 10) % 12;
    const int doy = (153 * mp + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
    epoch_s = static_cast<std::time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
    return true;
}

std::string eta_clock_time(int remaining_seconds, std::time_t now, bool use_24h) {
    if (remaining_seconds <= 0) {
        return "";
    }

    if (now == 0) {
        now = std::time(nullptr);
    }
    std::time_t finish = now + static_cast<std::time_t>(remaining_seconds);

    struct std::tm local_tm;
    if (localtime_r(&finish, &local_tm) == nullptr) {
        return "";
    }

    int hour = local_tm.tm_hour;
    int minute = local_tm.tm_min;
    char buf[32];

    if (use_24h) {
        std::snprintf(buf, sizeof(buf), "(~%d:%02d)", hour, minute);
    } else {
        const char* ampm = (hour >= 12) ? "PM" : "AM";
        int hour12 = hour % 12;
        if (hour12 == 0) {
            hour12 = 12;
        }
        std::snprintf(buf, sizeof(buf), "(~%d:%02d %s)", hour12, minute, ampm);
    }
    return std::string(buf);
}

int round_eta_seconds(int seconds) {
    if (seconds <= 0)
        return 0;
    if (seconds > 120) {
        return ((seconds + 15) / 30) * 30;
    }
    return ((seconds + 5) / 10) * 10;
}

} // namespace helix::format
