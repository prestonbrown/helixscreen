// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_slot_override.h"
#include "lane_observation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix {
struct SlotInfo;
}

namespace helix::ams {

/// The edit a commit writes, as a Pending observation: every field that moved
/// between what the editor opened on and what it committed, minus the moves no
/// person made.
///
///   - catalog_id and product_name arrive from the editor's auto-highlighted
///     product, which is why AmsEditOverlay::is_dirty() excludes them too;
///   - a cleared colour or weight lands on a sentinel (kAmsDefaultSlotColor, a
///     negative weight) that means "no reading", never a chosen value;
///   - on an edit that keeps a linked spool, material, brand, spool name and
///     Spoolman vendor id are the spool's statement, not the edit's.
///
/// spoolman_id is engaged exactly when the binding changed, an unlink as 0. A
/// cleared text field or a zeroed id travels engaged-empty, and filing it
/// withdraws the field's earlier pending value.
[[nodiscard]] Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited);

/// A stored lane record as a Record observation, whole. Only the fields the
/// record actually carries are observed: unknown is unset.
[[nodiscard]] Observation record_observation(const FilamentSlotOverride& record);

/// @p edited with every field a linked spool owns taken from the spool.
///
/// Applies to an edit that keeps the same positive spool id. Material, brand,
/// spool name and Spoolman vendor id then come from @p spool_record, the lane's
/// Spoolman record, where it names that spool and states the field, and from
/// @p original otherwise. Every other field, and every edit that changes or
/// clears the binding, passes through as @p edited has it.
[[nodiscard]] SlotInfo keep_spool_owned_identity(const SlotInfo& original, const SlotInfo& edited,
                                                 const std::optional<Observation>& spool_record);

/// Blank @p edited's spool name when the edit re-declared the spool under it.
///
/// A spool name describes the spool the brand, material and catalog product
/// named, a free-form name the user typed as much as a vendor's product line.
/// An edit that moves any of them (brand and material compared without case)
/// and leaves the name as it was would label the new spool with the old one's
/// name. A first catalog pick is not a move: the editor adopts one on
/// every Save. A name the edit itself changed stands, and so does any name on
/// a linked lane, which is the spool's own. Returns true when it blanked.
bool drop_stale_spool_name(const SlotInfo& original, SlotInfo& edited);

/// The field names of the fields a linked
/// spool owns that @p edited moved away from @p original and @p applied does
/// not carry. Empty for an edit that changes or clears the binding.
[[nodiscard]] std::vector<std::string_view> spool_owned_fields_dropped(const SlotInfo& original,
                                                                       const SlotInfo& edited,
                                                                       const SlotInfo& applied);

/// What a lane-shaped record's colour string says.
enum class ColorReadingKind {
    Observed,  ///< A colour, in ColorReading::rgb.
    Cleared,   ///< The producer states this lane has no colour.
    NoReading, ///< The producer said something that is not a colour.
};

/// Three answers, not two, because the consumers need different ones: SlotInfo
/// has no uint32_t that means "leave this alone", and an Observation's unset
/// field means "not observed", which is not the same statement as "this lane
/// has no colour".
struct ColorReading {
    ColorReadingKind kind;
    uint32_t rgb{0}; ///< Meaningful only when kind is Observed.
};

/// The colour @p raw states, for a caller that has already pulled the string
/// off whatever key its own wire spells it under.
///
/// It takes the string rather than the record because the key is the wire's
/// business and differs per producer, where the rule for reading the value
/// does not. The hex grammar is helix::parse_hex_color's, so `#RGB`, bare
/// `RRGGBB`, an `0x` prefix and the 8-digit `#RRGGBBAA` slicers emit all mean
/// here what they mean everywhere else in the tree, and a value with anything
/// else in it is refused rather than half-read.
///
/// Empty, or nothing but a `#`, is Cleared: a producer wiping a lane writes
/// the key empty, and AFC's SET_COLOR with no value stores the bare prefix.
///
/// This answers "what did the producer say", which is not the question
/// is_declarable_color (declared below) answers. kAmsDefaultSlotColor
/// written on a wire is a producer stating a grey and reads as Observed here;
/// the same value sitting in a struct is that struct's "no reading" sentinel
/// and is refused there.
[[nodiscard]] ColorReading read_lane_color(const std::string& raw);

/// True when a colour resting in a SlotInfo-shaped struct is a reading rather
/// than that struct's own "no colour" default. kAmsDefaultSlotColor is where
/// a cleared slot and a colourless record both land, so filing it would hand
/// every one of them a declared grey.
///
/// This is the struct-side counterpart of read_lane_color, and deliberately a
/// different answer: a producer writing #808080 on a WIRE is stating a grey,
/// where a struct resting on its default is not. A translation that already
/// holds the producer's string asks read_lane_color; one whose only access to
/// the colour is a decoded uint32_t asks this.
[[nodiscard]] bool is_declarable_color(uint32_t rgb);

/// The nearest colour to @p rgb that is_declarable_color accepts. A colour a
/// user picks is a statement of intent, so a pick that lands exactly on the
/// "no colour" marker becomes the visually identical 0x7F7F7F instead of being
/// read back as no colour. Every colour picker routes its result through this.
[[nodiscard]] uint32_t picked_color(uint32_t rgb);

} // namespace helix::ams
