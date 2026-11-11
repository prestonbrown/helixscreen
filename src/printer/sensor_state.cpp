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

#include "hv/json.hpp"

namespace helix {

SensorState::Options SensorState::make_options() {
    Options o;
    o.name = "SensorState";
    o.count_subject = "sensor_state_count";
    o.revision_subject = "sensor_state_revision";
    // Fed by server.sensors, not by PrinterDiscovery.
    o.from_printer_discovery = false;
    return o;
}

SensorState::SensorState() : SensorManager(make_options()) {}

SensorState& SensorState::instance() {
    static SensorState s;
    return s;
}

std::string SensorState::subject_key(const std::string& sensor_id, const std::string& key) {
    // Unit separator: cannot occur in a Moonraker sensor id or value key.
    return sensor_id + '\x1f' + key;
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
    set_sensors(sensors, nlohmann::json::object());
}

void SensorState::set_sensors(const std::vector<SensorInfo>& sensors,
                              const nlohmann::json& initial_values) {
    // The subjects must exist before they are filled; this is the first
    // moment SensorState has anything to show.
    init_subjects();

    std::vector<sensors::SensorEntry> entries;
    entries.reserve(sensors.size());
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        infos_.clear();
        for (const auto& info : sensors) {
            sensors::SensorEntry entry;
            entry.klipper_name = info.id;
            entry.display_name = info.friendly_name;
            entry.available = true;
            entries.push_back(std::move(entry));
            infos_[info.id] = info;
        }
        for (auto it = readings_.begin(); it != readings_.end();) {
            it = infos_.count(it->first) ? std::next(it) : readings_.erase(it);
        }
        for (auto it = initial_values.begin(); it != initial_values.end(); ++it) {
            if (!infos_.count(it.key()) || !it.value().is_object()) {
                continue;
            }
            for (auto vit = it.value().begin(); vit != it.value().end(); ++vit) {
                if (vit.value().is_number()) {
                    readings_[it.key()][vit.key()] =
                        to_centi_units(vit.key(), vit.value().get<double>());
                }
            }
        }
        sync_subjects_locked();
    }
    reconcile(std::move(entries));

    // Initial values land now rather than on the deferred refresh, so a widget
    // built right after discovery already reads them.
    refresh_subjects();

    spdlog::info("[SensorState] Initialized {} sensors", sensors.size());
}

void SensorState::sync_subjects_locked() {
    std::vector<std::string> keep;
    for (const auto& [id, info] : infos_) {
        for (const auto& key : info.value_keys) {
            keep.push_back(subject_key(id, key));
        }
    }
    subjects_by_key_.remove_missing(keep);
    for (const auto& key : keep) {
        SubjectLifetime unused;
        subjects_by_key_.get_or_create(key, 0, unused);
    }
}

void SensorState::register_subjects(SubjectManager& /*subjects*/) {}

bool SensorState::parse_status(const nlohmann::json& /*status*/) {
    // server.sensors arrives as notify_sensor_update, not in a status frame.
    return false;
}

void SensorState::refresh_subjects() {
    struct Reading {
        std::string key;
        int centi;
    };
    std::vector<Reading> readings;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        sync_subjects_locked();
        for (const auto& [id, values] : readings_) {
            for (const auto& [key, centi] : values) {
                readings.push_back({subject_key(id, key), centi});
            }
        }
    }
    for (const auto& r : readings) {
        SubjectLifetime unused;
        lv_subject_t* subject = subjects_by_key_.find(r.key, unused);
        if (subject != nullptr && lv_subject_get_int(subject) != r.centi) {
            lv_subject_set_int(subject, r.centi);
        }
    }
}

void SensorState::on_deinit_subjects() {
    subjects_by_key_.clear();
    infos_.clear();
    readings_.clear();
    entries_locked().clear();
}

lv_subject_t* SensorState::get_value_subject(const std::string& sensor_id, const std::string& key,
                                             SubjectLifetime& lt) {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    lv_subject_t* subject = subjects_by_key_.find(subject_key(sensor_id, key), lt);
    if (subject == nullptr) {
        lt.reset();
    }
    return subject;
}

std::vector<std::string> SensorState::sensor_ids() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    std::vector<std::string> ids;
    ids.reserve(entries_locked().size());
    for (const auto& entry : entries_locked()) {
        ids.push_back(entry.klipper_name);
    }
    return ids;
}

std::optional<SensorInfo> SensorState::get_sensor_info(const std::string& sensor_id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    auto it = infos_.find(sensor_id);
    return it != infos_.end() ? std::optional<SensorInfo>(it->second) : std::nullopt;
}

std::vector<std::string> SensorState::energy_sensor_ids() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    std::vector<std::string> ids;
    for (const auto& entry : entries_locked()) {
        auto it = infos_.find(entry.klipper_name);
        if (it != infos_.end() && is_energy_sensor(it->second)) {
            ids.push_back(entry.klipper_name);
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

    bool changed = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (!it.value().is_object() || !infos_.count(it.key())) {
                continue;
            }
            for (auto vit = it.value().begin(); vit != it.value().end(); ++vit) {
                if (!vit.value().is_number()) {
                    continue;
                }
                int centi = to_centi_units(vit.key(), vit.value().get<double>());
                auto& slot = readings_[it.key()];
                auto cur = slot.find(vit.key());
                if (cur == slot.end() || cur->second != centi) {
                    slot[vit.key()] = centi;
                    changed = true;
                }
            }
        }
    }
    // Subjects are written on the main thread by refresh_subjects(); a refresh
    // queued before deinit_subjects() is dropped.
    if (changed) {
        publish();
    }
}

} // namespace helix
