// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "spoolman_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file spool_assign.h
 * @brief The decisions behind the lane spool editor's pages
 *
 * Pure functions over a lane and the user's spools: whether a lane opens on
 * its summary or the empty page, which spools the New Spool list matches
 * against the Filament page (exact first, close after with what differs), the
 * spool tiles a new spool starts from (favorites, then the common spools), a
 * spool's size and remaining weight, and the lane history. No LVGL.
 */

namespace helix::spool_assign {

/// What the describe form says. An empty string, "Generic" for the brand, or
/// no colour means the field is unset and matches anything.
struct SpoolQuery {
    std::string material; ///< "PLA", "PETG HF"
    std::string brand;    ///< "Polymaker"
    std::string product;  ///< the product line or filament name, "PolyTerra"
    std::optional<uint32_t> color;

    [[nodiscard]] bool empty() const;
    bool operator==(const SpoolQuery& o) const {
        return material == o.material && brand == o.brand && product == o.product &&
               color == o.color;
    }
};

/// The fields a close match states differently from the form.
struct CloseFields {
    bool material = false;
    bool brand = false;
    bool product = false;
    bool color = false;

    [[nodiscard]] bool any() const {
        return material || brand || product || color;
    }
};

enum class MatchKind { None, Close, Exact };

struct SpoolMatch {
    MatchKind kind = MatchKind::None;
    CloseFields differs; ///< set only for Close
};

/// Colours within this weighted-RGB distance are the same colour.
inline constexpr int kExactColorDistance = 12;
/// Colours within this distance are offered as close.
inline constexpr int kCloseColorDistance = 60;

/// How @p spool stands against @p query. A field the form sets and the spool
/// leaves unstated (no colour, no vendor) cannot match.
SpoolMatch match_spool(const SpoolQuery& query, const SpoolInfo& spool);

struct RankedSpool {
    size_t index = 0; ///< into the spools passed to rank_spools()
    SpoolMatch match;
};

/// The most rows the list builds: each is a drawn spool, and a few hundred
/// would cost every form tap a long rebuild on a slow board. Search reaches
/// the rest.
inline constexpr size_t kMaxListedSpools = 40;

/// The spools the list shows for @p query: every spool for an empty query, else
/// the exact matches followed by the close ones. Within each group the input
/// order (recency) is kept, and at most @p max rows are returned.
std::vector<RankedSpool> rank_spools(const SpoolQuery& query, const std::vector<SpoolInfo>& spools,
                                     size_t max = kMaxListedSpools);

/// The close-match note, "close: PLA+, similar color": @p note_format (one
/// "{}", a whole translatable phrase) around the spool's own value for each
/// text field that differs and @p color_phrase for a color that does. Empty
/// when nothing differs.
std::string close_note(const CloseFields& differs, const SpoolInfo& spool,
                       const std::string& note_format, const std::string& color_phrase);

/// A starting point for a fresh install: material and colour, nothing else.
struct CommonSpool {
    const char* material;
    const char* color_name;
    uint32_t rgb;
};

/// PLA, PETG, ABS, ASA, each in Black, White and Gray, material-major.
const std::vector<CommonSpool>& common_spools();

/// The spool tiles a new spool starts from.
inline constexpr size_t kSpoolTiles = 12;

/// A starred catalog product.
struct FavoriteTile {
    std::string brand;      ///< its maker ("Creality")
    std::string name;       ///< the product's name ("CR-PETG")
    std::string product_id; ///< its catalog id
};

/// A spool tile's label: the vendor, then the product or material, the
/// vendor said once ("Creality CR-PETG", "Bambu ASA", "Generic PLA").
[[nodiscard]] std::string spool_tile_label(const std::string& vendor, const std::string& name);

/// One spool tile: a favorite (product_id set) or a common spool (common, an
/// index into common_spools()).
struct SpoolTile {
    std::string label;
    uint32_t rgb = 0;
    std::string product_id;
    size_t common = 0;
    [[nodiscard]] bool favorite() const {
        return !product_id.empty();
    }
};

/// The tiles a new spool starts from: @p favorites in star order, then the
/// common spools a color at a time (every material in Black, then White, then
/// Gray), @p count in all, each labelled by spool_tile_label().
[[nodiscard]] std::vector<SpoolTile> spool_tiles(const std::vector<FavoriteTile>& favorites,
                                                 size_t count = kSpoolTiles);

enum class LaneScreen { Assign, Edit };

/// A lane holding a spool (filament its status reports, or a spool it records
/// by link or material) opens on its summary (Edit), an unrecorded one in its
/// unknown state to be described; an empty lane opens on the empty page
/// (Assign). A lane its sensor reports empty is empty, whatever filament it
/// still records: that is what it last held.
[[nodiscard]] LaneScreen screen_for_lane(const SlotInfo& lane);

/// The editor page a lane change can act on: the lane's summary, its empty
/// page, or any other page (a list, a form), which belongs to the user.
enum class EditorPage { Summary, Empty, Other };

/// Where an open editor goes when its lane's state changes.
enum class LaneFollow { Stay, ToSummary, ToEmpty };

/// The editor follows its lane between the summary and the empty page: a
/// spool fed into a lane shown empty opens its summary, a lane that runs out
/// under its summary shows empty. It never moves while @p editing (unsaved
/// edits, a save in flight) nor off any other page.
[[nodiscard]] LaneFollow page_after_lane_change(EditorPage shown, bool editing, LaneScreen lane);

/// What a lane's summary row says a runout will do, where its backup is
/// not the user's to set.
enum class RunoutLine {
    None,            ///< nothing to state: no such feature, or not known yet
    Continues,       ///< continues from the backup slot
    NoMatch,         ///< switched on, but nothing to switch to: pauses
    Off,             ///< switched off: pauses
    FirmwareChooses, ///< on, the firmware picks the spool at runout time
};

/// @p backup is AmsBackend::runout_backup_for(): a slot, -1, or nullopt when
/// the backend cannot answer per slot.
[[nodiscard]] RunoutLine runout_line(const helix::printer::EndlessSpoolCapabilities& caps,
                                     std::optional<int> backup);

/// What a tap on the summary's lane row opens.
enum class LaneTap {
    None,         ///< informational: no chevron
    LanePage,     ///< the lane's own settings
    RefillToggle, ///< the unit's switch for the feature
};

[[nodiscard]] LaneTap lane_tap(bool lane_editable, bool refill_switchable);

/// @p product's name already says @p material as a whole word, case
/// ignored ("Polymaker PolyLite ASA" says "ASA"; "Polymaker Basic" does not
/// say "PLA"), so a line under it need not repeat the material.
[[nodiscard]] bool product_names_material(const std::string& product, const std::string& material);

// === Spool size ===

inline constexpr float kDefaultSpoolGrams = 1000.0f;

/// The size chips, in order. The last chip, Partial, opens the keypad.
inline constexpr float kSizeChipGrams[] = {1000.0f, 750.0f, 250.0f};
inline constexpr int kSizeChipCount = 4;
inline constexpr int kPartialChip = 3;

/// The size chips' state: the lit chip and the grams it stands for.
struct SpoolSize {
    int chip = 0;
    float remaining_g = kDefaultSpoolGrams;
    float total_g = kDefaultSpoolGrams;
};

/// The size Assign Spool starts from on a lane reading @p remaining_g of
/// @p total_g (negative = unknown): a known weight lights its chip, or Partial
/// with that weight, so an assign never writes over a reading; an unknown
/// weight starts at a full 1 kg spool.
[[nodiscard]] SpoolSize size_for_reading(float remaining_g, float total_g);

/// The Edit screen's - / + step.
inline constexpr float kRemainingStepGrams = 10.0f;

/// A linked spool with less than this left is offered for archiving when its
/// lane is cleared.
inline constexpr float kArchiveBelowGrams = 20.0f;

/// @p remaining_g moved by @p delta_g, kept at 0 or more and, when the spool's
/// total is known, at most that total.
[[nodiscard]] float step_remaining(float remaining_g, float delta_g, float total_g);

/// Whether Undo of a save that put @p after on a lane still applies to the lane
/// as it is @p now: the same spool link, material, brand and color.
[[nodiscard]] bool undo_still_applies(const SlotInfo& now, const SlotInfo& after);

/// Whether a save that took a lane from @p before to @p after changed anything
/// on the lane (its spool link, identity or weights), and so left something an
/// Undo could put back.
[[nodiscard]] bool save_changed_lane(const SlotInfo& before, const SlotInfo& after);

} // namespace helix::spool_assign
