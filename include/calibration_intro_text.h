// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Text the calibration intro pages share: when a step last ran.

#include <optional>
#include <string>

namespace helix::calibration {

/// The local month and day @p step_id last ran ("Nov 3"), from the calibration
/// history; nullopt when it never ran. A date the history cannot parse is
/// returned as stored.
[[nodiscard]] std::optional<std::string> last_run_month_day(const std::string& step_id);

} // namespace helix::calibration
