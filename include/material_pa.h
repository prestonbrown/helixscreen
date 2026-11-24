// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pressure advance by material (prestonbrown/helixscreen#2174): what a tuner's
// "Save for <material>" writes into [save_variables], in the names
// _HELIX_MATERIAL_PA (assets/config/helix_macros.cfg) reads at print start.
//
// An entry is the model plus every parameter it runs with. The macro sends
// all of them, each from the entry or else from the extruder's config
// section, because a nonlinear model keeps any parameter a command leaves out
// and a slicer's bare ADVANCE= replaces only the linear term. A material with
// no entry prints with the config's values.

#include "pa_model.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::material_pa {

/// The printer-side switch the macro obeys; off (absent) by default.
inline constexpr const char* kEnabledVar = "helix_material_pa_enabled";
/// Every entry's variables start with this, then the material key and '_'.
inline constexpr const char* kVarPrefix = "helix_pa_";
/// The macro that applies an entry; its presence marks a helper pack that
/// carries pressure advance by material.
inline constexpr const char* kApplyMacro = "_HELIX_MATERIAL_PA";

/// One material's tuned values. A linear entry is K alone: the macro reads
/// only its advance and takes time offset and smoothing from the config, so
/// what an earlier nonlinear entry for the same material left behind never
/// applies. A nonlinear entry carries all five parameters.
struct Entry {
    pa::ModelKind model = pa::ModelKind::Linear;
    /// K under a linear model, linear_advance under a nonlinear one.
    float advance = 0.0f;
    float offset = 0.0f;
    float velocity = 0.0f;
    float time_offset = 0.0f;
    float smooth_time = 0.0f;
};

/// A linear tuner's result: K alone.
Entry linear_entry(float k);
/// A nonlinear tuner's result: the model and all five parameters.
Entry nonlinear_entry(const pa::NonlinearValues& v);

/// The storage key for @p material: the Z offset macro's key rule
/// (material_zoffset::helix_key()), since one macro builds both. nullopt for
/// an empty material, which has no entry of its own, or a name the rule
/// cannot store.
std::optional<std::string> storage_key(std::string_view material);

/// `helix_pa_<key>_<field>`.
std::string var_name(std::string_view key, std::string_view field);

/// The script that saves @p entry for @p material and turns the switch on:
/// one SAVE_VARIABLE per variable, the model last: the macro counts an entry
/// only once its model is there. Empty when the material has no storage key.
std::string save_gcode(std::string_view material, const Entry& entry);

/// SAVE_VARIABLE for the switch.
std::string enable_gcode(bool on);

} // namespace helix::material_pa
