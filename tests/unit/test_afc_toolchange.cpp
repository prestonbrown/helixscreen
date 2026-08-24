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
    // AFC.get_status() publishes current_toolchange as a 1-BASED count of
    // changes started — it bumps the counter before logging "Change N out of M"
    // and clamps its internal -1 sentinel to 0 on the way out. AmsSystemInfo
    // stores a 0-based index, so the backend subtracts one.
    AfcToolchangeTestHelper afc;
    afc.initialize_test_lanes(4);

    SECTION("both fields present — AFC's 1-based count becomes a 0-based index") {
        afc.feed_afc_state(
            {{"current_toolchange", 2}, {"number_of_toolchanges", 5}, {"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == 1);
        REQUIRE(afc.info().number_of_toolchanges == 5);
    }

    SECTION("fields missing — keeps defaults") {
        afc.feed_afc_state({{"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == -1);
        REQUIRE(afc.info().number_of_toolchanges == 0);
    }

    SECTION("pre-first-swap state: AFC's clamped 0 means 'none yet'") {
        afc.feed_afc_state(
            {{"current_toolchange", 0}, {"number_of_toolchanges", 5}, {"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == -1);
        REQUIRE(afc.info().number_of_toolchanges == 5);
    }

    SECTION("a raw -1 sentinel floors at -1 rather than running past it") {
        // Current AFC clamps this away, but a firmware that stopped clamping
        // must not push the index below the documented "none yet" sentinel.
        afc.feed_afc_state(
            {{"current_toolchange", -1}, {"number_of_toolchanges", 5}, {"current_state", "Idle"}});
        REQUIRE(afc.info().current_toolchange == -1);
    }

    SECTION("last change of the print maps to total-1") {
        afc.feed_afc_state({{"current_toolchange", 5}, {"number_of_toolchanges", 5}});
        REQUIRE(afc.info().current_toolchange == 4);
        REQUIRE(afc.info().number_of_toolchanges == 5);
    }

    SECTION("print complete resets to zero") {
        afc.feed_afc_state({{"current_toolchange", 4}, {"number_of_toolchanges", 5}});
        REQUIRE(afc.info().current_toolchange == 3);

        afc.feed_afc_state({{"current_toolchange", 0}, {"number_of_toolchanges", 0}});
        REQUIRE(afc.info().current_toolchange == -1);
        REQUIRE(afc.info().number_of_toolchanges == 0);
    }
}

// ============================================================================
// Regression tests for #379: AFC + toolchanger lane tracking
// ============================================================================

// Extended helper that supports toolchanger updates and multi-extruder setup
class AfcToolchangerLaneHelper : public AmsBackendAfc {
  public:
    AfcToolchangerLaneHelper() : AmsBackendAfc(nullptr, nullptr) {}

    void feed_status_update(const nlohmann::json& params_inner) {
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params_inner, 0.0});
        handle_status_update(notification);
    }

    void initialize_test_lanes(int count) {
        std::vector<std::string> names;
        for (int i = 0; i < count; ++i) {
            names.push_back("lane" + std::to_string(i + 1));
        }
        initialize_slots(names);
    }

    void initialize_test_lanes_with_tool_map(int count) {
        initialize_test_lanes(count);

        // Set up 1:1 tool-to-slot mapping (T0->slot0, T1->slot1, ...)
        system_info_.tool_to_slot_map.clear();
        for (int i = 0; i < count; ++i) {
            system_info_.tool_to_slot_map.push_back(i);
        }
    }

    // Set extruder names via AFC state (same path as production code)
    void setup_extruder_names(const std::vector<std::string>& names) {
        nlohmann::json afc_data;
        afc_data["extruders"] = names;
        afc_data["current_state"] = "Idle";
        nlohmann::json params;
        params["AFC"] = afc_data;
        feed_status_update(params);
    }

    const AmsSystemInfo& info() const {
        return system_info_;
    }
};

TEST_CASE("AFC backend parses toolchanger.tool_number from Klipper status (#379)",
          "[afc][toolchange][regression]") {
    AfcToolchangerLaneHelper afc;
    afc.initialize_test_lanes(4);
    afc.setup_extruder_names({"extruder", "extruder1"});

    SECTION("toolchanger.tool_number updates current_tool") {
        nlohmann::json params;
        params["toolchanger"] = {{"tool_number", 1}};
        afc.feed_status_update(params);

        REQUIRE(afc.info().current_tool == 1);
    }

    SECTION("toolchanger.tool_number=-1 means no tool selected") {
        nlohmann::json params;
        params["toolchanger"] = {{"tool_number", 2}};
        afc.feed_status_update(params);
        REQUIRE(afc.info().current_tool == 2);

        params["toolchanger"] = {{"tool_number", -1}};
        afc.feed_status_update(params);
        REQUIRE(afc.info().current_tool == -1);
    }

    SECTION("toolchanger update with tool_to_slot_map triggers reconciliation") {
        // With a tool map, reconciliation sets current_slot from the map
        afc.initialize_test_lanes_with_tool_map(4);
        afc.setup_extruder_names({"extruder", "extruder1"});

        nlohmann::json params;
        params["toolchanger"] = {{"tool_number", 1}};
        afc.feed_status_update(params);

        REQUIRE(afc.info().current_tool == 1);
        REQUIRE(afc.info().current_slot == 1);
        REQUIRE(afc.info().filament_loaded == true);
    }

    SECTION("regression #379: correct lane selected when toolchanger updates current_tool") {
        // Before fix: current_tool starts at -1, AFC_extruder updates arrive
        // for both extruders, and the first one with a loaded lane wins
        // (picking the wrong lane). After fix: toolchanger.tool_number
        // arrives first, so current_tool is correct.

        // Step 1: Klipper sends toolchanger update (T1 active)
        nlohmann::json tc_params;
        tc_params["toolchanger"] = {{"tool_number", 1}};
        afc.feed_status_update(tc_params);
        REQUIRE(afc.info().current_tool == 1);

        // Step 2: AFC_extruder updates arrive for both extruders
        // extruder (T0) has lane1 loaded, extruder1 (T1) has lane3 loaded
        nlohmann::json ext_params;
        ext_params["AFC_extruder extruder"] = {{"lane_loaded", "lane1"}};
        ext_params["AFC_extruder extruder1"] = {{"lane_loaded", "lane3"}};
        afc.feed_status_update(ext_params);

        // With correct current_tool=1, extruder1 (T1) is the active tool
        // so its lane3 (slot index 2) should be selected
        REQUIRE(afc.info().current_slot == 2); // lane3 = slot index 2
    }

    SECTION("without toolchanger update, fallback picks first loaded lane") {
        // Documents fallback behavior when current_tool is stale (-1):
        // The first extruder with a loaded lane wins via the current_slot < 0 path.

        nlohmann::json ext_params;
        ext_params["AFC_extruder extruder"] = {{"lane_loaded", "lane1"}};
        ext_params["AFC_extruder extruder1"] = {{"lane_loaded", "lane3"}};
        afc.feed_status_update(ext_params);

        REQUIRE(afc.info().current_slot == 0); // lane1 = slot index 0 (fallback)
    }

    SECTION("a current_tool key on the AFC object does NOT override the toolchanger") {
        // AFC.get_status() has never published a "current_tool" key on any
        // version (AFC.py v1.2.0:2531-2564 and the v1.1.0 equivalent publish
        // current_load / current_lane / next_lane / current_state / … and no
        // tool number). The branch that let this phantom field outrank
        // Klipper's own toolchanger.tool_number was therefore unreachable, and
        // honouring it would have let an invented key point the UI at a tool
        // that is not on the carriage.
        //
        // toolchanger.tool_number is the real source, and it stays authoritative.
        nlohmann::json tc_params;
        tc_params["toolchanger"] = {{"tool_number", 1}};
        afc.feed_status_update(tc_params);
        REQUIRE(afc.info().current_tool == 1);

        nlohmann::json afc_params;
        afc_params["AFC"] = {{"current_tool", 0}, {"current_state", "Idle"}};
        afc.feed_status_update(afc_params);
        REQUIRE(afc.info().current_tool == 1);
    }

    SECTION("toolchanger update with non-object value is ignored") {
        nlohmann::json params;
        params["toolchanger"] = "invalid";
        afc.feed_status_update(params);
        REQUIRE(afc.info().current_tool == -1);
    }

    SECTION("toolchanger update without tool_number field is ignored") {
        nlohmann::json params;
        params["toolchanger"] = {{"status", "ready"}};
        afc.feed_status_update(params);
        REQUIRE(afc.info().current_tool == -1);
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

#include "ui_ams_tool_text.h"
#include "ui_update_queue.h"

#include "../ui_test_utils.h"
#include "ams_state.h"
#include "static_subject_registry.h"
#include "tool_state.h"

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

// ============================================================================
// Rendered toolchange text — the AFC wire value all the way to the label
// ============================================================================

// Drives the real formatter observers from src/ui/ui_ams_tool_text.cpp over the
// real AmsState subjects, fed by the real AFC parser. Nothing here reimplements
// the format string, so the assertions break if either half of the 1-based /
// 0-based contract moves.
struct AfcToolchangeRenderFixture {
    AfcToolchangeRenderFixture() {
        lv_init_safe();
        auto& state = AmsState::instance();
        state.init_subjects(false);
        // init_ams_tool_text_observers() also observes ToolState's tool badge.
        ToolState::instance().init_subjects(false);
        helix::ui::init_ams_tool_text_observers();

        auto backend = std::make_unique<AfcToolchangeTestHelper>();
        backend->initialize_test_lanes(4);
        afc_ = backend.get();
        state.set_backend(std::move(backend));
    }

    ~AfcToolchangeRenderFixture() {
        // Release the formatter observers before the subjects they watch die,
        // and clear the file-static "already initialized" latch for the next
        // section. deinit_one() rather than deinit_all(): the test binary's
        // registry also holds entries from earlier fixtures.
        StaticSubjectRegistry::instance().deinit_one("AmsToolTextObservers");
        AmsState::instance().deinit_subjects();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().shutdown();
    }

    /// Feed an AFC status frame exactly as the printer publishes it (1-based
    /// count) and let AmsState push it into the subjects the formatter observes.
    void feed_afc(int afc_count, int total) {
        afc_->feed_afc_state({{"current_toolchange", afc_count},
                              {"number_of_toolchanges", total},
                              {"current_state", "Idle"}});
        AmsState::instance().sync_from_backend();
        helix::ui::UpdateQueue::instance().drain();
    }

    std::string rendered() const {
        return std::string(
            lv_subject_get_string(AmsState::instance().get_toolchange_text_subject()));
    }

    AfcToolchangeTestHelper* afc_ = nullptr;
};

TEST_CASE_METHOD(AfcToolchangeRenderFixture, "AFC toolchange text renders AFC's own numbering",
                 "[afc][toolchange][format][render]") {
    SECTION("before the first change AFC reports 0 and the label reads 0 / N") {
        // Deliberate, and a visible change from the pre-normalization "1 / 161":
        // the field is a count of changes ACCOUNTED FOR, so before any change has
        // happened the honest reading is zero. The row stays visible either way —
        // visibility is gated on number_of_toolchanges > 0 alone
        // (ams_state.cpp), independent of this value.
        feed_afc(0, 161);
        REQUIRE(rendered() == "0 / 161");
    }

    SECTION("AFC's 'Change 1 out of 161' renders as 1 / 161, not 2 / 161") {
        feed_afc(1, 161);
        REQUIRE(rendered() == "1 / 161");
    }

    SECTION("mid print the label tracks AFC's console count") {
        feed_afc(37, 161);
        REQUIRE(rendered() == "37 / 161");
    }

    SECTION("the final change renders N / N, not N+1 / N") {
        // Assert one change BELOW the ceiling first. At the ceiling the clamp
        // also turns a broken 1-based store into "161 / 161", so feeding only
        // the final change cannot tell the fix from the bug it is named for —
        // drop the backend's -1 normalization and this section still passed.
        // At wire 160 the clamp is inert: correct normalization renders
        // "160 / 161" and a missing -1 renders "161 / 161".
        feed_afc(160, 161);
        REQUIRE(rendered() == "160 / 161");

        feed_afc(161, 161);
        REQUIRE(rendered() == "161 / 161");
    }

    SECTION("an over-reporting backend is clamped to the total") {
        // Defense in depth for the display layer: whatever a backend claims, the
        // label can never show more changes than the print has.
        feed_afc(162, 161);
        REQUIRE(rendered() == "161 / 161");
    }

    SECTION("no changes expected renders empty") {
        feed_afc(0, 0);
        REQUIRE(rendered().empty());
    }
}
