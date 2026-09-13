// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"

#include <string>
#include <unordered_map>

namespace helix::ams {

/// Per-slot record of what a user declared in one edit, which of those fields
/// the backend's write to firmware actually carried, and the boundary token
/// the write was made against. A producer record is filtered through it so a
/// backend does not read its own user's edit back and file it as firmware
/// truth.
///
/// Several backends write filament identity back to firmware when the user
/// edits a lane, and firmware republishes it through the same object a tag or
/// a saved variable is read from, spelled the same way. A value repeating our
/// own write is not a reading. Filing it as VendorCache changes nothing on
/// screen while the user's override stands, because resolve() ranks LocalUser
/// above VendorCache; the harm lands once the override is cleared, when the
/// lane asserts an abandoned edit as what the machine said and has no way
/// back to firmware truth. So the suppression has to outlive the override
/// that caused it, which is why its lifetime comes from a boundary the
/// backend states rather than from the override record.
///
/// Held by each backend that writes identity back, rather than by AmsBackend:
/// the consumer is each backend's own parse, which is SlotFingerprintTracker's
/// category and not own_write_expectations_'.
///
/// @warning Every method requires the holder's own mutex. The class takes no
///          lock, because a backend consults it inside the parse that
///          produced the value and those mutexes are not recursive.
class OwnWriteEchoes {
  public:
    /// Stage what the user declared in this edit of @p slot_index.
    ///
    /// @p declared must come from user_edit_observation(), never a per-field
    /// delta recomputed beside it. That function is the definition of what a
    /// person declared in an edit and it is what commit_slot_edit files as
    /// LocalUser, so deriving from it is what makes the two layers partition
    /// the fields instead of two copies of one rule agreeing by convention.
    /// A binding change is where a recomputed delta stops agreeing: linking a
    /// spool carries its colour, brand and material into the same commit and
    /// nobody chose those.
    ///
    /// Replaces whatever this slot held. Pair every stage() with arm() or
    /// abandon(): a staging left unarmed withholds nothing, but it lingers
    /// until the next stage() replaces it.
    void stage(int slot_index, Observation declared);

    /// The staged declaration, for the caller to prune down to the fields its
    /// write actually carried and to relocate any field its read path spells
    /// under a different name. nullptr when nothing is staged.
    ///
    /// The prune is the per-backend half and it is where correctness lives. A
    /// field armed that the write did not send withholds a genuine firmware
    /// reading until the boundary moves, and nothing here can catch that. The
    /// usual case is a field the user cleared: a write omits an empty key, so
    /// firmware keeps the value it had and what returns is firmware's.
    [[nodiscard]] Observation* staged(int slot_index);

    /// Arm the pruned staging against @p boundary, the token naming the
    /// physical spool the write was made against.
    ///
    /// A staging with no suppressible field left is dropped rather than
    /// armed, which is what makes a preview, a refused dispatch and a
    /// binding-only edit safe with no extra test at the call site.
    ///
    /// An empty @p boundary arms against "no token read yet", so the first
    /// non-empty token withhold() sees ends the suppression. That is a
    /// boundary, and it is not the same statement as a lane whose producer
    /// can never name a spool: arming there would withhold until the next
    /// edit, so such a lane must call abandon() instead of arm().
    void arm(int slot_index, std::string boundary);

    /// The write never went out, so no echo is coming. Drops the staging.
    void abandon(int slot_index);

    /// Remove from @p producer_record every field whose value repeats this
    /// slot's armed declaration, and return how many were removed. The count
    /// is the only handle a consumer has on the difference between a field
    /// this withheld and one the producer never mentioned, since the filtered
    /// record states neither.
    ///
    /// A @p boundary naming something other than the token the write was
    /// armed against is a different physical spool, so what we wrote to the
    /// old one has stopped explaining what is read now: the slot disarms and
    /// nothing is removed. An EMPTY boundary is the producer saying nothing
    /// this frame, which is not a change.
    ///
    /// A field the producer states differently consumes its own declaration,
    /// on top of the boundary rather than instead of it. The producer has
    /// demonstrated it can say something other than what we wrote, so that
    /// field is its own statement from here on. Value-difference releases the
    /// common swap early; the boundary catches a swap to a spool that reads
    /// identically, which value-difference cannot see.
    int withhold(int slot_index, const std::string& boundary, Observation& producer_record);

  private:
    struct Entry {
        Observation declared{ObservationSource::LocalUser};
        /// Token naming the spool the write was made against. Meaningful only
        /// once armed.
        std::string boundary;
        /// False between stage() and arm(). withhold() ignores an unarmed
        /// entry, so a backend that stages and then bails suppresses nothing.
        bool armed{false};
    };

    std::unordered_map<int, Entry> entries_;
};

} // namespace helix::ams
