// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The chamber air mode targets the user stores for the active printer: what the
// Exhaust and Recirculate buttons send to a provider that takes targets
// (chamber_air::takes_targets). Kept per printer, beside the printer's other
// settings, under "chamber_air".

#include "chamber_air_modes.h"

namespace helix::chamber_air {

inline constexpr double kMinTargetC = 20.0;
inline constexpr double kMaxTargetC = 80.0;
inline constexpr int kMinTargetPct = 10;
inline constexpr int kMaxTargetPct = 100;

/// @p t with every field inside the range the settings row offers.
[[nodiscard]] ModeTargets clamp_targets(ModeTargets t);

/// The active printer's stored targets, clamped; ModeTargets{} for what is unset.
[[nodiscard]] ModeTargets load_targets();

/// Stores @p t (clamped) for the active printer and saves the config.
void save_targets(const ModeTargets& t);

} // namespace helix::chamber_air
