// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "material_settings_manager.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

/**
 * @file preset_materials.h
 * @brief Single source of truth for "which materials are on the quick-preset slots".
 *
 * Preset material IDENTITY is owned by MaterialSettingsManager (user-configurable,
 * persisted to settings.json under "preset_materials"). This header is the one
 * place the rest of the UI reads it from, so the filament panel, the preheat
 * widget, the nozzle/bed/chamber temperature panels, the PID calibration panel
 * and the AMS environment overlay can never disagree about slot N. A hardcoded
 * {"PLA","PETG","ABS",...} copy anywhere else would show a reassigned slot's new
 * material in some places and the old literal in others.
 *
 * Two layers:
 *  - Plain accessors (name/all) — no LVGL, safe from any code including tests.
 *  - LVGL subjects (init/refresh) — globally-scoped string subjects so XML can
 *    bind button labels declaratively instead of hardcoding text="PLA".
 *
 * Threading: main LVGL thread only. Call refresh_subjects() via ui_queue_update()
 * if you ever need to poke it from a background thread.
 */
namespace helix::presets {

/// Number of quick-preset slots.
///
/// DERIVED from MaterialSettingsManager's default seed, never restated. This
/// module deliberately contains no material name literals and no default list
/// of its own — kDefaultPresetMaterials remains the single source for the
/// default slot assignment, and everything here reads through the manager.
inline constexpr int kPresetCount = static_cast<int>(kDefaultPresetMaterials.size());

/// Material name assigned to slot (0..kPresetCount-1). Empty string if out of range.
/// This is the verbatim material string — safe to hand to filament::find_material()
/// and to MaterialSettingsManager::get_override().
std::string name(int slot);

/// All preset material names, slot-indexed.
std::array<std::string, kPresetCount> all();

/// Display label for a slot: "Bambu PLA" when a branded filament is attached to
/// the slot, otherwise just the material name. Never use this as a material key.
std::string display_label(int slot);

/// Nozzle and bed temperature a slot prints at, whole °C.
struct SlotTemps {
    int nozzle = 0;
    int bed = 0;
};

/// The slot's temperatures: the attached branded product's when there is one,
/// otherwise the filament database recommendation. nullopt when the slot is
/// out of range or its material is unknown.
std::optional<SlotTemps> slot_temps(int slot);

/// "220°C / 60°C" style summary for a slot (branded product temps when attached,
/// otherwise the filament database recommendation). "---" if the material is unknown.
std::string temp_label(int slot);

/**
 * @brief Create the globally-scoped preset subjects. Idempotent.
 *
 * Registers "preset_material_0_name" … "preset_material_3_name" and
 * "preset_material_0_temps" … "preset_material_3_temps" in the global XML scope,
 * plus "preset_material_count". Self-registers its own deinit with
 * StaticSubjectRegistry so cleanup runs before lv_deinit().
 *
 * Must be called after LVGL init and after MaterialSettingsManager::init().
 */
void init_subjects();

/**
 * @brief Re-read MaterialSettingsManager and push into the subjects.
 *
 * Call after any set_preset_material() / set_preset_filament() /
 * reset_preset_materials(). No-op if init_subjects() has not run (unit tests
 * that only use the plain accessors).
 */
void refresh_subjects();

/// True once init_subjects() has run and the subjects are live.
bool subjects_ready();

} // namespace helix::presets
