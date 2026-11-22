// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the calibration intro pages share: when a step last ran, and which
// choice chip (cal_intro_chip) a tap came from.

#include "lvgl.h"

#include <optional>
#include <string>

namespace helix::calibration {

/// The local month and day @p step_id last ran ("Nov 3"), from the calibration
/// history; nullopt when it never ran. A date the history cannot parse is
/// returned as stored.
[[nodiscard]] std::optional<std::string> last_run_month_day(const std::string& step_id);

/// The `index` a cal_intro_chip passed as its tap's user data string; -1 when
/// there is none or it is not a number.
[[nodiscard]] int intro_chip_index(lv_event_t* e);

} // namespace helix::calibration
