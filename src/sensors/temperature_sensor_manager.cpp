// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temperature_sensor_manager.h"

#include "ui_update_queue.h"

#include "device_display_name.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "text_io.h"
#include "unit_conversions.h"

#include <algorithm>

// CRITICAL: Subject updates trigger lv_obj_invalidate() which asserts if called
// during LVGL rendering. WebSocket callbacks run on libhv's event loop thread,
// not the main LVGL thread. We must defer subject updates to the main thread
// via ui_queue_update() to avoid the "Invalidate area not allowed during rendering"
// assertion.

namespace helix::sensors {

// ============================================================================
// Singleton
// ============================================================================

TemperatureSensorManager& TemperatureSensorManager::instance() {
    static TemperatureSensorManager instance;
    return instance;
}

TemperatureSensorManager::TemperatureSensorManager() = default;

TemperatureSensorManager::~TemperatureSensorManager() = default;

// ============================================================================
// Discovery, Status and Config
// ============================================================================

void TemperatureSensorManager::discover(const std::vector<std::string>& klipper_objects) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[TemperatureSensorManager] Discovering temperature sensors from {} objects",
                  klipper_objects.size());

    std::vector<TemperatureSensorConfig> discovered;
    for (const auto& klipper_name : klipper_objects) {
        std::string sensor_name;
        TemperatureSensorType type = TemperatureSensorType::TEMPERATURE_SENSOR;

        if (!parse_klipper_name(klipper_name, sensor_name, type)) {
            continue;
        }

        // Exclude extruder and heater_bed (managed by PrinterState)
        if (sensor_name == "heater_bed") {
            continue;
        }
        if (sensor_name == "extruder" || sensor_name.find("extruder") == 0) {
            continue;
        }

        // Generate display name. A heater is named as one ("Chamber Heater"), so
        // it never reads the same as a thermistor of the same name.
        std::string display_name = helix::get_display_name(
            sensor_name, type == TemperatureSensorType::HEATER_GENERIC ? DeviceType::HEATER
                                                                       : DeviceType::TEMP_SENSOR);

        TemperatureSensorConfig config(klipper_name, sensor_name, display_name, type);

        // Auto-categorize based on sensor name
        if (sensor_name.find("chamber") != std::string::npos) {
            config.role = TemperatureSensorRole::CHAMBER;
            config.priority = 0;
        } else if (sensor_name.find("mcu") != std::string::npos) {
            config.role = TemperatureSensorRole::MCU;
            config.priority = 10;
        } else if (sensor_name == "raspberry_pi" || sensor_name == "host_temp" ||
                   sensor_name == "host" || sensor_name == "rpi" ||
                   sensor_name.find("raspberry") != std::string::npos) {
            config.role = TemperatureSensorRole::HOST;
            config.priority = 20;
        } else if (klipper_name.rfind("tmc2240 ", 0) == 0 ||
                   klipper_name.rfind("tmc5160 ", 0) == 0) {
            config.role = TemperatureSensorRole::STEPPER_DRIVER;
            config.priority = 30;
        } else {
            config.role = TemperatureSensorRole::AUXILIARY;
            config.priority = 100;
        }

        // Ensure a dynamic subject exists for this sensor
        ensure_sensor_subject(klipper_name);

        spdlog::debug("[TemperatureSensorManager] Discovered sensor: {} (type: {}, role: {}, "
                      "priority: {})",
                      sensor_name, temp_type_to_string(type), temp_role_to_string(config.role),
                      config.priority);
        discovered.push_back(std::move(config));
    }
    sensors_.reconcile(std::move(discovered));

    // Remove stale dynamic subjects using explicit two-phase protocol:
    // Phase 1: Expire lifetime tokens — invalidates ObserverGuard weak_ptrs
    //          so they won't call lv_observer_remove() on freed observers.
    // Phase 2: Erase subjects — DynamicIntSubject destructor calls lv_subject_deinit().
    for (auto& [name, subj] : temp_subjects_) {
        if (!sensors_.find(name) && subj) {
            spdlog::trace("[TemperatureSensorManager] Expiring lifetime token for orphaned "
                          "sensor: {}",
                          name);
            if (subj->lifetime)
                *subj->lifetime = false; // Signal death (#816)
            subj->lifetime.reset();      // Phase 1: expire before deinit
        }
    }
    for (auto it = temp_subjects_.begin(); it != temp_subjects_.end();) {
        if (!sensors_.find(it->first)) {
            it = temp_subjects_.erase(it); // Phase 2: deinit via destructor
        } else {
            ++it;
        }
    }

    // Update sensor count subject
    if (subjects_initialized_) {
        lv_subject_set_int(&sensor_count_, static_cast<int>(sensors_.size()));
    }

    spdlog::debug("[TemperatureSensorManager] Discovered {} temperature sensors", sensors_.size());

    // Update subjects to reflect new state
    update_subjects();
}

void TemperatureSensorManager::update_from_status(const nlohmann::json& status) {
    bool any_changed = false;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        for (const auto& sensor : sensors_) {
            const std::string& key = sensor.klipper_name;

            if (!status.contains(key)) {
                continue;
            }

            const auto& sensor_data = status[key];
            auto& state = sensors_.state_at(sensor.klipper_name);
            TemperatureSensorState old_state = state;

            // Field-restricted Moonraker subscriptions send null for fields the
            // underlying object lacks. Use find() + is_number() so unexpected
            // nulls (or wrong types during firmware restarts) don't throw
            // type_error.302 and crash the process.
            if (auto it = sensor_data.find("temperature");
                it != sensor_data.end() && it->is_number()) {
                state.temperature = it->get<float>();
            }
            // target is temperature_fan / heater_generic only; speed is temperature_fan only.
            if (auto it = sensor_data.find("target"); it != sensor_data.end() && it->is_number()) {
                state.target = it->get<float>();
            }
            if (auto it = sensor_data.find("speed"); it != sensor_data.end() && it->is_number()) {
                state.speed = it->get<float>();
            }

            // Check for state change
            if (state.temperature != old_state.temperature || state.target != old_state.target ||
                state.speed != old_state.speed) {
                any_changed = true;
                spdlog::trace("[TemperatureSensorManager] Sensor {} updated: temp={:.1f}C, "
                              "target={:.1f}C, speed={:.2f}",
                              sensor.sensor_name, state.temperature, state.target, state.speed);
            }
        }

        if (any_changed) {
            if (sync_mode_) {
                spdlog::trace(
                    "[TemperatureSensorManager] sync_mode: updating subjects synchronously");
                update_subjects();
            } else {
                spdlog::trace(
                    "[TemperatureSensorManager] async_mode: deferring via ui_queue_update");
                auto tok = lifetime_.token();
                tok.defer("TemperatureSensorManager::update_subjects", [] {
                    TemperatureSensorManager::instance().update_subjects_on_main_thread();
                });
            }
        }
    }
}

void TemperatureSensorManager::load_config(const nlohmann::json& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[TemperatureSensorManager] Loading config");

    if (!sensors_.apply_json(config, temp_role_from_string)) {
        spdlog::debug("[TemperatureSensorManager] No sensors config found");
        return;
    }

    update_subjects();
    spdlog::info("[TemperatureSensorManager] Config loaded");
}

nlohmann::json TemperatureSensorManager::save_config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[TemperatureSensorManager] Saving config");

    auto config = sensors_.to_json(temp_role_to_string, temp_type_to_string);

    spdlog::info("[TemperatureSensorManager] Config saved");
    return config;
}

// ============================================================================
// Initialization
// ============================================================================

void TemperatureSensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    spdlog::trace("[TemperatureSensorManager] Initializing subjects");

    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "temp_sensor_count", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit("TemperatureSensorManager", []() {
        TemperatureSensorManager::instance().deinit_subjects();
    });

    spdlog::trace("[TemperatureSensorManager] Subjects initialized");
}

void TemperatureSensorManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[TemperatureSensorManager] Deinitializing subjects");

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // Invalidate lifetime guard FIRST — expires tokens in queued callbacks
        // so they won't access state after we clear it (L072, L054)
        lifetime_.invalidate();

        // Clear all collections under mutex to prevent background thread access
        // to stale iterators during shutdown race
        sensors_.clear();

        // Signal subject death before clearing — sets bool to false so ALL
        // ObserverGuards detect dead subjects even with outstanding shared_ptr
        // copies held by other services. (#816)
        for (auto& [name, subj] : temp_subjects_) {
            if (subj && subj->lifetime)
                *subj->lifetime = false;
        }
        temp_subjects_.clear();

        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    spdlog::trace("[TemperatureSensorManager] Subjects deinitialized");
}

// ============================================================================
// Sensor Queries
// ============================================================================

bool TemperatureSensorManager::has_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !sensors_.empty();
}

std::vector<TemperatureSensorConfig> TemperatureSensorManager::get_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.configs(); // Return thread-safe copy
}

std::vector<TemperatureSensorConfig> TemperatureSensorManager::get_sensors_sorted() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto sorted = sensors_.configs();
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.display_name < b.display_name;
    });

    return sorted;
}

size_t TemperatureSensorManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.size();
}

// ============================================================================
// Configuration
// ============================================================================

void TemperatureSensorManager::set_sensor_role(const std::string& klipper_name,
                                               TemperatureSensorRole role) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (auto* sensor = sensors_.find(klipper_name)) {
        sensor->role = role;
        spdlog::info("[TemperatureSensorManager] Set role for {} to {}", sensor->sensor_name,
                     temp_role_to_string(role));
        update_subjects();
    }
}

void TemperatureSensorManager::apply_chamber_sensor_override(const std::string& klipper_name,
                                                             const std::string& heater_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // The chamber's heater_generic joins the chamber role so it is not listed
    // again as an auxiliary heater. A temperature_fan in the heater slot keeps
    // its own role: it is a fan with its own reading, listed as one.
    auto* chamber_heater =
        heater_name.rfind("heater_generic ", 0) == 0 ? sensors_.find(heater_name) : nullptr;

    // A sensor name this printer does not report would demote the incumbent
    // CHAMBER below and promote nothing in its place, vacating the chamber role.
    // Keep the auto-categorizer's classification standing instead, apart from
    // the heater, which is the chamber's whatever the sensor resolves to.
    if (!klipper_name.empty() && !sensors_.find(klipper_name)) {
        spdlog::debug("[TemperatureSensorManager] Chamber override '{}' not found in discovered "
                      "sensors; keeping auto-categorized roles",
                      klipper_name);
        if (chamber_heater && chamber_heater->role != TemperatureSensorRole::CHAMBER) {
            chamber_heater->role = TemperatureSensorRole::CHAMBER;
            chamber_heater->priority = 0;
            update_subjects();
        }
        return;
    }

    std::vector<std::string> chamber_names;
    if (!klipper_name.empty())
        chamber_names.push_back(klipper_name);
    if (chamber_heater)
        chamber_names.push_back(heater_name);
    auto is_chamber_name = [&](const std::string& name) {
        return std::find(chamber_names.begin(), chamber_names.end(), name) != chamber_names.end();
    };

    // Early-out when the named objects already are exactly the CHAMBER set:
    // avoids demoting + repromoting them and the accompanying log line on
    // every reconnect for printers whose chamber objects match the
    // auto-categorizer (e.g. literally named "chamber").
    if (!chamber_names.empty()) {
        bool needs_change = false;
        for (const auto& config : sensors_) {
            bool is_target = is_chamber_name(config.klipper_name);
            bool is_chamber = (config.role == TemperatureSensorRole::CHAMBER);
            if (is_target != is_chamber) {
                needs_change = true;
                break;
            }
        }
        if (!needs_change)
            return;
    }

    // Demote any existing CHAMBER sensor back to an inferred role
    for (auto& config : sensors_) {
        if (config.role == TemperatureSensorRole::CHAMBER) {
            std::string name_lower = config.sensor_name;
            name_lower = helix::text_io::to_lower(name_lower);
            if (name_lower.find("mcu") != std::string::npos) {
                config.role = TemperatureSensorRole::MCU;
                config.priority = 10;
            } else if (name_lower.find("raspberry") != std::string::npos ||
                       name_lower == "host_temp" || name_lower == "host" || name_lower == "rpi") {
                config.role = TemperatureSensorRole::HOST;
                config.priority = 20;
            } else {
                config.role = TemperatureSensorRole::AUXILIARY;
                config.priority = 100;
            }
        }
    }

    // Promote the chamber's sensor and heater to CHAMBER role.
    for (const auto& name : chamber_names) {
        if (auto* sensor = sensors_.find(name)) {
            sensor->role = TemperatureSensorRole::CHAMBER;
            sensor->priority = 0;
            spdlog::info("[TemperatureSensorManager] Chamber override: {}", name);
        }
    }

    update_subjects();
}

void TemperatureSensorManager::set_sensor_enabled(const std::string& klipper_name, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (auto* sensor = sensors_.find(klipper_name)) {
        sensor->enabled = enabled;
        spdlog::info("[TemperatureSensorManager] Set enabled for {} to {}", sensor->sensor_name,
                     enabled);
        update_subjects();
    }
}

// ============================================================================
// State Queries
// ============================================================================

std::optional<TemperatureSensorState>
TemperatureSensorManager::get_sensor_state(const std::string& klipper_name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const auto* state = sensors_.state(klipper_name);
    return state ? std::optional(*state) : std::nullopt;
}

// ============================================================================
// LVGL Subjects
// ============================================================================

lv_subject_t* TemperatureSensorManager::get_temp_subject(const std::string& klipper_name,
                                                         SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = temp_subjects_.find(klipper_name);
    if (it == temp_subjects_.end()) {
        lifetime.reset();
        return nullptr;
    }

    lifetime = it->second->lifetime;
    return &it->second->subject;
}

lv_subject_t* TemperatureSensorManager::get_temp_subject(const std::string& klipper_name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = temp_subjects_.find(klipper_name);
    if (it == temp_subjects_.end()) {
        return nullptr;
    }

    return &it->second->subject;
}

lv_subject_t* TemperatureSensorManager::get_target_subject(const std::string& klipper_name,
                                                           SubjectLifetime& lifetime) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = temp_subjects_.find(klipper_name);
    if (it == temp_subjects_.end()) {
        lifetime.reset();
        return nullptr;
    }

    lifetime = it->second->lifetime;
    return &it->second->target_subject;
}

lv_subject_t* TemperatureSensorManager::get_sensor_count_subject() {
    return &sensor_count_;
}

// ============================================================================
// Testing Support
// ============================================================================

void TemperatureSensorManager::set_sync_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sync_mode_ = enabled;
}

void TemperatureSensorManager::update_subjects_on_main_thread() {
    update_subjects();
}

// ============================================================================
// Private Helpers
// ============================================================================

bool TemperatureSensorManager::parse_klipper_name(const std::string& klipper_name,
                                                  std::string& sensor_name,
                                                  TemperatureSensorType& type) const {
    const std::string temp_sensor_prefix = "temperature_sensor ";
    const std::string temp_fan_prefix = "temperature_fan ";

    if (klipper_name.rfind(temp_sensor_prefix, 0) == 0) {
        sensor_name = klipper_name.substr(temp_sensor_prefix.length());
        type = TemperatureSensorType::TEMPERATURE_SENSOR;
        return true;
    }

    if (klipper_name.rfind(temp_fan_prefix, 0) == 0) {
        sensor_name = klipper_name.substr(temp_fan_prefix.length());
        type = TemperatureSensorType::TEMPERATURE_FAN;
        return true;
    }

    const std::string heater_generic_prefix = "heater_generic ";
    if (klipper_name.rfind(heater_generic_prefix, 0) == 0) {
        sensor_name = klipper_name.substr(heater_generic_prefix.length());
        type = TemperatureSensorType::HEATER_GENERIC;
        return true;
    }

    // TMC stepper drivers with built-in temperature sensing
    static const std::string tmc_prefixes[] = {"tmc2240 ", "tmc5160 "};
    for (const auto& prefix : tmc_prefixes) {
        if (klipper_name.rfind(prefix, 0) == 0) {
            sensor_name = klipper_name.substr(prefix.length());
            type = TemperatureSensorType::TEMPERATURE_SENSOR;
            return true;
        }
    }

    return false;
}

void TemperatureSensorManager::ensure_sensor_subject(const std::string& klipper_name) {
    if (temp_subjects_.find(klipper_name) != temp_subjects_.end()) {
        return; // Already exists
    }

    auto subj = std::make_unique<DynamicIntSubject>();
    lv_subject_init_int(&subj->subject, 0);
    lv_subject_init_int(&subj->target_subject, 0);
    subj->initialized = true;
    subj->lifetime = std::make_shared<bool>(true);

    spdlog::debug("[TemperatureSensorManager] Created dynamic subject for {}", klipper_name);
    temp_subjects_[klipper_name] = std::move(subj);
}

void TemperatureSensorManager::update_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Update per-sensor dynamic subjects with decidegrees values
    for (const auto& sensor : sensors_) {
        auto subj_it = temp_subjects_.find(sensor.klipper_name);
        if (subj_it == temp_subjects_.end() || !subj_it->second->initialized) {
            continue;
        }

        const auto* state = sensors_.state(sensor.klipper_name);
        if (!state) {
            continue;
        }

        // Convert temperature to decidegrees (×10 for 0.1°C resolution)
        int decidegrees = helix::units::to_decidegrees(state->temperature);
        lv_subject_set_int(&subj_it->second->subject, decidegrees);
        int target_deci = helix::units::to_decidegrees(state->target);
        if (lv_subject_get_int(&subj_it->second->target_subject) != target_deci)
            lv_subject_set_int(&subj_it->second->target_subject, target_deci);
    }

    spdlog::trace("[TemperatureSensorManager] Subjects updated: {} sensors", sensors_.size());
}

} // namespace helix::sensors
