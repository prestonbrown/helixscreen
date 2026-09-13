// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

/**
 * @file filament_favorites.h
 * @brief Starred (favorite) catalog product ids, persisted globally in Config.
 *
 * Favorites are a USER preference, not printer state: a filament starred in
 * the catalog picker is a favorite on every printer profile, so the ids live
 * at a root-level Config path rather than under the per-printer df() prefix.
 */

namespace helix::filament_favorites {

/// Config path holding the starred EffectiveFilament ids, in star order.
inline constexpr const char* kFavoriteIdsPath = "/filament/favorite_ids";

/// Starred product ids, read live from Config (no cache: every selector view
/// rebuild re-reads, so a star toggled in one place is visible everywhere).
std::vector<std::string> load_favorite_ids();

/// Whether @p product_id is starred. Empty/unknown ids are simply not members.
bool is_favorite(const std::string& product_id);

/// Flip @p product_id's star and persist immediately — there is no separate
/// save step; an unstarred row disappears from the favorites view on refresh.
/// Returns the NEW star state. An empty id is ignored (returns false).
bool toggle_favorite(const std::string& product_id);

} // namespace helix::filament_favorites
