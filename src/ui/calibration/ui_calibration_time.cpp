// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_calibration_time.h"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <fmt/format.h>

namespace helix::calibration {

std::string time_away_line(AwayKind kind, int minutes) {
    const int m = std::max(1, minutes);
    return kind == AwayKind::Prints
               ? fmt::format(fmt::runtime(lv_tr("Prints on its own, about {} min")), m)
               : fmt::format(fmt::runtime(lv_tr("Runs on its own, about {} min")), m);
}

std::string time_you_line(int minutes, const std::string& task) {
    const int m = std::max(1, minutes);
    return task.empty() ? fmt::format(fmt::runtime(lv_tr("Needs you, about {} min")), m)
                        : fmt::format(fmt::runtime(lv_tr("Needs you, about {} min: {}")), m, task);
}

} // namespace helix::calibration
