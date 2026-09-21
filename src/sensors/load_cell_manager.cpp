// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "load_cell_manager.h"

#include "ui_update_queue.h"

#include "ams_state.h"
#include "device_display_name.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"
#include "unit_conversions.h"

#include <algorithm>
#include <optional>

// CRITICAL: Subject updates trigger lv_obj_invalidate() which asserts if called
// during LVGL rendering. WebSocket callbacks run on libhv's event loop thread,
// not the main LVGL thread. We must defer subject updates to the main thread
// via ui_queue_update() to avoid the "Invalidate area not allowed during rendering"
// assertion.

namespace helix::sensors {

// ============================================================================
// Singleton
// ============================================================================

LoadCellManager& LoadCellManager::instance() {
    static LoadCellManager instance;
    return instance;
}

LoadCellManager::LoadCellManager() = default;

LoadCellManager::~LoadCellManager() = default;

// ============================================================================
// ISensorManager Interface
// ============================================================================

std::string LoadCellManager::category_name() const {
    return "load_cell";
}

void LoadCellManager::discover(const std::vector<std::string>& klipper_objects) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[LoadCellManager] Discovering load cells from {} objects",
                  klipper_objects.size());

    // Clear existing sensors
    sensors_.clear();

    for (const auto& klipper_name : klipper_objects) {
        const auto sensor_name = parse_klipper_name(klipper_name);
        if (!sensor_name) {
            continue;
        }

        // Generate display name
        std::string display_name = helix::get_display_name(*sensor_name, DeviceType::LOAD_CELL);

        LoadCellConfig config(klipper_name, *sensor_name, display_name);

        // Auto-categorize based on sensor name, or whether it is the only load cell.
        if (sensor_name->find("spool") != std::string::npos ||
            (*sensor_name == "" && klipper_objects.size() == 1)) {
            config.role = LoadCellRole::SPOOL_WEIGHT;
            config.priority = 0;
        } else {
            config.role = LoadCellRole::NONE;
            config.priority = 100;
        }

        sensors_.push_back(config);

        // Initialize state if not already present
        if (states_.find(klipper_name) == states_.end()) {
            LoadCellState state;
            state.available = true;
            states_[klipper_name] = state;
        } else {
            states_[klipper_name].available = true;
        }

        spdlog::debug("[LoadCellManager] Discovered sensor: {} (role: {}, priority: {})",
                      *sensor_name, load_cell_role_to_string(config.role), config.priority);
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

    // Update sensor count subject
    if (subjects_initialized_) {
        lv_subject_set_int(&sensor_count_, static_cast<int>(sensors_.size()));
    }

    spdlog::debug("[LoadCellManager] Discovered {} load cells", sensors_.size());

    // Update subjects to reflect new state
    update_subjects();
}

void LoadCellManager::update_from_status(const nlohmann::json& status) {
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
            LoadCellState old_state = state;

            // Field-restricted Moonraker subscriptions send null for fields the
            // underlying object lacks. Use find() + is_number() so unexpected
            // nulls (or wrong types during firmware restarts) don't throw
            // type_error.302 and crash the process.
            if (auto it = sensor_data.find("force_g"); it != sensor_data.end() && it->is_number()) {
                state.force_g = it->get<float>();
            }

            // Check for state change
            if (state.force_g != old_state.force_g) {
                any_changed = true;
                spdlog::trace("[LoadCellManager] Load cell {} updated: force_g={:.1f}",
                              sensor.sensor_name, state.force_g);
            }
        }

        if (any_changed) {
            if (sync_mode_) {
                spdlog::trace("[LoadCellManager] sync_mode: updating subjects synchronously");
                update_subjects();
            } else {
                spdlog::trace("[LoadCellManager] async_mode: deferring via ui_queue_update");
                auto tok = lifetime_.token();
                tok.defer("LoadCellManager::update_subjects",
                          [] { LoadCellManager::instance().update_subjects_on_main_thread(); });
            }
        }
    }
}

void LoadCellManager::load_config(const nlohmann::json& config) {
    (void)config;

    // Unimplemented.
}

nlohmann::json LoadCellManager::save_config() const {
    // Unimplemented.

    return nlohmann::json{};
}

// ============================================================================
// Initialization
// ============================================================================

void LoadCellManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    spdlog::trace("[LoadCellManager] Initializing subjects");

    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "load_cell_count", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "LoadCellManager", []() { LoadCellManager::instance().deinit_subjects(); });

    spdlog::trace("[LoadCellManager] Subjects initialized");
}

void LoadCellManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[LoadCellManager] Deinitializing subjects");

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        // Invalidate lifetime guard FIRST — expires tokens in queued callbacks
        // so they won't access state after we clear it (L072, L054)
        lifetime_.invalidate();

        // Clear all collections under mutex to prevent background thread access
        // to stale iterators during shutdown race
        sensors_.clear();
        states_.clear();

        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    spdlog::trace("[LoadCellManager] Subjects deinitialized");
}

// ============================================================================
// Sensor Queries
// ============================================================================

bool LoadCellManager::has_spool_weight_load_cell() const {
    if (find_config_by_role(LoadCellRole::SPOOL_WEIGHT)) {
        return true;
    }

    return false;
}

std::vector<LoadCellConfig> LoadCellManager::get_sensors_sorted() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto sorted = sensors_;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.display_name < b.display_name;
    });

    return sorted;
}

size_t LoadCellManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.size();
}

// ============================================================================
// LVGL Subjects
// ============================================================================

lv_subject_t* LoadCellManager::get_sensor_count_subject() {
    return &sensor_count_;
}

// ============================================================================
// Testing Support
// ============================================================================

void LoadCellManager::set_sync_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sync_mode_ = enabled;
}

void LoadCellManager::update_subjects_on_main_thread() {
    update_subjects();
}

// ============================================================================
// Private Helpers
// ============================================================================

const std::optional<const std::string>
LoadCellManager::parse_klipper_name(const std::string& klipper_name) const {
    const std::string load_cell_prefix = "load_cell ";

    if (klipper_name.rfind(load_cell_prefix, 0) == 0) {
        const auto& sensor_name = klipper_name.substr(load_cell_prefix.length());
        return std::make_optional(sensor_name);
    } else if (klipper_name == "load_cell") {
        return std::make_optional("");
    }

    return std::nullopt;
}

LoadCellConfig* LoadCellManager::find_config(const std::string& klipper_name) {
    for (auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const LoadCellConfig* LoadCellManager::find_config(const std::string& klipper_name) const {
    for (const auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const LoadCellConfig* LoadCellManager::find_config_by_role(LoadCellRole role) const {
    for (const auto& load_cell : sensors_) {
        if (load_cell.role == role) {
            return &load_cell;
        }
    }
    return nullptr;
}

void LoadCellManager::update_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    std::optional<float> spool_weight = std::nullopt;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);

        const auto& spool_weight_config = find_config_by_role(LoadCellRole::SPOOL_WEIGHT);
        if (spool_weight_config) {
            auto it = states_.find(spool_weight_config->klipper_name);
            if (it == states_.end() || !it->second.available) {
                // Sensor transiently unavailable.
            } else {
                spool_weight = std::make_optional(it->second.force_g);
            }
        }
    }

    if (spool_weight && *spool_weight >= 0) {
        auto& ams_state = AmsState::instance();
        auto spool_info = ams_state.raw_external_spool_info().value_or(SlotInfo{});
        // Only update the value if it changed significantly to
        // avoid frequent UI redraws due to measurement noise.
        if (spool_info.remaining_weight_g == -1 ||
            (std::abs(spool_info.remaining_weight_g - *spool_weight) > 0.5)) {
            spool_info.remaining_weight_g = *spool_weight;
            ams_state.set_external_spool_info_in_memory(spool_info);

            spdlog::debug("[LoadCellManager] Publishing updated spool weight to AmsState: {} g",
                          *spool_weight);
        }
    }
}

} // namespace helix::sensors
