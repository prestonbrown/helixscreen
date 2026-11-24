// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_time.h"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <fmt/format.h>

namespace helix::calibration {

std::string time_duration_text(int minutes) {
    const int m = std::max(1, minutes);
    if (m < 60) {
        return fmt::format(fmt::runtime(lv_tr("{} min")), m);
    }
    if (m % 60 == 0) {
        return fmt::format(fmt::runtime(lv_tr("{} h")), m / 60);
    }
    return fmt::format(fmt::runtime(lv_tr("{} h {} min")), m / 60, m % 60);
}

std::string time_away_line(AwayKind kind, int minutes) {
    const std::string duration = time_duration_text(minutes);
    return kind == AwayKind::Prints
               ? fmt::format(fmt::runtime(lv_tr("Prints on its own, about {}")), duration)
               : fmt::format(fmt::runtime(lv_tr("Runs on its own, about {}")), duration);
}

std::string time_you_line(int minutes, const std::string& task) {
    const std::string duration = time_duration_text(minutes);
    return task.empty()
               ? fmt::format(fmt::runtime(lv_tr("Needs you, about {}")), duration)
               : fmt::format(fmt::runtime(lv_tr("Needs you, about {}: {}")), duration, task);
}

} // namespace helix::calibration
