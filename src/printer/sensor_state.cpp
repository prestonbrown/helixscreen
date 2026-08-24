// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file sensor_state.cpp
 * @brief Moonraker server.sensors state tracking via WebSocket events
 *
 * Maintains per-sensor, per-value LVGL subjects encoded as centi-units.
 * Current uses *100000 (centi-milliamps) for mA precision; all others use *100.
 */

#include "sensor_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {

SensorState& SensorState::instance() {
    static SensorState s;
    return s;
}

int SensorState::to_centi_units(const std::string& key, double value) {
    if (key == "current") {
        // Current in amps → centi-milliamps (value * 1000 * 100) for mA precision
        return static_cast<int>(std::round(value * 100000.0));
    }
    // power, voltage, energy → value * 100
    return static_cast<int>(std::round(value * 100.0));
}

std::string SensorState::format_value(const std::string& key, int centi_value) {
    if (key == "power") {
        double watts = centi_value / 100.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f W", watts);
        return buf;
    }
    if (key == "voltage") {
        // Voltage displayed as integer (no decimal)
        int volts = centi_value / 100;
        return std::to_string(volts) + " V";
    }
    if (key == "current") {
        // centi_value is in centi-milliamps (value_amps * 100000)
        double amps = centi_value / 100000.0;
        char buf[32];
        if (amps < 1.0) {
            double milliamps = amps * 1000.0;
            std::snprintf(buf, sizeof(buf), "%.1f mA", milliamps);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f A", amps);
        }
        return buf;
    }
    if (key == "energy") {
        double kwh = centi_value / 100.0;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f kWh", kwh);
        return buf;
    }
    // Unknown key — format as generic centi-unit
    double val = centi_value / 100.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", val);
    return buf;
}

bool SensorState::is_energy_sensor(const SensorInfo& info) {
    static const std::vector<std::string> energy_keys = {"power", "voltage", "current", "energy"};
    for (const auto& key : info.value_keys) {
        if (std::find(energy_keys.begin(), energy_keys.end(), key) != energy_keys.end()) {
            return true;
        }
    }
    return false;
}

void SensorState::set_sensors(const std::vector<SensorInfo>& sensors) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Tear down existing subjects (freeze queue to prevent new callbacks during teardown).
    // Do NOT drain() here — it processes arbitrary queued callbacks re-entrantly from
    // within process_pending(), which can trigger UI rebuilds (e.g. NozzleTempsWidget
    // rebuild_rows) while subjects are in a half-torn-down state (#746). The
    // SubjectLifetime mechanism and weak_alive tokens in deferred lambdas already
    // ensure stale callbacks safely no-op after subjects are freed.
    if (subjects_initialized_) {
        lifetime_.invalidate();
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        for (auto& [id, entry] : sensors_) {
            for (auto& [key, subj] : entry.value_subjects) {
                if (subj->lifetime)
                    *subj->lifetime = false; // Signal death (#816)
                subj->lifetime.reset();
            }
        }
        for (auto& [id, entry] : sensors_) {
            for (auto& [key, subj] : entry.value_subjects) {
                if (subj->initialized) {
                    lv_subject_deinit(&subj->subject);
                    subj->initialized = false;
                }
            }
        }
        sensors_.clear();
    }

    // Create new sensor entries with subjects
    for (const auto& info : sensors) {
        SensorEntry entry;
        entry.info = info;

        for (const auto& key : info.value_keys) {
            auto dyn = std::make_unique<DynamicIntSubject>();
            lv_subject_init_int(&dyn->subject, 0);
            dyn->initialized = true;
            dyn->lifetime = std::make_shared<bool>(true);
            entry.value_subjects.emplace(key, std::move(dyn));
        }

        spdlog::trace("[SensorState] Created subjects for '{}': {} keys", info.id,
                      info.value_keys.size());

        sensors_.emplace(info.id, std::move(entry));
    }

    // Register with StaticSubjectRegistry on first init
    if (!subjects_initialized_) {
        subjects_initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit(
            "SensorState", []() { SensorState::instance().deinit_subjects(); });
    }

    spdlog::info("[SensorState] Initialized {} sensors", sensors.size());
}

void SensorState::set_sensors(const std::vector<SensorInfo>& sensors,
                              const nlohmann::json& initial_values) {
    // Create subjects first
    set_sensors(sensors);

    // Apply initial values atomically (already on UI thread, subjects just created)
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto it = initial_values.begin(); it != initial_values.end(); ++it) {
        const std::string& sensor_id = it.key();
        auto sit = sensors_.find(sensor_id);
        if (sit == sensors_.end())
            continue;

        for (auto vit = it.value().begin(); vit != it.value().end(); ++vit) {
            if (!vit.value().is_number())
                continue;

            auto kit = sit->second.value_subjects.find(vit.key());
            if (kit == sit->second.value_subjects.end() || !kit->second->initialized)
                continue;

            double raw = vit.value().get<double>();
            int centi = to_centi_units(vit.key(), raw);
            lv_subject_set_int(&kit->second->subject, centi);
            spdlog::trace("[SensorState] Initial value {}.{} = {}", sensor_id, vit.key(), centi);
        }
    }
}

lv_subject_t* SensorState::get_value_subject(const std::string& sensor_id, const std::string& key,
                                             SubjectLifetime& lt) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto sit = sensors_.find(sensor_id);
    if (sit == sensors_.end()) {
        lt.reset();
        return nullptr;
    }

    auto kit = sit->second.value_subjects.find(key);
    if (kit == sit->second.value_subjects.end() || !kit->second->initialized) {
        lt.reset();
        return nullptr;
    }

    lt = kit->second->lifetime;
    return &kit->second->subject;
}

std::vector<std::string> SensorState::sensor_ids() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::vector<std::string> ids;
    ids.reserve(sensors_.size());
    for (const auto& [id, entry] : sensors_) {
        ids.push_back(id);
    }
    return ids;
}

const SensorInfo* SensorState::get_sensor_info(const std::string& sensor_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = sensors_.find(sensor_id);
    if (it != sensors_.end()) {
        return &it->second.info;
    }
    return nullptr;
}

std::vector<std::string> SensorState::energy_sensor_ids() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::vector<std::string> ids;
    for (const auto& [id, entry] : sensors_) {
        if (is_energy_sensor(entry.info)) {
            ids.push_back(id);
        }
    }
    return ids;
}

void SensorState::subscribe(IMoonrakerAPI& api) {
    api.register_method_callback("notify_sensor_update", "sensor_state",
                                 [this](const nlohmann::json& msg) { on_sensor_update(msg); });
    spdlog::debug("[SensorState] Subscribed to notify_sensor_update");
}

void SensorState::unsubscribe(IMoonrakerAPI& api) {
    api.unregister_method_callback("notify_sensor_update", "sensor_state");
    deinit_subjects();
    spdlog::debug("[SensorState] Unsubscribed and cleaned up");
}

void SensorState::on_sensor_update(const nlohmann::json& msg) {
    // Moonraker sends: {"method": "notify_sensor_update", "params": [{"sensor_id": {"key": value,
    // ...}, ...}]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const auto& params = msg["params"][0];
    if (!params.is_object()) {
        return;
    }

    auto tok = lifetime_.token();

    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string& sensor_id = it.key();
        const auto& values = it.value();

        if (!values.is_object()) {
            continue;
        }

        for (auto vit = values.begin(); vit != values.end(); ++vit) {
            const std::string& key = vit.key();
            if (!vit.value().is_number()) {
                continue;
            }

            double raw_value = vit.value().get<double>();
            int centi = to_centi_units(key, raw_value);

            tok.defer("SensorState::on_sensor_update", [this, sensor_id, key, centi]() {
                std::lock_guard<std::recursive_mutex> lock(mutex_);

                auto sit = sensors_.find(sensor_id);
                if (sit == sensors_.end()) {
                    return;
                }

                auto kit = sit->second.value_subjects.find(key);
                if (kit == sit->second.value_subjects.end() || !kit->second->initialized) {
                    return;
                }

                if (lv_subject_get_int(&kit->second->subject) != centi) {
                    lv_subject_set_int(&kit->second->subject, centi);
                    spdlog::trace("[SensorState] Updated {}.{} = {}", sensor_id, key, centi);
                }
            });
        }
    }
}

void SensorState::deinit_subjects() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[SensorState] Deinitializing subjects");

    lifetime_.invalidate();

    // Freeze queue to prevent new callbacks between drain and subject destruction
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    // Phase 1: signal death then expire lifetime tokens (#816)
    for (auto& [id, entry] : sensors_) {
        for (auto& [key, subj] : entry.value_subjects) {
            if (subj->lifetime)
                *subj->lifetime = false;
            subj->lifetime.reset();
        }
    }

    // Phase 2: deinit subjects and clear map
    for (auto& [id, entry] : sensors_) {
        for (auto& [key, subj] : entry.value_subjects) {
            if (subj->initialized) {
                lv_subject_deinit(&subj->subject);
                subj->initialized = false;
            }
        }
    }
    sensors_.clear();

    subjects_initialized_ = false;
}

} // namespace helix
