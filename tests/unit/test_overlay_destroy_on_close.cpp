// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_destroy_on_close.cpp
 * @brief Overlays that free their widget tree on close rebuild it on reopen
 *
 * Run with: ./build/bin/helix-tests "[overlay_destroy_on_close]"
 *
 * Each case opens the real overlay through its caller path, pops it, and
 * checks the tree was deleted and the overlay's own root pointer dropped, then
 * reopens it and pops it again.
 */

#include "ui_ams_environment_overlay.h"
#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_calibration_pa.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_tool_offset.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_motion.h"
#include "ui_printer_manager_overlay.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_motion.h"
#include "ui_theme_editor_overlay.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

void count_delete(lv_event_t* e) {
    ++*static_cast<int*>(lv_event_get_user_data(e));
}

class DestroyOnCloseFixture : public LVGLUITestFixture {
  protected:
    DestroyOnCloseFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
    }

    ~DestroyOnCloseFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    /// Open, pop, check the tree is gone; then reopen and pop again.
    void expect_rebuilt_on_reopen(const std::function<void()>& open,
                                  const std::function<lv_obj_t*()>& root) {
        for (int round = 0; round < 2; ++round) {
            open();
            settle();
            lv_obj_t* opened = root();
            REQUIRE(opened != nullptr);
            int deletes = 0;
            lv_obj_add_event_cb(opened, count_delete, LV_EVENT_DELETE, &deletes);

            NavigationManager::instance().go_back();
            settle();
            CHECK(deletes == 1);
            CHECK(root() == nullptr);
        }
    }

    std::array<lv_obj_t*, UI_PANEL_COUNT> panels_{};
};

} // namespace

TEST_CASE_METHOD(DestroyOnCloseFixture, "Motion rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][motion]") {
    auto& p = get_global_motion_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Motion observers fired after close touch no freed tree",
                 "[overlay_destroy_on_close][motion]") {
    auto& p = get_global_motion_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
    // The homed and position observers outlive the tree and reach for the jog pad.
    lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "xyz");
    settle();
    lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "");
    settle();
    CHECK(p.get_root() == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Macro Buttons rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][macro_buttons]") {
    auto& p = helix::settings::get_macro_buttons_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Motion Settings rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][motion_settings]") {
    expect_rebuilt_on_reopen(
        [&] { helix::settings::show_motion_settings_overlay(); },
        [&] { return lv_obj_find_by_name(lv_screen_active(), "jog_speed_xy_slider"); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Theme Editor rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][theme_editor]") {
    auto& p = get_theme_editor_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Temp Graph rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][temp_graph]") {
    auto& p = get_global_temp_graph_overlay();
    expect_rebuilt_on_reopen([&] { p.open(TempGraphOverlay::Mode::Nozzle, lv_screen_active()); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "PA Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][pa_cal]") {
    auto& p = get_global_pa_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Tool Offset Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][tool_offset_cal]") {
    auto& p = get_global_tool_offset_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

#if HELIX_HAS_CFS
TEST_CASE_METHOD(DestroyOnCloseFixture, "CFS Chute Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][cfs_chute]") {
    auto& p = get_cfs_chute_calibration_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}
#endif

TEST_CASE_METHOD(DestroyOnCloseFixture, "PID Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][pid_cal]") {
    auto& p = get_global_pid_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture,
                 "PID Calibration results and ticks after close touch no tree",
                 "[overlay_destroy_on_close][pid_cal]") {
    auto& p = get_global_pid_cal_panel();
    REQUIRE(p.show(lv_screen_active()));
    settle();
    p.arm_eta_timer_for_test();
    REQUIRE(p.eta_timer_for_test() != nullptr);

    NavigationManager::instance().go_back();
    settle();
    CHECK(p.get_root() == nullptr);
    CHECK(p.eta_timer_for_test() == nullptr);

    p.on_calibration_result(false, 0, 0, 0, "late failure");
    p.on_calibration_result(true, 1.0f, 2.0f, 3.0f);
    settle();
    CHECK(p.get_root() == nullptr);

    REQUIRE(p.show(lv_screen_active()));
    settle();
    CHECK(p.get_state() == PIDCalibrationPanel::State::IDLE);
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Z Offset Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][zoffset_cal]") {
    auto& p = get_global_zoffset_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture,
                 "Z Offset Calibration results and probe updates after close touch no tree",
                 "[overlay_destroy_on_close][zoffset_cal]") {
    auto& p = get_global_zoffset_cal_panel();
    REQUIRE(p.show(lv_screen_active()));
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE(p.get_root() == nullptr);

    // The probe observers die with the tree: a Klipper-side probe starting
    // while the overlay is closed must not move a closed panel to ADJUSTING.
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.get_manual_probe_active_subject(), 1);
    settle();
    CHECK(p.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    lv_subject_set_int(ps.get_manual_probe_active_subject(), 0);
    settle();

    p.update_z_position(0.2f);
    p.on_calibration_result(true, "");
    p.on_calibration_result(false, "late failure");
    settle();
    CHECK(p.get_root() == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Touch Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][touch_cal]") {
    auto& p = helix::ui::get_touch_calibration_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active(), nullptr)); },
                             [&] { return p.get_root(); });
    // The lifted capture surface goes back under the root, so nothing is left on the screen.
    CHECK(lv_obj_find_by_name(lv_screen_active(), "touch_capture_overlay") == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Touch Calibration feedback after close touches no tree",
                 "[overlay_destroy_on_close][touch_cal]") {
    auto& p = helix::ui::get_touch_calibration_overlay();
    REQUIRE(p.show(lv_screen_active(), nullptr));
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE(p.get_root() == nullptr);

    p.on_progress();
    p.on_capture_feedback({10, 10});
    p.on_verify_feedback({10, 10});
    p.handle_screen_touched(nullptr);
    p.handle_screen_released();
    settle();
    CHECK(p.get_root() == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Printer Manager rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][printer_manager]") {
    auto& p = get_printer_manager_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

namespace {

/// Text of the label inside tab @p index of a repeat-built tab strip, or "".
std::string tab_label_text(lv_obj_t* strip, uint32_t index) {
    lv_obj_t* tab = strip ? lv_obj_get_child(strip, static_cast<int32_t>(index)) : nullptr;
    lv_obj_t* label = tab ? lv_obj_find_by_name(tab, "tab_label") : nullptr;
    return label ? lv_label_get_text(label) : "";
}

/// Two native strips and one effect, so the tab row, the swatches and the effect
/// chips are all built.
class LedDestroyFixture : public DestroyOnCloseFixture {
  protected:
    LedDestroyFixture() {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(nullptr, nullptr);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            helix::led::LedStripInfo strip;
            strip.name = id;
            strip.id = id;
            strip.backend = helix::led::LedBackendType::NATIVE;
            strip.supports_color = true;
            ctrl.native().add_strip(strip);
        }
        helix::led::LedEffectInfo glow;
        glow.name = "led_effect glow";
        glow.display_name = "Glow";
        glow.target_leds = {"neopixel chamber_light"};
        ctrl.effects().add_effect(glow);
    }

    ~LedDestroyFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
        helix::led::LedController::instance().deinit();
    }
};

/// A capped two-box AMS rig whose first box is mid-dry.
class AmsEnvDestroyFixture : public DestroyOnCloseFixture {
  protected:
    AmsEnvDestroyFixture() {
        auto& ams = AmsState::instance();
        ams.deinit_subjects();
        auto backend = std::make_unique<AmsBackendMock>();
        backend->set_multi_unit_mode(true);
        backend->set_environment_mode("capped");
        REQUIRE(backend->start().success());
        ams.set_backend(std::move(backend));
        ams.init_subjects(true);
        ams.sync_from_backend();
        zones_ = ams.get_backend()->get_environment_zones(-1);
        REQUIRE(zones_.size() == 2);
    }

    ~AmsEnvDestroyFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
        AmsState::instance().set_backend(nullptr);
    }

    std::vector<helix::printer::EnvironmentZone> zones_;
};

} // namespace

TEST_CASE_METHOD(LedDestroyFixture, "LED Control rebuilds its tree and tabs on reopen",
                 "[overlay_destroy_on_close][led_control]") {
    auto& p = get_led_control_overlay();
    expect_rebuilt_on_reopen(
        [&] { REQUIRE(helix::open_led_control_overlay(lv_screen_active(), "") != nullptr); },
        [&] { return p.get_root(); });

    // The tab pools go with the tree.
    CHECK(lv_xml_get_subject(nullptr, "led_tab_name_0") == nullptr);

    // A reopen builds its tab row against live pools: each tab names its device.
    REQUIRE(helix::open_led_control_overlay(lv_screen_active(), "") != nullptr);
    settle();
    lv_obj_t* row = lv_obj_find_by_name(p.get_root(), "led_tab_row");
    REQUIRE(row != nullptr);
    REQUIRE(lv_obj_get_child_count(row) == 2);
    for (uint32_t i = 0; i < 2; ++i) {
        lv_subject_t* name =
            lv_xml_get_subject(nullptr, ("led_tab_name_" + std::to_string(i)).c_str());
        REQUIRE(name != nullptr);
        CHECK(tab_label_text(row, i) == lv_subject_get_string(name));
        CHECK_FALSE(tab_label_text(row, i).empty());
    }
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(LedDestroyFixture,
                 "LED Control effect timeout, state frames and theme flips after close touch no "
                 "tree",
                 "[overlay_destroy_on_close][led_control]") {
    auto& p = get_led_control_overlay();
    REQUIRE(helix::open_led_control_overlay(lv_screen_active(), "neopixel chamber_light") !=
            nullptr);
    settle();

    // Tapping the effect chip arms the bounded wait for a status frame.
    lv_obj_t* chip = lv_obj_find_by_name(p.get_root(), "led_chip_0");
    REQUIRE(chip != nullptr);
    lv_obj_send_event(chip, LV_EVENT_CLICKED, nullptr);
    lv_subject_t* active = lv_xml_get_subject(nullptr, "led_active_chip");
    REQUIRE(active != nullptr);
    REQUIRE(lv_subject_get_int(active) == 0);

    NavigationManager::instance().go_back();
    settle();
    REQUIRE(p.get_root() == nullptr);

    // A state frame, a theme flip and the effect timeout all land after close.
    auto& ctrl = helix::led::LedController::instance();
    lv_subject_set_int(ctrl.get_led_state_version_subject(),
                       lv_subject_get_int(ctrl.get_led_state_version_subject()) + 1);
    lv_subject_set_int(theme_manager_get_changed_subject(),
                       lv_subject_get_int(theme_manager_get_changed_subject()) + 1);
    process_lvgl(4500);
    settle();
    CHECK(p.get_root() == nullptr);

    // And the next open still works.
    REQUIRE(helix::open_led_control_overlay(lv_screen_active(), "") != nullptr);
    settle();
    CHECK(lv_obj_find_by_name(p.get_root(), "led_chip_0") != nullptr);
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(AmsEnvDestroyFixture, "AMS Environment rebuilds its tree and zone tabs on reopen",
                 "[overlay_destroy_on_close][ams_environment]") {
    auto& p = get_ams_environment_overlay();
    expect_rebuilt_on_reopen([&] { p.show_zone(lv_screen_active(), zones_, 0, true); },
                             [&] { return p.get_root(); });

    // The zone-tab pools go with the tree.
    CHECK(lv_xml_get_subject(nullptr, "env_zone_tab_label_0") == nullptr);

    // A reopen builds its tab strip against live pools, and re-derives the dryer
    // state from the backend: box one is still drying.
    p.show_zone(lv_screen_active(), zones_, 0, true);
    settle();
    lv_obj_t* strip = lv_obj_find_by_name(p.get_root(), "zone_tab_strip");
    REQUIRE(strip != nullptr);
    REQUIRE(lv_obj_get_child_count(strip) == 2);
    for (uint32_t i = 0; i < 2; ++i) {
        lv_subject_t* label =
            lv_xml_get_subject(nullptr, ("env_zone_tab_label_" + std::to_string(i)).c_str());
        REQUIRE(label != nullptr);
        CHECK(tab_label_text(strip, i) == lv_subject_get_string(label));
        CHECK_FALSE(tab_label_text(strip, i).empty());
    }
    lv_subject_t* drying = lv_xml_get_subject(nullptr, "ams_env_overlay_drying_active");
    REQUIRE(drying != nullptr);
    CHECK(lv_subject_get_int(drying) == 1);
    lv_obj_t* temp = lv_obj_find_by_name(p.get_root(), "temp_input");
    REQUIRE(temp != nullptr);
    CHECK(std::string(lv_textarea_get_text(temp)) != "");
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(AmsEnvDestroyFixture,
                 "AMS Environment zone switches, refreshes and dryer updates after close touch "
                 "no tree",
                 "[overlay_destroy_on_close][ams_environment]") {
    auto& p = get_ams_environment_overlay();
    p.show_zone(lv_screen_active(), zones_, 0, true);
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE(p.get_root() == nullptr);

    // select_zone() republishes the dryer presets and inputs; refresh() and the
    // dryer subjects are what the live-update path drives.
    p.select_zone(1);
    p.select_zone(0);
    p.refresh();
    auto& ams = AmsState::instance();
    lv_subject_set_int(ams.get_dryer_active_subject(), 0);
    lv_subject_set_int(ams.get_dryer_current_temp_subject(), 42);
    settle();
    CHECK(p.get_root() == nullptr);
}
