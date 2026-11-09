// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The write paths that reach a lane's records without going through a
// backend's parse loop: the consumption meter, and a user clearing a slot.
//
// Both predate the lane source model and both used to write only the stores
// that existed when they were written. A store with several writers is only as
// good as its least-updated one, so what is pinned here is that these two now
// reach it.

#include "../lvgl_test_fixture.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "lane_apply.h"
#include "lane_observation.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include "../catch_amalgamated.hpp"

using helix::AmsBackendAfc;
using helix::AmsBackendMock;
using helix::SlotInfo;
using helix::SlotStatus;
using helix::ams::apply_resolved;
using helix::ams::file_pending;
using helix::ams::ingest;
using helix::ams::lane_sources;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::resolve;
using helix::test::RegisteredBackend;

namespace {

/// A lane carrying a record linking spool 42, a pending edit and the
/// server's details, the records a clear has to take with it.
void declare_identity(helix::ams::LaneId lane) {
    Observation record(ObservationSource::Record);
    record.spoolman_id = 42;
    ingest(lane, record);

    Observation server(ObservationSource::Spoolman);
    server.spoolman_id = 42;
    server.brand = "Polymaker";
    ingest(lane, server);

    Observation user(ObservationSource::Pending);
    user.spool_name = "Bench reel";
    file_pending(lane, user);

    Observation meter(ObservationSource::Metered);
    meter.remaining_weight_g = 480.0F;
    ingest(lane, meter);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Clearing a slot stops its identity painting the lane",
                 "[lane][writepath]") {
    // The clear reaches two stores. Erasing the backend's override while the
    // lane's records stand leaves resolve() still reporting what the user just
    // removed, and the lane repaints it on the next frame.
    RegisteredBackend<AmsBackendAfc> harness(nullptr, nullptr);
    const auto lane = harness.lane(0);

    declare_identity(lane);

    // What the lane shows before the clear, laid over a slot as a backend
    // would have built it.
    SlotInfo before;
    before.brand = "unset";
    before.spool_name = "unset";
    apply_resolved(before, resolve(lane_sources(lane)));
    REQUIRE(before.brand == "Polymaker");
    REQUIRE(before.spool_name == "Bench reel");
    REQUIRE(before.spoolman_id == 42);

    harness->clear_slot_override(0);

    const auto after_sources = lane_sources(lane);
    CHECK_FALSE(after_sources.record.has_value());
    CHECK_FALSE(after_sources.spoolman.has_value());
    CHECK_FALSE(after_sources.pending.has_value());
    CHECK_FALSE(after_sources.metered.has_value());

    // The lane now states none of it, so a backend's own values stand where
    // the cleared identity used to paint.
    SlotInfo after;
    after.brand = "from firmware";
    after.spool_name = "from firmware";
    after.spoolman_id = 0;
    apply_resolved(after, resolve(lane_sources(lane)));
    CHECK(after.brand == "from firmware");
    CHECK(after.spool_name == "from firmware");
    CHECK(after.spoolman_id == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "A clear leaves the machine's own readings alone",
                 "[lane][writepath]") {
    // A clear is a statement about the record somebody made, not about what
    // the hardware reports. Dropping the sensor's answer would empty a loaded
    // lane, and dropping the vendor cache would throw away firmware's own
    // colour and material, which is what should paint once the record is gone.
    RegisteredBackend<AmsBackendAfc> harness(nullptr, nullptr);
    const auto lane = harness.lane(0);

    declare_identity(lane);

    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    ingest(lane, sensed);

    Observation cache(ObservationSource::Firmware);
    cache.material = "PETG";
    cache.color_rgb = 0xED2C2C;
    ingest(lane, cache);

    harness->clear_slot_override(0);

    const auto after = lane_sources(lane);
    REQUIRE(after.sensed.has_value());
    CHECK(after.sensed->present == true);
    REQUIRE(after.firmware.has_value());
    CHECK(after.firmware->material == "PETG");

    const auto r = resolve(after);
    CHECK(r.present == true);
    CHECK(r.material == "PETG");
    CHECK(r.color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(LVGLTestFixture, "A metered weight reaches the lane it was measured on",
                 "[lane][writepath]") {
    // update_slot_weight is the consumption tracker's and Spoolman's way in,
    // and neither is a backend. Without this the number a lane shows and the
    // number the meter holds are two stores that drift apart for a whole print.
    RegisteredBackend<AmsBackendMock> harness(4);

    harness->update_slot_weight(1, 400.0F, 900.0F, /*persist=*/false);

    const auto sources = lane_sources(harness.lane(1));
    REQUIRE(sources.metered.has_value());
    CHECK(sources.metered->remaining_weight_g == Catch::Approx(400.0F));
    CHECK(sources.metered->total_weight_g == Catch::Approx(900.0F));

    // On the lane it was measured on, and no other.
    CHECK_FALSE(lane_sources(harness.lane(0)).metered.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "A weight update with no total leaves the backend's total alone",
                 "[lane][writepath]") {
    // A total below zero is the caller saying it has none to report. Filing it
    // as a reading would state a weight nobody measured; filing nothing leaves
    // the backend's own total standing, which is the "leave unchanged" the
    // parameter asks for.
    RegisteredBackend<AmsBackendMock> harness(4);

    harness->update_slot_weight(2, 250.0F, -1.0F, /*persist=*/false);

    const auto sources = lane_sources(harness.lane(2));
    REQUIRE(sources.metered.has_value());
    CHECK(sources.metered->remaining_weight_g == Catch::Approx(250.0F));
    CHECK_FALSE(sources.metered->total_weight_g.has_value());

    SlotInfo slot;
    slot.total_weight_g = 1000.0F;
    apply_resolved(slot, resolve(sources));
    CHECK(slot.remaining_weight_g == Catch::Approx(250.0F));
    CHECK(slot.total_weight_g == Catch::Approx(1000.0F));
}

TEST_CASE_METHOD(LVGLTestFixture, "A firmware frame cannot erase what a stored record remembered",
                 "[lane][writepath][remembered]") {
    // The defect Remembered exists to close. A stored record and a backend's
    // own parse both used to file VendorCache, ingest() replaces a source's
    // record whole, and a frame states only what that frame knows. So a brand
    // nobody had retyped since the last boot disappeared on the first poll.
    RegisteredBackend<AmsBackendAfc> harness(nullptr, nullptr);
    const auto lane = harness.lane(0);

    helix::ams::FilamentSlotOverride stored;
    stored.brand = "Polymaker";
    stored.material = "ASA";
    helix::test::file_override_as_lane_records(*harness, 0, stored);

    REQUIRE(lane_sources(lane).record.has_value());
    REQUIRE(lane_sources(lane).record->brand == "Polymaker");

    // The machine speaks, and says nothing about a brand.
    Observation frame(ObservationSource::Firmware);
    frame.material = "PLA";
    frame.color_rgb = 0xED2C2Cu;
    ingest(lane, frame);

    const auto after = lane_sources(lane);
    REQUIRE(after.record.has_value());
    CHECK(after.record->brand == "Polymaker");

    const auto r = resolve(after);
    // The frame outranks the disk copy on what it actually stated, and leaves
    // the rest of the disk copy standing.
    CHECK(r.material == "PLA");
    CHECK(r.color_rgb == 0xED2C2Cu);
    CHECK(r.brand == "Polymaker");
}

TEST_CASE_METHOD(LVGLTestFixture, "A remembered field never outranks the machine stating it",
                 "[lane][writepath][remembered]") {
    // Remembered is the weakest rung. A printer that does state the field wins,
    // or last boot's copy would pin a value the machine has moved on from.
    RegisteredBackend<AmsBackendAfc> harness(nullptr, nullptr);
    const auto lane = harness.lane(0);

    helix::ams::FilamentSlotOverride stored;
    stored.brand = "Polymaker";
    helix::test::file_override_as_lane_records(*harness, 0, stored);

    Observation frame(ObservationSource::Firmware);
    frame.brand = "Elegoo";
    ingest(lane, frame);

    CHECK(resolve(lane_sources(lane)).brand == "Elegoo");
}

// A spool name describes the spool the rest of the identity named. An edit that
// re-declares that identity without restating the name must not carry the old
// name onto the new spool (the "SnapSpeed" a Generic ASA lane inherited).
TEST_CASE("An edit that changes the spool's identity drops a name it did not restate",
          "[lane][spool_name]") {
    SlotInfo original;
    original.brand = "Snapmaker";
    original.material = "PLA";
    original.spool_name = "SnapSpeed";

    SECTION("a new brand") {
        SlotInfo edited = original;
        edited.brand = "Generic";
        edited.material = "ASA";
        CHECK(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name.empty());
    }
    SECTION("a new material under the same brand") {
        SlotInfo edited = original;
        edited.material = "PETG";
        CHECK(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name.empty());
    }
    SECTION("a different catalog product") {
        original.catalog_id = "sunlu-pla-marble";
        SlotInfo edited = original;
        edited.catalog_id = "sunlu-pla-plus-2-0";
        CHECK(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name.empty());
    }
}

TEST_CASE("An edit that keeps the spool's identity keeps its name", "[lane][spool_name]") {
    SlotInfo original;
    original.brand = "Sunlu";
    original.material = "PLA";
    original.spool_name = "Bench reel";

    SECTION("a colour edit with the brand and material re-cased") {
        SlotInfo edited = original;
        edited.brand = "SUNLU";
        edited.material = "pla";
        edited.color_rgb = 0x00FF00;
        CHECK_FALSE(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name == "Bench reel");
    }
    SECTION("a first catalog pick, which the editor makes on any Save") {
        SlotInfo edited = original;
        edited.catalog_id = "sunlu-pla-marble";
        CHECK_FALSE(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name == "Bench reel");
    }
    SECTION("a name the edit itself states") {
        SlotInfo edited = original;
        edited.brand = "Polymaker";
        edited.spool_name = "PolyLite";
        CHECK_FALSE(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name == "PolyLite");
    }
    SECTION("a linked spool, whose name is the spool's own") {
        original.spoolman_id = 7;
        SlotInfo edited = original;
        edited.brand = "Polymaker";
        CHECK_FALSE(helix::ams::drop_stale_spool_name(original, edited));
        CHECK(edited.spool_name == "Bench reel");
    }
}
