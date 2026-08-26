// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "format_utils.h"

#include <cstdio>
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

std::string duration(int total_seconds) {
    // Handle negative or zero
    if (total_seconds <= 0) {
        return "0s";
    }

    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    char buf[32];

    if (hours == 0 && minutes == 0) {
        // Under 1 minute: show seconds
        std::snprintf(buf, sizeof(buf), "%ds", seconds);
    } else if (hours == 0) {
        // Under 1 hour: show minutes only
        std::snprintf(buf, sizeof(buf), "%dm", minutes);
    } else if (minutes == 0) {
        // Exact hours
        std::snprintf(buf, sizeof(buf), "%dh", hours);
    } else {
        // Hours and minutes
        std::snprintf(buf, sizeof(buf), "%dh %dm", hours, minutes);
    }

    return std::string(buf);
}

std::string duration_remaining(int total_seconds) {
    // Handle negative or zero
    if (total_seconds <= 0) {
        return "0 min left";
    }

    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    char buf[32];

    if (hours > 0) {
        // Show as H:MM format for longer durations
        std::snprintf(buf, sizeof(buf), "%d:%02d left", hours, minutes);
    } else if (total_seconds < 300) {
        // Under 5 minutes: show minutes:seconds for precision
        if (minutes > 0) {
            std::snprintf(buf, sizeof(buf), "%d:%02d left", minutes, seconds);
        } else {
            std::snprintf(buf, sizeof(buf), "0:%02d left", seconds);
        }
    } else {
        // Show as "X min left" for shorter durations
        std::snprintf(buf, sizeof(buf), "%d min left", minutes > 0 ? minutes : 1);
    }

    return std::string(buf);
}

std::string duration_from_minutes(int total_minutes) {
    // Handle negative or zero
    if (total_minutes <= 0) {
        return "0 min";
    }

    int hours = total_minutes / 60;
    int minutes = total_minutes % 60;

    char buf[32];

    if (hours == 0) {
        // Under 1 hour: show minutes
        std::snprintf(buf, sizeof(buf), "%d min", minutes);
    } else if (minutes == 0) {
        // Exact hours
        std::snprintf(buf, sizeof(buf), "%dh", hours);
    } else {
        // Hours and minutes
        std::snprintf(buf, sizeof(buf), "%dh %dm", hours, minutes);
    }

    return std::string(buf);
}

size_t duration_to_buffer(char* buf, size_t buf_size, int total_seconds) {
    if (buf == nullptr || buf_size == 0) {
        return 0;
    }

    // Handle negative or zero
    if (total_seconds <= 0) {
        return static_cast<size_t>(std::snprintf(buf, buf_size, "0s"));
    }

    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    int written = 0;

    if (hours == 0 && minutes == 0) {
        // Under 1 minute: show seconds
        written = std::snprintf(buf, buf_size, "%ds", seconds);
    } else if (hours == 0) {
        // Under 1 hour: show minutes only
        written = std::snprintf(buf, buf_size, "%dm", minutes);
    } else if (minutes == 0) {
        // Exact hours
        written = std::snprintf(buf, buf_size, "%dh", hours);
    } else {
        // Hours and minutes
        written = std::snprintf(buf, buf_size, "%dh %dm", hours, minutes);
    }

    return written > 0 ? static_cast<size_t>(written) : 0;
}

std::string duration_padded(int total_seconds) {
    // Handle negative or zero
    if (total_seconds <= 0) {
        return "0s";
    }

    int hours = total_seconds / 3600;
    int minutes = (total_seconds % 3600) / 60;
    int seconds = total_seconds % 60;

    char buf[32];

    if (hours == 0 && total_seconds < 300) {
        // Under 5 minutes: show minutes and seconds for precision
        if (minutes > 0) {
            std::snprintf(buf, sizeof(buf), "%dm %02ds", minutes, seconds);
        } else {
            std::snprintf(buf, sizeof(buf), "%ds", seconds);
        }
    } else if (hours == 0) {
        // Under 1 hour: show minutes only
        std::snprintf(buf, sizeof(buf), "%dm", minutes);
    } else {
        // Hours and zero-padded minutes
        std::snprintf(buf, sizeof(buf), "%dh %02dm", hours, minutes);
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
