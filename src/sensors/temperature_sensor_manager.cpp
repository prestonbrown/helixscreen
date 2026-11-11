// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_sensor_manager.h"

#include "core/text_io.h"
#include "device_display_name.h"
#include "humidity_sensor_types.h"
#include "notice.h"
#include "printer_discovery.h"
#include "sort_small.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/spdlog.h"
#include "unit_conversions.h"

#include <algorithm>

#include "hv/json.hpp"

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
    if (hw::classify_object(klipper_name).kind == hw::ObjectKind::DriverTemperature) {
        return kStepperDriver;
    }
    return kAuxiliary;
}

/// The role a `[temperature_sensor]` section's sensor_type implies, or 0 for none.
uint8_t role_for_sensor_type(const nlohmann::json* body) {
    if (body == nullptr || !body->is_object()) {
        return 0;
    }
    const auto it = body->find("sensor_type");
    if (it == body->end() || !it->is_string()) {
        return 0;
    }
    const std::string type = helix::text_io::to_lower(it->get<std::string>());
    if (type == "temperature_mcu") {
        return kMcu;
    }
    if (type == "temperature_host") {
        return kHost;
    }
    return 0;
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
    // The configfile pass reads each sensor's sensor_type for the MCU and HOST roles.
    o.rediscover_on_config = true;
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

void TemperatureSensorManager::on_discovered(const PrinterDiscovery& hw) {
    for (auto& e : entries_locked()) {
        if (e.role == 0) {
            e.role = role_for_name(e.display_name, e.klipper_name);
        }
        // A sensor_type outranks the name, but only over the AUXILIARY fallback:
        // CHAMBER may have been moved here by apply_chamber_sensor_override().
        if (e.role == kAuxiliary) {
            if (const uint8_t typed = role_for_sensor_type(hw.section(e.klipper_name))) {
                e.role = typed;
            }
        }
        spdlog::debug("[TemperatureSensorManager] Discovered {} (role {})", e.klipper_name,
                      enum_id(kTemperatureSensorRoles, static_cast<TemperatureSensorRole>(e.role)));
    }

    driver_objects_.clear();
    for (const auto& [name, kind] : hw.object_kinds()) {
        if (hw::is_stepper_driver(kind)) {
            driver_objects_.push_back(name);
        }
    }
    for (auto it = driver_levels_.begin(); it != driver_levels_.end();) {
        const bool listed = std::find(driver_objects_.begin(), driver_objects_.end(), it->first) !=
                            driver_objects_.end();
        it = listed ? std::next(it) : driver_levels_.erase(it);
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
    driver_levels_.clear();
    driver_levels_notified_.clear();
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

    // A frame without drv_status leaves the level alone; a null one (motors off,
    // the driver unpolled) clears it.
    for (const auto& name : driver_objects_) {
        const auto driver = status.find(name);
        if (driver == status.end() || !driver->is_object()) {
            continue;
        }
        const auto flags = driver->find("drv_status");
        if (flags == driver->end()) {
            continue;
        }
        const DriverOvertemp level = classify_drv_status(*flags);
        const auto known = driver_levels_.find(name);
        const DriverOvertemp before =
            known != driver_levels_.end() ? known->second : DriverOvertemp::None;
        if (level == before) {
            continue;
        }
        if (level == DriverOvertemp::None) {
            driver_levels_.erase(name);
        } else {
            driver_levels_[name] = level;
        }
        changed = true;
        spdlog::info("[TemperatureSensorManager] {} overtemperature level {} -> {}", name,
                     static_cast<int>(before), static_cast<int>(level));
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
    std::map<std::string, DriverOvertemp> driver_levels;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        driver_levels = driver_levels_;
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

    // One notice per rise, so a driver that stays hot does not repeat it.
    for (const auto& alert : driver_overtemp_rises(driver_levels_notified_, driver_levels)) {
        publish_driver_overtemp_notice(alert);
    }
    driver_levels_notified_ = std::move(driver_levels);
}

void TemperatureSensorManager::publish_driver_overtemp_notice(const DriverOvertempAlert& alert) {
    const std::string driver = hw::classify_object(alert.klipper_name).instance;
    const std::string stepper = driver.empty() ? alert.klipper_name : driver;
    if (alert.level == DriverOvertemp::Shutdown) {
        publish_notice(
            {NoticeSeverity::ERROR,
             fmt::format(fmt::runtime(lv_tr("Stepper driver {} overheated and shut down")),
                         stepper),
             10000});
    } else {
        publish_notice(
            {NoticeSeverity::WARNING,
             fmt::format(fmt::runtime(lv_tr("Stepper driver {} is overheating")), stepper), 8000});
    }
}

// ============================================================================
// Stepper driver overtemperature
// ============================================================================

DriverOvertemp classify_drv_status(const nlohmann::json& drv_status) {
    if (!drv_status.is_object()) {
        return DriverOvertemp::None;
    }
    // Klipper publishes only the fields that are set, each as a non-zero number.
    auto set = [&drv_status](const char* field) {
        const auto it = drv_status.find(field);
        return it != drv_status.end() && it->is_number() && it->get<double>() != 0.0;
    };
    if (set("ot")) {
        return DriverOvertemp::Shutdown;
    }
    for (const char* field : {"otpw", "t120", "t143", "t150", "t157"}) {
        if (set(field)) {
            return DriverOvertemp::Warning;
        }
    }
    return DriverOvertemp::None;
}

std::vector<DriverOvertempAlert>
driver_overtemp_rises(const std::map<std::string, DriverOvertemp>& before,
                      const std::map<std::string, DriverOvertemp>& now) {
    std::vector<DriverOvertempAlert> rises;
    for (const auto& [name, level] : now) {
        const auto it = before.find(name);
        const DriverOvertemp was = it != before.end() ? it->second : DriverOvertemp::None;
        if (level > was) {
            rises.push_back({name, level});
        }
    }
    return rises;
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
