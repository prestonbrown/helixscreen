// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_sidebar_batch_steps.cpp
 * @brief The step bar a head-picker batch shows on a changer that mounts to feed.
 *
 * The batch path names its operation to the bar. Left to read the first frame,
 * the bar takes an unload of a loaded head for a tool swap and an unload of an
 * empty one for a load.
 */

#include "ui_ams_sidebar.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/toolchanger_test_access.h"
#include "../test_helpers/toolchanger_test_helper.h"
#include "../test_helpers/widget_in_tree.h"
#include "ams_state.h"
#include "ams_step_operation.h"
#include "filament_action_set.h"
#include "mount_feed_sequence.h"
#include "printer_discovery.h"
#include "static_subject_registry.h"
#include "test_helpers/ams_sidebar_xml.h"
#include "toolchanger_addon.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::AmsState;
using helix::StepOperationType;
using helix::ui::HeadPickOp;
using helix::ui::HeadPickRow;

namespace {

/// A mount-to-feed changer whose stage acks the test hands out.
class FeedChanger : public helix::test::ToolChangerHelper {
  public:
    explicit FeedChanger(int tools) : ToolChangerHelper(tools) {
        set_mounts_to_feed(true);
    }

    helix::AmsError execute_gcode(const std::string& gcode,
                                  helix::SharedCallback<void()> on_complete,
                                  helix::SharedCallback<void(const MoonrakerError&)> /*on_error*/,
                                  bool /*silent*/) override {
        sent_.push_back(gcode);
        acks_.push_back(std::move(on_complete));
        return helix::AmsErrorHelper::success();
    }

    helix::AmsError cancel() override {
        ++cancel_calls_;
        return ToolChangerHelper::cancel();
    }

    [[nodiscard]] bool can_cancel_operation() const override {
        return cancellable_.value_or(ToolChangerHelper::can_cancel_operation());
    }

    /// Ack the newest stage and let the deferred handlers run.
    void ack() {
        REQUIRE_FALSE(acks_.empty());
        auto cb = acks_.back();
        cb();
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    /// The operator answers the run's Insert prompt.
    void continue_prompt() {
        helix::ToolChangerTestAccess::feed_operator_reply(*this,
                                                          helix::mount_feed::Reply::Continue);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
    }

    int cancel_calls_ = 0;
    std::optional<bool> cancellable_;

  private:
    std::vector<helix::SharedCallback<void()>> acks_;
};

struct BatchStepsFixture : public XMLTestFixture {
    explicit BatchStepsFixture(int tools = 6) {
        auto owned = std::make_unique<FeedChanger>(tools);
        changer_ = owned.get();
        changer_->feed_status("ready", 0); // tool 1 on the carriage, loaded
        AmsState::instance().set_backend(std::move(owned));
        AmsState::instance().init_subjects(true);
        AmsState::instance().sync_from_backend();

        helix::test::register_ams_sidebar_xml();
        open();
    }

    /// Build the sidebar the way the AMS panel does each time it opens.
    void open() {
        panel_ = lv_obj_create(test_screen());
        const char* attrs[] = {"name", "ams_operation_sidebar", nullptr};
        REQUIRE(lv_xml_create(panel_, "ams_sidebar", attrs) != nullptr);
        sidebar_ = std::make_unique<helix::ui::AmsOperationSidebar>(state());
        REQUIRE(sidebar_->setup(panel_));
        sidebar_->init_observers();
        // The frames the setup fed land before the user taps anything.
        helix::ui::UpdateQueue::instance().drain();
    }

    ~BatchStepsFixture() override {
        close();
        AmsState::instance().set_backend(nullptr);
    }

    void close() {
        sidebar_.reset();
        if (panel_ && helix::test::widget_in_tree(panel_)) {
            lv_obj_delete(panel_);
        }
        panel_ = nullptr;
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Close the AMS panel and open it again: the new sidebar started nothing.
    void reopen() {
        close();
        open();
    }

    [[nodiscard]] lv_obj_t* cancel_button() const {
        lv_obj_t* btn = lv_obj_find_by_name(panel_, "btn_cancel_operation");
        REQUIRE(btn != nullptr);
        return btn;
    }

    [[nodiscard]] bool cancel_shown() const {
        return !lv_obj_has_flag(cancel_button(), LV_OBJ_FLAG_HIDDEN);
    }

    void pick(HeadPickOp op, const std::vector<int>& slots) {
        std::vector<HeadPickRow> rows;
        for (int slot : slots) {
            rows.push_back({slot, slot, true, "", true});
        }
        sidebar_->run_picked(op, rows);
        helix::ui::UpdateQueue::instance().drain();
        helix::ui::UpdateQueue::instance().drain();
        REQUIRE_FALSE(changer_->sent_.empty()); // the batch reached the backend
        REQUIRE(helix::test::widget_in_tree(panel_));
    }

    /// Every row label the step bar on screen shows, in order.
    [[nodiscard]] std::vector<std::string> labels() const {
        lv_obj_t* container = lv_obj_find_by_name(panel_, "progress_stepper_container");
        REQUIRE(container != nullptr);
        REQUIRE(lv_obj_get_child_count(container) > 0);
        lv_obj_t* bar = lv_obj_get_child(container, 0);
        std::vector<std::string> out;
        // Step rows carry an indicator and a label; the connectors carry nothing.
        for (uint32_t i = 0; i < lv_obj_get_child_count(bar); ++i) {
            lv_obj_t* item = lv_obj_get_child(bar, static_cast<int32_t>(i));
            if (lv_obj_get_child_count(item) >= 2) {
                out.emplace_back(lv_label_get_text(lv_obj_get_child(item, 1)));
            }
        }
        return out;
    }

    FeedChanger* changer_ = nullptr;
    std::unique_ptr<helix::ui::AmsOperationSidebar> sidebar_;
    lv_obj_t* panel_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(BatchStepsFixture,
                 "A batch unload of a loaded head shows the unload rows of every tool",
                 "[ams][sidebar][mount_feed][batch]") {
    pick(HeadPickOp::Unload, {0, 1, 2, 3, 4, 5});

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::UNLOAD);
    const auto shown = labels();
    REQUIRE(shown.size() == helix::mount_feed::bar_rows(/*load=*/false, 6).size());
    CHECK(shown.front() == "Tool 1 (1/6): Mount tool");
    CHECK(shown[2] == "Tool 1 (1/6): Retract filament");
    CHECK(shown.back() == "Tool 6 (6/6): Restore tool");
}

TEST_CASE_METHOD(BatchStepsFixture, "A batch load shows the load rows of every tool",
                 "[ams][sidebar][mount_feed][batch]") {
    pick(HeadPickOp::Load, {1, 2, 3});

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::LOAD_FRESH);
    const auto shown = labels();
    REQUIRE(shown.size() == helix::mount_feed::bar_rows(/*load=*/true, 3).size());
    CHECK(shown.front() == "Tool 2 (1/3): Insert filament");
    CHECK(shown.back() == "Tool 4 (3/3): Restore tool");
}

TEST_CASE_METHOD(BatchStepsFixture, "The live temperature follows the heat row under way",
                 "[ams][sidebar][mount_feed][batch]") {
    pick(HeadPickOp::Unload, {0, 1, 2});
    // Tool 1 is already on the carriage: the run opens on its heat row.
    auto shown = labels();
    REQUIRE(shown[1].rfind("Tool 1 (1/3): Heat nozzle ", 0) == 0);
    CHECK(shown[1].find("°C") != std::string::npos);
    CHECK(shown[4] == "Tool 2 (2/3): Heat nozzle");

    changer_->ack(); // tool 1 heated
    changer_->ack(); // tool 1 retracted
    changer_->ack(); // tool 2 mounted
    shown = labels();
    CHECK(shown[1] == "Tool 1 (1/3): Heat nozzle");
    REQUIRE(shown[4].rfind("Tool 2 (2/3): Heat nozzle ", 0) == 0);
    CHECK(shown[4].find("°C") != std::string::npos);
}

TEST_CASE_METHOD(BatchStepsFixture,
                 "A load batch reopened while it heats keeps the load rows, not a swap",
                 "[ams][sidebar][mount_feed][batch][reopen]") {
    pick(HeadPickOp::Load, {1, 2, 3});
    changer_->ack();             // the Insert prompt is up
    changer_->continue_prompt(); // filament inserted
    changer_->ack();             // tool 2 mounted: tool 2 heats
    REQUIRE(AmsState::instance().system_info()->current_slot >= 0);

    reopen();

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::LOAD_FRESH);
    const auto shown = labels();
    REQUIRE(shown.size() == helix::mount_feed::bar_rows(/*load=*/true, 3).size());
    CHECK(shown.front() == "Tool 2 (1/3): Insert filament");
}

TEST_CASE_METHOD(BatchStepsFixture,
                 "An unload batch reopened before it retracts keeps the unload rows",
                 "[ams][sidebar][mount_feed][batch][reopen]") {
    pick(HeadPickOp::Unload, {0, 1, 2});

    reopen();

    CHECK(AmsState::instance().get_active_step_operation() == StepOperationType::UNLOAD);
    const auto shown = labels();
    REQUIRE(shown.size() == helix::mount_feed::bar_rows(/*load=*/false, 3).size());
    CHECK(shown.front() == "Tool 1 (1/3): Mount tool");
    CHECK(shown[2] == "Tool 1 (1/3): Retract filament");
}

TEST_CASE_METHOD(BatchStepsFixture, "The heat row reads the heater of the tool it heats",
                 "[ams][sidebar][mount_feed][batch]") {
    auto& temps = state().temperature_state();
    temps.init_extruders({"extruder", "extruder1", "extruder2", "extruder3"});
    temps.set_active_extruder("extruder");
    lv_subject_set_int(temps.get_active_extruder_temp_subject(), 2000);
    lv_subject_set_int(temps.get_active_extruder_target_subject(), 0);
    REQUIRE(temps.get_extruder_temp_subject("extruder2") != nullptr);
    lv_subject_set_int(temps.get_extruder_temp_subject("extruder2"), 1230);
    lv_subject_set_int(temps.get_extruder_target_subject("extruder2"), 2400);

    pick(HeadPickOp::Unload, {2, 3});
    changer_->ack(); // Tool 3 mounted: Tool 3 heats
    auto shown = labels();
    REQUIRE(shown[1].rfind("Tool 3 (1/2): Heat nozzle ", 0) == 0);
    CHECK(shown[1].find("123") != std::string::npos);
    CHECK(shown[1].find("240") != std::string::npos);
    CHECK(shown[1].find("200") == std::string::npos);

    // The reading follows that heater, not the active one.
    lv_subject_set_int(temps.get_extruder_temp_subject("extruder2"), 1870);
    helix::ui::UpdateQueue::instance().drain();
    shown = labels();
    CHECK(shown[1].find("187") != std::string::npos);
}

TEST_CASE_METHOD(BatchStepsFixture,
                 "The heat row follows its heater after the extruder set is rebuilt mid-run",
                 "[ams][sidebar][mount_feed][batch]") {
    auto& temps = state().temperature_state();
    temps.init_extruders({"extruder", "extruder1", "extruder2", "extruder3"});
    lv_subject_set_int(temps.get_extruder_temp_subject("extruder2"), 1230);
    lv_subject_set_int(temps.get_extruder_target_subject("extruder2"), 2400);
    pick(HeadPickOp::Unload, {2, 3});
    changer_->ack(); // Tool 3 mounted: Tool 3 heats
    REQUIRE(labels()[1].find("123") != std::string::npos);

    // A Klipper restart's rediscovery re-creates extruder2's subjects.
    temps.init_extruders({"extruder", "extruder1", "extruder3"});
    temps.init_extruders({"extruder", "extruder1", "extruder2", "extruder3"});
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(temps.get_extruder_temp_subject("extruder2"), 1990);
    lv_subject_set_int(temps.get_extruder_target_subject("extruder2"), 2400);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(labels()[1].find("199") != std::string::npos);
}

TEST_CASE_METHOD(BatchStepsFixture,
                 "The sidebar offers Cancel while a cancellable run is under way",
                 "[ams][sidebar][mount_feed][batch][cancel]") {
    CHECK_FALSE(cancel_shown()); // idle

    pick(HeadPickOp::Unload, {0, 1, 2});
    REQUIRE(cancel_shown());

    lv_obj_send_event(cancel_button(), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(changer_->cancel_calls_ == 1);
}

TEST_CASE_METHOD(BatchStepsFixture,
                 "The sidebar withdraws Cancel once the backend holds one for a swap",
                 "[ams][sidebar][mount_feed][batch][cancel]") {
    // Dock sensors: a Cancel during a swap waits for it to settle.
    helix::PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"medusahc", "pin_watch io", "extruder", "extruder1",
                                            "extruder2", "extruder3", "extruder4", "extruder5"}));
    changer_->set_tool_sensor(helix::toolchanger_addon::resolve_tool_sensor(hw));
    changer_->feed(nlohmann::json{{"pin_watch io", {{"current_tool", 0}}}});
    helix::ui::UpdateQueue::instance().drain();

    pick(HeadPickOp::Load, {2});
    changer_->ack();             // the prompt is up
    changer_->continue_prompt(); // the Mount's swap is under way
    REQUIRE(cancel_shown());

    lv_obj_send_event(cancel_button(), LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(changer_->cancel_calls_ == 1);
    REQUIRE(changer_->get_system_info().is_busy()); // still the swap
    CHECK_FALSE(cancel_shown());
    // A second Cancel while the first is held is no failure.
    CHECK(changer_->cancel().success());
}

TEST_CASE_METHOD(BatchStepsFixture, "A printer switch takes the sidebar Cancel down",
                 "[ams][sidebar][mount_feed][batch][cancel]") {
    pick(HeadPickOp::Unload, {0, 1, 2});
    lv_subject_t* shown = lv_xml_get_subject(nullptr, "ams_sidebar_cancel_shown");
    REQUIRE(shown != nullptr);
    REQUIRE(lv_subject_get_int(shown) == 1);
    // The next printer's sidebar starts from the subjects' reset values.
    REQUIRE(StaticSubjectRegistry::instance().deinit_one(
        "AmsSidebarButtonGatingReset", StaticSubjectRegistry::Scope::PrinterSwitch));
    CHECK(lv_subject_get_int(shown) == 0);
}

TEST_CASE_METHOD(BatchStepsFixture, "The sidebar withholds Cancel when the backend cannot cancel",
                 "[ams][sidebar][mount_feed][batch][cancel]") {
    changer_->cancellable_ = false;
    pick(HeadPickOp::Unload, {0, 1, 2});
    CHECK_FALSE(cancel_shown());
}
