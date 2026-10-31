// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_optimistic_action.cpp
 * @brief A UI-started load or unload must not read as Idle before the backend
 *        has said anything about it (prestonbrown/helixscreen#1057).
 *
 * The production sidebar marks the operation busy (HEATING) the moment the
 * user taps, then waits for a UI preheat or for the backend's first frame.
 * Every sync in that window copies the backend's action, and the sidebar's own
 * stall watchdog syncs every 1.5s while the action is busy. A backend that
 * publishes nothing at dispatch still reports IDLE there.
 *
 * Each case builds the real sidebar over a real backend, drives the backend's
 * own status parser, and lets the real watchdog timer fire.
 */

#include "ui_ams_sidebar.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "ams_backend_afc.h"
#include "ams_backend_cfs.h"
#include "ams_backend_happy_hare.h"
#include "ams_backend_snapmaker.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "ams_types.h"
#include "filament_op_heater.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lib/lvgl/src/misc/lv_timer_private.h"
#include "moonraker_client_mock.h"
#include "post_op_cooldown_manager.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/ams_sidebar_xml.h"
#include "test_helpers/ams_state_test_access.h"
#include "test_helpers/cfs_test_access.h"
#include "test_helpers/gcode_recording_api.h"
#include "test_helpers/happy_hare_test_access.h"
#include "test_helpers/scoped_shared_resource.h"
#include "test_helpers/snapmaker_test_access.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::AmsAction;
using helix::AmsError;
using helix::AmsErrorHelper;
using helix::AmsState;
using helix::SlotStatus;

namespace helix {

/// Happy Hare with four available gates and running, sending through @p api.
/// HH sets no action of its own at dispatch: mmu.action is the only thing that
/// moves it.
class HhOptimisticBackend : public AmsBackendHappyHare {
  public:
    explicit HhOptimisticBackend(IMoonrakerAPI* api) : AmsBackendHappyHare(api, nullptr) {
        std::vector<std::string> names{"0", "1", "2", "3"};
        HappyHareTestAccess::slots(*this).initialize("MMU", names);
        AmsUnit unit;
        unit.name = "Happy Hare MMU";
        unit.slot_count = 4;
        for (int i = 0; i < 4; ++i) {
            if (auto* entry = HappyHareTestAccess::slots(*this).get_mut(i)) {
                entry->info.status = SlotStatus::AVAILABLE;
            }
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = i;
            slot.status = SlotStatus::AVAILABLE;
            unit.slots.push_back(slot);
        }
        system_info_.units.push_back(unit);
        system_info_.total_slots = 4;
        running_ = true;
    }

    bool toolhead_homed() const override {
        return homed;
    }

    void seat(int slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.filament_loaded = true;
        system_info_.current_slot = slot;
    }

    void stop_for_test() {
        running_ = false;
    }

    /// A printer.mmu delta through the real notification handler.
    void feed_mmu(const nlohmann::json& mmu) {
        nlohmann::json params;
        params["mmu"] = mmu;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    bool homed = true;
};

/// AFC with four lanes, running, gcode captured.
class AfcOptimisticBackend : public AmsBackendAfc {
  public:
    AfcOptimisticBackend() : AmsBackendAfc(nullptr, nullptr) {
        std::vector<std::string> lanes{"lane1", "lane2", "lane3", "lane4"};
        AfcTestAccess::initialize_slots(*this, lanes);
        for (int i = 0; i < 4; ++i) {
            if (auto* entry = AfcTestAccess::slots(*this).get_mut(i)) {
                entry->info.status = SlotStatus::AVAILABLE;
            }
        }
        running_ = true;
    }

    AmsError execute_gcode(const std::string& gcode) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    AmsError execute_gcode(const std::string& gcode, helix::SharedCallback<void()>) override {
        sent.push_back(gcode);
        return AmsErrorHelper::success();
    }
    bool toolhead_homed() const override {
        return true;
    }

    void seat(int slot) {
        std::lock_guard<std::mutex> lock(mutex_);
        AfcTestAccess::system_info(*this).filament_loaded = true;
        AfcTestAccess::system_info(*this).current_slot = slot;
    }

    void feed_afc(const nlohmann::json& afc) {
        nlohmann::json params;
        params["AFC"] = afc;
        nlohmann::json notification;
        notification["params"] = nlohmann::json::array({params, 0.0});
        handle_status_update(notification);
    }

    std::vector<std::string> sent;
};

/// Snapmaker U1, running, sending through @p api. The firmware publishes
/// nothing for an AUTO_FEEDING it has accepted until its channel_state moves,
/// and the AUTO_FEEDING answer arrives only when the feed is over.
class U1OptimisticBackend : public AmsBackendSnapmaker {
  public:
    explicit U1OptimisticBackend(IMoonrakerAPI* api) : AmsBackendSnapmaker(api, nullptr) {
        running_.store(true);
    }

    /// One head's filament_feed channel and motion sensor through the real
    /// status parser: a fitted module in automatic mode with its sensor armed,
    /// unless @p auto_mode says otherwise.
    void feed(int extruder, const std::string& channel_state, bool auto_mode = true) {
        const char* feed_key = (extruder <= 1) ? "filament_feed left" : "filament_feed right";
        nlohmann::json status = {
            {feed_key,
             {{"extruder" + std::to_string(extruder),
               {{"filament_detected", true},
                {"channel_state", channel_state},
                {"channel_error", "ok"},
                {"module_exist", true},
                {"disable_auto", !auto_mode}}}}},
            {"filament_motion_sensor e" + std::to_string(extruder) + "_filament",
             {{"enabled", true}}}};
        SnapmakerTestAccess::handle_status(*this, status);
    }

    /// Head @p extruder loaded to its nozzle and active.
    void seat(int extruder) {
        feed(extruder, "load_finish");
        SnapmakerTestAccess::set_current_slot(*this, extruder);
        SnapmakerTestAccess::set_current_tool(*this, extruder);
    }
};

namespace printer {

/// CFS, running, with the action script captured instead of sent.
class CfsOptimisticBackend : public AmsBackendCfs {
  public:
    CfsOptimisticBackend() : AmsBackendCfs(nullptr, nullptr) {
        running_ = true;
    }
    bool toolhead_homed() const override {
        return true;
    }

    std::vector<std::string> dispatched;

  private:
    AmsError dispatch_action_script(std::string gcode) override {
        dispatched.push_back(std::move(gcode));
        return AmsErrorHelper::success();
    }
};

} // namespace printer
} // namespace helix

namespace {

/// The production sidebar over a chosen backend, built the way ams_panel.xml
/// builds it.
struct OptimisticSidebarFixture : public XMLTestFixture {
    template <typename BackendT, typename... Args> BackendT& build(Args&&... args) {
        auto owned = std::make_unique<BackendT>(std::forward<Args>(args)...);
        BackendT& backend = *owned;
        AmsState::instance().set_backend(std::move(owned));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();

        helix::test::register_ams_sidebar_xml();
        panel_ = lv_obj_create(test_screen());
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        REQUIRE(lv_xml_create(panel_, "ams_sidebar", attrs) != nullptr);
        sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(state());
        REQUIRE(sidebar_->setup(panel_));
        sidebar_->init_observers(); // as the AMS panels do after setup()
        return backend;
    }

    ~OptimisticSidebarFixture() override {
        sidebar_.reset();
        if (panel_ && helix::test::widget_in_tree(panel_)) {
            lv_obj_delete(panel_);
        }
        helix::ui::UpdateQueue::instance().drain();
        AmsState::instance().set_backend(nullptr);
    }

    [[nodiscard]] static AmsAction shown_action() {
        return static_cast<AmsAction>(
            lv_subject_get_int(AmsState::instance().get_ams_action_subject()));
    }

    /// One tick of the sidebar's 1.5s stall watchdog, after the queue drains.
    /// The test pump runs only one-shot timers, so the periodic watchdog is
    /// found by its owner and fired directly.
    void let_watchdog_run() {
        // A deferred callback can queue another, so drain until quiet.
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        int fired = 0;
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            if (lv_timer_get_user_data(t) == sidebar_.get()) {
                t->timer_cb(t);
                ++fired;
            }
        }
        REQUIRE(fired == 1);
    }

    [[nodiscard]] helix::HhOptimisticBackend& build_hh() {
        return build<helix::HhOptimisticBackend>(&api_);
    }

    [[nodiscard]] helix::U1OptimisticBackend& build_u1() {
        return build<helix::U1OptimisticBackend>(&api_);
    }

    [[nodiscard]] bool steps_shown() const {
        lv_obj_t* container = lv_obj_find_by_name(panel_, "progress_stepper_container");
        REQUIRE(container != nullptr);
        return !lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN);
    }

    /// The step bar widget the sidebar has built, or null.
    [[nodiscard]] lv_obj_t* stepper() const {
        lv_obj_t* container = lv_obj_find_by_name(panel_, "progress_stepper_container");
        REQUIRE(container != nullptr);
        return lv_obj_get_child_count(container) > 0 ? lv_obj_get_child(container, 0) : nullptr;
    }

    [[nodiscard]] static helix::StepOperationType shown_steps() {
        return AmsState::instance().get_active_step_operation();
    }

    [[nodiscard]] static bool held() {
        return AmsState::instance().optimistic_action_held();
    }

    void set_nozzle(int temp_c, int target_c) {
        lv_subject_set_int(state().temperature_state().get_active_extruder_target_subject(),
                           target_c * 10);
        lv_subject_set_int(state().temperature_state().get_active_extruder_temp_subject(),
                           temp_c * 10);
        // The sidebar's temperature observers run deferred, one frame at a time.
        helix::ui::UpdateQueue::instance().drain();
    }

    /// A nozzle already at temperature, so an op dispatches with no preheat.
    void hot_nozzle() {
        set_nozzle(250, 250);
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    helix::test::GcodeRecordingApi api_{client_, state()};
    helix_test::ScopedSharedResource<helix::ui::FilamentOpHeater> heater_{
        std::make_shared<helix::ui::FilamentOpHeater>(state())};
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;
    lv_obj_t* panel_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare load: the UI preheat window does not read as Idle",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();

    // HH does not heat for a load, so the sidebar preheats first. The extruder
    // reads 0C here, so the load waits on the preheat and HH is not called.
    sidebar_->handle_load_with_preheat(1);
    REQUIRE(shown_action() == AmsAction::HEATING);
    REQUIRE(api_.sent.empty());

    // HH has nothing to say while the nozzle heats: its stored action is the
    // Idle from before the tap. A gate_status delta still reaches the parser
    // and queues a sync, as does the watchdog.
    hh.feed_mmu({{"gate_status", {1, 1, 1, 1}}});
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::HEATING);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare unload: the gap before mmu.action moves does not read as Idle",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(api_.sent == std::vector<std::string>{"MMU_UNLOAD"});
    REQUIRE(shown_action() == AmsAction::HEATING);

    // MMU_UNLOAD is queued, HH has not started it yet.
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::HEATING);

    // HH's first real signal drives from here on, including its Idle at the end.
    hh.feed_mmu({{"action", "Unloading"}});
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::UNLOADING);
    CHECK(AmsState::instance().is_filament_operation_active());

    hh.feed_mmu({{"action", "Idle"}});
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "AFC unload: AFC's own dispatch action holds the window",
                 "[ams][optimistic_action][afc]") {
    auto& afc = build<helix::AfcOptimisticBackend>();
    afc.seat(1);
    AmsState::instance().sync_from_backend();

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(afc.sent == std::vector<std::string>{"TOOL_UNLOAD LANE=lane2"});

    // A delta in the window carries no current_state: AFC publishes a field only
    // when it changes, and its state has not moved yet.
    afc.feed_afc({{"current_toolchange", 0}});
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "CFS unload: CFS's own dispatch action holds the window",
                 "[ams][optimistic_action][cfs]") {
    auto& cfs = build<helix::printer::CfsOptimisticBackend>();
    helix::CfsTestAccess::set_loaded_state(cfs, true, 1);
    AmsState::instance().sync_from_backend();

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(cfs.dispatched.size() == 1);

    // The K2 streams extruder temperatures while it heats for the cut.
    helix::CfsTestAccess::handle_status(
        cfs, {{"params", nlohmann::json::array(
                             {{{"extruder", {{"temperature", 180.0}, {"target", 220.0}}}}, 0.0})}});
    let_watchdog_run();
    CHECK(shown_action() != AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: an expired hold lets the backend's Idle through",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(held());
    let_watchdog_run();
    REQUIRE(shown_action() == AmsAction::HEATING);

    // HH never reports the operation at all. The guardrail is what ends it.
    helix::AmsStateTestAccess::age_optimistic_action(AmsState::instance(), std::chrono::hours(1));
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::IDLE);
    CHECK_FALSE(held());
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "Happy Hare: a refused unload does not hold the action",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();
    // A stopped backend refuses the op after the sidebar has marked it busy.
    hh.stop_for_test();

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(api_.sent.empty());
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "Happy Hare: a refused load does not hold the action",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.stop_for_test();
    // Hot already, so the load dispatches at once and the refusal reaches
    // fail_started_operation().
    set_nozzle(300, 300);

    sidebar_->handle_load_with_preheat(1);
    REQUIRE(api_.sent.empty());
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: a gcode error from MMU_UNLOAD ends the held action",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();
    // Paused, disabled or bypass selected: HH answers with an error and never
    // moves mmu.action.
    api_.fail = {"MMU_UNLOAD"};

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(api_.sent == std::vector<std::string>{"MMU_UNLOAD"});
    let_watchdog_run();
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "Happy Hare: a failed pre-op G28 ends the held action",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.seat(1);
    hh.homed = false;
    AmsState::instance().sync_from_backend();
    api_.fail = {"G28"};

    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(api_.sent == std::vector<std::string>{"G28"});
    let_watchdog_run();
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: a preheat whose nozzle target is taken away abandons the load",
                 "[ams][optimistic_action][happy_hare]") {
    build_hh();
    sidebar_->handle_load_with_preheat(1);
    REQUIRE(held());

    // The target lands, then disappears: the user cleared it, or Klipper shut down.
    set_nozzle(100, 300);
    REQUIRE(held());
    set_nozzle(100, 0);
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);

    // A later heat-up for something else must not fire the abandoned load.
    set_nozzle(300, 300);
    CHECK(api_.sent.empty());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: tearing the sidebar down mid-preheat ends the held action",
                 "[ams][optimistic_action][happy_hare]") {
    build_hh();
    sidebar_->handle_load_with_preheat(1);
    REQUIRE(held());
    REQUIRE(shown_action() == AmsAction::HEATING);

    sidebar_->cleanup();
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "Happy Hare: a backend swap ends the held action",
                 "[ams][optimistic_action][happy_hare]") {
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();
    hot_nozzle();
    sidebar_->handle_unload(1);
    REQUIRE(held());

    AmsState::instance().clear_backends();
    CHECK_FALSE(held());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: a sidebar preheat cancels a cooldown left from the last op",
                 "[ams][optimistic_action][happy_hare]") {
    build_hh();
    // The previous load or unload armed a cooldown that would zero the nozzle
    // target mid-preheat.
    auto& cd = PostOpCooldownManager::instance();
    cd.init();
    cd.schedule();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(cd.has_pending_timer());

    sidebar_->handle_load_with_preheat(1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(cd.has_pending_timer());
    CHECK(held());
    cd.cancel();
    helix::ui::UpdateQueue::instance().drain();
}

// =============================================================================
// The sidebar on a cold nozzle: FilamentOpHeater runs the wait
// =============================================================================

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare cold load: the Heat step follows the heater, then the load runs",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    build_hh();
    set_nozzle(25, 0);

    sidebar_->handle_load_with_preheat(1);
    REQUIRE(heater_.get().pending());
    REQUIRE(shown_action() == AmsAction::HEATING);
    REQUIRE(api_.sent.empty());

    // The heater's progress reports reach the status line while it climbs.
    set_nozzle(150, 220);
    const std::string detail =
        lv_subject_get_string(AmsState::instance().get_ams_action_detail_subject());
    CHECK_FALSE(detail.empty());

    set_nozzle(220, 220);
    CHECK_FALSE(heater_.get().pending());
    REQUIRE(api_.sent.size() == 1);
    CHECK(api_.sent[0].find("MMU_LOAD") != std::string::npos);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare cold load: a second tap withdraws the preheat and unwinds the step",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    build_hh();
    set_nozzle(25, 0);

    sidebar_->handle_load_with_preheat(1);
    REQUIRE(held());

    sidebar_->handle_load_with_preheat(1);
    CHECK_FALSE(heater_.get().pending());
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);

    set_nozzle(220, 220);
    CHECK(api_.sent.empty());
}

TEST_CASE_METHOD(OptimisticSidebarFixture, "Happy Hare cold unload: heats first, then unloads",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();
    set_nozzle(25, 0);

    sidebar_->handle_unload(1);
    REQUIRE(heater_.get().pending());
    REQUIRE(api_.sent.empty());
    REQUIRE(shown_action() == AmsAction::HEATING);

    set_nozzle(220, 220);
    CHECK(api_.sent == std::vector<std::string>{"MMU_UNLOAD"});
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: the load still runs after the sidebar closes mid-preheat",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    build_hh();
    set_nozzle(25, 0);
    sidebar_->handle_load_with_preheat(1);
    REQUIRE(heater_.get().pending());

    sidebar_->cleanup();
    sidebar_.reset();

    set_nozzle(220, 220);
    REQUIRE(api_.sent.size() == 1);
    CHECK(api_.sent[0].find("MMU_LOAD") != std::string::npos);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: a load on another lane keeps the step the new load armed",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    build_hh();
    set_nozzle(25, 0);
    sidebar_->handle_load_with_preheat(1);
    REQUIRE(held());

    // The second load replaces the first; the first's Cancelled must not tear
    // down the step the second has just armed.
    sidebar_->handle_load_with_preheat(2);
    CHECK(heater_.get().pending());
    CHECK(held());
    CHECK(shown_action() == AmsAction::HEATING);

    set_nozzle(220, 220);
    REQUIRE(api_.sent.size() == 1);
    CHECK(api_.sent[0].find("GATE=2") != std::string::npos);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: a withdrawn preheat leaves no Heat step to redraw",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    build_hh();
    set_nozzle(25, 0);
    sidebar_->handle_load_with_preheat(1);
    REQUIRE(heater_.get().pending());
    const std::string heating =
        lv_subject_get_string(AmsState::instance().get_ams_action_detail_subject());
    REQUIRE_FALSE(heating.empty());

    sidebar_->handle_load_with_preheat(1); // re-tap withdraws it
    REQUIRE_FALSE(heater_.get().pending());
    AmsState::instance().set_action_detail("");

    // Reactivating the panel redraws a preheat only if one is waiting.
    sidebar_->sync_from_state();
    CHECK(std::string(lv_subject_get_string(
              AmsState::instance().get_ams_action_detail_subject())) != heating);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Happy Hare: a load refused while an unload heats leaves the unload's step",
                 "[ams][optimistic_action][happy_hare][preheat]") {
    // The context menu reaches handle_load_with_preheat() with no busy check of
    // its own, so a Load can arrive while this sidebar's Unload heats.
    auto& hh = build_hh();
    hh.seat(1);
    AmsState::instance().sync_from_backend();
    set_nozzle(25, 0);
    sidebar_->handle_unload(1);
    REQUIRE(heater_.get().pending());
    REQUIRE(held());

    sidebar_->handle_load_with_preheat(2);
    CHECK(heater_.get().pending());
    CHECK(held());
    CHECK(shown_action() == AmsAction::HEATING);

    set_nozzle(220, 220);
    CHECK(api_.sent == std::vector<std::string>{"MMU_UNLOAD"});
}

// =============================================================================
// Snapmaker U1: silent until its firmware starts, one toolhead per lane
// =============================================================================

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1 unload: the gap before channel_state moves keeps the unload steps",
                 "[ams][optimistic_action][snapmaker]") {
    auto& u1 = build_u1();
    u1.seat(1);
    AmsState::instance().sync_from_backend();
    hot_nozzle();
    api_.hold_acks = true;

    sidebar_->handle_unload(1);
    REQUIRE(api_.sent == std::vector<std::string>{"AUTO_FEEDING EXTRUDER=1 UNLOAD=1"});
    REQUIRE(shown_steps() == helix::StepOperationType::UNLOAD);

    // The firmware has accepted the command and not started it yet.
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::UNLOADING);
    CHECK(steps_shown());

    u1.feed(1, "unload_homing");
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::UNLOADING);
    CHECK(shown_steps() == helix::StepOperationType::UNLOAD);
    CHECK(steps_shown());

    u1.feed(1, "unload_heating");
    let_watchdog_run();
    CHECK(shown_steps() == helix::StepOperationType::UNLOAD);
    CHECK(lv_subject_get_int(AmsState::instance().get_ams_operation_phase_subject()) == 2);

    // The macro answers after its last frame; neither one reopens the other.
    u1.feed(1, "unload_finish");
    api_.ack_held();
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::IDLE);
    CHECK_FALSE(steps_shown());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: the AUTO_FEEDING answer arriving before the last frame keeps "
                 "the operation",
                 "[ams][optimistic_action][snapmaker]") {
    auto& u1 = build_u1();
    u1.seat(1);
    AmsState::instance().sync_from_backend();
    hot_nozzle();
    api_.hold_acks = true;

    sidebar_->handle_unload(1);
    u1.feed(1, "unload_doing");
    let_watchdog_run();
    REQUIRE(shown_action() == AmsAction::UNLOADING);

    // Status frames are batched, so the answer can overtake unload_finish.
    api_.ack_held();
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::UNLOADING);
    CHECK(steps_shown());

    u1.feed(1, "unload_finish");
    let_watchdog_run();
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1 load: the gap before channel_state moves keeps the load steps",
                 "[ams][optimistic_action][snapmaker]") {
    auto& u1 = build_u1();
    u1.feed(2, "preload_finish");
    AmsState::instance().sync_from_backend();
    hot_nozzle();
    api_.hold_acks = true;

    sidebar_->handle_load_with_preheat(2);
    REQUIRE(api_.sent == std::vector<std::string>{"AUTO_FEEDING EXTRUDER=2 LOAD=1"});

    let_watchdog_run();
    CHECK(shown_action() != AmsAction::IDLE);
    CHECK(steps_shown());

    u1.feed(2, "load_homing");
    let_watchdog_run();
    CHECK(shown_steps() == helix::StepOperationType::LOAD_FRESH);
    CHECK(steps_shown());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: an AUTO_FEEDING answered with no frame ends the held action",
                 "[ams][optimistic_action][snapmaker]") {
    // FEED_AUTO answers OK without moving channel_state when it declines a load
    // (already loaded, no runout sensor), and the AUTO_FEEDING macro answers OK
    // after a RESPOND TYPE=error. Its answer means the feed is over either way.
    auto& u1 = build_u1();
    u1.feed(2, "preload_finish");
    AmsState::instance().sync_from_backend();
    hot_nozzle();

    sidebar_->handle_load_with_preheat(2);
    REQUIRE(api_.sent == std::vector<std::string>{"AUTO_FEEDING EXTRUDER=2 LOAD=1"});
    let_watchdog_run();
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
    CHECK_FALSE(steps_shown());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: a load on a head out of automatic mode is refused before sending",
                 "[ams][optimistic_action][snapmaker]") {
    auto& u1 = build_u1();
    u1.feed(2, "preload_finish", /*auto_mode=*/false);
    AmsState::instance().sync_from_backend();
    hot_nozzle();

    sidebar_->handle_load_with_preheat(2);
    CHECK(api_.sent.empty());
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: a timed-out AUTO_FEEDING keeps the held action",
                 "[ams][optimistic_action][snapmaker]") {
    // The RPC gave up waiting; the feed may still be running.
    auto& u1 = build_u1();
    u1.seat(1);
    AmsState::instance().sync_from_backend();
    hot_nozzle();
    api_.time_out = {"AUTO_FEEDING EXTRUDER=1 UNLOAD=1"};

    sidebar_->handle_unload(1);
    REQUIRE(api_.sent == std::vector<std::string>{"AUTO_FEEDING EXTRUDER=1 UNLOAD=1"});
    let_watchdog_run();
    CHECK(held());
    CHECK(shown_action() == AmsAction::UNLOADING);
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: an unload started elsewhere keeps one unload step bar",
                 "[ams][optimistic_action][snapmaker]") {
    // The Filament panel, the batch picker, a macro or Fluidd: the sidebar did
    // not start it, and the head it empties is loaded.
    auto& u1 = build_u1();
    u1.seat(0);
    AmsState::instance().sync_from_backend();
    let_watchdog_run();
    REQUIRE_FALSE(steps_shown());

    u1.feed(0, "unload_homing");
    let_watchdog_run();
    CHECK(shown_steps() == helix::StepOperationType::UNLOAD);
    CHECK(steps_shown());

    // Mark the bar, so a rebuilt one cannot pass for it at a reused address.
    lv_obj_t* bar = stepper();
    REQUIRE(bar != nullptr);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_USER_1);

    for (const char* phase : {"unload_heating", "unload_doing"}) {
        u1.feed(0, phase);
        let_watchdog_run();
        INFO("phase=" << phase);
        CHECK(shown_steps() == helix::StepOperationType::UNLOAD);
        CHECK(steps_shown());
        REQUIRE(stepper() == bar);
        CHECK(lv_obj_has_flag(bar, LV_OBJ_FLAG_USER_1));
        CHECK(lv_obj_get_child_count(lv_obj_find_by_name(panel_, "progress_stepper_container")) ==
              1);
    }

    u1.feed(0, "unload_finish");
    let_watchdog_run();
    CHECK_FALSE(steps_shown());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: a refused AUTO_FEEDING ends the held action",
                 "[ams][optimistic_action][snapmaker]") {
    auto& u1 = build_u1();
    u1.seat(1);
    AmsState::instance().sync_from_backend();
    // The firmware answers with an error and its channel_state never moves.
    api_.fail = {"AUTO_FEEDING EXTRUDER=1 UNLOAD=1"};
    hot_nozzle();

    sidebar_->handle_unload(1);
    REQUIRE(api_.sent == std::vector<std::string>{"AUTO_FEEDING EXTRUDER=1 UNLOAD=1"});
    let_watchdog_run();
    CHECK_FALSE(held());
    CHECK(shown_action() == AmsAction::IDLE);
    CHECK_FALSE(steps_shown());
}

TEST_CASE_METHOD(OptimisticSidebarFixture,
                 "Snapmaker U1: a batch the firmware never reports holds only the dispatch budget",
                 "[ams][optimistic_action][snapmaker][batch]") {
    auto& u1 = build_u1();
    u1.seat(0);
    u1.seat(1);
    AmsState::instance().sync_from_backend();
    hot_nozzle();
    api_.hold_acks = true;

    std::vector<helix::ui::HeadPickRow> chosen = {{0, 0, true, "", true}, {1, 1, true, "", true}};
    sidebar_->run_picked(helix::ui::HeadPickOp::Unload, chosen);
    REQUIRE_FALSE(api_.sent.empty()); // the batch reached the firmware
    REQUIRE(held());

    // Accepted and then silent: past the dispatch hold, well inside the preheat one.
    helix::AmsStateTestAccess::age_optimistic_action(AmsState::instance(), std::chrono::minutes(2));
    let_watchdog_run();
    CHECK_FALSE(held());
}
