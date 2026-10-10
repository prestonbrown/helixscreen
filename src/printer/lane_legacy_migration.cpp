// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_legacy_migration.h"

#include "filament_slot_override_store.h"
#include "lane_source_store.h"

#include <spdlog/spdlog.h>

#include <optional>

namespace helix::ams {

bool file_lane_sources(LaneId lane, const LaneSources& sources) {
    bool wrote = false;
    for (const std::optional<Observation>* held :
         {&sources.sensed, &sources.spoolman, &sources.vendor_cache, &sources.metered,
          &sources.remembered}) {
        if (held->has_value()) {
            ingest(lane, **held);
            wrote = true;
        }
    }
    if (sources.local_user.has_value()) {
        // ingest() refuses a LocalUser observation; commit_slot_edit() is that
        // source's only funnel.
        commit_slot_edit(lane, *sources.local_user);
        wrote = true;
    }
    return wrote;
}

void file_kept_identity(LaneId lane, int slot_index, const FilamentSlotOverride& kept) {
    LaneSources reloads =
        sources_from_record(kept, to_lane_data_record(slot_index, kept), LegacyLockKeys::LaneData);
    reloads.metered.reset();
    file_lane_sources(lane, reloads);
}

bool file_outside_edit_if_newer(LaneId lane, const LaneDataRecord& entry, Observation obs,
                                bool write_in_flight) {
    if (write_in_flight || wire_authored_by_helix(entry.wire) ||
        wire_authored_by_firmware(entry.wire) ||
        !outside_edit_wins(entry.record, lane_sources(lane).local_user)) {
        return false;
    }
    obs.source = ObservationSource::LocalUser;
    obs.remaining_weight_g.reset();
    obs.total_weight_g.reset();
    if (entry.record.updated_at.time_since_epoch().count() > 0) {
        obs.edited_at = entry.record.updated_at;
    }
    commit_slot_edit(lane, obs);
    return true;
}

int ingest_legacy_records(const FilamentSlotOverrideStore& store, LegacyLockKeys keys,
                          int backend_index) {
    int populated = 0;
    for (const auto& [slot_index, entry] : store.last_lane_data_records()) {
        const LaneSources sources = sources_from_record(entry.record, entry.wire, keys);
        const LaneId lane = lane_id_for(backend_index, slot_index);
        if (file_lane_sources(lane, sources)) {
            ++populated;
        }
    }
    if (populated > 0) {
        spdlog::info("[LaneMigration] Classified {} stored record(s) for backend {} into lane "
                     "sources",
                     populated, backend_index);
    }
    return populated;
}

} // namespace helix::ams
