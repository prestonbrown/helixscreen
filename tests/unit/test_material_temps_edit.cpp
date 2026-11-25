// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_material_temps_edit.cpp
 * @brief The Material Temperatures edit view's commit: Save in the header,
 *        enabled once something changes, and a Back that asks before it drops
 *        unsaved edits. The form fits its view at 480x272 and 800x480 without
 *        scrolling.
 */

#include "ui_button.h"
#include "ui_modal.h"
#include "ui_settings_material_temps.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/scoped_geometry.h"
#include "../ui_test_utils.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "material_settings_manager.h"
#include "static_panel_registry.h"

#include <lvgl.h>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::MaterialSettingsManager;

namespace {

struct ScreenSize {
    const char* what;
    int32_t w;
    int32_t h;
};

constexpr ScreenSize kSizes[] = {
    {"480x272", 480, 272},
    {"800x480", 800, 480},
};

/// The form must fit at every size, portrait micro included.
constexpr ScreenSize kFitSizes[] = {
    {"480x272", 480, 272},
    {"480x320", 480, 320},
    {"800x480", 800, 480},
    {"272x480", 272, 480},
};

void reset_material_temps_singleton() {
    helix::ui::UpdateQueue::instance().drain();
    StaticPanelRegistry::instance().destroy_all();
    helix::ui::UpdateQueue::instance().drain();
    // The next size builds its own tree; the last one's must not be found first.
    lv_obj_clean(lv_screen_active());
    helix::ui::UpdateQueue::instance().drain();
}

void set_int(const char* subject_name, int value) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, subject_name);
    REQUIRE(subject != nullptr);
    lv_subject_set_int(subject, value);
}

lv_obj_t* find_widget(const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(lv_screen_active(), name);
    REQUIRE(obj != nullptr);
    return obj;
}

/// Shown on screen: neither it nor any ancestor carries the hidden flag.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* p = obj; p != nullptr; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

bool inside_screen(lv_obj_t* obj, const ScreenSize& s) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    INFO("coords " << a.x1 << "," << a.y1 << " - " << a.x2 << "," << a.y2);
    return a.x1 >= 0 && a.y1 >= 0 && a.x2 < s.w && a.y2 < s.h;
}

bool disabled(lv_obj_t* obj) {
    return lv_obj_has_state(obj, LV_STATE_DISABLED);
}

bool editing() {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, "material_editing");
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject) != 0;
}

/// The layout-variant file the app loads at the display's size.
const char* variant_for_display() {
    lv_display_t* d = lv_display_get_default();
    const int32_t w = lv_display_get_horizontal_resolution(d);
    const int32_t h = lv_display_get_vertical_resolution(d);
    if (w == 480 && h == 272) {
        return "A:ui_xml/micro/material_temps_overlay.xml";
    }
    if (w == 272 && h == 480) {
        return "A:ui_xml/micro_portrait/material_temps_overlay.xml";
    }
    return "A:ui_xml/material_temps_overlay.xml";
}

/// Opens the overlay on the ABS edit view, the tallest form: a chamber column
/// and a preheat macro, so the macro-heating row shows too.
struct MaterialEditFixture : public XMLTestFixture {
    helix::settings::MaterialTempsOverlay& open_abs() {
        reset_material_temps_singleton();
        MaterialSettingsManager::instance().clear_override("ABS");
        set_int("printer_has_chamber_heater", 1);
        helix::ui::modal_init_subjects();
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("overlay_panel"));
        REQUIRE(register_component("modal_dialog"));
        REQUIRE(lv_xml_register_component_from_file(variant_for_display()) == LV_RESULT_OK);

        auto& overlay = helix::settings::get_material_temps_overlay();
        overlay.show(lv_screen_active());
        helix::ui::UpdateQueue::instance().drain();
        overlay.handle_material_row_clicked("ABS");
        process_lvgl(600);
        lv_obj_update_layout(lv_screen_active());
        UITest::init(lv_screen_active());
        return overlay;
    }

    ~MaterialEditFixture() override {
        UITest::cleanup();
        MaterialSettingsManager::instance().clear_override("ABS");
        reset_material_temps_singleton();
        // The standard registration for any case that follows.
        lv_xml_register_component_from_file("A:ui_xml/material_temps_overlay.xml");
    }
};

} // namespace

TEST_CASE_METHOD(MaterialEditFixture,
                 "Material edit Save sits in the header, labelled, inside the screen",
                 "[material_temps][save_header]") {
    for (const ScreenSize& s : kSizes) {
        INFO(s.what);
        helix::test::ScopedGeometry screen(s.w, s.h);
        open_abs();

        lv_obj_t* save = find_widget("action_button");
        CHECK(shown(save));
        CHECK(inside_screen(save, s));
        // A pill with its label, never a bare icon at micro.
        lv_obj_t* label = ui_button_get_label(save);
        REQUIRE(label != nullptr);
        CHECK(shown(label));
        CHECK(lv_obj_get_width(save) > lv_obj_get_height(save));

        reset_material_temps_singleton();
    }
}

TEST_CASE_METHOD(MaterialEditFixture,
                 "Material edit form fits its view without scrolling, Reset included",
                 "[material_temps][save_header]") {
    for (const ScreenSize& s : kFitSizes) {
        INFO(s.what);
        helix::test::ScopedGeometry screen(s.w, s.h);
        open_abs();
        // A macro chosen for the material shows its heating row.
        set_int("material_has_macro", 1);
        lv_obj_update_layout(lv_screen_active());

        lv_obj_t* view = find_widget("material_edit_view");
        REQUIRE(shown(view));
        CHECK(inside_screen(view, s));
        lv_area_t v;
        lv_obj_get_coords(view, &v);
        const int32_t floor_y = v.y2 - lv_obj_get_style_pad_bottom(view, LV_PART_MAIN);
        // Each shown row starts below the one before it and ends above the floor.
        int32_t prev_bottom = v.y1;
        for (uint32_t i = 0; i < lv_obj_get_child_count(view); ++i) {
            lv_obj_t* child = lv_obj_get_child(view, static_cast<int32_t>(i));
            if (!shown(child)) {
                continue;
            }
            lv_area_t c;
            lv_obj_get_coords(child, &c);
            INFO("child " << i << " " << c.y1 << "-" << c.y2 << ", floor " << floor_y);
            CHECK(c.y1 >= prev_bottom);
            CHECK(c.y2 <= floor_y);
            prev_bottom = c.y2;
        }
        // Inside the macro section, the switch's column sits clear of the dropdown.
        lv_area_t dd;
        lv_area_t sw;
        lv_obj_get_coords(find_widget("macro_dropdown"), &dd);
        lv_obj_get_coords(find_widget("macro_heating_row"), &sw);
        INFO("dropdown " << dd.x1 << "," << dd.y1 << "-" << dd.x2 << "," << dd.y2 << " switch "
                         << sw.x1 << "," << sw.y1 << "-" << sw.x2 << "," << sw.y2);
        CHECK((sw.x1 > dd.x2 || sw.y1 > dd.y2));
        CHECK(lv_obj_get_height(find_widget("macro_heating_row")) > 0);

        lv_obj_t* reset = find_widget("btn_reset_defaults");
        CHECK(shown(reset));
        CHECK(inside_screen(reset, s));

        reset_material_temps_singleton();
    }
}

TEST_CASE_METHOD(MaterialEditFixture, "Material edit Save shows only while a material is open",
                 "[material_temps][save_header]") {
    auto& overlay = open_abs();
    lv_obj_t* save = find_widget("action_button");
    CHECK(shown(save));

    overlay.handle_back_clicked();
    process_lvgl(50);
    REQUIRE_FALSE(editing());
    CHECK_FALSE(shown(save));
}

TEST_CASE_METHOD(MaterialEditFixture,
                 "Material edit Save is disabled until a field changes, then a tap saves",
                 "[material_temps][save_header]") {
    for (const ScreenSize& s : kSizes) {
        INFO(s.what);
        helix::test::ScopedGeometry screen(s.w, s.h);
        open_abs();
        lv_obj_t* save = find_widget("action_button");
        CHECK(disabled(save));

        // A tap on the greyed Save stores nothing and stays in the editor.
        UITest::click(save);
        process_lvgl(50);
        CHECK(MaterialSettingsManager::instance().get_override("ABS") == nullptr);
        CHECK(editing());

        lv_textarea_set_text(find_widget("edit_chamber_temp"), "60");
        CHECK_FALSE(disabled(save));

        REQUIRE(UITest::click(save));
        process_lvgl(50);
        const auto* ovr = MaterialSettingsManager::instance().get_override("ABS");
        REQUIRE(ovr != nullptr);
        REQUIRE(ovr->chamber_temp.has_value());
        CHECK(*ovr->chamber_temp == 60);
        CHECK_FALSE(editing());

        reset_material_temps_singleton();
        MaterialSettingsManager::instance().clear_override("ABS");
    }
}

TEST_CASE_METHOD(MaterialEditFixture, "Material edit Back asks before dropping unsaved edits",
                 "[material_temps][save_header]") {
    SECTION("no edits: Back returns to the list without asking") {
        open_abs();
        REQUIRE(UITest::click(find_widget("back_button")));
        process_lvgl(100);
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        CHECK_FALSE(editing());
    }
    SECTION("edits: Back asks, Cancel keeps them") {
        open_abs();
        lv_textarea_set_text(find_widget("edit_bed_temp"), "105");
        REQUIRE(UITest::click(find_widget("back_button")));
        process_lvgl(100);
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        CHECK(editing());

        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_secondary"), LV_EVENT_CLICKED, nullptr);
        process_lvgl(400);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(editing());
        CHECK(std::string(lv_textarea_get_text(find_widget("edit_bed_temp"))) == "105");
        CHECK_FALSE(disabled(find_widget("action_button")));
    }
    SECTION("edits: Discard returns to the list and stores nothing") {
        open_abs();
        lv_textarea_set_text(find_widget("edit_bed_temp"), "105");
        REQUIRE(UITest::click(find_widget("back_button")));
        process_lvgl(100);
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);

        lv_obj_send_event(lv_obj_find_by_name(dialog, "btn_primary"), LV_EVENT_CLICKED, nullptr);
        process_lvgl(400);
        helix::ui::UpdateQueue::instance().drain();
        CHECK_FALSE(editing());
        CHECK(MaterialSettingsManager::instance().get_override("ABS") == nullptr);
    }
    SECTION("edits: a backdrop tap leaves the edits and lets Back ask again") {
        open_abs();
        lv_textarea_set_text(find_widget("edit_bed_temp"), "105");
        REQUIRE(UITest::click(find_widget("back_button")));
        process_lvgl(100);
        lv_obj_t* dialog = ModalStack::instance().top_dialog();
        REQUIRE(dialog != nullptr);
        lv_obj_send_event(ModalStack::instance().backdrop_for(dialog), LV_EVENT_CLICKED, nullptr);
        process_lvgl(400);
        helix::ui::UpdateQueue::instance().drain();
        CHECK(ModalStack::instance().top_dialog() == nullptr);
        CHECK(editing());

        REQUIRE(UITest::click(find_widget("back_button")));
        process_lvgl(100);
        CHECK(ModalStack::instance().top_dialog() != nullptr);
    }
}

TEST_CASE_METHOD(MaterialEditFixture,
                 "Material edit Back asks again after code closes the discard prompt",
                 "[material_temps][save_header]") {
    open_abs();
    lv_textarea_set_text(find_widget("edit_bed_temp"), "105");
    REQUIRE(UITest::click(find_widget("back_button")));
    process_lvgl(100);
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);

    // Closed by code, not by a button or the backdrop: no callback of the
    // prompt's runs, and the overlay must not keep the freed dialog.
    Modal::hide(dialog);
    process_lvgl(600);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(ModalStack::instance().top_dialog() == nullptr);
    CHECK(editing());

    REQUIRE(UITest::click(find_widget("back_button")));
    process_lvgl(100);
    CHECK(ModalStack::instance().top_dialog() != nullptr);
}
