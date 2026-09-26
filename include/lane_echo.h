// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"

#include <cstdint>
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
    /// Replaces the slot's active declaration. An ARMED predecessor is not
    /// lost: stage() stashes it, arm() carries its still-declared fields
    /// forward onto the new staging, and a matched abandon() restores it
    /// whole. Pair every stage() with arm() or abandon(): a staging left
    /// unarmed withholds nothing, but it lingers until the next stage()
    /// replaces it.
    ///
    /// @return The staging's sequence stamp. A backend whose dispatch can fail
    ///         after the call returns (an HTTP response, a timer) captures
    ///         the stamp and passes it to the matched abandon(), so a failure
    ///         answer landing after a later edit restaged the slot cancels
    ///         only the edit it belongs to.
    std::uint64_t stage(int slot_index, Observation declared);

    /// The stamp of the slot's current staging, armed or not: 0 when the slot
    /// holds none. A caller that cannot see stage()'s return (a commit funnel
    /// around the backend's own apply) captures this before the dispatch and
    /// answers it to the matched abandon(), so a refusal cancels only the
    /// staging the refused edit created.
    [[nodiscard]] std::uint64_t staged_sequence(int slot_index) const;

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
    /// A staging that restates only some of what its predecessor declared
    /// inherits the rest, and one that declares nothing suppressible
    /// inherits all of it: the write an edit triggers re-sends every
    /// identity field, so the predecessor's un-restated declarations explain
    /// those echoes as much as the restated ones do. A restated field
    /// replaces the value it carried in with. The carry happens only when
    /// the boundary still names the spool the predecessor armed against; a
    /// differing boundary is a different physical spool, whose readings the
    /// predecessor's write does not explain, so there the carry drops and
    /// the new declaration stands alone.
    ///
    /// A staging left declaring nothing suppressible after that - no
    /// predecessor carried, nothing of its own - is dropped rather than
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
    ///
    /// This form drops whatever the slot holds, whatever edit staged it,
    /// including any predecessor the staging suspended. It is for a caller
    /// that knows no write is outstanding, such as a boundary event on this
    /// slot, not for an answer about one particular dispatch.
    void abandon(int slot_index);

    /// The matched form: the failure answer of the staging @p staged_sequence
    /// came from. A stamp naming a staging this slot no longer holds belongs
    /// to a superseded edit and drops nothing, so a slow failure of edit 1
    /// cannot cancel edit 2's guard. A staging that suspended an armed
    /// predecessor restores it: the failed write's echo is not coming, but
    /// the predecessor's went out and firmware is still repeating it.
    void abandon(int slot_index, std::uint64_t staged_sequence);

    /// The matched, partial form: the failure answer of ONE command of the
    /// staging @p staged_sequence, whose write carried the fields @p fields
    /// holds values in. No echo of those is coming, so their declarations
    /// come off - replaced by the suspended predecessor's for the same
    /// fields, because firmware still holds whatever the predecessor's write
    /// put there and keeps repeating it. The staging's other fields stay
    /// armed: their commands went out, and their echoes are owed the guard a
    /// whole abandon() would drop. A staging left declaring nothing
    /// suppressible falls back to the matched abandon().
    void abandon_fields(int slot_index, std::uint64_t staged_sequence, const Observation& fields);

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
    ///
    /// @p cleared names the fields this frame carried and read as a clear:
    /// the key present, the value empty. Only presence is read. The producer
    /// stating "no value" is a statement about the field, not silence, so it
    /// releases that field's declaration the way a differing value does; a
    /// declaration with every field released is dropped.
    int withhold(int slot_index, const std::string& boundary, Observation& producer_record,
                 const Observation& cleared = Observation{ObservationSource::VendorCache});

    /// Remove from @p producer_record every suppressible field whose value
    /// equals this slot's armed declaration, and return how many were
    /// removed. Releases nothing and consumes nothing: withhold() alone
    /// decides whether a declaration stands, by judging the fields a frame
    /// itself stated. This is the filing half for producers whose record
    /// accumulates: a field a later frame is silent about carries the last
    /// filed value forward, which for an echoed field is our own write, and
    /// filing it would put the abandoned edit back as the machine's word one
    /// frame after the echo was withheld.
    int strip_standing(int slot_index, Observation& producer_record) const;

    /// Whether an armed declaration stands on @p slot_index: a write of ours
    /// firmware may still echo. A record read while one stands cannot be
    /// judged the newest statement on its lane - our own write is the newest
    /// thing that happened to the lane, and the record is either that write's
    /// echo or something older - so a caller deciding newest-edit-wins must
    /// decline while this is true and let the strip alone decide what files.
    /// An unarmed staging suppresses nothing and its write never went out,
    /// so it does not count.
    [[nodiscard]] bool standing(int slot_index) const;

  private:
    struct Entry {
        Observation declared{ObservationSource::LocalUser};
        /// Token naming the spool the write was made against. Meaningful only
        /// once armed.
        std::string boundary;
        /// False between stage() and arm(). withhold() ignores an unarmed
        /// entry, so a backend that stages and then bails suppresses nothing.
        bool armed{false};
        /// Which edit staged this entry; matched abandon() answers to it.
        std::uint64_t sequence{0};
        /// The armed entry this staging replaced, held for arm() to carry
        /// forward and for a matched abandon() to restore. An edit declares
        /// only what moved while its write re-sends every identity field, so
        /// the un-restated declarations still explain the echoes.
        Observation carry_declared{ObservationSource::LocalUser};
        std::string carry_boundary;
        bool carry_armed{false};
        /// The predecessor's own staging stamp, so a restore lands under a
        /// stamp a duplicated failure answer can no longer reach.
        std::uint64_t carry_sequence{0};
    };

    std::unordered_map<int, Entry> entries_;
    std::uint64_t next_sequence_{0};
};

} // namespace helix::ams
