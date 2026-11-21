// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "spoolman_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file lane_spool_history.h
 * @brief The spools this printer's lanes have held, for Assign Spool without Spoolman
 *
 * Without Spoolman there is no spool list to pick from, so Assign Spool lists
 * the filaments previously assigned to this printer's lanes, most recent
 * first. Persisted per printer in Config.
 */

namespace helix::lane_spool_history {

struct Entry {
    std::string material;
    std::string brand;
    std::string product_name;
    std::string catalog_id;
    uint32_t color_rgb = 0;
    float total_weight_g = 0;
};

inline constexpr size_t kMaxEntries = 12;

/// The entry a lane's filament records: nothing for a lane with no material.
[[nodiscard]] std::optional<Entry> entry_from_slot(const SlotInfo& slot);

/// @p list with @p entry at the front: an entry naming the same filament
/// (material, brand, product and color) moves up rather than repeating, and the
/// list keeps at most @p max entries.
[[nodiscard]] std::vector<Entry> with_recent(std::vector<Entry> list, const Entry& entry,
                                             size_t max = kMaxEntries);

/// The entry as a spool row: no Spoolman id, its color as hex.
[[nodiscard]] SpoolInfo as_spool(const Entry& entry);

/// This printer's history, most recent first.
std::vector<Entry> load();

/// Record what @p slot now holds.
void record(const SlotInfo& slot);

} // namespace helix::lane_spool_history
