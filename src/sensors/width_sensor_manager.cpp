// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "width_sensor_manager.h"

#include "ui_update_queue.h"

#include "config.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>

// CRITICAL: Subject updates trigger lv_obj_invalidate() which asserts if called
// during LVGL rendering. WebSocket callbacks run on libhv's event loop thread,
// not the main LVGL thread. We must defer subject updates to the main thread
// via ui_async_call to avoid the "Invalidate area not allowed during rendering"
// assertion.

namespace helix::sensors {

// ============================================================================
// Singleton
// ============================================================================

WidthSensorManager& WidthSensorManager::instance() {
    static WidthSensorManager instance;
    return instance;
}

WidthSensorManager::WidthSensorManager() = default;

WidthSensorManager::~WidthSensorManager() = default;

// ============================================================================
// ISensorManager Interface
// ============================================================================

std::string WidthSensorManager::category_name() const {
    return "width";
}

void WidthSensorManager::discover(const std::vector<std::string>& klipper_objects) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[WidthSensorManager] Discovering width sensors from {} objects",
                  klipper_objects.size());

    // Clear existing sensors
    sensors_.clear();

    for (const auto& klipper_name : klipper_objects) {
        std::string sensor_name;
        WidthSensorType type = WidthSensorType::TSL1401CL;

        if (!parse_klipper_name(klipper_name, sensor_name, type)) {
            continue;
        }

        WidthSensorConfig config(klipper_name, sensor_name, type);
        sensors_.push_back(config);

        // Initialize state if not already present
        if (states_.find(klipper_name) == states_.end()) {
            WidthSensorState state;
            state.available = true;
            states_[klipper_name] = state;
        } else {
            states_[klipper_name].available = true;
        }

        spdlog::debug("[WidthSensorManager] Discovered sensor: {} (type: {})", sensor_name,
                      width_type_to_string(type));
    }

    // Mark sensors that disappeared as unavailable
    for (auto& [name, state] : states_) {
        bool found = false;
        for (const auto& sensor : sensors_) {
            if (sensor.klipper_name == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            state.available = false;
        }
    }

    // Remove stale entries to prevent unbounded memory growth
    for (auto it = states_.begin(); it != states_.end();) {
        if (!it->second.available) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }

    // Auto-assign first discovered sensor to FLOW_COMPENSATION role as a default.
    // This ensures the diameter subject gets populated for first-time users.
    // User-saved config from load_config_from_file() is applied AFTER discover()
    // and will override this default if the user explicitly set a different role.
    if (!sensors_.empty()) {
        bool has_flow_compensation = false;
        for (const auto& sensor : sensors_) {
            if (sensor.role == WidthSensorRole::FLOW_COMPENSATION) {
                has_flow_compensation = true;
                break;
            }
        }
        if (!has_flow_compensation) {
            sensors_.front().role = WidthSensorRole::FLOW_COMPENSATION;
            spdlog::debug(
                "[WidthSensorManager] Auto-assigned {} to FLOW_COMPENSATION role (default)",
                sensors_.front().sensor_name);
        }
    }

    // Update sensor count subject
    if (subjects_initialized_) {
        lv_subject_set_int(&sensor_count_, static_cast<int>(sensors_.size()));
    }

    spdlog::info("[WidthSensorManager] Discovered {} width sensors", sensors_.size());

    // Update subjects to reflect new state
    update_subjects();
}

void WidthSensorManager::update_from_status(const nlohmann::json& status) {
    bool any_changed = false;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        for (const auto& sensor : sensors_) {
            const std::string& key = sensor.klipper_name;

            if (!status.contains(key)) {
                continue;
            }

            const auto& sensor_data = status[key];
            auto& state = states_[sensor.klipper_name];
            WidthSensorState old_state = state;

            // Update diameter
            if (sensor_data.contains("Diameter")) {
                state.diameter = sensor_data["Diameter"].get<float>();
            }

            // Update raw value
            if (sensor_data.contains("Raw")) {
                state.raw_value = sensor_data["Raw"].get<float>();
            }

            // Check for state change
            if (state.diameter != old_state.diameter || state.raw_value != old_state.raw_value) {
                any_changed = true;
                spdlog::debug("[WidthSensorManager] Sensor {} updated: diameter={:.3f}mm, raw={}",
                              sensor.sensor_name, state.diameter, state.raw_value);
            }
        }

        if (any_changed) {
            if (sync_mode_) {
                spdlog::debug("[WidthSensorManager] sync_mode: updating subjects synchronously");
                update_subjects();
            } else {
                spdlog::debug("[WidthSensorManager] async_mode: deferring via ui_queue_update");
                helix::ui::queue_update(
                    [] { WidthSensorManager::instance().update_subjects_on_main_thread(); });
            }
        }
    }
}

void WidthSensorManager::inject_mock_sensors(std::vector<std::string>& objects,
                                             nlohmann::json& /*config_keys*/,
                                             nlohmann::json& /*moonraker_info*/) {
    // Width sensors are discovered from Klipper objects
    objects.emplace_back("hall_filament_width_sensor");
    spdlog::debug("[WidthSensorManager] Injected mock sensors: hall_filament_width_sensor");
}

void WidthSensorManager::inject_mock_status(nlohmann::json& status) {
    // Width sensor reports Raw value, Diameter, and is_active state
    status["hall_filament_width_sensor"] = {
        {"Raw", 500.0f}, {"Diameter", 1.75f}, {"is_active", true}};
}

void WidthSensorManager::load_config(const nlohmann::json& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[WidthSensorManager] Loading config");

    if (!config.contains("sensors") || !config["sensors"].is_array()) {
        spdlog::debug("[WidthSensorManager] No sensors config found");
        return;
    }

    for (const auto& sensor_json : config["sensors"]) {
        if (!sensor_json.contains("klipper_name")) {
            continue;
        }

        std::string klipper_name = sensor_json["klipper_name"].get<std::string>();
        auto* sensor = find_config(klipper_name);

        if (sensor) {
            if (sensor_json.contains("role")) {
                sensor->role = width_role_from_string(sensor_json["role"].get<std::string>());
            }
            if (sensor_json.contains("enabled")) {
                sensor->enabled = sensor_json["enabled"].get<bool>();
            }
            spdlog::debug("[WidthSensorManager] Loaded config for {}: role={}, enabled={}",
                          klipper_name, width_role_to_string(sensor->role), sensor->enabled);
        }
    }

    update_subjects();
    spdlog::info("[WidthSensorManager] Config loaded");
}

nlohmann::json WidthSensorManager::save_config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[WidthSensorManager] Saving config");

    nlohmann::json config;
    nlohmann::json sensors_array = nlohmann::json::array();

    for (const auto& sensor : sensors_) {
        nlohmann::json sensor_json;
        sensor_json["klipper_name"] = sensor.klipper_name;
        sensor_json["role"] = width_role_to_string(sensor.role);
        sensor_json["enabled"] = sensor.enabled;
        sensor_json["type"] = width_type_to_string(sensor.type);
        sensors_array.push_back(sensor_json);
    }

    config["sensors"] = sensors_array;

    spdlog::info("[WidthSensorManager] Config saved");
    return config;
}

void WidthSensorManager::load_config_from_file() {
    spdlog::debug("[WidthSensorManager] Loading config from file");

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[WidthSensorManager] Config not initialized");
        return;
    }

    // Reuse load_config() to avoid deserialization drift (mirrors save_config_to_file)
    std::string base_path = config->df() + "width_sensors";
    try {
        nlohmann::json& config_json = config->get_json(base_path);
        load_config(config_json);
    } catch (const std::exception& e) {
        spdlog::debug("[WidthSensorManager] No saved config found: {}", e.what());
    }

    spdlog::info("[WidthSensorManager] Config loaded from file");
}

void WidthSensorManager::save_config_to_file() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[WidthSensorManager] Saving config to file");

    Config* config = Config::get_instance();
    if (!config) {
        spdlog::warn("[WidthSensorManager] Config not initialized");
        return;
    }

    // Build path using default printer prefix
    std::string base_path = config->df() + "width_sensors";

    // Reuse save_config() to avoid serialization drift
    config->get_json(base_path) = save_config();
    config->save();

    spdlog::info("[WidthSensorManager] Config saved to file");
}

// ============================================================================
// Initialization
// ============================================================================

void WidthSensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    spdlog::trace("[WidthSensorManager] Initializing subjects");

    // Initialize subjects with SubjectManager for automatic cleanup
    // -1 = no sensor assigned, 0+ = diameter in mm * 1000
    UI_MANAGED_SUBJECT_INT(diameter_, -1, "filament_width_diameter", subjects_);
    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "width_sensor_count", subjects_);
    // Text subject for display (formatted as "1.75mm" or "--")
    UI_MANAGED_SUBJECT_STRING(diameter_text_, diameter_text_buf_, "--", "filament_diameter_text",
                              subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "WidthSensorManager", []() { WidthSensorManager::instance().deinit_subjects(); });

    spdlog::trace("[WidthSensorManager] Subjects initialized");
}

void WidthSensorManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[WidthSensorManager] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[WidthSensorManager] Subjects deinitialized");
}

// ============================================================================
// Sensor Queries
// ============================================================================

bool WidthSensorManager::has_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !sensors_.empty();
}

std::vector<WidthSensorConfig> WidthSensorManager::get_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_; // Return thread-safe copy
}

size_t WidthSensorManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.size();
}

// ============================================================================
// Configuration
// ============================================================================

void WidthSensorManager::set_sensor_role(const std::string& klipper_name, WidthSensorRole role) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // If assigning a role, clear it from any other sensor first
    if (role != WidthSensorRole::NONE) {
        for (auto& sensor : sensors_) {
            if (sensor.role == role && sensor.klipper_name != klipper_name) {
                spdlog::debug("[WidthSensorManager] Clearing role {} from {}",
                              width_role_to_string(role), sensor.sensor_name);
                sensor.role = WidthSensorRole::NONE;
            }
        }
    }

    auto* sensor = find_config(klipper_name);
    if (sensor) {
        sensor->role = role;
        spdlog::info("[WidthSensorManager] Set role for {} to {}", sensor->sensor_name,
                     width_role_to_string(role));
        update_subjects();
    }
}

void WidthSensorManager::set_sensor_enabled(const std::string& klipper_name, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* sensor = find_config(klipper_name);
    if (sensor) {
        sensor->enabled = enabled;
        spdlog::info("[WidthSensorManager] Set enabled for {} to {}", sensor->sensor_name, enabled);
        update_subjects();
    }
}

// ============================================================================
// State Queries
// ============================================================================

std::optional<WidthSensorState> WidthSensorManager::get_sensor_state(WidthSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (role == WidthSensorRole::NONE) {
        return std::nullopt;
    }

    const auto* config = find_config_by_role(role);
    if (!config) {
        return std::nullopt;
    }

    auto it = states_.find(config->klipper_name);
    if (it == states_.end()) {
        return std::nullopt;
    }

    return it->second; // Return thread-safe copy
}

bool WidthSensorManager::is_sensor_available(WidthSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (role == WidthSensorRole::NONE) {
        return false;
    }

    const auto* config = find_config_by_role(role);
    if (!config || !config->enabled) {
        return false;
    }

    auto it = states_.find(config->klipper_name);
    return it != states_.end() && it->second.available;
}

float WidthSensorManager::get_flow_compensation_diameter() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    const auto* config = find_config_by_role(WidthSensorRole::FLOW_COMPENSATION);
    if (!config || !config->enabled) {
        return 0.0f;
    }

    auto it = states_.find(config->klipper_name);
    if (it == states_.end() || !it->second.available) {
        return 0.0f;
    }

    return it->second.diameter;
}

// ============================================================================
// LVGL Subjects
// ============================================================================

lv_subject_t* WidthSensorManager::get_diameter_subject() {
    return &diameter_;
}

lv_subject_t* WidthSensorManager::get_sensor_count_subject() {
    return &sensor_count_;
}

lv_subject_t* WidthSensorManager::get_diameter_text_subject() {
    return &diameter_text_;
}

// ============================================================================
// Testing Support
// ============================================================================

void WidthSensorManager::set_sync_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sync_mode_ = enabled;
}

void WidthSensorManager::update_subjects_on_main_thread() {
    update_subjects();
}

// ============================================================================
// Private Helpers
// ============================================================================

bool WidthSensorManager::parse_klipper_name(const std::string& klipper_name,
                                            std::string& sensor_name, WidthSensorType& type) const {
    const std::string tsl_name = "tsl1401cl_filament_width_sensor";
    const std::string hall_name = "hall_filament_width_sensor";
    const std::string hall_alias = "hall_fila_switch";

    if (klipper_name == tsl_name) {
        sensor_name = "tsl1401cl";
        type = WidthSensorType::TSL1401CL;
        return true;
    }

    if (klipper_name == hall_name || klipper_name == hall_alias) {
        sensor_name = "hall";
        type = WidthSensorType::HALL;
        return true;
    }

    return false;
}

WidthSensorConfig* WidthSensorManager::find_config(const std::string& klipper_name) {
    for (auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const WidthSensorConfig* WidthSensorManager::find_config(const std::string& klipper_name) const {
    for (const auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const WidthSensorConfig* WidthSensorManager::find_config_by_role(WidthSensorRole role) const {
    for (const auto& sensor : sensors_) {
        if (sensor.role == role) {
            return &sensor;
        }
    }
    return nullptr;
}

void WidthSensorManager::update_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    // Get diameter value for flow compensation role
    auto get_diameter_value = [this]() -> int {
        const auto* config = find_config_by_role(WidthSensorRole::FLOW_COMPENSATION);
        if (!config || !config->enabled) {
            return -1; // No sensor assigned or disabled
        }

        auto it = states_.find(config->klipper_name);
        if (it == states_.end() || !it->second.available) {
            return -1; // Sensor unavailable
        }

        // Convert diameter to int (mm * 1000)
        return static_cast<int>(it->second.diameter * 1000.0f);
    };

    int diameter = get_diameter_value();
    lv_subject_set_int(&diameter_, diameter);

    // Text formatting (diameter_text_) handled by UI-layer observer in WidthSensorWidget

    spdlog::trace("[WidthSensorManager] Subjects updated: diameter={}", diameter);
}

} // namespace helix::sensors
