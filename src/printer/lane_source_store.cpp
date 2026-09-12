// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_source_store.h"

#include <spdlog/spdlog.h>

namespace helix::ams {

namespace {

/// Copy every field @p from observed onto @p into, leaving the rest alone.
/// Observation and ResolvedLane hold the same fields in different types
/// (optional vs plain), so this list and the resolver's are separate walks of
/// one field set rather than one shared loop.
void merge_observed_fields(Observation& into, const Observation& from) {
    if (from.present.has_value())
        into.present = from.present;
    if (from.color_rgb.has_value())
        into.color_rgb = from.color_rgb;
    if (from.color_name.has_value())
        into.color_name = from.color_name;
    if (from.material.has_value())
        into.material = from.material;
    if (from.brand.has_value())
        into.brand = from.brand;
    if (from.spool_name.has_value())
        into.spool_name = from.spool_name;
    if (from.catalog_id.has_value())
        into.catalog_id = from.catalog_id;
    if (from.product_name.has_value())
        into.product_name = from.product_name;
    if (from.spoolman_id.has_value())
        into.spoolman_id = from.spoolman_id;
    if (from.spoolman_vendor_id.has_value())
        into.spoolman_vendor_id = from.spoolman_vendor_id;
    if (from.remaining_weight_g.has_value())
        into.remaining_weight_g = from.remaining_weight_g;
    if (from.total_weight_g.has_value())
        into.total_weight_g = from.total_weight_g;
    if (from.echo_token.has_value())
        into.echo_token = from.echo_token;
}

/// The record slot @p s occupies on @p lane. No default: a source added to
/// ObservationSource later must fail this switch to compile, not fall through
/// to the wrong record.
const std::optional<Observation>& record_for(const LaneSources& lane, ObservationSource s) {
    switch (s) {
    case ObservationSource::Sensed:
        return lane.sensed;
    case ObservationSource::Spoolman:
        return lane.spoolman;
    case ObservationSource::LocalUser:
        return lane.local_user;
    case ObservationSource::VendorCache:
        return lane.vendor_cache;
    case ObservationSource::Metered:
        return lane.metered;
    }
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
    for (const auto& [id, unused] : lanes_) {
        (void)unused;
        out.push_back(id);
    }
    return out;
}

void ingest(LaneId lane, const Observation& obs) {
    LaneSourceStore::instance().write(lane, obs, /*amend=*/false);
}

LaneSources lane_sources(LaneId lane) {
    return LaneSourceStore::instance().get(lane);
}

std::vector<LaneId> known_lanes() {
    return LaneSourceStore::instance().lanes();
}

} // namespace helix::ams
