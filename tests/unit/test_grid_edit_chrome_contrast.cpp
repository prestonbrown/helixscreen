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

/// A 12x8 grid on page 1 holding one selected configurable widget, with the
/// edit-mode chrome drawn around it. The widget spans @p span tracks each way
/// from track (@p col, @p row): a whole cell by default.
struct ChromeScene {
    ConfigurableWidget pw;
    lv_obj_t* container = nullptr;
    lv_obj_t* widget = nullptr;
    PanelWidgetConfig* config = nullptr;
    // LVGL keeps pointers into the track arrays, so they live as long as the grid.
    std::vector<int32_t> col_dsc;
    std::vector<int32_t> row_dsc;
    static constexpr int kPageIndex = 1;

    explicit ChromeScene(lv_obj_t* screen, int col = 0, int row = 0,
                         int span = GridLayout::TRACKS_PER_CELL) {
        const int gutter = helix::GridLayout::gutter_px();
        REQUIRE(gutter > 0);

        // Same geometry contract as test_grid_edit_drag_path.cpp: a content box
        // that gives the Medium breakpoint an exact 12x8 grid.
        constexpr int content_w = 715;
        constexpr int content_h = 475;
        const auto dims = GridLayout::get_dimensions(UiBreakpoint::Medium, content_w, content_h);
        REQUIRE(dims.cols == 12);
        REQUIRE(dims.rows == 8);

        container = lv_obj_create(screen);
        lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_size(container, content_w, content_h);
        col_dsc = GridLayout::make_col_dsc(dims.cols);
        row_dsc = GridLayout::make_row_dsc(dims.rows);
        lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
        lv_obj_set_style_pad_column(container, gutter, 0);
        lv_obj_set_style_pad_row(container, gutter, 0);

        widget = lv_obj_create(container);
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
        config = &mgr.get_widget_config(panel_id);
    }
};

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "edit-mode chrome icons clear WCAG AA on their composited pills",
                 "[grid_edit][edit-chrome]") {
    ChromeScene scene(test_screen());
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);

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

TEST_CASE_METHOD(XMLTestFixture, "a press that slides off a selection pill is not a click",
                 "[grid_edit][edit-chrome][halfcell]") {
    // A whole-cell widget carries the trash pill on its corner, so a drag aimed
    // at the widget can start on the pill.
    ChromeScene scene(test_screen());
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);

    lv_obj_t* trash = GridEditModeTestAccess::remove_button(em);
    REQUIRE(trash != nullptr);
    lv_obj_update_layout(scene.container);
    lv_area_t pill;
    lv_obj_get_coords(trash, &pill);
    const int cx = (pill.x1 + pill.x2) / 2;
    const int cy = (pill.y1 + pill.y2) / 2;
    lv_point_t centre{cx, cy};
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &centre) == trash);

    pointer.press(cx, cy);
    pointer.move(cx + 30, cy + 30);
    pointer.move(cx + 80, cy + 80);
    pointer.release(cx + 80, cy + 80);
    CHECK(em.selected_widget() == scene.widget);
    CHECK(scene.config->page_entries(ChromeScene::kPageIndex).size() == 1);

    // A tap that lifts on the pill still removes the widget.
    pointer.press(cx, cy);
    pointer.release(cx, cy);
    CHECK(em.selected_widget() == nullptr);

    em.exit();
}

TEST_CASE_METHOD(XMLTestFixture, "a half-cell widget carries its selection pills outside it",
                 "[grid_edit][edit-chrome][halfcell]") {
    // Track (8, 4) of the 12x8 track grid: room for the pills on every side.
    ChromeScene scene(test_screen(), 8, 4, 1);
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);

    lv_area_t tile;
    lv_area_t box;
    lv_obj_get_coords(scene.widget, &tile);
    lv_obj_get_coords(scene.container, &box);
    lv_obj_t* trash = GridEditModeTestAccess::remove_button(em);
    lv_obj_t* cfg = GridEditModeTestAccess::configure_button(em);
    REQUIRE(trash != nullptr);
    REQUIRE(cfg != nullptr);

    for (lv_obj_t* pill : {trash, cfg}) {
        lv_area_t a;
        lv_obj_get_coords(pill, &a);
        INFO("pill " << a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2 << " tile " << tile.x1
                     << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2);
        CHECK((a.x2 < tile.x1 || a.x1 > tile.x2 || a.y2 < tile.y1 || a.y1 > tile.y2));
        CHECK((a.x1 >= box.x1 && a.y1 >= box.y1 && a.x2 <= box.x2 && a.y2 <= box.y2));
        CHECK(a.y2 < tile.y1); // above the tile, off its top corners
    }
    lv_area_t trash_area;
    lv_obj_get_coords(trash, &trash_area);
    CHECK(trash_area.x1 > tile.x2);

    // The pill is the hit target over the outward resize band it overlaps, so
    // a press there neither drags nor resizes the widget.
    const int cx = (trash_area.x1 + trash_area.x2) / 2;
    const int cy = (trash_area.y1 + trash_area.y2) / 2;
    lv_point_t centre{cx, cy};
    REQUIRE(lv_indev_search_obj(lv_screen_active(), &centre) == trash);
    pointer.press(trash_area.x1 + 1, trash_area.y2 - 1);
    pointer.move(trash_area.x1 + 1, trash_area.y2 + 40);
    pointer.move(trash_area.x1 + 1, trash_area.y2 + 80);
    pointer.release(trash_area.x1 + 1, trash_area.y2 + 80);
    CHECK(em.selected_widget() == scene.widget);
    const auto& entries = scene.config->page_entries(ChromeScene::kPageIndex);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].colspan == 1);
    CHECK(entries[0].rowspan == 1);

    // The widget's centre is a drag target, not a pill.
    lv_point_t mid{(tile.x1 + tile.x2) / 2, (tile.y1 + tile.y2) / 2};
    lv_obj_t* under = lv_indev_search_obj(lv_screen_active(), &mid);
    CHECK(under != trash);
    CHECK(under != cfg);

    // A tap on the outside trash pill removes the widget.
    pointer.press(cx, cy);
    pointer.release(cx, cy);
    CHECK(em.selected_widget() == nullptr);
    CHECK(scene.config->page_entries(ChromeScene::kPageIndex).empty());

    em.exit();
}

TEST_CASE_METHOD(XMLTestFixture, "a whole-cell widget keeps its selection pills on its corners",
                 "[grid_edit][edit-chrome][halfcell]") {
    ChromeScene scene(test_screen(), 8, 4);
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    lv_obj_update_layout(scene.container);

    lv_area_t tile;
    lv_area_t trash;
    lv_obj_get_coords(scene.widget, &tile);
    lv_obj_get_coords(GridEditModeTestAccess::remove_button(em), &trash);
    const int pill = lv_area_get_width(&trash);
    // A quarter of the pill overhangs the top-right corner; the rest is on the tile.
    CHECK(trash.x2 == tile.x2 + pill / 4);
    CHECK(trash.y1 == tile.y1 - pill / 4);

    em.exit();
}
