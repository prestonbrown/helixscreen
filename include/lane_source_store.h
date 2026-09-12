// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"
#include "lane_sources.h"

#include <map>
#include <mutex>
#include <vector>

namespace helix::ams {

class LaneSourceStoreTestAccess;

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

static_assert(MAX_BACKENDS * LANES_PER_BACKEND <= BYPASS_LANE_ID,
              "a backend block must not reach the bypass lane id");
static_assert(BYPASS_LANE_ID < FIRST_TOOL_LANE_ID,
              "the bypass id must not fall inside the tool block");

/// True when @p lane names a position. The scheme's one non-position is
/// INVALID_LANE_ID, and every other negative value is equally not a lane.
[[nodiscard]] constexpr bool is_lane_id(LaneId lane) {
    return lane >= 0;
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
///
/// A lane that is not a lane is warned about and dropped.
void ingest(LaneId lane, const Observation& obs);

/// The one way a human edit writes this store. Unlike ingest(), this AMENDS
/// the user's record field by field: a person states what they changed, and
/// what they declared earlier still stands. obs.source must be LocalUser.
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

/// Holds one LaneSources per lane for the life of the process.
///
/// write() is private with exactly three friends: ingest(), commit_slot_edit()
/// and LaneSourceStoreTestAccess. Those three are the only code that can
/// reach a lane's records; a fourth friend would be a third writer.
class LaneSourceStore {
  public:
    static LaneSourceStore& instance();

    [[nodiscard]] LaneSources get(LaneId lane) const;
    [[nodiscard]] std::vector<LaneId> lanes() const;

    LaneSourceStore(const LaneSourceStore&) = delete;
    LaneSourceStore& operator=(const LaneSourceStore&) = delete;

  private:
    LaneSourceStore() = default;

    /// @p amend false replaces the source's record whole; true merges the
    /// observed fields onto whatever that source already holds.
    void write(LaneId lane, const Observation& obs, bool amend);

    friend void ingest(LaneId, const Observation&);
    friend void commit_slot_edit(LaneId, const Observation&);
    friend class LaneSourceStoreTestAccess;

    /// Backends parse on the main thread, but the Spoolman and database
    /// callbacks that will feed this in plan 4 land on an HTTP worker.
    mutable std::mutex mutex_;
    std::map<LaneId, LaneSources> lanes_;
};

} // namespace helix::ams
