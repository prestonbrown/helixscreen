// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_snap_anim_lifetime.cpp
 * @brief Pins the lifetime of the resize snap animation in GridEditMode.
 *
 * commit_resize_with_snap() animates the pixel-tracking resize preview into
 * its final grid slot over 150ms and rebuilds the panel when the animation
 * completes. Three things can end that animation early, and none of them may
 * leave it running against memory that is gone:
 *
 *   - the preview widget is destroyed (the deferred rebuild's lv_obj_clean
 *     takes every container child with it),
 *   - GridEditMode::exit() runs (it nulls config_, which the completion
 *     callback dereferences),
 *   - GridEditMode itself is destroyed (the completion callback holds a raw
 *     GridEditMode*).
 *
 * The first case is what LVGL already solves for free — lv_obj_destructor
 * calls lv_anim_delete(obj, NULL) (lib/lvgl/src/core/lv_obj.c:614) — but only
 * for animations whose `var` IS the widget. An animation keyed on a heap
 * context instead is invisible to that sweep, so these tests assert on the
 * animation's presence in LVGL's own list rather than on any internal flag.
 */

#include "ui_breakpoint.h"

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/grid_edit_scene.h"
#include "../test_helpers/scoped_animations_enabled.h"
#include "config.h"
#include "display_settings_manager.h"
#include "grid_edit_mode.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "theme_manager.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Put `em` into the exact state commit_resize_with_snap() runs from and
/// return the preview widget the snap animation will drive. Stops short of the
/// commit so a test can take its animation-count baseline with everything else
/// (notably select_widget()'s infinite selection pulse) already running.
lv_obj_t* arm_resize(GridEditMode& em, GridEditScene& scene) {
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::kPageIndex));
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);

    GridEditModeTestAccess::make_resize_preview(em, 0, 0, 40, 40);
    lv_obj_t* preview = GridEditModeTestAccess::resize_preview(em);
    REQUIRE(preview != nullptr);
    return preview;
}

/// Run the commit that starts the snap animation. It hands the preview over to
/// the animation and nulls resize_outline_, which is asserted here so that a
/// refactor which stops doing so cannot quietly make these tests vacuous.
void commit_snap_resize(GridEditMode& em) {
    // Grow the widget by one whole cell — any changed span reaches the animated
    // branch; the specific geometry is not what these tests are about. Whole
    // cells rather than one track because "temperature" does not declare
    // supports_half_col, so an odd track count is not a size it can hold.
    GridEditMode::ResizeResult result{0, 0, GridEditScene::kColspan + GridLayout::TRACKS_PER_CELL,
                                      GridEditScene::kRowspan};
    GridEditModeTestAccess::commit_resize(em, result);
    REQUIRE(GridEditModeTestAccess::resize_preview(em) == nullptr);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: snap animation is cancelled when its preview widget dies",
                 "[grid_edit][grid_edit_snap_anim]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    GridEditScene scene(test_screen(), "test_grid_edit_snap_anim_preview_death");

    GridEditMode em;
    bool rebuilt = false;
    em.set_rebuild_callback([&rebuilt]() { rebuilt = true; });

    lv_obj_t* preview = arm_resize(em, scene);
    // Deltas, not absolutes: select_widget()'s selection pulse repeats forever
    // and other tests in this binary leave their own animations behind.
    const uint16_t anims_before = lv_anim_count_running();
    commit_snap_resize(em);
    REQUIRE(lv_anim_count_running() == static_cast<uint16_t>(anims_before + 1));

    // The core invariant: the animation must be keyed on the widget it mutates.
    // lv_obj_destructor sweeps by `var == obj`, so an animation keyed on
    // anything else survives its own target's destruction and keeps writing
    // into freed memory for the rest of the 150ms.
    REQUIRE(lv_anim_get(preview, nullptr) != nullptr);

    lv_obj_delete(preview);
    CHECK(lv_anim_get(preview, nullptr) == nullptr);
    CHECK(lv_anim_count_running() == anims_before);

    // Past the 150ms duration: a surviving animation would have completed here
    // and fired its completion callback, rebuilding on behalf of a preview that
    // no longer exists. Cancellation must suppress that.
    process_lvgl(300);
    CHECK_FALSE(rebuilt);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: snap animation is cancelled when any outline bar dies",
                 "[grid_edit][grid_edit_snap_anim]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    GridEditScene scene(test_screen(), "test_grid_edit_snap_anim_bar_death");

    GridEditMode em;
    lv_obj_t* preview = arm_resize(em, scene);
    lv_obj_t* other_bar = GridEditModeTestAccess::resize_outline(em)[2];
    REQUIRE(other_bar != nullptr);
    REQUIRE(other_bar != preview);
    const uint16_t anims_before = lv_anim_count_running();
    commit_snap_resize(em);
    REQUIRE(lv_anim_get(preview, nullptr) != nullptr);

    // The exec callback writes every bar, so a bar that is not the animation's
    // var dying first must end it too, or the next frame writes freed memory.
    lv_obj_delete(other_bar);
    CHECK(lv_anim_get(preview, nullptr) == nullptr);
    CHECK(lv_anim_count_running() == anims_before);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: exit() cancels an in-flight snap animation",
                 "[grid_edit][grid_edit_snap_anim]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    GridEditScene scene(test_screen(), "test_grid_edit_snap_anim_exit");

    GridEditMode em;
    lv_obj_t* preview = arm_resize(em, scene);
    const uint16_t anims_before = lv_anim_count_running();
    commit_snap_resize(em);
    REQUIRE(lv_anim_count_running() == static_cast<uint16_t>(anims_before + 1));
    REQUIRE(lv_anim_get(preview, nullptr) != nullptr);

    // exit() nulls config_ and leaves the preview alive as a container child.
    // An animation that runs on past this point completes into a callback that
    // dereferences config_ unconditionally.
    em.exit();
    CHECK(lv_anim_get(preview, nullptr) == nullptr);
    CHECK(lv_anim_count_running() == anims_before);

    process_lvgl(300);

    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: destruction cancels an in-flight snap animation",
                 "[grid_edit][grid_edit_snap_anim]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    GridEditScene scene(test_screen(), "test_grid_edit_snap_anim_destruction");

    uint16_t anims_before = 0;
    lv_obj_t* preview = nullptr;
    {
        GridEditMode em;
        preview = arm_resize(em, scene);
        anims_before = lv_anim_count_running();
        commit_snap_resize(em);
        REQUIRE(lv_anim_count_running() == static_cast<uint16_t>(anims_before + 1));
        REQUIRE(lv_anim_get(preview, nullptr) != nullptr);
    }

    // The completion callback holds a raw GridEditMode* and writes seven of its
    // members. Application::shutdown() reaches lv_anim_delete_all() only AFTER
    // m_panels.reset() (src/application/application.cpp:4462-4465), so the
    // destructor is the last chance to stop this animation while `this` is
    // still alive.
    CHECK(lv_anim_get(preview, nullptr) == nullptr);
    CHECK(lv_anim_count_running() == anims_before);

    process_lvgl(300);

    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a page switch that stops the snap leaves the rebuild to exit",
                 "[grid_edit][grid_edit_snap_anim]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    GridEditScene scene(test_screen(), "test_grid_edit_snap_anim_switch");
    lv_obj_t* other_page = lv_obj_create(test_screen());

    GridEditMode em;
    int rebuilds = 0;
    em.set_rebuild_callback([&rebuilds]() { ++rebuilds; });
    arm_resize(em, scene);
    commit_snap_resize(em);
    REQUIRE(GridEditModeTestAccess::snap_animating(em));

    // The switch lays the widget out at its new cell but cannot rebuild its
    // content for the new span; leaving edit mode does.
    em.switch_page(other_page, 0);
    REQUIRE_FALSE(GridEditModeTestAccess::snap_animating(em));
    process_lvgl(50);
    REQUIRE(rebuilds == 0);
    em.exit();
    process_lvgl(50);
    CHECK(rebuilds == 1);

    lv_obj_delete(other_page);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: the snap eases the whole outline onto the committed cell",
                 "[grid_edit][grid_edit_snap_anim]") {
    helix::ui::ScopedAnimationsEnabled animations_on;
    GridEditScene scene(test_screen(), "test_grid_edit_snap_anim_target");
    // Off the screen's origin, so a target in the container's own coordinates
    // would land somewhere else.
    lv_obj_set_pos(scene.container, 40, 30);
    lv_obj_update_layout(scene.container);

    GridEditMode em;
    em.set_rebuild_callback([]() {});
    arm_resize(em, scene);
    const auto bars = GridEditModeTestAccess::resize_outline(em);
    commit_snap_resize(em);
    // Just short of the 150 ms ease-out, where the outline has all but arrived.
    process_lvgl(140);
    REQUIRE(GridEditModeTestAccess::snap_animating(em));

    lv_area_t content;
    lv_obj_get_content_coords(scene.container, &content);
    const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(em);
    const int target_x2 =
        content.x1 +
        static_cast<int>(grid_track_origin(m.cell_w, m.gutter, 0) +
                         grid_track_extent(m.cell_w, m.gutter,
                                           GridEditScene::kColspan + GridLayout::TRACKS_PER_CELL));
    lv_obj_t* layer = lv_display_get_layer_top(lv_obj_get_display(scene.container));
    lv_obj_update_layout(layer);
    lv_area_t right;
    lv_obj_get_coords(bars[3], &right);
    lv_area_t top;
    lv_obj_get_coords(bars[0], &top);
    INFO("right bar ends at " << right.x2 + 1 << ", target " << target_x2);
    CHECK(std::abs(right.x2 + 1 - target_x2) <= 3);
    CHECK(std::abs(top.y1 - content.y1) <= 3);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}
