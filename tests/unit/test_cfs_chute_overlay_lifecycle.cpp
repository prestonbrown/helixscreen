// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_nav_manager.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_cfs_calibration.h"
#include "ams_state.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <array>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::get_cfs_chute_calibration_overlay;

namespace {

/// Records what the overlay asks of the backend and answers nothing until told.
class RecordingBackend : public AmsBackendMock, public ICfsCalibration {
  public:
    using AmsBackendMock::AmsBackendMock;
    void* capability(helix::TypeTag tag) override {
        return tag == helix::type_tag<ICfsCalibration>() ? static_cast<ICfsCalibration*>(this)
                                                         : AmsBackendMock::capability(tag);
    }
    AmsError calibrate_cutter(std::function<void(bool, const std::string&)>) override {
        return AmsErrorHelper::success();
    }
    AmsError start_chute_calibration(std::function<void()> on_ready,
                                     std::function<void(const std::string&)>,
                                     std::shared_ptr<std::atomic<bool>> cancel) override {
        ++starts;
        ready = std::move(on_ready);
        token = std::move(cancel);
        return AmsErrorHelper::success();
    }
    AmsError jog_chute_x(float) {
        return AmsErrorHelper::success();
    }
    AmsError jog_chute_y(float) override {
        return AmsErrorHelper::success();
    }
    AmsError save_chute_position(std::function<void()>,
                                 std::function<void(const std::string&)>) override {
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
    std::shared_ptr<std::atomic<bool>> token;
};

constexpr int kIdle = 0;
constexpr int kHoming = 1;
constexpr int kAdjusting = 2;

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

#endif // HELIX_HAS_CFS
