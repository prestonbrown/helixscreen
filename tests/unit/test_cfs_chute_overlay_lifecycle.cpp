// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_test_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "ams_backend_mock.h"
#include "ams_cfs_calibration.h"
#include "ams_state.h"
#include "app_globals.h"
#include "chute_jog_guard.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "misc/lv_timer_private.h"
#include "printer_state.h"
#include "settings_manager.h"

#include <array>
#include <set>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

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
    AmsError jog_chute_x(float delta) override {
        x_jogs.push_back(delta);
        return AmsErrorHelper::success();
    }
    AmsError jog_chute_y(float delta) override {
        y_jogs.push_back(delta);
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
    std::vector<float> x_jogs;
    std::vector<float> y_jogs;
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
        SettingsManager::instance().init_subjects();
        SettingsManager::instance().reset_jog_distances();

        REQUIRE(get_cfs_chute_calibration_overlay().show(lv_screen_active()));
        settle();
        get_cfs_chute_calibration_overlay().on_activate();
    }

    ~ChuteOverlayFixture() override {
        SettingsManager::instance().reset_jog_distances();
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

    /// Reports the head at (@p x, @p y) inside a K1 Max sized envelope, the way
    /// the status feed does.
    static void report_toolhead(double x, double y) {
        get_printer_state().motion_state().update_from_status(
            nlohmann::json{{"toolhead",
                            {{"position", {x, y, 0.0, 0.0}},
                             {"axis_minimum", {-5.0, -0.5, 0.0, 0.0}},
                             {"axis_maximum", {305.0, 307.5, 300.0, 0.0}}}}});
        helix::ui::UpdateQueue::instance().drain();
    }

    /// Adjusting with the head at park, past the guard's settle time, and the
    /// pad laid out so a press lands where a finger would.
    void reach_settled_park() {
        report_toolhead(kParkX, kParkY);
        reach_adjusting();
        process_lvgl(ChuteJogGuard::kSettleMs + 100);
        locate_pad();
    }

    void locate_pad() {
        UITest::init(lv_screen_active());
        lv_obj_t* pad =
            lv_obj_find_by_name(get_cfs_chute_calibration_overlay().get_root(), "jog_pad");
        REQUIRE(pad);
        lv_obj_update_layout(pad);
        lv_area_t coords;
        lv_obj_get_coords(pad, &coords);
        pad_radius_ = lv_area_get_width(&coords) / 2;
        REQUIRE(pad_radius_ > 0);
        pad_x_ = coords.x1 + pad_radius_;
        pad_y_ = coords.y1 + pad_radius_;
    }

    /// A tap at the given fraction of the pad's radius from its centre.
    void tap_pad(double dx, double dy) {
        REQUIRE(UITest::click_at(pad_x_ + static_cast<int32_t>(dx * pad_radius_),
                                 pad_y_ + static_cast<int32_t>(dy * pad_radius_)));
        process_lvgl(50);
    }

    /// A press held past several repeat intervals, then released. The harness
    /// pauses periodic timers, so the timer the press armed is fired by hand.
    void hold_pad(double dx, double dy) {
        std::set<lv_timer_t*> before;
        for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
            before.insert(t);
        }
        REQUIRE(UITest::press_at(pad_x_ + static_cast<int32_t>(dx * pad_radius_),
                                 pad_y_ + static_cast<int32_t>(dy * pad_radius_)));
        for (int i = 0; i < 40; ++i) {
            process_lvgl(50);
            std::vector<lv_timer_t*> armed;
            for (lv_timer_t* t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) {
                if (before.count(t) == 0 && t->timer_cb && t->repeat_count != 0) {
                    armed.push_back(t);
                }
            }
            for (lv_timer_t* t : armed) {
                t->timer_cb(t);
            }
        }
        REQUIRE(UITest::release());
        process_lvgl(50);
    }

    static constexpr double kParkX = 184.5;
    static constexpr double kParkY = 291.5;
    static constexpr double kInnerRing = 0.42;
    static constexpr double kOuterRing = 0.8;

  private:
    std::array<lv_obj_t*, kUiPanelCount> panels_{};
    int32_t pad_x_ = 0;
    int32_t pad_y_ = 0;
    int32_t pad_radius_ = 0;
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

TEST_CASE_METHOD(ChuteOverlayFixture, "Chute readouts follow the live toolhead on both axes",
                 "[ams][cfs][chute_overlay]") {
    report_toolhead(184.5, 304.25);
    auto text = [](const char* widget) {
        lv_obj_t* label =
            lv_obj_find_by_name(get_cfs_chute_calibration_overlay().get_root(), widget);
        REQUIRE(label);
        return std::string(lv_label_get_text(label));
    };
    CHECK(text("chute_x_display").find("184.50") != std::string::npos);
    CHECK(text("chute_y_display").find("304.25") != std::string::npos);
}

TEST_CASE_METHOD(ChuteOverlayFixture,
                 "The chute jog pad sends each ring and axis through the guard",
                 "[ams][cfs][chute_overlay]") {
    const auto fine = get_jog_mode_distances(JogMode::Fine);

    SECTION("nothing moves before the guard has settled") {
        report_toolhead(kParkX, kParkY);
        reach_adjusting();
        locate_pad();
        tap_pad(kOuterRing, 0.0);
        CHECK(backend->x_jogs.empty());
    }

    SECTION("the four directions reach the matching axis and sign") {
        reach_settled_park();
        tap_pad(kOuterRing, 0.0);
        REQUIRE(backend->x_jogs.size() == 1);
        CHECK(backend->x_jogs[0] == Catch::Approx(fine.outer));
        tap_pad(-kInnerRing, 0.0);
        REQUIRE(backend->x_jogs.size() == 2);
        CHECK(backend->x_jogs[1] == Catch::Approx(-fine.inner));
        tap_pad(0.0, -kInnerRing);
        REQUIRE(backend->y_jogs.size() == 1);
        CHECK(backend->y_jogs[0] == Catch::Approx(fine.inner));
        tap_pad(0.0, kInnerRing);
        REQUIRE(backend->y_jogs.size() == 2);
        CHECK(backend->y_jogs[1] == Catch::Approx(-fine.inner));
    }

    SECTION("past park a coarse X tap is refused and a fine one goes out") {
        reach_settled_park();
        tap_pad(0.0, -kOuterRing);
        REQUIRE(backend->y_jogs.size() == 1);
        tap_pad(kOuterRing, 0.0);
        CHECK(backend->x_jogs.empty());
        REQUIRE(fine.inner <= ChuteJogGuard::kFineStepMm + ChuteJogGuard::kStepToleranceMm);
        tap_pad(kInnerRing, 0.0);
        CHECK(backend->x_jogs.size() == 1);
    }

    SECTION("past park a held X key sends one step, at park it repeats") {
        reach_settled_park();
        hold_pad(kInnerRing, 0.0);
        const size_t at_park = backend->x_jogs.size();
        CHECK(at_park > 1);

        tap_pad(0.0, -kOuterRing);
        REQUIRE(backend->y_jogs.size() == 1);
        hold_pad(kInnerRing, 0.0);
        CHECK(backend->x_jogs.size() == at_park + 1);
    }
}

#endif // HELIX_HAS_CFS
