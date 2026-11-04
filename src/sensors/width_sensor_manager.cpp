// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "width_sensor_manager.h"

#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"

#include <algorithm>

#include "hv/json.hpp"

namespace helix::sensors {

namespace {

constexpr uint8_t kFlowCompensation = static_cast<uint8_t>(WidthSensorRole::FLOW_COMPENSATION);

} // namespace

WidthSensorManager::Options WidthSensorManager::make_options() {
    Options o;
    o.name = "WidthSensorManager";
    o.kinds = {hw::ObjectKind::WidthSensor};
    o.count_subject = "width_sensor_count";
    o.revision_subject = "width_sensor_revision";
    o.config_key = "width_sensors";
    o.settings_section = "width";
    for (const auto& r : kWidthSensorRoles) {
        const bool none = r.value == WidthSensorRole::NONE;
        o.roles.push_back({static_cast<uint8_t>(r.value), r.id, none ? "None" : r.display,
                           /*exclusive=*/!none, /*offered=*/true});
    }
    for (const auto& t : kWidthSensorTypes) {
        o.types.push_back({static_cast<uint8_t>(t.value), t.id, t.display});
    }
    return o;
}

SensorRowView WidthSensorManager::row(size_t index) const {
    SensorRowView view = SensorManager::row(index);
    const auto all = entries();
    if (index >= all.size()) {
        return view;
    }
    const SensorEntry& entry = all[index];
    std::string type;
    for (const auto& t : kWidthSensorTypes) {
        if (static_cast<uint8_t>(t.value) == entry.type) {
            type = lv_tr(t.display);
        }
    }
    if (!type.empty()) {
        view.name = fmt::format(fmt::runtime(lv_tr("{} width sensor")), type);
        view.detail = type + " \u00b7 " + entry.klipper_name;
    }
    return view;
}

WidthSensorManager& WidthSensorManager::instance() {
    static WidthSensorManager instance;
    return instance;
}

WidthSensorManager::WidthSensorManager() : SensorManager(make_options()) {}

void WidthSensorManager::register_subjects(SubjectManager& subjects) {
    // -1 = no sensor assigned, 0+ = diameter in mm * 1000
    UI_MANAGED_SUBJECT_INT(diameter_, -1, "filament_width_diameter", subjects);
    UI_MANAGED_SUBJECT_STRING(diameter_text_, diameter_text_buf_, "--", "filament_diameter_text",
                              subjects);
}

int WidthSensorManager::flow_diameter_micrometers_locked() const {
    for (const auto& entry : entries_locked()) {
        if (entry.role != kFlowCompensation || !entry.enabled || !entry.available) {
            continue;
        }
        const auto reading = readings_.find(entry.klipper_name);
        const float diameter = reading != readings_.end() ? reading->second.diameter : 0.0f;
        return static_cast<int>(diameter * 1000.0f);
    }
    return -1;
}

void WidthSensorManager::refresh_subjects() {
    int diameter = -1;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        diameter = flow_diameter_micrometers_locked();
    }
    lv_subject_set_int(&diameter_, diameter);
    spdlog::trace("[WidthSensorManager] diameter={}", diameter);
}

bool WidthSensorManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const auto sensor = status.find(entry.klipper_name);
        if (sensor == status.end() || !sensor->is_object()) {
            continue;
        }
        WidthSensorState& state = readings_[entry.klipper_name];
        const WidthSensorState before = state;
        // Field-restricted subscriptions send null for absent fields.
        if (const auto it = sensor->find("Diameter"); it != sensor->end() && it->is_number()) {
            state.diameter = it->get<float>();
        }
        if (const auto it = sensor->find("Raw"); it != sensor->end() && it->is_number()) {
            state.raw_value = it->get<float>();
        }
        if (state.diameter != before.diameter || state.raw_value != before.raw_value) {
            changed = true;
            spdlog::debug("[WidthSensorManager] {} diameter={:.3f}mm raw={}", entry.klipper_name,
                          state.diameter, state.raw_value);
        }
    }
    return changed;
}

void WidthSensorManager::on_entries_changed() {
    prune_vanished_locked(readings_);
}

void WidthSensorManager::on_discovered(const PrinterDiscovery& /*hw*/) {
    // First-time users get a populated diameter subject. A saved role, applied by
    // load_settings() afterwards, overrides this.
    auto& entries = entries_locked();
    if (entries.empty()) {
        return;
    }
    const bool held = std::any_of(entries.begin(), entries.end(),
                                  [](const SensorEntry& e) { return e.role == kFlowCompensation; });
    if (!held) {
        entries.front().role = kFlowCompensation;
        spdlog::debug("[WidthSensorManager] {} defaults to FLOW_COMPENSATION",
                      entries.front().klipper_name);
    }
}

lv_subject_t* WidthSensorManager::get_diameter_subject() {
    return &diameter_;
}

lv_subject_t* WidthSensorManager::get_diameter_text_subject() {
    return &diameter_text_;
}

} // namespace helix::sensors
