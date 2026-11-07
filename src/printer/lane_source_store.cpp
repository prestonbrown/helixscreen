// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_source_store.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace helix::ams {

namespace {

/// The tolerance a weight echo is compared within: the spool editor's own
/// resolution, below which a difference is float noise.
constexpr float kWeightEchoEpsilonG = 0.1f;

template <typename Fn, typename IntoTuple, typename FromTuple, std::size_t... I>
void zip_fields_impl(Fn&& fn, IntoTuple& into, const FromTuple& from, std::index_sequence<I...>) {
    (fn(std::get<I>(into), std::get<I>(from)), ...);
}

/// Hand @p fn each field of @p into beside the same field of @p from. Folds
/// over Observation::fields(), so a field added there is covered with no line
/// here.
template <typename Fn> void zip_fields(Observation& into, const Observation& from, Fn&& fn) {
    auto into_fields = into.fields();
    auto from_fields = from.fields();
    zip_fields_impl(std::forward<Fn>(fn), into_fields, from_fields,
                    std::make_index_sequence<std::tuple_size_v<decltype(into_fields)>>{});
}

template <typename Tuple, std::size_t... I>
bool any_observed_impl(const Tuple& fields, std::index_sequence<I...>) {
    return (std::get<I>(fields).has_value() || ...);
}

bool any_observed(const Observation& obs) {
    auto fields = obs.fields();
    return any_observed_impl(fields,
                             std::make_index_sequence<std::tuple_size_v<decltype(fields)>>{});
}

void merge_observed_fields(Observation& into, const Observation& from) {
    zip_fields(into, from, [](auto& to, const auto& value) {
        if (value.has_value())
            to = value;
    });
}

template <typename T> bool is_cleared(const T& value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return value.empty();
    } else if constexpr (std::is_same_v<T, int>) {
        return value == 0;
    } else {
        return false;
    }
}

template <typename T> bool same_value(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, float>) {
        return std::fabs(a - b) <= kWeightEchoEpsilonG;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
        return (a & 0xFFFFFFu) == (b & 0xFFFFFFu);
    } else {
        return a == b;
    }
}

/// A cleared field in an edit hands the field back to what Moonraker holds at
/// once (prestonbrown/helixscreen#1661), so it withdraws any earlier pending
/// value instead of standing as an empty one. Two clears stay: an unlink, which
/// must displace the link Moonraker still holds until the write lands, and a
/// colour's name cleared beside a new colour, which must displace one naming
/// the old colour.
void withdraw_cleared_fields(Observation& pending, const Observation& edit) {
    zip_fields(pending, edit, [](auto& mine, const auto& theirs) {
        if (theirs.has_value() && is_cleared(*theirs)) {
            mine.reset();
        }
    });
    if (edit.spoolman_id.has_value() && *edit.spoolman_id == 0) {
        pending.spoolman_id = 0;
    }
    if (edit.color_name.has_value() && edit.color_name->empty() && edit.color_rgb.has_value()) {
        pending.color_name = std::string();
    }
}

} // namespace

void strip_echoed_fields(Observation& pending, const Observation& reading) {
    const bool record = reading.source == ObservationSource::Record;
    zip_fields(pending, reading, [record](auto& mine, const auto& theirs) {
        if (!mine.has_value()) {
            return;
        }
        if (theirs.has_value() ? same_value(*mine, *theirs) : record && is_cleared(*mine)) {
            mine.reset();
        }
    });
}

LaneSourceStore& LaneSourceStore::instance() {
    static LaneSourceStore store;
    return store;
}

void LaneSourceStore::write(LaneId lane, const Observation& obs, bool amend) {
    std::lock_guard<std::mutex> lock(mutex_);
    LaneSources& sources = lanes_[lane];
    if (amend) {
        const auto& existing = record_for(sources, obs.source);
        Observation merged = existing.has_value() ? *existing : Observation(obs.source);
        merge_observed_fields(merged, obs);
        if (obs.source == ObservationSource::Pending) {
            withdraw_cleared_fields(merged, obs);
        }
        if (!any_observed(merged)) {
            sources.drop(obs.source);
            pending_deadline_.erase(lane);
            return;
        }
        sources.apply(merged);
    } else {
        sources.apply(obs);
    }
    if (obs.source == ObservationSource::Pending) {
        pending_deadline_[lane] = std::chrono::steady_clock::now() + kPendingWriteTimeout;
        return;
    }
    if ((obs.source == ObservationSource::Firmware || obs.source == ObservationSource::Record) &&
        sources.pending.has_value()) {
        strip_echoed_fields(*sources.pending, obs);
        if (!any_observed(*sources.pending)) {
            sources.pending.reset();
            pending_deadline_.erase(lane);
        }
    }
}

bool LaneSourceStore::expire_locked(LaneId lane, std::chrono::steady_clock::time_point now) {
    auto deadline = pending_deadline_.find(lane);
    if (deadline == pending_deadline_.end() || now < deadline->second) {
        return false;
    }
    pending_deadline_.erase(deadline);
    auto it = lanes_.find(lane);
    if (it == lanes_.end() || !it->second.pending.has_value()) {
        return false;
    }
    it->second.pending.reset();
    spdlog::warn("[LaneSourceStore] lane {}: edit not echoed within {}s; showing what Moonraker "
                 "holds",
                 lane, kPendingWriteTimeout.count());
    return true;
}

LaneSources LaneSourceStore::get(LaneId lane) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire_locked(lane, std::chrono::steady_clock::now());
    auto it = lanes_.find(lane);
    return it == lanes_.end() ? LaneSources{} : it->second;
}

std::vector<LaneId> LaneSourceStore::expire(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LaneId> lanes;
    for (const auto& entry : pending_deadline_) {
        lanes.push_back(entry.first);
    }
    std::vector<LaneId> expired;
    for (LaneId lane : lanes) {
        if (expire_locked(lane, now)) {
            expired.push_back(lane);
        }
    }
    return expired;
}

void LaneSourceStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lanes_.clear();
    pending_deadline_.clear();
    warned_drop_.reset();
}

void LaneSourceStore::drop_source(LaneId lane, ObservationSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == ObservationSource::Pending) {
        pending_deadline_.erase(lane);
    }
    auto it = lanes_.find(lane);
    if (it != lanes_.end()) {
        it->second.drop(source);
    }
}

bool LaneSourceStore::first_drop_of(LaneId lane) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (warned_drop_ == lane) {
        return false;
    }
    warned_drop_ = lane;
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
        if (LaneSourceStore::instance().first_drop_of(lane)) {
            spdlog::warn("[LaneSourceStore] lane {} names no position; reading dropped. Repeats "
                         "for this lane are silent.",
                         lane);
        }
        return;
    }
    if (obs.source == ObservationSource::Pending) {
        spdlog::warn("[LaneSourceStore] ingest called with a pending edit; dropped");
        return;
    }
    LaneSourceStore::instance().write(lane, obs, /*amend=*/false);
}

void file_pending(LaneId lane, const Observation& obs) {
    if (!is_lane_id(lane)) {
        if (LaneSourceStore::instance().first_drop_of(lane)) {
            spdlog::warn("[LaneSourceStore] lane {} names no position; edit dropped. Repeats "
                         "for this lane are silent.",
                         lane);
        }
        return;
    }
    if (obs.source != ObservationSource::Pending) {
        spdlog::warn("[LaneSourceStore] file_pending called with a non-pending source; dropped");
        return;
    }
    if (!any_observed(obs)) {
        return;
    }
    LaneSourceStore::instance().write(lane, obs, /*amend=*/true);
}

void withdraw_pending(LaneId lane) {
    drop_lane_source(lane, ObservationSource::Pending);
}

std::vector<LaneId> expire_pending(std::chrono::steady_clock::time_point now) {
    return LaneSourceStore::instance().expire(now);
}

void drop_lane_source(LaneId lane, ObservationSource source) {
    if (!is_lane_id(lane)) {
        return;
    }
    LaneSourceStore::instance().drop_source(lane, source);
}

void reset_lane_to_machine_readings(LaneId lane) {
    for (ObservationSource source : {ObservationSource::Record, ObservationSource::Pending,
                                     ObservationSource::Spoolman, ObservationSource::Metered}) {
        drop_lane_source(lane, source);
    }
}

void retract_lane_declarations(LaneId lane, const std::function<void(Observation&)>& trim) {
    const LaneSources sources = lane_sources(lane);
    if (sources.pending.has_value()) {
        Observation kept = *sources.pending;
        trim(kept);
        withdraw_pending(lane);
        file_pending(lane, kept);
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
