// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/observer_guard.h"
#include "lvgl/lvgl.h"
#include "material_settings_manager.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace helix {
class PrinterTemperatureState;
}

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

/// Nozzle, bed and chamber temperature a slot prints at, whole °C. chamber
/// is the material's chamber temperature, 0 for a material that needs none.
struct SlotTemps {
    int nozzle = 0;
    int bed = 0;
    int chamber = 0;
};

/// The slot's temperatures: the attached branded product's when there is one,
/// otherwise the filament database recommendation. nullopt when the slot is
/// out of range or its material is unknown.
std::optional<SlotTemps> slot_temps(int slot);

/// The bed and chamber targets a material preset sends with its nozzle
/// target, whole °C. nullopt means the preset leaves that heater alone.
struct HeaterTargets {
    std::optional<int> bed;
    std::optional<int> chamber;
};

/// What a material preset sends besides the nozzle. While "Material presets
/// heat the bed and chamber" is on (per printer, the default), @p bed, and
/// @p chamber when it is above 0 (a material with a chamber temperature).
/// Off, neither: the preset heats the nozzle alone. Every material preset
/// asks this: the Filament panel's, the Home preheat tile's and the
/// temperature graph's.
HeaterTargets heater_targets(int bed, int chamber = 0);

/// The chamber temperature a material preset asks for on this printer:
/// @p material_chamber_c when the printer has a chamber heater, otherwise 0.
int chamber_request(int material_chamber_c, const PrinterTemperatureState& temps);

/// "220°C / 60°C" style summary for a slot (branded product temps when attached,
/// otherwise the filament database recommendation). "---" if the material is unknown.
std::string temp_label(int slot);

/// The nozzle half of temp_label(): "220°C", or "---" if the material is unknown.
std::string nozzle_temp_label(int slot);

/// Whether a tap on slot @p slot's preset would run a macro: a material its
/// button stands for names one, whether or not this printer defines it. The
/// preset's mark.
[[nodiscard]] bool has_macro(int slot);

/// The material whose row a preset's macro is read from, and whose setting a
/// hold on the preset opens: the slot's own material, unless it names no macro
/// and another material its button stands for names one. The generic ABS
/// button stands for ASA too. Empty for a slot out of range.
[[nodiscard]] std::string macro_material(int slot);

/// The subjects a preset button binds its macro mark to: "preset_material_<slot>_has_macro",
/// 1 while the material names a macro, and "preset_material_<slot>_mark_beside_name", 1 while
/// the name fits beside the mark in the button as laid out. The Filament panel measures
/// the second. Nullptr before init_subjects(), or for a slot out of range.
lv_subject_t* has_macro_subject(int slot);
lv_subject_t* mark_beside_name_subject(int slot);

/// The subjects a preset button binds its lines to: "preset_material_<slot>_name",
/// "_temps" and "_nozzle_temp". Nullptr before init_subjects(), or for a slot out of
/// range.
lv_subject_t* name_subject(int slot);
lv_subject_t* temps_subject(int slot);
lv_subject_t* nozzle_temp_subject(int slot);

/// The lifetime of every subject above, for an observer bound to one.
[[nodiscard]] SubjectLifetime get_subjects_lifetime();

/**
 * @brief Create the globally-scoped preset subjects. Idempotent.
 *
 * Registers "preset_material_0_name" … "preset_material_3_name", the matching
 * "_temps", "_nozzle_temp" and "_has_macro" subjects, and "preset_material_count", in the
 * global XML scope. Self-registers its own deinit with
 * StaticSubjectRegistry so cleanup runs before lv_deinit().
 *
 * Must be called after LVGL init and after MaterialSettingsManager::init().
 */
void init_subjects();

/**
 * @brief Re-read MaterialSettingsManager and push into the subjects.
 *
 * Call after any set_preset_material() / set_preset_filament() /
 * reset_preset_materials(), and whenever a screen that shows the presets comes
 * back: a material's macro may have been set or cleared meanwhile. No-op if
 * init_subjects() has not run (unit tests that only use the plain accessors).
 */
void refresh_subjects();

/// True once init_subjects() has run and the subjects are live.
bool subjects_ready();

} // namespace helix::presets
