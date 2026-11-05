// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "load_cell_manager.h"

#include "ams_state.h"
#include "device_display_name.h"
#include "lane_source_store.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <optional>

#include "hv/json.hpp"

namespace helix::sensors {

namespace {

constexpr uint8_t kSpoolWeight = static_cast<uint8_t>(LoadCellRole::SPOOL_WEIGHT);

} // namespace

LoadCellManager::Options LoadCellManager::make_options() {
    Options o;
    o.name = "LoadCellManager";
    o.kinds = {hw::ObjectKind::LoadCell};
    o.count_subject = "load_cell_count";
    o.revision_subject = "load_cell_revision";
    o.settings_section = "load_cell";
    o.row_toggle = RowToggle::None;
    o.roles.push_back({kSpoolWeight, "spool_weight", "Spool Weight", /*exclusive=*/true,
                       /*offered=*/false});
    return o;
}

SensorRowView LoadCellManager::row(size_t index) const {
    SensorRowView view = SensorManager::row(index);
    const auto all = entries();
    if (index < all.size()) {
        view.name = helix::get_display_name(all[index].klipper_name, DeviceType::LOAD_CELL);
    }
    return view;
}

LoadCellManager& LoadCellManager::instance() {
    static LoadCellManager instance;
    return instance;
}

LoadCellManager::LoadCellManager() : SensorManager(make_options()) {}

void LoadCellManager::register_subjects(SubjectManager& /*subjects*/) {}

const SensorEntry* LoadCellManager::spool_weight_entry_locked() const {
    for (const auto& entry : entries_locked()) {
        if (entry.role == kSpoolWeight && entry.available) {
            return &entry;
        }
    }
    return nullptr;
}

bool LoadCellManager::has_spool_weight_load_cell() const {
    std::lock_guard<std::recursive_mutex> lock(mutex());
    return spool_weight_entry_locked() != nullptr;
}

std::vector<SensorEntry> LoadCellManager::get_sensors_sorted() const {
    auto sorted = entries();
    const auto shown = [](const SensorEntry& e) {
        return helix::get_display_name(e.klipper_name, DeviceType::LOAD_CELL);
    };
    std::sort(sorted.begin(), sorted.end(), [&shown](const auto& a, const auto& b) {
        const bool a_spool = a.role == kSpoolWeight;
        const bool b_spool = b.role == kSpoolWeight;
        if (a_spool != b_spool) {
            return a_spool;
        }
        return shown(a) < shown(b);
    });
    return sorted;
}

bool LoadCellManager::parse_status(const nlohmann::json& status) {
    bool changed = false;
    for (const auto& entry : entries_locked()) {
        const auto sensor = status.find(entry.klipper_name);
        if (sensor == status.end() || !sensor->is_object()) {
            continue;
        }
        // Field-restricted subscriptions send null for fields the object lacks.
        const auto force = sensor->find("force_g");
        if (force == sensor->end() || !force->is_number()) {
            continue;
        }
        LoadCellState& state = readings_[entry.klipper_name];
        const float value = force->get<float>();
        if (state.force_g != value) {
            state.force_g = value;
            changed = true;
            spdlog::trace("[LoadCellManager] {} force_g={:.1f}", entry.klipper_name, value);
        }
    }
    return changed;
}

void LoadCellManager::on_entries_changed() {
    prune_vanished_locked(readings_);
}

void LoadCellManager::on_discovered(const PrinterDiscovery& /*hw*/) {
    auto& entries = entries_locked();
    const bool held = std::any_of(entries.begin(), entries.end(),
                                  [](const SensorEntry& e) { return e.role == kSpoolWeight; });
    if (held) {
        return;
    }
    // A cell named for a spool, or the only cell, weighs the external spool.
    const bool only_cell = entries.size() == 1;
    for (auto& entry : entries) {
        const auto instance = hw::classify_object(entry.klipper_name).instance;
        if (only_cell || instance.find("spool") != std::string::npos) {
            entry.role = kSpoolWeight;
            spdlog::debug("[LoadCellManager] {} defaults to SPOOL_WEIGHT", entry.klipper_name);
            return;
        }
    }
}

void LoadCellManager::refresh_subjects() {
    std::optional<float> spool_weight;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex());
        if (const auto* entry = spool_weight_entry_locked()) {
            const auto reading = readings_.find(entry->klipper_name);
            if (reading != readings_.end()) {
                spool_weight = reading->second.force_g;
            }
        }
    }

    if (!spool_weight || *spool_weight < 0) {
        return;
    }

    auto& ams_state = AmsState::instance();
    auto spool_info = ams_state.raw_external_spool_info().value_or(SlotInfo{});
    bool weight_changed = false;
    if (spool_info.remaining_weight_g == -1) {
        weight_changed = true;
    } else {
        // Only update the value if it changed significantly to avoid frequent UI
        // redraws due to measurement noise. Different deltas for increasing and
        // decreasing weight keep the value from flip-flopping.
        const auto delta = *spool_weight - spool_info.remaining_weight_g;
        weight_changed = delta <= -0.5 || delta >= 1.0;
    }
    if (!weight_changed) {
        return;
    }
    spool_info.remaining_weight_g = *spool_weight;
    ams_state.set_external_spool_info_in_memory(spool_info);

    // Views read the resolved record, where the bypass lane's sources outrank the
    // raw one; filing the reading as Metered is what lets it show over a stale
    // user edit.
    helix::ams::Observation measured(helix::ams::ObservationSource::Metered);
    measured.remaining_weight_g = *spool_weight;
    helix::ams::ingest(helix::ams::kBypassLaneId, measured);

    spdlog::debug("[LoadCellManager] Publishing updated spool weight to AmsState: {} g",
                  *spool_weight);
}

} // namespace helix::sensors
