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
 * @brief The decisions behind the Assign Spool / Edit Spool screens
 *
 * Pure functions over the describe form and the user's spools: which spools
 * match what the form says (exact first, close after with what differs),
 * which screen a lane opens to, which action the screen offers, the spool
 * sizes and the common spools a fresh install starts from. No LVGL.
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

/// The grid of common spools shows until the user has this many spools.
inline constexpr size_t kFewSpools = 3;
[[nodiscard]] bool show_common_spools(size_t spool_count);

enum class LaneScreen { Assign, Edit };

/// A lane holding a spool (linked, or described by a material) opens on Edit
/// Spool; an empty lane opens on Assign Spool.
[[nodiscard]] LaneScreen screen_for_lane(const SlotInfo& lane);

/// The action at the bottom of the spools list.
enum class BottomAction {
    AddNewSecondary,     ///< "Not Listed? Add New Spool", dashed: the list has spools to Use
    AddToSpoolmanAndUse, ///< "Add to Spoolman & Assign", primary: nothing listed to Use
    AssignLocal,         ///< "Assign", primary: no Spoolman
};

[[nodiscard]] BottomAction bottom_action(bool spoolman_connected, size_t listed);

/// The quiet "assign without adding to Spoolman" link shows under the primary.
[[nodiscard]] inline bool offers_local_assign(BottomAction action) {
    return action == BottomAction::AddToSpoolmanAndUse;
}

// === Spool size ===

inline constexpr float kDefaultSpoolGrams = 1000.0f;

/// The size chips, in order. The last chip, Partial, opens the keypad.
inline constexpr float kSizeChipGrams[] = {1000.0f, 750.0f, 250.0f};
inline constexpr int kSizeChipCount = 4;
inline constexpr int kPartialChip = 3;

/// The chip a spool with @p remaining_g of @p total_g left lights: a full
/// spool of a chip's size lights that chip, anything else lights Partial.
[[nodiscard]] int size_chip_for(float remaining_g, float total_g);

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

/// A new spool with no weight yet holds a full default spool.
void apply_new_spool_default_weight(SlotInfo& slot);

} // namespace helix::spool_assign
