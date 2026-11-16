// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_source_store.h"
#include "lane_sources.h"

#include <cstdint>
#include <optional>
#include <string>

namespace helix::ams {

/// Drop what describes the spool a lane held before its binding changed: a
/// pending edit and the Spoolman details. Firmware, Record, Sensed and Metered
/// stay; the first two are what Moonraker holds, and a backend that erases its
/// stored record clears Record through reset_lane_to_machine_readings().
///
/// A lane that is not a lane is dropped, silently, like drop_lane_source().
void drop_previous_spool_declarations(LaneId lane);

/// What the hardware physically read off the spool in a lane, and nothing
/// else (prestonbrown/helixscreen#1710).
///
/// Every field is evidence about THIS spool only when the reader in the slot
/// produced it: a tag UID, or material and colour decoded from a tag. A value
/// the firmware remembers across inserts (a colour the user set on the
/// printer's own menu, a saved slot table) is not a read of the spool now in
/// the slot, and leaving it out is what makes an untagged insert classify as
/// NoEvidence instead of as the spool that was there before.
struct SpoolEvidence {
    /// A per-spool tag identifier nobody can set through the UI. Empty when
    /// the spool has no tag or the reader has not read it yet.
    std::string tag_uid;
    /// Material decoded from the spool. Empty when none was read.
    std::string material;
    /// Colour decoded from the spool, 0xRRGGBB, the primary colour of a
    /// multi-colour spool. nullopt when none was read, never the no-colour
    /// sentinel.
    std::optional<uint32_t> color_rgb;
    /// The reader finished with this spool, so an empty tag_uid means the
    /// spool carries no tag rather than "not read yet". A non-empty tag_uid
    /// implies it.
    bool tag_read_complete = false;
};

/// Whether the spool just inserted into a lane is the one that was there.
enum class InsertVerdict {
    SameSpool, ///< Keep everything, silently.
    /// Drop what described the previous spool, as a re-bind does
    /// (drop_previous_spool_declarations()). Firmware is left alone: it
    /// already holds the new spool's reading.
    DifferentSpool,
    NoEvidence, ///< Keep everything, and ask the user whether it is the same spool.
};

/// Compare what the hardware read off the inserted spool against what it read
/// off the one before. `before` is nullopt when no reading of the previous
/// spool exists.
///
/// A finished tag read on both sides decides alone when either found a tag:
/// a new tag is a new spool whatever its contents say, the same tag is the
/// same spool even when its contents were rewritten, and a tag appearing or
/// disappearing is a different spool. Otherwise material and colour decide, and they
/// must BOTH match to call it the same spool: two spools of one material and
/// colour are interchangeable, and a manufacturer change between them is the
/// user's to correct. Any field read on both sides that differs is a
/// different spool. Anything less is no evidence.
///
/// Pure: same inputs, same answer, no clock, no globals, no I/O, no logging.
[[nodiscard]] InsertVerdict classify_insert(const std::optional<SpoolEvidence>& before,
                                            const SpoolEvidence& after);

} // namespace helix::ams
