// SPDX-License-Identifier: GPL-3.0-or-later

// Where a print's stop reason reaches the user: the print-status card's paused
// and canceled overlays, the completion modal and the completion toast. Each
// case builds the production XML and reads the reason back off the widget the
// user sees. The last case follows a failed print's ending to the printer.

#include "ui_modal.h"
#include "ui_panel_print_status.h"
#include "ui_print_completion_modal.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/widget_in_tree.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "audio_settings_manager.h"
#include "core/update_queue.h"
#include "display_wake_request.h"
#include "moonraker_manager.h"
#include "print_completion.h"
#include "printer_state.h"

#include <algorithm>
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
    CHECK_FALSE(lv_obj_is_hidden(reason));
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
    CHECK(lv_obj_is_hidden(reason));

    state().print_state().set_print_stop_reason(kReason);
    settle();
    CHECK(label_text(reason) == kReason);
    CHECK_FALSE(lv_obj_is_hidden(reason));
}

TEST_CASE_METHOD(LVGLUITestFixture, "Completion modal: the stop reason shows under the filename",
                 "[print_completion][stop_reason]") {
    auto& audio = AudioSettingsManager::instance();
    const CompletionAlertMode saved_mode = audio.get_completion_alert_mode();
    audio.set_completion_alert_mode(CompletionAlertMode::ALERT);
    ObserverGuard completion = init_print_completion_observer();
    test::set_wire_state(state(), PrintJobState::PRINTING);
    UpdateQueue::instance().drain();
    state().print_state().set_print_stop_reason(kReason);
    test::set_wire_state(state(), PrintJobState::CANCELLED);
    UpdateQueue::instance().drain();

    REQUIRE(ModalStack::instance().top_component_name() == "print_completion_modal");
    lv_obj_t* reason =
        lv_obj_find_by_name(ModalStack::instance().top_dialog(), "stop_reason_label");
    REQUIRE(reason != nullptr);
    CHECK(label_text(reason) == kReason);
    CHECK_FALSE(lv_obj_is_hidden(reason));

    state().print_state().set_print_stop_reason("");
    UpdateQueue::instance().drain();
    CHECK(lv_obj_is_hidden(reason));

    // The next job makes this one's result stale.
    test::set_wire_state(state(), PrintJobState::PRINTING);
    UpdateQueue::instance().drain();
    process_lvgl(400);
    CHECK(ModalStack::instance().top_component_name().empty());
    completion.reset();
    audio.set_completion_alert_mode(saved_mode);
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
    // A user already watching print status sees the ending there, not in a toast.
    const auto ui_query = set_print_status_shown_query([]() { return true; });
    test::set_wire_state(state(), PrintJobState::PRINTING);
    UpdateQueue::instance().drain();
    test::set_wire_state(state(), PrintJobState::CANCELLED);
    UpdateQueue::instance().drain();
    set_print_status_shown_query(ui_query);
    CHECK(toasts.size() == 1);

    completion.reset();
    helix::ui::set_test_toast_hook(nullptr);
    audio.set_completion_alert_mode(saved_mode);
    state().print_state().set_print_stop_reason("");
    test::set_wire_state(state(), PrintJobState::STANDBY);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Completion alert: ALERT mode wakes the display and opens the modal",
                 "[print_completion][stop_reason]") {
    auto& audio = AudioSettingsManager::instance();
    const CompletionAlertMode saved_mode = audio.get_completion_alert_mode();
    audio.set_completion_alert_mode(CompletionAlertMode::ALERT);
    int wakes = 0;
    set_display_wake_handler([&wakes] { ++wakes; });
    ObserverGuard completion = init_print_completion_observer();

    test::set_wire_state(state(), PrintJobState::PRINTING);
    UpdateQueue::instance().drain();
    test::set_wire_state(state(), PrintJobState::COMPLETE);
    UpdateQueue::instance().drain();

    CHECK(wakes == 1);
    lv_obj_t* dialog = ModalStack::instance().dialog_for_component("print_completion_modal");
    CHECK(dialog != nullptr);
    for (const char* name : {"print_completion_title", "print_completion_filename"}) {
        CAPTURE(name);
        lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
        REQUIRE(subject != nullptr);
        CHECK_FALSE(std::string(lv_subject_get_string(subject)).empty());
    }

    if (dialog) {
        helix::ui::modal_hide(dialog);
    }
    completion.reset();
    set_display_wake_handler(nullptr);
    audio.set_completion_alert_mode(saved_mode);
    test::set_wire_state(state(), PrintJobState::STANDBY);
    UpdateQueue::instance().drain();
    process_lvgl(10);
}

namespace {

/// The app's MoonrakerManager on a fake client, installed as the global the
/// completion observer sends through, with the printer type and alert mode put
/// back however the case ends.
struct HeaterOffScope {
    PrinterState& state;
    MoonrakerManager moonraker;
    const std::vector<std::string>* sent = nullptr;
    std::string saved_type;
    CompletionAlertMode saved_mode;

    explicit HeaterOffScope(PrinterState& st)
        : state(st), saved_type(st.profile_state().printer_type()),
          saved_mode(AudioSettingsManager::instance().get_completion_alert_mode()) {
        auto fake = std::make_unique<helix::test::FakeMoonrakerClient>();
        sent = &fake->gcode_scripts;
        MoonrakerManagerTestAccess::install_client(moonraker, std::move(fake));
        set_moonraker_manager(&moonraker);
        AudioSettingsManager::instance().set_completion_alert_mode(
            CompletionAlertMode::NOTIFICATION);
        helix::ui::set_test_toast_hook([](ToastSeverity, const std::string&, uint32_t) {});
    }
    ~HeaterOffScope() {
        set_moonraker_manager(nullptr);
        state.profile_state().set_printer_type(saved_type, true, false, false);
        helix::ui::set_test_toast_hook(nullptr);
        AudioSettingsManager::instance().set_completion_alert_mode(saved_mode);
        test::set_wire_state(state, PrintJobState::STANDBY);
        UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Completion: a print error turns the heaters off unless the firmware does",
                 "[print_completion][stop_reason]") {
    HeaterOffScope scope(state());
    ObserverGuard completion = init_print_completion_observer();

    const bool firmware_owns = GENERATE(false, true);
    CAPTURE(firmware_owns);
    REQUIRE(state().profile_state().set_printer_type(
        firmware_owns ? "Creality K2 Plus (Jacobean firmware)" : "Creality K2 Plus", true, false,
        false));
    REQUIRE(state().profile_state().firmware_owns_error_heaters() == firmware_owns);
    test::set_wire_state(state(), PrintJobState::PRINTING);
    UpdateQueue::instance().drain();
    test::set_wire_state(state(), PrintJobState::ERROR);
    UpdateQueue::instance().drain();

    CHECK(std::count(scope.sent->begin(), scope.sent->end(), std::string("TURN_OFF_HEATERS")) ==
          (firmware_owns ? 0 : 1));
}
