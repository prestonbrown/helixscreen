// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Calibration results the printer's Klipper config already holds. A printer
// tuned before HelixScreen kept any history still has its Z offset, heater
// tuning, saved meshes and shaper in its config; the Calibration screen shows
// those, without a date, for steps that have no history entry.

#include "json_fwd.h"

#include <map>
#include <string>

namespace helix::calibration {

/// Step id to a short summary of the value found ("-1.250 mm", "PID").
using ConfigResults = std::map<std::string, std::string>;

/// The results in a configfile.settings object. Section names arrive
/// lowercased. A step whose config value is only Klipper's default is absent.
[[nodiscard]] ConfigResults results_in_config(const nlohmann::json& settings);

} // namespace helix::calibration
