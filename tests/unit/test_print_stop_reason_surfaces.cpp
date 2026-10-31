// SPDX-License-Identifier: GPL-3.0-or-later

// Where a print's stop reason reaches the user: the print-status card's paused
// and canceled overlays, the completion modal and the completion toast. Each
// case builds the production XML and reads the reason back off the widget the
// user sees.

#include "ui_modal.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/widget_in_tree.h"
#include "../ui_test_utils.h"
#include "audio_settings_manager.h"
#include "print_completion.h"
#include "printer_state.h"

#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::UpdateQueue;

namespace {

constexpr const char* kReason = "Quad gantry level failed";
constexpr const char* kLongReason =
    "lane3 filament failed to trigger pre extruder gear toolhead sensor, CHECK FILAMENT PATH, "
    "then set the lane loaded with SET_LANE_LOADED and move the filament with LANE_MOVE";

struct StopReasonPanelFixture : public LVGLUITestFixture {
    StopReasonPanelFixture() {
        panel_ = std::make_unique<PrintStatusPanel>(state(), nullptr);
        panel_->init_subjects();
        root_ = panel_->create(test_screen());
        lv_obj_set_size(root_, 800, 480);
        settle();
    }

    ~StopReasonPanelFixture() override {
        state().print_state().set_print_stop_reason("");
        test::set_wire_state(state(), PrintJobState::STANDBY);
        if (root_ && helix::test::widget_in_tree(root_)) {
            lv_obj_delete(root_);
        }
        root_ = nullptr;
        UpdateQueue::instance().drain();
        panel_.reset();
        UpdateQueue::instance().drain();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(10);
        lv_obj_update_layout(root_);
    }

    lv_obj_t* find(const char* name) const {
        return lv_obj_find_by_name(root_, name);
    }

    std::unique_ptr<PrintStatusPanel> panel_;
    lv_obj_t* root_ = nullptr;
};

std::string label_text(lv_obj_t* label) {
    return label ? std::string(lv_label_get_text(label)) : std::string("<missing>");
}

} // namespace

TEST_CASE_METHOD(StopReasonPanelFixture, "Paused overlay: the stop reason says why it paused",
                 "[print_status][stop_reason]") {
    test::set_wire_state(state(), PrintJobState::PAUSED);
    state().print_state().set_print_stop_reason(kReason);
    settle();

    lv_obj_t* reason = find("paused_badge_reason");
    REQUIRE(reason != nullptr);
    CHECK(label_text(reason) == kReason);
    CHECK_FALSE(lv_obj_has_flag(reason, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(StopReasonPanelFixture,
                 "Paused overlay: a long reason wraps inside the card under the badge",
                 "[print_status][stop_reason]") {
    test::set_wire_state(state(), PrintJobState::PAUSED);
    state().print_state().set_print_stop_reason(kLongReason);
    settle();

    lv_obj_t* overlay = find("print_paused_overlay");
    lv_obj_t* badge = find("paused_badge");
    lv_obj_t* reason = find("paused_badge_reason");
    REQUIRE(overlay != nullptr);
    REQUIRE(badge != nullptr);
    REQUIRE(reason != nullptr);
    REQUIRE(label_text(reason) == kLongReason);

    CHECK(lv_obj_get_parent(reason) == overlay);
    CHECK(lv_obj_get_width(badge) <= lv_obj_get_content_width(overlay));
    CHECK(lv_obj_get_width(reason) <= lv_obj_get_content_width(overlay));
}

TEST_CASE_METHOD(StopReasonPanelFixture, "Canceled overlay: the stop reason shows under the badge",
                 "[print_status][stop_reason]") {
    lv_obj_t* reason = find("cancel_reason_label");
    REQUIRE(reason != nullptr);
    CHECK(lv_obj_has_flag(reason, LV_OBJ_FLAG_HIDDEN));

    state().print_state().set_print_stop_reason(kReason);
    settle();
    CHECK(label_text(reason) == kReason);
    CHECK_FALSE(lv_obj_has_flag(reason, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Completion modal: the stop reason shows under the filename",
                 "[print_completion][stop_reason]") {
    ObserverGuard completion = init_print_completion_observer();
    state().print_state().set_print_stop_reason(kReason);
    UpdateQueue::instance().drain();

    lv_obj_t* dialog = helix::ui::modal_show("print_completion_modal");
    REQUIRE(dialog != nullptr);
    lv_obj_t* reason = lv_obj_find_by_name(dialog, "stop_reason_label");
    REQUIRE(reason != nullptr);
    CHECK(label_text(reason) == kReason);
    CHECK_FALSE(lv_obj_has_flag(reason, LV_OBJ_FLAG_HIDDEN));

    state().print_state().set_print_stop_reason("");
    UpdateQueue::instance().drain();
    CHECK(lv_obj_has_flag(reason, LV_OBJ_FLAG_HIDDEN));

    helix::ui::modal_hide(dialog);
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Completion toast: a canceled print says why",
                 "[print_completion][stop_reason]") {
    auto& audio = AudioSettingsManager::instance();
    const CompletionAlertMode saved_mode = audio.get_completion_alert_mode();
    audio.set_completion_alert_mode(CompletionAlertMode::NOTIFICATION);
    std::vector<std::string> toasts;
    helix::ui::set_test_toast_hook([&toasts](ToastSeverity, const std::string& message, uint32_t) {
        toasts.push_back(message);
    });
    ObserverGuard completion = init_print_completion_observer();

    test::set_wire_state(state(), PrintJobState::PRINTING);
    UpdateQueue::instance().drain();
    state().print_state().set_print_stop_reason(kReason);
    test::set_wire_state(state(), PrintJobState::CANCELLED);
    UpdateQueue::instance().drain();

    REQUIRE(toasts.size() == 1);
    CHECK(toasts[0] == std::string("Print canceled: Unknown - ") + kReason);

    completion.reset();
    helix::ui::set_test_toast_hook(nullptr);
    audio.set_completion_alert_mode(saved_mode);
    state().print_state().set_print_stop_reason("");
    test::set_wire_state(state(), PrintJobState::STANDBY);
    UpdateQueue::instance().drain();
}
