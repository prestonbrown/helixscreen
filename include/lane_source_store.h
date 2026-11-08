// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_observation.h"
#include "lane_sources.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace helix::ams {

/// A filament position anywhere on the printer. NOT a slot index: several
/// backends coexist, so a bare slot index would put one backend's lane 0 on
/// another's. A distinct type so that mistake does not compile: lane_id_for()
/// and AmsBackend::lane_id() are how a slot becomes a lane.
enum class LaneId : int {};

/// The integer a lane id is stored and logged as.
[[nodiscard]] constexpr int lane_id_value(LaneId lane) {
    return static_cast<int>(lane);
}

/// fmt / spdlog formatting: a lane id logs as its number.
[[nodiscard]] constexpr int format_as(LaneId lane) {
    return lane_id_value(lane);
}

/// Not a lane. Every way of naming a lane yields this when it cannot name a
/// real one, and the funnels drop it rather than writing, so a position that
/// cannot be addressed files no record instead of one on a neighbour's lane.
constexpr LaneId kInvalidLaneId{-1};

/// Ids reserved for one backend's slots. A backend's slot count follows its
/// firmware rather than AmsState::kMaxSlots, which bounds only how many slots
/// get subjects: AFC reports one lane per unit it finds and Happy Hare one per
/// gate, both uncapped, so a five-unit BoxTurtle or a twenty-gate MMU is
/// ordinary hardware. Sized well past any of them; the only cost is integer
/// range.
constexpr int kLanesPerBackend = 256;

/// Blocks reserved for backends. AmsState::backends_ holds one to three in
/// practice - a filament system beside a tool changer is the wide case.
constexpr int kMaxBackends = 8;

/// The bypass / external spool, which belongs to the printer rather than to a
/// backend. Above every backend block so adding backends never reaches it.
constexpr LaneId kBypassLaneId{10000};

/// The slot index the bypass lane's records carry: the same -2 every backend
/// reports as current_slot while the external spool feeds the toolhead.
constexpr int kBypassSlotIndex = -2;

/// Direct-drive tools, one id per tool from here up. A tool changer's spools
/// are lanes like any other and stop needing a parallel store.
constexpr LaneId kFirstToolLaneId{20000};

/// Ids the tool block holds. Nothing in the tree caps a tool count -
/// ToolState::tools_ is an unbounded vector - and the widest tool ceiling that
/// does exist is AFC's tool number at 64, so this is a deliberate ceiling
/// rather than a mirror of an existing one.
constexpr int kMaxToolLanes = 256;

/// One past the last id this scheme assigns. A reserved block added later
/// starts here; everything from here up is refused.
constexpr LaneId kEndLaneId{lane_id_value(kFirstToolLaneId) + kMaxToolLanes};

/// Every id the scheme can assign: one block per backend, the bypass, and the
/// tool lanes. This is the store's size bound, since only these ids are
/// accepted.
constexpr int kMaxLanes = kMaxBackends * kLanesPerBackend + 1 + kMaxToolLanes;

static_assert(kMaxBackends * kLanesPerBackend <= lane_id_value(kBypassLaneId),
              "a backend block must not reach the bypass lane id");
static_assert(kBypassLaneId < kFirstToolLaneId,
              "the bypass id must not fall inside the tool block");
static_assert(kEndLaneId > kFirstToolLaneId,
              "the tool block must not overflow a LaneId, or a block placed above it "
              "would wrap back inside it");

/// True when @p lane is an id this scheme assigns: a slot on one of the
/// backend blocks, the bypass, or a tool. Nothing else is a lane, and a
/// positive integer is not a lane merely for being positive - not the gap
/// between the last backend block and the bypass, not an id one past the
/// bypass, not anything from kEndLaneId up. The funnels drop what this
/// refuses, so a caller that computes an id wrongly is told about it rather
/// than handed a lane of its own.
[[nodiscard]] constexpr bool is_lane_id(LaneId id) {
    const int lane = lane_id_value(id);
    if (lane >= 0 && lane < kMaxBackends * kLanesPerBackend)
        return true;
    if (id == kBypassLaneId)
        return true;
    return id >= kFirstToolLaneId && id < kEndLaneId;
}

/// The lane id for @p slot_index on the backend registered at @p backend_index,
/// or kInvalidLaneId when the pair names no lane. AmsState::add_backend hands
/// a backend its own index, which combines with a slot index here to give the
/// backend its own block of lane ids.
///
/// The blocks are adjacent, so a slot index at or past kLanesPerBackend is
/// not an unused id: it is the neighbouring backend's slot, and a record filed
/// there is a record on the wrong lane, the failure this store exists to
/// remove. Out of range therefore yields no id at all rather than the nearest
/// one, which is a statement a caller can act on and a test can pin.
[[nodiscard]] constexpr LaneId lane_id_for(int backend_index, int slot_index) {
    if (backend_index < 0 || backend_index >= kMaxBackends)
        return kInvalidLaneId;
    if (slot_index < 0 || slot_index >= kLanesPerBackend)
        return kInvalidLaneId;
    return LaneId{backend_index * kLanesPerBackend + slot_index};
}

/// How long an edit of ours stands on a lane without its echo. A write that
/// neither echoes nor fails inside this window is withdrawn, and the lane shows
/// what Moonraker holds.
constexpr std::chrono::seconds kPendingWriteTimeout{30};

/// The one way a reading reaches a lane. Replaces this lane's record for
/// obs.source whole and leaves every other source untouched. A field the
/// source did not observe stops contributing, which is what keeps a stale
/// frame from re-asserting a value its author has stopped standing behind.
/// obs.source must not be Pending; file_pending() is that source's funnel.
///
/// A Firmware or Record reading is also the echo of our own writes: every
/// Pending field it states with the same value is withdrawn, because the
/// reading now carries it. A cleared Pending field (an unlink, an emptied
/// colour name) is echoed only by a Record that leaves the field unset;
/// firmware silent on a field says nothing about it.
///
/// A lane that is not a lane is warned about and dropped.
void ingest(LaneId lane, const Observation& obs);

/// File an edit of ours as Pending while its write is in flight. Amends the
/// lane's Pending record field by field, so a second edit before the first
/// echoes keeps both, and restarts the kPendingWriteTimeout window. A field
/// @p obs clears withdraws its earlier pending value, so the lane shows what
/// Moonraker holds at once (prestonbrown/helixscreen#1661); an unlink and a
/// colour name cleared beside a new colour stay pending until echoed.
/// obs.source must be Pending.
///
/// A lane that is not a lane is warned about and dropped.
void file_pending(LaneId lane, const Observation& obs);

/// Withdraw a lane's Pending record whole: the write failed, or was refused.
/// @p reason is handed to await_pending() waiters.
void withdraw_pending(LaneId lane, const std::string& reason = {});

/// End a lane's Pending record as written: the writer accepted it but its
/// echo will not state every field (a component answering a write directly).
/// Waiters hear Echoed; the lane shows what Moonraker holds from the next read.
void acknowledge_pending(LaneId lane);

/// How a lane's Pending record ended.
enum class PendingOutcome {
    Echoed,   ///< Moonraker holds every pending value (or nothing was pending)
    Failed,   ///< The write failed, was refused, or the record was dropped
    TimedOut, ///< No echo inside kPendingWriteTimeout
};

using PendingSettled = std::function<void(PendingOutcome, const std::string& reason)>;

/// How many edits have been filed on @p lane: taken before a commit, it lets
/// await_pending() report an edit that ended before anyone waited on it.
[[nodiscard]] std::uint64_t pending_filings(LaneId lane);

/// Call @p settled once, when @p lane's Pending record ends. An edit filed
/// after @p since (a pending_filings() count) that has already ended reports
/// how it ended; a lane with nothing filed since answers Echoed at once. It
/// runs on whichever thread ended the record (a database callback can),
/// outside the store's lock, so a UI caller defers it. Several waiters on one
/// lane all run.
void await_pending(LaneId lane, std::uint64_t since, PendingSettled settled);

/// Withdraw the Pending record of each of @p lanes whose window closed before
/// @p now, logging each at warn. Returns every one of @p lanes whose Pending
/// ended that way since the last call that named it, including one
/// lane_sources() expired on read, so the lanes' owner can repaint them.
std::vector<LaneId> expire_pending(std::chrono::steady_clock::time_point now,
                                   const std::vector<LaneId>& lanes);

/// Take off @p pending every field @p reading states with the same value.
/// A cleared Pending field (an unlink, an emptied colour name) is echoed by a
/// Record reading that leaves the field unset, since a record states "none" as
/// null. Firmware silent on a field is no echo: most firmware never holds the
/// field at all. Weights compare within 0.1 g. Pure.
void strip_echoed_fields(Observation& pending, const Observation& reading);

/// Remove one source's record from a lane, leaving every other source alone.
/// A lane that is not a lane is dropped, silently.
void drop_lane_source(LaneId lane, ObservationSource source);

/// Reset a lane to what the machine itself reports: drop the lane record, a
/// pending edit, the Spoolman details and the meter's estimate. Sensed and
/// Firmware survive, because a clear is not a statement about them. The
/// counterpart to a backend erasing its stored record.
///
/// A lane that is not a lane is dropped, silently, like drop_lane_source().
void reset_lane_to_machine_readings(LaneId lane);

/// Retract part of what a lane's Pending and Spoolman records say, leaving the
/// rest of each standing. @p trim is handed each record in turn and resets the
/// fields being retracted; a record left stating nothing is dropped. A
/// Pending record that keeps a field keeps its window and its waiters.
void retract_lane_declarations(LaneId lane, const std::function<void(Observation&)>& trim);

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
/// the map can never hold more than kMaxLanes entries.
///
/// write() is private with exactly two friends, ingest() and file_pending():
/// those are the only code that can put a reading on a lane. Removing a record
/// files nothing, so drop_source() and clear() are public beside them.
class LaneSourceStore {
  public:
    static LaneSourceStore& instance();

    /// A lane's records, with an expired Pending record withdrawn first.
    [[nodiscard]] LaneSources get(LaneId lane);
    [[nodiscard]] std::vector<LaneId> lanes() const;

    void clear();
    void drop_source(LaneId lane, ObservationSource source, const std::string& reason = {},
                     PendingOutcome outcome = PendingOutcome::Failed);
    std::vector<LaneId> expire(std::chrono::steady_clock::time_point now,
                               const std::vector<LaneId>& lanes);
    void trim_pending(LaneId lane, const std::function<void(Observation&)>& trim);
    void await(LaneId lane, std::optional<std::uint64_t> since, PendingSettled settled);
    [[nodiscard]] std::uint64_t filings(LaneId lane) const;

    LaneSourceStore(const LaneSourceStore&) = delete;
    LaneSourceStore& operator=(const LaneSourceStore&) = delete;

  private:
    LaneSourceStore() = default;

    /// A waiter taken off the map under the lock, run after it is released.
    struct Settlement {
        PendingSettled settled;
        PendingOutcome outcome;
        std::string reason;
    };
    using Settlements = std::vector<Settlement>;
    void settle_locked(LaneId lane, PendingOutcome outcome, const std::string& reason,
                       Settlements& out);
    static void run(Settlements& settlements);

    /// @p amend false replaces the source's record whole; true merges the
    /// observed fields onto whatever that source already holds.
    void write(LaneId lane, const Observation& obs, bool amend);

    bool expire_locked(LaneId lane, std::chrono::steady_clock::time_point now, Settlements& out);

    /// True when @p lane differs from the last lane a funnel warned about,
    /// latching it: a producer filing through an unregistered backend reaches
    /// this on every frame, and one line per id says all there is to say.
    bool first_drop_of(LaneId lane);

    friend void ingest(LaneId, const Observation&);
    friend void file_pending(LaneId, const Observation&);

    /// Backends parse on the main thread; Spoolman and database callbacks
    /// can land on an HTTP worker.
    mutable std::mutex mutex_;
    std::map<LaneId, LaneSources> lanes_;
    std::map<LaneId, std::chrono::steady_clock::time_point> pending_deadline_;
    std::map<LaneId, std::vector<PendingSettled>> waiters_;
    /// Per lane: edits filed, and how the last one ended (with the filing
    /// count it ended at), for a waiter that arrives after the end.
    std::map<LaneId, std::uint64_t> filings_;
    struct Ended {
        std::uint64_t filings = 0;
        PendingOutcome outcome = PendingOutcome::Echoed;
        std::string reason;
    };
    std::map<LaneId, Ended> last_ended_;
    std::optional<LaneId> warned_drop_;
    /// Lanes whose Pending timed out and whose owner has not been told.
    std::set<LaneId> expired_untold_;
};

} // namespace helix::ams
