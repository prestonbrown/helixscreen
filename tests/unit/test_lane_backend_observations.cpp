// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What each backend's own signal becomes in the lane source model. Cases here
// assert on the POPULATED SOURCES only - never on SlotInfo, the override store
// or anything the UI shows.

#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_state.h"
#include "filament_slot_override.h"
#include "lane_source_store.h"
#include "test_helpers/ad5x_ifs_test_access.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using helix::Ad5xIfsTestAccess;
using helix::AmsBackendAd5xIfs;
using helix::AmsState;
using helix::ams::lane_sources;

namespace {

/// A backend with no Moonraker behind it, registered with AmsState so that
/// registration stamps its backend index.
///
/// Registration is not scenery. AmsBackend::lane_id() answers INVALID_LANE_ID
/// until AmsState::add_backend() stamps an index, and the funnels drop what
/// that id names, so an unregistered backend files nothing and every case
/// below would assert against an empty store. set_backend() clears first, so
/// this backend is always block 0 whatever a previous test left behind.
class RegisteredAd5xIfs {
  public:
    RegisteredAd5xIfs() {
        auto owned = std::make_unique<AmsBackendAd5xIfs>(nullptr, nullptr);
        backend_ = owned.get();
        AmsState::instance().set_backend(std::move(owned));
    }

    ~RegisteredAd5xIfs() {
        AmsState::instance().clear_backends();
    }

    RegisteredAd5xIfs(const RegisteredAd5xIfs&) = delete;
    RegisteredAd5xIfs& operator=(const RegisteredAd5xIfs&) = delete;

    AmsBackendAd5xIfs& operator*() const {
        return *backend_;
    }

    /// The lane this backend's slot occupies, asked of the backend itself.
    /// Deriving it any other way would let a case pass against a lane the
    /// backend never writes.
    [[nodiscard]] helix::ams::LaneId lane(int slot) const {
        return backend_->lane_id(slot);
    }

  private:
    AmsBackendAd5xIfs* backend_;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "a registered backend's slots are its own block of lanes",
                 "[lane][ingest][ad5x]") {
    RegisteredAd5xIfs harness;

    CHECK(harness.lane(0) == helix::ams::lane_id_for(0, 0));
    CHECK(harness.lane(3) == helix::ams::lane_id_for(0, 3));
    CHECK(helix::ams::is_lane_id(harness.lane(0)));
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X ingests silk-sensor presence as sensed",
                 "[lane][ingest][ad5x]") {
    RegisteredAd5xIfs harness;
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 0, "ED2C2C");
    Ad5xIfsTestAccess::set_material(*harness, 0, "PETG");
    Ad5xIfsTestAccess::set_port_presence(*harness, 0, false);

    const auto lane = lane_sources(harness.lane(0));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == false);
    // The vendor file still remembers the last spool. That is a cache of a past
    // declaration, so it must not carry presence or identity into the sensor's
    // record.
    CHECK_FALSE(lane.sensed->color_rgb.has_value());
    CHECK_FALSE(lane.sensed->material.has_value());

    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0xED2C2Cu);
    REQUIRE(lane.vendor_cache->material.has_value());
    CHECK(*lane.vendor_cache->material == "PETG");
    CHECK_FALSE(lane.vendor_cache->present.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X files a sensed record with no reading before Ports is seen",
                 "[lane][ingest][ad5x]") {
    RegisteredAd5xIfs harness;

    // Native ZMOD publishes no per-port sensors, so port_presence_ is false for
    // every lane whether or not filament is there. An unread sensor is not a
    // reading of "empty".
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto unread = lane_sources(harness.lane(1));
    // The record must exist: its absence would mean the translation never ran,
    // which is a different fact from "the sensor has said nothing yet" and must
    // not pass for it.
    REQUIRE(unread.sensed.has_value());
    CHECK_FALSE(unread.sensed->present.has_value());

    // The same lane, once the silk mask has been read. This half is what makes
    // the half above a guard rather than a backend that never reports presence.
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 1, false);

    const auto read = lane_sources(harness.lane(1));
    REQUIRE(read.sensed.has_value());
    REQUIRE(read.sensed->present.has_value());
    CHECK(*read.sensed->present == false);
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X does not cache a colour it has not read",
                 "[lane][ingest][ad5x]") {
    RegisteredAd5xIfs harness;
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_color(*harness, 2, "");
    Ad5xIfsTestAccess::set_material(*harness, 2, "");
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    const auto lane = lane_sources(harness.lane(2));

    REQUIRE(lane.sensed.has_value());
    REQUIRE(lane.sensed->present.has_value());
    CHECK(*lane.sensed->present == true);

    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.vendor_cache->material.has_value());
}

TEST_CASE_METHOD(LVGLTestFixture, "AD5X caches pure black as a colour", "[lane][ingest][ad5x]") {
    RegisteredAd5xIfs harness;
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 3, true);
    Ad5xIfsTestAccess::set_color(*harness, 3, "000000");

    const auto lane = lane_sources(harness.lane(3));

    REQUIRE(lane.vendor_cache.has_value());
    REQUIRE(lane.vendor_cache->color_rgb.has_value());
    CHECK(*lane.vendor_cache->color_rgb == 0x000000u);
}

TEST_CASE_METHOD(LVGLTestFixture, "an override never reaches AD5X's vendor-cache record",
                 "[lane][ingest][ad5x]") {
    RegisteredAd5xIfs harness;
    Ad5xIfsTestAccess::set_ifs_status_ports_seen(*harness, true);

    // A user colour on a lane the vendor file says nothing about. SlotInfo is
    // persistent across frames and apply_overrides rewrites it in place, so from
    // the second frame on entry->info.color_rgb is this value. A translation
    // reading it back would file the user's own choice as something the vendor
    // store remembers.
    helix::ams::FilamentSlotOverride user;
    user.color_rgb = 0x00FF00u;
    user.color_set = true;
    user.user_locked_color = true;
    Ad5xIfsTestAccess::seed_override(*harness, 2, user);

    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);
    Ad5xIfsTestAccess::set_port_presence(*harness, 2, true);

    const auto lane = lane_sources(harness.lane(2));
    REQUIRE(lane.vendor_cache.has_value());
    CHECK_FALSE(lane.vendor_cache->color_rgb.has_value());
    CHECK_FALSE(lane.local_user.has_value());
}
