// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "moonraker_error.h"
#include "spoolman_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helix {

/**
 * @brief SpoolmanDB search state: whether the server has the route, and which
 * query is the latest.
 *
 * Spoolman serves /external/filament/search from 0.26.0; through Moonraker an
 * older server answers not-found (code -32601, "Not Found"). That answer is cached per Moonraker
 * connection, so the search UI asks the server once and then stays shown or hidden. Main thread
 * only.
 */
class SpoolmanCatalogSearch {
  public:
    enum class Availability { Unknown, Available, Unavailable };

    /// Also the number of result rows ui_xml/spool_wizard.xml builds.
    static constexpr int kResultLimit = 25;
    static constexpr size_t kMinQueryLength = 2;

    /// What is known for connection @p connection_generation; Unknown when the
    /// cached answer belongs to another connection or nothing was asked yet.
    static Availability availability(uint64_t connection_generation);

    /// Records how connection @p connection_generation answered a search: a
    /// success means available, not-found (MoonrakerError::is_not_found)
    /// unavailable. Any other error proves nothing about the route and records
    /// nothing.
    static void record_success(uint64_t connection_generation);
    static void record_error(uint64_t connection_generation, const MoonrakerError& err);

    /// Forgets the cached answer.
    static void reset_cache();

    /// Whether @p query is long enough to send.
    static bool is_searchable(const std::string& query);

    /// Starts a query. Its ticket stays current until the next begin() or
    /// invalidate(); a response carrying any other ticket is stale.
    uint64_t begin();
    bool is_current(uint64_t ticket) const {
        return ticket == generation_;
    }
    /// Makes every outstanding ticket stale (the wizard closed, the query cleared).
    void invalidate() {
        ++generation_;
    }

  private:
    uint64_t generation_ = 0;
};

namespace spoolman {

/// @p hex uppercased without '#' when it is 6 or 8 hex digits (Spoolman's
/// colour form, alpha last), else empty.
std::string normalize_color_hex(const std::string& hex);

/// @p name trimmed, case-folded and with each whitespace run collapsed to
/// one space: the form two filament names are compared in.
std::string normalized_name(const std::string& name);

/// The record in @p filaments that is exactly the one a save would file: the
/// same stored material string, the same colour and the same name after
/// normalized_name(). The one rule for reusing a Spoolman filament without
/// asking; anything less exact is for the user to choose.
const FilamentInfo* find_matching_filament(const std::vector<FilamentInfo>& filaments,
                                           const std::string& material, const std::string& name,
                                           const std::string& color_hex);

/// The name a new Spoolman filament is filed under: @p product (a catalog
/// product name or the precise type, "ABS+" or "Silk PLA") then the colour,
/// trimmed and joined by one space. Empty only when both are blank.
std::string new_filament_name(const std::string& product, const std::string& color_name);

/// What to call @p filament to the user: its name, or for a record with none
/// its material and @p color_name ("ABS Gray").
std::string filament_display_name(const FilamentInfo& filament, const std::string& color_name);

/// The one temperature Spoolman stores for a range: @p recommended when it is
/// set and inside the range, else the midpoint, or the one end that is set; 0
/// when nothing is.
int temperature_for_range(int min_temp, int max_temp, int recommended = 0);

/// Of @p filaments not already an exact match, the one that is most likely the
/// same product: a material that records as @p material (filament::slicer_material()
/// of both sides, so a record filed as "ABS+" still counts as ABS) and the
/// nearest colour within FilamentMapper's colour tolerance. nullptr when none
/// is near enough. Offered to the user, never taken silently. The record
/// @p exclude_id, the one the spool already points at, is never offered:
/// taking it would leave the spool where it is and drop the edit.
const FilamentInfo* find_similar_filament(const std::vector<FilamentInfo>& filaments,
                                          const std::string& material, const std::string& color_hex,
                                          int exclude_id = 0);

/// The filament in @p filaments that is this catalog product: vendor
/// @p vendor_id, and the same name and material (case-insensitive, trimmed)
/// and the same set of colours in any order. A multi-colour product never
/// matches a solid filament. @p colors is color_hex, or multi_color_hexes,
/// comma-separated either way.
const FilamentInfo* find_catalog_filament(const std::vector<FilamentInfo>& filaments, int vendor_id,
                                          const std::string& name, const std::string& material,
                                          const std::string& colors);

} // namespace spoolman
} // namespace helix
