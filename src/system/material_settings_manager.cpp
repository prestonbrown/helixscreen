// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "material_settings_manager.h"

#include "config.h"
#include "filament_catalog.h"

#include <spdlog/spdlog.h>

namespace helix {

MaterialSettingsManager& MaterialSettingsManager::instance() {
    static MaterialSettingsManager s_instance;
    return s_instance;
}

void MaterialSettingsManager::init() {
    if (initialized_) {
        return;
    }
    load_from_config();
    load_presets_from_config();
    initialized_ = true;
    spdlog::info("[MaterialSettingsManager] Initialized with {} override(s)", overrides_.size());
}

const filament::MaterialOverride*
MaterialSettingsManager::get_override(const std::string& name) const {
    auto it = overrides_.find(name);
    if (it != overrides_.end()) {
        return &it->second;
    }
    return nullptr;
}

void MaterialSettingsManager::set_override(const std::string& name,
                                           const filament::MaterialOverride& override) {
    overrides_[name] = override;
    save_to_config();
    spdlog::info("[MaterialSettingsManager] Set override for '{}'", name);
}

void MaterialSettingsManager::clear_override(const std::string& name) {
    if (overrides_.erase(name) > 0) {
        save_to_config();
        spdlog::info("[MaterialSettingsManager] Cleared override for '{}'", name);
    }
}

bool MaterialSettingsManager::has_override(const std::string& name) const {
    return overrides_.count(name) > 0;
}

void MaterialSettingsManager::load_from_config() {
    Config* config = Config::get_instance();
    if (!config || !config->exists("/material_overrides")) {
        return;
    }

    try {
        auto& overrides_json = config->get_json("/material_overrides");
        if (!overrides_json.is_object()) {
            return;
        }

        for (auto& [name, values] : overrides_json.items()) {
            filament::MaterialOverride ovr;
            if (values.contains("nozzle_min") && values["nozzle_min"].is_number_integer()) {
                ovr.nozzle_min = values["nozzle_min"].get<int>();
            }
            if (values.contains("nozzle_max") && values["nozzle_max"].is_number_integer()) {
                ovr.nozzle_max = values["nozzle_max"].get<int>();
            }
            if (values.contains("bed_temp") && values["bed_temp"].is_number_integer()) {
                ovr.bed_temp = values["bed_temp"].get<int>();
            }
            if (values.contains("chamber_temp") && values["chamber_temp"].is_number_integer()) {
                ovr.chamber_temp = values["chamber_temp"].get<int>();
            }
            if (values.contains("preheat_macro") && values["preheat_macro"].is_string()) {
                ovr.preheat_macro = values["preheat_macro"].get<std::string>();
            }
            if (values.contains("macro_handles_heating") &&
                values["macro_handles_heating"].is_boolean()) {
                ovr.macro_handles_heating = values["macro_handles_heating"].get<bool>();
            }
            overrides_[name] = ovr;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[MaterialSettingsManager] Failed to load overrides: {}", e.what());
    }
}

void MaterialSettingsManager::save_to_config() {
    Config* config = Config::get_instance();
    if (!config) {
        return;
    }

    // Build JSON object for all overrides
    nlohmann::json overrides_json = nlohmann::json::object();
    for (const auto& [name, ovr] : overrides_) {
        nlohmann::json entry = nlohmann::json::object();
        if (ovr.nozzle_min)
            entry["nozzle_min"] = *ovr.nozzle_min;
        if (ovr.nozzle_max)
            entry["nozzle_max"] = *ovr.nozzle_max;
        if (ovr.bed_temp)
            entry["bed_temp"] = *ovr.bed_temp;
        if (ovr.chamber_temp)
            entry["chamber_temp"] = *ovr.chamber_temp;
        if (ovr.preheat_macro)
            entry["preheat_macro"] = *ovr.preheat_macro;
        if (ovr.macro_handles_heating)
            entry["macro_handles_heating"] = *ovr.macro_handles_heating;
        overrides_json[name] = entry;
    }

    config->get_json("/material_overrides") = overrides_json;
    config->save();
}

void MaterialSettingsManager::assign_defaults() {
    preset_materials_ = default_preset_materials();
}

void MaterialSettingsManager::load_presets_from_config() {
    // Start from defaults; only override slots the stored config validly provides.
    assign_defaults();
    for (auto& pf : preset_filaments_) {
        pf.reset();
    }

    Config* config = Config::get_instance();
    if (!config || !config->exists("/preset_materials")) {
        return;
    }
    try {
        auto& arr = config->get_json("/preset_materials");
        if (!arr.is_array() || arr.size() != 4) {
            return; // malformed → keep defaults
        }
        for (int i = 0; i < 4; ++i) {
            const auto& v = arr[i];
            if (v.is_string()) {
                // Legacy / defensive: pre-migration or hand-edited bare-string entry.
                std::string s = v.get<std::string>();
                if (!s.empty()) {
                    preset_materials_[i] = s;
                }
                continue;
            }
            if (!v.is_object()) {
                continue;
            }
            if (v.contains("type") && v["type"].is_string()) {
                std::string t = v["type"].get<std::string>();
                if (!t.empty()) {
                    preset_materials_[i] = t;
                }
            }
            if (v.contains("filament_id") && v["filament_id"].is_string() &&
                !v["filament_id"].get<std::string>().empty()) {
                PresetFilament pf;
                pf.filament_id = v["filament_id"].get<std::string>();
                if (v.contains("brand") && v["brand"].is_string()) {
                    pf.brand = v["brand"].get<std::string>();
                }
                if (v.contains("name") && v["name"].is_string()) {
                    pf.name = v["name"].get<std::string>();
                }
                if (v.contains("nozzle") && v["nozzle"].is_number_integer()) {
                    pf.nozzle = v["nozzle"].get<int>();
                }
                if (v.contains("bed") && v["bed"].is_number_integer()) {
                    pf.bed = v["bed"].get<int>();
                }
                preset_filaments_[i] = pf;
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("[MaterialSettingsManager] Failed to load presets: {}", e.what());
        assign_defaults();
        for (auto& pf : preset_filaments_) {
            pf.reset();
        }
    }
}

void MaterialSettingsManager::save_presets_to_config() {
    Config* config = Config::get_instance();
    if (!config) {
        return;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 0; i < 4; ++i) {
        nlohmann::json entry = nlohmann::json::object();
        entry["type"] = preset_materials_[i];
        if (preset_filaments_[i] && preset_filaments_[i]->is_branded()) {
            const auto& pf = *preset_filaments_[i];
            entry["filament_id"] = pf.filament_id;
            entry["brand"] = pf.brand;
            entry["name"] = pf.name;
            entry["nozzle"] = pf.nozzle;
            entry["bed"] = pf.bed;
        }
        arr.push_back(entry);
    }
    config->get_json("/preset_materials") = arr;
    config->save();
}

void MaterialSettingsManager::set_preset_material(int index, const std::string& material) {
    if (index < 0 || index >= 4 || material.empty()) {
        return;
    }
    preset_materials_[index] = material;
    preset_filaments_[index].reset(); // plain type-swap reverts to generic
    save_presets_to_config();
    spdlog::info("[MaterialSettingsManager] Preset slot {} set to {}", index, material);
}

void MaterialSettingsManager::reset_preset_materials() {
    assign_defaults();
    for (auto& pf : preset_filaments_) {
        pf.reset();
    }
    save_presets_to_config();
    spdlog::info("[MaterialSettingsManager] Presets reset to defaults");
}

std::optional<MaterialSettingsManager::PresetFilament>
MaterialSettingsManager::get_preset_filament(int index) const {
    if (index < 0 || index >= 4) {
        return std::nullopt;
    }
    return preset_filaments_[index];
}

void MaterialSettingsManager::set_preset_filament(int index,
                                                  const helix::printer::EffectiveFilament& ef) {
    if (index < 0 || index >= 4) {
        return;
    }
    PresetFilament pf;
    pf.filament_id = ef.id;
    pf.brand = ef.brand;
    pf.name = ef.name;
    pf.nozzle = ef.nozzle_recommended;
    pf.bed = ef.bed_temp;
    preset_filaments_[index] = pf;
    if (!ef.type.empty()) {
        preset_materials_[index] = ef.type; // type kept in lockstep
    }
    save_presets_to_config();
    spdlog::info("[MaterialSettingsManager] Preset slot {} set to branded filament '{}'", index,
                 ef.id);
}

void MaterialSettingsManager::clear_preset_filament(int index) {
    if (index < 0 || index >= 4) {
        return;
    }
    preset_filaments_[index].reset();
    save_presets_to_config();
}

} // namespace helix

// ============================================================================
// Bridge function for filament_database.h
// ============================================================================

namespace filament {

const MaterialOverride* get_material_override(std::string_view name) {
    auto& mgr = helix::MaterialSettingsManager::instance();
    return mgr.get_override(std::string(name));
}

} // namespace filament
