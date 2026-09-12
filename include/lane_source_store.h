// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"
#include "lane_sources.h"

#include <map>
#include <mutex>
#include <vector>

class LaneSourceStoreTestAccess; // NAMESPACE_OK: matches the global-namespace class in
                                 // tests/test_helpers/lane_source_store_test_access.h

namespace helix::ams {

/// A filament position anywhere on the printer. NOT a slot index: several
/// backends coexist, so a bare slot index would put one backend's lane 0 on
/// another's.
using LaneId = int;

/// Ids per backend block. Matches AmsState::MAX_SLOTS, which is how many slots
/// a backend can have subjects for, so a lane that can be shown has an id.
constexpr int LANES_PER_BACKEND = 16;

/// The bypass / external spool, which belongs to the printer rather than to a
/// backend. Far above the backend blocks so adding backends never reaches it.
constexpr LaneId BYPASS_LANE_ID = 1000;

/// Direct-drive tools, one id per tool from here up. A tool changer's spools
/// are lanes like any other and stop needing a parallel store.
constexpr LaneId FIRST_TOOL_LANE_ID = 2000;

/// The lane id for @p slot_index on the backend registered at @p backend_index.
/// AmsState::add_backend hands a backend its own index; a backend reaches this
/// through AmsBackend::lane_id() rather than passing its index around.
[[nodiscard]] constexpr LaneId lane_id_for(int backend_index, int slot_index) {
    return backend_index * LANES_PER_BACKEND + slot_index;
}

/// The one way a non-UI source reaches a lane. Replaces this lane's record for
/// obs.source whole and leaves every other source untouched. A field the
/// source did not observe stops contributing, which is what keeps a stale
/// frame from re-asserting a value its author has stopped standing behind.
void ingest(LaneId lane, const Observation& obs);

/// The one way a human edit reaches a lane. Unlike ingest(), this AMENDS the
/// user's record field by field: a person states what they changed, and what
/// they declared earlier still stands. obs.source must be LocalUser.
void commit_slot_edit(LaneId lane, const Observation& obs);

/// This lane's records, by value. An unwritten lane reads as nothing observed.
[[nodiscard]] LaneSources lane_sources(LaneId lane);

/// Every lane that has been written, ascending.
[[nodiscard]] std::vector<LaneId> known_lanes();

/// Holds one LaneSources per lane for the life of the process.
///
/// write() is private with exactly three friends: ingest(), commit_slot_edit()
/// and ::LaneSourceStoreTestAccess. Those three are the only code that can
/// reach a lane's records; a fourth friend would be a third writer.
/// Widening that friend list is what tests/shell/test_code_lint.bats watches for.
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
    friend class ::LaneSourceStoreTestAccess;

    /// Backends parse on the main thread, but the Spoolman and database
    /// callbacks that will feed this in plan 4 land on an HTTP worker.
    mutable std::mutex mutex_;
    std::map<LaneId, LaneSources> lanes_;
};

} // namespace helix::ams
