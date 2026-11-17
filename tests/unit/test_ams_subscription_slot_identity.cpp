// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// AmsSubscriptionBackend's clear template: the one announcement, and the lane
// reset that reaches a slot no frame has cached on the backends whose traits
// say so.

#include "../lvgl_test_fixture.h"
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_indx.h"
#include "ams_backend_openams.h"
#include "ams_backend_toolchanger.h"
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

TEST_CASE("The subscription base announces a clear once", "[ams][ams_subscription]") {
    IdentityProbe backend;
    std::vector<std::string> events;
    backend.set_event_callback([&events](const std::string& event, const std::string& data) {
        events.push_back(event + ":" + data);
    });

    backend.clear_slot_override(1);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == std::string(AmsBackend::kEventSlotChanged) + ":1");
}

namespace {

/// Whether a clear on a registered, never-started @p Backend drops the record
/// of lane @p slot, which no frame has cached.
template <class Backend> bool clear_drops_uncached_record(int slot = 1) {
    auto owned = std::make_unique<Backend>(nullptr, nullptr);
    auto* backend = owned.get();
    REQUIRE(helix::AmsState::instance().add_backend(std::move(owned)) >= 0);
    REQUIRE(backend->get_system_info().total_slots <= slot);

    const helix::ams::LaneId lane = backend->lane_id(slot);
    helix::ams::Observation record(helix::ams::ObservationSource::Record);
    record.material = "PETG";
    helix::ams::ingest(lane, record);
    REQUIRE(helix::ams::lane_sources(lane).record.has_value());

    backend->clear_slot_override(slot);
    const bool dropped = !helix::ams::lane_sources(lane).record.has_value();
    helix::AmsState::instance().clear_backends();
    return dropped;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "A clear reaches a slot no frame has cached only where the backend says so",
                 "[ams][ams_subscription]") {
    CHECK(clear_drops_uncached_record<AmsBackendOpenAms>());
    CHECK(clear_drops_uncached_record<AmsBackendHappyHare>());
    CHECK(clear_drops_uncached_record<AmsBackendToolChanger>());
    CHECK(clear_drops_uncached_record<AmsBackendIndx>());
    // AD5X builds its four ports up front; a fifth index is one no frame cached.
    CHECK_FALSE(clear_drops_uncached_record<AmsBackendAd5xIfs>(4));
}
