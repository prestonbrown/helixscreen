// Copyright (C) 2025-2026 356C LLC
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

#include "../lvgl_ui_test_fixture.h"
#include "../ui_test_utils.h"
#include "core/subject.h"
#include "core/update_queue.h"
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
        ui_ams_mini_status_set_slot_label(w, i, "PLA", 50 + i);
    }
}

/// Fills `count` slots with `material`, so the wide view's text column is sized
/// against a name of that length rather than the default's short one.
void fill_slots_material(lv_obj_t* w, int count, const char* material) {
    ui_ams_mini_status_set_slot_count(w, count);
    for (int i = 0; i < count; ++i) {
        ui_ams_mini_status_set_slot_label(w, i, material, 50 + i);
    }
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini bar mode: width bands pick bar width + visible cap",
                 "[ui][ams_mini][widget_size]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 100);
    helix::ui::UpdateQueue::instance().drain(); // flush any stray create-time auto-sync
    // Resolve the container's real width BEFORE any slot data triggers the
    // first rebuild — rebuild_bars() reads lv_obj_get_content_width() at call
    // time, so an unresolved (still-zero) container would clamp every bar
    // down to kMinBarWidthPx regardless of the width_px band under test.
    lv_obj_update_layout(w);
    fill_slots(w, 8);

    // width_px < 100: bar cap is width_px / 8 (11px), at most 6 of the 8 slots shown.
    ui_ams_mini_status_set_width(w, 90);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(w);

    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(bars));
    REQUIRE(lv_obj_get_width(lv_obj_get_child(bars, 0)) == 11);

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
    REQUIRE_FALSE(lv_obj_is_hidden(label));
    REQUIRE(std::string(lv_label_get_text(label)) == "+2");

    // 100 <= width_px < w_normal(): 13px bar cap, all 8 slots shown.
    ui_ams_mini_status_set_width(w, 110);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(w);

    REQUIRE(lv_obj_get_width(lv_obj_get_child(bars, 0)) == 13);
    REQUIRE(lv_obj_is_hidden(label)); // no overflow: all 8 fit

    lv_obj_delete(w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini mode dispatch: width_px vs w_normal() boundary",
                 "[ui][ams_mini][widget_size]") {
    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 100);
    helix::ui::UpdateQueue::instance().drain();
    fill_slots(w, 2);

    // Just below w_normal(): bar view.
    ui_ams_mini_status_set_width(w, helix::widget_size::w_normal() - 1);
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(UITest::find_by_name(w, "ams_spools_container") == nullptr);
    lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
    REQUIRE(bars != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(bars));

    // At w_normal(): spool view.
    ui_ams_mini_status_set_width(w, helix::widget_size::w_normal());
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* spools = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(spools != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(spools));
    REQUIRE(lv_obj_is_hidden(bars));

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
    constexpr int kHintPx = 300;
    auto lay_out_in = [this](int parent_w) {
        lv_obj_t* parent = lv_obj_create(test_screen());
        lv_obj_set_scrollable(parent, false);
        lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
        lv_obj_set_size(parent, parent_w, 100);

        lv_obj_t* w = ui_ams_mini_status_create(parent, 100);
        helix::ui::UpdateQueue::instance().drain();
        fill_slots(w, 6); // width-limited, not data-limited
        ui_ams_mini_status_set_width(w, kHintPx);
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

    auto [narrow_avail, narrow_cells] = lay_out_in(kHintPx);
    auto [wide_avail, wide_cells] = lay_out_in(kHintPx * 2);

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
    lv_obj_set_scrollable(wide_parent, false);
    lv_obj_set_style_pad_all(wide_parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(wide_parent, 0, LV_PART_MAIN);
    lv_obj_set_size(wide_parent, 600, 100);

    ui_ams_mini_status_init();
    lv_obj_t* w = ui_ams_mini_status_create(wide_parent, 100);
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

namespace {

/// What PanelWidgetManager actually hands the mini-status on each shipping
/// panel, at the size the DEFAULT LAYOUT gives it.
///
/// `ams` carries no anchor in any table in assets/config/default_layout.json,
/// so it auto-places at its registry default of 2x2 tracks — one cell — and
/// the width below is `grid_track_extent(cell_w, gutter, 2)` for that panel.
///
/// Measured, not derived: each row is the `[AmsMiniStatus] Width set to Npx`
/// line from `helix-screen --test -s <WxH> -vv` with the default mock AMS.
/// Re-measure that way rather than recomputing by hand — the width depends on
/// the container's content box, the gutter and the nav width, and every
/// attempt in this plan to predict one of those arithmetically has been wrong.
struct PanelCase {
    const char* name;
    UiBreakpoint bp;
    int width_px;
};

/// Pins the tier the widget reads while a case runs.
///
/// The mini-status resolves its own mode boundary through
/// `widget_size::w_normal()`, which reads the AMBIENT breakpoint subject — the
/// display it is being drawn on. The test harness has one small fixed display,
/// so without this every case is judged against Micro's 116px band and the
/// wide panels wrongly come out in spool view. Setting the subject is what
/// makes each case actually represent its panel.
///
/// Deliberately a local copy: test_default_layout.cpp and test_grid_edit_mode.cpp
/// each carry their own, the same per-TU convention as LayoutManagerTestAccess.
class BreakpointGuard {
  public:
    explicit BreakpointGuard(UiBreakpoint bp) {
        subj_ = theme_manager_get_breakpoint_subject();
        if (subj_) {
            if (auto* owner = helix::Subject::owner_of(subj_); owner && !owner->is_live()) {
                owner->init_int(0);
            }
            original_ = lv_subject_get_int(subj_);
            lv_subject_set_int(subj_, static_cast<int>(bp));
        }
    }
    ~BreakpointGuard() {
        if (subj_)
            lv_subject_set_int(subj_, original_);
    }
    BreakpointGuard(const BreakpointGuard&) = delete;
    BreakpointGuard& operator=(const BreakpointGuard&) = delete;

  private:
    lv_subject_t* subj_ = nullptr;
    int original_ = 0;
};

const std::vector<PanelCase> kShippingPanels = {
    {"micro 480x272", UiBreakpoint::Micro, 70},
    {"tiny 480x320", UiBreakpoint::Tiny, 82},
    {"small 480x400", UiBreakpoint::Small, 79},
    {"medium 800x480", UiBreakpoint::Medium, 114},
    {"large 1024x600", UiBreakpoint::Large, 107},
    {"xlarge 1280x720", UiBreakpoint::XLarge, 134},
    {"xxlarge 1920x1080", UiBreakpoint::XXLarge, 182},
    {"micro portrait 272x480", UiBreakpoint::Micro, 64},
    {"tiny portrait 320x480", UiBreakpoint::Tiny, 76},
    {"medium portrait 480x800", UiBreakpoint::Medium, 112},
    {"large portrait 600x1024", UiBreakpoint::Large, 112},
    {"tall 320x1480", UiBreakpoint::Tiny, 76},
    {"ultrawide 1480x320", UiBreakpoint::Tiny, 76},
    {"ultrawide 1920x440", UiBreakpoint::Small, 75},
};

} // namespace

// The answer to "how many spools does each panel show" is: none of them show
// spools at all. A one-cell widget is ~55-70% of w_normal() on every tier, and
// the spool view needs w_normal(), so MIN_SPOOL_W does not participate in the
// shipped layout — it only starts mattering once a user resizes the widget to
// two cells or more.
//
// Pinning it because it is load-bearing in the other direction: the bar view is
// what every AMS owner actually sees on the home screen, and a grid change that
// silently promoted one panel into the spool view would change the widget's
// whole character on that panel without touching this widget's code.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: every shipping panel gets the bar view",
                 "[ui][ams_mini][widget_size][1126]") {
    ui_ams_mini_status_init();

    for (const auto& c : kShippingPanels) {
        BreakpointGuard bp_guard(c.bp);
        INFO(c.name << " width " << c.width_px << "px, w_normal(" << static_cast<int>(c.bp)
                    << ") = " << helix::widget_size::w_normal(c.bp));

        // The mode boundary the widget itself uses, evaluated for that panel's
        // tier rather than the tier the test binary happens to run at.
        CHECK(c.width_px < helix::widget_size::w_normal(c.bp));

        lv_obj_t* w = ui_ams_mini_status_create(test_screen(), 100);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(w);
        fill_slots(w, 4); // the default mock AMS: Happy Hare, 4 slots

        ui_ams_mini_status_set_width(w, c.width_px);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(w);

        CHECK(UITest::find_by_name(w, "ams_spools_container") == nullptr);
        lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
        REQUIRE(bars != nullptr);
        CHECK_FALSE(lv_obj_is_hidden(bars));

        // All four slots fit at every shipping width — the <100px visible cap
        // is 6, so it never bites on a 4-slot system.
        CHECK(lv_obj_get_child_count(bars) >= 4);

        lv_obj_delete(w);
    }
}

// The bar-width cap is a fixed fraction of the widget's box, so four lanes fill
// the same share of their tile on every panel. Pinned per shipping panel because
// a step back to absolute pixel bands reads wrong on some screens and fine on
// others.
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini: bar width band per shipping panel",
                 "[ui][ams_mini][widget_size][1126]") {
    ui_ams_mini_status_init();

    struct Expect {
        const char* name;
        UiBreakpoint bp;
        int width_px;
        int bar_w;
    };
    // width_px / 8 against the measured widths above. Four lanes always fit
    // under that cap, so the cap is exactly what renders. Each case runs at its
    // own tier: the widest width is spool mode at the test binary's default.
    const std::vector<Expect> cases = {
        {"micro 480x272", UiBreakpoint::Micro, 70, 8},
        {"micro portrait 272x480", UiBreakpoint::Micro, 64, 8},
        {"small 480x400", UiBreakpoint::Small, 79, 9},
        {"ultrawide 1920x440", UiBreakpoint::Small, 75, 9},
        {"medium 800x480", UiBreakpoint::Medium, 114, 14},
        {"large 1024x600", UiBreakpoint::Large, 107, 13},
        {"xlarge 1280x720", UiBreakpoint::XLarge, 134, 16},
        {"xxlarge 1920x1080", UiBreakpoint::XXLarge, 182, 22},
    };

    for (const auto& c : cases) {
        BreakpointGuard bp_guard(c.bp);
        lv_obj_t* parent = lv_obj_create(test_screen());
        lv_obj_set_scrollable(parent, false);
        lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
        lv_obj_set_size(parent, c.width_px, 64);

        lv_obj_t* w = ui_ams_mini_status_create(parent, 100);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(parent);
        fill_slots(w, 4);
        ui_ams_mini_status_set_width(w, c.width_px);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(parent);

        lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container");
        REQUIRE(bars != nullptr);
        REQUIRE_FALSE(lv_obj_is_hidden(bars));
        REQUIRE(lv_obj_get_child_count(bars) >= 1);

        INFO(c.name << " width " << c.width_px << "px expects " << c.bar_w << "px bars");
        CHECK(lv_obj_get_width(lv_obj_get_child(bars, 0)) == c.bar_w);

        lv_obj_delete(w);
        lv_obj_delete(parent);
    }
}

/**
 * The spool cell reserves a text column (`text_w`) for the material label and
 * shrinks the spool graphic to make room for it. "Enough room" is measured in
 * `font_small`, which runs 10-26px across the tiers, so a reservation that under-
 * reserves lets `LV_LABEL_LONG_MODE_WRAP` break the string at whatever fits and the
 * label comes out one glyph per line, spilling over its neighbour.
 *
 * Where the name goes depends on how hard the row had to squeeze to seat every
 * lane, so each outcome is pinned here: a column beside the spool holds its
 * label on one line, a cell too narrow for one stacks the name over the spool at
 * full cell width, and a row too short for even that drops the name rather than
 * hiding lanes behind a scrollbar.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: material label fits its reserved column",
                 "[ui][ams_mini][widget_size]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);

    // Narrow axis 1080 -> XXLarge, where font_small is noto_sans_light_26. The
    // refresh moves the font tokens and the breakpoint subject to match; the
    // guard's destructor puts both back.
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
        lv_obj_set_scrollable(parent, false);
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

    SECTION("a squeezed row stacks the name over the spool") {
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
            // Over the spool, not beside it: as wide as the cell, which a
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
}

/**
 * The container's SIZE_CHANGED fires before its children are laid out, so the
 * spool row is still at its old width when the container announces the new
 * one. Cells sized there fit the old row: a row that narrowed scrolls by the
 * difference, and one that lost height to the widget label keeps a percent it
 * no longer has room for. The row's own size change is what resizes the cells.
 */
TEST_CASE_METHOD(LVGLUITestFixture, "ams_mini spool mode: cells follow a row that resizes",
                 "[ui][ams_mini][widget_size]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution medium(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_set_scrollable(parent, false);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
    lv_obj_set_size(parent, 336, 113);

    lv_obj_t* w = ui_ams_mini_status_create(parent, 113);
    // Hosted the way the home tile hosts it, with the row's height given by the
    // tile rather than by its contents.
    lv_obj_set_height(w, lv_pct(100));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    fill_slots(w, 6);
    ui_ams_mini_status_set_width(w, 336);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);

    lv_obj_t* sc = UITest::find_by_name(w, "ams_spools_container");
    REQUIRE(sc != nullptr);
    REQUIRE(lv_obj_get_scroll_right(sc) <= 0);

    lv_obj_set_width(parent, 328);
    lv_obj_update_layout(parent);
    INFO("row " << lv_obj_get_content_width(sc) << "px, cell "
                << lv_obj_get_width(UITest::find_by_name(w, "spool_cell_0")) << "px, scroll_right "
                << lv_obj_get_scroll_right(sc));
    CHECK(lv_obj_get_scroll_right(sc) <= 0);

    lv_obj_t* pct = UITest::find_by_name(w, "spool_pct_0");
    REQUIRE(pct != nullptr);
    REQUIRE_FALSE(lv_obj_is_hidden(pct));
    lv_obj_set_height(parent, 81);
    lv_obj_update_layout(parent);
    CHECK(lv_obj_is_hidden(pct));

    lv_obj_delete(w);
    lv_obj_delete(parent);
    theme_manager_refresh_layout_constants(disp);
}

namespace {

/// True when @p obj and every ancestor up to @p root are unhidden and it has a size.
bool shown_within(lv_obj_t* obj, lv_obj_t* root) {
    if (lv_obj_get_width(obj) <= 0 || lv_obj_get_height(obj) <= 0)
        return false;
    for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
            return false;
        if (o == root)
            return true;
    }
    return false;
}

/// Lanes the strip draws in whichever view it is in: bars or spool cells.
int drawn_lanes(lv_obj_t* w, int slot_count) {
    int n = 0;
    if (lv_obj_t* bars = UITest::find_by_name(w, "ams_bars_container")) {
        const uint32_t kids = lv_obj_get_child_count(bars);
        for (uint32_t i = 0; i < kids; ++i)
            n += shown_within(lv_obj_get_child(bars, static_cast<int32_t>(i)), w) ? 1 : 0;
    }
    for (int i = 0; i < slot_count; ++i) {
        const std::string name = "spool_cell_" + std::to_string(i);
        if (lv_obj_t* cell = UITest::find_by_name(w, name.c_str()))
            n += shown_within(cell, w) ? 1 : 0;
    }
    return n;
}

} // namespace

/**
 * A home tile resized in Edit Mode announces its new width before the grid
 * lays the tile out at it, so the strip first rebuilds against its old box.
 * Laying out the new box then resizes the strip in the middle of that rebuild.
 * At 480x272 a 4x1 tile is wide enough for the spool view but too short for a
 * usable spool, and the strip has to land on the bars, not on neither view.
 */
TEST_CASE_METHOD(LVGLUITestFixture,
                 "ams_mini: a tile widened from 1x1 to 4x1 at 480x272 draws its lanes",
                 "[ui][ams_mini][widget_size]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution micro(disp, 480, 272);
    theme_manager_refresh_layout_constants(disp);
    ui_ams_mini_status_init();

    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);
    lv_obj_set_size(parent, 70, 57);

    lv_obj_t* w = ui_ams_mini_status_create(parent, 57);
    lv_obj_set_size(w, lv_pct(100), lv_pct(100));
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    fill_slots(w, 4);
    ui_ams_mini_status_set_width(w, 70);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    REQUIRE(drawn_lanes(w, 4) == 4);

    // The manager's order: the width first, then the grid's layout pass.
    lv_obj_set_width(parent, 284);
    ui_ams_mini_status_set_width(w, 286);
    lv_obj_update_layout(parent);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(parent);
    CHECK(drawn_lanes(w, 4) == 4);

    lv_obj_delete(w);
    lv_obj_delete(parent);
    theme_manager_refresh_layout_constants(disp);
}
