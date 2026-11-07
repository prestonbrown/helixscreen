// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_spool_widget_wide.cpp
 * @brief Regression for #1109 — active_spool widget shows a default white spool
 *        when resized larger than 1x1.
 *
 * The widget has two spool_canvas objects: spool_compact (1x1) and spool_wide
 * (>=2 wide). update_spool_display() colors only the canvas selected by the
 * current is_wide_ mode. This test drives the reporter's flow — attach at 1x1,
 * then on_size_changed(2) — and asserts the *wide* canvas ends up carrying the
 * real filament color, not the kDefaultColor (0xE0E0E0) white.
 */

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "src/ui/panel_widgets/active_spool_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr uint32_t kTestSpoolColor = 0x00FF00; // distinct green, != kDefaultColor
constexpr uint32_t kDefaultWhite = 0xE0E0E0;   // ui_spool_canvas.cpp kDefaultColor

bool color_is(lv_color_t c, uint32_t rgb) {
    return c.red == ((rgb >> 16) & 0xFF) && c.green == ((rgb >> 8) & 0xFF) &&
           c.blue == (rgb & 0xFF);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "active_spool wide canvas is colored after resize to 2x1",
                 "[active_spool][panel_widget][1109]") {
    // Give the widget an active (external) spool with a known color.
    SlotInfo ext;
    ext.color_rgb = kTestSpoolColor;
    ext.material = "PLA";
    ext.brand = "TestBrand";
    ext.total_weight_g = 1000.0f;
    ext.remaining_weight_g = 500.0f;
    helix::AmsStateTestAccess::seed_external_spool(ext);

    // Build the controller, then its component, the way the panel manager
    // does: the widget's subjects must exist before the XML binds to them.
    ActiveSpoolWidget widget(api());
    lv_obj_t* comp = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_active_spool", widget.xml_attrs()));
    REQUIRE(comp != nullptr);
    widget.attach(comp, test_screen());

    lv_obj_t* spool_compact = lv_obj_find_by_name(comp, "spool_compact");
    lv_obj_t* spool_wide = lv_obj_find_by_name(comp, "spool_wide");
    REQUIRE(spool_compact != nullptr);
    REQUIRE(spool_wide != nullptr);

    // 1x1 (default): compact canvas carries the real color.
    widget.on_size_changed(1, 1, 100, 100);
    process_lvgl(30);
    INFO("compact color after 1x1: " << std::hex << ui_spool_canvas_get_color(spool_compact).red);
    REQUIRE(color_is(ui_spool_canvas_get_color(spool_compact), kTestSpoolColor));

    // Resize to 2x1 — the reporter's repro. The wide canvas must be colored,
    // not left at the kDefaultColor white.
    widget.on_size_changed(2, 1, 200, 100);
    process_lvgl(30);

    lv_color_t wide = ui_spool_canvas_get_color(spool_wide);
    INFO("wide color after 2x1: r=" << (int)wide.red << " g=" << (int)wide.green
                                    << " b=" << (int)wide.blue);
    REQUIRE_FALSE(color_is(wide, kDefaultWhite)); // the bug: stuck at default white
    REQUIRE(color_is(wide, kTestSpoolColor));

    AmsState::instance().clear_external_spool_info();
}

// The actual #1109 repro: a rebuild that RECYCLES the ActiveSpoolWidget instance
// (populate_widgets reuse path — save/exit edit mode, page/theme change, reconnect)
// re-attaches it to a FRESH component whose XML defaults are wide_layout hidden +
// spool_compact visible. Because is_wide_ persists as true, on_size_changed(2)
// early-returns and never unhides wide_layout, so the default-white spool_compact
// is left visible — the reported "static white spool".
TEST_CASE_METHOD(LVGLUITestFixture, "active_spool stays colored after a 2x1 instance is recycled",
                 "[active_spool][panel_widget][1109]") {
    SlotInfo ext;
    ext.color_rgb = kTestSpoolColor;
    ext.material = "PLA";
    ext.total_weight_g = 1000.0f;
    ext.remaining_weight_g = 500.0f;
    helix::AmsStateTestAccess::seed_external_spool(ext);

    ActiveSpoolWidget widget(api());

    // First placement at 2x1 — is_wide_ becomes true.
    lv_obj_t* comp1 = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_active_spool", widget.xml_attrs()));
    REQUIRE(comp1 != nullptr);
    widget.attach(comp1, test_screen());
    widget.on_size_changed(2, 1, 200, 100);
    process_lvgl(30);

    // Rebuild: destroy the old component, recycle the SAME widget onto a fresh one,
    // and re-run the manager's attach() + on_size_changed(colspan) sequence.
    lv_obj_delete(comp1);
    lv_obj_t* comp2 = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_active_spool", widget.xml_attrs()));
    REQUIRE(comp2 != nullptr);
    widget.attach(comp2, test_screen());
    widget.on_size_changed(2, 1, 200, 100);
    process_lvgl(30);

    lv_obj_t* wide2 = lv_obj_find_by_name(comp2, "spool_wide");
    lv_obj_t* wide_layout2 = lv_obj_find_by_name(comp2, "spoolman_wide_layout");
    lv_obj_t* compact2 = lv_obj_find_by_name(comp2, "spool_compact");
    REQUIRE(wide2 != nullptr);
    REQUIRE(wide_layout2 != nullptr);
    REQUIRE(compact2 != nullptr);

    // The wide layout must be visible and its canvas colored; the compact canvas
    // must be hidden (so its default-white state is never shown).
    INFO("wide_layout hidden=" << lv_obj_has_flag(wide_layout2, LV_OBJ_FLAG_HIDDEN)
                               << " compact hidden="
                               << lv_obj_has_flag(compact2, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(wide_layout2, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(compact2, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(color_is(ui_spool_canvas_get_color(wide2), kTestSpoolColor));

    AmsState::instance().clear_external_spool_info();
}

TEST_CASE_METHOD(LVGLUITestFixture, "active_spool's compact spool is sized by its box",
                 "[active_spool][panel_widget][tile]") {
    // The compact spool is a square one line of the tile's icon face, so a
    // bigger box draws a bigger spool, and a box too small for the smallest
    // face is refused rather than clipped.
    ActiveSpoolWidget widget(api());
    lv_obj_t* comp = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "panel_widget_active_spool", widget.xml_attrs()));
    REQUIRE(comp != nullptr);
    widget.attach(comp, test_screen());
    lv_obj_t* spool = lv_obj_find_by_name(comp, "spool_compact");
    REQUIRE(spool != nullptr);

    widget.on_size_changed(1, 1, 50, 50);
    lv_obj_update_layout(comp);
    const int small = lv_obj_get_height(spool);
    widget.on_size_changed(2, 2, 110, 110);
    lv_obj_update_layout(comp);
    const int large = lv_obj_get_height(spool);
    INFO("spool " << small << "px at 50x50, " << large << "px at 110x110");
    CHECK(large > small);
    CHECK(lv_obj_get_width(spool) == large);

    CHECK_FALSE(widget.fits_at(8, 8));
    CHECK(widget.fits_at(200, 200));
    widget.detach();
}
