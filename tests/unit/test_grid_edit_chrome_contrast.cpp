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

#include <optional>

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

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "edit-mode chrome pills: icons clear WCAG AA, and only a tap presses one",
                 "[grid_edit][edit-chrome][halfcell]") {
    // A whole cell carries its pills on its corners, clear of the delete-page
    // button in the page's top-right corner; a half cell, too small to share,
    // carries them just outside it: mid-grid, beside the delete-page button,
    // beside a diagonal neighbour whose select tap a pill must not take, or in
    // a packed 480x272 grid under the delete-page button, on its own corner
    // and no more than a sliver onto any neighbour.
    enum class Scene {
        Whole,
        WholeUnderDeletePage,
        HalfMidGrid,
        HalfBesideDeletePage,
        HalfBesideNeighbour,
        HalfPackedMicro
    };
    const Scene scene =
        GENERATE(Scene::Whole, Scene::WholeUnderDeletePage, Scene::HalfMidGrid,
                 Scene::HalfBesideDeletePage, Scene::HalfBesideNeighbour, Scene::HalfPackedMicro);
    CAPTURE(static_cast<int>(scene));
    const bool whole = scene == Scene::Whole || scene == Scene::WholeUnderDeletePage;
    const bool micro = scene == Scene::HalfPackedMicro;
    const int col = scene == Scene::Whole                  ? 0
                    : scene == Scene::WholeUnderDeletePage ? 10
                    : scene == Scene::HalfBesideDeletePage ? 10
                    : micro                                ? 11
                                                           : 8;
    const int row = scene == Scene::Whole || scene == Scene::WholeUnderDeletePage || micro ? 0
                    : scene == Scene::HalfBesideDeletePage                                 ? 1
                                                                                           : 4;
    const int span = whole ? GridLayout::TRACKS_PER_CELL : 1;
    const int gutter = helix::GridLayout::gutter_px();
    REQUIRE(gutter > 0);
    std::optional<helix::test::ScopedBreakpoint> bp;
    if (micro) {
        bp.emplace(UiBreakpoint::Micro);
    }

    // Same geometry contract as test_grid_edit_drag_path.cpp: a content box
    // that gives the breakpoint an exact 12x8 grid. Micro's is the home grid's
    // content box on a 480x272 panel.
    const int content_w = micro ? 430 : 715;
    const int content_h = micro ? 264 : 475;
    const auto dims = GridLayout::get_dimensions(micro ? UiBreakpoint::Micro : UiBreakpoint::Medium,
                                                 content_w, content_h);
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
    lv_obj_t* neighbour = nullptr;
    if (scene == Scene::HalfBesideNeighbour) {
        neighbour = lv_obj_create(container);
        lv_obj_set_name(neighbour, "neighbour");
        lv_obj_set_grid_cell(neighbour, LV_GRID_ALIGN_STRETCH, col + 1, 1, LV_GRID_ALIGN_STRETCH,
                             row - 1, 1);
    }
    std::vector<lv_obj_t*> packed;
    if (micro) {
        for (int c = 0; c < dims.cols; ++c) {
            for (int r = 0; r < dims.rows; ++r) {
                if (c == col && r == row) {
                    continue;
                }
                lv_obj_t* n = lv_obj_create(container);
                lv_obj_set_name(n, ("n_" + std::to_string(c) + "_" + std::to_string(r)).c_str());
                lv_obj_set_grid_cell(n, LV_GRID_ALIGN_STRETCH, c, 1, LV_GRID_ALIGN_STRETCH, r, 1);
                packed.push_back(n);
            }
        }
    }
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
    int delete_page_calls = 0;
    em.set_delete_page_callback([&]() { ++delete_page_calls; });
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
    helix_test::ScopedPointerIndev pointer;
    // A tap selects a widget and draws its trash under the finger, so a pill
    // ignores taps for a moment after it appears: a double tap's second tap.
    {
        lv_area_t t;
        lv_obj_get_coords(trash, &t);
        pointer.press((t.x1 + t.x2) / 2, (t.y1 + t.y2) / 2);
        pointer.release((t.x1 + t.x2) / 2, (t.y1 + t.y2) / 2);
        REQUIRE(config.page_entries(kPageIndex).size() == 1);
        lv_tick_inc(GridEditMode::kPillLockoutMs);
    }
    auto covers_centre_of = [](const lv_area_t& a, lv_obj_t* obj) {
        lv_area_t t;
        lv_obj_get_coords(obj, &t);
        const int32_t cx = (t.x1 + t.x2) / 2;
        const int32_t cy = (t.y1 + t.y2) / 2;
        return cx >= a.x1 && cx <= a.x2 && cy >= a.y1 && cy <= a.y2;
    };
    for (lv_obj_t* p : {trash, configure}) {
        lv_area_t a;
        lv_obj_get_coords(p, &a);
        CAPTURE(a.x1, a.y1, a.x2, a.y2);
        const bool on_del =
            !(a.x2 < del_area.x1 || a.x1 > del_area.x2 || a.y2 < del_area.y1 || a.y1 > del_area.y2);
        // Only a packed corner leaves a pill nowhere but over the delete-page
        // button, and a tap on that part acts on neither.
        CHECK((!on_del || micro));
        if (on_del) {
            const int32_t x = (std::max(a.x1, del_area.x1) + std::min(a.x2, del_area.x2)) / 2;
            const int32_t y = (std::max(a.y1, del_area.y1) + std::min(a.y2, del_area.y2)) / 2;
            pointer.press(x, y);
            pointer.release(x, y);
            REQUIRE(config.page_entries(kPageIndex).size() == 1);
            CHECK(pw.configure_calls == 0);
            CHECK(delete_page_calls == 0);
        }
        CHECK_FALSE(covers_centre_of(a, widget));
        if (!whole && !micro) {
            CHECK((a.x1 >= box.x1 && a.y1 >= box.y1 && a.x2 <= box.x2 && a.y2 <= box.y2));
        }
        if (scene == Scene::HalfMidGrid) {
            CHECK(a.y2 < tile.y1); // off the tile, above its top corners
        }
        if (neighbour) {
            lv_area_t n;
            lv_obj_get_coords(neighbour, &n);
            CHECK((a.x2 < n.x1 || a.x1 > n.x2 || a.y2 < n.y1 || a.y1 > n.y2));
        }
        // A pill deeper into a neighbour than a few px takes its select tap.
        for (lv_obj_t* n : packed) {
            lv_area_t na;
            lv_obj_get_coords(n, &na);
            const int32_t ow = std::min(a.x2, na.x2) - std::max(a.x1, na.x1) + 1;
            const int32_t oh = std::min(a.y2, na.y2) - std::max(a.y1, na.y1) + 1;
            CAPTURE(lv_obj_get_name(n), ow, oh);
            CHECK((ow <= 0 || oh <= 0 || std::min(ow, oh) <= 6));
        }
    }
    // Every pixel of the delete-page button is the delete-page button.
    for (int32_t x = del_area.x1; x <= del_area.x2 && !micro; ++x) {
        for (int32_t y = del_area.y1; y <= del_area.y2; ++y) {
            lv_point_t p{x, y};
            CAPTURE(x, y);
            REQUIRE(lv_indev_search_obj(lv_screen_active(), &p) == del);
        }
    }
    if (scene == Scene::Whole) {
        CHECK(pill.x2 == tile.x2 + quarter);
        CHECK(pill.y1 == tile.y1 - quarter);
    } else if (!whole && !micro) {
        CHECK(pill.x1 > tile.x2);
    }
    // The tile's centre is its drag target, never a pill.
    lv_point_t mid{(tile.x1 + tile.x2) / 2, (tile.y1 + tile.y2) / 2};
    lv_obj_t* under = lv_indev_search_obj(lv_screen_active(), &mid);
    CHECK(under != trash);
    CHECK(under != configure);

    // A press that starts on a pill or the delete-page button is that
    // button's, even over the outward resize band: sliding off neither resizes
    // the widget nor taps the button.
    auto centre_of = [](lv_obj_t* obj) {
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        lv_point_t c{(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        REQUIRE(lv_indev_search_obj(lv_screen_active(), &c) == obj);
        return c;
    };
    auto tap = [&](lv_point_t c) {
        pointer.press(c.x, c.y);
        pointer.release(c.x, c.y);
    };
    const lv_point_t centre = centre_of(trash);
    for (lv_obj_t* button : {trash, configure, del}) {
        const lv_point_t c = centre_of(button);
        const int dx = c.x > 300 ? -40 : 40; // toward the grid's middle
        pointer.press(c.x, c.y);
        pointer.move(c.x + dx, c.y + 30);
        pointer.release(c.x + 2 * dx, c.y + 80);
        CHECK(em.selected_widget() == widget);
        REQUIRE(config.page_entries(kPageIndex).size() == 1);
        CHECK(config.page_entries(kPageIndex)[0].colspan == span);
        CHECK(pw.configure_calls == 0);
        CHECK(delete_page_calls == 0);
    }

    // A hold is not a tap: LVGL still sends CLICKED on the release that ends
    // a long press, and an outside pill sits over cells a hold may aim at.
    for (lv_obj_t* button : {trash, configure, del}) {
        const lv_point_t c = centre_of(button);
        pointer.press(c.x, c.y);
        pointer.hold(pointer.long_press_hold_ms());
        pointer.release(c.x, c.y);
        CHECK(em.selected_widget() == widget);
        REQUIRE(config.page_entries(kPageIndex).size() == 1);
        CHECK(pw.configure_calls == 0);
        CHECK(delete_page_calls == 0);
    }

    // A tap that lifts on the button still acts; the trash last, as it
    // removes the widget.
    tap(centre_of(configure));
    CHECK(pw.configure_calls == 1);
    tap(centre_of(del));
    CHECK(delete_page_calls == 1);
    tap(centre);
    CHECK(em.selected_widget() == nullptr);

    em.exit();
}
