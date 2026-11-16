// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_dryer_presentation.h"

#include "ui_temperature_utils.h"

#include "core/text_io.h"
#include "lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace helix::ui {

std::string drying_status_text(const DryerInfo& dryer) {
    if (!dryer.active) {
        return {};
    }
    const int remaining = std::max(dryer.remaining_min, 0);
    char buf[96];
    if (dryer.phase == DryingPhase::Heating && dryer.target_temp_c > 0.0f) {
        char temp[16];
        temperature::format_temperature(static_cast<int>(std::lround(dryer.target_temp_c)), temp,
                                        sizeof(temp));
        // One key with the temperature inside it, so a translation can move it.
        snprintf(buf, sizeof(buf), lv_tr("Heating to %s: %d:%02d left"), temp, remaining / 60,
                 remaining % 60);
    } else {
        snprintf(buf, sizeof(buf), "%s: %d:%02d %s", lv_tr("Drying"), remaining / 60,
                 remaining % 60, lv_tr("left"));
    }
    return buf;
}

std::string format_duration_hours(int minutes) {
    if (minutes % 60 == 0) {
        return std::to_string(minutes / 60);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f", minutes / 60.0);
    std::string text = buf;
    while (text.back() == '0') {
        text.pop_back();
    }
    return text;
}

std::optional<int> parse_duration_hours(const char* text) {
    if (!text || !text[0]) {
        return std::nullopt;
    }
    const std::optional<double> hours = text_io::parse_double(text);
    if (!hours || !(*hours > 0.0)) {
        return std::nullopt;
    }
    // Far past any dryer's ceiling, which the caller clamps to; bounded so the
    // conversion cannot overflow.
    constexpr double kMaxHours = 1.0e5;
    const long minutes = std::lround(std::min(*hours, kMaxHours) * 60.0);
    if (minutes <= 0) {
        return std::nullopt;
    }
    return static_cast<int>(minutes);
}

} // namespace helix::ui
