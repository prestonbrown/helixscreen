// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_zone_presentation.h"

#include "ams_environment_zone.h"
#include "display_numbering.h"

#include <string>
#include <vector>

namespace helix::ui {

ZoneVerdict zone_verdict(const helix::printer::EnvironmentZone& zone) {
    if (!zone.env.has_humidity) {
        return ZoneVerdict::Unknown;
    }
    if (zone.env.humidity_pct <= kZoneHumidityOkMax) {
        return ZoneVerdict::Ok;
    }
    if (zone.env.humidity_pct <= kZoneHumidityMarginalMax) {
        return ZoneVerdict::Marginal;
    }
    return ZoneVerdict::TooHumid;
}

ZoneStatus zone_status(const helix::printer::EnvironmentZone& zone) {
    if (zone.dryer.active) {
        return {ZoneStatusKind::Drying, ZoneVerdict::Ok};
    }
    if (!zone.dryer.supported) {
        return {ZoneStatusKind::Passive, zone_verdict(zone)};
    }
    return {ZoneStatusKind::Verdict, zone_verdict(zone)};
}

std::string zone_display_label(const helix::printer::EnvironmentZone& zone,
                               const std::string& unit_word, LaneNoun slot_noun,
                               const std::string& type_name) {
    if (!zone.label.empty()) {
        return zone.label;
    }
    if (zone.gates.size() == 1) {
        return lane_label(slot_noun, zone.gates.front());
    }
    const int unit_number = lane_number(zone.unit_index);
    if (unit_number < 0) {
        // The representative gate didn't resolve to a known unit (Happy Hare can
        // report this). Neither sentinel reads as a unit number a user would trust,
        // so the label falls back to the system type alone.
        return type_name;
    }
    const std::string prefix = type_name.empty() ? std::string{} : type_name + " ";
    return prefix + unit_word + " " + std::to_string(unit_number);
}

bool zones_span_units(const std::vector<helix::printer::EnvironmentZone>& zones) {
    if (zones.size() < 2) {
        return false;
    }
    const int first = zones.front().unit_index;
    for (const auto& z : zones) {
        if (z.unit_index != first) {
            return true;
        }
    }
    return false;
}

std::string zone_slot_text(const helix::printer::EnvironmentZone& zone, LaneNoun noun) {
    if (zone.gates.empty()) {
        return {};
    }
    if (zone.gates.front() == zone.gates.back()) {
        return lane_label(noun, zone.gates.front());
    }
    return lane_range_label(noun, zone.gates.front(), zone.gates.back());
}

} // namespace helix::ui
