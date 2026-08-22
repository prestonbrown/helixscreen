// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_panel_bindings.cpp
 * @brief TDD tests for XML component subject-to-UI bindings
 *
 * These tests verify that LVGL subjects correctly update UI widgets through
 * declarative XML bindings.
 *
 * Test Categories:
 * - [ui][home_panel] - Home panel bindings
 * - [ui][controls_panel] - Controls panel bindings
 * - [ui][print_status_panel] - Print status panel bindings
 * - [ui][temp_panel] - Temperature panel bindings (nozzle + bed)
 * - [bind_text] - Text binding tests
 * - [bind_value] - Value binding tests (bars, sliders)
 * - [bind_flag] - Flag binding tests (visibility)
 * - [bind_style] - Style binding tests (colors, appearance)
 *
 * The XMLTestFixture provides:
 * - LVGL display with fonts and theme initialized
 * - Custom widgets registered (icon, text_*, ui_card, temp_display)
 * - PrinterState subjects registered for XML bindings
 */

#include "ui_panel_controls.h"
#include "ui_temp_display.h"

#include "../test_fixtures.h"
#include "../ui_test_utils.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
// Helper to set values on the XML-registered subject (what temp_display actually reads)
// This is critical for test isolation - other tests may have registered their own
// subjects with the same names, so we must use lv_xml_get_subject to get the
// subject that's ACTUALLY in the registry, not state().get_*_subject().
static void set_xml_subject(const char* name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(NULL, name);
    REQUIRE(subject != nullptr); // Fail fast if subject not registered
    lv_subject_set_int(subject, value);
}

// String counterpart of set_xml_subject, for bind_text targets.
static void set_xml_subject_str(const char* name, const char* value) {
    lv_subject_t* subject = lv_xml_get_subject(NULL, name);
    REQUIRE(subject != nullptr);
    lv_subject_copy_string(subject, value);
}

// Look up a named descendant, failing the test with the name if it is absent.
// Panel XML is edited far more often than these tests, so a rename must report
// WHICH widget vanished rather than a bare nullptr deref.
static lv_obj_t* require_named(lv_obj_t* root, const char* name) {
    REQUIRE(root != nullptr);
    lv_obj_t* found = lv_obj_find_by_name(root, name);
    INFO("looking for widget named '" << name << "'");
    REQUIRE(found != nullptr);
    return found;
}

static std::string label_text_of(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    const char* t = lv_label_get_text(obj);
    return t ? std::string(t) : std::string();
}

// controls_panel's bind_text/bind_value/bind_style targets (controls_pos_*,
// controls_*_status, controls_fan_pct, *_homed) are owned by ControlsPanel, not
// by PrinterState, so XMLTestFixture does not register them. Build the real
// panel object and let it publish its own subjects: binding against subjects the
// test invented would assert only that lv_xml resolves a name, not that the
// panel and its XML agree on one.
//
// The ctor is trivial (no setup(), no XML); deinit_subjects() in the destructor
// keeps the global XML subject registry from carrying dangling pointers into the
// next test.
class ControlsPanelSubjects {
  public:
    explicit ControlsPanelSubjects(PrinterState& st) : panel_(st, nullptr) {
        panel_.init_subjects();
    }
    ~ControlsPanelSubjects() {
        panel_.deinit_subjects();
    }
    ControlsPanelSubjects(const ControlsPanelSubjects&) = delete;
    ControlsPanelSubjects& operator=(const ControlsPanelSubjects&) = delete;

  private:
    ControlsPanel panel_;
};

// controls_panel binds every homing button's background through two bind_style
// rules (see the <consts> block at the top of controls_panel.xml):
//   home_btn_homed   -> bg_color #success
//   home_btn_unhomed -> bg_color #text @ opa 50
// Driving the subject and reading the resolved background is what proves the
// binding is live; asserting on the style NAME would only re-read the XML.
static void check_homing_button_tracks_subject(lv_obj_t* panel, const char* button_name,
                                               const char* subject_name) {
    lv_obj_t* btn = require_named(panel, button_name);

    set_xml_subject(subject_name, 1);
    lv_color_t homed = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
    CHECK(lv_color_to_u32(homed) == lv_color_to_u32(theme_manager_get_color("success")));

    set_xml_subject(subject_name, 0);
    lv_color_t unhomed = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
    CHECK(lv_color_to_u32(unhomed) == lv_color_to_u32(theme_manager_get_color("text")));
    CHECK(lv_color_to_u32(unhomed) != lv_color_to_u32(homed));
}

// =============================================================================
// TEMPERATURE PANEL BINDING TESTS (NOZZLE + BED) - READY FOR IMPLEMENTATION
// =============================================================================
// XMLTestFixture initialization now works - the theme init hang was fixed by
// deleting the test screen before theme initialization, then recreating it after.
//
// Remaining issue: lv_timer_handler() hangs when there are async subject updates
// scheduled. This prevents using process_lvgl() in tests that use XMLTestFixture.

TEST_CASE_METHOD(XMLTestFixture, "temp_display: binds to extruder temperature subjects",
                 "[ui][temp_display][bind_current][bind_target]") {
    // Test verifies the temp_display widget correctly binds to temperature subjects
    // and displays the expected values.

    // 1. Set temperature values BEFORE creating component using XML-registered subjects
    // Temperature is in decidegrees (200.0°C = 2000, 210.0°C = 2100)
    set_xml_subject("extruder_temp", 2000);   // 200.0°C
    set_xml_subject("extruder_target", 2100); // 210.0°C

    // 2. temp_display is already registered by XMLTestFixture (ui_temp_display_init)
    // Just need to create an instance with binding attributes
    const char* attrs[] = {"bind_current", "extruder_temp", "bind_target", "extruder_target",
                           "show_target",  "true",          nullptr};
    lv_obj_t* temp = create_component("temp_display", attrs);
    REQUIRE(temp != nullptr);
    REQUIRE(ui_temp_display_is_valid(temp));

    // 3. Verify initial values are bound correctly
    // temp_display converts decidegrees to degrees (2000 -> 200)
    int displayed_current = ui_temp_display_get_current(temp);
    int displayed_target = ui_temp_display_get_target(temp);

    REQUIRE(displayed_current == 200);
    REQUIRE(displayed_target == 210);
}

TEST_CASE_METHOD(XMLTestFixture, "temp_display: reactive update when subject changes",
                 "[ui][temp_display][reactive]") {
    // Test verifies the temp_display widget updates reactively when subjects change

    // 1. Set initial temperatures using XML-registered subjects
    set_xml_subject("extruder_temp", 1500);   // 150.0°C
    set_xml_subject("extruder_target", 2000); // 200.0°C

    // 2. Create temp_display with bindings
    const char* attrs[] = {"bind_current", "extruder_temp", "bind_target", "extruder_target",
                           "show_target",  "true",          nullptr};
    lv_obj_t* temp = create_component("temp_display", attrs);
    REQUIRE(temp != nullptr);

    // 3. Verify initial values
    REQUIRE(ui_temp_display_get_current(temp) == 150);
    REQUIRE(ui_temp_display_get_target(temp) == 200);

    // 4. Update subjects - this should trigger reactive update
    set_xml_subject("extruder_temp", 1800);   // 180.0°C
    set_xml_subject("extruder_target", 2200); // 220.0°C

    // 5. Verify values updated reactively
    REQUIRE(ui_temp_display_get_current(temp) == 180);
    REQUIRE(ui_temp_display_get_target(temp) == 220);
}

TEST_CASE_METHOD(XMLTestFixture, "temp_display: target shows -- when heater off (target=0)",
                 "[ui][temp_display][heater_off]") {
    // Test verifies target displays "--" when heater is off (target=0)

    // 1. Set current temp but target=0 (heater off) using XML-registered subjects
    set_xml_subject("extruder_temp", 250); // 25.0°C (ambient)
    set_xml_subject("extruder_target", 0); // Off

    // 2. Create temp_display with bindings
    const char* attrs[] = {"bind_current", "extruder_temp", "bind_target", "extruder_target",
                           "show_target",  "true",          nullptr};
    lv_obj_t* temp = create_component("temp_display", attrs);
    REQUIRE(temp != nullptr);

    // 3. Verify current shows actual value
    REQUIRE(ui_temp_display_get_current(temp) == 25);

    // 4. Verify target is 0 (the display shows "--" but getter returns 0)
    REQUIRE(ui_temp_display_get_target(temp) == 0);
}

TEST_CASE_METHOD(XMLTestFixture, "nozzle_temp_panel: temp_display shows current temperature",
                 "[ui][temp_panel][bind_current][.xml_required]") {
    SKIP("Requires nozzle_status subject registration - implement when subject is available");

    // Test implementation ready - uncomment when all subjects are registered:
    // REQUIRE(register_component("temp_display"));
    // REQUIRE(register_component("header_bar"));
    // REQUIRE(register_component("overlay_panel"));
    // REQUIRE(register_component("nozzle_temp_panel"));
    // lv_subject_set_int(state().get_active_extruder_temp_subject(), 20000);
    // lv_obj_t* panel = create_component("nozzle_temp_panel");
    // REQUIRE(panel != nullptr);
    // process_lvgl(100);
    // lv_obj_t* temp_display = UITest::find_by_name(panel, "nozzle_temp_display");
    // REQUIRE(temp_display != nullptr);
    // int displayed_current = ui_temp_display_get_current(temp_display);
    // REQUIRE(displayed_current == 200); // 20000 decidegrees = 200C
}

TEST_CASE_METHOD(XMLTestFixture, "temp_display: binds to bed temperature subjects",
                 "[ui][temp_display][bind_current][bind_target]") {
    // Test verifies the temp_display widget works with bed temperature subjects

    // 1. Set bed temperature values using XML-registered subjects
    set_xml_subject("bed_temp", 600);   // 60.0°C
    set_xml_subject("bed_target", 700); // 70.0°C

    // 2. Create temp_display with bed bindings
    const char* attrs[] = {"bind_current", "bed_temp", "bind_target", "bed_target",
                           "show_target",  "true",     nullptr};
    lv_obj_t* temp = create_component("temp_display", attrs);
    REQUIRE(temp != nullptr);
    REQUIRE(ui_temp_display_is_valid(temp));

    // 3. Verify bed values are bound correctly
    REQUIRE(ui_temp_display_get_current(temp) == 60);
    REQUIRE(ui_temp_display_get_target(temp) == 70);
}

// =============================================================================
// HOME PANEL BINDING TESTS (SKIP - complex dependencies)
// =============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: status_text binding updates label",
                 "[ui][home_panel][bind_text][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: printer_type_text binding updates label",
                 "[ui][home_panel][bind_text][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: print_display_filename binding updates label",
                 "[ui][home_panel][bind_text][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: print_progress_text binding updates label",
                 "[ui][home_panel][bind_text][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: print_progress_bar binding updates bar value",
                 "[ui][home_panel][bind_value][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: disconnected_overlay hidden when connected",
                 "[ui][home_panel][bind_flag][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: notification_badge hidden when count is zero",
                 "[ui][home_panel][bind_flag][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: extruder_temp binding updates temp_display",
                 "[ui][home_panel][bind_current][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "home_panel: extruder_target binding updates temp_display target",
                 "[ui][home_panel][bind_target][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: network_label binding updates text",
                 "[ui][home_panel][bind_text][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

// =============================================================================
// CONTROLS PANEL BINDING TESTS (SKIP - complex dependencies)
// =============================================================================

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: pos_x binding updates position text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject_str("controls_pos_x", "12.34");
    lv_obj_t* panel = create_component("controls_panel");
    CHECK(label_text_of(require_named(panel, "pos_x")) == "12.34");

    // Reactive: the label must follow a later publish, not just whatever the
    // subject happened to hold when the widget was built.
    set_xml_subject_str("controls_pos_x", "-5.00");
    CHECK(label_text_of(require_named(panel, "pos_x")) == "-5.00");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: pos_y binding updates position text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject_str("controls_pos_y", "12.34");
    lv_obj_t* panel = create_component("controls_panel");
    CHECK(label_text_of(require_named(panel, "pos_y")) == "12.34");

    // Reactive: the label must follow a later publish, not just whatever the
    // subject happened to hold when the widget was built.
    set_xml_subject_str("controls_pos_y", "-5.00");
    CHECK(label_text_of(require_named(panel, "pos_y")) == "-5.00");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: pos_z binding updates position text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject_str("controls_pos_z", "12.34");
    lv_obj_t* panel = create_component("controls_panel");
    CHECK(label_text_of(require_named(panel, "pos_z")) == "12.34");

    // Reactive: the label must follow a later publish, not just whatever the
    // subject happened to hold when the widget was built.
    set_xml_subject_str("controls_pos_z", "-5.00");
    CHECK(label_text_of(require_named(panel, "pos_z")) == "-5.00");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "controls_panel: speed_pct binding updates text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    SKIP("Controls panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "controls_panel: flow_pct binding updates text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    SKIP("Controls panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: x_homed drives btn_home_x background",
                 "[ui][controls_panel][bind_style][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    lv_obj_t* panel = create_component("controls_panel");
    check_homing_button_tracks_subject(panel, "btn_home_x", "x_homed");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: y_homed drives btn_home_y background",
                 "[ui][controls_panel][bind_style][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    lv_obj_t* panel = create_component("controls_panel");
    check_homing_button_tracks_subject(panel, "btn_home_y", "y_homed");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: z_homed drives btn_home_z background",
                 "[ui][controls_panel][bind_style][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    lv_obj_t* panel = create_component("controls_panel");
    check_homing_button_tracks_subject(panel, "btn_home_z", "z_homed");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: part_fan_slider binding updates slider value",
                 "[ui][controls_panel][bind_value][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject("controls_fan_pct", 40);
    lv_obj_t* panel = create_component("controls_panel");
    lv_obj_t* slider = require_named(panel, "part_fan_slider");
    CHECK(lv_slider_get_value(slider) == 40);

    set_xml_subject("controls_fan_pct", 100);
    CHECK(lv_slider_get_value(slider) == 100);

    set_xml_subject("controls_fan_pct", 0);
    CHECK(lv_slider_get_value(slider) == 0);
}

// =============================================================================
// PRINT STATUS PANEL BINDING TESTS (SKIP - complex dependencies)
// =============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: print_display_filename binding updates label",
                 "[ui][print_status_panel][bind_text][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: print_elapsed binding updates time label",
                 "[ui][print_status_panel][bind_text][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: print_remaining binding updates time label",
                 "[ui][print_status_panel][bind_text][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: print_progress bar binding updates value",
                 "[ui][print_status_panel][bind_value][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: print_progress_text binding updates label",
                 "[ui][print_status_panel][bind_text][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "print_status_panel: print_layer_text binding updates label",
                 "[ui][print_status_panel][bind_text][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: preparing_overlay hidden when not preparing",
                 "[ui][print_status_panel][bind_flag][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: print_complete_overlay visibility on outcome",
                 "[ui][print_status_panel][bind_flag][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}

// =============================================================================
// NOZZLE/BED TEMP PANEL STATUS BINDING TESTS (SKIP - needs nozzle_status subject)
// =============================================================================

TEST_CASE_METHOD(MoonrakerTestFixture, "nozzle_temp_panel: status_message binding updates text",
                 "[ui][temp_panel][bind_text][.xml_required]") {
    SKIP("Requires nozzle_status subject registration - implement when subject is available");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "bed_temp_panel: temp_display shows target temperature",
                 "[ui][temp_panel][bind_target][.xml_required]") {
    SKIP("Requires full bed_temp_panel test - similar to nozzle tests above");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "bed_temp_panel: status_message binding updates text",
                 "[ui][temp_panel][bind_text][.xml_required]") {
    SKIP("Requires bed_status subject registration - implement when subject is available");
}

// =============================================================================
// ADDITIONAL BINDING TESTS (MIXED PANELS - SKIP)
// =============================================================================

TEST_CASE_METHOD(XMLTestFixture,
                 "controls_panel: nozzle_temp_display binding (temp_display widget)",
                 "[ui][controls_panel][bind_current][bind_target][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    // Subjects carry decidegrees; temp_display renders whole degrees.
    set_xml_subject("extruder_temp", 2015);
    set_xml_subject("extruder_target", 2200);
    lv_obj_t* panel = create_component("controls_panel");
    lv_obj_t* disp = require_named(panel, "nozzle_temp_display");
    REQUIRE(ui_temp_display_is_valid(disp));
    CHECK(ui_temp_display_get_current(disp) == 201);
    CHECK(ui_temp_display_get_target(disp) == 220);

    set_xml_subject("extruder_temp", 1000);
    CHECK(ui_temp_display_get_current(disp) == 100);
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: bed_temp_display binding (temp_display widget)",
                 "[ui][controls_panel][bind_current][bind_target][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject("bed_temp", 600);
    set_xml_subject("bed_target", 650);
    lv_obj_t* panel = create_component("controls_panel");
    lv_obj_t* disp = require_named(panel, "bed_temp_display");
    REQUIRE(ui_temp_display_is_valid(disp));
    CHECK(ui_temp_display_get_current(disp) == 60);
    CHECK(ui_temp_display_get_target(disp) == 65);

    // Nozzle and bed must not share a subject - a nozzle publish leaves the bed
    // display alone.
    set_xml_subject("extruder_temp", 2500);
    CHECK(ui_temp_display_get_current(disp) == 60);
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: nozzle_status binding updates status text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject_str("controls_nozzle_status", "Heating");
    lv_obj_t* panel = create_component("controls_panel");
    CHECK(label_text_of(require_named(panel, "nozzle_status")) == "Heating");

    set_xml_subject_str("controls_nozzle_status", "");
    CHECK(label_text_of(require_named(panel, "nozzle_status")).empty());
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: bed_status binding updates status text",
                 "[ui][controls_panel][bind_text][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    set_xml_subject_str("controls_bed_status", "Heating");
    lv_obj_t* panel = create_component("controls_panel");
    CHECK(label_text_of(require_named(panel, "bed_status")) == "Heating");

    set_xml_subject_str("controls_bed_status", "");
    CHECK(label_text_of(require_named(panel, "bed_status")).empty());
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "home_panel: print_card_idle visibility bound to print_active",
                 "[ui][home_panel][bind_flag][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "home_panel: print_card_printing visibility bound to print_show_progress",
                 "[ui][home_panel][bind_flag][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(MoonrakerTestFixture, "home_panel: printer_image dimmed style when disconnected",
                 "[ui][home_panel][bind_style][.xml_required]") {
    SKIP("Home panel has many component dependencies - implement after simpler panels work");
}

TEST_CASE_METHOD(XMLTestFixture, "controls_panel: all_homed drives btn_home_all background",
                 "[ui][controls_panel][bind_style][.xml_required]") {
    ControlsPanelSubjects owner(state());
    REQUIRE(register_component("controls_panel"));
    lv_obj_t* panel = create_component("controls_panel");
    check_homing_button_tracks_subject(panel, "btn_home_all", "all_homed");
}

TEST_CASE_METHOD(MoonrakerTestFixture,
                 "print_status_panel: timelapse button visibility bound to capability",
                 "[ui][print_status_panel][bind_flag][.xml_required]") {
    SKIP("Print status panel has many component dependencies - implement after simpler panels "
         "work");
}
