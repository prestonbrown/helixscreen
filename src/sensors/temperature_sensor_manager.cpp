// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_sensor_manager.h"

#include "device_display_name.h"
#include "humidity_sensor_types.h"
#include "spdlog/spdlog.h"
#include "text_io.h"
#include "unit_conversions.h"

#include <algorithm>

namespace helix::sensors {

namespace {

constexpr uint8_t kChamber = static_cast<uint8_t>(TemperatureSensorRole::CHAMBER);
constexpr uint8_t kMcu = static_cast<uint8_t>(TemperatureSensorRole::MCU);
constexpr uint8_t kHost = static_cast<uint8_t>(TemperatureSensorRole::HOST);
constexpr uint8_t kAuxiliary = static_cast<uint8_t>(TemperatureSensorRole::AUXILIARY);
constexpr uint8_t kStepperDriver = static_cast<uint8_t>(TemperatureSensorRole::STEPPER_DRIVER);

/// Lower sorts first in the pickers.
int priority_of(uint8_t role) {
    switch (role) {
    case kChamber:
        return 0;
    case kMcu:
        return 10;
    case kHost:
        return 20;
    case kStepperDriver:
        return 30;
    default:
        return 100;
    }
}

bool is_host_name(const std::string& name) {
    return name == "host_temp" || name == "host" || name == "rpi" ||
           name.find("raspberry") != std::string::npos;
}

/// The role a sensor's name implies. `name` is the part after the object prefix.
uint8_t role_for_name(const std::string& name, const std::string& klipper_name) {
    if (name.find("chamber") != std::string::npos) {
        return kChamber;
    }
    if (name.find("mcu") != std::string::npos) {
        return kMcu;
    }
    if (is_host_name(name)) {
        return kHost;
    }
    if (klipper_name.rfind("tmc2240 ", 0) == 0 || klipper_name.rfind("tmc5160 ", 0) == 0) {
        return kStepperDriver;
    }
    return kAuxiliary;
}

/// The role a sensor falls back to when CHAMBER is taken from it.
uint8_t role_after_chamber(const std::string& name) {
    const std::string lower = helix::text_io::to_lower(name);
    if (lower.find("mcu") != std::string::npos) {
        return kMcu;
    }
    if (is_host_name(lower)) {
        return kHost;
    }
    return kAuxiliary;
}

TemperatureSensorType type_of(const std::string& klipper_name) {
    switch (hw::classify_object(klipper_name).kind) {
    case hw::ObjectKind::TemperatureFan:
        return TemperatureSensorType::TEMPERATURE_FAN;
    case hw::ObjectKind::GenericHeater:
        return TemperatureSensorType::HEATER_GENERIC;
    default:
        return TemperatureSensorType::TEMPERATURE_SENSOR;
    }
}

TemperatureSensorConfig config_of(const SensorEntry& entry) {
    const TemperatureSensorType type = type_of(entry.klipper_name);
    TemperatureSensorConfig config(
        entry.klipper_name, entry.display_name,
        // A heater is named as one ("Chamber Heater") and a humidity chip carries its
        // chip ("Chamber Temperature (BME280)"), so neither reads the same as a
        // thermistor of the same name.
        with_humidity_chip_suffix(
            helix::get_display_name(entry.display_name,
                                    type == TemperatureSensorType::HEATER_GENERIC
                                        ? DeviceType::HEATER
                                        : DeviceType::TEMP_SENSOR),
            entry.klipper_name),
        type);
    config.role = static_cast<TemperatureSensorRole>(entry.role);
    config.enabled = entry.enabled;
    config.priority = priority_of(entry.role);
    return config;
}

/// Settings and widget order: priority, then display name.
bool sorts_before(const TemperatureSensorConfig& a, const TemperatureSensorConfig& b) {
    if (a.priority != b.priority) {
        return a.priority < b.priority;
    }
    return a.display_name < b.display_name;
}

/// Stable insertion sort. These lists hold a handful of sensors, where
/// std::sort and std::stable_sort cost firmware flash and buy nothing.
template <typename T, typename Less> void sort_small(std::vector<T>& items, Less less) {
    for (size_t i = 1; i < items.size(); ++i) {
        T item = std::move(items[i]);
        size_t j = i;
        for (; j > 0 && less(item, items[j - 1]); --j) {
            items[j] = std::move(items[j - 1]);
        }
        items[j] = std::move(item);
    }
}

} // namespace

TemperatureSensorManager::Options TemperatureSensorManager::make_options() {
    Options o;
    o.name = "TemperatureSensorManager";
    o.kinds = {hw::ObjectKind::TemperatureSensor, hw::ObjectKind::TemperatureFan,
               hw::ObjectKind::DriverTemperature, hw::ObjectKind::GenericHeater,
               hw::ObjectKind::HumidityChip};
    o.count_subject = "temp_sensor_count";
    o.revision_subject = "temp_sensor_revision";
    o.settings_section = "temp";
    o.row_toggle = RowToggle::None;
    // Readings go to the temperature subjects; Settings rows show none.
    o.rows_show_readings = false;
    for (const auto& r : kTemperatureSensorRoles) {
        if (r.value == TemperatureSensorRole::NONE) {
            continue;
        }
        o.roles.push_back({static_cast<uint8_t>(r.value), r.id, r.display, /*exclusive=*/false,
                           /*offered=*/false});
    }
    return o;
}

bool TemperatureSensorManager::listed_in_settings(const SensorEntry& entry) const {
    return role_of<TemperatureSensorRole>(entry) != TemperatureSensorRole::CHAMBER;
}

SensorRowView TemperatureSensorManager::row(size_t index) const {
    SensorRowView view = SensorManager::row(index);
    const auto all = entries();
    if (index < all.size()) {
        view.name = config_of(all[index]).display_name;
    }
    return view;
}

std::vector<size_t> TemperatureSensorManager::settings_rows() const {
    auto rows = SensorManager::settings_rows();
    const auto all = entries();
    std::vector<TemperatureSensorConfig> configs;
    configs.reserve(all.size());
    for (const auto& e : all) {
        configs.push_back(config_of(e));
    }
    sort_small(rows,
               [&configs](size_t a, size_t b) { return sorts_before(configs[a], configs[b]); });
    return rows;
}

TemperatureSensorManager& TemperatureSensorManager::instance() {
    static TemperatureSensorManager instance;
    return instance;
}

TemperatureSensorManager::TemperatureSensorManager() : SensorManager(make_options()) {}

// ============================================================================
// Discovery hooks
// ============================================================================

void TemperatureSensorManager::on_discovered(const PrinterDiscovery& /*hw*/) {
    for (auto& e : entries_locked()) {
        if (e.role != 0) {
            continue;
        }
        e.role = role_for_name(e.display_name, e.klipper_name);
        spdlog::debug("[TemperatureSensorManager] Discovered {} (role {})", e.klipper_name,
                      enum_id(kTemperatureSensorRoles, static_cast<TemperatureSensorRole>(e.role)));
    }
}

void TemperatureSensorManager::on_entries_changed() {
    std::map<std::string, TemperatureSensorState> kept;
    for (const auto& e : entries_locked()) {
        auto old = readings_.find(e.klipper_name);
        TemperatureSensorState state =
            old != readings_.end() ? old->second : TemperatureSensorState{};
        state.available = true;
        kept.emplace(e.klipper_name, state);
    }
    readings_ = std::move(kept);
    sync_subjects_locked();
}

void TemperatureSensorManager::sync_subjects_locked() {
    std::vector<std::string> names;
    names.reserve(entries_locked().size());
    for (const auto& e : entries_locked()) {
        names.push_back(e.klipper_name);
    }
    // The temperature map goes first: its lifetime expires before either subject is freed.
    temp_subjects_.remove_missing(names);
    target_subjects_.remove_missing(names);
    for (const auto& name : names) {
        SubjectLifetime unused;
        temp_subjects_.get_or_create(name, 0, unused);
        target_subjects_.get_or_create(name, 0, unused);
    }
}

void TemperatureSensorManager::on_deinit_subjects() {
    temp_subjects_.clear();
    target_subjects_.clear();
}

void TemperatureSensorManager::register_subjects(SubjectManager& /*subjects*/) {}

// ============================================================================
// Status
// ============================================================================

bool TemperatureSensorManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const auto sensor = status.find(entry.klipper_name);
        if (sensor == status.end() || !sensor->is_object()) {
            continue;
        }
        auto& state = readings_[entry.klipper_name];
        const TemperatureSensorState before = state;

        // Field-restricted subscriptions send null for fields the object lacks.
        if (auto it = sensor->find("temperature"); it != sensor->end() && it->is_number()) {
            state.temperature = it->get<float>();
        }
        // target is temperature_fan / heater_generic only; speed is temperature_fan only.
        if (auto it = sensor->find("target"); it != sensor->end() && it->is_number()) {
            state.target = it->get<float>();
        }
        if (auto it = sensor->find("speed"); it != sensor->end() && it->is_number()) {
            state.speed = it->get<float>();
        }

        if (state.temperature != before.temperature || state.target != before.target ||
            state.speed != before.speed) {
            changed = true;
            spdlog::trace("[TemperatureSensorManager] {} temp={:.1f}C target={:.1f}C speed={:.2f}",
                          entry.klipper_name, state.temperature, state.target, state.speed);
        }
    }
    return changed;
}

void TemperatureSensorManager::refresh_subjects() {
    struct Reading {
        std::string name;
        int temperature;
        int target;
    };
    std::vector<Reading> readings;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        // Subjects are rebuilt here too: deinit clears them and entries outlive it.
        sync_subjects_locked();
        for (const auto& e : entries_locked()) {
            const auto it = readings_.find(e.klipper_name);
            const TemperatureSensorState state =
                it != readings_.end() ? it->second : TemperatureSensorState{};
            readings.push_back({e.klipper_name, helix::units::to_decidegrees(state.temperature),
                                helix::units::to_decidegrees(state.target)});
        }
    }
    for (const auto& r : readings) {
        SubjectLifetime unused;
        if (lv_subject_t* temp = temp_subjects_.find(r.name, unused)) {
            lv_subject_set_int(temp, r.temperature);
        }
        if (lv_subject_t* target = target_subjects_.find(r.name, unused)) {
            if (lv_subject_get_int(target) != r.target) {
                lv_subject_set_int(target, r.target);
            }
        }
    }
}

// ============================================================================
// Reads
// ============================================================================

std::vector<TemperatureSensorConfig> TemperatureSensorManager::get_sensors() const {
    std::vector<TemperatureSensorConfig> out;
    for (const auto& e : entries()) {
        out.push_back(config_of(e));
    }
    return out;
}

std::vector<TemperatureSensorConfig> TemperatureSensorManager::get_sensors_sorted() const {
    auto sorted = get_sensors();
    sort_small(sorted, sorts_before);
    return sorted;
}

std::optional<TemperatureSensorState>
TemperatureSensorManager::get_sensor_state(const std::string& klipper_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    const auto it = readings_.find(klipper_name);
    return it != readings_.end() ? std::optional(it->second) : std::nullopt;
}

// ============================================================================
// Chamber override
// ============================================================================

void TemperatureSensorManager::apply_chamber_sensor_override(const std::string& klipper_name,
                                                             const std::string& heater_name) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        auto& entries = entries_locked();
        auto find = [&entries](const std::string& name) -> SensorEntry* {
            for (auto& e : entries) {
                if (e.klipper_name == name) {
                    return &e;
                }
            }
            return nullptr;
        };

        // The chamber's heater_generic joins the chamber role so it is not listed
        // again as an auxiliary heater. A temperature_fan in the heater slot keeps
        // its own role: it is a fan with its own reading, listed as one.
        SensorEntry* chamber_heater =
            heater_name.rfind("heater_generic ", 0) == 0 ? find(heater_name) : nullptr;

        // A sensor name this printer does not report would demote the incumbent
        // CHAMBER and promote nothing in its place. The name-derived roles stand
        // instead, apart from the heater, which is the chamber's whatever the
        // sensor resolves to.
        if (!klipper_name.empty() && find(klipper_name) == nullptr) {
            spdlog::debug("[TemperatureSensorManager] Chamber override '{}' not found in "
                          "discovered sensors; keeping auto-categorized roles",
                          klipper_name);
            if (chamber_heater == nullptr || chamber_heater->role == kChamber) {
                return;
            }
            chamber_heater->role = kChamber;
        } else {
            std::vector<std::string> chamber_names;
            if (!klipper_name.empty()) {
                chamber_names.push_back(klipper_name);
            }
            if (chamber_heater != nullptr) {
                chamber_names.push_back(heater_name);
            }
            auto is_chamber_name = [&](const std::string& name) {
                return std::find(chamber_names.begin(), chamber_names.end(), name) !=
                       chamber_names.end();
            };

            // Nothing to do when the named objects already are exactly the CHAMBER set,
            // which keeps every reconnect quiet on printers whose chamber objects match
            // the name rules.
            if (!chamber_names.empty()) {
                const bool needs_change =
                    std::any_of(entries.begin(), entries.end(), [&](const SensorEntry& e) {
                        return is_chamber_name(e.klipper_name) != (e.role == kChamber);
                    });
                if (!needs_change) {
                    return;
                }
            }

            for (auto& e : entries) {
                if (e.role == kChamber) {
                    e.role = role_after_chamber(e.display_name);
                }
            }
            for (const auto& name : chamber_names) {
                if (SensorEntry* e = find(name)) {
                    e->role = kChamber;
                    spdlog::info("[TemperatureSensorManager] Chamber override: {}", name);
                }
            }
        }
    }
    publish();
}

// ============================================================================
// Subjects
// ============================================================================

lv_subject_t* TemperatureSensorManager::get_temp_subject(const std::string& klipper_name,
                                                         SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return temp_subjects_.find(klipper_name, lifetime);
}

lv_subject_t* TemperatureSensorManager::get_temp_subject(const std::string& klipper_name) {
    SubjectLifetime unused;
    return get_temp_subject(klipper_name, unused);
}

lv_subject_t* TemperatureSensorManager::get_target_subject(const std::string& klipper_name,
                                                           SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    SubjectLifetime unused;
    lv_subject_t* target = target_subjects_.find(klipper_name, unused);
    if (target == nullptr) {
        lifetime.reset();
        return nullptr;
    }
    temp_subjects_.find(klipper_name, lifetime);
    return target;
}

} // namespace helix::sensors
