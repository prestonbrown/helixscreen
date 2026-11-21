// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The print status panel's printer tile and job tile, driven through printer
// state: what each chip and checklist subject publishes, and how the callout
// layout drops, glows and relayouts across a phase switch.

#include "ui_ams_mini_status.h"
#include "ui_print_status_tiles.h"
#include "ui_update_queue.h"

#include "../test_helpers/print_status_panel_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "../test_helpers/scoped_geometry.h"
#include "../test_helpers/scoped_portrait_layout.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_controller.h"
#include "prerendered_images.h"
#include "print_start_phase.h"
#include "print_status_chips.h"
#include "print_status_layout_fitter.h"
#include "printer_discovery.h"
#include "printer_image_manager.h"
#include "printer_state.h"
#include "status_dispatch.h"
#include "target_tool.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::PrintStatusTiles;
using helix::ui::UpdateQueue;
using print_status_panel_test::PrintStatusPanelFixture;

namespace {

/// A cache generation decodes and resizes a real image on a worker thread. A
/// sharded sweep keeps every core busy, so the wait is generous; it returns as
/// soon as the result lands.
constexpr uint32_t kGenerationBudgetMs = 30000;

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

std::string subject_str(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    const char* v = lv_subject_get_string(s);
    return v ? v : "";
}

/// A printer picture tagged with every region, so no chip docks for want of a point.
const ImageRegions& tagged_regions() {
    static const ImageRegions r = [] {
        ImageRegions x;
        x.src_w = 400;
        x.src_h = 400;
        x.nozzle = {0.5f, 0.35f};
        x.bed_left = {0.3f, 0.7f};
        x.bed_right = {0.7f, 0.7f};
        x.part_fan = NormPoint{0.55f, 0.4f};
        x.chamber = NormPoint{0.5f, 0.2f};
        x.light = NormPoint{0.2f, 0.1f};
        return x;
    }();
    return r;
}

/// A 400x400 printer picture at `path`, tagged with every region.
PrintStatusTiles::Image tagged_image(const std::string& path) {
    PrintStatusTiles::Image img;
    img.path = path;
    img.natural_w = 400;
    img.natural_h = 400;
    img.regions = &tagged_regions();
    return img;
}

/// The lane tray's ams_mini_status, which the app registers with AmsState
/// standing, a four-lane filament system for it to draw, and the gate
/// following AmsState as it does in the app. The returned token takes the
/// filament system away again.
std::shared_ptr<void> enable_lane_tray_widget() {
    auto& ams = AmsState::instance();
    ams.init_subjects();
    auto mock = std::make_unique<AmsBackendMock>(4);
    REQUIRE(mock->start().success());
    ams.set_backend(std::move(mock));
    ams.sync_from_backend();
    ui_ams_mini_status_init();
    helix::ui::TargetTool::instance().attach(get_printer_state());
    return std::shared_ptr<void>(nullptr, [](void*) { AmsState::instance().clear_backends(); });
}

/// The panel's own XML built at a screen size, with a tagged printer picture.
class TileFixture : public PrintStatusPanelFixture {
  public:
    TileFixture(int32_t screen_w, int32_t screen_h, bool lane_tray = false)
        : PrintStatusPanelFixture([screen_w, screen_h, lane_tray]() -> std::shared_ptr<void> {
              // The chips read the light and tool-badge subjects their owners publish.
              helix::led::LedController::instance().init(nullptr, nullptr);
              ToolState::instance().init_subjects(false);
              auto filament = lane_tray ? enable_lane_tray_widget() : nullptr;
              // The tile follows the image-changed subject, which exists
              // before any panel, as in the app.
              helix::PrinterImageManager::instance().init_subjects();
              auto geometry = std::make_shared<helix::test::ScopedGeometry>(screen_w, screen_h);
              return std::shared_ptr<void>(geometry.get(), [geometry, filament](void*) {});
          }) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        tile_ = lv_obj_find_by_name(root_, "ps_printer_tile");
        REQUIRE(tile_ != nullptr);
        tiles().set_image_source([]() { return tagged_image("A:test-printer.png"); });
        settle();
    }

    PrintStatusTiles& tiles() {
        return PrintStatusPanelTestAccess::tiles(panel());
    }

    /// Deliver queued observer fires, then run the update they scheduled.
    void settle() {
        UpdateQueue::instance().drain();
        lv_obj_update_layout(root_);
        UpdateQueue::instance().drain();
        lv_obj_update_layout(root_);
        tiles().update_now();
    }

    lv_obj_t* tile_ = nullptr;
};

struct Landscape800 : TileFixture {
    Landscape800() : TileFixture(800, 480) {}
};
struct Portrait320 : TileFixture {
    Portrait320() : TileFixture(320, 480) {}
};
struct Portrait272 : TileFixture {
    Portrait272() : TileFixture(272, 480) {}
};

/// The panel built from its ultrawide layout at 1480x320.
class Ultrawide1480 : public PrintStatusPanelFixture {
  public:
    explicit Ultrawide1480(bool lane_tray = false)
        : PrintStatusPanelFixture([lane_tray]() -> std::shared_ptr<void> {
              helix::led::LedController::instance().init(nullptr, nullptr);
              ToolState::instance().init_subjects(false);
              auto filament = lane_tray ? enable_lane_tray_widget() : nullptr;
              helix::PrinterImageManager::instance().init_subjects();
              auto layout =
                  std::make_shared<ScopedLayoutClass>("ultrawide", "ui_is_ultrawide", 1480, 320);
              REQUIRE(lv_xml_register_component_from_file(
                          "A:ui_xml/ultrawide/print_status_panel.xml") == LV_RESULT_OK);
              return std::shared_ptr<void>(layout.get(), [layout, filament](void*) {
                  lv_xml_register_component_from_file("A:ui_xml/print_status_panel.xml");
              });
          }) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(root_);
        PrintStatusPanelTestAccess::relayout(panel());
        UpdateQueue::instance().drain();
        lv_obj_update_layout(root_);
    }

    lv_obj_t* find(const char* name) {
        return lv_obj_find_by_name(root_, name);
    }
};

void set_int(lv_subject_t* s, int v) {
    lv_subject_set_int(s, v);
}

} // namespace

TEST_CASE_METHOD(Landscape800, "preparing shows checklist rows from collector input",
                 "[print_status]") {
    tiles().set_checklist_source([]() -> std::optional<PreprintChecklistInput> {
        PreprintChecklistInput in;
        in.done_in_order = {PrintStartPhase::HOMING};
        in.actual_seconds[static_cast<int>(PrintStartPhase::HOMING)] = 12;
        in.current = PrintStartPhase::HEATING_BED;
        in.current_elapsed_seconds = 30;
        in.expected_order = {PrintStartPhase::HOMING, PrintStartPhase::HEATING_BED,
                             PrintStartPhase::HEATING_NOZZLE, PrintStartPhase::BED_MESH};
        in.predicted_seconds[static_cast<int>(PrintStartPhase::HEATING_BED)] = 90;
        in.predicted_seconds[static_cast<int>(PrintStartPhase::BED_MESH)] = 120;
        return in;
    });
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_BED));
    UpdateQueue::instance().drain();

    CHECK(subject_int("ps_step_count") == 4);
    CHECK(subject_int("ps_step_0_status") == static_cast<int>(PreprintStepStatus::Done));
    CHECK(subject_str("ps_step_0_time") == "12s");
    CHECK(subject_int("ps_step_1_status") == static_cast<int>(PreprintStepStatus::Current));
    CHECK(subject_str("ps_step_1_time") == "1m");
    CHECK(subject_int("ps_step_2_status") == static_cast<int>(PreprintStepStatus::Upcoming));
    CHECK(subject_str("ps_step_2_time").empty()); // nothing predicts it
    CHECK(subject_str("ps_step_3_time") == "2m");
    CHECK(subject_str("ps_step_4_name").empty()); // past the count
    CHECK(subject_str("ps_step_position_text") == "Step 2 of 4");
    CHECK(subject_str("ps_step_current_name") == subject_str("ps_step_1_name"));
    CHECK_FALSE(subject_str("ps_step_current_name").empty());
}

TEST_CASE_METHOD(Landscape800, "printing hides checklist and shows preview", "[print_status]") {
    tiles().set_checklist_source([]() -> std::optional<PreprintChecklistInput> {
        PreprintChecklistInput in;
        in.current = PrintStartPhase::HOMING;
        return in;
    });
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HOMING));
    UpdateQueue::instance().drain();
    process_lvgl(900); // the preparing view shows after its debounce
    REQUIRE(subject_int("preparing_visible") == 1);
    REQUIRE(subject_int("ps_step_count") == 1);

    tiles().set_checklist_source(
        []() -> std::optional<PreprintChecklistInput> { return PreprintChecklistInput{}; });
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
    CHECK(subject_int("preparing_visible") == 0);
    CHECK(subject_int("ps_step_count") == 0);
    CHECK(subject_str("ps_step_position_text").empty());
}

TEST_CASE_METHOD(Landscape800, "phase switch relayouts and clears pre-print glows",
                 "[print_status]") {
    auto& temps = state().temperature_state();
    set_int(temps.get_active_extruder_temp_subject(), 1000);
    set_int(temps.get_active_extruder_target_subject(), 2200);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    settle();
    REQUIRE(subject_int("ps_chip_nozzle_shown") == 1);
    REQUIRE(subject_int("ps_glow_nozzle") == 1);
    REQUIRE(subject_int("ps_chip_nozzle_tone") == static_cast<int>(ChipTone::Active));
    const int layouts = tiles().layout_count();

    // The start sequence ends and its macro drops the preheat in the same batch.
    set_int(temps.get_active_extruder_target_subject(), 0);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
    process_lvgl(50); // the coalesced update runs on its own timer

    // One pass, and a second when the mode it picks differs from the last one.
    CHECK(tiles().layout_count() > layouts);
    CHECK(tiles().layout_count() <= layouts + 2);
    CHECK(subject_int("ps_glow_nozzle") == 0);
    CHECK(subject_int("ps_chip_nozzle_progress") == -1);
}

TEST_CASE_METHOD(Landscape800, "a heater chip paints every part from its heat state",
                 "[print_status]") {
    using namespace helix::ui::temperature;
    auto& temps = state().temperature_state();
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_bed");
    REQUIRE(chip != nullptr);
    lv_obj_t* glyph = lv_obj_find_by_name(chip, "bed_icon_glyph");
    REQUIRE(glyph != nullptr);
    lv_obj_t* text = lv_obj_find_by_name(chip, "chip_text");
    lv_obj_t* bar = lv_obj_find_by_name(chip, "chip_progress");
    lv_obj_t* glow = lv_obj_find_by_name(root_, "ps_glow_bed");
    const auto same = [](lv_color_t a, HeatState s) {
        return lv_color_eq(a, get_heating_state_color(s));
    };

    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_BED));
    set_int(temps.get_bed_target_subject(), 600);
    set_int(temps.get_bed_temp_subject(), 300);
    settle();
    REQUIRE(subject_int("ps_chip_bed_tone") == static_cast<int>(ChipTone::Active));
    CHECK(same(lv_obj_get_style_text_color(glyph, LV_PART_MAIN), HeatState::Heating));
    CHECK(same(lv_obj_get_style_text_color(text, LV_PART_MAIN), HeatState::Heating));
    CHECK(same(lv_obj_get_style_border_color(chip, LV_PART_MAIN), HeatState::Heating));
    CHECK(same(lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR), HeatState::Heating));
    CHECK(same(lv_obj_get_style_bg_color(glow, LV_PART_MAIN), HeatState::Heating));
    CHECK(subject_int("ps_glow_bed") == 1);

    set_int(temps.get_bed_temp_subject(), 900);
    settle();
    CHECK(same(lv_obj_get_style_text_color(glyph, LV_PART_MAIN), HeatState::Cooling));
    CHECK(same(lv_obj_get_style_text_color(text, LV_PART_MAIN), HeatState::Cooling));
    CHECK(same(lv_obj_get_style_border_color(chip, LV_PART_MAIN), HeatState::Cooling));
    CHECK(subject_int("ps_glow_bed") == 0);

    set_int(temps.get_bed_temp_subject(), 600);
    settle();
    CHECK(same(lv_obj_get_style_text_color(text, LV_PART_MAIN), HeatState::AtTemp));
    // Holding, the outline goes back to the plain pill.
    CHECK_FALSE(same(lv_obj_get_style_border_color(chip, LV_PART_MAIN), HeatState::AtTemp));
    CHECK_FALSE(same(lv_obj_get_style_border_color(chip, LV_PART_MAIN), HeatState::Heating));

    // A rebuilt tree starts unpainted, however unchanged the reading is.
    lv_obj_remove_local_style_prop(text, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
    tiles().on_tree_destroyed();
    settle();
    CHECK(same(lv_obj_get_style_text_color(text, LV_PART_MAIN), HeatState::AtTemp));

    // Each of the other heater chips has its own binder tinting its own glyph.
    const struct {
        const char* glyph;
        lv_subject_t* temp;
        lv_subject_t* target;
    } others[] = {{"nozzle_icon_glyph", temps.get_active_extruder_temp_subject(),
                   temps.get_active_extruder_target_subject()},
                  {"chamber_icon_glyph", temps.get_chamber_temp_subject(),
                   temps.get_chamber_effective_target_subject()}};
    for (const auto& o : others) {
        INFO(o.glyph);
        set_int(o.target, 600);
        set_int(o.temp, 300);
        settle();
        lv_obj_t* other = lv_obj_find_by_name(root_, o.glyph);
        REQUIRE(other != nullptr);
        CHECK(same(lv_obj_get_style_text_color(other, LV_PART_MAIN), HeatState::Heating));
    }
}

TEST_CASE_METHOD(Landscape800, "a heater off target mid-print keeps the amber of Warn",
                 "[print_status]") {
    using namespace helix::ui::temperature;
    auto& temps = state().temperature_state();
    set_int(temps.get_bed_target_subject(), 600);
    set_int(temps.get_bed_temp_subject(), 300);
    settle();
    REQUIRE(subject_int("ps_chip_bed_tone") == static_cast<int>(ChipTone::Warn));
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_bed");
    REQUIRE(chip != nullptr);
    const lv_color_t amber = theme_manager_get_color("warning");
    CHECK(lv_color_eq(
        lv_obj_get_style_text_color(lv_obj_find_by_name(chip, "chip_text"), LV_PART_MAIN), amber));
    CHECK(lv_color_eq(lv_obj_get_style_border_color(chip, LV_PART_MAIN), amber));
    // The icon and the bar still say what the heater is doing.
    CHECK(lv_color_eq(
        lv_obj_get_style_bg_color(lv_obj_find_by_name(chip, "chip_progress"), LV_PART_INDICATOR),
        get_heating_state_color(HeatState::Heating)));
}

TEST_CASE_METHOD(Landscape800, "a heater at its target shows the check", "[print_status]") {
    auto& temps = state().temperature_state();
    set_int(temps.get_bed_target_subject(), 600);
    set_int(temps.get_bed_temp_subject(), 600);
    settle();
    REQUIRE(subject_int("ps_chip_bed_shown") == 1);
    CHECK(subject_int("ps_chip_bed_state") ==
          static_cast<int>(helix::ui::temperature::HeaterStatusState::Ready));
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_bed");
    REQUIRE(chip != nullptr);
    lv_obj_t* check = lv_obj_find_by_name(chip, "status_ready");
    lv_obj_t* flame = lv_obj_find_by_name(chip, "status_heating");
    REQUIRE(check != nullptr);
    REQUIRE(flame != nullptr);
    CHECK_FALSE(lv_obj_has_flag(check, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(flame, LV_OBJ_FLAG_HIDDEN));
    // The duty is in the chip's own text; the status row adds only the glyph,
    // which is all the chip's width reserves for it.
    lv_obj_t* duty = lv_obj_find_by_name(chip, "status_duty");
    REQUIRE(duty != nullptr);
    CHECK(std::string(lv_label_get_text(duty)).empty());
    // A chip with no heater shows no glyph.
    lv_obj_t* fan = lv_obj_find_by_name(root_, "ps_chip_part_fan");
    REQUIRE(fan != nullptr);
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(fan, "status_ready"), LV_OBJ_FLAG_HIDDEN));
    set_int(temps.get_bed_target_subject(), 0);
    set_int(temps.get_bed_temp_subject(), 250);
}

TEST_CASE_METHOD(Landscape800, "the printer tile follows a change of printer image",
                 "[print_status]") {
    auto& images = helix::PrinterImageManager::instance();
    tiles().use_displayed_image();
    settle();
    const std::string before = subject_str("ps_printer_image_src");
    images.set_active_image("shipped:qidi-q2");
    settle();
    CHECK(subject_str("ps_printer_image_src").find("qidi-q2") != std::string::npos);
    images.set_active_image("");
    settle();
    CHECK(subject_str("ps_printer_image_src") == before);
}

TEST_CASE_METHOD(Landscape800, "chips lay out again when the printer tile resizes",
                 "[print_status]") {
    settle();
    const int before = tiles().layout_count();
    lv_obj_set_height(tile_, lv_obj_get_height(tile_) - 30);
    lv_obj_update_layout(root_);
    UpdateQueue::instance().drain();
    process_lvgl(50); // the chip update is coalesced onto a timer
    CHECK(tiles().layout_count() > before);
}

TEST_CASE_METHOD(Landscape800, "the speed_flow chip shows only during a job, Warn at speed 104",
                 "[print_status]") {
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    settle();
    CHECK(subject_int("ps_chip_speed_flow_shown") == 0);
    state().update_from_status({{"print_stats", {{"state", "printing"}}}});
    settle();
    REQUIRE(subject_int("ps_chip_speed_flow_shown") == 1);
    REQUIRE(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Normal));

    set_int(state().motion_state().get_speed_factor_subject(), 104);
    settle();
    CHECK(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Warn));
    CHECK(subject_int("ps_chip_speed_flow_shown") == 1);
    // The factor is Tune's to show; the chip keeps the live rates.
    CHECK(subject_str("ps_chip_speed_flow_text") == "0 mm/s\n0.0 mm\xC2\xB3/s");
}

namespace {

/// Spin the part fan, stop the aux fan, and lay the tile out.
void fans_part_spinning_aux_stopped(TileFixture& fx) {
    auto& fans = PrinterStateTestAccess::get_fan_state(fx.state());
    fans.init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux"});
    fans.update_fan_speed("fan", 0.5);
    fans.update_fan_speed("fan_generic aux", 0.0);
    fx.settle();
    fx.settle(); // a mode change reschedules once
}

} // namespace

TEST_CASE_METHOD(Portrait272, "the smallest tile keeps the unusual chips and dims a stopped fan",
                 "[print_status]") {
    state().update_from_status({{"print_stats", {{"state", "printing"}}}});
    fans_part_spinning_aux_stopped(*this);

    // Which expendable chip a tight tile drops first is the engine's rule
    // ([callout_layout] "hotend fan drops first when chips do not fit").
    REQUIRE(subject_int("ps_callout_mode") == static_cast<int>(CalloutMode::Staggered));
    CHECK(subject_int("ps_chip_part_fan_shown") == 1);
    CHECK(subject_int("ps_chip_speed_flow_shown") == 1);
    if (subject_int("ps_chip_aux_fan_shown") == 1) {
        CHECK(subject_int("ps_chip_aux_fan_tone") == static_cast<int>(ChipTone::Dim));
    }
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Portrait320, "fan at 0 stays, dimmed, while the tile has room for it",
                 "[print_status]") {
    fans_part_spinning_aux_stopped(*this);

    REQUIRE(subject_int("ps_callout_mode") != static_cast<int>(CalloutMode::ImageOnly));
    CHECK(subject_int("ps_chip_aux_fan_shown") == 1);
    CHECK(subject_int("ps_chip_aux_fan_tone") == static_cast<int>(ChipTone::Dim));
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Landscape800, "outcome shows reprint and keeps preview", "[print_status]") {
    lv_obj_t* reprint = lv_obj_find_by_name(root_, "btn_reprint");
    lv_obj_t* cancel = lv_obj_find_by_name(root_, "btn_cancel");
    REQUIRE(reprint != nullptr);
    REQUIRE(cancel != nullptr);
    set_int(state().print_state().get_print_outcome_subject(),
            static_cast<int>(PrintOutcome::COMPLETE));
    UpdateQueue::instance().drain();

    CHECK_FALSE(lv_obj_has_flag(reprint, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(cancel, LV_OBJ_FLAG_HIDDEN));
    CHECK(subject_int("preparing_visible") == 0); // the job tile stays the preview
    set_int(state().print_state().get_print_outcome_subject(),
            static_cast<int>(PrintOutcome::NONE));
}

TEST_CASE_METHOD(Landscape800, "more menu lists only available actions", "[print_status]") {
    lv_subject_t* webcam = lv_xml_get_subject(nullptr, "printer_has_webcam");
    lv_subject_t* leds = lv_xml_get_subject(nullptr, "led_controllable");
    lv_subject_t* timelapse = lv_xml_get_subject(nullptr, "timelapse_owner");
    REQUIRE(webcam != nullptr);
    REQUIRE(leds != nullptr);
    REQUIRE(timelapse != nullptr);
    set_int(webcam, 0);
    set_int(timelapse, 0);
    set_int(leds, 1);

    lv_obj_t* anchor = lv_obj_create(test_screen());
    PrintStatusPanelTestAccess::open_more_menu(panel(), anchor);
    auto& menu = PrintStatusPanelTestAccess::more_menu(panel());
    REQUIRE(menu.is_visible());
    lv_obj_t* camera = lv_obj_find_by_name(test_screen(), "btn_more_camera");
    lv_obj_t* light = lv_obj_find_by_name(test_screen(), "btn_more_light");
    lv_obj_t* tl = lv_obj_find_by_name(test_screen(), "btn_more_timelapse");
    REQUIRE(camera != nullptr);
    REQUIRE(light != nullptr);
    REQUIRE(tl != nullptr);
    CHECK(lv_obj_has_flag(camera, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(tl, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(light, LV_OBJ_FLAG_HIDDEN));
    menu.hide();
    set_int(leds, 0);
}

TEST_CASE_METHOD(Landscape800, "nozzle chip shows active tool on a toolchanger", "[print_status]") {
    auto& tools = ToolState::instance();
    lv_subject_copy_string(tools.get_tool_badge_text_subject(), "T1");
    set_int(tools.get_show_tool_badge_subject(), 1);
    auto& temps = state().temperature_state();
    set_int(temps.get_active_extruder_temp_subject(), 2100);
    set_int(temps.get_active_extruder_target_subject(), 2100);
    settle();

    CHECK(subject_str("ps_chip_nozzle_text").rfind("T1 ", 0) == 0);

    set_int(tools.get_show_tool_badge_subject(), 0);
    lv_subject_copy_string(tools.get_tool_badge_text_subject(), "");
    settle();
    CHECK(subject_str("ps_chip_nozzle_text").rfind("T1", 0) == std::string::npos);
}

namespace {

/// klipper-toolchanger with a part fan and a hotend fan on every tool, and the
/// wizard's part and hotend roles left on T0's, as a real printer had them.
PrinterFanState& load_toolchanger_with_tool_fans(PrinterState& ps) {
    PrinterDiscovery hw;
    hw.parse_objects(nlohmann::json::array({"toolhead", "extruder", "extruder1", "extruder2",
                                            "toolchanger", "tool T0", "tool T1", "tool T2"}));
    ToolState::instance().init_tools(hw);
    ps.temperature_state().init_extruders({"extruder", "extruder1", "extruder2"});
    auto& fans = PrinterStateTestAccess::get_fan_state(ps);
    FanRoleConfig roles;
    roles.part_fan = "fan_generic T0_part_fan";
    roles.hotend_fan = "heater_fan T0_hotend_fan";
    fans.init_fans({"fan_generic T0_part_fan", "heater_fan T0_hotend_fan",
                    "fan_generic T1_part_fan", "heater_fan T1_hotend_fan",
                    "fan_generic T2_part_fan", "heater_fan T2_hotend_fan",
                    "heater_fan nevermore_fan", "temperature_fan _leviathan_mcu_fans"},
                   roles);
    // configfile.settings keys sections lowercased.
    fans.set_heater_fans(fan_heaters::map_from_config(
        nlohmann::json{{"heater_fan t0_hotend_fan", {{"heater", "extruder"}}},
                       {"heater_fan t1_hotend_fan", {{"heater", "extruder1"}}},
                       {"heater_fan t2_hotend_fan", {{"heater", "extruder2"}}},
                       {"heater_fan nevermore_fan", {{"heater", "heater_bed"}}}}));
    return fans;
}

void unload_toolchanger(PrinterState& ps) {
    auto& fans = PrinterStateTestAccess::get_fan_state(ps);
    fans.set_active_tool_fans("", "");
    fans.set_tool_fans({}, {});
    fans.set_heater_fans({});
    fans.init_fans({});
    ps.temperature_state().init_extruders({"extruder"});
    ToolState::instance().init_tools(PrinterDiscovery{});
}

/// That printer's status frame with T2 mounted.
nlohmann::json t2_mounted_frame() {
    return nlohmann::json{{"toolhead", {{"extruder", "extruder2"}}},
                          {"toolchanger", {{"tool_number", 2}}},
                          {"tool T0", {{"extruder", "extruder"}, {"fan", "T0_part_fan"}}},
                          {"tool T1", {{"extruder", "extruder1"}, {"fan", "T1_part_fan"}}},
                          {"tool T2", {{"extruder", "extruder2"}, {"fan", "T2_part_fan"}}},
                          {"fan_generic T0_part_fan", {{"speed", 0.2}}},
                          {"fan_generic T1_part_fan", {{"speed", 0.5}}},
                          {"fan_generic T2_part_fan", {{"speed", 0.7}}},
                          {"heater_fan T0_hotend_fan", {{"speed", 1.0}}},
                          {"heater_fan T2_hotend_fan", {{"speed", 0.4}}}};
}

} // namespace

TEST_CASE_METHOD(Landscape800, "fan chips follow the active tool's own fans on a toolchanger",
                 "[print_status][fan_state][tool_state]") {
    auto& fans = load_toolchanger_with_tool_fans(state());

    dispatch_status(t2_mounted_frame());
    settle();
    settle();

    CHECK(fans.classify_primary_fans().part == "fan_generic T2_part_fan");
    CHECK(fans.classify_primary_fans().hotend == "heater_fan T2_hotend_fan");
    // The home printer picture's fan chip reads the main fan speed.
    CHECK(lv_subject_get_int(fans.get_fan_speed_subject()) == 70);
    CHECK(subject_str("ps_chip_part_fan_text").find("70%") != std::string::npos);
    CHECK(subject_str("ps_chip_hotend_fan_text").find("40%") != std::string::npos);

    // Back to T0: the chips go with it.
    dispatch_status(nlohmann::json{{"toolhead", {{"extruder", "extruder"}}},
                                   {"toolchanger", {{"tool_number", 0}}}});
    settle();
    settle();
    CHECK(lv_subject_get_int(fans.get_fan_speed_subject()) == 20);
    CHECK(subject_str("ps_chip_part_fan_text").find("20%") != std::string::npos);
    CHECK(subject_str("ps_chip_hotend_fan_text").find("100%") != std::string::npos);

    unload_toolchanger(state());
}

TEST_CASE_METHOD(Landscape800, "the aux chip shows no tool's own fan on a toolchanger",
                 "[print_status][fan_state][tool_state]") {
    // T1's part fan has run, so it is the commandable fan the aux slot would prefer.
    auto& fans = load_toolchanger_with_tool_fans(state());
    dispatch_status(t2_mounted_frame());
    settle();
    settle();

    const std::string aux = fans.classify_primary_fans().aux;
    CHECK(aux.find("_part_fan") == std::string::npos);
    CHECK(aux.find("_hotend_fan") == std::string::npos);
    CHECK(aux == "temperature_fan _leviathan_mcu_fans");

    unload_toolchanger(state());
}

namespace {

bool hidden(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Two steps done of five, the third under way.
std::optional<PreprintChecklistInput> five_steps() {
    PreprintChecklistInput in;
    in.done_in_order = {PrintStartPhase::HOMING, PrintStartPhase::HEATING_BED};
    in.current = PrintStartPhase::HEATING_NOZZLE;
    in.expected_order = {PrintStartPhase::HOMING, PrintStartPhase::HEATING_BED,
                         PrintStartPhase::HEATING_NOZZLE, PrintStartPhase::QGL,
                         PrintStartPhase::BED_MESH};
    return in;
}

} // namespace

TEST_CASE_METHOD(Landscape800, "the checklist shows a row per step and none past them",
                 "[print_status]") {
    tiles().set_checklist_source(five_steps);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    UpdateQueue::instance().drain();
    process_lvgl(900); // the preparing view shows after its debounce
    REQUIRE(subject_int("preparing_visible") == 1);
    REQUIRE(subject_int("ps_step_count") == 5);
    REQUIRE(subject_int("ps_checklist_compact") == 0);

    CHECK_FALSE(hidden(root_, "ps_job_checklist"));
    CHECK_FALSE(hidden(root_, "ps_checklist_list"));
    CHECK(hidden(root_, "ps_checklist_compact"));
    CHECK_FALSE(hidden(root_, "ps_step_row_4"));
    CHECK(hidden(root_, "ps_step_row_5"));
    CHECK(hidden(root_, "ps_step_row_7"));
    // The pre-print strip line replaces the printing one.
    CHECK_FALSE(hidden(root_, "ps_strip_prepare_line"));
    CHECK(hidden(root_, "ps_strip_print_line"));

    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
    REQUIRE(subject_int("preparing_visible") == 0);
    CHECK(hidden(root_, "ps_job_checklist"));
    CHECK_FALSE(hidden(root_, "ps_strip_print_line"));
}

TEST_CASE_METHOD(Portrait272, "a narrow job tile shows the current step compactly",
                 "[print_status]") {
    tiles().set_checklist_source(five_steps);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HEATING_NOZZLE));
    UpdateQueue::instance().drain();
    process_lvgl(900);
    REQUIRE(subject_int("ps_checklist_compact") == 1);
    CHECK(hidden(root_, "ps_checklist_list"));
    CHECK_FALSE(hidden(root_, "ps_checklist_compact"));
    lv_obj_t* current = lv_obj_find_by_name(root_, "ps_checklist_current");
    REQUIRE(current != nullptr);
    CHECK(std::string(lv_label_get_text(current)) == subject_str("ps_step_current_name"));
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(Landscape800, "More hides when its menu has nothing to offer", "[print_status]") {
    lv_subject_t* webcam = lv_xml_get_subject(nullptr, "printer_has_webcam");
    lv_subject_t* leds = lv_xml_get_subject(nullptr, "led_controllable");
    lv_subject_t* timelapse = lv_xml_get_subject(nullptr, "timelapse_owner");
    REQUIRE(webcam != nullptr);
    REQUIRE(leds != nullptr);
    REQUIRE(timelapse != nullptr);
    set_int(webcam, 0);
    set_int(timelapse, 0);
    set_int(leds, 0);
    CHECK(hidden(root_, "btn_more"));
    set_int(timelapse, 1);
    CHECK_FALSE(hidden(root_, "btn_more"));
    set_int(timelapse, 0);
    set_int(leds, 1);
    CHECK_FALSE(hidden(root_, "btn_more"));
    set_int(leds, 0);

    // A webcam on a remote screen offers the Camera entry, unless the build has no camera.
    lv_subject_t* camera = lv_xml_get_subject(nullptr, "print_status_camera_available");
    REQUIRE(camera != nullptr);
    set_int(webcam, 1);
    state().set_moonraker_is_remote(true);
    UpdateQueue::instance().drain(); // the deferred set, then the observer it fires
    UpdateQueue::instance().drain();
    CHECK(subject_int("print_status_camera_available") == HELIX_HAS_CAMERA);
    set_int(camera, 1);
    CHECK_FALSE(hidden(root_, "btn_more"));
    set_int(camera, 0);
    CHECK(hidden(root_, "btn_more"));
    set_int(webcam, 0);
    state().set_moonraker_is_remote(false);
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(Landscape800, "strip buttons are square beside the progress", "[print_status]") {
    REQUIRE(subject_int("ps_strip_stacked") == 0);
    lv_obj_t* strip = lv_obj_find_by_name(root_, "ps_strip");
    lv_obj_t* progress = lv_obj_find_by_name(root_, "ps_strip_progress");
    lv_obj_t* pause = lv_obj_find_by_name(root_, "btn_pause");
    REQUIRE(strip != nullptr);
    REQUIRE(progress != nullptr);
    REQUIRE(pause != nullptr);
    const int32_t side = theme_manager_get_spacing("ps_strip_button");
    // As tall as the strip, which is a square button's height at the least.
    CHECK(lv_obj_get_height(pause) >= side);
    CHECK(lv_obj_get_height(pause) == lv_obj_get_height(progress));
    CHECK(lv_obj_get_width(pause) <= side);
    CHECK(lv_obj_get_width(pause) >= side - theme_manager_get_spacing("space_sm"));
    // The progress keeps at least 45% of the strip.
    CHECK(lv_obj_get_width(progress) * 100 >= lv_obj_get_content_width(strip) * 40);
    CHECK(lv_obj_get_height(strip) ==
          helix::ui::PrintStatusLayoutFitter::strip_row_height(progress));
    // The fitter's estimate before layout reads the bar's height from the same token.
    lv_obj_t* bar = lv_obj_find_by_name(root_, "print_progress");
    REQUIRE(bar != nullptr);
    CHECK(lv_obj_get_height(bar) == theme_manager_get_spacing("progress_bar_height"));
}

TEST_CASE_METHOD(Portrait320, "a narrow strip stacks progress over a full row of buttons",
                 "[print_status]") {
    REQUIRE(subject_int("ps_strip_stacked") == 1);
    lv_obj_t* buttons = lv_obj_find_by_name(root_, "ps_strip_buttons");
    lv_obj_t* progress = lv_obj_find_by_name(root_, "ps_strip_progress");
    lv_obj_t* strip = lv_obj_find_by_name(root_, "ps_strip");
    REQUIRE(buttons != nullptr);
    REQUIRE(progress != nullptr);
    CHECK(lv_obj_get_width(buttons) == lv_obj_get_content_width(strip));
    CHECK(lv_obj_get_y(buttons) > lv_obj_get_y(progress));
    CHECK(lv_obj_get_height(buttons) == theme_manager_get_spacing("button_height"));
}

TEST_CASE_METHOD(Landscape800, "a tuned speed turns the speed/flow chip amber", "[print_status]") {
    state().update_from_status({{"print_stats", {{"state", "printing"}}}});
    set_int(state().motion_state().get_speed_factor_subject(), 104);
    settle();
    REQUIRE(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Warn));
    lv_obj_t* chip = lv_obj_find_by_name(root_, "ps_chip_speed_flow");
    REQUIRE(chip != nullptr);
    const lv_color_t warn = theme_manager_get_color("warning");
    CHECK(lv_color_eq(lv_obj_get_style_border_color(chip, LV_PART_MAIN), warn));
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    settle();
    CHECK_FALSE(lv_color_eq(lv_obj_get_style_border_color(chip, LV_PART_MAIN), warn));
}

namespace {

/// Every chip the tile can show at once: three heaters, three fans, the light
/// and the two-line speed/flow chip.
void all_eight_chips(TileFixture& fx) {
    auto& temps = fx.state().temperature_state();
    set_int(fx.state().capabilities_state().subject(Capability::HasChamber), 1);
    set_int(temps.get_active_extruder_temp_subject(), 2200);
    set_int(temps.get_active_extruder_target_subject(), 2200);
    set_int(temps.get_bed_temp_subject(), 600);
    set_int(temps.get_bed_target_subject(), 600);
    set_int(temps.get_chamber_temp_subject(), 450);
    set_int(temps.get_chamber_effective_target_subject(), 450);
    set_int(lv_xml_get_subject(nullptr, "led_controllable"), 1);
    auto& fans = PrinterStateTestAccess::get_fan_state(fx.state());
    fans.init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux"});
    fans.update_fan_speed("fan", 0.5);
    fans.update_fan_speed("heater_fan hotend_fan", 1.0);
    fans.update_fan_speed("fan_generic aux", 0.0);
    fx.settle();
    fx.settle(); // a mode change reschedules once
}

void clear_all_eight_chips(TileFixture& fx) {
    auto& temps = fx.state().temperature_state();
    set_int(fx.state().capabilities_state().subject(Capability::HasChamber), 0);
    set_int(temps.get_active_extruder_target_subject(), 0);
    set_int(temps.get_bed_target_subject(), 0);
    set_int(temps.get_chamber_effective_target_subject(), 0);
    set_int(lv_xml_get_subject(nullptr, "led_controllable"), 0);
    PrinterStateTestAccess::get_fan_state(fx.state()).init_fans({});
}

/// No chip on screen runs past the printer area's content box or onto another.
void check_chips_inside_and_apart(lv_obj_t* root) {
    lv_obj_update_layout(root);
    lv_obj_t* area = lv_obj_find_by_name(root, "ps_printer_area");
    REQUIRE(area != nullptr);
    lv_area_t box;
    lv_obj_get_content_coords(area, &box);
    std::vector<std::pair<std::string, lv_area_t>> shown;
    for (const char* name : {"ps_chip_nozzle", "ps_chip_bed", "ps_chip_chamber", "ps_chip_part_fan",
                             "ps_chip_hotend_fan", "ps_chip_aux_fan", "ps_chip_light",
                             "ps_chip_speed_flow", "ps_chip_toolhead"}) {
        lv_obj_t* chip = lv_obj_find_by_name(root, name);
        if (!chip || lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN))
            continue;
        lv_area_t a;
        lv_obj_get_coords(chip, &a);
        INFO(name << " at " << a.x1 << "," << a.y1 << " to " << a.x2 << "," << a.y2 << " in "
                  << box.x1 << "," << box.y1 << " to " << box.x2 << "," << box.y2);
        CHECK(a.x1 >= box.x1);
        CHECK(a.y1 >= box.y1);
        CHECK(a.x2 <= box.x2);
        CHECK(a.y2 <= box.y2);
        shown.emplace_back(name, a);
    }
    for (size_t i = 0; i < shown.size(); ++i)
        for (size_t j = i + 1; j < shown.size(); ++j) {
            const auto& a = shown[i].second;
            const auto& b = shown[j].second;
            INFO(shown[i].first << " vs " << shown[j].first);
            CHECK_FALSE((a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2));
        }
}

} // namespace

TEST_CASE_METHOD(Landscape800, "the two-line speed/flow chip lays out at its own height",
                 "[print_status]") {
    state().update_from_status({{"print_stats", {{"state", "printing"}}}});
    settle();
    lv_obj_update_layout(root_);
    lv_obj_t* speed = lv_obj_find_by_name(root_, "ps_chip_speed_flow");
    lv_obj_t* bed = lv_obj_find_by_name(root_, "ps_chip_bed");
    REQUIRE(speed != nullptr);
    REQUIRE(bed != nullptr);
    REQUIRE(subject_int("ps_chip_speed_flow_shown") == 1);
    REQUIRE(lv_obj_get_height(speed) > lv_obj_get_height(bed));
    // The layout placed it at the height it renders, so the chips around it
    // were spaced for that height.
    const CalloutChipOut* placed = nullptr;
    for (const auto& c : tiles().last_layout().chips)
        if (c.kind == CalloutKind::SpeedFlow)
            placed = &c;
    REQUIRE(placed != nullptr);
    CHECK(placed->rect.h == lv_obj_get_height(speed));
    check_chips_inside_and_apart(root_);
}

TEST_CASE_METHOD(Landscape800, "every chip at once at 800x480: none clips or overlaps",
                 "[print_status]") {
    all_eight_chips(*this);
    REQUIRE(subject_int("ps_chip_chamber_shown") == 1);
    CHECK(subject_int("ps_callout_mode") != static_cast<int>(CalloutMode::ImageOnly));
    check_chips_inside_and_apart(root_);
    clear_all_eight_chips(*this);
}

namespace {

/// A checklist whose step count the test sets, so each re-read of it shows
/// in ps_step_count.
std::function<std::optional<PreprintChecklistInput>()> steps_source(const size_t& steps) {
    return [&steps]() -> std::optional<PreprintChecklistInput> {
        static const std::vector<PrintStartPhase> kOrder{
            PrintStartPhase::HOMING, PrintStartPhase::BED_MESH, PrintStartPhase::PURGING};
        PreprintChecklistInput in;
        in.current = PrintStartPhase::HOMING;
        in.expected_order.assign(kOrder.begin(), kOrder.begin() + steps);
        return in;
    };
}

/// The printer picture's rect, which moves whenever the callout mode does.
CalloutRect image_rect(lv_obj_t* root) {
    lv_obj_update_layout(root);
    lv_obj_t* img = lv_obj_find_by_name(root, "ps_printer_image");
    REQUIRE(img != nullptr);
    return {lv_obj_get_x(img), lv_obj_get_y(img), lv_obj_get_width(img), lv_obj_get_height(img)};
}

bool same_rect(const CalloutRect& a, const CalloutRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/// Sizes the printer area directly. Measured with the stock chips: at 200x280
/// a stopped part fan or an untuned speed fits BothSides while a spinning fan
/// or a tune needs Pinned, so a budget that followed live flags would flip the
/// mode there; at 320x208 the line mode sheds the aux fan's room.
void size_printer_area(lv_obj_t* root, int w, int h) {
    lv_obj_t* area = lv_obj_find_by_name(root, "ps_printer_area");
    REQUIRE(area != nullptr);
    lv_obj_set_size(area, w, h);
}

/// Three fans, all stopped. The part fan has run once, so it keeps the part
/// slot when the aux fan spins.
void three_fans_stopped(TileFixture& fx) {
    auto& fans = PrinterStateTestAccess::get_fan_state(fx.state());
    fans.init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux"});
    fans.update_fan_speed("fan", 0.5);
    fans.update_fan_speed("fan", 0.0);
    fans.update_fan_speed("fan_generic aux", 0.0);
    fx.settle();
    fx.settle(); // a mode change reschedules once
}

} // namespace

TEST_CASE_METHOD(Landscape800, "pre-print progress re-reads the checklist", "[print_status]") {
    size_t steps = 1;
    tiles().set_checklist_source(steps_source(steps));
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HOMING));
    UpdateQueue::instance().drain();
    REQUIRE(subject_int("ps_step_count") == 1);

    steps = 2;
    set_int(state().print_state().get_print_start_progress_subject(), 40);
    UpdateQueue::instance().drain();

    CHECK(subject_int("ps_step_count") == 2);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(Landscape800, "the pre-print elapsed tick re-reads the checklist",
                 "[print_status]") {
    size_t steps = 1;
    tiles().set_checklist_source(steps_source(steps));
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::HOMING));
    UpdateQueue::instance().drain();
    process_lvgl(900); // the preparing view shows after its debounce
    REQUIRE(subject_int("preparing_visible") == 1);
    REQUIRE(subject_int("ps_step_count") == 1);

    steps = 2;
    set_int(state().print_state().get_preprint_elapsed_subject(), 15);
    UpdateQueue::instance().drain();

    CHECK(subject_int("ps_step_count") == 2);
    set_int(state().print_state().get_print_start_phase_subject(),
            static_cast<int>(PrintStartPhase::IDLE));
    UpdateQueue::instance().drain();
}

// Heater readings arrive with every status update, idle or not, and reading the
// checklist asks the collector's predictor for its phase durations, which
// falls back to a printer-database lookup when there is no history.
TEST_CASE_METHOD(Landscape800, "a heater reading does not re-read the checklist",
                 "[print_status]") {
    int reads = 0;
    tiles().set_checklist_source([&reads]() -> std::optional<PreprintChecklistInput> {
        ++reads;
        return PreprintChecklistInput{};
    });

    auto& temps = state().temperature_state();
    for (int deci = 250; deci < 260; ++deci) {
        set_int(temps.get_bed_temp_subject(), deci);
        set_int(temps.get_active_extruder_temp_subject(), deci);
        UpdateQueue::instance().drain();
        process_lvgl(50); // the coalesced chip update runs on its own timer
    }
    settle();

    CHECK(reads == 0);
}

TEST_CASE_METHOD(Landscape800, "a fan spinning up mid-print does not change the mode",
                 "[print_status]") {
    size_printer_area(root_, 200, 280);
    three_fans_stopped(*this);
    const int mode = subject_int("ps_callout_mode");
    const CalloutRect before = image_rect(root_);
    REQUIRE(mode != static_cast<int>(CalloutMode::ImageOnly));

    PrinterStateTestAccess::get_fan_state(state()).update_fan_speed("fan", 0.6);
    settle();
    settle();
    CHECK(subject_int("ps_chip_part_fan_tone") == static_cast<int>(ChipTone::Active));
    CHECK(subject_int("ps_callout_mode") == mode);
    CHECK(same_rect(image_rect(root_), before));
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Landscape800, "a tuned speed does not change the mode", "[print_status]") {
    state().update_from_status({{"print_stats", {{"state", "printing"}}}});
    size_printer_area(root_, 200, 280);
    three_fans_stopped(*this);
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    settle();
    settle();
    const int mode = subject_int("ps_callout_mode");
    const CalloutRect before = image_rect(root_);
    REQUIRE(mode != static_cast<int>(CalloutMode::ImageOnly));

    set_int(state().motion_state().get_speed_factor_subject(), 104);
    settle();
    settle();
    CHECK(subject_int("ps_chip_speed_flow_tone") == static_cast<int>(ChipTone::Warn));
    CHECK(subject_int("ps_callout_mode") == mode);
    CHECK(same_rect(image_rect(root_), before));
    set_int(state().motion_state().get_speed_factor_subject(), 100);
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Landscape800, "a fan taking the part slot mid-print sheds nothing new",
                 "[print_status]") {
    // At this size the one-side line mode fits the stopped aux fan only by
    // shedding the hotend fan.
    size_printer_area(root_, 320, 220);
    // The [fan] has never run, so the generic fan takes the part slot once it
    // spins, and the aux slot empties.
    auto& fans = PrinterStateTestAccess::get_fan_state(state());
    fans.init_fans({"fan", "heater_fan hotend_fan", "fan_generic aux"});
    fans.update_fan_speed("fan", 0.0);
    fans.update_fan_speed("fan_generic aux", 0.0);
    settle();
    settle();
    REQUIRE(fans.classify_primary_fans().aux == "fan_generic aux");
    const int mode = subject_int("ps_callout_mode");
    const CalloutRect before = image_rect(root_);
    REQUIRE(mode == static_cast<int>(CalloutMode::OneSide));
    REQUIRE(subject_int("ps_chip_hotend_fan_shown") == 0);

    fans.update_fan_speed("fan_generic aux", 0.6);
    settle();
    settle();
    REQUIRE(fans.classify_primary_fans().part == "fan_generic aux");
    REQUIRE(fans.classify_primary_fans().aux.empty());
    // The aux slot stays budgeted, so the chips do not rearrange around its absence.
    CHECK(subject_int("ps_callout_mode") == mode);
    CHECK(same_rect(image_rect(root_), before));
    CHECK(subject_int("ps_chip_hotend_fan_shown") == 0);
    fans.init_fans({});
}

TEST_CASE_METHOD(Landscape800, "a rebuilt tree shows the exact-size copy as an exact copy",
                 "[print_status]") {
    tiles().use_displayed_image();
    settle();
    const std::string source = helix::PrinterImageManager::instance().get_displayed_image_path(
        lv_display_get_horizontal_resolution(lv_display_get_default()));
    REQUIRE_FALSE(source.empty());
    lv_obj_update_layout(root_);
    lv_obj_t* img = lv_obj_find_by_name(root_, "ps_printer_image");
    REQUIRE(img != nullptr);
    const std::string cache_src = "A:" + helix::get_cached_printer_image_path(
                                             source, lv_obj_get_width(img), lv_obj_get_height(img));
    REQUIRE(wait_until([&]() { return subject_str("ps_printer_image_src") == cache_src; }));
    REQUIRE(lv_image_get_inner_align(img) == LV_IMAGE_ALIGN_CENTER);

    // The memory-reclaim teardown, then a fresh tree whose image starts as the
    // XML declares it: CONTAIN-scaled.
    lv_obj_t* cached = panel().get_panel();
    panel().destroy_overlay_ui(cached);
    process_lvgl(50);
    root_ = panel().create(test_screen());
    REQUIRE(root_ != nullptr);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    settle();

    img = lv_obj_find_by_name(root_, "ps_printer_image");
    REQUIRE(img != nullptr);
    CHECK(subject_str("ps_printer_image_src") == cache_src);
    CHECK(lv_image_get_inner_align(img) == LV_IMAGE_ALIGN_CENTER);
}

TEST_CASE_METHOD(Landscape800, "a phase-dropped chip that turns unusual docks", "[print_status]") {
    size_printer_area(root_, 320, 208);
    three_fans_stopped(*this);
    const int mode = subject_int("ps_callout_mode");
    const CalloutRect before = image_rect(root_);
    REQUIRE((mode == static_cast<int>(CalloutMode::OneSide) ||
             mode == static_cast<int>(CalloutMode::BothSides)));
    const auto aux_dropped = [this] {
        const auto& d = tiles().last_layout().dropped;
        return std::find(d.begin(), d.end(), CalloutKind::AuxFan) != d.end();
    };
    REQUIRE(subject_int("ps_chip_aux_fan_shown") == 0);
    REQUIRE(aux_dropped());

    PrinterStateTestAccess::get_fan_state(state()).update_fan_speed("fan_generic aux", 0.6);
    settle();
    settle();
    CHECK(subject_int("ps_chip_aux_fan_shown") == 1);
    CHECK_FALSE(aux_dropped());
    CHECK(subject_int("ps_callout_mode") == mode);
    CHECK(same_rect(image_rect(root_), before));
    check_chips_inside_and_apart(root_);
    PrinterStateTestAccess::get_fan_state(state()).init_fans({});
}

TEST_CASE_METHOD(Landscape800, "a chip the phase sheds is never placed without room",
                 "[print_status]") {
    all_eight_chips(*this);
    // Measured: at this size the line mode sheds the hotend fan, the light and
    // speed/flow, and leaves no free room for the spinning aux fan.
    size_printer_area(root_, 300, 160);
    PrinterStateTestAccess::get_fan_state(state()).update_fan_speed("fan_generic aux", 0.6);
    settle();
    settle();
    const CalloutLayout& out = tiles().last_layout();
    REQUIRE(out.mode == CalloutMode::OneSide);
    REQUIRE_FALSE(out.dropped.empty());
    // Every chip on screen got its rect from this layout.
    size_t shown = 0;
    for (size_t i = 0; i < ui::kPrintStatusChipCount; ++i) {
        const std::string key = ui::print_status_chip_key(static_cast<ui::PrintStatusChip>(i));
        shown += subject_int(("ps_chip_" + key + "_shown").c_str()) == 1 ? 1 : 0;
    }
    CHECK(out.chips.size() == shown);
    CHECK(subject_int("ps_chip_aux_fan_shown") == 0);
    check_chips_inside_and_apart(root_);
    clear_all_eight_chips(*this);
}

TEST_CASE_METHOD(Landscape800, "each glow and leader line follows its own chip's subjects",
                 "[print_status]") {
    const char* keys[] = {"nozzle",     "bed",     "chamber", "part_fan",
                          "hotend_fan", "aux_fan", "light",   "speed_flow"};
    const char* colors[] = {nullptr, nullptr, nullptr,   "info",
                            "info",  "info",  "warning", "warning"};
    const auto subj = [](const std::string& name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name.c_str());
        REQUIRE(s != nullptr);
        return s;
    };
    const auto obj = [this](const std::string& name) {
        lv_obj_t* o = lv_obj_find_by_name(root_, name.c_str());
        REQUIRE(o != nullptr);
        return o;
    };
    const auto hidden = [](lv_obj_t* o) { return lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); };
    set_int(subj("ps_callout_mode"), static_cast<int>(CalloutMode::BothSides));
    for (const char* k : keys) {
        set_int(subj(std::string("ps_glow_") + k), 0);
        set_int(subj(std::string("ps_line_") + k), 0);
        set_int(subj(std::string("ps_chip_") + k + "_shown"), 1);
        set_int(subj(std::string("ps_chip_") + k + "_tone"), static_cast<int>(ChipTone::Normal));
    }
    for (size_t i = 0; i < std::size(keys); ++i) {
        const std::string k = keys[i];
        INFO(k);
        lv_obj_t* glow = obj("ps_glow_" + k);
        lv_obj_t* line = obj("ps_line_" + k);
        // A heater glow is painted from its heat state, not from the markup.
        if (colors[i])
            CHECK(lv_color_eq(lv_obj_get_style_bg_color(glow, LV_PART_MAIN),
                              theme_manager_get_color(colors[i])));
        set_int(subj("ps_glow_" + k), 1);
        set_int(subj("ps_line_" + k), 1);
        for (const char* other : keys) {
            INFO("other " << other);
            CHECK(hidden(obj(std::string("ps_glow_") + other)) == (k != other));
            CHECK(hidden(obj(std::string("ps_line_") + other)) == (k != other));
        }
        const int soft = lv_obj_get_style_bg_opa(glow, LV_PART_MAIN);
        set_int(subj("ps_chip_" + k + "_tone"), static_cast<int>(ChipTone::Active));
        CHECK(lv_obj_get_style_bg_opa(glow, LV_PART_MAIN) > soft);
        set_int(subj("ps_chip_" + k + "_shown"), 0);
        CHECK(hidden(line));
        set_int(subj("ps_chip_" + k + "_shown"), 1);
        set_int(subj("ps_callout_mode"), static_cast<int>(CalloutMode::Docked));
        CHECK(hidden(line));
        set_int(subj("ps_callout_mode"), static_cast<int>(CalloutMode::BothSides));
        set_int(subj("ps_chip_" + k + "_tone"), static_cast<int>(ChipTone::Normal));
        set_int(subj("ps_glow_" + k), 0);
        set_int(subj("ps_line_" + k), 0);
    }
}

TEST_CASE_METHOD(Landscape800, "the printer tile shows the exact-size copy of its picture",
                 "[print_status]") {
    tiles().use_displayed_image();
    settle();
    const std::string source = helix::PrinterImageManager::instance().get_displayed_image_path(
        lv_display_get_horizontal_resolution(lv_display_get_default()));
    REQUIRE_FALSE(source.empty());
    lv_obj_update_layout(root_);
    lv_obj_t* img = lv_obj_find_by_name(root_, "ps_printer_image");
    REQUIRE(img != nullptr);
    const std::string cache_path =
        helix::get_cached_printer_image_path(source, lv_obj_get_width(img), lv_obj_get_height(img));
    const std::string cache_src = "A:" + cache_path;
    // A miss shows the scaled source and hands the copy to a worker; a hit shows it now.
    INFO("source " << source);
    REQUIRE(wait_until([&]() { return subject_str("ps_printer_image_src") == cache_src; }));
    CHECK(std::filesystem::exists(cache_path));
    // A reading tick keeps the copy.
    settle();
    CHECK(subject_str("ps_printer_image_src") == cache_src);
}

TEST_CASE_METHOD(Landscape800,
                 "a new picture during a cache generation still gets its exact-size copy",
                 "[print_status]") {
    auto& images = helix::PrinterImageManager::instance();
    const int32_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    const std::string first = images.get_displayed_image_path(screen_w);
    images.set_active_image("shipped:qidi-q2");
    const std::string second = images.get_displayed_image_path(screen_w);
    images.set_active_image("");
    REQUIRE_FALSE(first.empty());
    REQUIRE(second != first);
    // Both lay out alike, so the two copies are cut for one rect. No picture
    // until the caches are cleared, so nothing generates early.
    std::string source;
    tiles().set_image_source([&source]() { return tagged_image(source); });
    settle();
    lv_obj_update_layout(root_);
    lv_obj_t* img = lv_obj_find_by_name(root_, "ps_printer_image");
    REQUIRE(img != nullptr);
    const int32_t w = lv_obj_get_width(img), h = lv_obj_get_height(img);
    std::error_code ec;
    for (const std::string& p : {first, second})
        std::filesystem::remove(helix::get_cached_printer_image_path(p, w, h), ec);

    // The first picture's generation is still in flight (its result lands only
    // when the queue drains) when the second picture arrives.
    source = first;
    tiles().update_now();
    source = second;
    tiles().update_now();
    REQUIRE(subject_str("ps_printer_image_src") == second);
    const std::string want = "A:" + helix::get_cached_printer_image_path(second, w, h);
    CHECK(wait_until([&]() { return subject_str("ps_printer_image_src") == want; },
                     kGenerationBudgetMs));
    for (const std::string& p : {first, second})
        std::filesystem::remove(helix::get_cached_printer_image_path(p, w, h), ec);
}

TEST_CASE_METHOD(Landscape800, "a cache generation landing after deinit leaves the tile alone",
                 "[print_status]") {
    const std::string source = helix::PrinterImageManager::instance().get_displayed_image_path(
        lv_display_get_horizontal_resolution(lv_display_get_default()));
    REQUIRE_FALSE(source.empty());
    std::string shown;
    tiles().set_image_source([&shown]() { return tagged_image(shown); });
    settle();
    lv_obj_update_layout(root_);
    lv_obj_t* img = lv_obj_find_by_name(root_, "ps_printer_image");
    REQUIRE(img != nullptr);
    const std::string cache_path =
        helix::get_cached_printer_image_path(source, lv_obj_get_width(img), lv_obj_get_height(img));
    std::error_code ec;
    std::filesystem::remove(cache_path, ec);

    // Another picture's generation holds the slot until a drain, so this miss is refused.
    shown = "A:not-a-printer.png";
    tiles().update_now();
    shown = source;
    tiles().update_now();
    REQUIRE(subject_str("ps_printer_image_src") == source);
    tiles().deinit();
    // The refused miss is still written; no result may reach the subject.
    REQUIRE(wait_until([&]() { return std::filesystem::exists(cache_path); }, kGenerationBudgetMs));
    process_lvgl(50);
    UpdateQueue::instance().drain();
    CHECK(subject_str("ps_printer_image_src") == source);
    std::filesystem::remove(cache_path, ec);
}

TEST_CASE_METHOD(Ultrawide1480,
                 "ultrawide builds one row of columns at the widths the fitter decides",
                 "[print_status][ultrawide]") {
    // The ultrawide body, not the landscape strip.
    REQUIRE(find("ps_uw_info") != nullptr);
    CHECK(find("ps_strip") == nullptr);
    CHECK(find("metadata_overlay") == nullptr);

    lv_obj_t* content = find("overlay_content");
    REQUIRE(content != nullptr);
    const int32_t w = lv_obj_get_content_width(content);
    const int32_t h = lv_obj_get_content_height(content);
    const auto cols =
        helix::decide_print_status_columns(w, h, theme_manager_get_spacing("space_md"));
    REQUIRE(cols.preview_w > 0);

    lv_obj_t* job = find("ps_job_tile");
    lv_obj_t* printer = find("ps_printer_tile");
    lv_obj_t* readouts = find("ps_uw_readouts");
    lv_obj_t* actions = find("ps_uw_actions");
    lv_obj_t* info = find("ps_uw_info");
    REQUIRE((job && printer && readouts && actions && info));
    CHECK(lv_obj_get_width(job) == cols.preview_w);
    CHECK(lv_obj_get_height(job) == h);
    CHECK(lv_obj_get_width(printer) == cols.printer_w);
    CHECK(lv_obj_get_width(readouts) == cols.readouts_w);
    CHECK(lv_obj_get_width(actions) == cols.actions_w);
    CHECK(lv_obj_get_width(info) == cols.info_w);

    // The buttons stack, and a heater row and the speed row are taps.
    lv_obj_t* pause = find("btn_pause");
    lv_obj_t* cancel = find("btn_cancel");
    REQUIRE((pause && cancel));
    CHECK(lv_obj_get_y(cancel) > lv_obj_get_y(pause));
    CHECK(lv_obj_get_x(cancel) == lv_obj_get_x(pause));
    CHECK(lv_obj_has_flag(find("readout_bed"), LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_has_flag(find("readout_speed"), LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(Ultrawide1480, "ultrawide readouts drop the chamber row without a chamber",
                 "[print_status][ultrawide]") {
    lv_subject_t* has_chamber = lv_xml_get_subject(nullptr, "printer_has_chamber");
    REQUIRE(has_chamber != nullptr);
    const int original = lv_subject_get_int(has_chamber);
    lv_obj_t* row = find("readout_chamber");
    REQUIRE(row != nullptr);

    lv_subject_set_int(has_chamber, 1);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(has_chamber, 0);
    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(has_chamber, original);
}

TEST_CASE_METHOD(Landscape800, "a standard screen keeps the strip and builds no columns",
                 "[print_status][ultrawide]") {
    CHECK(lv_obj_find_by_name(root_, "ps_strip") != nullptr);
    CHECK(lv_obj_find_by_name(root_, "ps_uw_info") == nullptr);
    CHECK(lv_obj_find_by_name(root_, "metadata_overlay") != nullptr);
}

// The lane tray: the readouts' column on ultrawide, a strip under stacked
// tiles, nothing beside side-by-side tiles. The gate is
// has_filament_system_or_multi_tool, which TargetTool publishes.

namespace {

/// Sets the lane tray gate for a case and puts it back after.
class ScopedLaneTrayGate {
  public:
    explicit ScopedLaneTrayGate(int value)
        : gate_(lv_xml_get_subject(nullptr, "has_filament_system_or_multi_tool")) {
        REQUIRE(gate_ != nullptr);
        original_ = lv_subject_get_int(gate_);
        set(value);
    }
    ~ScopedLaneTrayGate() {
        lv_subject_set_int(gate_, original_);
        UpdateQueue::instance().drain();
    }
    void set(int value) {
        lv_subject_set_int(gate_, value);
        UpdateQueue::instance().drain();
    }

  private:
    lv_subject_t* gate_;
    int original_ = 0;
};

bool shown(lv_obj_t* obj) {
    return obj && !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

struct Portrait600 : TileFixture {
    Portrait600() : TileFixture(600, 1024, true) {}
};
struct Portrait480x680Tray : TileFixture {
    Portrait480x680Tray() : TileFixture(480, 680, true) {}
};
struct Portrait480x800Tray : TileFixture {
    Portrait480x800Tray() : TileFixture(480, 800, true) {}
};
struct Portrait480x1280Tray : TileFixture {
    Portrait480x1280Tray() : TileFixture(480, 1280, true) {}
};
struct UltrawideTray : Ultrawide1480 {
    UltrawideTray() : Ultrawide1480(true) {}
};

} // namespace

TEST_CASE_METHOD(UltrawideTray, "ultrawide shows the lane tray in the readouts' column when gated",
                 "[print_status_tray][ultrawide]") {
    ScopedLaneTrayGate gate(0);
    lv_obj_update_layout(root_);
    lv_obj_t* readouts = find("ps_uw_readouts");
    lv_obj_t* tray = find("ps_uw_lane_tray");
    REQUIRE((readouts && tray));
    CHECK(shown(readouts));
    CHECK_FALSE(shown(tray));
    const int32_t readouts_w = lv_obj_get_width(readouts);
    const int32_t printer_w = lv_obj_get_width(find("ps_printer_tile"));

    gate.set(1);
    lv_obj_update_layout(root_);
    CHECK_FALSE(shown(readouts));
    CHECK(shown(tray));
    // The tray takes the readouts' width; the printer picture keeps its own.
    CHECK(lv_obj_get_width(tray) == readouts_w);
    CHECK(lv_obj_get_width(find("ps_printer_tile")) == printer_w);
    lv_obj_t* content = find("overlay_content");
    const auto cols = helix::decide_print_status_columns(lv_obj_get_content_width(content),
                                                         lv_obj_get_content_height(content),
                                                         theme_manager_get_spacing("space_md"));
    CHECK(lv_obj_get_width(tray) == cols.readouts_w);
    CHECK(lv_obj_get_height(tray) == lv_obj_get_content_height(content));
    // The tray widget itself, which opens the filament system's panel on a tap.
    lv_obj_t* status = lv_obj_find_by_name(tray, "ps_lane_tray_status");
    REQUIRE(status != nullptr);
    CHECK(lv_obj_has_flag(status, LV_OBJ_FLAG_CLICKABLE));

    gate.set(0);
    lv_obj_update_layout(root_);
    CHECK(shown(readouts));
    CHECK_FALSE(shown(tray));
}

TEST_CASE_METHOD(Portrait600, "stacked tiles give the lane tray a strip under the strip when gated",
                 "[print_status_tray]") {
    ScopedLaneTrayGate gate(0);
    settle();
    settle();
    lv_obj_t* tray = lv_obj_find_by_name(root_, "ps_lane_tray");
    lv_obj_t* strip = lv_obj_find_by_name(root_, "ps_strip");
    lv_obj_t* content = lv_obj_find_by_name(root_, "overlay_content");
    REQUIRE((tray && strip && content));
    REQUIRE(subject_int("ps_arrangement") == static_cast<int>(PrintStatusArrangement::Stacked));
    CHECK_FALSE(shown(tray));
    const int32_t tiles_h_plain = lv_obj_get_height(lv_obj_find_by_name(root_, "ps_tiles"));

    gate.set(1);
    settle();
    settle();
    CHECK(shown(tray));
    const int tray_h = subject_int("ps_lane_tray_h");
    CHECK(tray_h > 0);
    CHECK(lv_obj_get_height(tray) == tray_h);
    // Under the strip, inside the content box: nothing scrolls.
    lv_area_t tray_a, strip_a, content_a;
    lv_obj_get_coords(tray, &tray_a);
    lv_obj_get_coords(strip, &strip_a);
    lv_obj_get_content_coords(content, &content_a);
    CHECK(tray_a.y1 > strip_a.y2);
    CHECK(tray_a.y2 <= content_a.y2);
    CHECK(lv_obj_get_height(lv_obj_find_by_name(root_, "ps_tiles")) < tiles_h_plain);

    gate.set(0);
    settle();
    CHECK_FALSE(shown(tray));
}

TEST_CASE_METHOD(Landscape800, "side by side tiles show no lane tray even when gated",
                 "[print_status_tray]") {
    ScopedLaneTrayGate gate(1);
    settle();
    REQUIRE(subject_int("ps_arrangement") == static_cast<int>(PrintStatusArrangement::SideBySide));
    CHECK(subject_int("ps_lane_tray") == 0);
    CHECK_FALSE(shown(lv_obj_find_by_name(root_, "ps_lane_tray")));
}

namespace {
/// The printer tile's height and chip mode with the lane tray gated match
/// what it has without: the tray takes its room from the preview alone.
void check_printer_tile_kept(TileFixture& f) {
    ScopedLaneTrayGate gate(0);
    f.settle();
    f.settle();
    const int32_t plain_h = lv_obj_get_height(f.tile_);
    const int plain_mode = subject_int("ps_callout_mode");
    REQUIRE(plain_h > 0);
    gate.set(1);
    f.settle();
    f.settle();
    CAPTURE(plain_h, lv_obj_get_height(f.tile_), subject_int("ps_lane_tray_h"));
    CHECK(lv_obj_get_height(f.tile_) >= plain_h);
    CHECK(subject_int("ps_callout_mode") == plain_mode);
}
} // namespace

TEST_CASE_METHOD(Portrait600, "the lane tray leaves the printer tile its height at 600x1024",
                 "[print_status_tray]") {
    check_printer_tile_kept(*this);
    REQUIRE(subject_int("ps_lane_tray") == static_cast<int>(PrintStatusLaneTray::Strip));
    // The tile stands at the height the fitter decided, not a flex share.
    CHECK(lv_obj_get_height(tile_) == subject_int("ps_printer_tile_h"));
}

TEST_CASE_METHOD(Portrait480x680Tray, "the lane tray leaves the printer tile its height at 480x680",
                 "[print_status_tray]") {
    check_printer_tile_kept(*this);
}

TEST_CASE_METHOD(Portrait480x800Tray, "the lane tray leaves the printer tile its height at 480x800",
                 "[print_status_tray]") {
    check_printer_tile_kept(*this);
}

TEST_CASE_METHOD(Portrait480x1280Tray,
                 "the lane tray leaves the printer tile its height at 480x1280",
                 "[print_status_tray]") {
    check_printer_tile_kept(*this);
    CHECK(subject_int("ps_lane_tray") == static_cast<int>(PrintStatusLaneTray::Strip));
}

TEST_CASE_METHOD(UltrawideTray, "several heads with no lanes keep the readouts, not an empty tray",
                 "[print_status_tray][ultrawide]") {
    ScopedLaneTrayGate gate(1); // a multi-head printer
    lv_subject_t* slots = AmsState::instance().get_slot_count_subject();
    const int original = lv_subject_get_int(slots);
    lv_subject_set_int(slots, 0);
    PrintStatusPanelTestAccess::relayout(panel());
    UpdateQueue::instance().drain();
    lv_obj_update_layout(root_);
    CHECK(subject_int("ps_lane_tray") == 0);
    CHECK(shown(find("ps_uw_readouts")));
    CHECK_FALSE(shown(find("ps_uw_lane_tray")));

    lv_subject_set_int(slots, original);
    PrintStatusPanelTestAccess::relayout(panel());
    UpdateQueue::instance().drain();
}
