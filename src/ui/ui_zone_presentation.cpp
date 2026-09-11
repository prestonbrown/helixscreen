// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_zone_presentation.h"

#include "ams_environment_zone.h"

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
                               const std::string& unit_word, const std::string& slot_word,
                               const std::string& type_name) {
    if (!zone.label.empty()) {
        return zone.label;
    }
    if (zone.gates.size() == 1) {
        return slot_word + " " + std::to_string(zone.gates.front() + 1);
    }
    const std::string prefix = type_name.empty() ? std::string{} : type_name + " ";
    return prefix + unit_word + " " + std::to_string(zone.unit_index + 1);
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

std::string zone_slot_text(const helix::printer::EnvironmentZone& zone,
                           const std::string& plural_word, const std::string& singular_word) {
    if (zone.gates.empty()) {
        return {};
    }
    const int first = zone.gates.front() + 1;
    const int last = zone.gates.back() + 1;
    if (first == last) {
        return singular_word + " " + std::to_string(first);
    }
    return plural_word + " " + std::to_string(first) + "-" + std::to_string(last);
}

} // namespace helix::ui
