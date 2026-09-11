// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_environment_zone.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace helix::printer {

namespace {

/// A backend sizes its lists to the lanes it has configured, which need not reach
/// every gate the system reports.
const std::string& at_or_empty(const std::vector<std::string>& list, int gate) {
    static const std::string empty;
    const auto index = static_cast<size_t>(gate);
    return index < list.size() ? list[index] : empty;
}

ZoneDryingState state_from_firmware(const std::string& s) {
    if (s == "active") {
        return ZoneDryingState::Active;
    }
    if (s == "queued") {
        return ZoneDryingState::Queued;
    }
    if (s == "complete") {
        return ZoneDryingState::Complete;
    }
    // Happy Hare emits 'canceled'; its own header documents 'cancelled'. Taking both
    // keeps an upstream correction from silently reading as Idle.
    if (s == "canceled" || s == "cancelled") {
        return ZoneDryingState::Cancelled;
    }
    return ZoneDryingState::Idle;
}

/// Live states outrank finished ones, and Cancelled outranks Complete so a zone that was
/// stopped partway does not report as cleanly finished. Deliberately not the enum's own
/// ordering, which stays stable for storage.
int precedence(ZoneDryingState s) {
    switch (s) {
    case ZoneDryingState::Active:
        return 4;
    case ZoneDryingState::Queued:
        return 3;
    case ZoneDryingState::Cancelled:
        return 2;
    case ZoneDryingState::Complete:
        return 1;
    case ZoneDryingState::Idle:
        break;
    }
    return 0;
}

} // namespace

ZoneDryingState fold_zone_drying_state(const std::vector<int>& gates,
                                       const std::vector<std::string>& per_gate_state) {
    ZoneDryingState best = ZoneDryingState::Idle;
    for (int gate : gates) {
        if (gate < 0 || static_cast<size_t>(gate) >= per_gate_state.size()) {
            continue;
        }
        const ZoneDryingState s = state_from_firmware(per_gate_state[static_cast<size_t>(gate)]);
        if (precedence(s) > precedence(best)) {
            best = s;
        }
    }
    return best;
}

void queue_zones_waiting_for_the_cap(std::vector<EnvironmentZone>& zones) {
    const bool any_running = std::any_of(zones.begin(), zones.end(), [](const EnvironmentZone& z) {
        return z.state == ZoneDryingState::Active;
    });
    if (!any_running) {
        return;
    }
    for (auto& z : zones) {
        if (z.dryer.supported && z.state != ZoneDryingState::Active) {
            z.state = ZoneDryingState::Queued;
        }
    }
}

std::vector<EnvironmentZone>
derive_environment_zones(const std::vector<std::string>& per_gate_heaters,
                         const std::vector<std::string>& per_gate_sensors, int gate_count,
                         const std::vector<std::string>& per_gate_state) {
    std::vector<EnvironmentZone> zones;
    std::unordered_map<std::string, size_t> zone_of_key;

    // Ascending gate order is what makes first appearance equal lowest gate, so
    // the result needs no separate sort to satisfy its ordering contract.
    for (int gate = 0; gate < gate_count; ++gate) {
        const std::string& heater = at_or_empty(per_gate_heaters, gate);
        const std::string& sensor = at_or_empty(per_gate_sensors, gate);
        if (heater.empty() && sensor.empty()) {
            continue;
        }

        // Klipper object names are unique and cannot contain '|', so the pair
        // separates unambiguously and two boxes never share a key.
        std::string key = heater + '|' + sensor;

        auto it = zone_of_key.find(key);
        if (it == zone_of_key.end()) {
            EnvironmentZone zone;
            zone.id = key;
            zone.heater_name = heater;
            zone.sensor_name = sensor;
            zone.dryer.supported = !heater.empty();
            zones.push_back(std::move(zone));
            it = zone_of_key.emplace(std::move(key), zones.size() - 1).first;
        }
        zones[it->second].gates.push_back(gate);
    }

    // Folding here rather than in each caller keeps the gate-to-zone mapping in one
    // place; a caller holding only the zone cannot rebuild it.
    for (auto& zone : zones) {
        zone.state = fold_zone_drying_state(zone.gates, per_gate_state);
    }

    return zones;
}

ZonePresentation select_zone_presentation(const std::vector<EnvironmentZone>& zones) {
    if (zones.size() <= 1) {
        return ZonePresentation::Single;
    }
    const bool first = zones.front().dryer.supported;
    for (const auto& z : zones) {
        if (z.dryer.supported != first) {
            return ZonePresentation::List;
        }
    }
    return zones.size() > kMaxZoneTabs ? ZonePresentation::List : ZonePresentation::Tabs;
}

} // namespace helix::printer
