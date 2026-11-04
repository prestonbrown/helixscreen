// SPDX-License-Identifier: GPL-3.0-or-later
#include "chamber_wait_line.h"

#include "core/text_io.h"

namespace helix {

bool wait_names_chamber_heater(const WaitLine& line, const std::string& chamber_heater) {
    if (line.kind != WaitKind::TemperatureWait || line.sensor.empty() || chamber_heater.empty()) {
        return false;
    }
    return line.sensor == text_io::to_lower(chamber_heater);
}

double chamber_wait_target_c(const WaitLine& line) {
    switch (line.kind) {
    case WaitKind::M191:
        return line.target.value_or(0.0);
    case WaitKind::TemperatureWait:
        return line.minimum.value_or(0.0);
    default:
        return 0.0;
    }
}

} // namespace helix
