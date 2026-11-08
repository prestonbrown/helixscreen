// tests/test_helpers/seeded_override.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeding a stored lane record in a fixture, the way a backend's read would.
#pragma once

#include "ams_backend.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"

namespace helix::test {

/// File a stored record on its lane as the Record source, the way
/// refresh_lane_records() files every record it reads.
///
/// @p backend must be registered with AmsState, or lane_id() answers
/// kInvalidLaneId and the funnel drops the record. RegisteredBackend is how a
/// fixture gets that.
inline void file_override_as_lane_records(const AmsBackend& backend, int slot_index,
                                          const helix::ams::FilamentSlotOverride& ovr) {
    helix::ams::ingest(backend.lane_id(slot_index), helix::ams::record_observation(ovr));
}

/// Edit a slot the way the application does: AmsBackend::commit_user_edit(),
/// which files the edit as Pending and has the backend write it. The editor's
/// before-state is the slot as it stands.
inline void edit_slot_as_user(AmsBackend& backend, int slot_index, const helix::SlotInfo& info) {
    (void)backend.commit_user_edit(slot_index, backend.get_slot_info(slot_index), info);
}

/// File what Spoolman says a linked spool is, through the same
/// SpoolmanManager::file_spool_on_lane() the application uses. Spoolman's
/// details count only for the spool the lane links, so a lane that links
/// nothing yet gets the link on its record first, as a stored link would.
inline void spool_states(const AmsBackend& backend, int slot_index, const SpoolInfo& spool) {
    const helix::ams::LaneId lane = backend.lane_id(slot_index);
    const helix::ams::LaneSources sources = helix::ams::lane_sources(lane);
    if (helix::ams::linked_spool_id(sources) == 0) {
        helix::ams::Observation record =
            sources.record.value_or(helix::ams::Observation(helix::ams::ObservationSource::Record));
        record.spoolman_id = spool.id;
        helix::ams::ingest(lane, record);
    }
    SpoolmanManager::file_spool_on_lane(lane, spool);
}

/// What SpoolmanManager runs when the server denies a spool a slot is bound
/// to: the spool's details stop filling the lane. The caller owns the repaint.
inline void spool_denied_on_lane(const AmsBackend& backend, int slot_index) {
    helix::ams::drop_lane_source(backend.lane_id(slot_index),
                                 helix::ams::ObservationSource::Spoolman);
}

} // namespace helix::test
