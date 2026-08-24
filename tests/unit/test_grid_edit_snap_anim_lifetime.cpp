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

/// HelixTestFixture forces animations OFF for every test (see the long note in
/// tests/helix_test_fixture.cpp — modal exit timing). commit_resize_with_snap()
/// only animates when they are ON, so without this the whole snap-animation
/// branch is skipped and every assertion below passes against nothing.
class ScopedAnimationsEnabled {
  public:
    ScopedAnimationsEnabled() {
        subject_ = lv_xml_get_subject(nullptr, "settings_animations_enabled");
        if (subject_) {
            prev_ = lv_subject_get_int(subject_);
            lv_subject_set_int(subject_, 1);
        }
    }
    ~ScopedAnimationsEnabled() {
        if (subject_) {
            lv_subject_set_int(subject_, prev_);
        }
    }
    ScopedAnimationsEnabled(const ScopedAnimationsEnabled&) = delete;
    ScopedAnimationsEnabled& operator=(const ScopedAnimationsEnabled&) = delete;

  private:
    lv_subject_t* subject_ = nullptr;
    int prev_ = 0;
};

constexpr int COLSPAN = 2;
constexpr int ROWSPAN = 2;

/// Container + one named child widget + a matching config page, sized so
/// current_metrics() reports real cell geometry. Mirrors the setup in
/// test_grid_edit_drag_path.cpp, minus the synthetic indev: these tests drive
/// commit_resize_with_snap() directly, so no pointer choreography is needed.
struct ResizeScene {
    lv_obj_t* container = nullptr;
    lv_obj_t* widget = nullptr;
    PanelWidgetConfig* config = nullptr;
    std::string panel_id;

    // lv_obj_set_grid_dsc_array() stores the POINTER, so these must outlive
    // the container. As constructor locals they dangled, and current_metrics()
    // read off the end of them on every enter() (caught by valgrind).
    std::vector<int32_t> col_dsc;
    std::vector<int32_t> row_dsc;

    ResizeScene(lv_obj_t* parent, std::string id) : panel_id(std::move(id)) {
        const int gutter = theme_manager_get_spacing("space_xs");
        REQUIRE(gutter > 0);

        const int ncols = GridLayout::get_cols(UiBreakpoint::Medium);
        const int nrows = GridLayout::get_rows(UiBreakpoint::Medium);
        constexpr int CELL_PX = 30;
        const int content_w = ncols * CELL_PX + (ncols - 1) * gutter;
        const int content_h = nrows * CELL_PX + (nrows - 1) * gutter;

        container = lv_obj_create(parent);
        lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_size(container, content_w, content_h);

        col_dsc = GridLayout::make_col_dsc(UiBreakpoint::Medium);
        row_dsc = GridLayout::make_row_dsc(UiBreakpoint::Medium);
        lv_obj_set_grid_dsc_array(container, col_dsc.data(), row_dsc.data());
        lv_obj_set_style_pad_column(container, gutter, 0);
        lv_obj_set_style_pad_row(container, gutter, 0);

        widget = lv_obj_create(container);
        lv_obj_set_name(widget, "temperature");
        lv_obj_remove_flag(widget, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_grid_cell(widget, LV_GRID_ALIGN_STRETCH, 0, COLSPAN, LV_GRID_ALIGN_STRETCH, 0,
                             ROWSPAN);
        lv_obj_update_layout(container);

        // Widget lives on page 1: PanelWidgetConfig::load() appends registry
        // defaults onto page 0 only, so a second page keeps the entry list
        // exactly what this test wrote.
        auto* cfg = Config::get_instance();
        cfg->set<nlohmann::json>(
            cfg->df() + "panel_widgets/" + panel_id,
            nlohmann::json{{"main_page_index", 0},
                           {"next_page_id", 2},
                           {"pages",
                            {{{"id", "main"}, {"widgets", nlohmann::json::array()}},
                             {{"id", "spy"},
                              {"widgets",
                               {{{"id", "temperature"},
                                 {"enabled", true},
                                 {"col", 0},
                                 {"row", 0},
                                 {"colspan", COLSPAN},
                                 {"rowspan", ROWSPAN}}}}}}}});

        auto& mgr = PanelWidgetManager::instance();
        mgr.get_widget_config(panel_id).mark_dirty();
        mgr.clear_panel_config(panel_id);
        config = &mgr.get_widget_config(panel_id);

        // A widget ID parse_widget_array() doesn't recognise is dropped
        // silently, which would make commit_resize_with_snap() bail at its
        // find_config_index_for_widget() guard and never start an animation.
        REQUIRE(config->page_entries(PAGE_INDEX).size() == 1);
    }

    ~ResizeScene() {
        PanelWidgetManager::instance().clear_panel_config(panel_id);
    }

    ResizeScene(const ResizeScene&) = delete;
    ResizeScene& operator=(const ResizeScene&) = delete;

    static constexpr size_t PAGE_INDEX = 1;
};

/// Put `em` into the exact state commit_resize_with_snap() runs from and
/// return the preview widget the snap animation will drive. Stops short of the
/// commit so a test can take its animation-count baseline with everything else
/// (notably select_widget()'s infinite selection pulse) already running.
lv_obj_t* arm_resize(GridEditMode& em, ResizeScene& scene) {
    em.enter(scene.container, scene.config, static_cast<int>(ResizeScene::PAGE_INDEX));
    em.select_widget(scene.widget);
    REQUIRE(em.selected_widget() == scene.widget);

    GridEditModeTestAccess::make_resize_preview(em, 0, 0, 40, 40);
    lv_obj_t* preview = GridEditModeTestAccess::resize_preview(em);
    REQUIRE(preview != nullptr);
    return preview;
}

/// Run the commit that starts the snap animation. It hands the preview over to
/// the animation and nulls resize_preview_, which is asserted here so that a
/// refactor which stops doing so cannot quietly make these tests vacuous.
void commit_snap_resize(GridEditMode& em) {
    // Grow the widget by one column — any changed span reaches the animated
    // branch; the specific geometry is not what these tests are about.
    GridEditMode::ResizeResult result{0, 0, COLSPAN + 1, ROWSPAN};
    GridEditModeTestAccess::commit_resize(em, result);
    REQUIRE(GridEditModeTestAccess::resize_preview(em) == nullptr);
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: snap animation is cancelled when its preview widget dies",
                 "[grid_edit][grid_edit_snap_anim]") {
    ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    ResizeScene scene(test_screen(), "test_grid_edit_snap_anim_preview_death");

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

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: exit() cancels an in-flight snap animation",
                 "[grid_edit][grid_edit_snap_anim]") {
    ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    ResizeScene scene(test_screen(), "test_grid_edit_snap_anim_exit");

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
    ScopedAnimationsEnabled animations_on;
    REQUIRE(DisplaySettingsManager::instance().get_animations_enabled());

    ResizeScene scene(test_screen(), "test_grid_edit_snap_anim_destruction");

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
