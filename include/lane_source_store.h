// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"
#include "lane_sources.h"

#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace helix::ams {

/// A filament position anywhere on the printer. NOT a slot index: several
/// backends coexist, so a bare slot index would put one backend's lane 0 on
/// another's.
using LaneId = int;

/// Not a lane. Every way of naming a lane yields this when it cannot name a
/// real one, and the funnels drop it rather than writing, so a position that
/// cannot be addressed files no record instead of one on a neighbour's lane.
constexpr LaneId INVALID_LANE_ID = -1;

/// Ids reserved for one backend's slots. A backend's slot count follows its
/// firmware rather than AmsState::MAX_SLOTS, which bounds only how many slots
/// get subjects: AFC reports one lane per unit it finds and Happy Hare one per
/// gate, both uncapped, so a five-unit BoxTurtle or a twenty-gate MMU is
/// ordinary hardware. Sized well past any of them; the only cost is integer
/// range.
constexpr int LANES_PER_BACKEND = 256;

/// Blocks reserved for backends. AmsState::backends_ holds one to three in
/// practice - a filament system beside a tool changer is the wide case.
constexpr int MAX_BACKENDS = 8;

/// The bypass / external spool, which belongs to the printer rather than to a
/// backend. Above every backend block so adding backends never reaches it.
constexpr LaneId BYPASS_LANE_ID = 10000;

/// Direct-drive tools, one id per tool from here up. A tool changer's spools
/// are lanes like any other and stop needing a parallel store.
constexpr LaneId FIRST_TOOL_LANE_ID = 20000;

/// Ids the tool block holds. Nothing in the tree caps a tool count -
/// ToolState::tools_ is an unbounded vector - and the widest tool ceiling that
/// does exist is AFC's tool number at 64, so this is a deliberate ceiling
/// rather than a mirror of an existing one.
constexpr int MAX_TOOL_LANES = 256;

/// One past the last id this scheme assigns. A reserved block added later
/// starts here; everything from here up is refused.
constexpr LaneId END_LANE_ID = FIRST_TOOL_LANE_ID + MAX_TOOL_LANES;

/// Every id the scheme can assign: one block per backend, the bypass, and the
/// tool lanes. This is the store's size bound, since only these ids are
/// accepted.
constexpr int MAX_LANES = MAX_BACKENDS * LANES_PER_BACKEND + 1 + MAX_TOOL_LANES;

static_assert(MAX_BACKENDS * LANES_PER_BACKEND <= BYPASS_LANE_ID,
              "a backend block must not reach the bypass lane id");
static_assert(BYPASS_LANE_ID < FIRST_TOOL_LANE_ID,
              "the bypass id must not fall inside the tool block");
static_assert(END_LANE_ID > FIRST_TOOL_LANE_ID,
              "the tool block must not overflow a LaneId, or a block placed above it "
              "would wrap back inside it");

/// True when @p lane is an id this scheme assigns: a slot on one of the
/// backend blocks, the bypass, or a tool. Nothing else is a lane, and a
/// positive integer is not a lane merely for being positive - not the gap
/// between the last backend block and the bypass, not an id one past the
/// bypass, not anything from END_LANE_ID up. The funnels drop what this
/// refuses, so a caller that computes an id wrongly is told about it rather
/// than handed a lane of its own.
[[nodiscard]] constexpr bool is_lane_id(LaneId lane) {
    if (lane >= 0 && lane < MAX_BACKENDS * LANES_PER_BACKEND)
        return true;
    if (lane == BYPASS_LANE_ID)
        return true;
    return lane >= FIRST_TOOL_LANE_ID && lane < END_LANE_ID;
}

/// The lane id for @p slot_index on the backend registered at @p backend_index,
/// or INVALID_LANE_ID when the pair names no lane. AmsState::add_backend hands
/// a backend its own index, which combines with a slot index here to give the
/// backend its own block of lane ids.
///
/// The blocks are adjacent, so a slot index at or past LANES_PER_BACKEND is
/// not an unused id: it is the neighbouring backend's slot, and a record filed
/// there is a record on the wrong lane, the failure this store exists to
/// remove. Out of range therefore yields no id at all rather than the nearest
/// one, which is a statement a caller can act on and a test can pin.
[[nodiscard]] constexpr LaneId lane_id_for(int backend_index, int slot_index) {
    if (backend_index < 0 || backend_index >= MAX_BACKENDS)
        return INVALID_LANE_ID;
    if (slot_index < 0 || slot_index >= LANES_PER_BACKEND)
        return INVALID_LANE_ID;
    return backend_index * LANES_PER_BACKEND + slot_index;
}

/// The one way a non-UI source writes this store. Replaces this lane's record
/// for obs.source whole and leaves every other source untouched. A field the
/// source did not observe stops contributing, which is what keeps a stale
/// frame from re-asserting a value its author has stopped standing behind.
/// obs.source must not be LocalUser; commit_slot_edit() is that source's only
/// funnel.
///
/// A lane that is not a lane is warned about and dropped.
void ingest(LaneId lane, const Observation& obs);

/// The one way a human edit writes this store. Unlike ingest(), this AMENDS
/// the user's record field by field: a person states what they changed, and
/// what they declared earlier still stands. obs.source must be LocalUser;
/// every other source's only funnel is ingest().
///
/// This is the declaration layer: it records the user's authorship as a lane
/// source record. AmsState::commit_slot_edit (ams_state.h) is the method
/// layer that performs the edit against every backing store.
///
/// A lane that is not a lane is warned about and dropped.
void commit_slot_edit(LaneId lane, const Observation& obs);

/// This lane's records, by value. An unwritten lane reads as nothing observed.
[[nodiscard]] LaneSources lane_sources(LaneId lane);

/// Every lane that has been written, ascending.
[[nodiscard]] std::vector<LaneId> known_lanes();

/// Forget every lane. Backend registration stamps indices from 0 again after a
/// teardown, so one printer's block 0 becomes the next printer's, and a record
/// that outlived the backend it was made through would describe hardware
/// nobody edited. Called from AmsState::clear_backends().
///
/// This drops records for a reconnect to the same printer too. Nothing keys
/// the store per printer, and re-deriving a record a machine still reports is
/// cheap where inheriting a stranger's is the failure this store exists to
/// remove; plan 4 owns the per-printer key if one is wanted.
void reset_lane_sources();

/// Holds one LaneSources per lane until the backends that wrote them go away.
/// Bounded by construction: the funnels accept only ids is_lane_id() admits, so
/// the map can never hold more than MAX_LANES (2305) entries.
///
/// write() is private with exactly two friends: ingest() and commit_slot_edit().
/// Those two are the only code that can reach a lane's records; a third friend
/// would be a third writer.
class LaneSourceStore {
  public:
    static LaneSourceStore& instance();

    [[nodiscard]] LaneSources get(LaneId lane) const;
    [[nodiscard]] std::vector<LaneId> lanes() const;

    /// Discard every lane's records. Not a writer - it files nothing, so it
    /// needs none of the friendship write() is guarded by.
    void clear();

    LaneSourceStore(const LaneSourceStore&) = delete;
    LaneSourceStore& operator=(const LaneSourceStore&) = delete;

  private:
    LaneSourceStore() = default;

    /// @p amend false replaces the source's record whole; true merges the
    /// observed fields onto whatever that source already holds.
    void write(LaneId lane, const Observation& obs, bool amend);

    /// True when @p lane is not the id the last dropped-lane warning named,
    /// latching it so the next call about the same id is false.
    ///
    /// The message carries the id and nothing else, so repeating it for one id
    /// tells a reader nothing they have not been told. A producer filing
    /// through a backend that has no index yet reaches it three times per lane
    /// per frame, which is enough to push unrelated lines out of a test's log
    /// ring. A changed id is a different fact and speaks again.
    bool first_drop_of(LaneId lane);

    friend void ingest(LaneId, const Observation&);
    friend void commit_slot_edit(LaneId, const Observation&);

    /// Backends parse on the main thread, but the Spoolman and database
    /// callbacks that will feed this in plan 4 land on an HTTP worker.
    mutable std::mutex mutex_;
    std::map<LaneId, LaneSources> lanes_;

    /// The lane id the last dropped-lane warning named. Cleared with the
    /// lanes, so a test that wants the warning gets it: every fixture calls
    /// reset_lane_sources().
    std::optional<LaneId> warned_drop_lane_;
};

} // namespace helix::ams
