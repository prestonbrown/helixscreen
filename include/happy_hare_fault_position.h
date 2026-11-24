// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"

#include <string_view>

/**
 * @file happy_hare_fault_position.h
 * @brief Which lane sensor a Happy Hare pause message is at.
 *
 * Happy Hare's runout handler (`MMU_SENSOR_RUNOUT`, `Mmu._runout`) raises
 * `Runout detected on T0 (gate 0) by mmu_entry_0` when a gate's pre-gate (entry) sensor
 * reports the filament gone and no endless-spool gate is available. Happy Hare 3 names the
 * same sensor `mmu_pre_gate_<gate>` and words it `Runout detected on mmu_pre_gate_0`.
 *
 * Pure functions over strings: no LVGL, no printer state, no locking.
 */
namespace helix::happy_hare {

/// PREP for a runout that names a pre-gate (entry) sensor; NONE for any other message.
[[nodiscard]] LaneSensor happy_hare_fault_lane_sensor(std::string_view message);

} // namespace helix::happy_hare
