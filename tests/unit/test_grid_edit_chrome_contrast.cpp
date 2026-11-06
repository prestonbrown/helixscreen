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
#include "../test_helpers/grid_edit_scene.h"
#include "../test_helpers/scoped_breakpoint.h"
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
    /// Counts the configure pill's taps; false asks for no rebuild.
    bool on_edit_configure() override {
        ++configure_calls;
        return false;
    }
    int configure_calls = 0;
};

lv_obj_t* icon_label_of(lv_obj_t* pill) {
    // The pill's only child is the icon label create_selection_chrome centres.
    REQUIRE(lv_obj_get_child_count(pill) == 1);
    return lv_obj_get_child(pill, 0);
}

/// A 12x8 grid on page 1 holding one selected configurable widget, with the
/// edit-mode chrome drawn around it. The widget spans @p span tracks each way
/// from track (@p col, @p row): a whole cell by default. The grid is laid out
/// for @p bp, which the caller holds active with a ScopedBreakpoint: Medium is
/// the 800x480 home grid, Micro the 480x272 one.
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
                         int span = GridLayout::TRACKS_PER_CELL,
                         UiBreakpoint bp = UiBreakpoint::Medium) {
        const int gutter = helix::GridLayout::gutter_px();
        REQUIRE(gutter > 0);

        // Same geometry contract as test_grid_edit_drag_path.cpp: a content box
        // that gives the breakpoint an exact 12x8 grid. Micro's is the home
        // grid's content box on a 480x272 panel.
        const bool micro = bp == UiBreakpoint::Micro;
        const int content_w = micro ? 430 : 715;
        const int content_h = micro ? 264 : 475;
        const auto dims = GridLayout::get_dimensions(bp, content_w, content_h);
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

    /// Fill every other track of the grid with a half-cell neighbour tile.
    void pack_half_cells(int col, int row) {
        for (int c = 0; c < 12; ++c) {
            for (int r = 0; r < 8; ++r) {
                if (c == col && r == row) {
                    continue;
                }
                lv_obj_t* n = lv_obj_create(container);
                lv_obj_set_name(n, ("n_" + std::to_string(c) + "_" + std::to_string(r)).c_str());
                lv_obj_remove_flag(n, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_grid_cell(n, LV_GRID_ALIGN_STRETCH, c, 1, LV_GRID_ALIGN_STRETCH, r, 1);
                neighbours.push_back(n);
            }
        }
        lv_obj_update_layout(container);
    }
    std::vector<lv_obj_t*> neighbours;
};

bool area_has_point(const lv_area_t& a, lv_point_t p) {
    return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

lv_point_t centre_of_area(const lv_area_t& a) {
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

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
    // at the widget can start on the pill. The configure pill and the
    // delete-page button take the same guard.
    ChromeScene scene(test_screen());
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    int delete_page_calls = 0;
    em.set_delete_page_callback([&]() { ++delete_page_calls; });
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);
    lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms());

    auto centre_of = [](lv_obj_t* obj) {
        REQUIRE(obj != nullptr);
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        lv_point_t c{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        REQUIRE(lv_indev_search_obj(lv_screen_active(), &c) == obj);
        return c;
    };
    // Slides toward the grid's middle, off every button, before lifting.
    auto slide_off = [&](lv_point_t c) {
        const int dx = c.x > 300 ? -40 : 40;
        pointer.press(c.x, c.y);
        pointer.move(c.x + dx, c.y + 30);
        pointer.move(c.x + 2 * dx, c.y + 80);
        pointer.release(c.x + 2 * dx, c.y + 80);
    };
    auto tap = [&](lv_point_t c) {
        pointer.press(c.x, c.y);
        pointer.release(c.x, c.y);
    };

    const lv_point_t trash = centre_of(GridEditModeTestAccess::remove_button(em));
    const lv_point_t configure = centre_of(GridEditModeTestAccess::configure_button(em));
    const lv_point_t delete_page = centre_of(GridEditModeTestAccess::delete_page_button(em));

    for (const lv_point_t& c : {trash, configure, delete_page}) {
        INFO("button at " << c.x << "," << c.y);
        slide_off(c);
        CHECK(em.selected_widget() == scene.widget);
        CHECK(scene.config->page_entries(ChromeScene::kPageIndex).size() == 1);
        CHECK(scene.pw.configure_calls == 0);
        CHECK(delete_page_calls == 0);
    }

    // Taps that lift on the button still act.
    tap(configure);
    CHECK(scene.pw.configure_calls == 1);
    tap(delete_page);
    CHECK(delete_page_calls == 1);
    tap(trash);
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
    lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms());

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

TEST_CASE_METHOD(XMLTestFixture, "a half-cell widget's outside pills leave the delete-page button",
                 "[grid_edit][edit-chrome][halfcell]") {
    // Track (10, 1): just left of and below the delete-page button in the
    // secondary page's top-right corner, where the trash pill would go first.
    ChromeScene scene(test_screen(), 10, 1, 1);
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);

    lv_obj_t* del = GridEditModeTestAccess::delete_page_button(em);
    REQUIRE(del != nullptr);
    lv_area_t del_area;
    lv_obj_get_coords(del, &del_area);
    for (lv_obj_t* pill : {GridEditModeTestAccess::remove_button(em),
                           GridEditModeTestAccess::configure_button(em)}) {
        REQUIRE(pill != nullptr);
        lv_area_t a;
        lv_obj_get_coords(pill, &a);
        INFO("pill " << a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2 << " delete-page "
                     << del_area.x1 << "," << del_area.y1 << "-" << del_area.x2 << ","
                     << del_area.y2);
        CHECK(
            (a.x2 < del_area.x1 || a.x1 > del_area.x2 || a.y2 < del_area.y1 || a.y1 > del_area.y2));
    }
    lv_point_t centre{(del_area.x1 + del_area.x2) / 2, (del_area.y1 + del_area.y2) / 2};
    CHECK(lv_indev_search_obj(lv_screen_active(), &centre) == del);

    em.exit();
}

TEST_CASE_METHOD(XMLTestFixture, "a half-cell widget's outside pills spare a diagonal neighbour",
                 "[grid_edit][edit-chrome][halfcell]") {
    // A neighbour one track up and right sits where the trash pill would go
    // first; a pill over it would turn the tap that selects it into a delete.
    ChromeScene scene(test_screen(), 8, 4, 1);
    lv_obj_t* neighbour = lv_obj_create(scene.container);
    lv_obj_set_name(neighbour, "neighbour");
    lv_obj_set_grid_cell(neighbour, LV_GRID_ALIGN_STRETCH, 9, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_update_layout(scene.container);
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);

    lv_area_t n;
    lv_obj_get_coords(neighbour, &n);
    for (lv_obj_t* pill : {GridEditModeTestAccess::remove_button(em),
                           GridEditModeTestAccess::configure_button(em)}) {
        REQUIRE(pill != nullptr);
        lv_area_t a;
        lv_obj_get_coords(pill, &a);
        INFO("pill " << a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2 << " neighbour " << n.x1
                     << "," << n.y1 << "-" << n.x2 << "," << n.y2);
        CHECK((a.x2 < n.x1 || a.x1 > n.x2 || a.y2 < n.y1 || a.y1 > n.y2));
    }

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

TEST_CASE_METHOD(XMLTestFixture, "a whole-cell widget's corner pill leaves the delete-page button",
                 "[grid_edit][edit-chrome]") {
    // The grid's top-right whole cell on a secondary page: the trash pill's
    // corner spot overhangs the delete-page button.
    ChromeScene scene(test_screen(), 10, 0);
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    int delete_page_calls = 0;
    em.set_delete_page_callback([&]() { ++delete_page_calls; });
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);

    lv_obj_t* del = GridEditModeTestAccess::delete_page_button(em);
    lv_obj_t* trash = GridEditModeTestAccess::remove_button(em);
    lv_obj_t* cfg = GridEditModeTestAccess::configure_button(em);
    REQUIRE(del != nullptr);
    REQUIRE(trash != nullptr);
    REQUIRE(cfg != nullptr);
    lv_area_t del_area;
    lv_area_t tile;
    lv_obj_get_coords(del, &del_area);
    lv_obj_get_coords(scene.widget, &tile);
    for (lv_obj_t* pill : {trash, cfg}) {
        lv_area_t a;
        lv_obj_get_coords(pill, &a);
        INFO("pill " << a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2 << " delete-page "
                     << del_area.x1 << "," << del_area.y1 << "-" << del_area.x2 << ","
                     << del_area.y2);
        CHECK(
            (a.x2 < del_area.x1 || a.x1 > del_area.x2 || a.y2 < del_area.y1 || a.y1 > del_area.y2));
        // Still on the tile's edge, off its centre.
        CHECK_FALSE((a.x2 < tile.x1 || a.x1 > tile.x2 || a.y2 < tile.y1 || a.y1 > tile.y2));
        CHECK_FALSE(area_has_point(a, centre_of_area(tile)));
    }
    // Every pixel of the delete-page button is the delete-page button.
    for (int32_t x = del_area.x1; x <= del_area.x2; ++x) {
        for (int32_t y = del_area.y1; y <= del_area.y2; ++y) {
            lv_point_t p{x, y};
            lv_obj_t* hit = lv_indev_search_obj(lv_screen_active(), &p);
            INFO(x << "," << y);
            REQUIRE(hit != trash);
            REQUIRE(hit != cfg);
        }
    }
    lv_point_t c = centre_of_area(del_area);
    pointer.press(c.x, c.y);
    pointer.release(c.x, c.y);
    CHECK(delete_page_calls == 1);
    CHECK(scene.config->page_entries(ChromeScene::kPageIndex).size() == 1);

    em.exit();
}

TEST_CASE_METHOD(
    XMLTestFixture,
    "a packed half-cell widget at 480x272 keeps its pills to a sliver of any neighbour",
    "[grid_edit][edit-chrome][halfcell]") {
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
    struct Spot {
        const char* what;
        int col, row;
    };
    const Spot spot = GENERATE(
        Spot{"interior", 5, 4}, Spot{"top row", 5, 0}, Spot{"under the delete-page button", 11, 0},
        Spot{"top-left corner", 0, 0}, Spot{"right edge", 11, 4}, Spot{"left edge", 0, 3},
        Spot{"bottom-left corner", 0, 7}, Spot{"bottom-right corner", 11, 7});
    CAPTURE(spot.what, spot.col, spot.row);
    ChromeScene scene(test_screen(), spot.col, spot.row, 1, UiBreakpoint::Micro);
    scene.pack_half_cells(spot.col, spot.row);
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    int delete_page_calls = 0;
    em.set_delete_page_callback([&]() { ++delete_page_calls; });
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);
    lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms());

    lv_area_t tile;
    lv_obj_get_coords(scene.widget, &tile);
    lv_obj_t* trash = GridEditModeTestAccess::remove_button(em);
    lv_obj_t* cfg = GridEditModeTestAccess::configure_button(em);
    lv_obj_t* del = GridEditModeTestAccess::delete_page_button(em);
    REQUIRE(trash != nullptr);
    REQUIRE(cfg != nullptr);
    REQUIRE(del != nullptr);
    lv_area_t del_area;
    lv_obj_get_coords(del, &del_area);
    for (lv_obj_t* pill : {trash, cfg}) {
        lv_area_t a;
        lv_obj_get_coords(pill, &a);
        INFO("pill " << a.x1 << "," << a.y1 << "-" << a.x2 << "," << a.y2 << " tile " << tile.x1
                     << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2);
        CHECK(lv_area_get_width(&a) == 24);
        CHECK_FALSE(area_has_point(a, centre_of_area(tile)));
        // A pill deeper into a neighbour than a few px takes the tap that
        // would select it.
        for (lv_obj_t* n : scene.neighbours) {
            lv_area_t na;
            lv_obj_get_coords(n, &na);
            const int32_t ow = std::min(a.x2, na.x2) - std::max(a.x1, na.x1) + 1;
            const int32_t oh = std::min(a.y2, na.y2) - std::max(a.y1, na.y1) + 1;
            INFO("neighbour " << lv_obj_get_name(n) << " overlap " << ow << "x" << oh);
            CHECK((ow <= 0 || oh <= 0 || std::min(ow, oh) <= 6));
        }
        // Where the corner leaves a pill nowhere but over the delete-page
        // button, a tap on the part over the button acts on neither.
        if (!(a.x2 < del_area.x1 || a.x1 > del_area.x2 || a.y2 < del_area.y1 ||
              a.y1 > del_area.y2)) {
            const int32_t x = (std::max(a.x1, del_area.x1) + std::min(a.x2, del_area.x2)) / 2;
            const int32_t y = (std::max(a.y1, del_area.y1) + std::min(a.y2, del_area.y2)) / 2;
            pointer.press(x, y);
            pointer.release(x, y);
            CHECK(scene.config->page_entries(ChromeScene::kPageIndex).size() == 1);
            CHECK(scene.pw.configure_calls == 0);
            CHECK(delete_page_calls == 0);
        }
    }
    // A press on the tile's centre lands on the tile, so a drag from there
    // moves it.
    lv_point_t mid = centre_of_area(tile);
    lv_obj_t* under = lv_indev_search_obj(lv_screen_active(), &mid);
    CHECK(under != trash);
    CHECK(under != cfg);

    em.exit();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a release that ends a long press on a selection pill does nothing",
                 "[grid_edit][edit-chrome][halfcell]") {
    // A half-cell widget's outside pills sit over cells a hold may be aimed at
    // (the hold that opens the widget catalog); the pill takes that press.
    ChromeScene scene(test_screen(), 8, 4, 1);
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    int delete_page_calls = 0;
    em.set_delete_page_callback([&]() { ++delete_page_calls; });
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);
    lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms());

    auto centre_on = [](lv_obj_t* obj) {
        REQUIRE(obj != nullptr);
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        lv_point_t c = centre_of_area(a);
        REQUIRE(lv_indev_search_obj(lv_screen_active(), &c) == obj);
        return c;
    };
    const lv_point_t trash = centre_on(GridEditModeTestAccess::remove_button(em));
    const lv_point_t configure = centre_on(GridEditModeTestAccess::configure_button(em));
    const lv_point_t delete_page = centre_on(GridEditModeTestAccess::delete_page_button(em));
    for (const lv_point_t& c : {trash, configure, delete_page}) {
        INFO("button at " << c.x << "," << c.y);
        pointer.press(c.x, c.y);
        pointer.hold(pointer.long_press_hold_ms());
        pointer.release(c.x, c.y);
        CHECK(em.selected_widget() == scene.widget);
        CHECK(scene.config->page_entries(ChromeScene::kPageIndex).size() == 1);
        CHECK(scene.pw.configure_calls == 0);
        CHECK(delete_page_calls == 0);
    }

    em.exit();
}

TEST_CASE_METHOD(XMLTestFixture, "a selection pill ignores taps just after it appears",
                 "[grid_edit][edit-chrome]") {
    // A tap on a widget's top-right corner selects it and draws the trash
    // pill under the finger; a double tap's second tap lands on that pill.
    ChromeScene scene(test_screen(), 8, 4);
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    em.enter(scene.container, scene.config, ChromeScene::kPageIndex);
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);
    lv_obj_update_layout(scene.container);

    lv_area_t a;
    lv_obj_get_coords(GridEditModeTestAccess::remove_button(em), &a);
    const lv_point_t c = centre_of_area(a);
    pointer.press(c.x, c.y);
    pointer.release(c.x, c.y);
    CHECK(em.selected_widget() == scene.widget);
    CHECK(scene.config->page_entries(ChromeScene::kPageIndex).size() == 1);

    lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms());
    pointer.press(c.x, c.y);
    pointer.release(c.x, c.y);
    CHECK(em.selected_widget() == nullptr);
    CHECK(scene.config->page_entries(ChromeScene::kPageIndex).empty());

    em.exit();
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a selection pill's lockout runs from its first frame to the press",
                 "[grid_edit][edit-chrome]") {
    GridEditScene scene(test_screen(), "test_pill_lockout");
    helix_test::ScopedPointerIndev pointer;
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    em.select_widget(scene.widget);
    lv_obj_update_layout(scene.container);
    lv_obj_t* pill = lv_obj_find_by_name(scene.container, "edit_trash_pill");
    REQUIRE(pill != nullptr);
    lv_area_t a;
    lv_obj_get_coords(pill, &a);
    const lv_point_t c{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};

    SECTION("a double tap: the press lands inside the lockout and lifts after it") {
        lv_tick_inc(240);
        pointer.press(c.x, c.y);
        lv_tick_inc(70);
        pointer.release(c.x, c.y);
    }
    SECTION("a slow board: the press comes right after the frame that shows the pill") {
        lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms() + 100);
        lv_refr_now(nullptr);
        pointer.press(c.x, c.y);
        pointer.release(c.x, c.y);
    }
    CHECK(scene.config->is_placed("temperature"));

    // A press after the lockout acts.
    lv_tick_inc(GridEditModeTestAccess::pill_lockout_ms());
    pointer.press(c.x, c.y);
    pointer.release(c.x, c.y);
    CHECK_FALSE(scene.config->is_placed("temperature"));

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}
