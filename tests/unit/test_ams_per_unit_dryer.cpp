// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "../test_helpers/qidi_box_test_access.h"
#include "ams_backend_qidi.h"
#include "ams_state.h"

#include <ctime>
#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::printer;
using json = nlohmann::json;

// Build a 2-box QIDI backend with box1 actively drying and box2 idle.
static std::unique_ptr<AmsBackendQidi> make_two_box_qidi_one_drying() {
    auto backend = std::make_unique<AmsBackendQidi>(nullptr, nullptr);
    QidiBoxTestAccess::parse_vars(*backend, json{{"box_count", 2}});
    QidiBoxTestAccess::set_clock(*backend, [] { return std::time_t{1'000'000}; });
    QidiBoxTestAccess::set_drying_timer_supported(*backend, true);
    QidiBoxTestAccess::handle_status(
        *backend,
        json{{"heater_generic heater_box1", json{{"temperature", 48.0}, {"target", 55.0}}},
             {"heater_generic heater_box2", json{{"temperature", 24.0}, {"target", 0.0}}}});
    QidiBoxTestAccess::apply_box_extras(
        *backend, json{{"box_drying_state", json{{"box1", json{{"end_time", 1'000'000 + 30 * 60}}},
                                                 {"box2", json{{"end_time", 0}}}}}});
    return backend;
}

TEST_CASE_METHOD(LVGLTestFixture, "Per-unit dryer fan-out: only the drying box is active",
                 "[ams][dryer][multi-unit][fanout]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ams.set_backend(make_two_box_qidi_one_drying());
    ams.sync_from_backend();

    lv_subject_t* a0 = ams.get_env_ind_drying_active_subject(0);
    lv_subject_t* a1 = ams.get_env_ind_drying_active_subject(1);
    REQUIRE(a0 != nullptr);
    REQUIRE(a1 != nullptr);
    CHECK(lv_subject_get_int(a0) == 1);
    CHECK(lv_subject_get_int(a1) == 0);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "Dryer scalar subjects mirror the selected unit",
                 "[ams][dryer][multi-unit][mirror]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false); // MANDATORY: without this every lv_subject_set_int is a silent
                              // no-op → false-RED (Task 4 caught this). Mirrors sibling
                              // test_ams_realtime_filament_state.cpp.
    ams.set_backend(make_two_box_qidi_one_drying());

    ams.set_dryer_mirror_unit(0);
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(ams.get_dryer_active_subject()) == 1);

    ams.set_dryer_mirror_unit(1);
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(ams.get_dryer_active_subject()) == 0);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Per-unit dryer fan-out: the dryer version moves with the set of drying boxes",
                 "[ams][dryer][multi-unit][fanout]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto owned = make_two_box_qidi_one_drying();
    AmsBackendQidi* backend = owned.get();
    ams.set_backend(std::move(owned));
    ams.sync_from_backend();

    lv_subject_t* version = ams.get_units_dryer_version_subject();
    REQUIRE(version != nullptr);
    const int with_box1 = lv_subject_get_int(version);
    CHECK(with_box1 > 0);

    // The same boxes drying: nothing to announce.
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(version) == with_box1);

    // Box 1 stops and box 2 starts: the count of drying boxes is still one, but a unit on
    // another page needs to hear that it is a different one.
    QidiBoxTestAccess::apply_box_extras(
        *backend,
        json{{"box_drying_state", json{{"box1", json{{"end_time", 0}}},
                                       {"box2", json{{"end_time", 1'000'000 + 30 * 60}}}}}});
    ams.sync_from_backend();
    CHECK(lv_subject_get_int(version) != with_box1);

    ams.deinit_subjects();
}

TEST_CASE_METHOD(LVGLTestFixture, "Detail env subjects mirror the selected detail unit",
                 "[ams][dryer][multi-unit][detail]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(
        false); // MANDATORY (see Task 4/5): else set_int is a silent no-op → false RED
    ams.set_backend(make_two_box_qidi_one_drying());

    ams.sync_from_backend();    // populate per-unit env_ind_* subjects
    ams.set_detail_env_unit(0); // mirror unit 0 (box1 drying)
    CHECK(lv_subject_get_int(ams.get_env_ind_detail_drying_active_subject()) == 1);

    ams.set_detail_env_unit(1); // mirror unit 1 (box2 idle)
    CHECK(lv_subject_get_int(ams.get_env_ind_detail_drying_active_subject()) == 0);

    ams.deinit_subjects();
}
