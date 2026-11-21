// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "json_fwd.h"
#include "spoolman_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @file lane_spool_history.h
 * @brief The spools this printer's lanes have held, for Assign Spool without Spoolman
 *
 * Without Spoolman there is no spool list to pick from, so Assign Spool lists
 * the filaments previously assigned to this printer's lanes, most recent
 * first. Kept in the printer's own Moonraker database (namespace
 * kDbNamespace, key kDbKey), so it follows the printer across screen
 * reinstalls and other clients.
 */

struct MoonrakerError; // NAMESPACE_OK: forward declaration of the global MoonrakerError

namespace helix {
class IMoonrakerAPI;
}

namespace helix::lane_spool_history {

inline constexpr const char* kDbNamespace = "helix-screen";
inline constexpr const char* kDbKey = "lane_spool_history";

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

/// The history a stored value holds: entries without a material, and
/// anything that is not an entry, are skipped.
[[nodiscard]] std::vector<Entry> from_json(const nlohmann::json& value);
[[nodiscard]] nlohmann::json to_json(const std::vector<Entry>& entries);

/// Read this printer's history, most recent first. @p on_loaded runs once, on
/// the reply's thread, with an empty list when nothing is stored or the read
/// fails.
void load(IMoonrakerAPI& api, std::function<void(std::vector<Entry>)> on_loaded);

/// The history a record builds on, from what the read of the stored value
/// answered: the stored list, an empty one when nothing is stored yet
/// (not-found), and nothing when the read failed otherwise, since writing then
/// would replace a history the read could not see.
[[nodiscard]] std::optional<std::vector<Entry>> base_for_record(const nlohmann::json* stored,
                                                                const MoonrakerError* error);

/// Record what @p slot now holds at the front of the history. Call on the main
/// thread; records made while one is being written ride the next write.
void record(IMoonrakerAPI& api, const SlotInfo& slot);

} // namespace helix::lane_spool_history
