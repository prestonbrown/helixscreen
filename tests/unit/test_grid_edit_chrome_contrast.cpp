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

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/scoped_pointer_indev.h"
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

TEST_CASE_METHOD(XMLTestFixture,
                 "edit-mode chrome pills: icons clear WCAG AA, and only a tap presses one",
                 "[grid_edit][edit-chrome][halfcell]") {
    // A whole cell at the grid origin carries its pills on its corners; a half
    // cell, too small to share, carries them just outside it, mid-grid or
    // beside the delete-page button in the page's top-right corner.
    enum class Scene { Whole, HalfMidGrid, HalfBesideDeletePage };
    const Scene scene = GENERATE(Scene::Whole, Scene::HalfMidGrid, Scene::HalfBesideDeletePage);
    CAPTURE(static_cast<int>(scene));
    const bool half = scene != Scene::Whole;
    const int col = scene == Scene::HalfMidGrid ? 8 : scene == Scene::Whole ? 0 : 10;
    const int row = scene == Scene::HalfMidGrid ? 4 : scene == Scene::Whole ? 0 : 1;
    const int span = half ? 1 : GridLayout::TRACKS_PER_CELL;
    const int gutter = helix::GridLayout::gutter_px();
    REQUIRE(gutter > 0);

    // Same geometry contract as test_grid_edit_drag_path.cpp: a content box
    // that gives the Medium breakpoint an exact 12x8 grid.
    constexpr int content_w = 715;
    constexpr int content_h = 475;
    const auto dims = GridLayout::get_dimensions(UiBreakpoint::Medium, content_w, content_h);
    REQUIRE(dims.cols == 12);
    REQUIRE(dims.rows == 8);

    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_size(container, content_w, content_h);
    auto col_dsc = GridLayout::make_col_dsc(dims.cols);
    auto row_dsc = GridLayout::make_row_dsc(dims.rows);
    lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
    lv_obj_set_style_pad_column(container, gutter, 0);
    lv_obj_set_style_pad_row(container, gutter, 0);

    ConfigurableWidget pw;
    lv_obj_t* widget = lv_obj_create(container);
    lv_obj_set_name(widget, "humidity");
    lv_obj_remove_flag(widget, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, col, span, LV_GRID_ALIGN_STRETCH, row,
                         span);
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
                             {"col", col},
                             {"row", row},
                             {"colspan", span},
                             {"rowspan", span}}}}}}}});

    auto& mgr = PanelWidgetManager::instance();
    mgr.get_widget_config(panel_id).mark_dirty();
    mgr.clear_panel_config(panel_id);
    auto& config = mgr.get_widget_config(panel_id);
    constexpr int kPageIndex = 1;

    GridEditMode em;
    em.enter(container, &config, kPageIndex);
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
    lv_obj_t* trash = GridEditModeTestAccess::remove_button(em);
    check_pill(trash, "trash");
    lv_obj_t* configure = GridEditModeTestAccess::configure_button(em);
    check_pill(configure, "configure");

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

    lv_obj_update_layout(container);
    lv_area_t tile;
    lv_area_t box;
    lv_area_t pill;
    lv_obj_get_coords(widget, &tile);
    lv_obj_get_coords(container, &box);
    lv_obj_get_coords(trash, &pill);
    const int quarter = lv_area_get_width(&pill) / 4;
    lv_area_t del_area;
    lv_obj_get_coords(del, &del_area);
    if (half) {
        for (lv_obj_t* p : {trash, configure}) {
            lv_area_t a;
            lv_obj_get_coords(p, &a);
            if (scene == Scene::HalfMidGrid) {
                CHECK(a.y2 < tile.y1); // off the tile, above its top corners
            }
            CHECK((a.x1 >= box.x1 && a.y1 >= box.y1 && a.x2 <= box.x2 && a.y2 <= box.y2));
            CHECK((a.x2 < del_area.x1 || a.x1 > del_area.x2 || a.y2 < del_area.y1 ||
                   a.y1 > del_area.y2));
        }
        CHECK(pill.x1 > tile.x2);
        lv_point_t del_centre{(del_area.x1 + del_area.x2) / 2, (del_area.y1 + del_area.y2) / 2};
        CHECK(lv_indev_search_obj(lv_screen_active(), &del_centre) == del);
    } else {
        CHECK(pill.x2 == tile.x2 + quarter);
        CHECK(pill.y1 == tile.y1 - quarter);
    }

    // A press that starts on a pill is the pill's, even over the outward
    // resize band: sliding off neither resizes the widget nor taps the pill.
    helix_test::ScopedPointerIndev pointer;
    lv_point_t centre{(pill.x1 + pill.x2) / 2, (pill.y1 + pill.y2) / 2};
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &centre) == trash);
    pointer.press(centre.x, centre.y);
    pointer.move(centre.x + 30, centre.y + 30);
    pointer.release(centre.x + 80, centre.y + 80);
    CHECK(em.selected_widget() == widget);
    REQUIRE(config.page_entries(kPageIndex).size() == 1);
    CHECK(config.page_entries(kPageIndex)[0].colspan == span);

    // A tap that lifts on the pill still removes the widget.
    pointer.press(centre.x, centre.y);
    pointer.release(centre.x, centre.y);
    CHECK(em.selected_widget() == nullptr);

    em.exit();
}
