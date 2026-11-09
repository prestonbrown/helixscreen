// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fan tachometers reported by a status object other than the fan's own.
//
// Klipper puts a fan's rpm on the fan object itself, and PrinterFanState reads that
// directly. Some firmwares instead read their tachometers in a separate module whose
// fields name a sensor slot, not a fan, and the slot's tach pin is never the fan's PWM
// pin. Which fan a slot measures is firmware knowledge, and this module is the ONLY
// place that holds it. PrinterFanState asks readings() and never names a firmware.
//
// A slot with no evidence for which fan it measures stays unmapped: no RPM beats an
// RPM shown on the wrong fan.

#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

namespace helix::fan_tach {

/// One tachometer field and the fan object it measures.
struct TachSource {
    std::string object; ///< status object carrying the reading
    std::string field;  ///< numeric RPM field inside that object
    std::string fan;    ///< fan object name the reading belongs to

    bool operator==(const TachSource& o) const {
        return object == o.object && field == o.field && fan == o.fan;
    }
};

using TachMap = std::vector<TachSource>;

/// Resolve every mappable tachometer from Klipper's configfile.settings. Empty when no
/// known tachometer module is configured or none of its slots can be tied to a fan.
TachMap map_from_config(const nlohmann::json& settings);

/// RPM readings in one status update, as (fan object name, rpm). Fields absent from the
/// update, or not numeric, yield nothing so the last reading stays.
std::vector<std::pair<std::string, int>> readings(const TachMap& map, const nlohmann::json& status);

} // namespace helix::fan_tach
