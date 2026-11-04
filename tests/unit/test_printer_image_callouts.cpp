// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_printer_manager_overlay.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_dir_guard.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/printer_image_regions_test_access.h"
#include "../test_helpers/process_async_timers.h"
#include "../test_helpers/scoped_pointer_indev.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "core/update_queue.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_controller.h"
#include "led/ui_led_control_overlay.h"
#include "light_button_config.h"
#include "lock_manager.h"
#include "lvgl_image_writer.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "prerender_size_class.h"
#include "prerendered_images.h"
#include "printer_image_manager.h"
#include "printer_image_regions.h"
#include "printer_images.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/callout_chip.h"
#include "src/ui/panel_widgets/printer_image_widget.h"
#include "src/ui/panel_widgets/text_measure.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "wizard_config_paths.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE_METHOD(LVGLUITestFixture,
                 "printer image: callout layer and chips exist, hidden when idle",
                 "[printer_image][callouts]") {
    // Callout subjects (callout_nozzle_shown, printer_callout_mode, ...) must
    // exist before the XML parses, or every bind_flag_if(_eq) on them warns
    // and skips, leaving each chip at its unbound (visible) XML default.
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    REQUIRE(h.child("callout_layer"));
    for (const char* n : {"callout_chip_nozzle", "callout_chip_bed", "callout_chip_chamber",
                          "callout_chip_fan", "callout_chip_light", "callout_chip_toolhead"}) {
        INFO(n);
        REQUIRE(h.child(n));
        CHECK(lv_obj_has_flag(h.child(n), LV_OBJ_FLAG_HIDDEN));
    }
    CHECK(lv_obj_has_flag(h.child("callout_layer"), LV_OBJ_FLAG_IGNORE_LAYOUT));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "printer image: every callout chip's clicked callback is registered",
                 "[printer_image][callouts]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    CHECK(lv_xml_get_event_cb(nullptr, "printer_callout_nozzle_cb") ==
          PrinterImageWidget::printer_callout_nozzle_cb);
    CHECK(lv_xml_get_event_cb(nullptr, "printer_callout_bed_cb") ==
          PrinterImageWidget::printer_callout_bed_cb);
    CHECK(lv_xml_get_event_cb(nullptr, "printer_callout_chamber_cb") ==
          PrinterImageWidget::printer_callout_chamber_cb);
    CHECK(lv_xml_get_event_cb(nullptr, "printer_callout_fan_cb") ==
          PrinterImageWidget::printer_callout_fan_cb);
    CHECK(lv_xml_get_event_cb(nullptr, "printer_callout_light_cb") ==
          PrinterImageWidget::printer_callout_light_cb);
}

TEST_CASE_METHOD(LVGLUITestFixture, "printer image: the light chip has no text label or subject",
                 "[printer_image][callouts]") {
    // callout_chip_light has no text to show: it borrows activity_chip's
    // styles.activity_chip look as a plain lv_obj holding only its icon,
    // rather than being an activity_chip instance with no text_subject.
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();

    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    lv_obj_t* light = h.child("callout_chip_light");
    REQUIRE(light);
    CHECK(lv_obj_find_by_name(light, "chip_text") == nullptr);
    // Still looks like a pill: same styles.activity_chip look every other
    // chip borrows (bg_opa and border_width are literals in that style, not
    // theme-token defaults a bare lv_obj would already carry).
    CHECK(lv_obj_get_style_bg_opa(light, LV_PART_MAIN) == LV_OPA_COVER * 65 / 100);
    CHECK(lv_obj_get_style_border_width(light, LV_PART_MAIN) == 1);
}

// ---------------------------------------------------------------------------
// Live data: PrinterState -> chip shown/text, layout, taps
// ---------------------------------------------------------------------------

namespace {

/// Registers the widget's subjects and tags the fallback image (the widget shows
/// generic-corexy with no printer type) with the K1C's points, so the widget
/// under test is "tagged" while the returned guard lives.
[[nodiscard]] ScopedImageRegions prepare_tagged_widget(int src_w = 1601, int src_h = 1204) {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    ImageRegions r;
    r.src_w = src_w;
    r.src_h = src_h;
    r.nozzle = {0.513f, 0.279f};
    r.part_fan = NormPoint{0.488f, 0.206f};
    r.chamber = NormPoint{0.313f, 0.379f};
    r.light = NormPoint{0.321f, 0.164f};
    r.bed_left = {0.308f, 0.571f};
    r.bed_right = {0.611f, 0.573f};
    return ScopedImageRegions({{"generic-corexy", r}});
}

/// True when `obj` lies inside `chip`'s content box horizontally.
bool within_chip(lv_obj_t* chip, lv_obj_t* obj) {
    lv_area_t chip_box, box;
    lv_obj_get_content_coords(chip, &chip_box);
    lv_obj_get_coords(obj, &box);
    return box.x1 >= chip_box.x1 && box.x2 <= chip_box.x2;
}

/// Observer handlers run from the UpdateQueue, and the layout from a one-shot
/// timer they schedule; drain both until nothing is left.
void settle() {
    for (int i = 0; i < 4; ++i) {
        helix::ui::UpdateQueue::instance().drain_until_empty();
        process_async_timers();
    }
}

std::string text_of(PanelWidgetHarness<PrinterImageWidget>& h, const char* chip) {
    return lv_label_get_text(lv_obj_find_by_name(h.child(chip), "chip_text"));
}
bool shown(PanelWidgetHarness<PrinterImageWidget>& h, const char* chip) {
    return !lv_obj_has_flag(h.child(chip), LV_OBJ_FLAG_HIDDEN);
}

/// Native `chamber_light` and `sb_leds` strips with no known state while the
/// guard lives. Declare it before the harness: the widget observes the
/// controller's state subject.
struct ScopedLedStrips {
    explicit ScopedLedStrips(MoonrakerAPI* api = nullptr, MoonrakerClientMock* client = nullptr) {
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(api, client);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            helix::led::LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = helix::led::LedBackendType::NATIVE;
            s.supports_color = true;
            ctrl.native().add_strip(s);
        }
    }
    ~ScopedLedStrips() {
        helix::led::LedController::instance().deinit();
    }
    ScopedLedStrips(const ScopedLedStrips&) = delete;
    ScopedLedStrips& operator=(const ScopedLedStrips&) = delete;

    /// Feeds a Klipper status frame reporting @p id fully on or off.
    static void report(const char* id, bool on) {
        const double v = on ? 1.0 : 0.0;
        helix::led::LedController::instance().update_from_status(
            {{id, {{"color_data", {{v, v, v, 0.0}}}}}});
    }
};

int mode_now() {
    return lv_subject_get_int(lv_xml_get_subject(nullptr, "printer_callout_mode"));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: bed heating shows the bed chip with heater_display text",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    CHECK_FALSE(lv_obj_has_flag(h.child("callout_layer"), LV_OBJ_FLAG_HIDDEN));
    CHECK(shown(h, "callout_chip_bed"));
    CHECK(text_of(h, "callout_chip_bed") == helix::ui::temperature::heater_display(400, 600).temp);
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "callout_bed_heating")) == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: idle cold printer shows no chips",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 250);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 0);
    lv_subject_set_int(state().temperature_state().get_active_extruder_temp_subject(), 250);
    lv_subject_set_int(state().temperature_state().get_active_extruder_target_subject(), 0);
    lv_subject_set_int(state().fan_state().get_fan_speed_subject(), 0);
    settle();
    CHECK_FALSE(shown(h, "callout_chip_bed"));
    CHECK_FALSE(shown(h, "callout_chip_nozzle"));
    CHECK_FALSE(shown(h, "callout_chip_fan"));
    CHECK_FALSE(shown(h, "callout_chip_toolhead"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: heater off but hot keeps the chip until 50C",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 0);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 640);
    settle();
    CHECK(shown(h, "callout_chip_bed"));
    CHECK(text_of(h, "callout_chip_bed") == "64°"); // pinned: no unit letter
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 500);
    settle();
    CHECK_FALSE(shown(h, "callout_chip_bed"));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a heater off but still hot greys its chip text",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_t* bed_shown = lv_xml_get_subject(nullptr, "callout_bed_shown");
    lv_obj_t* label = lv_obj_find_by_name(h.child("callout_chip_bed"), "chip_text");
    REQUIRE(label);
    const lv_color_t inactive = theme_manager_get_color("text_subtle");
    const auto text_color = [&] { return lv_obj_get_style_text_color(label, LV_PART_MAIN); };

    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 0);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 640);
    settle();
    REQUIRE(shown(h, "callout_chip_bed"));
    CHECK(lv_subject_get_int(bed_shown) == 2);
    CHECK(lv_color_eq(text_color(), inactive));

    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 2200);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 2200);
    settle();
    REQUIRE(shown(h, "callout_chip_bed"));
    CHECK(lv_subject_get_int(bed_shown) == 1);
    CHECK_FALSE(lv_color_eq(text_color(), inactive));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: fan on shows percent; light needs the LED capability",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    const ScopedLedStrips leds;
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_t* has_led = lv_xml_get_subject(nullptr, "printer_has_led");
    REQUIRE(has_led);
    lv_subject_set_int(state().temperature_state().get_active_extruder_target_subject(), 0);
    lv_subject_set_int(state().temperature_state().get_active_extruder_temp_subject(), 250);
    lv_subject_set_int(state().fan_state().get_fan_speed_subject(), 80);
    ScopedLedStrips::report("neopixel chamber_light", true);
    lv_subject_set_int(has_led, 0);
    settle();
    CHECK(shown(h, "callout_chip_fan"));
    CHECK(text_of(h, "callout_chip_fan") == "80%");
    CHECK_FALSE(shown(h, "callout_chip_light"));
    lv_subject_set_int(has_led, 1);
    settle();
    CHECK(shown(h, "callout_chip_light"));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a compact chip slot is as tall as the light chip's icon when it shows",
                 "[printer_image][callouts]") {
    // Small and below pair a 15px font_xs line with an 18px xs icon line; stand
    // in for that by pointing icon_font_xs at a taller face than font_xs.
    const std::string icon_font = lv_xml_get_const_silent(nullptr, "icon_font_xs");
    struct Restore {
        std::string v;
        ~Restore() {
            lv_xml_update_const(nullptr, "icon_font_xs", v.c_str());
        }
    } restore{icon_font};
    REQUIRE(lv_xml_update_const(nullptr, "icon_font_xs", "mdi_icons_32") == LV_RESULT_OK);
    const int text_line = lv_font_get_line_height(theme_manager_get_font("font_xs"));
    const int icon_line = lv_font_get_line_height(theme_manager_get_font("icon_font_xs"));
    REQUIRE(icon_line > text_line);

    lv_obj_t* chip = lv_obj_create(test_screen());
    lv_obj_set_style_pad_ver(chip, 3, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    CHECK(helix::ui::compact_callout_chip_h(chip, false) == text_line + 2 * 3 + 2);
    CHECK(helix::ui::compact_callout_chip_h(chip, true) == icon_line + 2 * 3 + 2);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a two-line chip is as wide as its widest line and two lines tall",
                 "[printer_image][callouts]") {
    lv_obj_t* chip = lv_obj_create(test_screen());
    lv_obj_set_style_pad_ver(chip, 3, 0);
    lv_obj_set_style_pad_hor(chip, 5, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    const auto m = helix::ui::measure_callout_chip(chip);
    const std::string top = "999 mm/s", bottom = "99.9 mm\xC2\xB3/s";
    const std::string both = top + "\n" + bottom;
    CHECK(m.chip_w(16, both) == std::max(m.chip_w(16, top), m.chip_w(16, bottom)));
    CHECK(m.chip_h_for(top) == m.chip_h);
    CHECK(m.chip_h_for(both) == std::max(2 * m.text_line_h, m.icon_line_h) + 2 * 3 + 2);
    CHECK(m.chip_h_for(both) > m.chip_h);

    const int xs_line = lv_font_get_line_height(theme_manager_get_font("font_xs"));
    CHECK(helix::ui::compact_callout_chip_h(chip, false, 2) == 2 * xs_line + 2 * 3 + 2);
    CHECK(helix::ui::compact_callout_chip_w(chip, both, 0) ==
          std::max(helix::ui::compact_callout_chip_w(chip, top, 0),
                   helix::ui::compact_callout_chip_w(chip, bottom, 0)));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: the light chip shows lit or dark with the chamber light, not another "
                 "strip",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    const ScopedLedStrips leds;
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_t* has_led = lv_xml_get_subject(nullptr, "printer_has_led");
    REQUIRE(has_led);
    lv_subject_set_int(has_led, 1);
    const auto lit = [&] {
        lv_obj_t* chip = h.child("callout_chip_light");
        const bool lit_shown =
            !lv_obj_has_flag(lv_obj_find_by_name(chip, "callout_light_lit"), LV_OBJ_FLAG_HIDDEN);
        const bool dark_shown =
            !lv_obj_has_flag(lv_obj_find_by_name(chip, "callout_light_dark"), LV_OBJ_FLAG_HIDDEN);
        REQUIRE(lit_shown != dark_shown);
        return lit_shown;
    };
    ScopedLedStrips::report("neopixel sb_leds", true);
    settle();
    CHECK(shown(h, "callout_chip_light"));
    CHECK_FALSE(lit());
    ScopedLedStrips::report("neopixel chamber_light", true);
    settle();
    CHECK(shown(h, "callout_chip_light"));
    CHECK(lit());
    ScopedLedStrips::report("neopixel chamber_light", false);
    settle();
    CHECK(shown(h, "callout_chip_light"));
    CHECK_FALSE(lit());
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: no light to switch shows no light chip and budgets none",
                 "[printer_image][callouts]") {
    // At this size the chamber and light chips together pin the callouts
    // (see the capability relayout case); the chamber alone does not.
    const auto regions = prepare_tagged_widget();
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    lv_subject_set_int(state().capabilities_state().subject(Capability::HasChamberHeater), 1);
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    settle();
    REQUIRE(ctrl.chamber_light().empty());
    CHECK_FALSE(shown(h, "callout_chip_light"));
    CHECK(mode_now() == static_cast<int>(CalloutMode::BothSides));
    ctrl.deinit();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a heating nozzle with the fan on merges into the toolhead chip",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_active_extruder_temp_subject(), 1800);
    lv_subject_set_int(state().temperature_state().get_active_extruder_target_subject(), 2200);
    lv_subject_set_int(state().fan_state().get_fan_speed_subject(), 50);
    settle();
    CHECK(shown(h, "callout_chip_toolhead"));
    CHECK_FALSE(shown(h, "callout_chip_nozzle"));
    CHECK_FALSE(shown(h, "callout_chip_fan"));
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Pinned));
    CHECK(text_of(h, "callout_chip_toolhead") == "180 / 220°  50%");
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: pinned chips are compact: no icon, no unit letter, a smaller font",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Pinned));
    lv_obj_t* chip = h.child("callout_chip_bed");
    lv_obj_t* label = lv_obj_find_by_name(chip, "chip_text");
    CHECK(text_of(h, "callout_chip_bed") == "40 / 60°");
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(chip, "heater_icon"), LV_OBJ_FLAG_HIDDEN));
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    CHECK(font == theme_manager_get_font("font_xs"));
    // Placed at the width it renders: its text plus chrome, no icon gap.
    const int text_px = helix::ui::measure_text_px(lv_label_get_text(label), font);
    CHECK(lv_obj_get_width(label) >= text_px);
    const int chrome = lv_obj_get_style_pad_left(chip, LV_PART_MAIN) +
                       lv_obj_get_style_pad_right(chip, LV_PART_MAIN) +
                       2 * lv_obj_get_style_border_width(chip, LV_PART_MAIN);
    CHECK(lv_obj_get_width(chip) == chrome + text_px + theme_manager_get_spacing("space_md"));
    const int compact_w = lv_obj_get_width(chip);

    // The same chip in a line mode is the full chip.
    h.resize(8, 4, 480, 160);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::BothSides));
    CHECK(text_of(h, "callout_chip_bed") == helix::ui::temperature::heater_display(400, 600).temp);
    CHECK_FALSE(lv_obj_has_flag(lv_obj_find_by_name(chip, "heater_icon"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_style_text_font(label, LV_PART_MAIN) == theme_manager_get_font("font_small"));
    CHECK(lv_obj_get_width(chip) > compact_w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: single cell hides the whole layer",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    REQUIRE_FALSE(lv_obj_has_flag(h.child("callout_layer"), LV_OBJ_FLAG_HIDDEN));
    h.resize(2, 2, 80, 80);
    settle();
    CHECK(lv_obj_has_flag(h.child("callout_layer"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a chip sits inside the image container",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    lv_obj_update_layout(h.root());
    lv_obj_t* chip = h.child("callout_chip_bed");
    lv_obj_t* layer = h.child("callout_layer");
    CHECK(lv_obj_get_x(chip) > 0);
    CHECK(lv_obj_get_y(chip) > 0);
    CHECK(lv_obj_get_x(chip) + lv_obj_get_width(chip) <= lv_obj_get_width(layer));
    CHECK(lv_obj_get_y(chip) + lv_obj_get_height(chip) <= lv_obj_get_height(layer));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a detached widget stops publishing",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 0);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 250);
    settle();
    lv_subject_t* bed_shown = lv_xml_get_subject(nullptr, "callout_bed_shown");
    REQUIRE(lv_subject_get_int(bed_shown) == 0);
    h.widget().detach();
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    CHECK(lv_subject_get_int(bed_shown) == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a chip narrower than its text dots the label",
                 "[printer_image][callouts]") {
    // A tall image in a narrow tile: pinned, but the area is narrower than the
    // bed chip, so clamp_into shrinks the chip below its text.
    const auto regions = prepare_tagged_widget(400, 1600);
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 48, 320);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE_FALSE(lv_obj_has_flag(h.child("callout_layer"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* chip = h.child("callout_chip_bed");
    lv_obj_t* label = lv_obj_find_by_name(chip, "chip_text");
    REQUIRE(label);
    REQUIRE(lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS);
    CHECK(within_chip(chip, label));
    // A dotted label rewrites its own text with the ellipsis once it overflows.
    const std::string shown = lv_label_get_text(label);
    REQUIRE(shown.size() >= 3);
    CHECK(shown.compare(shown.size() - 3, 3, "...") == 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a laid-out chip is not narrower than its text",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    lv_obj_update_layout(h.root());
    lv_obj_t* label = lv_obj_find_by_name(h.child("callout_chip_bed"), "chip_text");
    CHECK(lv_obj_get_width(label) >=
          helix::ui::measure_text_px(lv_label_get_text(label),
                                     lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a chip shows its full text before its first layout, and fits its "
                 "rect after it",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    helix::ui::UpdateQueue::instance().drain_until_empty();
    lv_obj_update_layout(h.root());
    lv_obj_t* chip = h.child("callout_chip_bed");
    lv_obj_t* label = lv_obj_find_by_name(chip, "chip_text");
    REQUIRE_FALSE(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
    const int text_px = helix::ui::measure_text_px(lv_label_get_text(label),
                                                   lv_obj_get_style_text_font(label, LV_PART_MAIN));
    CHECK(lv_obj_get_width(label) >= text_px);
    settle();
    lv_obj_update_layout(h.root());
    CHECK(lv_obj_get_width(label) >=
          helix::ui::measure_text_px(lv_label_get_text(label),
                                     lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    CHECK(within_chip(chip, label));
}

// A multi-tool printer draws the tool number beside the nozzle glyph; the chip
// has to budget for it or the temperature is cut.
TEST_CASE_METHOD(LVGLUITestFixture, "callouts: the nozzle chip budgets the tool badge",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    // The badge binds ToolState's subjects, which must exist before the XML parses.
    struct ToolSubjects {
        ToolSubjects() {
            // init_subjects() is a no-op on an initialized ToolState, which would leave its
            // names out of this test's XML scope; start from uninitialized.
            ToolState::instance().deinit_subjects();
            ToolState::instance().init_subjects(true);
        }
        ~ToolSubjects() {
            ToolState::instance().deinit_subjects();
        }
    } tool_subjects;
    auto& tools = ToolState::instance();
    lv_subject_copy_string(tools.get_tool_badge_text_subject(), "12");
    lv_subject_set_int(tools.get_show_tool_badge_subject(), 1);
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().fan_state().get_fan_speed_subject(), 0);
    lv_subject_set_int(state().temperature_state().get_active_extruder_temp_subject(), 1800);
    lv_subject_set_int(state().temperature_state().get_active_extruder_target_subject(), 2200);
    settle();
    lv_obj_update_layout(h.root());
    lv_obj_t* chip = h.child("callout_chip_nozzle");
    REQUIRE_FALSE(lv_obj_has_flag(chip, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* badge = lv_obj_find_by_name(chip, "tool_badge");
    REQUIRE(badge);
    REQUIRE_FALSE(lv_obj_has_flag(badge, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* label = lv_obj_find_by_name(chip, "chip_text");
    CHECK(lv_obj_get_width(label) >=
          helix::ui::measure_text_px(lv_label_get_text(label),
                                     lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    CHECK(within_chip(chip, label));
    CHECK(within_chip(chip, badge));
}

// The populate_widgets reuse path: the old component is deleted under the widget,
// then the SAME instance is attached to a fresh one (#1109 shape).
TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a recycled instance drives its new tree",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    auto& display = DisplaySettingsManager::instance();
    const bool prev_animations = display.get_animations_enabled();
    display.set_animations_enabled(true);
    PrinterImageWidget widget;
    auto* comp1 =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_printer_image", nullptr));
    REQUIRE(comp1);
    widget.attach(comp1, test_screen());
    lv_obj_update_layout(comp1);
    widget.on_size_changed(4, 4, 160, 160);
    lv_subject_set_int(get_printer_state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(get_printer_state().temperature_state().get_bed_temp_subject(), 400);
    lv_subject_set_int(get_printer_state().fan_state().get_fan_speed_subject(), 60);
    settle();
    REQUIRE(lv_anim_get(lv_obj_find_by_name(comp1, "callout_bed_glow"), nullptr) != nullptr);

    lv_obj_delete(comp1);
    settle();
    auto* comp2 =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_printer_image", nullptr));
    REQUIRE(comp2);
    widget.attach(comp2, test_screen());
    // Until the timer positions its chips, the new tree shows none of them.
    CHECK(lv_obj_has_flag(lv_obj_find_by_name(comp2, "callout_layer"), LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(comp2);
    widget.on_size_changed(4, 4, 160, 160);
    lv_subject_set_int(get_printer_state().temperature_state().get_bed_target_subject(), 650);
    settle();
    lv_obj_update_layout(comp2);
    lv_obj_t* bed = lv_obj_find_by_name(comp2, "callout_chip_bed");
    CHECK_FALSE(lv_obj_has_flag(bed, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_x(bed) > 0);
    CHECK(lv_anim_get(lv_obj_find_by_name(comp2, "callout_bed_glow"), nullptr) != nullptr);
    widget.detach();
    lv_obj_delete(comp2);
    display.set_animations_enabled(prev_animations);
}

namespace {

/// The temperature graph overlay is a process-lifetime singleton observing this
/// case's subjects. Destroy it before they die, or a later case's destroy_all()
/// tears down observers on freed subjects.
struct TempGraphOverlayScope {
    ~TempGraphOverlayScope() {
        StaticPanelRegistry::instance().destroy_all();
        helix::ui::UpdateQueue::instance().drain();
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: tapping the bed chip opens the bed temperature graph, not the "
                 "printer manager",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();

    TempGraphOverlayScope overlay_scope;
    get_global_temp_graph_overlay().init_subjects();
    lv_subject_t* mode = lv_xml_get_subject(nullptr, "temp_graph_mode");
    REQUIRE(mode);
    lv_subject_set_int(mode, static_cast<int>(TempGraphOverlay::Mode::GraphOnly));
    helix::ui::send_tap(h.child("callout_chip_bed"));
    process_lvgl(30);
    CHECK(lv_subject_get_int(mode) == static_cast<int>(TempGraphOverlay::Mode::Bed));
    CHECK_FALSE(
        NavigationManager::instance().is_panel_in_stack(get_printer_manager_overlay().get_root()));
}

namespace {

bool printer_manager_open() {
    lv_obj_t* root = get_printer_manager_overlay().get_root();
    return root && NavigationManager::instance().is_panel_in_stack(root);
}

void count_event_cb(lv_event_t* e) {
    ++*static_cast<int*>(lv_event_get_user_data(e));
}

} // namespace

namespace {

size_t scripts_naming(const MoonrakerClientMock& client, const std::string& what) {
    const auto& h = client.gcode_script_history();
    return static_cast<size_t>(std::count_if(h.begin(), h.end(), [&](const std::string& s) {
        return s.find(what) != std::string::npos;
    }));
}

/// A widget whose LED controller sends through a mock client, with the chamber
/// light reported on and the light chip showing.
struct LightChipRig {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    std::unique_ptr<MoonrakerAPIMock> api;
    std::unique_ptr<ScopedLedStrips> leds;

    explicit LightChipRig(PrinterState& ps) {
        api = std::make_unique<MoonrakerAPIMock>(client, ps);
        leds = std::make_unique<ScopedLedStrips>(api.get(), &client);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: tapping the light chip switches the chamber light, not another strip",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    LightChipRig rig(state());
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    ScopedLedStrips::report("neopixel chamber_light", true);
    settle();
    REQUIRE(shown(h, "callout_chip_light"));
    helix::ui::set_event_bubble_recursive(h.root());

    rig.client.clear_gcode_script_history();
    helix::ui::send_tap(h.child("callout_chip_light"));
    settle();
    CHECK(scripts_naming(rig.client, R"(LED="chamber_light" RED=0 GREEN=0 BLUE=0)") == 1);
    CHECK(scripts_naming(rig.client, "sb_leds") == 0);
    CHECK_FALSE(printer_manager_open());

    // A dark chip still shows, and a tap on it switches the light back on.
    ScopedLedStrips::report("neopixel chamber_light", false);
    settle();
    REQUIRE(shown(h, "callout_chip_light"));
    rig.client.clear_gcode_script_history();
    helix::ui::send_tap(h.child("callout_chip_light"));
    settle();
    CHECK(scripts_naming(rig.client, "chamber_light") == 1);
    CHECK(scripts_naming(rig.client, "RED=0 GREEN=0 BLUE=0") == 0);
    destroy_printer_manager_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a light chip tap while a light command is in flight sends nothing",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    LightChipRig rig(state());
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    settle();
    REQUIRE(shown(h, "callout_chip_light"));

    // Not drained: its ACK has not landed.
    helix::led::LedController::instance().set_power({"neopixel sb_leds"}, true);
    REQUIRE(helix::led::LedController::instance().light_command_in_flight());
    rig.client.clear_gcode_script_history();
    helix::ui::send_tap(h.child("callout_chip_light"));
    CHECK(rig.client.gcode_script_history().empty());
    settle();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: holding the light chip opens the LEDs overlay on the chamber light",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    LightChipRig rig(state());
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(test_screen());
    }
    NavigationManager::instance().set_panels(panels.data());

    // The overlay otherwise reopens on the last focused device. Opened and
    // closed before the widget exists: closing hides every screen child that
    // is not a main panel, the widget's tree included.
    ScopedLedStrips::report("neopixel sb_leds", true);
    REQUIRE(helix::open_led_control_overlay(test_screen(), "neopixel sb_leds") != nullptr);
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE(get_led_control_overlay().focused_device() == "neopixel sb_leds");

    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    ScopedLedStrips::report("neopixel chamber_light", true);
    settle();
    REQUIRE(shown(h, "callout_chip_light"));

    int long_presses_reaching_grid = 0;
    lv_obj_add_event_cb(h.root(), count_event_cb, LV_EVENT_LONG_PRESSED,
                        &long_presses_reaching_grid);
    helix::ui::set_event_bubble_recursive(h.root());
    lv_obj_update_layout(test_screen());
    lv_area_t box;
    lv_obj_get_coords(h.child("callout_chip_light"), &box);
    const int x = (box.x1 + box.x2) / 2;
    const int y = (box.y1 + box.y2) / 2;
    lv_point_t centre{x, y};
    lv_obj_t* hit = lv_indev_search_obj(test_screen(), &centre);
    REQUIRE(hit == h.child("callout_chip_light"));

    // A real press, held for three times the long-press time, then lifted: it is one
    // hold, and the lift must not also toggle the light.
    int holds_on_chip = 0;
    lv_obj_add_event_cb(h.child("callout_chip_light"), count_event_cb, LV_EVENT_LONG_PRESSED,
                        &holds_on_chip);
    rig.client.clear_gcode_script_history();
    helix_test::ScopedPointerIndev finger;
    finger.press(x, y);
    finger.hold(3 * finger.long_press_hold_ms());
    finger.release(x, y);
    settle();
    CHECK(get_led_control_overlay().focused_device() == "neopixel chamber_light");
    CHECK(long_presses_reaching_grid == 0);
    CHECK(holds_on_chip == 1);
    CHECK(scripts_naming(rig.client, "chamber_light") == 0);
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: tapping a light with no readable state flashes the dark bulb",
                 "[printer_image][callouts]") {
    // With no API nothing ever reports the strips' state.
    const auto regions = prepare_tagged_widget();
    const ScopedLedStrips leds;
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    settle();
    REQUIRE(shown(h, "callout_chip_light"));
    lv_obj_t* dark = lv_obj_find_by_name(h.child("callout_chip_light"), "callout_light_dark");
    REQUIRE(dark);
    REQUIRE_FALSE(lv_obj_has_flag(dark, LV_OBJ_FLAG_HIDDEN));
    const lv_color_t resting = lv_obj_get_style_text_color(dark, LV_PART_MAIN);
    const lv_color_t lamp = theme_manager_get_color("light_icon_on");
    REQUIRE_FALSE(lv_color_eq(resting, lamp));

    helix::ui::send_tap(h.child("callout_chip_light"));
    CHECK(lv_color_eq(lv_obj_get_style_text_color(dark, LV_PART_MAIN), lamp));

    // The flash ends back on the chip's own muted color.
    for (int i = 0; i < 20; ++i) {
        lv_tick_inc(50);
        lv_timer_handler_safe();
    }
    CHECK(lv_color_eq(lv_obj_get_style_text_color(dark, LV_PART_MAIN), resting));
    CHECK(lv_obj_get_style_opa(dark, LV_PART_MAIN) == LV_OPA_COVER);
    settle();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a light with a readable state gets no flash on a tap",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    LightChipRig rig(state());
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    ScopedLedStrips::report("neopixel chamber_light", false);
    settle();
    lv_obj_t* dark = lv_obj_find_by_name(h.child("callout_chip_light"), "callout_light_dark");
    REQUIRE_FALSE(lv_obj_has_flag(dark, LV_OBJ_FLAG_HIDDEN));
    const lv_color_t resting = lv_obj_get_style_text_color(dark, LV_PART_MAIN);

    helix::ui::send_tap(h.child("callout_chip_light"));
    CHECK(lv_color_eq(lv_obj_get_style_text_color(dark, LV_PART_MAIN), resting));
    settle();
}

namespace {

/// A PIN set and the screen locked while the guard lives.
struct ScopedScreenLock {
    ScopedScreenLock() {
        auto& lock = helix::LockManager::instance();
        lock.remove_pin();
        REQUIRE(lock.set_pin("1234"));
        lock.lock();
        REQUIRE(lock.is_locked());
    }
    ~ScopedScreenLock() {
        helix::LockManager::instance().remove_pin();
    }
    ScopedScreenLock(const ScopedScreenLock&) = delete;
    ScopedScreenLock& operator=(const ScopedScreenLock&) = delete;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: the light chip neither switches nor opens anything while locked",
                 "[printer_image][callouts][lock]") {
    const auto regions = prepare_tagged_widget();
    LightChipRig rig(state());
    std::array<lv_obj_t*, kUiPanelCount> panels{};
    for (auto& p : panels) {
        p = lv_obj_create(test_screen());
    }
    NavigationManager::instance().set_panels(panels.data());
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_has_led"), 1);
    ScopedLedStrips::report("neopixel chamber_light", true);
    settle();
    REQUIRE(shown(h, "callout_chip_light"));
    lv_obj_t* overlay_root = helix::open_led_control_overlay(test_screen(), "neopixel sb_leds");
    REQUIRE(overlay_root != nullptr);
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE_FALSE(NavigationManager::instance().is_panel_in_stack(overlay_root));

    {
        const ScopedScreenLock locked;
        rig.client.clear_gcode_script_history();
        helix::ui::send_tap(h.child("callout_chip_light"));
        lv_obj_send_event(h.child("callout_chip_light"), LV_EVENT_LONG_PRESSED, nullptr);
        settle();
        CHECK(rig.client.gcode_script_history().empty());
        CHECK_FALSE(NavigationManager::instance().is_panel_in_stack(overlay_root));
        CHECK(get_led_control_overlay().focused_device() == "neopixel sb_leds");
    }

    // Unlocked, the same tap switches the light: the guard is the lock, not a latch.
    helix::ui::send_tap(h.child("callout_chip_light"));
    settle();
    CHECK(scripts_naming(rig.client, "chamber_light") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a chip tap under the home panel's bubbling tree opens only its "
                 "control",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    helix::ui::set_event_bubble_recursive(h.root());

    TempGraphOverlayScope overlay_scope;
    get_global_temp_graph_overlay().init_subjects();
    lv_subject_t* mode = lv_xml_get_subject(nullptr, "temp_graph_mode");
    REQUIRE(mode);
    lv_subject_set_int(mode, static_cast<int>(TempGraphOverlay::Mode::GraphOnly));
    helix::ui::send_tap(h.child("callout_chip_bed"));
    process_lvgl(30);
    CHECK(lv_subject_get_int(mode) == static_cast<int>(TempGraphOverlay::Mode::Bed));
    CHECK_FALSE(printer_manager_open());
    destroy_printer_manager_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a tap on the bare printer image still opens the printer manager",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    settle();
    helix::ui::set_event_bubble_recursive(h.root());

    helix::ui::send_tap(h.child("printer_image"));
    process_lvgl(30);
    CHECK(printer_manager_open());
    destroy_printer_manager_overlay();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a long-press on a chip still bubbles to the home grid",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    helix::ui::set_event_bubble_recursive(h.root());

    int pressed = 0;
    int long_pressed = 0;
    lv_obj_add_event_cb(h.root(), count_event_cb, LV_EVENT_PRESSED, &pressed);
    lv_obj_add_event_cb(h.root(), count_event_cb, LV_EVENT_LONG_PRESSED, &long_pressed);
    lv_obj_send_event(h.child("callout_chip_bed"), LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(h.child("callout_chip_bed"), LV_EVENT_LONG_PRESSED, nullptr);
    CHECK(pressed == 1);
    CHECK(long_pressed == 1);
}

// ---------------------------------------------------------------------------
// Leader lines, bed glow, and the image moving aside
// ---------------------------------------------------------------------------

namespace {

/// Where the layout draws the image when it does not move it: contain-fit into
/// the container, from prepare_tagged_widget()'s default source size.
CalloutRect fitted_image(PanelWidgetHarness<PrinterImageWidget>& h) {
    lv_obj_t* c = h.child("printer_container");
    return fit_image(lv_obj_get_content_width(c), lv_obj_get_content_height(c), 1601, 1204);
}

/// Deletes one scaled-image cache entry, refusing anything outside the cache.
void forget_cache_entry(const std::string& path) {
    REQUIRE(path.rfind(helix::get_printer_image_cache_dir() + "/", 0) == 0);
    REQUIRE(path.size() > 4);
    REQUIRE(path.compare(path.size() - 4, 4, ".bin") == 0);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: wide widget draws a line to the bed chip",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    settle();
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::BothSides));
    CHECK_FALSE(lv_obj_has_flag(h.child("callout_line_bed"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(h.child("callout_bed_glow"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(h.child("callout_line_nozzle"), LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(),
                       600); // at target: glow off, line stays
    settle();
    CHECK(lv_obj_has_flag(h.child("callout_bed_glow"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(h.child("callout_line_bed"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: pinned mode draws no lines",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Pinned));
    CHECK(lv_obj_has_flag(h.child("callout_line_bed"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: the bed line runs from the bed point to the chip",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::BothSides));
    lv_obj_t* line = h.child("callout_line_bed");
    lv_obj_t* chip = h.child("callout_chip_bed");
    // Point, elbow, chip edge: the last run is level into the chip.
    REQUIRE(lv_line_get_point_count(line) == 3);
    const lv_point_precise_t* p = lv_line_get_points(line);
    const CalloutRect img = fitted_image(h);
    const float mx = (0.308f + 0.611f) / 2, my = (0.571f + 0.573f) / 2;
    CHECK(p[0].x == img.x + int(mx * float(img.w)));
    CHECK(p[0].y == img.y + int(my * float(img.h)));
    const int cx = lv_obj_get_x(chip), cw = lv_obj_get_width(chip);
    CHECK((p[2].x == cx || p[2].x == cx + cw));
    CHECK(p[2].y == lv_obj_get_y(chip) + lv_obj_get_height(chip) / 2);
    CHECK(p[1].y == p[2].y);
    CHECK(std::min(p[0].x, p[2].x) <= p[1].x);
    CHECK(p[1].x <= std::max(p[0].x, p[2].x));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a one-side layout moves the image aside; a wider tile restores it",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 400, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::OneSide));
    lv_obj_t* img = h.child("printer_image");
    lv_obj_t* container = h.child("printer_container");
    const CalloutRect fit = fitted_image(h);
    CHECK(lv_obj_get_x(img) == 0);
    CHECK(lv_obj_get_y(img) == fit.y);
    CHECK(lv_obj_get_width(img) == fit.w);
    CHECK(lv_obj_get_height(img) == fit.h);
    CHECK(lv_obj_get_x(h.child("callout_chip_bed")) >= fit.w);

    h.resize(8, 4, 480, 160);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::BothSides));
    CHECK(lv_obj_get_x(img) == 0);
    CHECK(lv_obj_get_y(img) == 0);
    CHECK(lv_obj_get_width(img) == lv_obj_get_content_width(container));
    CHECK(lv_obj_get_height(img) == lv_obj_get_content_height(container));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a both-sides layout with a shrunk image places it at its rect",
                 "[printer_image][callouts]") {
    // Too short to stack the three budget chips on one side, so only both sides
    // draws lines; narrowing the tile reaches the widths where that needs the shrink.
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    lv_obj_t* img = h.child("printer_image");
    lv_obj_t* container = h.child("printer_container");
    bool shrunk_seen = false;
    // The container pads its content box, so the tile carries that on top.
    const int pad = theme_manager_get_spacing("space_sm");
    for (int w = 480; w >= 320 && !shrunk_seen; w -= 2) {
        h.resize(8, 4, w + 2 * pad, 120 + 2 * pad);
        settle();
        lv_obj_update_layout(h.root());
        if (mode_now() != static_cast<int>(CalloutMode::BothSides))
            break;
        if (lv_obj_get_width(img) == lv_obj_get_content_width(container))
            continue;
        shrunk_seen = true;
        const CalloutRect fit = fitted_image(h);
        CAPTURE(w, fit.w, lv_obj_get_width(img));
        CHECK(lv_obj_get_width(img) < fit.w);
        CHECK(lv_obj_get_width(img) >=
              fit.w - fit.w * callout_detail::kMaxImageShrinkPctBothSides / 100);
        CHECK(lv_obj_get_x(img) ==
              (lv_obj_get_content_width(container) - lv_obj_get_width(img)) / 2);
    }
    CHECK(shrunk_seen);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a moved image drops the exact-size copy cut for its old rect",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    settle();
    lv_obj_t* img = h.child("printer_image");
    const auto src_now = [&] {
        const auto* p = static_cast<const char*>(lv_image_get_src(img));
        return std::string(p ? p : "");
    };
    // Warm or freshly generated, the exact-size copy is a cache .bin entry.
    const auto is_cache = [](const std::string& p) {
        return p.size() > 4 && p.compare(p.size() - 4, 4, ".bin") == 0;
    };
    if (!wait_until([&] { return is_cache(src_now()); }))
        SKIP("no cacheable printer image in this tree (source '" + src_now() + "')");
    const std::string full_cache = src_now();

    h.resize(8, 4, 400, 160);
    settle();
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::OneSide));
    CHECK(src_now() != full_cache);
    h.widget().detach();
}

// First display on a cold cache: the image moves while the full-size copy is
// still generating. That copy must not land on the moved image.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a copy generated for the old rect does not land on a moved image",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    const std::string source = printer_images::default_printer_image_path();

    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_obj_t* img = h.child("printer_image");
    lv_obj_t* container = h.child("printer_container");
    const int area_h = lv_obj_get_content_height(container);
    const std::string full =
        helix::get_cached_printer_image_path(source, lv_obj_get_content_width(container), area_h);
    const CalloutRect side = fit_image(400, area_h, 1601, 1204);
    const std::string moved = helix::get_cached_printer_image_path(source, side.w, side.h);
    forget_cache_entry(full);
    forget_cache_entry(moved);
    const auto src_now = [&] {
        const auto* p = static_cast<const char*>(lv_image_get_src(img));
        return std::string(p ? p : "");
    };

    // Timers only: the refresh shows the source and the cache check hands the
    // full-size generation to a worker. Its result waits in the UpdateQueue.
    helix::ui::UpdateQueue::instance().drain_until_empty();
    process_async_timers();
    REQUIRE(src_now() == source);
    auto& q = helix::ui::UpdateQueue::instance();
    for (int i = 0; i < 2000 && !(std::filesystem::exists(full) &&
                                  !helix::ui::UpdateQueueTestAccess::queue_empty(q));
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!std::filesystem::exists(full))
        SKIP("no cacheable printer image in this tree (source '" + source + "')");

    // The image moves before that result is applied.
    h.resize(8, 4, 400, 160);
    process_async_timers();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::OneSide));
    REQUIRE(lv_obj_get_width(img) == side.w);

    const bool landed = wait_until([&] { return src_now() == "A:" + moved; }, 3000);
    CHECK(src_now() != "A:" + full);
    CHECK(landed);
    h.widget().detach();
    forget_cache_entry(full);
    forget_cache_entry(moved);
}

// A copy cut for the tile's full size already on disk is what the image shows
// before a one-side layout moves it; after the move it must not be drawn at
// the scale that full-size copy needed.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a one-side image draws the source contain-fit into its rect",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    const std::string source = printer_images::default_printer_image_path();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 400, 160);
    lv_obj_t* img = h.child("printer_image");
    lv_obj_t* container = h.child("printer_container");
    const int area_w = lv_obj_get_content_width(container);
    const int area_h = lv_obj_get_content_height(container);
    const std::string full = helix::get_cached_printer_image_path(source, area_w, area_h);
    const CalloutRect side = fit_image(area_w, area_h, 1601, 1204);
    const std::string moved = helix::get_cached_printer_image_path(source, side.w, side.h);
    forget_cache_entry(moved);
    if (!helix::generate_cached_printer_image(source, area_w, area_h, full))
        SKIP("no cacheable printer image in this tree (source '" + source + "')");
    const auto src_now = [&] {
        const auto* p = static_cast<const char*>(lv_image_get_src(img));
        return std::string(p ? p : "");
    };

    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::OneSide));
    CHECK(lv_obj_get_width(img) == side.w);
    CHECK(lv_obj_get_height(img) == side.h);
    // Never the full-size copy; until the rect's own copy exists, the source
    // contain-scaled to the rect.
    CHECK(src_now() != "A:" + full);
    if (src_now() == source)
        CHECK(lv_image_get_inner_align(img) == LV_IMAGE_ALIGN_CONTAIN);

    // The rect's own copy, drawn 1:1.
    CHECK(wait_until([&] { return src_now() == "A:" + moved; }, 3000));
    CHECK(lv_image_get_inner_align(img) == LV_IMAGE_ALIGN_CENTER);
    CHECK(lv_image_get_scale(img) == LV_SCALE_NONE);
    h.widget().detach();
    forget_cache_entry(full);
    forget_cache_entry(moved);
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: a tile with no area puts a moved image back",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 400, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    lv_obj_t* img = h.child("printer_image");
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::OneSide));
    REQUIRE(lv_obj_get_style_width(img, LV_PART_MAIN) != LV_PCT(100));
    h.resize(8, 4, 0, 0);
    settle();
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::ImageOnly));
    CHECK(lv_obj_get_style_width(img, LV_PART_MAIN) == LV_PCT(100));
    CHECK(lv_obj_get_style_height(img, LV_PART_MAIN) == LV_PCT(100));
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: the disconnected overlay is centred on the image",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().network_state().get_printer_connection_state_subject(), 0);
    settle();
    lv_obj_update_layout(h.root());
    lv_obj_t* overlay = h.child("disconnected_overlay");
    lv_obj_t* container = h.child("printer_container");
    REQUIRE_FALSE(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_x(overlay) ==
          (lv_obj_get_content_width(container) - lv_obj_get_width(overlay)) / 2);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: the bed glow covers the bed edge and pulses while animations are on",
                 "[printer_image][callouts]") {
    const auto regions = prepare_tagged_widget();
    auto& display = DisplaySettingsManager::instance();
    const bool prev = display.get_animations_enabled();
    display.set_animations_enabled(true);
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    settle();
    lv_obj_update_layout(h.root());
    lv_obj_t* glow = h.child("callout_bed_glow");
    REQUIRE_FALSE(lv_obj_has_flag(glow, LV_OBJ_FLAG_HIDDEN));
    const CalloutRect img = fitted_image(h);
    const int x0 = img.x + int(0.308f * float(img.w));
    const int w = img.x + int(0.611f * float(img.w)) - x0;
    const int cy = img.y + int((0.571f + 0.573f) / 2 * float(img.h));
    CHECK(lv_obj_get_x(glow) == x0);
    CHECK(lv_obj_get_width(glow) == w);
    CHECK(lv_obj_get_height(glow) == w / 4);
    CHECK(lv_obj_get_y(glow) == cy - w / 8);
    CHECK(lv_anim_get(glow, nullptr) != nullptr);

    display.set_animations_enabled(false);
    settle();
    CHECK(lv_anim_get(glow, nullptr) == nullptr);

    display.set_animations_enabled(true);
    settle();
    REQUIRE(lv_anim_get(glow, nullptr) != nullptr);
    h.widget().detach();
    CHECK(lv_anim_get(glow, nullptr) == nullptr);
    display.set_animations_enabled(prev);
}

TEST_CASE_METHOD(LVGLUITestFixture, "callouts: an untagged image draws no bed glow",
                 "[printer_image][callouts]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    const ScopedImageRegions untagged({});
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    settle();
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Docked));
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "callout_bed_heating")) == 1);
    CHECK(lv_obj_has_flag(h.child("callout_bed_glow"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a capability found after layout relayouts with no chip change",
                 "[printer_image][callouts]") {
    // The chamber joins the budget with no chip showing; the bands spread their
    // chips, so the image keeps both sides and the light chip stays put.
    const auto regions = prepare_tagged_widget();
    lv_subject_t* has_led = lv_xml_get_subject(nullptr, "printer_has_led");
    lv_subject_t* has_chamber = state().capabilities_state().subject(Capability::HasChamberHeater);
    REQUIRE(has_led);
    lv_subject_set_int(has_led, 1);
    lv_subject_set_int(has_chamber, 0);
    const ScopedLedStrips leds;
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    settle();
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::BothSides));
    REQUIRE(shown(h, "callout_chip_light"));
    lv_subject_set_int(has_chamber, 1);
    settle();
    CHECK_FALSE(shown(h, "callout_chip_chamber"));
    CHECK(shown(h, "callout_chip_light"));
    CHECK(mode_now() == static_cast<int>(CalloutMode::BothSides));
}

// A custom image can be re-imported in place: same id, same path, new pixels.
TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a custom image re-imported under the same path lays out with its "
                 "new aspect",
                 "[printer_image][callouts]") {
    helix::init_widget_registrations();
    helix::PanelWidgetManager::instance().init_widget_subjects();
    const ScopedImageRegions untagged({});
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("helix-callout-reimport-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    auto& pim = helix::PrinterImageManager::instance();
    pim.init(root.string());
    Config* cfg = Config::get_instance();
    const std::string key = cfg->df() + helix::PRINTER_IMAGE;
    struct Restore {
        Config* cfg;
        std::string key;
        std::filesystem::path root;
        ~Restore() {
            cfg->set<std::string>(key, "");
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    } restore{cfg, key, root};
    const int size = helix::get_printer_image_size(
        lv_display_get_horizontal_resolution(lv_display_get_default()));
    const std::string bin = pim.get_custom_dir() + "reimport-" + std::to_string(size) + ".bin";
    const auto write_image = [&](int w, int h) {
        const std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0x80);
        REQUIRE(helix::write_lvgl_bin(bin, w, h, LV_COLOR_FORMAT_ARGB8888, px.data(), px.size()));
    };
    write_image(40, 160); // tall: a side band holds the docked chips
    cfg->set<std::string>(key, "custom:reimport");

    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(8, 4, 480, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    lv_subject_set_int(state().temperature_state().get_bed_temp_subject(), 400);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Docked));
    lv_obj_t* container = h.child("printer_container");
    lv_obj_t* chip = h.child("callout_chip_bed");
    const int area_w = lv_obj_get_content_width(container);
    const int area_h = lv_obj_get_content_height(container);
    const CalloutRect tall = fit_image(area_w, area_h, 40, 160);
    REQUIRE(lv_obj_get_x(chip) >= tall.x + tall.w); // in the band beside the image

    // Re-imported wide: no band left, so the chip docks along the bottom edge.
    // Nothing else changes, so the refresh alone has to relayout.
    write_image(160, 40);
    h.widget().refresh_printer_image();
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Docked));
    CHECK(lv_obj_get_y(chip) + lv_obj_get_height(chip) ==
          area_h - theme_manager_get_spacing("space_xs"));
    h.widget().detach();
}

namespace {

/// The bed chip's centre, relative to its parent.
CalloutPoint bed_chip_centre(PanelWidgetHarness<PrinterImageWidget>& h) {
    lv_obj_t* chip = h.child("callout_chip_bed");
    return {lv_obj_get_x(chip) + lv_obj_get_width(chip) / 2,
            lv_obj_get_y(chip) + lv_obj_get_height(chip) / 2};
}

/// Where the bed chip centres over `img` for a bed edge from x0 to x1 at y.
CalloutPoint bed_point(const CalloutRect& img, float x0, float x1, float y) {
    return {img.x + int((x0 + x1) / 2 * float(img.w)), img.y + int(y * float(img.h))};
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "callouts: a user entry at the image's natural size pins chips at the user's "
                 "points; one pixel off falls back to the shipped entry",
                 "[printer_image][callouts][image_tagger]") {
    const ConfigDirGuard cfg("callouts_user_entry");
    const auto regions = prepare_tagged_widget();
    const std::string shown = PrinterImageManager::instance().get_displayed_image_path(
        printer_images::current_screen_width());
    REQUIRE(printer_image_region_key(shown) == "generic-corexy");
    lv_image_header_t hdr;
    REQUIRE(lv_image_decoder_get_info(shown.c_str(), &hdr) == LV_RESULT_OK);
    const int nat_w = int(hdr.w), nat_h = int(hdr.h);
    const auto write_user_tags = [&](int size_w) {
        std::ofstream(cfg.dir / "printer_image_regions.json")
            << R"({"generic-corexy": {"size": [)" << size_w << ", " << nat_h
            << R"(], "nozzle": [0.5, 0.2], "bed": [[0.6, 0.75], [0.8, 0.75]]}})";
        reload_user_image_regions();
    };

    write_user_tags(nat_w);
    PanelWidgetHarness<PrinterImageWidget> h(test_screen());
    h.resize(4, 4, 160, 160);
    lv_subject_set_int(state().temperature_state().get_bed_target_subject(), 600);
    settle();
    lv_obj_update_layout(h.root());
    REQUIRE(mode_now() == static_cast<int>(CalloutMode::Pinned));
    lv_obj_t* c = h.child("printer_container");
    const CalloutRect user_img =
        fit_image(lv_obj_get_content_width(c), lv_obj_get_content_height(c), nat_w, nat_h);
    const CalloutPoint want_user = bed_point(user_img, 0.6f, 0.8f, 0.75f);
    CHECK(std::abs(bed_chip_centre(h).x - want_user.x) <= 1);
    CHECK(std::abs(bed_chip_centre(h).y - want_user.y) <= 1);

    // A save that cannot reach the disk changes nothing the widget draws.
    const auto file = cfg.dir / "printer_image_regions.json";
    std::filesystem::remove(file);
    std::filesystem::create_directories(file / "keep");
    ImageRegions moved = *lookup_user_image_regions("generic-corexy", nat_w, nat_h);
    moved.bed_left = {0.1f, 0.3f};
    moved.bed_right = {0.2f, 0.3f};
    REQUIRE_FALSE(save_user_image_regions("generic-corexy", moved));
    h.widget().refresh_printer_image();
    settle();
    lv_obj_update_layout(h.root());
    CHECK(std::abs(bed_chip_centre(h).x - want_user.x) <= 1);
    CHECK(std::abs(bed_chip_centre(h).y - want_user.y) <= 1);
    std::filesystem::remove_all(file);

    write_user_tags(nat_w + 1);
    h.widget().refresh_printer_image();
    settle();
    lv_obj_update_layout(h.root());
    const CalloutPoint want_shipped = bed_point(fitted_image(h), 0.308f, 0.611f, 0.572f);
    CHECK(std::abs(bed_chip_centre(h).x - want_shipped.x) <= 1);
    CHECK(std::abs(bed_chip_centre(h).y - want_shipped.y) <= 1);
}
