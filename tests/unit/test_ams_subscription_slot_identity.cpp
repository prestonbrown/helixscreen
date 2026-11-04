// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// AmsSubscriptionBackend's slot-identity template: the write, the refusal and
// the one announcement, through a backend that takes every default hook.

#include "../lvgl_test_fixture.h"
#include "ams_backend_openams.h"
#include "ams_state.h"
#include "lane_observation.h"
#include "lane_source_store.h"
#include "lane_sources.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class IdentityProbe : public AmsBackendOpenAms {
  public:
    IdentityProbe() : AmsBackendOpenAms(nullptr, nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        AmsUnit unit;
        unit.name = "probe";
        unit.slot_count = 2;
        unit.first_slot_global_index = 0;
        unit.slots.resize(2);
        for (int i = 0; i < 2; ++i) {
            unit.slots[static_cast<size_t>(i)].slot_index = i;
        }
        system_info_.units.push_back(std::move(unit));
        system_info_.total_slots = 2;
    }
};

} // namespace

TEST_CASE("The subscription base writes a synced identity and announces it once",
          "[ams][ams_subscription]") {
    IdentityProbe backend;
    std::vector<std::string> events;
    backend.set_event_callback([&events](const std::string& event, const std::string& data) {
        events.push_back(event + ":" + data);
    });

    SlotInfo info;
    info.material = "PETG";
    info.color_rgb = 0x112233u;
    info.color_name = "Teal";

    REQUIRE(backend.sync_external_identity(1, info).success());
    const SlotInfo after = backend.get_slot_info(1);
    CHECK(after.material == "PETG");
    CHECK(after.color_rgb == 0x112233u);
    CHECK(after.color_name == "Teal");
    REQUIRE(events.size() == 1);
    CHECK(events[0] == std::string(AmsBackend::kEventSlotChanged) + ":1");

    SECTION("an index naming no slot is refused as out of range and announces nothing") {
        const AmsError err = backend.sync_external_identity(2, info);
        CHECK(err.result == AmsResult::INVALID_SLOT);
        CHECK(events.size() == 1);
    }

    SECTION("a clear announces the slot once") {
        backend.clear_slot_override(1);
        REQUIRE(events.size() == 2);
        CHECK(events[1] == std::string(AmsBackend::kEventSlotChanged) + ":1");
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "A backend that clears without a cached slot still resets the lane",
                 "[ams][ams_subscription]") {
    // Registered with AmsState so its slots have lanes, never started, so no
    // frame has cached a slot: the clear must still reach the lane's standing
    // declarations.
    auto owned = std::make_unique<AmsBackendOpenAms>(nullptr, nullptr);
    auto* backend = owned.get();
    REQUIRE(helix::AmsState::instance().add_backend(std::move(owned)) >= 0);
    REQUIRE(backend->get_system_info().total_slots == 0);

    const helix::ams::LaneId lane = backend->lane_id(1);
    helix::ams::Observation edit(helix::ams::ObservationSource::LocalUser);
    edit.material = "PETG";
    helix::ams::commit_slot_edit(lane, edit);
    REQUIRE(helix::ams::lane_sources(lane).local_user.has_value());

    backend->clear_slot_override(1);
    CHECK_FALSE(helix::ams::lane_sources(lane).local_user.has_value());
    helix::AmsState::instance().clear_backends();
}
