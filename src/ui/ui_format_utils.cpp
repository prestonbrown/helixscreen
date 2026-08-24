// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"

#include "display_settings_manager.h"
#include "format_utils.h"
#include "locale_formats.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <cstdio>
#include <ctime>

namespace helix::ui {

std::string format_layer_progress(int current, int total, bool accurate, int z_centimm) {
    // "~" marks a count guessed from the progress fraction. Slicer/Moonraker
    // fields and Z-height-derived layers are both accurate (Mainsail parity).
    const char* marker = accurate ? "" : "~";
    char buf[64];
    if (total > 0) {
        snprintf(buf, sizeof(buf), "%s %s%d / %d", lv_tr("Layer"), marker, current, total);
    } else {
        snprintf(buf, sizeof(buf), "%s %s%d", lv_tr("Layer"), marker, current);
    }

    std::string out(buf);
    if (z_centimm > 0) {
        char z_buf[24];
        snprintf(z_buf, sizeof(z_buf), " (%.1fmm)", z_centimm / 100.0);
        out += z_buf;
    }
    return out;
}

std::string format_print_time(int minutes) {
    return helix::format::duration_from_minutes(minutes);
}

std::string format_filament_weight(float grams) {
    char buf[32];
    if (grams < 1.0f) {
        snprintf(buf, sizeof(buf), "%.1f g", grams);
    } else if (grams < 10.0f) {
        snprintf(buf, sizeof(buf), "%.1f g", grams);
    } else {
        snprintf(buf, sizeof(buf), "%.0f g", grams);
    }
    return std::string(buf);
}

std::string format_layer_count(uint32_t layer_count) {
    if (layer_count == 0) {
        return helix::format::UNAVAILABLE;
    }
    char buf[32];
    if (layer_count == 1) {
        snprintf(buf, sizeof(buf), "%s", lv_tr("1 layer"));
    } else {
        snprintf(buf, sizeof(buf), lv_tr("%u layers"), layer_count);
    }
    return std::string(buf);
}

std::string format_print_height(double height_mm) {
    if (height_mm <= 0.0) {
        return helix::format::UNAVAILABLE;
    }
    char buf[32];
    if (height_mm < 1.0) {
        snprintf(buf, sizeof(buf), "%.2f mm", height_mm);
    } else if (height_mm < 10.0) {
        snprintf(buf, sizeof(buf), "%.1f mm", height_mm);
    } else {
        snprintf(buf, sizeof(buf), "%.0f mm", height_mm);
    }
    return std::string(buf);
}

std::string format_file_size(size_t bytes) {
    char buf[32];
    if (bytes < 1024) {
        snprintf(buf, sizeof(buf), "%zu B", bytes);
    } else if (bytes < 1024 * 1024) {
        double kb = bytes / 1024.0;
        snprintf(buf, sizeof(buf), "%.1f KB", kb);
    } else if (bytes < 1024 * 1024 * 1024) {
        double mb = bytes / (1024.0 * 1024.0);
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
    } else {
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        snprintf(buf, sizeof(buf), "%.2f GB", gb);
    }
    return std::string(buf);
}

// with_seconds selects the :SS-bearing variant of the same pattern. The
// 12H/24H decision itself lives here only, so format_time() and
// format_time_with_seconds() can't drift apart on which setting wins.
static const char* get_time_format_string(bool with_seconds = false) {
    TimeFormat format = DisplaySettingsManager::instance().get_time_format();
    // %I = hour (01-12, zero-padded) — POSIX standard, works on musl and glibc.
    // Callers strip the leading zero for cleaner display.
    // NOTE: %l (space-padded hour) is a GNU extension NOT supported by musl libc,
    // which produces empty strings on K1C (MIPS/musl) and other musl-based targets.
    if (format == TimeFormat::HOUR_12) {
        return with_seconds ? "%I:%M:%S %p" : "%I:%M %p";
    }
    return with_seconds ? "%H:%M:%S" : "%H:%M";
}

std::string format_time(const struct tm* tm_info) {
    if (!tm_info) {
        return helix::format::UNAVAILABLE;
    }

    char buf[16];
    strftime(buf, sizeof(buf), get_time_format_string(), tm_info);

    std::string result(buf);
    // Strip leading zero only for 12H format (%I produces "01"-"12")
    // 24H "08:30" should keep the zero
    if (DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12 &&
        !result.empty() && result[0] == '0') {
        result.erase(0, 1);
    }
    return result;
}

std::string format_time_with_seconds(const struct tm* tm_info) {
    if (!tm_info) {
        return helix::format::UNAVAILABLE;
    }

    char buf[16];
    strftime(buf, sizeof(buf), get_time_format_string(/*with_seconds=*/true), tm_info);

    std::string result(buf);
    // Same leading-zero strip as format_time() — only 12H's %I needs it.
    if (DisplaySettingsManager::instance().get_time_format() == TimeFormat::HOUR_12 &&
        !result.empty() && result[0] == '0') {
        result.erase(0, 1);
    }
    return result;
}

std::string format_modified_date(time_t timestamp) {
    struct tm tm_buf;
    struct tm* timeinfo = localtime_r(&timestamp, &tm_buf);
    if (!timeinfo) {
        return "Unknown";
    }
    return format_localized_modified_date(timeinfo);
}

std::string format_short_date(time_t timestamp) {
    struct tm tm_buf;
    struct tm* timeinfo = localtime_r(&timestamp, &tm_buf);
    if (!timeinfo) {
        return "Unknown";
    }
    return format_localized_short_date(timeinfo);
}

std::string format_relative_time(uint64_t elapsed_ms) {
    if (elapsed_ms < 60000) { // < 1 min
        return lv_tr("Just now");
    }
    if (elapsed_ms < 3600000) { // < 1 hour
        uint64_t mins = elapsed_ms / 60000;
        if (mins == 1) {
            return lv_tr("1 min ago");
        }
        char buf[32];
        snprintf(buf, sizeof(buf), lv_tr("%lu min ago"), static_cast<unsigned long>(mins));
        return buf;
    }
    if (elapsed_ms < 86400000) { // < 1 day
        uint64_t hours = elapsed_ms / 3600000;
        if (hours == 1) {
            return lv_tr("1 hour ago");
        }
        char buf[32];
        snprintf(buf, sizeof(buf), lv_tr("%lu hours ago"), static_cast<unsigned long>(hours));
        return buf;
    }
    uint64_t days = elapsed_ms / 86400000;
    if (days == 1) {
        return lv_tr("1 day ago");
    }
    char buf[32];
    snprintf(buf, sizeof(buf), lv_tr("%lu days ago"), static_cast<unsigned long>(days));
    return buf;
}

} // namespace helix::ui
