// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"

#include "display_settings_manager.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <cstdio>
#include <ctime>

namespace helix::ui {

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

const char* get_time_format_string() {
    TimeFormat format = DisplaySettingsManager::instance().get_time_format();
    // %l = hour (1-12, space-padded), %I = hour (01-12, zero-padded)
    // Using %l for cleaner display without leading zero
    return (format == TimeFormat::HOUR_12) ? "%l:%M %p" : "%H:%M";
}

std::string format_time(const struct tm* tm_info) {
    if (!tm_info) {
        return helix::format::UNAVAILABLE;
    }

    char buf[16];
    strftime(buf, sizeof(buf), get_time_format_string(), tm_info);

    // Trim leading space from %l if present (space-padded hour)
    std::string result(buf);
    if (!result.empty() && result[0] == ' ') {
        result.erase(0, 1);
    }
    return result;
}

std::string format_modified_date(time_t timestamp) {
    char buf[64];
    struct tm* timeinfo = localtime(&timestamp);
    if (timeinfo) {
        // Format: "Jan 15 2:30 PM" (12H) or "Jan 15 14:30" (24H)
        TimeFormat format = DisplaySettingsManager::instance().get_time_format();
        if (format == TimeFormat::HOUR_12) {
            strftime(buf, sizeof(buf), "%b %d %l:%M %p", timeinfo);
            // Trim double spaces from %l (space-padded hour)
            std::string result(buf);
            size_t pos;
            while ((pos = result.find("  ")) != std::string::npos) {
                result.erase(pos, 1);
            }
            return result;
        } else {
            strftime(buf, sizeof(buf), "%b %d %H:%M", timeinfo);
        }
    } else {
        snprintf(buf, sizeof(buf), "Unknown");
    }
    return std::string(buf);
}

} // namespace helix::ui
