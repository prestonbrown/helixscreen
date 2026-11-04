// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "humidity_sensor_manager.h"

#include "spdlog/spdlog.h"

#include <algorithm>

#include "hv/json.hpp"

namespace helix::sensors {

namespace {

constexpr uint8_t kChamber = static_cast<uint8_t>(HumiditySensorRole::CHAMBER);
constexpr uint8_t kDryer = static_cast<uint8_t>(HumiditySensorRole::DRYER);

bool holds_role(const std::vector<SensorEntry>& entries, uint8_t role) {
    return std::any_of(entries.begin(), entries.end(),
                       [role](const SensorEntry& e) { return e.role == role; });
}

} // namespace

HumiditySensorManager::Options HumiditySensorManager::make_options() {
    Options o;
    o.name = "HumiditySensorManager";
    o.kinds = {hw::ObjectKind::HumidityChip};
    o.count_subject = "humidity_sensor_count";
    o.revision_subject = "humidity_sensor_revision";
    o.settings_section = "humidity";
    o.row_toggle = RowToggle::None;
    for (const auto& r : kHumiditySensorRoles) {
        if (r.value == HumiditySensorRole::NONE) {
            continue;
        }
        o.roles.push_back({static_cast<uint8_t>(r.value), r.id, r.display, /*exclusive=*/true,
                           /*offered=*/false});
    }
    for (const auto& c : humidity_sensor_chips()) {
        // The chip table's views point at string literals, so data() is NUL-terminated.
        o.types.push_back(
            {static_cast<uint8_t>(c.type), c.config_id.data(), c.display_name.data()});
    }
    return o;
}

HumiditySensorManager& HumiditySensorManager::instance() {
    static HumiditySensorManager instance;
    return instance;
}

HumiditySensorManager::HumiditySensorManager() : SensorManager(make_options()) {}

lv_subject_t* HumiditySensorManager::get_chamber_humidity_subject() {
    return &chamber_humidity_;
}

void HumiditySensorManager::register_subjects(SubjectManager& subjects) {
    // -1 = no sensor assigned, 0+ = humidity x 10
    UI_MANAGED_SUBJECT_INT(chamber_humidity_, -1, "chamber_humidity", subjects);
}

bool HumiditySensorManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const auto sensor = status.find(entry.klipper_name);
        if (sensor == status.end() || !sensor->is_object()) {
            continue;
        }
        // Field-restricted subscriptions send null for absent fields.
        const auto it = sensor->find("humidity");
        if (it == sensor->end() || !it->is_number()) {
            continue;
        }
        const float value = it->get<float>();
        const auto [slot, inserted] = humidity_.try_emplace(entry.klipper_name, value);
        if (inserted || slot->second != value) {
            slot->second = value;
            changed = true;
            spdlog::trace("[HumiditySensorManager] {} humidity={:.1f}%", entry.klipper_name, value);
        }
    }
    return changed;
}

void HumiditySensorManager::refresh_subjects() {
    int chamber = -1;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        for (const auto& e : entries_locked()) {
            if (e.role == kChamber && e.enabled && e.available) {
                const auto it = humidity_.find(e.klipper_name);
                const float humidity = it != humidity_.end() ? it->second : 0.0f;
                chamber = static_cast<int>(humidity * 10.0f);
                break;
            }
        }
    }
    lv_subject_set_int(&chamber_humidity_, chamber);
}

void HumiditySensorManager::on_entries_changed() {
    prune_vanished_locked(humidity_);
}

void HumiditySensorManager::on_discovered(const PrinterDiscovery& /*hw*/) {
    // The first sensor named "chamber" takes CHAMBER and the first named "dryer" takes DRYER,
    // unless a sensor already holds the role.
    auto& entries = entries_locked();
    bool has_chamber = holds_role(entries, kChamber);
    bool has_dryer = holds_role(entries, kDryer);
    for (auto& e : entries) {
        if (e.role != 0) {
            continue;
        }
        if (!has_chamber && e.display_name.find("chamber") != std::string::npos) {
            e.role = kChamber;
            has_chamber = true;
            spdlog::info("[HumiditySensorManager] {} takes CHAMBER", e.display_name);
        } else if (!has_dryer && e.display_name.find("dryer") != std::string::npos) {
            e.role = kDryer;
            has_dryer = true;
            spdlog::info("[HumiditySensorManager] {} takes DRYER", e.display_name);
        }
    }
}

} // namespace helix::sensors
