// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_layouts.cpp
 * @brief The motion panel's selectable Jog-tab layouts and their step list
 *
 * Run with: ./build/bin/helix-tests "[motion-layout]"
 *
 * Settings > Motion > Layout picks the Jog tab's arrangement. Every layout's
 * buttons go through one jog handler named by user_data, so the arrows, the
 * axis rows and the pad layout's Z column share clamping, the bed_moves
 * inversion and hold-to-repeat. The arrow layouts offer every configured jog
 * distance as a step (prestonbrown/helixscreen#865).
 */

#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_settings_motion.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::Axis;
using helix::JogButtonStep;
using helix::MotionLayout;

// ============================================================================
// Pure helpers
// ============================================================================

TEST_CASE("jog steps: distances sort ascending and duplicates merge", "[motion-layout]") {
    // The shipped table: Coarse inner repeats Fine outer, Turbo inner Coarse outer.
    const auto steps = helix::unique_sorted_steps({0.1f, 1.0f, 1.0f, 10.0f, 10.0f, 50.0f});
    REQUIRE(steps.size() == 4);
    CHECK(steps[0] == Catch::Approx(0.1f));
    CHECK(steps[1] == Catch::Approx(1.0f));
    CHECK(steps[2] == Catch::Approx(10.0f));
    CHECK(steps[3] == Catch::Approx(50.0f));

    // Out of order in, ascending out; a keypad float a hair off still merges.
    const auto messy = helix::unique_sorted_steps({100.0f, 25.0f, 0.1f, 0.1000001f, 25.0005f});
    REQUIRE(messy.size() == 3);
    CHECK(messy[0] == Catch::Approx(0.1f));
    CHECK(messy[1] == Catch::Approx(25.0f));
    CHECK(messy[2] == Catch::Approx(100.0f));

    // Distinct steps 0.01mm apart both survive.
    CHECK(helix::unique_sorted_steps({0.05f, 0.06f}).size() == 2);
    CHECK(helix::unique_sorted_steps({}).empty());
}

TEST_CASE("jog steps: the nearest step wins", "[motion-layout]") {
    const std::vector<float> steps{0.1f, 1.0f, 10.0f, 50.0f};
    CHECK(helix::nearest_step_index(steps, 10.0f) == 2);
    CHECK(helix::nearest_step_index(steps, 25.0f) == 2);
    CHECK(helix::nearest_step_index(steps, 31.0f) == 3);
    CHECK(helix::nearest_step_index(steps, 0.0f) == 0);
    CHECK(helix::nearest_step_index(steps, 500.0f) == 3);
    CHECK(helix::nearest_step_index({}, 5.0f) == 0);
}

TEST_CASE("jog buttons: user_data names decode", "[motion-layout]") {
    auto z = helix::parse_jog_button("z_up_large");
    REQUIRE(z);
    CHECK(z->axis == Axis::Z);
    CHECK(z->sign == 1);
    CHECK(z->step == JogButtonStep::Outer);

    auto zd = helix::parse_jog_button("z_down_small");
    REQUIRE(zd);
    CHECK(zd->sign == -1);
    CHECK(zd->step == JogButtonStep::Inner);

    auto x = helix::parse_jog_button("x_minus_step");
    REQUIRE(x);
    CHECK(x->axis == Axis::X);
    CHECK(x->sign == -1);
    CHECK(x->step == JogButtonStep::Selected);

    auto y = helix::parse_jog_button("y_plus_large");
    REQUIRE(y);
    CHECK(y->axis == Axis::Y);
    CHECK(y->sign == 1);

    // X and Y take plus/minus, Z takes up/down; nothing else decodes.
    CHECK_FALSE(helix::parse_jog_button("x_up_step"));
    CHECK_FALSE(helix::parse_jog_button("z_plus_step"));
    CHECK_FALSE(helix::parse_jog_button("e_plus_step"));
    CHECK_FALSE(helix::parse_jog_button("x_plus_huge"));
    CHECK_FALSE(helix::parse_jog_button("x_plus"));
    CHECK_FALSE(helix::parse_jog_button("xx_plus_step"));
    CHECK_FALSE(helix::parse_jog_button(""));
    CHECK_FALSE(helix::parse_jog_button(nullptr));
}

TEST_CASE_METHOD(HelixTestFixture, "jog steps: the list follows the configured distances",
                 "[motion-layout]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    s.reset_jog_distances();
    CHECK(helix::jog_step_list() == std::vector<float>{0.1f, 1.0f, 10.0f, 50.0f});

    s.set_jog_distance(helix::JogMode::Turbo, false, 25.0f);
    s.set_jog_distance(helix::JogMode::Turbo, true, 100.0f);
    CHECK(helix::jog_step_list() == std::vector<float>{0.1f, 1.0f, 10.0f, 25.0f, 100.0f});
    s.reset_jog_distances();
}

TEST_CASE_METHOD(HelixTestFixture, "motion layout setting defaults to the pad and clamps",
                 "[motion-layout]") {
    auto& s = helix::SettingsManager::instance();
    s.init_subjects();
    CHECK(s.get_motion_layout() == MotionLayout::Pad);
    s.set_motion_layout(MotionLayout::AxisRows);
    CHECK(s.get_motion_layout() == MotionLayout::AxisRows);
    CHECK(lv_subject_get_int(s.subject_motion_layout()) == 3);
    s.set_motion_layout(static_cast<MotionLayout>(9));
    CHECK(s.get_motion_layout() == MotionLayout::AxisRows);
    s.set_motion_layout(MotionLayout::Pad);
}

// ============================================================================
// Panel
// ============================================================================

namespace {

/// Records every relative move and home, acking at once.
class RecordingMotionAPI : public MoonrakerMotionAPI {
  public:
    using MoonrakerMotionAPI::MoonrakerMotionAPI;
    struct Move {
        double dx, dy, dz;
    };
    void move_relative(double dx, double dy, double dz, double, double, SuccessCallback on_success,
                       ErrorCallback) override {
        moves.push_back({dx, dy, dz});
        if (on_success)
            on_success();
    }
    void home_axes(const std::string& axes, SuccessCallback, ErrorCallback) override {
        homes.push_back(axes);
    }
    std::vector<Move> moves;
    std::vector<std::string> homes;
};

class RecordingAPI : public MoonrakerAPI {
  public:
    RecordingAPI(helix::IMoonrakerClient& client, helix::PrinterState& state)
        : MoonrakerAPI(client, state), recording_(client, state, get_safety_limits()) {}
    MoonrakerMotionAPI& motion() override {
        return recording_;
    }
    RecordingMotionAPI recording_;
};

class LayoutFixture : public LVGLUITestFixture {
  public:
    LayoutFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
        helix::SettingsManager::instance().init_subjects();
        helix::SettingsManager::instance().reset_jog_distances();
        set_moonraker_api(&api_);

        get_printer_state().update_from_status(
            {{"toolhead",
              {{"homed_axes", "xyz"},
               {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
               {"axis_maximum", {235.0, 235.0, 275.0, 0.0}}}},
             {"gcode_move", {{"gcode_position", {100.0, 100.0, 100.0, 0.0}}}}});
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        lv_subject_set_int(get_printer_state().network_state().get_nav_buttons_enabled_subject(),
                           1);
        drain();
    }

    ~LayoutFixture() override {
        set_moonraker_api(previous_api_);
        StaticPanelRegistry::instance().destroy_all();
        drain();
        helix::SettingsManager::instance().set_motion_layout(MotionLayout::Pad);
        helix::SettingsManager::instance().reset_jog_distances();
        lv_subject_set_int(
            get_printer_state().capabilities_state().subject(helix::Capability::BedMoves), 0);
        drain();
    }

    void show_with(MotionLayout layout) {
        helix::SettingsManager::instance().set_motion_layout(layout);
        REQUIRE(get_global_motion_panel().show(lv_screen_active()));
        drain();
        lv_obj_update_layout(lv_screen_active());
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static lv_obj_t* find(const char* name) {
        return lv_obj_find_by_name(get_global_motion_panel().get_root(), name);
    }

    static bool shown(const char* name) {
        lv_obj_t* obj = find(name);
        REQUIRE(obj != nullptr);
        for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
                return false;
        }
        return true;
    }

    static void click(const char* name) {
        lv_obj_t* obj = find(name);
        REQUIRE(obj != nullptr);
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    /// Click @p name inside the widget named @p scope.
    static void click_in(const char* scope, const char* name) {
        lv_obj_t* parent = find(scope);
        REQUIRE(parent != nullptr);
        lv_obj_t* obj = lv_obj_find_by_name(parent, name);
        REQUIRE(obj != nullptr);
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    RecordingMotionAPI& motion() {
        return api_.recording_;
    }

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    RecordingAPI api_{client_, get_printer_state()};
    IMoonrakerAPI* previous_api_ = get_moonraker_api();
};

} // namespace

TEST_CASE_METHOD(LayoutFixture, "motion layout: the pad layout builds no alternative",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Pad);
    CHECK(shown("jog_pad"));
    CHECK(shown("z_up_large"));
    CHECK(find("arrows_x_plus") == nullptr);
    CHECK(find("cross_x_plus") == nullptr);
    CHECK(find("rows_x_plus_large") == nullptr);
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: arrows replace the pad and the Z column",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    CHECK(shown("arrows_x_plus"));
    CHECK(shown("arrows_z_up"));
    CHECK_FALSE(shown("jog_pad"));
    CHECK_FALSE(shown("z_up_large"));
    CHECK_FALSE(shown("jog_mode_fine"));

    // Other tabs keep the shared Z column.
    get_global_motion_panel().set_motion_tab(1);
    drain();
    CHECK(shown("z_up_large"));
    get_global_motion_panel().set_motion_tab(0);
    drain();
    CHECK_FALSE(shown("z_up_large"));

    // Changing the setting while the panel is open swaps the layout in place.
    helix::SettingsManager::instance().set_motion_layout(MotionLayout::AxisRows);
    drain();
    CHECK(find("arrows_x_plus") == nullptr);
    CHECK(shown("rows_x_plus_large"));
    helix::SettingsManager::instance().set_motion_layout(MotionLayout::Pad);
    drain();
    CHECK(find("rows_x_plus_large") == nullptr);
    CHECK(shown("jog_pad"));
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: the step selector offers every distance",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    for (const char* name : {"step_0", "step_1", "step_2", "step_3"}) {
        CHECK(shown(name));
    }
    CHECK(find("step_4") == nullptr);
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "motion_step_label_0"))) ==
          "0.1");
    CHECK(std::string(lv_subject_get_string(lv_xml_get_subject(nullptr, "motion_step_label_3"))) ==
          "50");

    auto& panel = get_global_motion_panel();
    click("step_2");
    CHECK(panel.selected_jog_step() == Catch::Approx(10.0f));
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "motion_step_active_2")) == 1);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "motion_step_active_1")) == 0);

    click("arrows_x_plus");
    click("arrows_y_minus");
    REQUIRE(motion().moves.size() == 2);
    CHECK(motion().moves[0].dx == Catch::Approx(10.0));
    CHECK(motion().moves[0].dy == Catch::Approx(0.0));
    CHECK(motion().moves[1].dy == Catch::Approx(-10.0));

    // A new distance joins the selector the next time the panel activates,
    // and the selection stays on the step it was on.
    helix::SettingsManager::instance().set_jog_distance(helix::JogMode::Turbo, false, 25.0f);
    panel.refresh_step_list();
    drain();
    CHECK(shown("step_4"));
    CHECK(panel.selected_jog_step() == Catch::Approx(10.0f));
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "motion_step_count")) == 5);
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: the selected step persists",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::ArrowsHome);
    click("step_3");
    CHECK(Config::get_instance()->get<float>("/motion/jog_step", 0.0f) == Catch::Approx(50.0f));

    // Dropping that distance moves the selection to the nearest one left.
    helix::SettingsManager::instance().set_jog_distance(helix::JogMode::Turbo, true, 30.0f);
    get_global_motion_panel().refresh_step_list();
    drain();
    CHECK(get_global_motion_panel().selected_jog_step() == Catch::Approx(30.0f));
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: Z arrows follow the bed_moves inversion",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::ArrowsHome);
    click("step_1"); // 1mm
    click("cross_z_up");
    lv_subject_set_int(
        get_printer_state().capabilities_state().subject(helix::Capability::BedMoves), 1);
    drain();
    click("cross_z_up");
    REQUIRE(motion().moves.size() == 2);
    CHECK(motion().moves[0].dz == Catch::Approx(1.0));
    // Up on a bed-moves printer raises the bed: G-code Z goes down.
    CHECK(motion().moves[1].dz == Catch::Approx(-1.0));
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: axis rows move the jog mode's distances",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::AxisRows);
    auto& panel = get_global_motion_panel();
    panel.set_jog_mode(helix::JogMode::Coarse);
    drain();
    click("rows_x_plus_large");
    click("rows_x_minus_small");
    click("rows_y_plus_small");
    click("rows_z_down_large");
    REQUIRE(motion().moves.size() == 4);
    CHECK(motion().moves[0].dx == Catch::Approx(10.0));
    CHECK(motion().moves[1].dx == Catch::Approx(-1.0));
    CHECK(motion().moves[2].dy == Catch::Approx(1.0));
    CHECK(motion().moves[3].dz == Catch::Approx(-10.0));

    // The rows' mode buttons switch the distances the rows move.
    click_in("rows_modes", "jog_mode_fine");
    CHECK(panel.get_jog_mode() == helix::JogMode::Fine);
    click("rows_x_plus_large");
    REQUIRE(motion().moves.size() == 5);
    CHECK(motion().moves[4].dx == Catch::Approx(1.0));
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: home buttons home their axes",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::ArrowsHome);
    click("cross_home_xy");
    click("cross_home_z");
    click("cross_home_all");
    REQUIRE(motion().homes.size() == 3);
    CHECK(motion().homes[0] == "XY");
    CHECK(motion().homes[1] == "Z");
    CHECK(motion().homes[2].empty());
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: a home button is solid while its axis is unhomed",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    lv_obj_t* home_x = find("arrows_home_x");
    lv_obj_t* home_all = find("arrows_home_all");
    REQUIRE(home_x);
    REQUIRE(home_all);
    CHECK(lv_obj_get_style_bg_opa(home_x, LV_PART_MAIN) < LV_OPA_COVER);
    CHECK(lv_obj_get_style_bg_opa(home_all, LV_PART_MAIN) < LV_OPA_COVER);

    get_printer_state().update_from_status({{"toolhead", {{"homed_axes", "yz"}}}});
    drain();
    CHECK(lv_obj_get_style_bg_opa(home_x, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_bg_opa(home_all, LV_PART_MAIN) == LV_OPA_COVER);
    CHECK(lv_obj_get_style_bg_opa(find("arrows_home_y"), LV_PART_MAIN) < LV_OPA_COVER);
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: a held arrow repeats through the shared timer",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    click("step_1");
    lv_obj_t* arrow = find("arrows_x_minus");
    REQUIRE(arrow);
    auto& panel = get_global_motion_panel();
    lv_obj_send_event(arrow, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().ticking());
    CHECK(panel.jog_hold_timer().poll(helix::HoldRepeat::DELAY_MS));
    CHECK(
        panel.jog_hold_timer().poll(helix::HoldRepeat::DELAY_MS + helix::HoldRepeat::kIntervalMs));
    lv_obj_send_event(arrow, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(arrow, LV_EVENT_CLICKED, nullptr);
    drain();
    // Two repeat ticks, and the release click after a repeat adds nothing.
    REQUIRE(motion().moves.size() == 2);
    CHECK(motion().moves[0].dx == Catch::Approx(-1.0));
    CHECK(motion().moves[1].dx == Catch::Approx(-1.0));
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: a new panel starts on the persisted step",
                 "[motion-layout]") {
    Config::get_instance()->set<float>("/motion/jog_step", 25.0f);
    MotionPanel fresh;
    CHECK(fresh.selected_jog_step() == Catch::Approx(25.0f));
    Config::get_instance()->set<float>("/motion/jog_step", 1.0f);
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: the settings dropdown picks the layout",
                 "[motion-layout][xml]") {
    helix::settings::show_motion_settings_overlay();
    drain();
    lv_obj_t* row = lv_obj_find_by_name(lv_screen_active(), "row_motion_layout");
    if (!row)
        row = lv_obj_find_by_name(lv_layer_top(), "row_motion_layout");
    REQUIRE(row != nullptr);
    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    REQUIRE(dropdown != nullptr);
    CHECK(lv_dropdown_get_selected(dropdown) == 0);

    lv_dropdown_set_selected(dropdown, 2);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    drain();
    CHECK(helix::SettingsManager::instance().get_motion_layout() == MotionLayout::ArrowsHome);

    // The dropdown follows the setting when it changes elsewhere.
    helix::SettingsManager::instance().set_motion_layout(MotionLayout::AxisRows);
    drain();
    CHECK(lv_dropdown_get_selected(dropdown) == 3);
}

namespace {

/// One jog key and the relative move a tap on it must send.
struct ExpectedJog {
    const char* name;
    double dx, dy, dz;
};

size_t jog_buttons_declared(const char* path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    const std::string xml = text.str();
    size_t n = 0;
    for (size_t pos = 0; (pos = xml.find("<motion_jog_button ", pos)) != std::string::npos; ++pos)
        ++n;
    return n;
}

} // namespace

TEST_CASE_METHOD(LayoutFixture, "motion layout: every jog key sends its own move",
                 "[motion-layout][xml]") {
    // Coarse mode (inner 1, outer 10), selected step 0.1 (index 0), a
    // head-moves printer, everything homed mid-envelope: no clamp, no
    // inversion, and every distance distinct per key class.
    struct LayoutCase {
        MotionLayout layout;
        const char* file;
        std::vector<ExpectedJog> keys;
    };
    const std::vector<LayoutCase> cases = {
        {MotionLayout::Pad,
         "ui_xml/motion_panel.xml",
         // The Z column is declared once per orientation; the landscape copy is built.
         {{"z_up_large", 0, 0, 10},
          {"z_up_small", 0, 0, 1},
          {"z_down_small", 0, 0, -1},
          {"z_down_large", 0, 0, -10}}},
        {MotionLayout::Arrows,
         "ui_xml/components/motion_jog_arrows.xml",
         {{"arrows_x_minus", -0.1, 0, 0},
          {"arrows_x_plus", 0.1, 0, 0},
          {"arrows_y_plus", 0, 0.1, 0},
          {"arrows_y_minus", 0, -0.1, 0},
          {"arrows_z_up", 0, 0, 0.1},
          {"arrows_z_down", 0, 0, -0.1}}},
        {MotionLayout::ArrowsHome,
         "ui_xml/components/motion_jog_arrows_home.xml",
         {{"cross_x_minus", -0.1, 0, 0},
          {"cross_x_plus", 0.1, 0, 0},
          {"cross_y_plus", 0, 0.1, 0},
          {"cross_y_minus", 0, -0.1, 0},
          {"cross_z_up", 0, 0, 0.1},
          {"cross_z_down", 0, 0, -0.1}}},
        {MotionLayout::AxisRows,
         "ui_xml/components/motion_jog_rows.xml",
         {{"rows_x_minus_large", -10, 0, 0},
          {"rows_x_minus_small", -1, 0, 0},
          {"rows_x_plus_small", 1, 0, 0},
          {"rows_x_plus_large", 10, 0, 0},
          {"rows_y_minus_large", 0, -10, 0},
          {"rows_y_minus_small", 0, -1, 0},
          {"rows_y_plus_small", 0, 1, 0},
          {"rows_y_plus_large", 0, 10, 0},
          {"rows_z_down_large", 0, 0, -10},
          {"rows_z_down_small", 0, 0, -1},
          {"rows_z_up_small", 0, 0, 1},
          {"rows_z_up_large", 0, 0, 10}}},
    };

    show_with(MotionLayout::Pad);
    auto& panel = get_global_motion_panel();
    panel.set_jog_mode(helix::JogMode::Coarse);
    for (const auto& lc : cases) {
        helix::SettingsManager::instance().set_motion_layout(lc.layout);
        drain();
        if (find("step_0"))
            click("step_0");
        const size_t declared = jog_buttons_declared(lc.file);
        const size_t expected =
            lc.layout == MotionLayout::Pad ? 2 * lc.keys.size() : lc.keys.size();
        INFO(lc.file << " declares " << declared << " jog keys");
        REQUIRE(declared == expected);
        for (const auto& key : lc.keys) {
            INFO("key " << key.name);
            motion().moves.clear();
            click(key.name);
            REQUIRE(motion().moves.size() == 1);
            CHECK(motion().moves[0].dx == Catch::Approx(key.dx));
            CHECK(motion().moves[0].dy == Catch::Approx(key.dy));
            CHECK(motion().moves[0].dz == Catch::Approx(key.dz));
        }
    }
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: a held key deleted mid-hold stops jogging",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    auto& panel = get_global_motion_panel();
    lv_obj_t* arrow = find("arrows_x_plus");
    REQUIRE(arrow);
    lv_obj_send_event(arrow, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().ticking());

    // Deleting the held key sends no RELEASED.
    lv_obj_delete(arrow);
    drain();
    // The first tick finds the key gone and ends the hold without jogging.
    panel.jog_hold_timer().poll(helix::HoldRepeat::DELAY_MS);
    CHECK_FALSE(panel.jog_hold_timer().ticking());
    CHECK_FALSE(
        panel.jog_hold_timer().poll(helix::HoldRepeat::DELAY_MS + helix::HoldRepeat::kIntervalMs));
    CHECK(motion().moves.empty());
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: a layout change ends a hold",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    auto& panel = get_global_motion_panel();
    lv_obj_t* arrow = find("arrows_y_plus");
    REQUIRE(arrow);
    lv_obj_send_event(arrow, LV_EVENT_PRESSED, nullptr);
    REQUIRE(panel.jog_hold_timer().ticking());

    helix::SettingsManager::instance().set_motion_layout(MotionLayout::AxisRows);
    drain();
    CHECK_FALSE(panel.jog_hold_timer().ticking());
    CHECK_FALSE(panel.jog_hold_timer().poll(helix::HoldRepeat::DELAY_MS));
    CHECK(motion().moves.empty());
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: the pad fits when it appears after another layout",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::AxisRows);
    helix::SettingsManager::instance().set_motion_layout(MotionLayout::Pad);
    drain();
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* pad = find("jog_pad");
    REQUIRE(pad);
    CHECK(lv_obj_get_width(pad) > 50);
    CHECK(lv_obj_get_width(pad) == lv_obj_get_height(pad));
}

TEST_CASE("jog layout fit: keys stay near square and the blocks fit the view", "[motion-layout]") {
    // The layout slot measured at each target size, its gap, and the step bar
    // height (button height, never under the touch minimum).
    struct Box {
        const char* label;
        int w, h, gap, step_h;
        bool portrait;
    };
    const Box boxes[] = {
        {"480x272", 358, 216, 2, 48, false}, {"480x320", 346, 271, 2, 48, false},
        {"800x480", 603, 400, 5, 52, false}, {"1024x600", 777, 512, 6, 72, false},
        {"480x800", 466, 529, 5, 80, true},
    };
    constexpr int kMinKey = 48;
    for (const auto& box : boxes) {
        for (auto layout :
             {MotionLayout::Arrows, MotionLayout::ArrowsHome, MotionLayout::AxisRows}) {
            INFO(box.label << " layout " << static_cast<int>(layout));
            const auto fit = helix::jog_layout_fit(layout, box.portrait, box.w, box.h, box.gap,
                                                   box.step_h, 6, kMinKey);
            REQUIRE(fit.key_w > 0);
            REQUIRE(fit.key_h > 0);
            const double aspect = static_cast<double>(std::max(fit.key_w, fit.key_h)) /
                                  std::min(fit.key_w, fit.key_h);
            CHECK(aspect <= 1.41);
            CHECK(fit.key_h >= kMinKey);
            if (layout == MotionLayout::AxisRows) {
                CHECK(fit.row.w <= box.w);
                CHECK(4 * fit.row.h + 3 * box.gap <= box.h);
                continue;
            }
            const int body_w =
                box.portrait ? fit.home.w : fit.xy.w + fit.z.w + fit.home.w + 2 * box.gap;
            const int body_h = box.portrait ? fit.xy.h + box.gap + fit.home.h : fit.xy.h;
            CHECK(body_w <= box.w);
            CHECK(body_h + box.gap + box.step_h <= box.h);
            CHECK(fit.steps_w <= body_w);
            // Six step keys never come out wider than 1.4x their height.
            CHECK(fit.steps_w <= 6 * box.step_h * 7 / 5 + 5 * box.gap);
            // The keys use the view: one axis is within a key of filling it.
            const bool fills_w = box.w - body_w < fit.key_w;
            const bool fills_h = box.h - (body_h + box.gap + box.step_h) < fit.key_h;
            CHECK((fills_w || fills_h));
        }
    }
    CHECK(helix::jog_layout_fit(MotionLayout::Pad, false, 600, 400, 5, 52, 4, kMinKey).key_w == 0);
}

TEST_CASE_METHOD(LayoutFixture, "motion layout: the panel applies the fitted key blocks",
                 "[motion-layout][xml]") {
    show_with(MotionLayout::Arrows);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t* xy = find("arrows_xy");
    lv_obj_t* y_plus = find("arrows_y_plus");
    REQUIRE(xy);
    REQUIRE(y_plus);
    // The cross is sized from the key, and each key is near square.
    CHECK(lv_obj_get_width(xy) > 0);
    const int w = lv_obj_get_width(y_plus);
    const int h = lv_obj_get_height(y_plus);
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    CHECK(static_cast<double>(std::max(w, h)) / std::min(w, h) <= 1.45);
}
