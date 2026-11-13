// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_dry_gates.cpp
 * @brief Drying a chosen set of gates: the generic request, the per-lane mock rig,
 *        AmsState's per-slot drying subject and the slot's drying badge
 *        (prestonbrown/helixscreen#1026)
 */

#include "ui_ams_slot.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_environment_zone.h"
#include "ams_state.h"
#include "ams_types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsBackendMock;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsResult;
using helix::AmsState;
using helix::DryingRequest;
using helix::printer::ZoneDryingState;

namespace {

/// Records which plain start the base request forwards to.
class ForwardingMock : public AmsBackendMock {
  public:
    ForwardingMock() : AmsBackendMock(4) {
        set_dryer_enabled(true);
        set_dryer_humidity_target(true);
    }
    AmsError start_drying(float, int, int, int unit) override {
        calls.push_back("plain unit=" + std::to_string(unit));
        return AmsErrorHelper::success();
    }
    AmsError start_drying_to_humidity(float, int, float pct, int, int unit) override {
        calls.push_back("humidity=" + std::to_string(static_cast<int>(pct)) +
                        " unit=" + std::to_string(unit));
        return AmsErrorHelper::success();
    }
    AmsError stop_drying(int unit) override {
        calls.push_back("stop unit=" + std::to_string(unit));
        return AmsErrorHelper::success();
    }
    std::vector<std::string> calls;
};

std::unique_ptr<AmsBackendMock> make_per_lane_rig() {
    auto mock = std::make_unique<AmsBackendMock>(4);
    mock->set_dryer_enabled(true);
    mock->set_environment_mode("emu_dryer");
    return mock;
}

ZoneDryingState slot_state(const AmsBackendMock& mock, int gate) {
    const auto info = mock.get_system_info();
    const helix::SlotInfo* slot = info.get_slot_global(gate);
    REQUIRE(slot != nullptr);
    return slot->drying_state;
}

lv_obj_t* create_ams_slot(lv_obj_t* parent, int slot_index) {
    const std::string index_str = std::to_string(slot_index);
    const char* attrs[] = {"slot_index", index_str.c_str(), nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_slot", attrs));
}

bool shown(lv_obj_t* slot, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(slot, name);
    REQUIRE(obj != nullptr);
    return !lv_obj_is_hidden(obj);
}

} // namespace

TEST_CASE("A plain drying request is exactly the plain start", "[ams][dryer][dry_gates]") {
    ForwardingMock mock;

    CHECK(mock.start_drying_request({.temp_c = 50.0f, .duration_min = 60, .unit = 1}).success());
    CHECK(mock.start_drying_request(
                  {.temp_c = 50.0f, .duration_min = 60, .humidity_pct = 20.0f, .unit = 0})
              .success());
    CHECK(mock.stop_drying_gates(1, {}).success());
    CHECK(mock.calls ==
          std::vector<std::string>{"plain unit=1", "humidity=20 unit=0", "stop unit=1"});

    // A dryer that heats its gates together has no selection to honour.
    mock.calls.clear();
    CHECK(mock.start_drying_request({.temp_c = 50.0f, .duration_min = 60, .gates = {1}}).result ==
          AmsResult::NOT_SUPPORTED);
    DryingRequest rotate{.temp_c = 50.0f, .duration_min = 60};
    rotate.rotate = true;
    CHECK(mock.start_drying_request(rotate).result == AmsResult::NOT_SUPPORTED);
    CHECK(mock.stop_drying_gates(0, {1}).result == AmsResult::NOT_SUPPORTED);
    CHECK(mock.calls.empty());
    CHECK_FALSE(mock.get_dryer_info(0).supports_gate_selection);
    CHECK_FALSE(mock.get_dryer_info(0).supports_rotation);
}

TEST_CASE("The per-lane mock rig dries lanes one by one", "[ams][dryer][dry_gates]") {
    auto mock = make_per_lane_rig();
    const auto dryer = mock->get_dryer_info(0);
    CHECK(dryer.supports_gate_selection);
    CHECK(dryer.supports_rotation);
    // Its boxes report sensors, so the AMS panel offers the way into the dryer overlay.
    CHECK(mock->has_environment_sensors());

    SECTION("a cycle is already part-way through when the user walks up") {
        // start() has not run, so the declared cycle is the configured state alone.
        CHECK(slot_state(*mock, 0) == ZoneDryingState::Complete);
        CHECK(slot_state(*mock, 2) == ZoneDryingState::Complete);
        CHECK(slot_state(*mock, 3) == ZoneDryingState::Idle);
    }
    SECTION("each lane's zone carries its own state") {
        REQUIRE(mock->start_drying_request(
                        {.temp_c = 50.0f, .duration_min = 60, .unit = 0, .gates = {1, 3}})
                    .success());
        const auto zones = mock->get_environment_zones(0);
        REQUIRE(zones.size() == 4);
        CHECK(zones[0].state == ZoneDryingState::Idle);
        CHECK(zones[1].state == ZoneDryingState::Active);
        CHECK(zones[1].dryer.active);
        CHECK(zones[2].state == ZoneDryingState::Idle);
        CHECK_FALSE(zones[2].dryer.active);
        CHECK(zones[3].state == ZoneDryingState::Queued);
        REQUIRE(mock->stop_drying(0).success());
    }
    SECTION("stopping one lane hands the heater to the next") {
        REQUIRE(mock->start_drying_request(
                        {.temp_c = 50.0f, .duration_min = 60, .unit = 0, .gates = {1, 3}})
                    .success());
        REQUIRE(mock->stop_drying_gates(0, {1}).success());
        CHECK(slot_state(*mock, 1) == ZoneDryingState::Cancelled);
        CHECK(slot_state(*mock, 3) == ZoneDryingState::Active);
        CHECK(mock->get_dryer_info(0).active);

        REQUIRE(mock->stop_drying_gates(0, {3}).success());
        CHECK(slot_state(*mock, 3) == ZoneDryingState::Cancelled);
        CHECK_FALSE(mock->get_dryer_info(0).active);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "AmsState publishes each slot's drying state",
                 "[ams][dryer][dry_gates][state]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    auto owned = make_per_lane_rig();
    AmsBackendMock* mock = owned.get();
    ams.set_backend(std::move(owned));

    REQUIRE(mock->start_drying_request(
                    {.temp_c = 50.0f, .duration_min = 60, .unit = 0, .gates = {2, 0}})
                .success());
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(ams.get_slot_drying_subject(2)) ==
          static_cast<int>(ZoneDryingState::Active));
    CHECK(lv_subject_get_int(ams.get_slot_drying_subject(0)) ==
          static_cast<int>(ZoneDryingState::Queued));
    CHECK(lv_subject_get_int(ams.get_slot_drying_subject(1)) ==
          static_cast<int>(ZoneDryingState::Idle));
    CHECK(ams.get_slot_drying_subject(-1) == nullptr);
    CHECK(ams.get_slot_drying_subject(AmsState::kMaxSlots) == nullptr);

    REQUIRE(mock->stop_drying(0).success());
    ams.clear_backends();
}

TEST_CASE_METHOD(LVGLUITestFixture, "The slot badge shows its lane's drying state",
                 "[ams][dryer][dry_gates][ui]") {
    ui_ams_slot_register();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);

    lv_obj_t* slot = create_ams_slot(test_screen(), 1);
    REQUIRE(slot != nullptr);
    lv_subject_t* drying = ams.get_slot_drying_subject(1);
    REQUIRE(drying != nullptr);

    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Idle));
    CHECK_FALSE(shown(slot, "drying_badge"));

    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Queued));
    CHECK(shown(slot, "drying_badge"));
    CHECK(shown(slot, "drying_badge_queued"));
    CHECK_FALSE(shown(slot, "drying_badge_active"));

    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Active));
    CHECK(shown(slot, "drying_badge_active"));
    CHECK_FALSE(shown(slot, "drying_badge_queued"));

    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Complete));
    CHECK(shown(slot, "drying_badge_complete"));
    CHECK_FALSE(shown(slot, "drying_badge_active"));

    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Cancelled));
    CHECK(shown(slot, "drying_badge_cancelled"));
    CHECK_FALSE(shown(slot, "drying_badge_complete"));

    // Moved to another lane, the badge follows that lane and no longer this one.
    ui_ams_slot_set_index(slot, 2);
    lv_subject_set_int(ams.get_slot_drying_subject(2), static_cast<int>(ZoneDryingState::Idle));
    CHECK_FALSE(shown(slot, "drying_badge"));
    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Active));
    CHECK_FALSE(shown(slot, "drying_badge"));

    lv_subject_set_int(drying, static_cast<int>(ZoneDryingState::Idle));
    lv_obj_delete(slot);
}
