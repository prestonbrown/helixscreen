// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_many_unit_env.cpp
 * @brief The detail env chip and the viewed unit's disconnected flag describe the
 *        unit they name for ANY unit index, not only those with per-unit subjects.
 */

#include "ui_ams_environment_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "static_panel_registry.h"

#include <lvgl/lvgl.h>

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Enough units that the last two sit past the per-unit subjects.
constexpr int kUnits = AmsState::MAX_UNITS + 4;
constexpr int kLast = kUnits - 1;
// A unit at the first index with no per-unit subjects, and its predecessor.
constexpr int kPast = AmsState::MAX_UNITS + 2;
constexpr int kBelow = AmsState::MAX_UNITS - 1;

class FleetMock : public AmsBackendMock {
  public:
    FleetMock() : AmsBackendMock(kUnits * 4) {}

    float temp_of(int u) const {
        return 20.0f + static_cast<float>(u);
    }

    AmsSystemInfo get_system_info() const override {
        AmsSystemInfo info = AmsBackendMock::get_system_info();
        info.units.clear();
        for (int u = 0; u < kUnits; ++u) {
            AmsUnit unit;
            unit.unit_index = u;
            unit.name = "fleet_unit_" + std::to_string(u);
            unit.slot_count = 4;
            unit.first_slot_global_index = u * 4;
            unit.connected = !(u == disconnected_unit);
            EnvironmentData env;
            env.temperature_c = temp_of(u) + bump;
            env.humidity_pct = 30.0f + static_cast<float>(u);
            env.has_humidity = (u != kLast);
            unit.environment = env;
            for (int s = 0; s < 4; ++s) {
                SlotInfo slot;
                slot.slot_index = s;
                slot.global_index = u * 4 + s;
                unit.slots.push_back(slot);
            }
            info.units.push_back(unit);
        }
        info.total_slots = kUnits * 4;
        return info;
    }

    DryerInfo get_dryer_info(int unit = 0) const override {
        DryerInfo d;
        d.supported = true;
        d.active = (unit == drying_unit);
        d.remaining_min = 135;
        return d;
    }

    int disconnected_unit = -1;
    int drying_unit = kPast;
    float bump = 0.0f;
};

FleetMock* install() {
    AmsState::instance().deinit_subjects();
    auto mock = std::make_unique<FleetMock>();
    FleetMock* raw = mock.get();
    REQUIRE(mock->start().success());
    AmsState::instance().set_backend(std::move(mock));
    AmsState::instance().init_subjects(true);
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    return raw;
}

int subject_int(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_int(subj);
}

std::string subject_str(const char* name) {
    lv_subject_t* subj = lv_xml_get_subject(nullptr, name);
    REQUIRE(subj != nullptr);
    return lv_subject_get_string(subj);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "The detail env chip shows a unit past the per-unit cap",
                 "[ams][pages][env]") {
    static_assert(kUnits > AmsState::MAX_UNITS);
    FleetMock* mock = install();
    auto& ams = AmsState::instance();

    ams.set_detail_env_unit(kPast);
    helix::ui::UpdateQueue::instance().drain();

    CHECK(subject_str("ams_env_ind_detail_temp_text") == std::to_string(20 + kPast) + "\xC2\xB0"
                                                                            "C");
    CHECK(subject_str("ams_env_ind_detail_humidity_text") == std::to_string(30 + kPast) + "%");
    CHECK(subject_int("ams_env_ind_detail_visible") == 1);
    CHECK(subject_int("ams_env_ind_detail_humidity_visible") == 1);
    CHECK(subject_int("ams_env_ind_detail_drying_active") == 1);
    CHECK(subject_str("ams_env_ind_detail_drying_text") == "2:15");

    SECTION("the humidity row hides for a unit without a humidity reading") {
        ams.set_detail_env_unit(kLast);
        CHECK(subject_int("ams_env_ind_detail_humidity_visible") == 0);
        CHECK(subject_int("ams_env_ind_detail_drying_active") == 0);
        CHECK(subject_str("ams_env_ind_detail_temp_text") == std::to_string(20 + kLast) + "\xC2\xB0"
                                                                                "C");
    }

    SECTION("an update to the detail unit's data re-mirrors") {
        mock->bump = 5.0f;
        ams.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(subject_str("ams_env_ind_detail_temp_text") == std::to_string(25 + kPast) + "\xC2\xB0"
                                                                                "C");
    }

    SECTION("a unit below the cap still mirrors its own reading") {
        ams.set_detail_env_unit(kBelow);
        CHECK(subject_str("ams_env_ind_detail_temp_text") == std::to_string(20 + kBelow) + "\xC2\xB0"
                                                                                           "C");
        CHECK(subject_int("ams_env_ind_detail_drying_active") == 0);
    }

    AmsState::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLUITestFixture, "The viewed unit's disconnected flag covers any unit index",
                 "[ams][pages][env]") {
    FleetMock* mock = install();
    mock->disconnected_unit = kPast;
    AmsState::instance().sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();

    AmsState::instance().set_viewed_unit(kPast);
    CHECK(subject_int("ams_viewed_unit_disconnected") == 1);
    AmsState::instance().set_viewed_unit(kPast - 1);
    CHECK(subject_int("ams_viewed_unit_disconnected") == 0);
    AmsState::instance().set_viewed_unit(kPast);
    CHECK(subject_int("ams_viewed_unit_disconnected") == 1);

    SECTION("the flag follows the unit's connection as the backend reports it") {
        mock->disconnected_unit = -1;
        AmsState::instance().sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
        CHECK(subject_int("ams_viewed_unit_disconnected") == 0);
    }

    AmsState::instance().deinit_subjects();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "The environment overlay of a unit past the per-unit subjects stays live",
                 "[ams][pages][env]") {
    FleetMock* mock = install();
    auto& ams = AmsState::instance();
    // The unit view points the env chip's mirror at the unit on screen, and its overlay
    // opens for that unit.
    ams.set_detail_env_unit(kPast);
    helix::ui::UpdateQueue::instance().drain();

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    auto& overlay = helix::ui::get_ams_environment_overlay();
    overlay.show_zone(test_screen(), mock->get_environment_zones(kPast), 0, false);
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    REQUIRE(subject_str("ams_env_overlay_temp_text").rfind(std::to_string(20 + kPast), 0) == 0);

    mock->bump = 5.0f;
    ams.sync_from_backend();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    CHECK(subject_str("ams_env_overlay_temp_text").rfind(std::to_string(25 + kPast), 0) == 0);

    NavigationManager::instance().go_back();
    helix::ui::UpdateQueue::instance().drain();
    process_lvgl(10);
    AmsState::instance().deinit_subjects();
}
