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

/// The spools the list shows for @p query: every spool for an empty query, else
/// the exact matches followed by the close ones. Within each group the input
/// order (recency) is kept.
std::vector<RankedSpool> rank_spools(const SpoolQuery& query, const std::vector<SpoolInfo>& spools);

/// "close: PLA+, color", naming the spool's own value for each text field.
/// Empty when nothing differs. @p color_word is the localized word for colour.
std::string close_note(const CloseFields& differs, const SpoolInfo& spool,
                       const std::string& color_word);

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

/// The Edit screen's - / + step.
inline constexpr float kRemainingStepGrams = 10.0f;

/// @p remaining_g moved by @p delta_g, kept at 0 or more and, when the spool's
/// total is known, at most that total.
[[nodiscard]] float step_remaining(float remaining_g, float delta_g, float total_g);

/// A new spool with no weight yet holds a full default spool.
void apply_new_spool_default_weight(SlotInfo& slot);

} // namespace helix::spool_assign
