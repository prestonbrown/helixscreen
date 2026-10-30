// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_control_buttons_layout.cpp
 * @brief Print Controls tile: button direction and label visibility per size.
 *
 * Run with: ./build/bin/helix-tests "[control_buttons]"
 */

#include "ui_button.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "helix/ui/text_metrics.h"
#include "lvgl.h"
#include "print_control_view.h"
#include "src/ui/panel_widgets/control_buttons_widget.h"
#include "system_settings_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

// Roughly one cell at 800x480: 112px track pairs, 8px padding and gap.
constexpr int kPad = 8;
constexpr int kGap = 8;
constexpr int kCellPx = 112;
constexpr int kTwoCellsPx = 233;
constexpr int kEnglishNeed = 90;

/// A ui_button's text label: its label child that is not the icon glyph.
lv_obj_t* text_label(lv_obj_t* btn) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class) && child != ui_button_get_icon(btn)) {
            return child;
        }
    }
    return nullptr;
}

/// Tracks tall for a pixel height in these cases: one cell or two.
int rows_for(int h) {
    return h > kCellPx ? 4 : 2;
}

ControlButtonsLayout layout_for(int colspan, int w, int h, int need = kEnglishNeed,
                                bool tiny = false) {
    return decide_control_buttons_layout(colspan, rows_for(h), w, h, kPad, kGap, need, tiny);
}

} // namespace

TEST_CASE("control buttons: one cell is two icon buttons side by side",
          "[control_buttons][layout]") {
    const auto l = layout_for(2, kCellPx, kCellPx);
    CHECK_FALSE(l.column);
    CHECK_FALSE(l.labels);
    CHECK_FALSE(l.fill);
}

TEST_CASE("control buttons: two cells wide show labels side by side", "[control_buttons][layout]") {
    const auto l = layout_for(4, kTwoCellsPx, kCellPx);
    CHECK_FALSE(l.column);
    CHECK(l.labels);
    // One cell tall keeps the large-button height.
    CHECK_FALSE(l.fill);
}

TEST_CASE("control buttons: one cell wide by two tall stacks icon buttons",
          "[control_buttons][layout]") {
    const auto l = layout_for(2, kCellPx, kTwoCellsPx);
    CHECK(l.column);
    CHECK(l.fill);
    // Under two cells wide the text goes, even though a stacked button spans
    // the full width.
    CHECK_FALSE(l.labels);
}

TEST_CASE("control buttons: a square two-cell tile stays side by side",
          "[control_buttons][layout]") {
    const auto l = layout_for(4, kTwoCellsPx, kTwoCellsPx);
    CHECK_FALSE(l.column);
    CHECK(l.labels);
    // Two cells tall: the buttons fill it rather than stopping at one row's height.
    CHECK(l.fill);
}

TEST_CASE("control buttons: a label too wide for its button is dropped",
          "[control_buttons][layout]") {
    // Each side-by-side button at two cells is (233 - 16 - 8) / 2 = 104px.
    CHECK(layout_for(4, kTwoCellsPx, kCellPx, 104).labels);
    CHECK_FALSE(layout_for(4, kTwoCellsPx, kCellPx, 105).labels);
    // A long translation ("Fortsetzen") needs more than the English label.
    CHECK_FALSE(layout_for(4, kTwoCellsPx, kCellPx, 130).labels);
}

TEST_CASE("control buttons: the Tiny breakpoint never shows labels", "[control_buttons][layout]") {
    CHECK_FALSE(layout_for(4, kTwoCellsPx, kCellPx, kEnglishNeed, true).labels);
    CHECK_FALSE(layout_for(8, 470, kCellPx, kEnglishNeed, true).labels);
}

TEST_CASE("control buttons: a tall two-cell-wide tile stacks labelled buttons",
          "[control_buttons][layout]") {
    const auto l = layout_for(4, kTwoCellsPx, 470);
    CHECK(l.column);
    CHECK(l.labels);
}

TEST_CASE_METHOD(LVGLUITestFixture, "control buttons tile lays its buttons out for its size",
                 "[control_buttons][panel_widget]") {
    PanelWidgetHarness<ControlButtonsWidget> h(test_screen());
    lv_obj_t* primary = h.child("btn_primary");
    lv_obj_t* stop = h.child("btn_stop");
    REQUIRE(primary != nullptr);
    REQUIRE(stop != nullptr);
    lv_obj_t* stop_label = text_label(stop);
    REQUIRE(stop_label != nullptr);

    auto inside_root = [&](lv_obj_t* btn) {
        lv_area_t r, b;
        lv_obj_get_coords(h.root(), &r);
        lv_obj_get_coords(btn, &b);
        return b.x1 >= r.x1 && b.y1 >= r.y1 && b.x2 <= r.x2 && b.y2 <= r.y2;
    };

    SECTION("one cell wide by two tall stacks them, icons only") {
        h.resize(2, 4, 120, 250);
        CHECK(lv_obj_get_y(stop) > lv_obj_get_y(primary));
        CHECK(lv_obj_get_x(stop) == lv_obj_get_x(primary));
        CHECK(lv_obj_has_flag(stop_label, LV_OBJ_FLAG_HIDDEN));
        CHECK(inside_root(primary));
        CHECK(inside_root(stop));
    }

    SECTION("two cells wide puts them side by side with labels") {
        h.resize(4, 2, 300, 120);
        CHECK(lv_obj_get_x(stop) > lv_obj_get_x(primary));
        CHECK(lv_obj_get_y(stop) == lv_obj_get_y(primary));
        CHECK_FALSE(lv_obj_has_flag(stop_label, LV_OBJ_FLAG_HIDDEN));
        CHECK(inside_root(primary));
        CHECK(inside_root(stop));
    }

    SECTION("two cells tall side by side fills the height") {
        h.resize(4, 4, 300, 300);
        CHECK(lv_obj_get_x(stop) > lv_obj_get_x(primary));
        const int32_t inner_h = lv_obj_get_content_height(h.root());
        CHECK(lv_obj_get_height(primary) == inner_h);
        CHECK(lv_obj_get_height(stop) == inner_h);
        CHECK(inside_root(stop));
    }

    SECTION("one cell tall side by side stays capped") {
        h.resize(4, 2, 300, 300);
        CHECK(lv_obj_get_height(stop) < lv_obj_get_content_height(h.root()));
    }

    SECTION("one cell is side by side, icons only") {
        h.resize(2, 2, 120, 120);
        CHECK(lv_obj_get_x(stop) > lv_obj_get_x(primary));
        CHECK(lv_obj_has_flag(stop_label, LV_OBJ_FLAG_HIDDEN));
        CHECK(inside_root(stop));
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "control buttons size labels for the transient labels too",
                 "[control_buttons][panel_widget]") {
    PanelWidgetHarness<ControlButtonsWidget> h(test_screen());
    lv_obj_t* primary = h.child("btn_primary");
    REQUIRE(primary != nullptr);
    lv_obj_t* label = text_label(primary);
    REQUIRE(label != nullptr);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);

    // "Resuming..." is wider than the steady labels; a button sized only for
    // Pause/Resume overflows the moment a resume is in flight.
    const int resuming =
        static_cast<int>(helix::ui::text_width(lv_tr(helix::ui::kControlLabelResuming), font));
    const int resume =
        static_cast<int>(helix::ui::text_width(lv_tr(helix::ui::kControlLabelResume), font));
    REQUIRE(resuming > resume);
    CHECK(h.widget().button_need_px() > resuming);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "control buttons re-take the label verdict on a language change",
                 "[control_buttons][panel_widget]") {
    PanelWidgetHarness<ControlButtonsWidget> h(test_screen());
    h.resize(4, 2, 300, 120);
    lv_subject_t* labels = lv_xml_get_subject(nullptr, "control_buttons_labels");
    REQUIRE(labels != nullptr);
    REQUIRE(lv_subject_get_int(labels) == 1);

    // A stale verdict, as a language switch without a resize would leave it.
    lv_subject_set_int(labels, 0);
    lv_subject_t* language = SystemSettingsManager::instance().subject_language();
    REQUIRE(language != nullptr);
    const int32_t saved = lv_subject_get_int(language);
    lv_subject_set_int(language, saved == 0 ? 1 : 0);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(lv_subject_get_int(labels) == 1);
    lv_subject_set_int(language, saved);
    helix::ui::UpdateQueue::instance().drain();
}
