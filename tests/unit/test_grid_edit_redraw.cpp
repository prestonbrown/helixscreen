// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_redraw.cpp
 * @brief How much of the screen each edit-mode step repaints.
 *
 * A slow panel renders about a millisecond per thousand pixels, so a step that
 * invalidates a widget-sized box every pointer move drags at a few frames a
 * second. These cases read the display's invalidated areas after one step.
 */

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/grid_edit_scene.h"
#include "../test_helpers/scoped_whole_cell_def.h"
#include "../test_helpers/widget_in_tree.h"
#include "core/lv_obj_private.h"        // scr_layout_inv is private state
#include "display/lv_display_private.h" // inv_areas / inv_p are private state
#include "grid_edit_mode.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Render whatever is pending, so the next step's invalidation starts from none.
void flush_invalidation() {
    lv_display_t* disp = lv_display_get_default();
    lv_refr_now(disp);
    REQUIRE(disp->inv_p == 0);
}

/// Pixels the display has queued for repaint since the last render, after a
/// layout pass turns pending position changes into invalidations.
int32_t invalidated_px() {
    lv_display_t* disp = lv_display_get_default();
    lv_obj_update_layout(lv_display_get_screen_active(disp));
    lv_obj_update_layout(lv_display_get_layer_top(disp));
    int32_t px = 0;
    for (uint32_t i = 0; i < disp->inv_p; ++i) {
        if (!disp->inv_area_joined[i]) {
            px += lv_area_get_size(&disp->inv_areas[i]);
        }
    }
    return px;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: an unchanged snap preview repaints nothing",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_snap");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));

    constexpr int SPAN = GridEditScene::kColspan;
    GridEditModeTestAccess::update_snap_preview(em, 0, 0, SPAN, SPAN, true);
    lv_obj_t* preview = GridEditModeTestAccess::snap_preview(em);
    REQUIRE(preview != nullptr);
    flush_invalidation();

    // A resize step whose pointer stays inside the same snapped cell.
    GridEditModeTestAccess::update_snap_preview(em, 0, 0, SPAN, SPAN, true);
    CHECK(GridEditModeTestAccess::snap_preview(em) == preview);
    CHECK(invalidated_px() == 0);

    // A new cell moves the same bars instead of creating others, and repaints
    // their strips, not the cell they outline.
    const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(em);
    GridEditModeTestAccess::update_snap_preview(em, SPAN, 0, SPAN, SPAN, false);
    CHECK(GridEditModeTestAccess::snap_preview(em) == preview);
    const int32_t repainted = invalidated_px();
    CHECK(repainted > 0);
    const int cell_w = static_cast<int>(grid_track_extent(m.cell_w, m.gutter, SPAN));
    const int cell_h = static_cast<int>(grid_track_extent(m.cell_h, m.gutter, SPAN));
    CHECK(repainted < cell_w * cell_h);
    lv_area_t content;
    lv_obj_get_content_coords(scene.container, &content);
    lv_area_t top;
    lv_obj_get_coords(preview, &top);
    CHECK(top.x1 == content.x1 + static_cast<int>(grid_track_origin(m.cell_w, m.gutter, SPAN)));
    CHECK(top.y1 == content.y1);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a resize step repaints the outline's edges, not its box",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_outline");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));

    GridEditModeTestAccess::make_resize_preview(em, 0, 0, 200, 150);
    flush_invalidation();

    // The right edge follows the pointer 60px outward.
    constexpr int W = 260;
    constexpr int H = 150;
    GridEditModeTestAccess::make_resize_preview(em, 0, 0, W, H);
    const int32_t repainted = invalidated_px();
    INFO("repainted " << repainted << " px for a " << W << "x" << H << " outline");
    CHECK(repainted > 0);
    // A box outline repaints its old and new areas, more than W*H. The bars
    // repaint strips a few pixels thick.
    CHECK(repainted < W * H / 8);

    // Still drawn where asked: the right bar sits on the box's right edge.
    lv_obj_t* right = GridEditModeTestAccess::resize_outline(em)[3];
    REQUIRE(right != nullptr);
    lv_area_t content;
    lv_obj_get_content_coords(scene.container, &content);
    lv_area_t bar;
    lv_obj_get_coords(right, &bar);
    CHECK(bar.x2 + 1 == content.x1 + W);
    CHECK(lv_area_get_height(&bar) == H);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a dragged widget moves on the top layer and settles back",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_drag");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    em.select_widget(scene.widget);
    lv_obj_t* overlay = GridEditModeTestAccess::selection_overlay(em);
    REQUIRE(overlay != nullptr);
    lv_obj_update_layout(scene.container);
    lv_area_t before;
    lv_obj_get_coords(scene.widget, &before);
    const int32_t index_before = lv_obj_get_index(scene.widget);

    GridEditModeTestAccess::lift_dragged_widget(em);
    lv_obj_t* layer = lv_display_get_layer_top(lv_obj_get_display(scene.container));
    REQUIRE(lv_obj_get_parent(scene.widget) == layer);
    CHECK(lv_obj_get_parent(overlay) == layer);
    flush_invalidation();
    lv_obj_t* screen = lv_obj_get_screen(scene.container);
    REQUIRE_FALSE(screen->scr_layout_inv);

    // Each pointer move runs this. Inside the page it dirties the page's grid,
    // and the next refresh lays out every object on the screen for it.
    const lv_point_t to = {before.x1 + 37, before.y1 + 21};
    GridEditModeTestAccess::place_dragged_widget(em, to);
    CHECK_FALSE(screen->scr_layout_inv);
    lv_obj_update_layout(layer);
    lv_area_t widget_area;
    lv_area_t overlay_area;
    lv_obj_get_coords(scene.widget, &widget_area);
    lv_obj_get_coords(overlay, &overlay_area);
    CHECK(widget_area.x1 == to.x);
    CHECK(widget_area.y1 == to.y);
    CHECK(lv_area_get_width(&widget_area) == lv_area_get_width(&before));
    CHECK(lv_area_get_height(&widget_area) == lv_area_get_height(&before));
    CHECK(overlay_area.x1 == widget_area.x1);
    CHECK(overlay_area.y1 == widget_area.y1);

    // Settling returns it to the page, below the shield, in its cell.
    GridEditModeTestAccess::settle_dragged_widget(em);
    REQUIRE(lv_obj_get_parent(scene.widget) == scene.container);
    lv_obj_t* shield = GridEditModeTestAccess::shield(em);
    REQUIRE(shield != nullptr);
    CHECK(lv_obj_get_index(scene.widget) < lv_obj_get_index(shield));
    CHECK(lv_obj_get_index(scene.widget) == index_before);
    CHECK_FALSE(lv_obj_has_flag(scene.widget, LV_OBJ_FLAG_FLOATING));
    lv_obj_update_layout(scene.container);
    lv_area_t after;
    lv_obj_get_coords(scene.widget, &after);
    CHECK(after.x1 == before.x1);
    CHECK(after.y1 == before.y1);
    CHECK(lv_area_get_width(&after) == lv_area_get_width(&before));

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: leaving edit mode mid-drag puts the widget back in its page",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_drag_exit");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    em.select_widget(scene.widget);
    GridEditModeTestAccess::lift_dragged_widget(em);
    REQUIRE(lv_obj_get_parent(scene.widget) != scene.container);

    em.exit();
    CHECK(lv_obj_get_parent(scene.widget) == scene.container);
    CHECK_FALSE(lv_obj_has_flag(scene.widget, LV_OBJ_FLAG_FLOATING));

    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a rebuild mid-drag takes the lifted widget off the top layer",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_drag_forget");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    em.select_widget(scene.widget);
    GridEditModeTestAccess::lift_dragged_widget(em);
    lv_obj_t* lifted = scene.widget;

    // The owner rebuilds its pages and forgets them first.
    em.forget_scope();
    process_lvgl(50);
    CHECK_FALSE(helix::test::widget_in_tree(lifted));

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: a drop that moves nothing repaints the widget only",
                 "[grid_edit][grid_edit_redraw]") {
    // Snapping like no selection, so selecting it keeps the drawn lattice.
    ScopedWholeCellDef whole_cell("temperature");
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_noop_drop");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    lv_obj_update_layout(scene.container);
    flush_invalidation();

    GridEditModeTestAccess::reselect_in_place(em, scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);

    const int32_t page_px = lv_area_get_size(&scene.container->coords);
    const int32_t repainted = invalidated_px();
    INFO("repainted " << repainted << " of the page's " << page_px << " px");
    CHECK(repainted < page_px / 2);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: a resize step leaves the page's layout alone",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_layout");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    GridEditModeTestAccess::make_resize_preview(em, 0, 0, 200, 150);
    GridEditModeTestAccess::update_snap_preview(em, 0, 0, GridEditScene::kColspan,
                                                GridEditScene::kRowspan, true);
    flush_invalidation();
    lv_obj_t* screen = lv_obj_get_screen(scene.container);
    REQUIRE_FALSE(screen->scr_layout_inv);

    // Each pointer move runs both. A preview inside the page dirties its grid,
    // and the next refresh lays out every object on the screen for it.
    GridEditModeTestAccess::make_resize_preview(em, 0, 0, 260, 150);
    GridEditModeTestAccess::update_snap_preview(
        em, GridEditScene::kColspan, 0, GridEditScene::kColspan, GridEditScene::kRowspan, true);
    CHECK_FALSE(screen->scr_layout_inv);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a selection that snaps like the last keeps the drawn lattice",
                 "[grid_edit][grid_edit_redraw]") {
    ScopedWholeCellDef whole_cell("temperature");
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_lattice");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    REQUIRE(GridEditModeTestAccess::drawn_dot_count(em) > 0);
    flush_invalidation();

    // The widget snaps to whole cells, as the lattice drawn for no selection
    // does. A lattice change repaints the whole page; selecting the widget
    // repaints its outline only.
    em.select_widget(scene.widget);
    const int32_t page_px = lv_area_get_size(&scene.container->coords);
    const int32_t repainted = invalidated_px();
    INFO("repainted " << repainted << " of the page's " << page_px << " px");
    CHECK(repainted < page_px / 2);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: the lattice is drawn, not built from objects",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_lattice_objects");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    lv_obj_t* shield = GridEditModeTestAccess::shield(em);
    REQUIRE(shield != nullptr);
    // A fine lattice is a hundred-odd dots, which a slow board takes ~200 ms
    // to create as objects; the shield draws them instead.
    REQUIRE(GridEditModeTestAccess::drawn_dot_count(em) > 1);
    CHECK(lv_obj_get_child_count(shield) <= 1); // at most the delete-page button

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: drawing the selection chrome leaves layout to the next refresh",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_chrome_layout");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    flush_invalidation();
    lv_obj_t* screen = lv_obj_get_screen(scene.container);
    REQUIRE_FALSE(screen->scr_layout_inv);

    // The chrome is placed from coordinates already known; a forced layout
    // pass here lays out the whole screen (~50 ms on a slow board) on every
    // selection.
    em.select_widget(scene.widget);
    REQUIRE(GridEditModeTestAccess::selection_overlay(em) != nullptr);
    CHECK(screen->scr_layout_inv);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}
