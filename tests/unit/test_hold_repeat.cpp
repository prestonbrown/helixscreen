// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_hold_repeat.cpp
 * @brief Hold-to-repeat: timing decisions, the shared LVGL binding, and the
 *        blocked-jog stop at panel level.
 *
 * The pure HoldRepeat tests run without LVGL on purpose: the unit-test
 * harness only executes one-shot timers, never the periodic lv_timer a real
 * hold runs on, so everything time-based is driven with explicit
 * elapsed-millisecond values through HoldRepeat::on_elapsed() and
 * HoldRepeatTimer::poll().
 */

#include "hold_repeat.h"
#include "hold_repeat_timer.h"

#include "../catch_amalgamated.hpp"

using helix::HoldRepeat;

TEST_CASE("hold repeat fires nothing before the delay", "[hold_repeat]") {
    HoldRepeat hr;
    hr.press();
    CHECK(hr.active());
    CHECK_FALSE(hr.on_elapsed(0));
    CHECK_FALSE(hr.on_elapsed(HoldRepeat::DELAY_MS - 1));
    CHECK(hr.on_elapsed(HoldRepeat::DELAY_MS));
    CHECK(hr.swallow_click());
}

TEST_CASE("hold repeat cadence after the first fire", "[hold_repeat]") {
    HoldRepeat hr;
    hr.press();
    REQUIRE(hr.on_elapsed(HoldRepeat::DELAY_MS));
    // One repeat per kIntervalMs, anchored on the press.
    CHECK_FALSE(hr.on_elapsed(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs - 1));
    CHECK(hr.on_elapsed(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    CHECK_FALSE(hr.on_elapsed(HoldRepeat::DELAY_MS + 2 * HoldRepeat::kIntervalMs - 1));
    CHECK(hr.on_elapsed(HoldRepeat::DELAY_MS + 2 * HoldRepeat::kIntervalMs));
}

TEST_CASE("a late poll fires once and skips the missed intervals", "[hold_repeat]") {
    HoldRepeat hr;
    hr.press();
    // A stalled main loop: several interval boundaries went by unanswered.
    REQUIRE(hr.on_elapsed(HoldRepeat::DELAY_MS + 600));
    // The next poll, one period later, must not repay the missed intervals.
    CHECK_FALSE(hr.on_elapsed(HoldRepeat::DELAY_MS + 650));
    CHECK(hr.on_elapsed(HoldRepeat::DELAY_MS + 600 + HoldRepeat::kIntervalMs));
}

TEST_CASE("a tap with no repeat jogs once via the click path", "[hold_repeat]") {
    HoldRepeat hr;
    hr.press();
    CHECK_FALSE(hr.on_elapsed(250));
    hr.release();
    CHECK_FALSE(hr.swallow_click());
    // Nothing fires after release even if polled late.
    CHECK_FALSE(hr.on_elapsed(5000));
}

TEST_CASE("release swallows the click only after a repeat fired", "[hold_repeat]") {
    HoldRepeat hr;
    // Released without any repeat: the click goes through.
    hr.press();
    hr.release();
    CHECK_FALSE(hr.swallow_click());
    // Released after one repeat: the click must not jog again.
    hr.press();
    REQUIRE(hr.on_elapsed(HoldRepeat::DELAY_MS));
    hr.release();
    CHECK(hr.swallow_click());
}

TEST_CASE("cancel stops ticks and forgets the repeat", "[hold_repeat]") {
    HoldRepeat hr;
    hr.press();
    REQUIRE(hr.on_elapsed(HoldRepeat::DELAY_MS));
    hr.cancel();
    CHECK_FALSE(hr.active());
    CHECK_FALSE(hr.on_elapsed(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    CHECK_FALSE(hr.swallow_click());
}

TEST_CASE("a new press after cancel starts clean", "[hold_repeat]") {
    HoldRepeat hr;
    hr.press();
    REQUIRE(hr.on_elapsed(HoldRepeat::DELAY_MS));
    hr.cancel();
    hr.press();
    // The full delay applies again and no stale swallow carries over.
    CHECK(hr.active());
    CHECK_FALSE(hr.swallow_click());
    CHECK_FALSE(hr.on_elapsed(HoldRepeat::DELAY_MS - 1));
    REQUIRE(hr.on_elapsed(HoldRepeat::DELAY_MS));
    CHECK(hr.swallow_click());
}

// ============================================================================
// The shared LVGL binding
// ============================================================================

#include "../lvgl_test_fixture.h"

namespace {

struct FireCtx {
    int fires = 0;
    bool allow = true;

    bool fire() {
        ++fires;
        return allow;
    }
};

bool ctx_fire(void* user_data) {
    return static_cast<FireCtx*>(user_data)->fire();
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "HoldRepeatTimer fires on poll and stops when refused",
                 "[hold_repeat][ui]") {
    helix::HoldRepeatTimer timer;
    FireCtx ctx;

    timer.begin(&ctx_fire, &ctx);
    REQUIRE(timer.ticking());

    // Below the delay: nothing fires, ticking continues.
    CHECK_FALSE(timer.poll(HoldRepeat::DELAY_MS - 1));
    CHECK(ctx.fires == 0);
    CHECK(timer.ticking());

    // First repeat fires; the owner allows more.
    CHECK(timer.poll(HoldRepeat::DELAY_MS));
    REQUIRE(ctx.fires == 1);
    CHECK(timer.ticking());
    CHECK(timer.swallow_click());

    // The owner refuses (jog blocked): ticking stops, but the repeat that
    // already fired still swallows the imminent click.
    ctx.allow = false;
    CHECK(timer.poll(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    REQUIRE(ctx.fires == 2);
    CHECK_FALSE(timer.ticking());
    // A stopped timer polls nothing further, whatever the clock says.
    CHECK_FALSE(timer.poll(60'000));
    CHECK(ctx.fires == 2);
    CHECK(timer.swallow_click());

    timer.cancel();
    CHECK_FALSE(timer.swallow_click());
}

TEST_CASE_METHOD(LVGLTestFixture, "HoldRepeatTimer release stops ticks and keeps the swallow",
                 "[hold_repeat][ui]") {
    helix::HoldRepeatTimer timer;
    FireCtx ctx;

    timer.begin(&ctx_fire, &ctx);
    CHECK(timer.poll(HoldRepeat::DELAY_MS));
    REQUIRE(ctx.fires == 1);

    timer.release();
    CHECK_FALSE(timer.ticking());
    CHECK(timer.swallow_click());
    CHECK_FALSE(timer.poll(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    CHECK(ctx.fires == 1);

    timer.cancel();
    CHECK_FALSE(timer.swallow_click());
}
// ============================================================================
// Panel level: a blocked jog cancels the Z hold repeat
// ============================================================================

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/log_capture.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "core/update_queue.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "toolhead_homing.h"

#include <array>
#include <lvgl.h>
#include <string>
#include <vector>

TEST_CASE_METHOD(LVGLUITestFixture, "a Z jog blocked at the ceiling cancels the hold repeat",
                 "[motion][hold_repeat][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    lv_obj_t* root = panel.get_root();
    REQUIRE(root != nullptr);
    lv_obj_t* z_up = lv_obj_find_by_name(root, "z_up_large");
    REQUIRE(z_up != nullptr);

    // Known envelope (Z 0..250), all axes homed, toolhead parked at the Z
    // ceiling: any z_up jog clamps to zero.
    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       25'000); // 250.00mm
    helix::ui::UpdateQueue::instance().drain();

    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& msg) { warnings.push_back(msg); });

    // Press arms the repeat (this is the XML pressed event's whole job).
    lv_obj_send_event(z_up, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().ticking());

    // Before the delay: no repeat, no jog attempt, no toast.
    CHECK_FALSE(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS - 1));
    CHECK(warnings.empty());

    // First repeat at the delay is the press's INITIAL jog: a fresh press
    // already at the limit warns every time, and the refusal stops the repeat.
    CHECK(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS));
    CHECK_FALSE(panel.jog_hold_timer().ticking());
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("Z is at its limit") != std::string::npos);
    CHECK(warnings.front().find("250.00mm") != std::string::npos);

    // Repeat ticks into the wall can never raise a toast (and none fire: the
    // refusal already stopped the timer).
    CHECK_FALSE(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    CHECK(warnings.size() == 1);

    // The release click is swallowed (a repeat fired): no extra jog, no toast.
    lv_obj_send_event(z_up, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(z_up, LV_EVENT_CLICKED, nullptr);
    CHECK(warnings.size() == 1);
    CHECK_FALSE(panel.jog_hold_timer().ticking());

    helix::ui::set_test_notification_warning_hook(nullptr);
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a hold that repeated jogs exactly once on release",
                 "[motion][hold_repeat][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    lv_obj_t* root = panel.get_root();
    lv_obj_t* z_up = lv_obj_find_by_name(root, "z_up_large");
    REQUIRE(z_up != nullptr);

    // Known envelope (Z 0..250), homed, toolhead mid-range: every z_up jog
    // moves, so how many jogs the gesture dispatched is observable.
    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       10'000); // 100.00mm
    helix::ui::UpdateQueue::instance().drain();

    // A real API over a mock client: every dispatched jog lands in the mock's
    // gcode history as one relative-move script.
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api = get_moonraker_api();
    set_moonraker_api(&api);
    client.clear_gcode_script_history();

    // handle_jog_button() logs one line per accepted jog decision, before any
    // coalescing: the count stays honest whether or not the move acked yet.
    helix::TextLogCapture log;
    const auto jog_decisions = [&log] {
        int n = 0;
        const std::string blob = log.get_captured();
        for (size_t pos = blob.find("Z jog:"); pos != std::string::npos;
             pos = blob.find("Z jog:", pos + 1)) {
            ++n;
        }
        return n;
    };
    const auto jog_scripts = [&client] {
        int n = 0;
        for (const auto& script : client.gcode_script_history()) {
            if (script.find("G91") != std::string::npos) {
                ++n;
            }
        }
        return n;
    };

    lv_obj_send_event(z_up, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().ticking());
    // Exactly one repeat fires inside the delay window, and it dispatches.
    REQUIRE(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS));
    REQUIRE(jog_decisions() == 1);

    // Release after a repeated hold: the CLICKED must be swallowed, adding
    // neither a jog decision nor a second move.
    lv_obj_send_event(z_up, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(z_up, LV_EVENT_CLICKED, nullptr);
    CHECK(jog_decisions() == 1);
    CHECK(jog_scripts() == 1);
    CHECK_FALSE(panel.jog_hold_timer().ticking());

    set_moonraker_api(previous_api);
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

// ============================================================================
// Limit feedback: a fresh press at a limit warns every time, repeat ticks
// stay silent, and the Z buttons render their limit state.
// ============================================================================

TEST_CASE("z_direction_blocked decides from the bound the button drives toward",
          "[motion][limits]") {
    // At the ceiling: up is impossible, down is not.
    CHECK(helix::z_direction_blocked(true, true, 250.0, 0.0, 250.0, +1.0));
    CHECK_FALSE(helix::z_direction_blocked(true, true, 250.0, 0.0, 250.0, -1.0));
    // At the floor: down is impossible, up is not.
    CHECK(helix::z_direction_blocked(true, true, 0.0, 0.0, 250.0, -1.0));
    CHECK_FALSE(helix::z_direction_blocked(true, true, 0.0, 0.0, 250.0, +1.0));
    // Mid-range: both directions have room.
    CHECK_FALSE(helix::z_direction_blocked(true, true, 125.0, 0.0, 250.0, +1.0));
    CHECK_FALSE(helix::z_direction_blocked(true, true, 125.0, 0.0, 250.0, -1.0));

    // bed_moves printers pass the post-inversion direction: at the ceiling,
    // the button driving G-code Z- still has room.
    CHECK_FALSE(helix::z_direction_blocked(true, true, 250.0, 0.0, 250.0, -10.0));
    CHECK(helix::z_direction_blocked(true, true, 250.0, 0.0, 250.0, +10.0));

    // A hair inside the envelope is still at the bound: the epsilon reads the
    // distance to the bound, not the move.
    CHECK(helix::z_direction_blocked(true, true, 250.0 - 1e-7, 0.0, 250.0, +1.0));

    // Without homing or a known envelope the buttons stay enabled.
    CHECK_FALSE(helix::z_direction_blocked(false, true, 250.0, 0.0, 250.0, +1.0));
    CHECK_FALSE(helix::z_direction_blocked(true, false, 250.0, 0.0, 250.0, +1.0));
}

TEST_CASE_METHOD(LVGLUITestFixture, "two taps at the Z limit warn twice", "[motion][limits][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       25'000); // 250.00mm
    helix::ui::UpdateQueue::instance().drain();

    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& msg) { warnings.push_back(msg); });

    // Each tap is a fresh press: both warn, with the limit it hit.
    CHECK_FALSE(panel.handle_jog_button("z_up_large"));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("Z is at its limit (250.00mm)") != std::string::npos);
    CHECK_FALSE(panel.handle_jog_button("z_up_large"));
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[1].find("Z is at its limit (250.00mm)") != std::string::npos);

    helix::ui::set_test_notification_warning_hook(nullptr);
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a hold that moves then hits the ceiling stops silently",
                 "[motion][limits][xml]") {
    // The Z button distances come from SettingsManager; without init_subjects()
    // the cache reads as zero and clamps to the 0.01mm floor, so the hold
    // would creep a hundredth at a time and never reach the ceiling.
    SettingsManager::instance().init_subjects();

    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    lv_obj_t* root = panel.get_root();
    lv_obj_t* z_up = lv_obj_find_by_name(root, "z_up_large");
    REQUIRE(z_up != nullptr);

    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::ui::UpdateQueue::instance().drain();

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api = get_moonraker_api();
    set_moonraker_api(&api);

    // 5mm below the ceiling, seeded after the klippy/print-state churn above:
    // processing those events rewrites the position subjects. The first tick
    // (Coarse outer, 10mm) moves the remaining 5 as a silent partial; the
    // second is fully clamped and stops the repeat without a toast.
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       24'500); // 245.00mm
    helix::ui::UpdateQueue::instance().drain();

    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& msg) { warnings.push_back(msg); });

    lv_obj_send_event(z_up, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().ticking());

    // Partial first tick: moves 5 of the requested 10, no toast, keeps ticking.
    CHECK(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS));
    CHECK(panel.jog_hold_timer().ticking());
    CHECK(warnings.empty());

    // Repeat tick into the ceiling: fully clamped, stops the repeat, no toast.
    CHECK(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    CHECK_FALSE(panel.jog_hold_timer().ticking());
    CHECK(warnings.empty());

    helix::ui::set_test_notification_warning_hook(nullptr);
    set_moonraker_api(previous_api);
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "Z buttons disable at the ceiling and swap under bed_moves",
                 "[motion][limits][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    lv_obj_t* root = panel.get_root();
    lv_obj_t* up_large = lv_obj_find_by_name(root, "z_up_large");
    lv_obj_t* up_small = lv_obj_find_by_name(root, "z_up_small");
    lv_obj_t* down_large = lv_obj_find_by_name(root, "z_down_large");
    REQUIRE(up_large != nullptr);
    REQUIRE(up_small != nullptr);
    REQUIRE(down_large != nullptr);

    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       10'000); // 100.00mm
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(up_large, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(down_large, LV_STATE_DISABLED));

    // Reaching the max disables both up buttons; down still has room.
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       25'000); // 250.00mm
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_obj_has_state(up_large, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(up_small, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(down_large, LV_STATE_DISABLED));

    // A bed-moves printer swaps the direction each on-screen arrow drives, so
    // the disabled pair swaps with it.
    lv_subject_set_int(get_printer_state().capabilities_state().subject(Capability::BedMoves), 1);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(up_large, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(down_large, LV_STATE_DISABLED));

    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a coordinate tap sends one absolute single-axis move",
                 "[motion][coords][xml]") {
    SettingsManager::instance().init_subjects();

    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::ui::UpdateQueue::instance().drain();

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api = get_moonraker_api();
    set_moonraker_api(&api);
    client.clear_gcode_script_history();

    // The setup reached the branch under test: Y homed with a known envelope,
    // so the tap dispatches immediately with no homing detour.
    REQUIRE(get_printer_state().motion_state().get_axis_bounds().has_y);
    REQUIRE(helix::axis_is_homed(get_printer_state(), helix::Axis::Y));

    panel.request_axis_target('y', 100.0);
    helix::ui::UpdateQueue::instance().drain();

    const auto& history = client.gcode_script_history();
    int move_lines = 0;
    std::string move;
    int homing_lines = 0;
    for (const auto& script : history) {
        if (script.find("G0 Y100") != std::string::npos) {
            ++move_lines;
            move = script;
        }
        if (script.find("G28") != std::string::npos) {
            ++homing_lines;
        }
    }
    REQUIRE(move_lines == 1);
    CHECK(homing_lines == 0);
    // Only Y is commanded: the other axes must not appear as G0 terms.
    CHECK(move.find(" X") == std::string::npos);
    CHECK(move.find(" Z") == std::string::npos);

    set_moonraker_api(previous_api);
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a Z button disabled under a hold drops its pressed look and follows commanded Z",
                 "[motion][limits][xml]") {
    SettingsManager::instance().init_subjects();

    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    lv_obj_t* z_up = lv_obj_find_by_name(panel.get_root(), "z_up_large");
    REQUIRE(z_up != nullptr);

    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
    lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                       static_cast<int>(helix::PrintJobState::STANDBY));
    helix::ui::UpdateQueue::instance().drain();

    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api{client, get_printer_state()};
    IMoonrakerAPI* previous_api = get_moonraker_api();
    set_moonraker_api(&api);

    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(),
                       24'500); // 245.00mm
    helix::ui::UpdateQueue::instance().drain();

    // A real finger sets PRESSED through the input device; the test sets it.
    lv_obj_add_state(z_up, LV_STATE_PRESSED);
    lv_obj_send_event(z_up, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS)); // moves the last 5mm

    // Any recompute before the head reports 250 reads commanded Z, so the up
    // buttons stay live even while the move toward the ceiling is in flight.
    lv_subject_set_int(get_printer_state().capabilities_state().subject(Capability::BedMoves), 1);
    lv_subject_set_int(get_printer_state().capabilities_state().subject(Capability::BedMoves), 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK_FALSE(lv_obj_has_state(z_up, LV_STATE_DISABLED));

    // The head reaches the ceiling: the held button disables under the finger,
    // and the next tick's refusal clears the pressed look LVGL would keep.
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_z_subject(), 25'000);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(lv_obj_has_state(z_up, LV_STATE_DISABLED));
    CHECK(panel.jog_hold_timer().poll(HoldRepeat::DELAY_MS + HoldRepeat::kIntervalMs));
    CHECK_FALSE(panel.jog_hold_timer().ticking());
    CHECK_FALSE(lv_obj_has_state(z_up, LV_STATE_PRESSED));

    set_moonraker_api(previous_api);
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a diagonal along a wall moves the free axis without warning",
                 "[motion][limits][xml]") {
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels)
        p = lv_obj_create(lv_screen_active());
    NavigationManager::instance().set_panels(panels.data());

    lv_obj_t* cached = nullptr;
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    cached = get_global_motion_panel().get_root();
    helix::ui::UpdateQueue::instance().drain();

    MotionPanel& panel = get_global_motion_panel();
    get_printer_state().update_from_status({{"toolhead",
                                             {{"homed_axes", "xyz"},
                                              {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
                                              {"axis_maximum", {235.0, 235.0, 250.0, 0.0}}}}});
    helix::ui::UpdateQueue::instance().drain();
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_x_subject(),
                       23'500); // X at max
    lv_subject_set_int(get_printer_state().motion_state().get_gcode_position_y_subject(),
                       10'000); // Y mid
    helix::ui::UpdateQueue::instance().drain();

    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& msg) { warnings.push_back(msg); });

    // NE: X is pinned, Y still moves, so the press is partial travel.
    panel.jog(helix::JogDirection::NE, 10.0f);
    CHECK(warnings.empty());

    // E: nothing can move, so the fresh press warns.
    panel.jog(helix::JogDirection::E, 10.0f);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("X is at its limit (235.00mm)") != std::string::npos);

    helix::ui::set_test_notification_warning_hook(nullptr);
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
}
