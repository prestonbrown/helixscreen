// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_source_store.h"

#include "ams_state.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <tuple>
#include <utility>

namespace helix::ams {

static_assert(LANES_PER_BACKEND == AmsState::MAX_SLOTS,
              "a lane must exist for every slot a backend can have subjects for");

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
    LaneSourceStore::instance().write(lane, obs, /*amend=*/false);
}

void commit_slot_edit(LaneId lane, const Observation& obs) {
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
    LaneSourceStore::instance().write(lane, obs, /*amend=*/true);
}

LaneSources lane_sources(LaneId lane) {
    return LaneSourceStore::instance().get(lane);
}

std::vector<LaneId> known_lanes() {
    return LaneSourceStore::instance().lanes();
}

} // namespace helix::ams
