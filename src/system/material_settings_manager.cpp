// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "material_settings_manager.h"

#include "config.h"
#include "core/text_io.h"
#include "filament_catalog.h"
#include "json_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

namespace helix {

MaterialSettingsManager& MaterialSettingsManager::instance() {
    static MaterialSettingsManager s_instance;
    return s_instance;
}

namespace {

using helix::printer::FilamentCatalog;

// The keys MaterialSettingsManager owns inside one overlay `types` entry.
// Any other key there (density, dry_temp, ...) is hand-authored and preserved.
constexpr const char* kKeyNozzleMin = "nozzle_min";
constexpr const char* kKeyNozzleMax = "nozzle_max";
constexpr const char* kKeyBed = "bed";
constexpr const char* kKeyChamber = "chamber";
constexpr const char* kKeyMacro = "preheat_macro";
constexpr const char* kKeyMacroHeats = "macro_handles_heating";
constexpr const char* kManagedKeys[] = {kKeyNozzleMin, kKeyNozzleMax, kKeyBed,
                                        kKeyChamber,   kKeyMacro,     kKeyMacroHeats};

/// The entry naming @p name (any case, aliases resolved), or end().
std::vector<nlohmann::json>::iterator find_entry(std::vector<nlohmann::json>& entries,
                                                 const std::string& name) {
    const std::string key = helix::text_io::to_lower(std::string(filament::resolve_alias(name)));
    return std::find_if(entries.begin(), entries.end(), [&](const nlohmann::json& e) {
        return helix::text_io::to_lower(std::string(
                   filament::resolve_alias(helix::json_util::safe_string(e, "name")))) == key;
    });
}

std::optional<filament::MaterialOverride> parse_override(const nlohmann::json& e) {
    filament::MaterialOverride ovr;
    auto read_int = [&](const char* key, std::optional<int>& out) {
        auto it = e.find(key);
        if (it != e.end() && it->is_number())
            out = static_cast<int>(it->get<double>());
    };
    read_int(kKeyNozzleMin, ovr.nozzle_min);
    read_int(kKeyNozzleMax, ovr.nozzle_max);
    read_int(kKeyBed, ovr.bed_temp);
    read_int(kKeyChamber, ovr.chamber_temp);
    if (auto it = e.find(kKeyMacro); it != e.end() && it->is_string())
        ovr.preheat_macro = it->get<std::string>();
    if (auto it = e.find(kKeyMacroHeats); it != e.end() && it->is_boolean())
        ovr.macro_handles_heating = it->get<bool>();
    if (!ovr.nozzle_min && !ovr.nozzle_max && !ovr.bed_temp && !ovr.chamber_temp &&
        !ovr.preheat_macro && !ovr.macro_handles_heating)
        return std::nullopt;
    return ovr;
}

void write_override(nlohmann::json& e, const filament::MaterialOverride& ovr) {
    if (ovr.nozzle_min)
        e[kKeyNozzleMin] = *ovr.nozzle_min;
    if (ovr.nozzle_max)
        e[kKeyNozzleMax] = *ovr.nozzle_max;
    if (ovr.bed_temp)
        e[kKeyBed] = *ovr.bed_temp;
    if (ovr.chamber_temp)
        e[kKeyChamber] = *ovr.chamber_temp;
    if (ovr.preheat_macro)
        e[kKeyMacro] = *ovr.preheat_macro;
    if (ovr.macro_handles_heating)
        e[kKeyMacroHeats] = *ovr.macro_handles_heating;
}

} // namespace

void MaterialSettingsManager::init() {
    if (initialized_) {
        return;
    }
    if (migrate_settings_overrides()) {
        filament::reload_materials();
    }
    load_from_overlay();
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

const filament::MaterialOverride*
MaterialSettingsManager::find_override_for_material(const std::string& name) const {
    // Overrides are keyed by the database spelling; spool metadata may differ in case.
    const auto material = filament::find_material(name);
    return get_override(material ? material->name : name);
}

void MaterialSettingsManager::set_override(const std::string& name,
                                           const filament::MaterialOverride& override) {
    if (write_to_overlay(name, override)) {
        spdlog::info("[MaterialSettingsManager] Set override for '{}'", name);
    }
}

void MaterialSettingsManager::clear_override(const std::string& name) {
    if (overrides_.count(name) == 0) {
        return;
    }
    const auto material = filament::find_material(name);
    if (material && material->user_defined) {
        // Its temps ARE its definition: there is no shipped row to fall back to.
        spdlog::warn("[MaterialSettingsManager] '{}' is a user-defined type; nothing to reset",
                     name);
        return;
    }
    if (write_to_overlay(name, std::nullopt)) {
        spdlog::info("[MaterialSettingsManager] Cleared override for '{}'", name);
    }
}

bool MaterialSettingsManager::has_override(const std::string& name) const {
    return overrides_.count(name) > 0;
}

void MaterialSettingsManager::load_from_overlay() {
    overrides_.clear();
    for (const auto& e : FilamentCatalog::load_user_types()) {
        auto ovr = parse_override(e);
        if (!ovr)
            continue;
        const std::string name = helix::json_util::safe_string(e, "name");
        const auto material = filament::find_material(name);
        overrides_[material ? material->name : name] = *ovr;
    }
}

bool MaterialSettingsManager::write_to_overlay(
    const std::string& name, const std::optional<filament::MaterialOverride>& ovr) {
    auto entries = FilamentCatalog::load_user_types();
    auto it = find_entry(entries, name);
    if (it == entries.end()) {
        if (!ovr)
            return true;
        entries.push_back({{"name", name}});
        it = std::prev(entries.end());
    }
    for (const char* key : kManagedKeys)
        it->erase(key);
    if (ovr)
        write_override(*it, *ovr);
    if (it->size() == 1) // only "name" left: the entry says nothing
        entries.erase(it);

    if (!FilamentCatalog::save_user_types(entries)) {
        spdlog::warn("[MaterialSettingsManager] Could not save '{}' to the filament overlay", name);
        return false;
    }
    filament::reload_materials();
    load_from_overlay();
    return true;
}

bool MaterialSettingsManager::migrate_settings_overrides() {
    Config* config = Config::get_instance();
    if (!config->exists("/material_overrides")) {
        return false;
    }
    const nlohmann::json legacy = config->get_json("/material_overrides");
    bool wrote = false;

    // Saving over an overlay that will not parse replaces the user's hand
    // edits with the migrated entries alone. Settings keep the only copy, and
    // the next start retries once the file is fixed.
    if (const std::string path = FilamentCatalog::user_overlay_path();
        FilamentCatalog::overlay_file_is_corrupt(path)) {
        spdlog::warn("[MaterialSettingsManager] {} does not parse; leaving material_overrides "
                     "in settings.json until it does",
                     path);
        return false;
    }

    if (legacy.is_object() && !legacy.empty()) {
        // settings.json spelled two of the keys differently from the overlay.
        static constexpr std::pair<const char*, const char*> kRenames[] = {
            {"nozzle_min", kKeyNozzleMin}, {"nozzle_max", kKeyNozzleMax},
            {"bed_temp", kKeyBed},         {"chamber_temp", kKeyChamber},
            {"preheat_macro", kKeyMacro},  {"macro_handles_heating", kKeyMacroHeats}};

        auto entries = FilamentCatalog::load_user_types();
        for (const auto& [name, values] : legacy.items()) {
            if (!values.is_object())
                continue;
            auto it = find_entry(entries, name);
            if (it == entries.end()) {
                entries.push_back({{"name", name}});
                it = std::prev(entries.end());
            }
            // A field already in the overlay wins: it is the newer copy.
            for (const auto& [from, to] : kRenames) {
                if (values.contains(from) && !it->contains(to))
                    (*it)[to] = values[from];
            }
            if (it->size() == 1)
                entries.erase(it);
        }
        // Settings keep the only copy until the overlay write lands, so a
        // failed write is retried on the next start.
        if (!FilamentCatalog::save_user_types(entries)) {
            spdlog::warn("[MaterialSettingsManager] Could not move material_overrides into the "
                         "filament overlay; leaving them in settings.json");
            return false;
        }
        wrote = true;
        spdlog::info("[MaterialSettingsManager] Moved {} material override(s) from settings.json "
                     "into the filament overlay",
                     legacy.size());
    }
    config->get_json("").erase("material_overrides");
    config->save();
    return wrote;
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
    if (!config->exists("/preset_materials")) {
        return;
    }
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
}

void MaterialSettingsManager::save_presets_to_config() {
    Config* config = Config::get_instance();
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
