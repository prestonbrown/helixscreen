// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "cold_pull_temps.h"
#include "filament_database.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>

namespace helix::printer {
struct EffectiveFilament;
}

namespace helix {

inline constexpr std::array<const char*, 4> kDefaultPresetMaterials{"PLA", "PETG", "ABS", "TPU"};

/**
 * @brief kDefaultPresetMaterials as owning strings.
 *
 * Single source for every default-preset seed: the member initializer (which
 * must hold before init() runs, since construction is defaulted) and
 * assign_defaults(). Do not re-type the literals at either site.
 */
inline std::array<std::string, 4> default_preset_materials() {
    std::array<std::string, 4> out;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = kDefaultPresetMaterials[i];
    }
    return out;
}

/**
 * @brief Manages user overrides for material temperature settings
 *
 * Overrides live in the filament overlay (user_filaments.json) as `types`
 * entries, the same place a hand-edit puts them; the fields they set are
 * already merged into filament::materials(). This manager is the sparse view
 * (which fields the user set) and the writer. Quick-button presets stay in
 * settings.json.
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class MaterialSettingsManager {
  public:
    static MaterialSettingsManager& instance();

    // Non-copyable
    MaterialSettingsManager(const MaterialSettingsManager&) = delete;
    MaterialSettingsManager& operator=(const MaterialSettingsManager&) = delete;

    /** @brief Load overrides; moves any left in settings.json into the overlay first */
    void init();

    /** @brief Get override for a material (nullptr if none) */
    const filament::MaterialOverride* get_override(const std::string& name) const;

    /** @brief Override for a material named in any case or alias ("abs" finds "ABS") */
    const filament::MaterialOverride* find_override_for_material(const std::string& name) const;

    /** @brief Set override for a material (saves to the overlay) */
    void set_override(const std::string& name, const filament::MaterialOverride& override);

    /** @brief Remove override for a material (saves to the overlay). No-op for a
     *         user-defined type, whose temps are its definition. */
    void clear_override(const std::string& name);

    /** @brief Check if a material has any overrides */
    bool has_override(const std::string& name) const;

    /** @brief Get all overrides (for UI list display) */
    const std::unordered_map<std::string, filament::MaterialOverride>& get_all_overrides() const {
        return overrides_;
    }

    /** @brief Get the 4 preset materials assigned to the quick-material buttons */
    std::array<std::string, 4> get_preset_materials() const {
        return preset_materials_;
    }

    /** @brief Assign a material name to a preset slot (0-3); saves to config */
    void set_preset_material(int index, const std::string& material);

    /** @brief Restore all preset slots to kDefaultPresetMaterials; saves to config */
    void reset_preset_materials();

    /** @brief Branded filament info attached to a quick-material preset slot */
    struct PresetFilament {
        std::string filament_id;
        std::string brand;
        std::string name;
        int nozzle = 0;
        int bed = 0;
        bool is_branded() const {
            return !filament_id.empty();
        }
    };

    /** @brief Get the branded filament attached to a preset slot (nullopt if generic) */
    std::optional<PresetFilament> get_preset_filament(int index) const;

    /** @brief Attach a branded filament to a preset slot (0-3); saves to config */
    void set_preset_filament(int index, const helix::printer::EffectiveFilament& ef);

    /** @brief Clear the branded filament from a preset slot (0-3); saves to config */
    void clear_preset_filament(int index);

    /** @brief The cold pull temperatures for @p m: the user's edit, else the default */
    [[nodiscard]] ColdPullTemps get_cold_pull_temps(ColdPullMaterial m) const;
    /** @brief Save a cold pull edit for @p m */
    void set_cold_pull_temps(ColdPullMaterial m, ColdPullTemps t);
    /** @brief Drop the edit for @p m, back to its default */
    void reset_cold_pull_temps(ColdPullMaterial m);
    /** @brief The material the last cold pull used; PLA when none */
    [[nodiscard]] ColdPullMaterial get_cold_pull_material() const;
    void set_cold_pull_material(ColdPullMaterial m);

  private:
    friend class TestAccess;
    MaterialSettingsManager() = default;
    ~MaterialSettingsManager() = default;

    void load_from_overlay();
    bool write_to_overlay(const std::string& name,
                          const std::optional<filament::MaterialOverride>& ovr);
    /// @return true when the overlay changed
    bool migrate_settings_overrides();
    void load_presets_from_config();
    void save_presets_to_config();
    /** @brief Reset all 4 preset slots (in memory only) to kDefaultPresetMaterials */
    void assign_defaults();

    std::unordered_map<std::string, filament::MaterialOverride> overrides_;
    std::array<std::string, 4> preset_materials_ = default_preset_materials();
    std::array<std::optional<PresetFilament>, 4> preset_filaments_{};
    bool initialized_ = false;
};

} // namespace helix
