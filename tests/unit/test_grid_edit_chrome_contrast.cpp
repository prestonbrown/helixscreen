// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_chrome_contrast.cpp
 * @brief Renders the real edit-mode selection chrome and measures its icons
 *
 * The chrome pills are translucent, so the pass/fail question is the WCAG
 * ratio of each rendered icon against its pill composited over the surface
 * beneath, asserted here from the live widget tree, independently of the
 * theme helper that picks the colour (a helper could be self-consistent and
 * still pick a colour nobody can read on screen).
 */

#include "ui_breakpoint.h"

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../ui_test_utils.h"
#include "config.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// A PanelWidget that advertises edit-mode configuration, so the selection
/// chrome draws the configure pill beside the trash pill.
class ConfigurableWidget : public PanelWidget {
  public:
    void attach(lv_obj_t*, lv_obj_t*) override {}
    void detach() override {}
    const char* id() const override {
        return "humidity";
    }
    bool has_edit_configure() const override {
        return true;
    }
};

lv_obj_t* icon_label_of(lv_obj_t* pill) {
    // The pill's only child is the icon label create_selection_chrome centres.
    REQUIRE(lv_obj_get_child_count(pill) == 1);
    return lv_obj_get_child(pill, 0);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "edit-mode chrome icons clear WCAG AA on their composited pills",
                 "[grid_edit][edit-chrome]") {
    const int gutter = theme_manager_get_spacing("space_xs");
    REQUIRE(gutter > 0);

    // Same geometry contract as test_grid_edit_drag_path.cpp: derive the
    // content box from the Medium breakpoint's own track counts so every cell
    // is exact, and pin the breakpoint the fixture's display resolves to.
    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(as_breakpoint(lv_subject_get_int(bp_subj)) == UiBreakpoint::Medium);
    const int ncols = GridLayout::get_cols(UiBreakpoint::Medium);
    const int nrows = GridLayout::get_rows(UiBreakpoint::Medium);
    REQUIRE(ncols > 0);
    REQUIRE(nrows > 0);
    constexpr int CELL_PX = 55;
    const int content_w = ncols * CELL_PX + (ncols - 1) * gutter;
    const int content_h = nrows * CELL_PX + (nrows - 1) * gutter;

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, content_w, content_h);
    auto col_dsc = GridLayout::make_col_dsc(UiBreakpoint::Medium);
    auto row_dsc = GridLayout::make_row_dsc(UiBreakpoint::Medium);
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    ConfigurableWidget pw;
    lv_obj_t* widget = lv_obj_create(container);
    lv_obj_set_name(widget, "humidity");
    lv_obj_remove_flag(widget, LV_OBJ_FLAG_SCROLLABLE);
    constexpr int SPAN = 2;
    lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, 0, SPAN, LV_GRID_ALIGN_STRETCH, 0, SPAN);
    lv_obj_set_user_data(widget, &pw);
    lv_obj_update_layout(container);

    // A second page holds the widget (see test_grid_edit_drag_path.cpp for why
    // page 0 cannot keep an exact widget list), and entering on that page is
    // what makes the delete-page pill appear.
    const std::string panel_id = "test_grid_edit_chrome_contrast";
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(
        cfg->df() + "panel_widgets/" + panel_id,
        nlohmann::json{{"main_page_index", 0},
                       {"next_page_id", 2},
                       {"pages",
                        {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                         {{"id", "spy"},
                          {"widgets",
                           {{{"id", "humidity"},
                             {"enabled", true},
                             {"col", 0},
                             {"row", 0},
                             {"colspan", SPAN},
                             {"rowspan", SPAN}}}}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    auto& config = mgr.get_widget_config(panel_id);
    constexpr int PAGE_INDEX = 1;

    GridEditMode em;
    em.enter(container, &config, PAGE_INDEX);
    em.select_widget(widget);
    REQUIRE(em.selected_widget() == widget);

    // The pill as it lands on screen: text-coloured fill at half opacity
    // floating over the screen background (the top-row pills overhang the
    // grid entirely), re-derived here rather than asked of the helper.
    const lv_color_t pill_fill = theme_manager_get_color("text");
    const lv_color_t pill_composited =
        lv_color_mix(pill_fill, theme_manager_get_color("screen_bg"), LV_OPA_50);
    auto check_pill = [&](lv_obj_t* pill, const char* what) {
        REQUIRE(pill != nullptr);
        CAPTURE(what);
        CHECK(lv_obj_get_style_bg_opa(pill, LV_PART_MAIN) == LV_OPA_50);
        const lv_color_t icon = lv_obj_get_style_text_color(icon_label_of(pill), LV_PART_MAIN);
        CAPTURE(lv_color_to_u32(icon) & 0xFFFFFF, lv_color_to_u32(pill_composited) & 0xFFFFFF,
                wcag::contrast(icon, pill_composited));
        CHECK(wcag::contrast(icon, pill_composited) >= 4.5);
    };
    check_pill(GridEditModeTestAccess::remove_button(em), "trash");
    check_pill(GridEditModeTestAccess::configure_button(em), "configure");

    // The delete-page pill: danger fill at 80% over the screen background.
    // The theme layer's own contrast bar is 4:1, so that is the assertion.
    lv_obj_t* del = GridEditModeTestAccess::delete_page_button(em);
    REQUIRE(del != nullptr);
    const lv_color_t del_composited =
        lv_color_mix(ThemeManager::instance().current_palette().danger,
                     ThemeManager::instance().current_palette().screen_bg, LV_OPA_80);
    const lv_color_t del_icon = lv_obj_get_style_text_color(icon_label_of(del), LV_PART_MAIN);
    CAPTURE(lv_color_to_u32(del_icon) & 0xFFFFFF, lv_color_to_u32(del_composited) & 0xFFFFFF,
            wcag::contrast(del_icon, del_composited));
    CHECK(wcag::contrast(del_icon, del_composited) >= 4.0);

    em.exit();
}
