// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_source_store.h"

#include "lane_translation.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <tuple>
#include <utility>

namespace helix::ams {

namespace {

template <typename IntoTuple, typename FromTuple, std::size_t... I>
void merge_fields_impl(IntoTuple& into, const FromTuple& from, std::index_sequence<I...>) {
    (
        [&] {
            if (std::get<I>(from).has_value())
                std::get<I>(into) = std::get<I>(from);
        }(),
        ...);
}

template <typename Tuple, std::size_t... I>
bool any_observed_impl(const Tuple& fields, std::index_sequence<I...>) {
    return (std::get<I>(fields).has_value() || ...);
}

/// True when @p obs observed at least one field. Folds over
/// Observation::fields() so a field added there is covered with no line here.
bool any_observed(const Observation& obs) {
    auto fields = obs.fields();
    return any_observed_impl(fields,
                             std::make_index_sequence<std::tuple_size_v<decltype(fields)>>{});
}

/// Copy every field @p from has observed onto @p into, leaving the rest
/// alone. Folds over Observation::fields() so a field added there is amended
/// automatically, with no matching line to add here.
void merge_observed_fields(Observation& into, const Observation& from) {
    auto into_fields = into.fields();
    auto from_fields = from.fields();
    merge_fields_impl(into_fields, from_fields,
                      std::make_index_sequence<std::tuple_size_v<decltype(into_fields)>>{});
}

} // namespace

LaneSourceStore& LaneSourceStore::instance() {
    static LaneSourceStore store;
    return store;
}

void LaneSourceStore::write(LaneId lane, const Observation& obs, bool amend) {
    std::lock_guard<std::mutex> lock(mutex_);
    LaneSources& sources = lanes_[lane];
    if (!amend) {
        sources.apply(obs);
        return;
    }
    const auto& existing = record_for(sources, obs.source);
    Observation merged = existing.has_value() ? *existing : Observation(obs.source);
    merge_observed_fields(merged, obs);
    sources.apply(merged);
}

LaneSources LaneSourceStore::get(LaneId lane) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lanes_.find(lane);
    return it == lanes_.end() ? LaneSources{} : it->second;
}

void LaneSourceStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.clear();
    warned_producer_drop_.reset();
    warned_edit_drop_.reset();
}

void LaneSourceStore::drop_source(LaneId lane, ObservationSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lanes_.find(lane);
    if (it == lanes_.end()) {
        return;
    }
    it->second.drop(source);
}

bool LaneSourceStore::first_drop_of(DropSite site, LaneId lane) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& latch = site == DropSite::Producer ? warned_producer_drop_ : warned_edit_drop_;
    if (latch == lane) {
        return false;
    }
    latch = lane;
    return true;
}

std::vector<LaneId> LaneSourceStore::lanes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LaneId> out;
    out.reserve(lanes_.size());
    for (const auto& entry : lanes_) {
        out.push_back(entry.first);
    }
    return out;
}

void ingest(LaneId lane, const Observation& obs) {
    if (!is_lane_id(lane)) {
        if (LaneSourceStore::instance().first_drop_of(LaneSourceStore::DropSite::Producer, lane)) {
            spdlog::warn("[LaneSourceStore] ingest called with lane {}, which names no position; "
                         "dropped. Repeats for this lane are silent.",
                         lane);
        }
        return;
    }
    if (obs.source == ObservationSource::LocalUser) {
        spdlog::warn("[LaneSourceStore] ingest called with a user source; dropped");
        return;
    }
    LaneSourceStore::instance().write(lane, obs, /*amend=*/false);
}

void commit_slot_edit(LaneId lane, const Observation& obs) {
    if (!is_lane_id(lane)) {
        if (LaneSourceStore::instance().first_drop_of(LaneSourceStore::DropSite::UserEdit, lane)) {
            spdlog::warn("[LaneSourceStore] commit_slot_edit called with lane {}, which names no "
                         "position; dropped. Repeats for this lane are silent.",
                         lane);
        }
        return;
    }
    if (obs.source != ObservationSource::LocalUser) {
        spdlog::warn("[LaneSourceStore] commit_slot_edit called with a non-user source; dropped");
        return;
    }
    // An amendment that observed nothing states nothing. Writing it anyway
    // would leave a declaration with no content on the lane, which reads as
    // "a person declared something here" to anything testing for a record.
    if (!any_observed(obs)) {
        return;
    }

    // A field the edit cleared is a withdrawal, so it comes off the statement
    // and off whatever the rung already declared for it: an engaged empty
    // filed here would outrank the machine's own reading until a restart.
    // The clear still reaches the stored record, through the edit's values.
    Observation statement = obs;
    withdraw_cleared_fields(statement, obs);
    Observation rung(ObservationSource::LocalUser);
    const LaneSources standing = LaneSourceStore::instance().get(lane);
    if (standing.local_user.has_value()) {
        rung = *standing.local_user;
        withdraw_cleared_fields(rung, obs);
    }
    merge_observed_fields(rung, statement);
    // The stamp of the newest edit this rung has absorbed: the caller's when
    // it filed from a record, and this moment for an edit made here. The
    // next record another tool writes is measured against it.
    rung.edited_at = statement.edited_at.value_or(std::chrono::system_clock::now());
    LaneSourceStore::instance().drop_source(lane, ObservationSource::LocalUser);
    if (any_observed(rung)) {
        LaneSourceStore::instance().write(lane, rung, /*amend=*/false);
    }
}

void drop_lane_source(LaneId lane, ObservationSource source) {
    if (!is_lane_id(lane)) {
        return;
    }
    LaneSourceStore::instance().drop_source(lane, source);
}

void reset_lane_to_machine_readings(LaneId lane) {
    drop_lane_source(lane, ObservationSource::Spoolman);
    drop_lane_source(lane, ObservationSource::LocalUser);
    drop_lane_source(lane, ObservationSource::Metered);
    drop_lane_source(lane, ObservationSource::Remembered);
}

void retract_lane_declarations(LaneId lane, const std::function<void(Observation&)>& trim) {
    const LaneSources sources = lane_sources(lane);
    if (sources.local_user.has_value()) {
        Observation kept = *sources.local_user;
        trim(kept);
        drop_lane_source(lane, ObservationSource::LocalUser);
        commit_slot_edit(lane, kept);
    }
    if (sources.spoolman.has_value()) {
        Observation kept = *sources.spoolman;
        trim(kept);
        ingest(lane, kept);
    }
}

LaneSources lane_sources(LaneId lane) {
    return LaneSourceStore::instance().get(lane);
}

std::vector<LaneId> known_lanes() {
    return LaneSourceStore::instance().lanes();
}

void reset_lane_sources() {
    LaneSourceStore::instance().clear();
}

} // namespace helix::ams
