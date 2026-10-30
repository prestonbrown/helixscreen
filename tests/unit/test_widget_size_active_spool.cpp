// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_active_spool.cpp
 * @brief active_spool picks compact/wide layout from physical pixels, not
 * colspan, and the recycle path (#1109) survives the swap.
 *
 * `on_size_changed` (active_spool_widget.cpp:135) computes
 * `wide = width_px >= w_normal()` and memoizes it in `is_wide_` *before*
 * checking `widget_obj_` is non-null (:136-141). `attach()` calls
 * `apply_layout_visibility()` unconditionally to paper over a call that
 * landed while unattached (see the comment at :95-101 and issue #1109) —
 * that ordering bug is out of scope here and is left untouched.
 *
 * Every object `is_wide_` drives is asserted:
 *   - `spoolman_wide_layout` / `spool_compact` hidden flags
 *     (apply_layout_visibility)
 *   - whichever spool canvas (`spool_wide` / `spool_compact`) carries the
 *     real filament color (update_spool_display's `active_spool` pick)
 *   - `spoolman_no_spool_label` hidden flag and `spoolman_material` text,
 *     both of which only branch on `is_wide_` in the no-spool case
 *     (update_spool_display)
 *
 * The first two cases use spans that contradict the pixel verdict
 * (`colspan>=2` would land the opposite mode), so an implementation that
 * still reads `colspan` fails here instead of passing by coincidence.
 *
 * The third case is the actual #1109 shape: attach, resize wide, delete the
 * component, re-attach the SAME widget instance to a fresh component, resize
 * wide again. A fresh component's XML defaults to wide_layout hidden +
 * spool_compact visible; because `is_wide_` is already true from before the
 * recycle, on_size_changed's memo check early-returns and the only thing
 * that can still fix the visible state is attach()'s apply_layout_visibility()
 * call. This test is what the apply_layout_visibility() mutation (deleting
 * that call from attach()) is meant to turn red.
 */

#include "ui_spool_canvas.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_widget_labels.h"
#include "ams_state.h"
#include "ams_types.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "panel_widget_size.h"
#include "src/ui/panel_widgets/active_spool_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::widget_size;

namespace {

constexpr uint32_t kTestSpoolColor = 0x00FF00; // distinct green, != kDefaultColor
constexpr uint32_t kDefaultWhite = 0xE0E0E0;   // ui_spool_canvas.cpp kDefaultColor

bool color_is(lv_color_t c, uint32_t rgb) {
    return c.red == ((rgb >> 16) & 0xFF) && c.green == ((rgb >> 8) & 0xFF) &&
           c.blue == (rgb & 0xFF);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "active_spool hidden flags and canvas color follow pixels, not spans",
                 "[widget_size][active_spool][panel_widget]") {
    SlotInfo ext;
    ext.color_rgb = kTestSpoolColor;
    ext.material = "PLA";
    ext.brand = "TestBrand";
    ext.total_weight_g = 1000.0f;
    ext.remaining_weight_g = 500.0f;
    AmsState::instance().set_external_spool_info_in_memory(ext);

    PanelWidgetHarness<ActiveSpoolWidget> h(test_screen(), api());

    lv_obj_t* wide_layout = h.child("spoolman_wide_layout");
    lv_obj_t* spool_compact = h.child("spool_compact");
    lv_obj_t* spool_wide = h.child("spool_wide");
    REQUIRE(wide_layout != nullptr);
    REQUIRE(spool_compact != nullptr);
    REQUIRE(spool_wide != nullptr);

    // attach() applies compact defaults (XML: wide_layout hidden, compact shown)
    // and colors the compact canvas immediately.
    CHECK(lv_obj_has_flag(wide_layout, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(spool_compact, LV_OBJ_FLAG_HIDDEN));
    CHECK(color_is(ui_spool_canvas_get_color(spool_compact), kTestSpoolColor));

    // Wide by pixel, contradicting span (colspan=1 -> old predicate stays compact).
    h.resize(1, 1, w_normal(), h_tall());
    CHECK_FALSE(lv_obj_has_flag(wide_layout, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(spool_compact, LV_OBJ_FLAG_HIDDEN));
    CHECK(color_is(ui_spool_canvas_get_color(spool_wide), kTestSpoolColor));

    // Back to compact by pixel, contradicting span (colspan=3 -> old predicate goes wide).
    h.resize(3, 3, w_normal() - 1, h_tall() - 1);
    CHECK(lv_obj_has_flag(wide_layout, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(spool_compact, LV_OBJ_FLAG_HIDDEN));
    CHECK(color_is(ui_spool_canvas_get_color(spool_compact), kTestSpoolColor));

    AmsState::instance().clear_external_spool_info();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "active_spool no-spool label and material text follow pixels, not spans",
                 "[widget_size][active_spool][panel_widget]") {
    AmsState::instance().clear_external_spool_info();

    PanelWidgetHarness<ActiveSpoolWidget> h(test_screen(), api());

    lv_obj_t* no_spool_label = h.child("spoolman_no_spool_label");
    lv_obj_t* material_label = h.child("spoolman_material");
    REQUIRE(no_spool_label != nullptr);
    REQUIRE(material_label != nullptr);

    // Compact + no spool: label visible, material text blank.
    CHECK_FALSE(lv_obj_has_flag(no_spool_label, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(material_label)).empty());

    // Wide by pixel, contradicting span (colspan=1).
    h.resize(1, 1, w_normal(), h_tall());
    CHECK(lv_obj_has_flag(no_spool_label, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(material_label)) == lv_tr("No Spool"));

    // Back to compact by pixel, contradicting span (colspan=3).
    h.resize(3, 3, w_normal() - 1, h_tall() - 1);
    CHECK_FALSE(lv_obj_has_flag(no_spool_label, LV_OBJ_FLAG_HIDDEN));
    CHECK(std::string(lv_label_get_text(material_label)).empty());
}

// The #1109 shape, driven through the pixel predicate. A rebuild that
// RECYCLES the ActiveSpoolWidget instance (populate_widgets reuse path --
// save/exit edit mode, page/theme change, reconnect) re-attaches it to a
// FRESH component whose XML defaults are wide_layout hidden + spool_compact
// visible. Because is_wide_ persists as true, on_size_changed's memo check
// early-returns and never unhides wide_layout on its own -- only attach()'s
// apply_layout_visibility() call fixes it up.
TEST_CASE_METHOD(LVGLUITestFixture, "active_spool stays colored after a wide instance is recycled",
                 "[active_spool][panel_widget][1109]") {
    SlotInfo ext;
    ext.color_rgb = kTestSpoolColor;
    ext.material = "PLA";
    ext.total_weight_g = 1000.0f;
    ext.remaining_weight_g = 500.0f;
    AmsState::instance().set_external_spool_info_in_memory(ext);

    ActiveSpoolWidget widget(api());

    // First placement, wide by pixel -- is_wide_ becomes true.
    lv_obj_t* comp1 =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_active_spool", nullptr));
    REQUIRE(comp1 != nullptr);
    widget.attach(comp1, test_screen());
    widget.on_size_changed(2, 1, w_normal(), h_tall());
    process_lvgl(30);

    // Rebuild: destroy the old component, recycle the SAME widget onto a fresh
    // one, and re-run the manager's attach() + on_size_changed(pixels) sequence.
    lv_obj_delete(comp1);
    lv_obj_t* comp2 =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_active_spool", nullptr));
    REQUIRE(comp2 != nullptr);
    widget.attach(comp2, test_screen());
    widget.on_size_changed(2, 1, w_normal(), h_tall());
    process_lvgl(30);

    lv_obj_t* wide2 = lv_obj_find_by_name(comp2, "spool_wide");
    lv_obj_t* wide_layout2 = lv_obj_find_by_name(comp2, "spoolman_wide_layout");
    lv_obj_t* compact2 = lv_obj_find_by_name(comp2, "spool_compact");
    REQUIRE(wide2 != nullptr);
    REQUIRE(wide_layout2 != nullptr);
    REQUIRE(compact2 != nullptr);

    // The wide layout must be visible and its canvas colored; the compact
    // canvas must be hidden (so its default-white state is never shown).
    INFO("wide_layout hidden=" << lv_obj_has_flag(wide_layout2, LV_OBJ_FLAG_HIDDEN)
                               << " compact hidden="
                               << lv_obj_has_flag(compact2, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(lv_obj_has_flag(wide_layout2, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(compact2, LV_OBJ_FLAG_HIDDEN));
    REQUIRE_FALSE(color_is(ui_spool_canvas_get_color(wide2), kDefaultWhite));
    REQUIRE(color_is(ui_spool_canvas_get_color(wide2), kTestSpoolColor));

    AmsState::instance().clear_external_spool_info();
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "active_spool: a box draws the same parts whatever it was before",
                 "[widget_size][active_spool][1805]") {
    helix::test::ScopedWidgetLabelsShown labels;
    PanelWidgetHarness<ActiveSpoolWidget> h(test_screen(), api());
    CHECK(h.unsettled_boxes(box_sweep(173, 173, 20, 240)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(60, 60, 20, 240)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(20, 300, 29, 29)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(20, 300, 60, 60)) == std::vector<std::string>{});
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
