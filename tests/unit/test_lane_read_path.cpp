// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AmsBackend::apply_resolved_lane() — the method a backend calls when it has
// finished building a SlotInfo and hands it to the lane source model.
//
// The free function helix::ams::apply_resolved() is pinned by
// test_lane_apply.cpp. What is pinned HERE is the method: that it derives the
// lane from the backend's own stamped index rather than from a bare slot
// index, and that it declines to touch a lane no source has written.

#include "../helix_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lane_observation.h"
#include "lane_source_store.h"
#include "test_helpers/registered_backend.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendMock;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::ams::ingest;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::test::RegisteredBackend;

namespace {

/// A backend that lets a case call the protected method under test.
///
/// apply_resolved_lane() is protected because a backend calls it on itself,
/// holding its own mutex_. A derived probe is how a test reaches it without
/// widening the production surface, and inheriting a real backend keeps the
/// registration, the stamped index and lane_id() all production code.
class ProbeBackend : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;

    void resolve_onto(SlotInfo& slot, int slot_index) {
        apply_resolved_lane(slot, slot_index);
    }
};

/// A SlotInfo carrying a value in every field the resolver owns, as a backend
/// that had just parsed a frame would leave it.
SlotInfo firmware_built_slot() {
    SlotInfo slot;
    slot.slot_index = 0;
    slot.global_index = 0;
    slot.status = SlotStatus::AVAILABLE;
    slot.color_rgb = 0xED2C2C;
    slot.color_name = "Fire Engine";
    slot.material = "PETG";
    slot.brand = "Kingroon";
    slot.spool_name = "Workshop reel";
    slot.catalog_id = "kingroon-petg-red";
    slot.product_name = "HyperPETG";
    slot.spoolman_id = 7;
    slot.spoolman_vendor_id = 3;
    slot.remaining_weight_g = 612.0F;
    slot.total_weight_g = 1000.0F;
    return slot;
}

void check_fields_match(const SlotInfo& got, const SlotInfo& want) {
    CHECK(got.status == want.status);
    CHECK(got.color_rgb == want.color_rgb);
    CHECK(got.color_name == want.color_name);
    CHECK(got.material == want.material);
    CHECK(got.brand == want.brand);
    CHECK(got.spool_name == want.spool_name);
    CHECK(got.catalog_id == want.catalog_id);
    CHECK(got.product_name == want.product_name);
    CHECK(got.spoolman_id == want.spoolman_id);
    CHECK(got.spoolman_vendor_id == want.spoolman_vendor_id);
    CHECK(got.remaining_weight_g == want.remaining_weight_g);
    CHECK(got.total_weight_g == want.total_weight_g);
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "A lane no source has written is left as the backend built it",
                 "[lane][readpath]") {
    // resolve() reports nothing observed for an unwritten lane, so every field
    // is left as the backend built it. Reachable on shipping hardware: a tool
    // changer files no identity at all, and ACE and AD5X file two fields out
    // of eleven.
    RegisteredBackend<ProbeBackend> harness(4);
    REQUIRE(harness->backend_index() == 0);
    REQUIRE_FALSE(helix::ams::lane_sources(harness.lane(1)).sensed.has_value());

    const SlotInfo built = firmware_built_slot();
    SlotInfo slot = built;
    harness->resolve_onto(slot, 1);

    check_fields_match(slot, built);
}

TEST_CASE_METHOD(HelixTestFixture, "A lane with one record resolves onto the backend's slot",
                 "[lane][readpath]") {
    // The counterpart of the case above: on a field a source HAS spoken to,
    // the model is the authority and the backend's value gives way. Both
    // halves are needed, since a method that never wrote anything would pass
    // the leave-it-alone case on its own.
    RegisteredBackend<ProbeBackend> harness(4);

    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    ingest(harness.lane(1), sensed);

    Observation cache(ObservationSource::Firmware);
    cache.material = "ABS";
    cache.color_rgb = 0x1B6AC9;
    ingest(harness.lane(1), cache);

    SlotInfo slot = firmware_built_slot();
    harness->resolve_onto(slot, 1);

    CHECK(slot.material == "ABS");
    CHECK(slot.color_rgb == 0x1B6AC9);
    CHECK(slot.status == SlotStatus::AVAILABLE);
    // No source spoke to these, so the backend's own values stand. The method
    // merges per field; it does not replace the slot.
    CHECK(slot.brand == "Kingroon");
    CHECK(slot.spoolman_id == 7);
}

TEST_CASE_METHOD(HelixTestFixture, "A sensed absence narrows the status the backend stamped",
                 "[lane][readpath]") {
    // Presence is the sensor's answer, and the method carries it through
    // narrow_status. A lane the sensor calls empty reads EMPTY whatever the
    // backend wrote, and identity survives so the lane ghosts rather than
    // blanking.
    RegisteredBackend<ProbeBackend> harness(4);

    Observation sensed(ObservationSource::Sensed);
    sensed.present = false;
    ingest(harness.lane(0), sensed);

    Observation cache(ObservationSource::Firmware);
    cache.material = "PETG";
    ingest(harness.lane(0), cache);

    SlotInfo slot = firmware_built_slot();
    slot.status = SlotStatus::LOADED;
    harness->resolve_onto(slot, 0);

    CHECK(slot.status == SlotStatus::EMPTY);
    CHECK(slot.material == "PETG");
}

TEST_CASE_METHOD(HelixTestFixture, "apply_resolved_lane reads the backend's own block, not slot 0",
                 "[lane][readpath]") {
    // The method derives its lane from the backend's stamped index. A second
    // backend numbers its own slots from zero too, so an implementation that
    // resolved a bare slot index would hand this backend the first one's
    // records.
    auto second = std::make_unique<ProbeBackend>(4);
    auto* raw = second.get();

    RegisteredBackend<ProbeBackend> first(4);
    REQUIRE(helix::AmsState::instance().add_backend(std::move(second)) == 1);
    REQUIRE(raw->backend_index() == 1);
    REQUIRE(first.lane(0) != raw->lane_id(0));

    Observation a(ObservationSource::Firmware);
    a.material = "PLA";
    ingest(first.lane(0), a);

    Observation b(ObservationSource::Firmware);
    b.material = "ASA";
    ingest(raw->lane_id(0), b);

    SlotInfo on_first = firmware_built_slot();
    first->resolve_onto(on_first, 0);
    CHECK(on_first.material == "PLA");

    SlotInfo on_second = firmware_built_slot();
    raw->resolve_onto(on_second, 0);
    CHECK(on_second.material == "ASA");
}

TEST_CASE_METHOD(HelixTestFixture, "An unregistered backend resolves nothing onto its slot",
                 "[lane][readpath]") {
    // backend_index_ rests at -1 until AmsState stamps it, which makes
    // lane_id() kInvalidLaneId. Nothing is filed there and nothing resolves
    // from there, so a backend reading before registration leaves its own
    // values standing rather than painting the resolver's defaults across
    // every slot it owns.
    ProbeBackend unregistered(4);
    REQUIRE(unregistered.backend_index() == -1);
    REQUIRE(unregistered.lane_id(0) == helix::ams::kInvalidLaneId);

    const SlotInfo built = firmware_built_slot();
    SlotInfo slot = built;
    unregistered.resolve_onto(slot, 0);

    check_fields_match(slot, built);
}
