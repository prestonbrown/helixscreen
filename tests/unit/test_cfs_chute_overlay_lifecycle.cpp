// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "moonraker_error.h"
#include "printer_state.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_cfs_chute_calibration_overlay;

namespace {

/// Records what the overlay asks of the backend and answers nothing until told.
class RecordingBackend : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    AmsError calibrate_cutter(std::function<void(bool, const std::string&)>) override {
        return AmsErrorHelper::success();
    }
    AmsError start_chute_calibration(std::function<void()> on_ready, ChuteStepFailed on_failed,
                                     std::shared_ptr<std::atomic<bool>> cancel) override {
        ++starts;
        ready = std::move(on_ready);
        start_failed = std::move(on_failed);
        token = std::move(cancel);
        return AmsErrorHelper::success();
    }
    AmsError jog_chute_y(float) override {
        return AmsErrorHelper::success();
    }
    AmsError save_chute_position(std::function<void()>, ChuteSaveFailed on_failed) override {
        save_failed = std::move(on_failed);
        return AmsErrorHelper::success();
    }
    AmsError exit_chute_calibration() override {
        ++exits;
        return AmsErrorHelper::success();
    }
    bool last_chute_saved_position(double&, double&) const override {
        return false;
    }

    int starts = 0;
    int exits = 0;
    std::function<void()> ready;
    ChuteStepFailed start_failed;
    ChuteSaveFailed save_failed;
    std::shared_ptr<std::atomic<bool>> token;
};

constexpr int kIdle = 0;
constexpr int kHoming = 1;
constexpr int kAdjusting = 2;
constexpr int kDone = 4;

class ChuteOverlayFixture : public LVGLUITestFixture {
  public:
    RecordingBackend* backend = nullptr;

    ChuteOverlayFixture() {
        for (auto& p : panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(panels_.data());
        auto& ams = AmsState::instance();
        ams.deinit_subjects();
        auto owned = std::make_unique<RecordingBackend>(4);
        backend = owned.get();
        backend->set_operation_delay(0);
        REQUIRE(backend->start().success());
        ams.set_backend(std::move(owned));
        ams.init_subjects(true);

        REQUIRE(get_cfs_chute_calibration_overlay().show(lv_screen_active()));
        settle();
        get_cfs_chute_calibration_overlay().on_activate();
    }

    ~ChuteOverlayFixture() override {
        helix::test::set_wire_state(get_printer_state(), PrintJobState::STANDBY);
        get_cfs_chute_calibration_overlay().on_deactivate(DeactivateReason::Shutdown);
        NavigationManager::instance().go_back();
        settle();
        AmsState::instance().clear_backends();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    void tap_start() {
        lv_obj_t* start =
            lv_obj_find_by_name(get_cfs_chute_calibration_overlay().get_root(), "btn_chute_start");
        REQUIRE(start);
        lv_obj_send_event(start, LV_EVENT_CLICKED, nullptr);
    }

    void reach_adjusting() {
        tap_start();
        REQUIRE(backend->ready);
        backend->ready();
        REQUIRE(state() == kAdjusting);
    }

    static int state() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "cfs_chute_state"));
    }

  private:
    std::array<lv_obj_t*, kUiPanelCount> panels_{};
};

} // namespace

TEST_CASE_METHOD(ChuteOverlayFixture, "Chute overlay sleep sends nothing and loses nothing",
                 "[ams][cfs][chute_overlay]") {
    auto& overlay = get_cfs_chute_calibration_overlay();

    SECTION("while adjusting: no move, and the session survives the wake") {
        reach_adjusting();
        overlay.on_deactivate(DeactivateReason::Suspended);
        CHECK(backend->exits == 0);
        overlay.on_activate();
        CHECK(state() == kAdjusting);
    }

    SECTION("while homing: the running chain is not cancelled") {
        tap_start();
        const auto token = backend->token;
        REQUIRE(token);
        overlay.on_deactivate(DeactivateReason::Suspended);
        CHECK_FALSE(token->load());
        overlay.on_activate();
        CHECK(state() == kHoming);
        CHECK_FALSE(token->load());
    }

    SECTION("leaving after a wake still re-parks and the next open starts clean") {
        reach_adjusting();
        overlay.on_deactivate(DeactivateReason::Suspended);
        overlay.on_activate();
        overlay.on_deactivate(DeactivateReason::NavigateAway);
        CHECK(backend->exits == 1);
        overlay.on_activate();
        CHECK(state() == kIdle);
    }
}

TEST_CASE_METHOD(ChuteOverlayFixture, "Leaving the chute overlay ends its running chain for good",
                 "[ams][cfs][chute_overlay]") {
    auto& overlay = get_cfs_chute_calibration_overlay();
    tap_start();
    const auto first = backend->token;
    REQUIRE(first);

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    CHECK(first->load());
    // Reopening must not revive the chain still waiting on its home.
    overlay.on_activate();
    CHECK(first->load());

    tap_start();
    REQUIRE(backend->token != first);
    CHECK_FALSE(backend->token->load());
    CHECK(first->load());
}

TEST_CASE_METHOD(ChuteOverlayFixture, "A print starting mid-adjust ends the chute session",
                 "[ams][cfs][chute_overlay]") {
    reach_adjusting();
    helix::test::set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(state() == kIdle);

    get_cfs_chute_calibration_overlay().on_deactivate(DeactivateReason::NavigateAway);
    CHECK(backend->exits == 0);
}

namespace {

MoonrakerError chute_error(MoonrakerErrorType type, std::string message) {
    MoonrakerError err;
    err.type = type;
    err.message = std::move(message);
    return err;
}

struct ToastLog {
    std::vector<std::string> shown;
    ToastLog() {
        helix::ui::set_test_notification_error_hook(
            [this](const std::string& m) { shown.push_back(m); });
    }
    ~ToastLog() {
        helix::ui::set_test_notification_error_hook(nullptr);
    }
};

} // namespace

TEST_CASE_METHOD(ChuteOverlayFixture, "Chute overlay reports each failed step once (#2007)",
                 "[ams][cfs][chute_overlay][2007]") {
    ToastLog toasts;
    const std::string envelope = R"({"code":"key165","msg":"Box did not answer","values":[]})";

    SECTION("a step Klipper raised is left to the gcode error router") {
        tap_start();
        REQUIRE(backend->start_failed);
        backend->start_failed(chute_error(MoonrakerErrorType::JSON_RPC_ERROR, envelope));
        settle();
        CHECK(toasts.shown.empty());
        CHECK(state() == kIdle);
    }

    SECTION("a step Klipper never saw is toasted, with the firmware message decoded") {
        tap_start();
        REQUIRE(backend->start_failed);
        backend->start_failed(chute_error(MoonrakerErrorType::TIMEOUT, envelope));
        settle();
        REQUIRE(toasts.shown.size() == 1);
        CHECK(toasts.shown[0].find("Box did not answer") != std::string::npos);
        CHECK(toasts.shown[0].find("key165") == std::string::npos);
        CHECK(state() == kIdle);
    }

    SECTION("a Klippy disconnect is Moonraker's answer, with no `!!` copy, so it is toasted") {
        tap_start();
        REQUIRE(backend->start_failed);
        MoonrakerError gone =
            chute_error(MoonrakerErrorType::JSON_RPC_ERROR, "Klippy Disconnected");
        gone.code = 503;
        backend->start_failed(gone);
        settle();
        CHECK(toasts.shown.size() == 1);
        CHECK(state() == kIdle);
    }

    SECTION("a park that times out after a good save names the park, not the save") {
        reach_adjusting();
        lv_obj_t* save =
            lv_obj_find_by_name(get_cfs_chute_calibration_overlay().get_root(), "btn_chute_save");
        REQUIRE(save);
        lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr);
        REQUIRE(backend->save_failed);
        backend->save_failed(chute_error(MoonrakerErrorType::TIMEOUT, "Request timed out"),
                             /*position_saved=*/true);
        settle();
        REQUIRE(toasts.shown.size() == 1);
        CHECK(toasts.shown[0].find("did not return to park") != std::string::npos);
        CHECK(state() == kDone);
    }

    SECTION("a failed park after a good save ends on the done screen with a note") {
        reach_adjusting();
        lv_obj_t* save =
            lv_obj_find_by_name(get_cfs_chute_calibration_overlay().get_root(), "btn_chute_save");
        REQUIRE(save);
        lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr);
        REQUIRE(backend->save_failed);
        backend->save_failed(chute_error(MoonrakerErrorType::JSON_RPC_ERROR, envelope),
                             /*position_saved=*/true);
        settle();
        CHECK(toasts.shown.empty());
        REQUIRE(state() == kDone);
        const char* text =
            lv_subject_get_string(lv_xml_get_subject(nullptr, "cfs_chute_saved_pos"));
        REQUIRE(text);
        CHECK(std::string(text).find("may still be at the chute") != std::string::npos);
    }
}

#endif // HELIX_HAS_CFS
