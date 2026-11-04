// tests/test_helpers/seeded_override.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeding a stored override in a fixture, the way a backend's init would.
#pragma once

#include "ams_backend.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "filament_slot_override_store.h"
#include "json_fwd.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"

namespace helix::test {

/// File the lane records a stored override would have produced at load.
///
/// Every backend's on_started() pairs load_blocking() with
/// ingest_legacy_records(), so the application never holds an override whose
/// lane records are missing. A fixture that writes overrides_ alone models a
/// state the application cannot be in, and anything reading through the lane
/// model then sees an empty lane and leaves the backend's own values standing.
///
/// Routes each source through the funnel that source is allowed to use, and
/// classifies with the same pure sources_from_record() the real migration
/// uses, so a fixture cannot disagree with production about what a record
/// means.
///
/// The wire document is built by to_lane_data_record(), the same emitter
/// save_async writes through. Authorship is read off that document and not off
/// the struct: the lock keys and the declared set live there, and a record
/// classified against an empty object declares nothing at all, so a fixture
/// handing over one would test the unlocked path while believing it had seeded
/// a locked record. LegacyLockKeys::LaneData is the only spelling reachable
/// here because the emitter is the lane_data one, which is also the only
/// spelling any backend's ingest_legacy_records() passes.
///
/// A record this seeds still declares only what it carries. The lock flags
/// speak for colour and material; every other identity field answers to the
/// record's own declared set, so a brand on a record whose set is empty is
/// remembered rather than declared. A fixture that means a person typed the
/// brand wants edit_slot_as_user() below.
///
/// @p backend must be registered with AmsState, or lane_id() answers
/// kInvalidLaneId and the funnels drop every record. RegisteredBackend is how
/// a fixture gets that.
inline void file_override_as_lane_records(const AmsBackend& backend, int slot_index,
                                          const helix::ams::FilamentSlotOverride& ovr) {
    const helix::ams::LaneSources sources =
        helix::ams::sources_from_record(ovr, helix::ams::to_lane_data_record(slot_index, ovr),
                                        helix::ams::LegacyLockKeys::LaneData);
    helix::ams::file_lane_sources(backend.lane_id(slot_index), sources);
}

/// Edit a slot the way the application does.
///
/// AmsState::commit_slot_edit hands the backend write and the lane record to
/// AmsBackend::commit_user_edit(), and this calls that same method, so a
/// fixture's edit passes the declaration to apply_user_edit, drops what a
/// binding change leaves stale, files the declaration and repaints exactly as
/// production does. A fixture that calls apply_user_edit alone performs only the
/// backend write, so the lane keeps whatever it held and the edit has nothing
/// standing behind it.
///
/// The editor's before-state is the slot as it stands, which is what the
/// editor opens on.
inline void edit_slot_as_user(AmsBackend& backend, int slot_index, const helix::SlotInfo& info) {
    (void)backend.commit_user_edit(slot_index, backend.get_slot_info(slot_index), info);
}

/// File what Spoolman says a linked spool is, the way the application does.
///
/// Linking is a statement about the binding, so user_edit_observation files the
/// id alone; the brand, colour and material that rode in with it are the
/// server's word, and SpoolmanManager files them on every fetch of the spool.
/// This goes through that same SpoolmanManager::file_spool_on_lane(), for a
/// backend test with no manager and no server behind it, so a fixture files
/// exactly the fields production does. A fixture that links a spool without
/// this has a lane naming an id nothing describes.
inline void spool_states(const AmsBackend& backend, int slot_index, const SpoolInfo& spool) {
    SpoolmanManager::file_spool_on_lane(backend.lane_id(slot_index), spool);
}

/// Stage what SpoolmanManager runs when the server denies a spool a slot is
/// bound to: stop tracking the spool, then re-file what the slot showed as
/// remembered, so the lane goes on showing what is loaded. A backend test has
/// no manager behind it, so this walks the same funnels the manager does,
/// from the same identity snapshot: the slot's struct with both binding ids
/// zeroed, because a kept record that named the spool would file as the
/// spool's whole identity and stand the record just dropped right back up.
///
/// The caller owns the repaint: a caching backend paints from its lane only
/// through repaint_slot_from_lane() or a frame, and which of those follows is
/// the question the test is asking.
inline void spool_denied_on_lane(const AmsBackend& backend, int slot_index) {
    helix::ams::drop_lane_source(backend.lane_id(slot_index),
                                 helix::ams::ObservationSource::Spoolman);
    SlotInfo kept = backend.get_slot_info(slot_index);
    kept.spoolman_id = 0;
    kept.spoolman_filament_id = 0;
    const helix::ams::Observation nothing_declared(helix::ams::ObservationSource::Remembered);
    helix::ams::file_kept_identity(
        backend.lane_id(slot_index), slot_index,
        helix::ams::user_override_from_slot_info(nothing_declared, kept, kept.material, nullptr));
}

} // namespace helix::test
