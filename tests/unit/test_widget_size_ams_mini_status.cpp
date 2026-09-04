// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_ams_mini_status.cpp
 * @brief Pixel-width thresholds for the ams_mini_status widget.
 *
 * ams_mini_status is a pure-XML widget driven through a C API, not a
 * PanelWidget subclass, so it does not use PanelWidgetHarness<W>. It already
 * takes a plain `width_px` and derives everything -- bar-width band, visible
 * bar cap, BAR/SPOOL mode, and the spool-cell count in the wide view -- from
 * that single pixel value plus (for the spool view) the real laid-out width
 * of its own container.
 */

#include "ui_ams_mini_status.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "panel_widget_size.h"
#include "theme_manager.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

/// Fills `count` slots (1..8) with arbitrary-but-present data so bar/spool
/// rendering has something to draw.
void fill_slots(lv_obj_t* w, int count) {
    ui_ams_mini_status_set_slot_count(w, count);
    for (int i = 0; i < count; ++i) {
        ui_ams_mini_status_set_slot_full(w, i, 0xFF0000 + i, 50 + i, true, "PLA", 50 + i);
    }
}

/// Fills `count` slots with `material`, so the wide view's text column is sized
/// against a name of that length rather than the default's short one.
void fill_slots_material(lv_obj_t* w, int count, const char* material) {
    ui_ams_mini_status_set_slot_count(w, count);
    for (int i = 0; i < count; ++i) {
        ui_ams_mini_status_set_slot_full(w, i, 0xFF0000 + i, 50 + i, true, material, 50 + i);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini bar mode: width bands pick bar width + visible cap",
                 "[ui][ams_mini][widget_size]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain(); // flush any stray create-time auto-sync
    // Resolve the container's real width BEFORE any slot data triggers the
    // first rebuild — rebuild_bars() reads lv_obj_get_content_width() at call
    // time, so an unresolved (still-zero) container would clamp every bar
    // down to MIN_BAR_WIDTH_PX regardless of the width_px band under test.
    lv_obj_update_layout(w);
    fill_slots(w, 8);

    // Tight band: width_px < 100 -> 8px bars, at most 6 of the 8 slots shown.
    ui_ams_mini_status_set_width(w, 90);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(w);

    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(bars, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_get_width(lv_obj_get_child(bars, 0)) == 8);

    // 8 slots, max 6 visible -> "+2" overflow badge, visible and non-empty.
    // overflow_label isn't named, so find it by type among the container's children.
    lv_obj_t* label = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(w); ++i) {
        lv_obj_t* child = lv_obj_get_child(w, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            label = child;
            break;
        }
    }
    REQUIRE(label != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(std::string(lv_label_get_text(label)) == "+2");

    // Medium band: 100 <= width_px < w_normal() -> 10px bars, all 8 slots shown
    // (the old <150 branch's min(max_visible, 8) was a no-op; removing it
    // must not change this — max_visible was already clamped to 8).
    ui_ams_mini_status_set_width(w, 110);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(w);

    REQUIRE(lv_obj_get_width(lv_obj_get_child(bars, 0)) == 10);
    REQUIRE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)); // no overflow: all 8 fit

    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini mode dispatch: width_px vs w_normal() boundary",
                 "[ui][ams_mini][widget_size]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 60);
    helix::ui::UpdateQueue::instance().drain();
    fill_slots(w, 2);

    // Just below w_normal(): bar view.
    ui_ams_mini_status_set_width(w, helix::widget_size::w_normal() - 1);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(UITest::find_by_name(w, "ams_spools_container") == nullptr);
    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(bars, LV_OBJ_FLAG_HIDDEN));

    // At w_normal(): spool view.
    ui_ams_mini_status_set_width(w, helix::widget_size::w_normal());
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE_FALSE(lv_obj_has_flag(spools, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(lv_obj_has_flag(bars, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(w);
}

/**
 * The spool count comes from the container's REAL laid-out width, not from the
 * `width_px` hint the manager pushes. That hint is a cell_w*colspan estimate and
 * runs a few pixels wide, which is enough on its own to tip the row into a
 * scrollbar.
 *
 * Two containers of different real widths handed the SAME width_px must
 * therefore lay out differently. That is the seam; re-deriving the widget's own
 * arithmetic here would pass just as happily against a hint-driven layout.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: cell width derives from real width",
                 "[ui][ams_mini][widget_size]") {
    ui_ams_mini_status_init();

    // Same width_px hint into both, only the real parent width differs.
    constexpr int HINT_PX = 300;
    auto lay_out_in = [this](int parent_w) {
        lv_obj_t* parent = lv_obj_create(test_screen());
        lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
        lv_obj_set_size(parent, parent_w, 60);

        lv_obj_t* w = ui_ams_mini_status_create(parent, 60);
        helix::ui::UpdateQueue::instance().drain();
        fill_slots(w, 6); // width-limited, not data-limited
        ui_ams_mini_status_set_width(w, HINT_PX);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(parent);

        lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
        REQUIRE(sc != nullptr);

        std::vector<int> widths;
        for (int i = 0; i < 6; ++i) {
            lv_obj_t* c = UITest::find_by_name(w, ("spool_cell_" + std::to_string(i)).c_str());
            if (c)
                widths.push_back(lv_obj_get_width(c));
        }
        const int avail = lv_obj_get_content_width(sc);
        lv_obj_delete(w);
        lv_obj_delete(parent);
        return std::pair<int, std::vector<int>>{avail, widths};
    };

    auto [narrow_avail, narrow_cells] = lay_out_in(HINT_PX);
    auto [wide_avail, wide_cells] = lay_out_in(HINT_PX * 2);

    REQUIRE(narrow_avail > 0);
    REQUIRE(wide_avail > narrow_avail);
    REQUIRE_FALSE(narrow_cells.empty());
    REQUIRE_FALSE(wide_cells.empty());

    // Every cell in a row is one equal share of it.
    for (int cw : narrow_cells)
        CHECK(cw == narrow_cells.front());
    for (int cw : wide_cells)
        CHECK(cw == wide_cells.front());

    // The wider container spends its extra width on the row rather than
    // ignoring it in favour of the (identical) hint.
    INFO("narrow " << narrow_avail << "px -> " << narrow_cells.size() << " cells of "
                   << narrow_cells.front() << "px; wide " << wide_avail << "px -> "
                   << wide_cells.size() << " cells of " << wide_cells.front() << "px");
    CHECK(wide_cells.front() * static_cast<int>(wide_cells.size()) >
          narrow_cells.front() * static_cast<int>(narrow_cells.size()));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini spool mode: sparse slot_count caps visible below width capacity",
                 "[ui][ams_mini][widget_size]") {
    // A container wide enough for several spool cells, but only 1 real slot:
    // the derived visible count must not exceed the actual slot_count (no
    // blank reserved columns for spools that don't exist).
    lv_obj_t* wide_parent = lv_obj_create(test_screen());
    lv_obj_remove_flag(wide_parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(wide_parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(wide_parent, 0, LV_PART_MAIN);
    lv_obj_set_size(wide_parent, 600, 60);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(wide_parent, 60);
    helix::ui::UpdateQueue::instance().drain();
    fill_slots(w, 1);

    ui_ams_mini_status_set_width(w, 600);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(wide_parent);

    lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(sc != nullptr);
    REQUIRE(lv_obj_get_child_count(sc) == 1);

    lv_obj_t* cell0 = UITest::find_by_name(w, "spool_cell_0");
    REQUIRE(cell0 != nullptr);
    int avail_w = lv_obj_get_content_width(sc);
    // Single slot fills the whole row (minus the -2px safety margin), not the
    // min-cell-sized fraction a width-limited row would use.
    REQUIRE(lv_obj_get_width(cell0) >= avail_w - 4);

    lv_obj_delete(w);
    lv_obj_delete(wide_parent);
}

/**
 * The spool cell reserves a text column (`text_w`) for the material label and
 * shrinks the spool graphic to make room for it. "Enough room" is measured in
 * `font_small`, which runs 10-26px across the tiers, so a reservation that under-
 * reserves lets `LV_LABEL_LONG_WRAP` break the string at whatever fits and the
 * label comes out one glyph per line, spilling over its neighbour.
 *
 * Where the name goes depends on how hard the row had to squeeze to seat every
 * lane, so each outcome is pinned here: a column beside the spool holds its
 * label on one line, a cell too narrow for one stacks the name underneath at
 * full cell width, and a row too short for even that drops the name rather than
 * hiding lanes behind a scrollbar.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: material label fits its reserved column",
                 "[ui][ams_mini][widget_size]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // Narrow axis 1080 -> XXLarge, where font_small is noto_sans_light_26.
    // The refresh is what actually moves the font tokens and the breakpoint
    // subject; ScopedResolution alone only changes the pixel dimensions.
    ScopedResolution xxlarge(disp, 1080, 1920);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    // Build at the target height rather than resizing afterwards: a widget that
    // has already rendered keeps its cells when the resize collapses its
    // container, so a mutated parent measures the previous layout.
    struct Row {
        lv_obj_t* parent;
        lv_obj_t* w;
    };
    auto row_of = [this](int h, int lanes) {
        lv_obj_t* parent = lv_obj_create(test_screen());
        lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
        lv_obj_set_size(parent, 480, h);

        lv_obj_t* w = ui_ams_mini_status_create(parent, h);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(parent);
        fill_slots(w, lanes);
        ui_ams_mini_status_set_width(w, 480);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(parent);
        return Row{parent, w};
    };

    SECTION("a reserved column holds its label on one line") {
        // Few enough lanes that the row seats them all without squeezing, so
        // the column sits beside the spool, sized to the widest name.
        Row r = row_of(120, 3);
        lv_obj_t* mat = UITest::find_by_name(r.w, "spool_material_0");
        REQUIRE(mat != nullptr);
        const lv_font_t* font = lv_obj_get_style_text_font(mat, LV_PART_MAIN);
        REQUIRE(font != nullptr);

        lv_point_t text_size;
        lv_text_get_size(&text_size, lv_label_get_text(mat), font,
                         lv_obj_get_style_text_letter_space(mat, LV_PART_MAIN),
                         lv_obj_get_style_text_line_space(mat, LV_PART_MAIN), LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        INFO("text column " << lv_obj_get_content_width(mat) << "px, '" << lv_label_get_text(mat)
                            << "' measures " << text_size.x << "px, line height "
                            << lv_font_get_line_height(font) << "px");

        // The reserved column holds the string the cell is about to draw...
        CHECK(text_size.x <= lv_obj_get_content_width(mat));
        // ...and the symptom that follows from it: one line, measured off the
        // laid-out object rather than by re-deriving the arithmetic under test.
        CHECK(lv_obj_get_height(mat) < 2 * lv_font_get_line_height(font));

        lv_obj_delete(r.w);
        lv_obj_delete(r.parent);
    }

    SECTION("a squeezed row stacks the name under the spool") {
        // Eight lanes in 480px at 26px type: no column fits BESIDE a spool, so
        // the cell turns vertical and the name takes the full cell width.
        Row r = row_of(120, 8);
        lv_obj_t* sc = UITest::find_by_name(r.w, "ams_spools_container");
        REQUIRE(sc != nullptr);
        for (int i = 0; i < 8; ++i) {
            INFO("lane " << i);
            lv_obj_t* cell = UITest::find_by_name(r.w, ("spool_cell_" + std::to_string(i)).c_str());
            REQUIRE(cell != nullptr);
            lv_obj_t* mat =
                UITest::find_by_name(r.w, ("spool_material_" + std::to_string(i)).c_str());
            REQUIRE(mat != nullptr);
            // Under the spool, not beside it: as wide as the cell, which a
            // side-by-side column never is.
            CHECK(lv_obj_get_width(mat) == lv_obj_get_width(cell));
            // A column-flow cell spaces its children by pad_row, and the height
            // budget that decided to stack counts exactly one `gap` there. Any
            // other value and the text block runs past the bottom of the cell.
            CHECK(lv_obj_get_style_pad_row(cell, LV_PART_MAIN) ==
                  theme_manager_get_spacing("space_xxs"));
        }
        INFO("scroll_right " << lv_obj_get_scroll_right(sc));
        CHECK(lv_obj_get_scroll_right(sc) <= 0);

        lv_obj_delete(r.w);
        lv_obj_delete(r.parent);
    }

    // Put the token table back where the rest of the suite expects it.
    theme_manager_refresh_layout_constants(disp);
}

/**
 * Under the spool is the last place a name can go. A row with no height for a
 * line of text beneath a spool has nowhere left to put one, so it spends the
 * width on the spools and every lane still gets a cell rather than some lanes
 * getting a name and the rest getting a scrollbar.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: a row too short to stack drops the name",
                 "[ui][ams_mini][widget_size]") {
    ui_ams_mini_status_init();

    // The create height is the row height the widget sizes against - it is what
    // `avail_h` falls back to before a laid-out height resolves. 50px leaves no
    // room for a spool with a line of text under it.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
    lv_obj_set_size(parent, 480, 50);

    lv_obj_t* w = ui_ams_mini_status_create(parent, 50);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    fill_slots(w, 8);
    ui_ams_mini_status_set_width(w, 480);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);

    lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(sc != nullptr); // spool mode, so there is a row to squeeze
    INFO("scroll_right " << lv_obj_get_scroll_right(sc));

    for (int i = 0; i < 8; ++i) {
        INFO("lane " << i);
        REQUIRE(UITest::find_by_name(w, ("spool_cell_" + std::to_string(i)).c_str()) != nullptr);
        CHECK(UITest::find_by_name(w, ("spool_material_" + std::to_string(i)).c_str()) == nullptr);
    }
    CHECK(lv_obj_get_scroll_right(sc) <= 0);

    lv_obj_delete(w);
    lv_obj_delete(parent);
}

/**
 * A four-lane system in a widget the home grid sizes to a third of an 800x480
 * screen. The material name alone decides how many cells the row admits:
 * min_spool_cell_w() reserves the widest name in full on EVERY cell, so one
 * "PETG-GF" lane widens all four past what the row holds and the strip scrolls
 * with the user's own lanes hidden behind it (prestonbrown/helixscreen#1434).
 *
 * The lane badge and spool colour are what this row exists to show; spelling
 * the material in full is not worth hiding half the lanes. While the squeezed
 * text column still clears a readable floor, every lane gets a cell and the row
 * does not scroll.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: every lane fits before the row scrolls",
                 "[ui][ams_mini][widget_size]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);

    // The width a four-track AMS tile gets on the twelve-track home grid at
    // this resolution, measured off the running app.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
    lv_obj_set_size(parent, 228, 54);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(parent, 228);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    fill_slots_material(w, 4, "PETG-GF");

    ui_ams_mini_status_set_width(w, 228);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);

    lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(sc != nullptr);

    // Every lane drew a cell...
    for (int i = 0; i < 4; ++i) {
        INFO("lane " << i);
        REQUIRE(UITest::find_by_name(w, ("spool_cell_" + std::to_string(i)).c_str()) != nullptr);
    }

    const int avail_w = lv_obj_get_content_width(sc);
    int content = 0;
    for (int i = 0; i < 4; ++i) {
        content +=
            lv_obj_get_width(UITest::find_by_name(w, ("spool_cell_" + std::to_string(i)).c_str()));
    }
    content += 3 * theme_manager_get_spacing("space_xxs");
    INFO("container " << avail_w << "px, four cells + gaps " << content << "px, scroll_right "
                      << lv_obj_get_scroll_right(sc));

    // ...and they fit, so the row has nothing to scroll to.
    CHECK(content <= avail_w);
    CHECK(lv_obj_get_scroll_right(sc) <= 0);

    lv_obj_delete(w);
    lv_obj_delete(parent);
    theme_manager_refresh_layout_constants(disp);
}

/**
 * Between "the widest name fits every cell" and "no name fits any cell" there
 * is a band where a shortened name still reads: a short row keeps the spool
 * small, which leaves width the name can use even though the full string would
 * have pushed a lane off the row.
 *
 * A name arriving here ellipsized rather than wrapped is the whole point -
 * LV_LABEL_LONG_DOT only cuts once the label's height stops it wrapping, so a
 * label left free to grow taller silently wraps instead and the dots never
 * appear.
 */
TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini spool mode: a squeezed name is ellipsized, not wrapped",
                 "[ui][ams_mini][widget_size]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution xxlarge(disp, 1080, 1920); // font_small = 26px
    theme_manager_refresh_layout_constants(disp);

    // Short row -> small spool -> width left over for a shortened name, and a
    // container too narrow to spell the full one on all four cells.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
    lv_obj_set_size(parent, 600, 44);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(parent, 44);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    fill_slots_material(w, 4, "PETG Carbon Fiber");

    ui_ams_mini_status_set_width(w, 600);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);

    lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(sc != nullptr);
    lv_obj_t* mat = UITest::find_by_name(w, "spool_material_0");
    REQUIRE(mat != nullptr); // the band keeps a column

    const lv_font_t* font = lv_obj_get_style_text_font(mat, LV_PART_MAIN);
    REQUIRE(font != nullptr);
    const char* shown = lv_label_get_text(mat);
    INFO("column " << lv_obj_get_content_width(mat) << "px shows '" << shown << "', label height "
                   << lv_obj_get_height(mat) << "px, line height " << lv_font_get_line_height(font)
                   << "px, scroll_right " << lv_obj_get_scroll_right(sc));

    // LV_LABEL_LONG_DOT rewrites the label's own buffer with the shortened
    // string, so the truncation is observable as the text it will draw.
    CHECK(std::string(shown) != "PETG Carbon Fiber");
    CHECK(std::string(shown).find("...") != std::string::npos);
    // One line, not a wrapped stack.
    CHECK(lv_obj_get_height(mat) < 2 * lv_font_get_line_height(font));
    // And the lanes it was squeezed for are all on the row.
    for (int i = 0; i < 4; ++i)
        CHECK(UITest::find_by_name(w, ("spool_cell_" + std::to_string(i)).c_str()) != nullptr);
    CHECK(lv_obj_get_scroll_right(sc) <= 0);

    lv_obj_delete(w);
    lv_obj_delete(parent);
    theme_manager_refresh_layout_constants(disp);
}
