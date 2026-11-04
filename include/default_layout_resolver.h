// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_breakpoint.h"

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

/// Default home layout resolution: a `default_layout.json` table plus a
/// snapshot of capability subjects in, placements out. Pure: it reads no LVGL
/// subject and no file, so the same inputs always give the same layout.
///
/// A table's `anchors` array (and each variant's) holds two kinds of entry:
///
/// - a fixed anchor `{"id", "placements": {tier: {col,row,colspan,rowspan}}}`,
///   placed wherever its tier key says;
/// - a slot `{"slot", "placements": {tier: region}, "alternatives": [...]}`,
///   whose region is filled by the first alternative whose lead cell has a
///   qualifying candidate; the last alternative needs none. Each alternative is `{"cells":
///   [{"candidates", "at"}]}`, `at` being `[col, row, colspan, rowspan]` relative to the region,
///   and `candidates` either an inline array (ids, or `{"id", "requires": [...]}`) or the name of a
///   top-level `candidate_lists` entry. Each cell takes the first qualifying candidate not already
///   placed, so cells sharing a list fill in the order they are written and a cell with none stays
///   empty. With a measured grid, an alternative whose cells run off it gives way to a later one
///   that stays on it; when none does, the region alone decides.
///
/// A table's `disabled` object lists widget ids switched off per placement key,
/// chosen the way placements are, so a grid-qualified list replaces the bare
/// tier's on that grid.
///
/// Every widget named in a candidate list of the active table that ends up
/// unplaced is reported in `disabled`, including the candidates of a slot that
/// has no region at this tier.
namespace helix::layout {

struct CapabilitySnapshot {
    std::map<std::string, int>
        subjects; // gate and `requires` subject name -> value; absent reads 0
    int get(const std::string& name) const;
};

struct ResolvedPlacement {
    std::string id;
    int col, row, colspan, rowspan; // tracks
    /// The anchor's `config` with its placement-level `config` merged over
    /// it, or an empty object.
    nlohmann::json config;
};

struct ResolvedDefault {
    std::vector<ResolvedPlacement> placed;
    std::vector<std::string> disabled; // losing candidates + tier `disabled` ids
    std::string signature; // tier|cols|rows|variant chain|preset|name=value,... for every consulted
                           // subject, sorted
    bool ok = false; // false: table unusable (missing, no anchors for this tier); caller falls back
};

/// Resolve @p table (with @p overlay applied when non-null) for one tier and
/// grid. @p variant_chain is most-specific first, as LayoutManager orders it;
/// the first variant with a non-empty `anchors` wins, else the top-level table.
ResolvedDefault resolve_default_layout(const nlohmann::json& table,
                                       const nlohmann::json* overlay, // preset seed or nullptr
                                       std::string_view tier, int cols, int rows,
                                       const std::vector<std::string>& variant_chain,
                                       std::string_view preset, const CapabilitySnapshot& caps);

/// Registry gate subjects of every widget id @p table names (base, variants,
/// slots and candidate lists) plus every `requires` subject; sorted, unique.
std::vector<std::string> subjects_named_by(const nlohmann::json& table);

/// Hash of a serialize_pages() payload's arrangement: per page, the id, col,
/// row, colspan and rowspan of each placed entry (enabled, with a grid
/// position), in order; and the page count. Unplaced and disabled entries do
/// not contribute, so neither does per-widget `config` nor whether an id has a
/// registry definition.
std::string arrangement_hash(const nlohmann::json& pages_payload);

/// Breakpoint names to try for one tier, most specific first. A chain shorter
/// than three ends at nullptr; a breakpoint outside the enum's range has none.
using BreakpointChain = std::array<const char*, 3>;

/// The one table of breakpoint fallbacks: micro to tiny to small, tiny to small,
/// xlarge to large, xxlarge to xlarge to large.
const BreakpointChain& breakpoint_fallback_chain(UiBreakpoint breakpoint);

/// Placement key for a tier on a measured grid, or "" when nothing matches.
///
/// Walks breakpoint_fallback_chain(), trying the grid-qualified form
/// ("<tier>@<cols>x<rows>") before the bare tier name at each rung. Tier
/// specificity dominates: a table entry for this tier on some other grid beats
/// one for a coarser tier on this exact grid, because the tier decides how much
/// text has to fit. The grid breaks ties within a tier, which is the case the
/// UI scale creates: one panel, one tier, a different track count per scale.
///
/// With @p grid_cols / @p grid_rows at 0 only bare tier names match.
std::string choose_placement_key(const nlohmann::json& by_bp, UiBreakpoint breakpoint,
                                 int grid_cols, int grid_rows);

} // namespace helix::layout
