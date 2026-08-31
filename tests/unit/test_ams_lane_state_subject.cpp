// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_fixtures.h"
#include "ams_backend_mock.h"
#include "ams_lane_state.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(XMLTestFixture, "lane_state subject tracks the backend",
                 "[ams][lane_state][subject]") {
    auto owned = std::make_unique<AmsBackendMock>(4);
    owned->set_operation_delay(0);
    auto* mock = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    // Lane 0: assigned + present. Lane 1: ejected but assigned. Lane 2: bare.
    {
        SlotInfo i0 = mock->get_slot_info(0);
        i0.material = "PLA";
        REQUIRE(mock->set_slot_info(0, i0).success());
        mock->force_slot_status(0, SlotStatus::AVAILABLE);

        SlotInfo i1 = mock->get_slot_info(1);
        i1.material = "PETG";
        REQUIRE(mock->set_slot_info(1, i1).success());
        mock->force_slot_status(1, SlotStatus::EMPTY);

        SlotInfo i2 = mock->get_slot_info(2);
        i2.material.clear();
        i2.brand.clear();
        i2.spool_name.clear();
        i2.spoolman_id = 0;
        REQUIRE(mock->set_slot_info(2, i2).success());
        mock->force_slot_status(2, SlotStatus::EMPTY);
    }

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    process_lvgl(20);

    auto state_of = [](int i) {
        lv_subject_t* s = AmsState::instance().get_slot_lane_state_subject(i);
        REQUIRE(s != nullptr);
        return static_cast<helix::ui::LaneState>(lv_subject_get_int(s));
    };

    CHECK(state_of(0) == helix::ui::LaneState::Present);
    CHECK(state_of(1) == helix::ui::LaneState::Ghosted);
    CHECK(state_of(2) == helix::ui::LaneState::Empty);

    AmsState::instance().clear_backends();
}

TEST_CASE_METHOD(XMLTestFixture, "lane_state subject is XML-registered per lane",
                 "[ams][lane_state][subject]") {
    // deinit FIRST, deliberately. init_subjects() early-returns when initialized_
    // is already set and does NOT register the XML names on that path, so a bare
    // init_subjects(true) is a no-op whenever anything earlier in the process
    // already brought AmsState up. This case therefore used to assert against
    // names some previous test happened to register: green standalone and in a
    // full [ams] run, red for any shard subset without such a test. Forcing a
    // real registration pass makes it independent of execution order.
    AmsState::instance().deinit_subjects();
    AmsState::instance().init_subjects(true);

    // Name shape must match what the chrome binds to: ams_slot_<n>_lane_state.
    CHECK(lv_xml_get_subject(nullptr, "ams_slot_0_lane_state") != nullptr);
    CHECK(lv_xml_get_subject(nullptr, "ams_slot_3_lane_state") != nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "lane_state subject bounds-checks its index",
                 "[ams][lane_state][subject]") {
    AmsState::instance().init_subjects(true);
    CHECK(AmsState::instance().get_slot_lane_state_subject(-1) == nullptr);
    CHECK(AmsState::instance().get_slot_lane_state_subject(AmsState::MAX_SLOTS) == nullptr);
}

TEST_CASE_METHOD(XMLTestFixture, "lane_state follows the single-slot update path",
                 "[ams][lane_state][subject]") {
    // update_slot() is the EVENT_SLOT_UPDATED fast path — update_slot_for_backend(0, n)
    // delegates straight to it, so a backend that emits per-slot events instead of a
    // full sync reaches this code and nothing else. It used to write slot_statuses_
    // without writing slot_lane_states_, leaving every lane rendering surface bound to
    // a stale classification until the next full sync happened to repair it.
    auto owned = std::make_unique<AmsBackendMock>(4);
    owned->set_operation_delay(0);
    auto* mock = owned.get();
    AmsState::instance().set_backend(std::move(owned));

    {
        SlotInfo i0 = mock->get_slot_info(0);
        i0.material = "PLA";
        REQUIRE(mock->set_slot_info(0, i0).success());
        mock->force_slot_status(0, SlotStatus::AVAILABLE);
    }

    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    // Drain the sync set_slot_info() queued, so the assertion window below belongs to
    // update_slot() alone and cannot be repaired by a full resync arriving late.
    process_lvgl(20);

    lv_subject_t* lane = AmsState::instance().get_slot_lane_state_subject(0);
    REQUIRE(lane != nullptr);
    REQUIRE(static_cast<helix::ui::LaneState>(lv_subject_get_int(lane)) ==
            helix::ui::LaneState::Present);

    // Eject the lane but keep its identity. force_slot_status() mutates the mock in
    // place and emits no event, so no sync is queued to do update_slot()'s job for it.
    mock->force_slot_status(0, SlotStatus::EMPTY);
    AmsState::instance().update_slot(0);

    // Assert synchronously — update_slot() writes subjects directly, and running the
    // LVGL loop here would let any queued sync close the window this test opens.
    CHECK(static_cast<helix::ui::LaneState>(lv_subject_get_int(lane)) ==
          helix::ui::LaneState::Ghosted);
    // The status subject moved too — proving the lane genuinely changed and the check
    // above is not passing because nothing happened at all.
    CHECK(lv_subject_get_int(AmsState::instance().get_slot_status_subject(0)) ==
          static_cast<int>(SlotStatus::EMPTY));

    AmsState::instance().clear_backends();
}
