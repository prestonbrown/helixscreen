// SPDX-License-Identifier: GPL-3.0-or-later

#include "happy_hare_fault_position.h"

#include <string_view>

namespace helix::happy_hare {

namespace {

bool message_contains(std::string_view message, std::string_view needle) {
    return message.find(needle) != std::string_view::npos;
}

} // namespace

LaneSensor happy_hare_fault_lane_sensor(std::string_view message) {
    // Happy Hare 4.2+: "Runout detected on %s by %s"; 4.0 and 4.1: "Runout detected on %s". Both
    // name the unprefixed sensor mmu_entry_<gate>.
    if (message_contains(message, "Runout detected on ") && message_contains(message, "mmu_entry"))
        return LaneSensor::PREP;
    // Happy Hare 3: "Runout detected on %s" with the sensor name mmu_pre_gate_<gate>.
    if (message_contains(message, "Runout detected on mmu_pre_gate"))
        return LaneSensor::PREP;
    return LaneSensor::NONE;
}

} // namespace helix::happy_hare
