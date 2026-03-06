// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_mock.h"
#include "ams_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// Test helper — inherits from AmsBackendAfc to call protected parse methods
class AfcToolchangeTestHelper : public AmsBackendAfc {
  public:
    AfcToolchangeTestHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void initialize_test_lanes(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
        }
        initialize_slots(names);
    }

    void feed_afc_state(const nlohmann::json& afc_data) {
        nlohmann::json notification;
        nlohmann::json params;
        params["AFC"] = afc_data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    const AmsSystemInfo& info() const {
        return system_info_;
    }
};

TEST_CASE("AFC toolchange fields in AmsSystemInfo default to safe values", "[afc][toolchange]") {
    AmsSystemInfo info;
    REQUIRE(info.current_toolchange == -1);
    REQUIRE(info.number_of_toolchanges == 0);
}

TEST_CASE("AFC backend parses toolchange fields from status update", "[afc][toolchange]") {
    AfcToolchangeTestHelper afc;
    afc.initialize_test_lanes(4);

    SECTION("both fields present") {
        afc.feed_afc_state(
            {{"current_toolchange", 2}, {"number_of_toolchanges", 5}, {"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == 2);
        REQUIRE(afc.info().number_of_toolchanges == 5);
    }

    SECTION("fields missing — keeps defaults") {
        afc.feed_afc_state({{"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == -1);
        REQUIRE(afc.info().number_of_toolchanges == 0);
    }

    SECTION("pre-first-swap state: current=-1, total=5") {
        afc.feed_afc_state(
            {{"current_toolchange", -1}, {"number_of_toolchanges", 5}, {"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == -1);
        REQUIRE(afc.info().number_of_toolchanges == 5);
    }

    SECTION("print complete resets to zero") {
        afc.feed_afc_state({{"current_toolchange", 4}, {"number_of_toolchanges", 5}});
        REQUIRE(afc.info().current_toolchange == 4);

        afc.feed_afc_state({{"current_toolchange", 0}, {"number_of_toolchanges", 0}});
        REQUIRE(afc.info().current_toolchange == 0);
        REQUIRE(afc.info().number_of_toolchanges == 0);
    }
}

// Test helper for HH toolchange — reuses the pattern from test_ams_backend_happy_hare.cpp
class HHToolchangeTestHelper : public AmsBackendHappyHare {
  public:
    HHToolchangeTestHelper() : AmsBackendHappyHare(nullptr, nullptr) {}

    void feed_mmu_state(const nlohmann::json& mmu_data) {
        nlohmann::json notification;
        nlohmann::json params;
        params["mmu"] = mmu_data;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    const AmsSystemInfo& info() const {
        return system_info_;
    }
};

TEST_CASE("Happy Hare backend parses toolchange fields", "[hh][toolchange]") {
    HHToolchangeTestHelper hh;

    SECTION("num_toolchanges maps to current_toolchange (count-1)") {
        // num_toolchanges=3 means 3 swaps done => 0-based index = 2
        hh.feed_mmu_state(
            {{"num_toolchanges", 3}, {"slicer_tool_map", {{"total_toolchanges", 8}}}});
        REQUIRE(hh.info().current_toolchange == 2);
        REQUIRE(hh.info().number_of_toolchanges == 8);
    }

    SECTION("num_toolchanges=0 before first swap") {
        hh.feed_mmu_state(
            {{"num_toolchanges", 0}, {"slicer_tool_map", {{"total_toolchanges", 5}}}});
        REQUIRE(hh.info().current_toolchange == -1);
        REQUIRE(hh.info().number_of_toolchanges == 5);
    }

    SECTION("slicer_tool_map.total_toolchanges is null") {
        hh.feed_mmu_state(
            {{"num_toolchanges", 2}, {"slicer_tool_map", {{"total_toolchanges", nullptr}}}});
        REQUIRE(hh.info().current_toolchange == 1);
        REQUIRE(hh.info().number_of_toolchanges == 0);
    }

    SECTION("slicer_tool_map missing entirely") {
        hh.feed_mmu_state({{"num_toolchanges", 2}});
        REQUIRE(hh.info().current_toolchange == 1);
        REQUIRE(hh.info().number_of_toolchanges == 0);
    }

    SECTION("fields missing keeps defaults") {
        hh.feed_mmu_state({{"action", "Idle"}});
        REQUIRE(hh.info().current_toolchange == -1);
        REQUIRE(hh.info().number_of_toolchanges == 0);
    }
}

TEST_CASE("Mock backend supports toolchange simulation", "[afc][toolchange][mock]") {
    AmsBackendMock mock(4);

    SECTION("set_toolchange_progress updates system info") {
        mock.set_toolchange_progress(2, 5);
        auto info = mock.get_system_info();
        REQUIRE(info.current_toolchange == 2);
        REQUIRE(info.number_of_toolchanges == 5);
    }

    SECTION("defaults are -1 and 0") {
        auto info = mock.get_system_info();
        REQUIRE(info.current_toolchange == -1);
        REQUIRE(info.number_of_toolchanges == 0);
    }
}

// ============================================================================
// AmsState subject tests (require LVGL)
// ============================================================================

#include "ui_update_queue.h"

#include "../ui_test_utils.h"
#include "ams_state.h"
#include "static_subject_registry.h"

#include <lvgl.h>

// LVGL test fixture - init per test case (no lv_deinit to avoid destroying shared state)
struct LvglFixture {
    LvglFixture() {
        lv_init_safe();
    }
    ~LvglFixture() {
        AmsState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().shutdown();
    }
};

TEST_CASE("AmsState toolchange subjects reflect backend data", "[afc][toolchange][subjects]") {
    LvglFixture lv;
    auto& state = AmsState::instance();
    state.init_subjects(false);

    auto* vis_subj = state.get_toolchange_visible_subject();
    auto* text_subj = state.get_toolchange_text_subject();

    SECTION("initially hidden") {
        REQUIRE(lv_subject_get_int(vis_subj) == 0);
        REQUIRE(std::string(lv_subject_get_string(text_subj)).empty());
    }
}

TEST_CASE("AmsState toolchange text formatting", "[afc][toolchange][format]") {
    LvglFixture lv;
    auto& state = AmsState::instance();
    state.init_subjects(false);

    // Create and set a mock backend
    auto mock = std::make_unique<AmsBackendMock>(4);
    auto* mock_ptr = mock.get();
    state.set_backend(std::move(mock));

    auto* vis_subj = state.get_toolchange_visible_subject();
    auto* text_subj = state.get_toolchange_text_subject();

    SECTION("mid-print: raw subjects reflect 0-based current and total") {
        mock_ptr->set_toolchange_progress(2, 5); // 0-based: 3rd swap of 5
        state.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(vis_subj) == 1);
        // Raw int subjects store backend values; text formatting is UI-layer
        REQUIRE(lv_subject_get_int(state.get_ams_current_toolchange_subject()) == 2);
        REQUIRE(lv_subject_get_int(state.get_ams_number_of_toolchanges_subject()) == 5);
    }

    SECTION("before first swap: current is -1, total is N") {
        mock_ptr->set_toolchange_progress(-1, 5);
        state.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(vis_subj) == 1);
        REQUIRE(lv_subject_get_int(state.get_ams_current_toolchange_subject()) == -1);
        REQUIRE(lv_subject_get_int(state.get_ams_number_of_toolchanges_subject()) == 5);
    }

    SECTION("first swap complete: current is 0, total is 5") {
        mock_ptr->set_toolchange_progress(0, 5);
        state.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(vis_subj) == 1);
        REQUIRE(lv_subject_get_int(state.get_ams_current_toolchange_subject()) == 0);
        REQUIRE(lv_subject_get_int(state.get_ams_number_of_toolchanges_subject()) == 5);
    }

    SECTION("no swaps expected: hidden") {
        mock_ptr->set_toolchange_progress(-1, 0);
        state.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();

        REQUIRE(lv_subject_get_int(vis_subj) == 0);
        REQUIRE(std::string(lv_subject_get_string(text_subj)).empty());
    }

    SECTION("print ends, AFC resets: hidden") {
        // Mid-print
        mock_ptr->set_toolchange_progress(3, 5);
        state.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(vis_subj) == 1);

        // Print complete - AFC resets
        mock_ptr->set_toolchange_progress(0, 0);
        state.sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE(lv_subject_get_int(vis_subj) == 0);
    }
}
