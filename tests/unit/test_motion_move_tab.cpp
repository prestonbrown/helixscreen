// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_move_tab.cpp
 * @brief The Move tab's presets, Park fallback and gating
 *
 * Run with: ./build/bin/helix-tests "[motion][move-tab]"
 *
 * The motion panel's Jog/Move tab switch drives everything through the
 * motion_tab subject: the tab containers' hidden flags follow it, the preset
 * grid computes its target at tap time from the live axis envelope, and Park
 * resolves through StandardMacros, lifting Z and parking over the rear of the
 * plate when no park macro exists. Every grid button is disabled while a print
 * is active, the toolhead is busy, or the nav buttons are off, and the C++
 * handlers re-check the print and nav gates.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/bed_occupancy_state_test_access.h"
#include "../test_helpers/holding_motion_api.h"
#include "../test_helpers/modal_answer.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "bed_occupancy_state.h"
#include "bed_plate_context.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

// The full status frame that makes the panel actionable: axes homed, a
// A centre-origin Voron 2.4 envelope (±175 in X/Y, 275 tall), klippy
// ready, no print running.
nlohmann::json ready_status(const char* homed_axes) {
    return {
        {"toolhead",
         {{"homed_axes", homed_axes},
          {"axis_minimum", {-175.0, -175.0, 0.0, 0.0}},
          {"axis_maximum", {175.0, 175.0, 275.0, 0.0}}}},
        {"gcode_move", {{"gcode_position", {10.0, 10.0, 10.0, 0.0}}}},
    };
}

using helix::HoldingAPI;

class MoveTabFixture : public LVGLUITestFixture {
  public:
    MoveTabFixture() {
        std::array<lv_obj_t*, kUiPanelCount> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());

        helix::SettingsManager::instance().init_subjects();

        lv_obj_t* cached = nullptr;
        REQUIRE(get_global_motion_panel().show(lv_screen_active()));
        cached = get_global_motion_panel().get_root();
        drain();

        set_moonraker_api(&api_);
        api_.update_safety_limits_from_printer([&] { limits_done_ = true; },
                                               [&](const MoonrakerError&) { limits_done_ = true; });
        for (int i = 0; i < 200 && !limits_done_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        client_.clear_gcode_script_history();

        get_printer_state().update_from_status(ready_status("xyz"));
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        lv_subject_set_int(get_printer_state().print_state().get_print_state_enum_subject(),
                           static_cast<int>(helix::PrintJobState::STANDBY));
        // No live connection exists in the fixture, so the derived gate would
        // stay shut; set it the way a connected READY printer would.
        lv_subject_set_int(get_printer_state().network_state().get_nav_buttons_enabled_subject(),
                           1);
        drain();
    }

    ~MoveTabFixture() override {
        set_moonraker_api(previous_api_);
        if (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        StandardMacros::instance().reset();
        drain();
        StaticPanelRegistry::instance().destroy_all();
        drain();
    }

    static void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }

    static void click(lv_obj_t* widget) {
        lv_obj_send_event(widget, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    lv_obj_t* panel_widget(const char* name) const {
        lv_obj_t* widget = lv_obj_find_by_name(get_global_motion_panel().get_root(), name);
        REQUIRE(widget != nullptr);
        return widget;
    }

    std::string all_scripts() const {
        std::string joined;
        for (const auto& script : client_.gcode_script_history()) {
            joined += script;
            joined += "\n";
        }
        return joined;
    }

    // 350x350 with a 10% inset: the rear-right preset lands at 140.0/140.0
    // and the front row at Y -140.0.
    static constexpr float kRearRightXY = 140.0f;
    static constexpr float kFrontY = -140.0f;

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api_{client_, get_printer_state()};
    IMoonrakerAPI* previous_api_ = get_moonraker_api();
    /// Discovery widens the motion safety window from the machine's own
    /// steppers; the centre-origin persona needs that before any bed gesture
    /// can target a negative coordinate.
    std::atomic<bool> limits_done_{false};
};

} // namespace

namespace {

/// Puts a part of @p height_mm on the bed for the panel to see, for the scope's life.
class ScopedPartOnBed {
  public:
    explicit ScopedPartOnBed(double height_mm) : state_(get_printer_state(), nullptr) {
        state_.init_subjects();
        // The state's own observers fire on the drain and re-decide from its (empty)
        // history; the verdict goes in after them.
        MoveTabFixture::drain();
        helix::BedOccupancyStateTestAccess::set_verdict(state_,
                                                        {true, height_mm, "job-on-the-bed"});
        previous_ = helix::get_bed_occupancy_state();
        helix::set_bed_occupancy_state(&state_);
    }
    ~ScopedPartOnBed() {
        helix::set_bed_occupancy_state(previous_);
    }
    ScopedPartOnBed(const ScopedPartOnBed&) = delete;
    ScopedPartOnBed& operator=(const ScopedPartOnBed&) = delete;

  private:
    helix::BedOccupancyState state_;
    helix::BedOccupancyState* previous_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(MoveTabFixture, "motion tab switching toggles the tab containers",
                 "[motion][move-tab][xml]") {
    auto& ps = get_printer_state();
    // Jog is the panel's landing tab: the jog row is up, the Move tab is not.
    CHECK_FALSE(lv_obj_has_flag(panel_widget("jog_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(panel_widget("jog_modes"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));

    get_global_motion_panel().set_motion_tab(1);
    drain();
    CHECK(lv_obj_has_flag(panel_widget("jog_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("jog_modes"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));
    // The Z column stays on every tab.
    CHECK_FALSE(lv_obj_has_flag(panel_widget("z_controls"), LV_OBJ_FLAG_HIDDEN));

    get_global_motion_panel().set_motion_tab(0);
    drain();
    CHECK_FALSE(lv_obj_has_flag(panel_widget("jog_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(MoveTabFixture, "the tab rail renders filled pills", "[motion][move-tab][xml]") {
    // The rail's direct children are the zone_tab roots: the active tab is a
    // solid primary pill, idle tabs sit on the card fill. The AMS strip's
    // flat/selected-card look is the zone_tab default; the motion XML opts
    // into the pill pair by style-name props.
    lv_obj_t* rail = panel_widget("motion_tab_rail");
    REQUIRE(lv_obj_get_child_count(rail) >= 2);
    lv_obj_t* active = lv_obj_get_child(rail, 0);
    lv_obj_t* idle = lv_obj_get_child(rail, 1);
    const auto& palette = ThemeManager::instance().current_palette();
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(active, LV_PART_MAIN), palette.primary));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(idle, LV_PART_MAIN), palette.card_bg));
}

TEST_CASE_METHOD(MoveTabFixture, "the selected tab pill's icon and label read on its fill",
                 "[motion][move-tab][xml]") {
    // Light mode pairs dark #text with a dark #primary, so the pill only reads
    // when its content takes the contrast colour.
    REQUIRE_FALSE(theme_manager_is_dark_mode());
    for (const char* strip : {"motion_tab_rail", "header_tabs"}) {
        INFO(strip);
        lv_obj_t* active = lv_obj_get_child(panel_widget(strip), 0);
        REQUIRE(active != nullptr);
        const lv_color_t fill = lv_obj_get_style_bg_color(active, LV_PART_MAIN);
        REQUIRE(lv_color_eq(fill, ThemeManager::instance().current_palette().primary));
        for (const char* part : {"tab_icon", "tab_label"}) {
            INFO(part);
            lv_obj_t* obj = lv_obj_find_by_name(active, part);
            REQUIRE(obj != nullptr);
            CHECK(helix::contrast_ratio(lv_obj_get_style_text_color(obj, LV_PART_MAIN), fill) >=
                  kThemeTextContrastThreshold);
        }
    }
}

TEST_CASE_METHOD(MoveTabFixture, "homed preset tap sends one absolute move", "[motion][move-tab]") {
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("preset_rear_right"));

    const std::string scripts = all_scripts();
    CHECK(scripts.find("G0 X140 Y140") != std::string::npos);
    // Exactly one absolute move, and no homing detour for homed axes.
    CHECK(scripts.find("G28") == std::string::npos);
    size_t move_entries = 0;
    for (const auto& script : client_.gcode_script_history()) {
        if (script.find("X140") != std::string::npos) {
            ++move_entries;
        }
    }
    CHECK(move_entries == 1);
}

TEST_CASE_METHOD(MoveTabFixture, "unhomed presets are disabled and send nothing",
                 "[motion][move-tab][xml]") {
    get_global_motion_panel().set_motion_tab(1);
    drain();
    lv_obj_t* preset = panel_widget("preset_front");
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));

    // Any one axis unhomed is enough: X and Y homed, Z not.
    get_printer_state().update_from_status(ready_status("xy"));
    drain();
    for (const char* name :
         {"preset_rear_left", "preset_rear", "preset_rear_right", "preset_left", "preset_center",
          "preset_right", "preset_front_left", "preset_front", "preset_front_right"}) {
        INFO(name);
        CHECK(lv_obj_has_state(panel_widget(name), LV_STATE_DISABLED));
    }

    // The backstop: a click delivered anyway neither homes nor moves.
    lv_obj_clear_state(preset, LV_STATE_DISABLED);
    click(preset);
    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "a preset lifts above a part still on the bed",
                 "[motion][move-tab]") {
    ScopedPartOnBed part(38.0);
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("preset_front"));

    // The default 5mm clearance over a 38mm part, before the travel.
    const std::string scripts = all_scripts();
    INFO(scripts);
    const auto lift = scripts.find("Z43");
    const auto travel = scripts.find("Y-140");
    REQUIRE(lift != std::string::npos);
    REQUIRE(travel != std::string::npos);
    CHECK(lift < travel);
}

TEST_CASE_METHOD(MoveTabFixture, "a preset from below clearance lifts to it first",
                 "[motion][move-tab]") {
    get_printer_state().update_from_status(
        {{"gcode_move", {{"gcode_position", {10.0, 10.0, 0.2, 0.0}}}}});
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("preset_front"));

    const std::string scripts = all_scripts();
    INFO(scripts);
    const auto lift = scripts.find("Z5");
    const auto travel = scripts.find("Y-140");
    REQUIRE(lift != std::string::npos);
    REQUIRE(travel != std::string::npos);
    CHECK(lift < travel);
}

TEST_CASE_METHOD(MoveTabFixture, "a preset refuses a part too tall to clear",
                 "[motion][move-tab]") {
    // Z travel is 275mm; a 274.5mm part leaves under 1mm above it.
    ScopedPartOnBed part(274.5);
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("preset_front"));

    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "park runs the configured macro", "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder", "gcode_macro PARK"});
    StandardMacros::instance().init(hardware);
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::ParkToolhead).detected_macro ==
            "PARK");

    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_park"));

    const std::string scripts = all_scripts();
    CHECK(scripts.find("PARK") != std::string::npos);
    CHECK(scripts.find("Y-140") == std::string::npos); // fallback not taken
}

TEST_CASE_METHOD(MoveTabFixture,
                 "park without a macro lifts Z and parks over the rear of the plate",
                 "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().init(hardware);
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::ParkToolhead).is_empty());

    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_park"));

    // 350x350 plate, commanded Z 10: centred in X, 10mm inside the rear edge,
    // nozzle up 10mm, and never the front preset.
    const std::string scripts = all_scripts();
    CHECK(scripts.find("PARK") == std::string::npos);
    CHECK(scripts.find("X0 ") != std::string::npos);
    CHECK(scripts.find("Y165") != std::string::npos);
    CHECK(scripts.find("Z20") != std::string::npos);
    CHECK(scripts.find("Y-140") == std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "unhomed park is disabled and sends nothing, macro or not",
                 "[motion][move-tab][xml]") {
    const bool with_macro = GENERATE(false, true);
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(with_macro ? nlohmann::json{"extruder", "gcode_macro PARK"}
                                      : nlohmann::json{"extruder"});
    StandardMacros::instance().init(hardware);

    get_global_motion_panel().set_motion_tab(1);
    drain();
    get_printer_state().update_from_status(ready_status(""));
    drain();
    lv_obj_t* park = panel_widget("move_park");
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));

    lv_obj_clear_state(park, LV_STATE_DISABLED);
    click(park);
    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "park lifts to the plate floor when that is higher",
                 "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().init(hardware);
    ScopedPartOnBed part(38.0);
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_park"));

    // Commanded Z 10: current + 10 is 20, the floor over a 38mm part is 43.
    const std::string scripts = all_scripts();
    INFO(scripts);
    CHECK(scripts.find("Z43") != std::string::npos);
    CHECK(scripts.find("Y165") != std::string::npos);
    CHECK(scripts.find("Z20") == std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "park refuses a part too tall to clear", "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().init(hardware);
    ScopedPartOnBed part(274.5);
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_park"));

    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "an unhomed coordinate is disabled and its entry sends nothing",
                 "[motion][move-tab][coords][xml]") {
    get_printer_state().update_from_status(ready_status("yz"));
    drain();
    // Only the unhomed axis's coordinate greys out (header or portrait row,
    // whichever this layout built).
    lv_obj_t* root = get_global_motion_panel().get_root();
    lv_obj_t* x_group = lv_obj_find_by_name(root, "header_pos_x_group");
    lv_obj_t* y_group = lv_obj_find_by_name(root, "header_pos_y_group");
    if (!x_group) {
        x_group = lv_obj_find_by_name(root, "row_pos_x_group");
        y_group = lv_obj_find_by_name(root, "row_pos_y_group");
    }
    REQUIRE(x_group != nullptr);
    REQUIRE(y_group != nullptr);
    CHECK(lv_obj_has_state(x_group, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(y_group, LV_STATE_DISABLED));

    get_global_motion_panel().request_axis_target('x', 50.0);
    drain();
    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());

    // The homed axis still moves.
    get_global_motion_panel().request_axis_target('y', 50.0);
    drain();
    CHECK(all_scripts().find("Y50") != std::string::npos);

    // Each axis greys out on its own homing, Y and Z included.
    lv_obj_t* z_group = lv_obj_find_by_name(root, "header_pos_z_group");
    if (!z_group) {
        z_group = lv_obj_find_by_name(root, "row_pos_z_group");
    }
    REQUIRE(z_group != nullptr);
    get_printer_state().update_from_status(ready_status("xz"));
    drain();
    CHECK_FALSE(lv_obj_has_state(x_group, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(y_group, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(z_group, LV_STATE_DISABLED));
    get_printer_state().update_from_status(ready_status("xy"));
    drain();
    CHECK_FALSE(lv_obj_has_state(y_group, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(z_group, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(MoveTabFixture, "the grid greys out while the toolhead is busy, Z stays live",
                 "[motion][move-tab]") {
    auto& ps = get_printer_state();
    get_global_motion_panel().set_motion_tab(1);
    drain();

    lv_obj_t* preset = panel_widget("preset_center");
    lv_obj_t* park = panel_widget("move_park");
    lv_obj_t* z_up = panel_widget("z_up_small");

    ps.update_from_status({{"idle_timeout", {{"state", "Printing"}}}});
    drain();
    CHECK(lv_obj_has_state(preset, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(z_up, LV_STATE_DISABLED));

    ps.update_from_status({{"idle_timeout", {{"state", "Ready"}}}});
    drain();
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(MoveTabFixture, "motors off opens the shared confirm", "[motion][move-tab]") {
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_motors_off"));

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // Dismissing the confirm must not disable the motors.
    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);
    click(secondary);
    CHECK(all_scripts().find("M84") == std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "a print or disabled nav gates the move grid",
                 "[motion][move-tab]") {
    auto& ps = get_printer_state();
    get_global_motion_panel().set_motion_tab(1);
    drain();

    lv_obj_t* preset = panel_widget("preset_rear_right");
    lv_obj_t* park = panel_widget("move_park");
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));

    // The print gate rides the lifecycle pipeline (job_holds_machine ->
    // machine_motion_blocked), never the raw print_active subject: that reads 0
    // while a host-side pre-print block is already homing the toolhead.
    ps.update_from_status({{"print_stats", {{"state", "printing"}}}});
    drain();
    CHECK(lv_subject_get_int(ps.print_state().get_machine_motion_blocked_subject()) == 1);
    CHECK(lv_obj_has_state(preset, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));

    ps.update_from_status({{"print_stats", {{"state", "standby"}}}});
    lv_subject_set_int(ps.network_state().get_nav_buttons_enabled_subject(), 0);
    drain();
    CHECK(lv_obj_has_state(preset, LV_STATE_DISABLED));

    lv_subject_set_int(ps.network_state().get_nav_buttons_enabled_subject(), 1);
    drain();
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));

    // The backstop: with the gate shut, a synthesized click on an enabled
    // widget still moves nothing.
    ps.update_from_status({{"print_stats", {{"state", "printing"}}}});
    drain();
    lv_obj_clear_state(preset, LV_STATE_DISABLED);
    click(preset);
    CHECK(all_scripts().empty());
}

TEST_CASE_METHOD(MoveTabFixture,
                 "MotionPanel: a boot-time init_subjects() marks the panel initialized",
                 "[motion][move-tab][init]") {
    // SubjectInitializer initializes the panel before anything opens it; show()
    // skips init_subjects() only when the panel reports itself initialized.
    helix::ui::destroy_static_panels(); // the fixture's own panel is already initialized
    auto& panel = get_global_motion_panel();
    REQUIRE_FALSE(panel.are_subjects_initialized());

    panel.init_subjects();

    CHECK(panel.are_subjects_initialized());
}

namespace {

/// A point far outside the Bed tab's surface on the given side: every touch
/// there clamps to the plate's edge, so the target does not depend on the
/// test screen's size.
lv_point_t far_point(lv_obj_t* area, bool right, bool bottom) {
    lv_area_t c;
    lv_obj_get_coords(area, &c);
    return {right ? c.x2 + 1000 : c.x1 - 1000, bottom ? c.y2 + 1000 : c.y1 - 1000};
}

void bed_tap(lv_obj_t* area, lv_point_t at) {
    auto& panel = get_global_motion_panel();
    panel.handle_bed_touch(helix::BedTouch::Pressed, area, at);
    panel.handle_bed_touch(helix::BedTouch::Released, area, at);
    MoveTabFixture::drain();
}

/// Script entries that travel in XY.
size_t xy_moves(const MoonrakerClientMock& client) {
    size_t n = 0;
    for (const auto& script : client.gcode_script_history()) {
        if (script.find("G0 X") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

std::string bed_readout() {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, "motion_bed_readout"));
}

/// At rest the readout shows no drag target (the header carries the position).
bool readout_at_rest() {
    return bed_readout().find("X ") == std::string::npos;
}

} // namespace

TEST_CASE_METHOD(MoveTabFixture, "bed tab: the plate fits its slot and the pills show",
                 "[motion][bed-tab][xml]") {
    CHECK(lv_obj_has_flag(panel_widget("bed_tab"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(panel_widget("motion_tab_bed"), LV_OBJ_FLAG_HIDDEN));

    get_global_motion_panel().set_motion_tab(2);
    drain();
    CHECK_FALSE(lv_obj_has_flag(panel_widget("bed_tab"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));

    // 350x350 travel: a square plate inside the surface.
    lv_obj_t* area = panel_widget("bed_map_area");
    lv_obj_t* plate = panel_widget("bed_map_plate");
    const int32_t w = lv_obj_get_width(plate);
    REQUIRE(w > 0);
    CHECK(std::abs(w - lv_obj_get_height(plate)) <= 1);
    CHECK(w <= lv_obj_get_width(area));
    CHECK(lv_obj_get_height(plate) <= lv_obj_get_height(area));
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a tap commits one move clamped to the plate",
                 "[motion][bed-tab]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    bed_tap(area, far_point(area, false, false));

    const std::string scripts = all_scripts();
    INFO(scripts);
    // Rear-left corner, inset by the clamp's edge margin; Z 10 needs no lift.
    CHECK(scripts.find("G0 X-174.979996 Y174.979996") != std::string::npos);
    CHECK(scripts.find('Z') == std::string::npos);
    CHECK(xy_moves(client_) == 1);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a tap lands on the point drawn under it",
                 "[motion][bed-tab]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_area_t plate;
    lv_obj_get_coords(panel_widget("bed_map_plate"), &plate);

    bed_tap(panel_widget("bed_map_area"), {(plate.x1 + plate.x2) / 2, (plate.y1 + plate.y2) / 2});

    double x = 0.0;
    double y = 0.0;
    for (const auto& script : client_.gcode_script_history()) {
        if (const auto at = script.find("G0 X"); at != std::string::npos) {
            REQUIRE(std::sscanf(script.c_str() + at, "G0 X%lf Y%lf", &x, &y) == 2);
        }
    }
    // The middle of the drawn plate is the middle of the bed, give or
    // take a pixel of rounding.
    CHECK(x == Catch::Approx(0.0).margin(1.5));
    CHECK(y == Catch::Approx(0.0).margin(1.5));
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: the head follows a drag and release adds nothing new",
                 "[motion][bed-tab]") {
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    drain();
    // Rear-right is under the finger, on screen and already on its way.
    CHECK(bed_readout().find("X 175.0  Y 175.0") != std::string::npos);
    CHECK(xy_moves(client_) == 1);

    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    // The finger resting there repeats PRESSING; it sends nothing more.
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    drain();
    CHECK(bed_readout().find("X 175.0  Y -175.0") != std::string::npos);
    INFO(all_scripts());
    CHECK(xy_moves(client_) == 2);
    CHECK(all_scripts().find(" Y-174.979996 ") != std::string::npos);

    // Released where the last target already went: no third move.
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, true));
    drain();
    CHECK(xy_moves(client_) == 2);
    CHECK(readout_at_rest());
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: release past the last streamed point sends it",
                 "[motion][bed-tab]") {
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    drain();
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, true));
    drain();
    INFO(all_scripts());
    CHECK(xy_moves(client_) == 2);
    CHECK(all_scripts().find(" Y-174.979996 ") != std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a drag from below clearance lifts first, to one height",
                 "[motion][bed-tab]") {
    auto& ps = get_printer_state();
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    ps.update_from_status({{"gcode_move", {{"gcode_position", {10.0, 10.0, 0.2, 0.0}}}}});
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    drain();
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    drain();
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, false, true));
    drain();

    // Every move of the gesture carries the one lift height decided at its
    // start, so a coalesced replacement can never drop it; the mock acks
    // without moving the commanded Z, which a per-move decision would misread.
    const std::string scripts = all_scripts();
    INFO(scripts);
    CHECK(xy_moves(client_) == 3);
    const auto lift = scripts.find("G0 Z5");
    REQUIRE(lift != std::string::npos);
    CHECK(lift < scripts.find("G0 X"));
    size_t z_lines = 0;
    for (size_t at = scripts.find("G0 Z"); at != std::string::npos;
         at = scripts.find("G0 Z", at + 1)) {
        CHECK(scripts.compare(at, 6, "G0 Z5 ") == 0);
        ++z_lines;
    }
    CHECK(z_lines == 3);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: below clearance the lift is announced, then done first",
                 "[motion][bed-tab]") {
    auto& ps = get_printer_state();
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    CHECK(bed_readout().find("lift") == std::string::npos);

    ps.update_from_status({{"gcode_move", {{"gcode_position", {10.0, 10.0, 0.2, 0.0}}}}});
    drain();
    // Announced before anything is tapped.
    CHECK(bed_readout().find("Z 0.20, will lift to 5mm") != std::string::npos);
    CHECK(client_.gcode_script_history().empty());

    lv_obj_t* area = panel_widget("bed_map_area");
    bed_tap(area, far_point(area, true, true));

    const std::string scripts = all_scripts();
    const auto lift = scripts.find("G0 Z5");
    INFO(scripts);
    const auto travel = scripts.find("G0 X174.9");
    REQUIRE(lift != std::string::npos);
    REQUIRE(travel != std::string::npos);
    CHECK(lift < travel);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: unhomed hides the marker and offers Home",
                 "[motion][bed-tab]") {
    auto& ps = get_printer_state();
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* marker = panel_widget("bed_map_marker");
    lv_obj_t* home = panel_widget("bed_map_home");
    CHECK_FALSE(lv_obj_has_flag(marker, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN));

    ps.update_from_status(ready_status("z"));
    drain();
    CHECK(lv_obj_has_flag(marker, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN));

    click(home);
    CHECK(all_scripts().find("G28") != std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a print disables the tab and the plate",
                 "[motion][bed-tab]") {
    auto& ps = get_printer_state();
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* pill = panel_widget("motion_tab_bed");
    lv_obj_t* area = panel_widget("bed_map_area");
    CHECK_FALSE(lv_obj_has_state(pill, LV_STATE_DISABLED));

    ps.update_from_status({{"print_stats", {{"state", "printing"}}}});
    drain();
    CHECK(lv_obj_has_state(pill, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(panel_widget("header_tab_bed"), LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(area, LV_STATE_DISABLED));

    // The backstop: a touch delivered anyway moves nothing.
    bed_tap(area, far_point(area, false, false));
    CHECK(all_scripts().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a tap on an unhomed plate sends nothing",
                 "[motion][bed-tab]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    get_printer_state().update_from_status(ready_status(""));
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");
    CHECK(lv_obj_has_state(area, LV_STATE_DISABLED));

    bed_tap(area, far_point(area, false, false));

    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: Cleared shows while a part may be on the bed",
                 "[motion][bed-tab][xml]") {
    ScopedPartOnBed part(38.0);
    // The panel's XML binds bed_occupied when it is built, so rebuild it with the
    // state's subjects registered, as the app does (the session creates them first).
    helix::ui::destroy_static_panels();
    drain();
    REQUIRE(get_global_motion_panel().show(lv_screen_active()));
    drain();
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_subject_t* occupied = lv_xml_get_subject(nullptr, "bed_occupied");
    REQUIRE(occupied != nullptr);
    lv_obj_t* cleared = panel_widget("bed_map_cleared");
    CHECK(lv_subject_get_int(occupied) == 1);
    CHECK_FALSE(lv_obj_has_flag(cleared, LV_OBJ_FLAG_HIDDEN));

    click(cleared);
    // A dismissed question clears nothing; only a yes does.
    helix::test::dismiss_top_modal();
    CHECK(lv_subject_get_int(occupied) == 1);
    click(cleared);
    helix::test::answer_top_modal("btn_primary");

    CHECK(lv_subject_get_int(occupied) == 0);
    CHECK(lv_obj_has_flag(cleared, LV_OBJ_FLAG_HIDDEN));
    // The lift falls back to the plain clearance (commanded Z 10 is above it).
    CHECK(bed_readout().find("last print") == std::string::npos);
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: each tap is its own gesture, even on the same spot",
                 "[motion][bed-tab]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    bed_tap(area, far_point(area, false, false));
    bed_tap(area, far_point(area, false, false));

    INFO(all_scripts());
    CHECK(xy_moves(client_) == 2);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: the XML wires a real pointer drag to the panel",
                 "[motion][bed-tab][xml]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");
    lv_area_t c;
    lv_obj_get_coords(area, &c);
    UITest::init(lv_screen_active());

    // Through LVGL's own input pipeline: a misspelled trigger or callback name
    // in motion_bed_map.xml leaves this drag moving nothing.
    UITest::press_at(c.x1 + 2, c.y1 + 2);
    UITest::press_at((c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2);
    drain();
    CHECK(bed_readout().find("X ") != std::string::npos);
    UITest::press_at(c.x2 - 2, c.y2 - 2);
    UITest::release();
    drain();
    UITest::cleanup();

    INFO(all_scripts());
    CHECK(xy_moves(client_) >= 2);
    CHECK(all_scripts().find(" Y-174.979996 ") != std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: the surface stays live while the toolhead reports busy",
                 "[motion][bed-tab]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    // idle_timeout reads Printing during any move, the drag's own included; a
    // surface disabled by it stops receiving the drag that caused it.
    get_printer_state().update_from_status({{"idle_timeout", {{"state", "Printing"}}}});
    drain();
    CHECK_FALSE(lv_obj_has_state(area, LV_STATE_DISABLED));
    get_printer_state().update_from_status({{"idle_timeout", {{"state", "Ready"}}}});
    drain();
}

TEST_CASE_METHOD(MoveTabFixture,
                 "bed tab: no gesture starts while another operation holds the head",
                 "[motion][bed-tab]") {
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    // A manual probe session holds the head absolutely: no recent app motion
    // can excuse it the way it excuses idle_timeout.
    lv_subject_t* probe = get_printer_state().calibration_state().get_manual_probe_active_subject();
    lv_subject_set_int(probe, 1);
    REQUIRE(get_printer_state().is_external_blocking_operation_active());
    auto& panel = get_global_motion_panel();
    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    drain();
    // The finger is not tracked at all: no target under it, nothing sent.
    CHECK(bed_readout().empty());
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, true));
    lv_subject_set_int(probe, 0);
    drain();

    CHECK(xy_moves(client_) == 0);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a drag sample replacing a pending one keeps the lift",
                 "[motion][bed-tab]") {
    HoldingAPI held_api{client_, get_printer_state()};
    set_moonraker_api(&held_api);
    auto& moves = held_api.holding_.held;
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    get_printer_state().update_from_status(
        {{"gcode_move", {{"gcode_position", {10.0, 10.0, 0.2, 0.0}}}}});
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    REQUIRE(moves.size() == 1); // in flight, carrying the lift
    REQUIRE(moves[0].target.z.has_value());
    // Two more samples while it is in flight: the second replaces the first
    // pending one wholesale.
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, false, true));
    REQUIRE(moves.size() == 1);

    moves[0].on_success();
    drain();
    REQUIRE(moves.size() == 2);
    // The flushed target is the latest sample, still at the gesture's lift Z.
    CHECK(moves[1].target.x == Catch::Approx(-174.98));
    REQUIRE(moves[1].target.z.has_value());
    CHECK(*moves[1].target.z == Catch::Approx(*moves[0].target.z));

    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, false, true));
    set_moonraker_api(&api_);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a sample queued behind a move dies with the gate",
                 "[motion][bed-tab]") {
    HoldingAPI held_api{client_, get_printer_state()};
    set_moonraker_api(&held_api);
    auto& moves = held_api.holding_.held;
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    REQUIRE(moves.size() == 1);

    lv_subject_set_int(get_printer_state().network_state().get_nav_buttons_enabled_subject(), 0);
    drain();
    moves[0].on_success();
    drain();
    CHECK(moves.size() == 1);

    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, true));
    set_moonraker_api(&api_);
}

TEST_CASE_METHOD(MoveTabFixture,
                 "bed tab: a queued sample dies when another operation takes the head",
                 "[motion][bed-tab]") {
    HoldingAPI held_api{client_, get_printer_state()};
    set_moonraker_api(&held_api);
    auto& moves = held_api.holding_.held;
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    REQUIRE(moves.size() == 1);

    // A probe session started elsewhere leaves nav enabled and motion unblocked, so only
    // the plate's own gate can stop the queued target.
    lv_subject_t* probe = get_printer_state().calibration_state().get_manual_probe_active_subject();
    lv_subject_set_int(probe, 1);
    REQUIRE(get_printer_state().is_external_blocking_operation_active());
    moves[0].on_success();
    drain();
    CHECK(moves.size() == 1);

    lv_subject_set_int(probe, 0);
    panel.handle_bed_touch(helix::BedTouch::Lost, area, far_point(area, true, true));
    set_moonraker_api(&api_);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: Z unhomed counts as unhomed, like Park",
                 "[motion][bed-tab]") {
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    get_printer_state().update_from_status(ready_status("xy"));
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");
    // With Z unknown no lift can be computed, so the map offers Home instead.
    CHECK(lv_obj_has_flag(panel_widget("bed_map_marker"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(panel_widget("bed_map_home"), LV_OBJ_FLAG_HIDDEN));

    // Neither a drag nor its release moves or homes anything.
    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, true));
    drain();
    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a tap lifts above a part still on the bed",
                 "[motion][bed-tab]") {
    ScopedPartOnBed part(38.0);
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");
    // The default 5mm clearance over a 38mm part.
    CHECK(bed_readout().find("will lift to 43mm: last print is 38mm tall") != std::string::npos);

    bed_tap(area, far_point(area, false, false));

    const std::string scripts = all_scripts();
    INFO(scripts);
    const auto lift = scripts.find("Z43");
    const auto travel = scripts.find("G0 X-174.97");
    REQUIRE(lift != std::string::npos);
    REQUIRE(travel != std::string::npos);
    CHECK(lift < travel);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a part too tall to clear refuses the move",
                 "[motion][bed-tab]") {
    // Z travel is 275mm; a 274.5mm part leaves under 1mm above it.
    ScopedPartOnBed part(274.5);
    get_global_motion_panel().set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");
    // Warned before any tap, as the home widget's plate warns.
    CHECK(bed_readout() == helix::part_too_tall_text());

    bed_tap(area, far_point(area, false, false));
    auto& panel = get_global_motion_panel();
    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, false));
    drain();

    INFO(all_scripts());
    CHECK(client_.gcode_script_history().empty());
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: a refused move ends the stream for that gesture",
                 "[motion][bed-tab]") {
    HoldingAPI held_api{client_, get_printer_state()};
    set_moonraker_api(&held_api);
    auto& moves = held_api.holding_.held;
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    REQUIRE(moves.size() == 1);
    MoonrakerError err;
    err.message = "Move out of range";
    moves[0].on_error(err);
    drain();

    // Every later sample would be refused the same way, one toast each.
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, true));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, false, true));
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, false, true));
    drain();
    CHECK(moves.size() == 1);

    // The next gesture starts clean.
    bed_tap(area, far_point(area, true, true));
    CHECK(moves.size() == 2);
    set_moonraker_api(&api_);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: leaving the panel ends the gesture in progress",
                 "[motion][bed-tab]") {
    auto& panel = get_global_motion_panel();
    panel.set_motion_tab(2);
    drain();
    lv_obj_t* area = panel_widget("bed_map_area");

    panel.handle_bed_touch(helix::BedTouch::Pressed, area, far_point(area, false, false));
    panel.handle_bed_touch(helix::BedTouch::Pressing, area, far_point(area, true, false));
    drain();
    REQUIRE(xy_moves(client_) == 1);
    REQUIRE(bed_readout().find("X ") != std::string::npos);

    panel.on_deactivating(DeactivateReason::NavigateAway);
    drain();
    CHECK(readout_at_rest());

    // A release the panel never saw the press for moves nothing.
    panel.handle_bed_touch(helix::BedTouch::Released, area, far_point(area, true, true));
    drain();
    CHECK(xy_moves(client_) == 1);
}

TEST_CASE_METHOD(MoveTabFixture, "motion: a Z frame landing before its ack is not counted twice",
                 "[motion][bed-tab]") {
    HoldingAPI held_api{client_, get_printer_state()};
    set_moonraker_api(&held_api);
    auto& motion = held_api.holding_;
    auto& panel = get_global_motion_panel();
    panel.set_jog_mode(helix::JogMode::Coarse);
    panel.set_motion_tab(2);
    get_printer_state().update_from_status(
        {{"gcode_move", {{"gcode_position", {10.0, 10.0, 12.0, 0.0}}}}});
    drain();

    // Z down 10. Klipper can report the new position before acking the G0
    // (a move that waits on a full lookahead buffer), so the frame comes first.
    REQUIRE(panel.handle_jog_button("z_down_large"));
    REQUIRE(motion.held_relative.size() == 1);
    REQUIRE(motion.held_relative[0].dz == Catch::Approx(-10.0));
    get_printer_state().update_from_status(
        {{"gcode_move", {{"gcode_position", {10.0, 10.0, 2.0, 0.0}}}}});
    drain();
    motion.held_relative[0].on_success();
    drain();

    // The head is at 2: the readout and every lift start from there.
    CHECK(bed_readout().find("Z 2.00, will lift to 5mm") != std::string::npos);
    panel.park_over_plate();
    REQUIRE(motion.held.size() == 1);
    REQUIRE(motion.held[0].target.z.has_value());
    CHECK(*motion.held[0].target.z == Catch::Approx(12.0));
    set_moonraker_api(&api_);
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: at rest the readout repeats no position",
                 "[motion][bed-tab]") {
    auto& ps = get_printer_state();
    get_global_motion_panel().set_motion_tab(2);
    drain();
    // The header shows where the head is; above clearance there is nothing to say.
    CHECK(bed_readout().empty());
    // The empty label still holds its line, so the plate does not jump.
    lv_obj_update_layout(lv_screen_active());
    CHECK(lv_obj_get_height(panel_widget("bed_map_readout")) > 0);

    ps.update_from_status({{"gcode_move", {{"gcode_position", {158.6, 104.0, 0.2, 0.0}}}}});
    drain();
    CHECK(bed_readout() == "Z 0.20, will lift to 5mm");

    ps.update_from_status(ready_status("xy"));
    drain();
    CHECK(bed_readout() == "Home all axes to use the map");
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: crosshair guides cross the plate through the marker",
                 "[motion][bed-tab][xml]") {
    get_global_motion_panel().set_motion_tab(2);
    get_printer_state().update_from_status(
        {{"gcode_move", {{"gcode_position", {60.0, 150.0, 10.0, 0.0}}}}});
    drain();
    lv_obj_update_layout(lv_screen_active());
    lv_area_t marker;
    lv_area_t plate;
    lv_area_t vertical;
    lv_area_t horizontal;
    lv_obj_get_coords(panel_widget("bed_map_marker"), &marker);
    lv_obj_get_content_coords(panel_widget("bed_map_plate"), &plate);
    lv_obj_t* guide_x = panel_widget("bed_map_guide_x");
    lv_obj_t* guide_y = panel_widget("bed_map_guide_y");
    lv_obj_get_coords(guide_x, &vertical);
    lv_obj_get_coords(guide_y, &horizontal);

    // Through the marker's centre...
    CHECK(std::abs((vertical.x1 + vertical.x2) / 2 - (marker.x1 + marker.x2) / 2) <= 1);
    CHECK(std::abs((horizontal.y1 + horizontal.y2) / 2 - (marker.y1 + marker.y2) / 2) <= 1);
    // ...edge to edge across the plate, and no further.
    CHECK(std::abs(vertical.y1 - plate.y1) <= 1);
    CHECK(std::abs(vertical.y2 - plate.y2) <= 1);
    CHECK(std::abs(horizontal.x1 - plate.x1) <= 1);
    CHECK(std::abs(horizontal.x2 - plate.x2) <= 1);

    get_printer_state().update_from_status(ready_status(""));
    drain();
    CHECK(lv_obj_has_flag(guide_x, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(guide_y, LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(MoveTabFixture, "bed tab: unhomed shows the jog pad's warning home button",
                 "[motion][bed-tab][xml]") {
    get_global_motion_panel().set_motion_tab(2);
    get_printer_state().update_from_status(ready_status(""));
    drain();
    lv_obj_t* home = panel_widget("bed_map_home");
    REQUIRE_FALSE(lv_obj_has_flag(home, LV_OBJ_FLAG_HIDDEN));
    const auto& palette = ThemeManager::instance().current_palette();
    CHECK(lv_color_eq(lv_obj_get_style_border_color(home, LV_PART_MAIN), palette.warning));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(home, LV_PART_MAIN), palette.elevated_bg));
    CHECK(lv_obj_get_style_border_width(home, LV_PART_MAIN) == 3);
    CHECK(lv_obj_get_width(home) == lv_obj_get_height(home));
}

TEST_CASE_METHOD(MoveTabFixture, "motion: the header title stays on one line",
                 "[motion][move-tab][xml]") {
    // Squeezed by the coordinates, a wrapping title broke mid-word ("MOTI" /
    // "ON"); clip measures it unwrapped, so it keeps one line at any width.
    lv_obj_t* title = panel_widget("header_title");
    CHECK(lv_label_get_long_mode(title) == LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(title, 20);
    lv_obj_update_layout(title);
    CHECK(lv_obj_get_height(title) ==
          lv_font_get_line_height(lv_obj_get_style_text_font(title, LV_PART_MAIN)));
}
