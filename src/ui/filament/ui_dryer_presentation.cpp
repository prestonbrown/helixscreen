// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_dryer_presentation.h"

#include "ui_temperature_utils.h"

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

} // namespace helix::ui
