// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_micro_home_placement.cpp
 * @brief The authored default home layout lands exactly on small grids.
 *
 * populate_widgets() decides anchor spans by asking each tile fits_at() over
 * pixel extents computed from the MEASURED tracks (grid_cell_metrics). A tile
 * whose fits_at() floors sizes against the nominal per-tier track
 * (GridLayout::GRID_CELL) instead rejects the span those same extents describe
 * whenever the content box quantises to a smaller track than the tier targets
 * (31.25px delivered against a 34px nominal at 480x272), and the grow walk
 * reseats it over a later anchor. The failures cascade into auto-placement and
 * the write-back persists the scramble.
 *
 * These cases drive the real defaults path: a fresh config's pending anchors
 * resolved through default_layout.json, then one populate_widgets(), at
 * shipping geometries, and read LVGL's own grid-cell styles back as the
 * oracle: every anchor the table seats must land at its authored cell and
 * span, unchanged.
 *
 * 320x240 is the honest edge: its content box (270x232) is an 8x6 grid with a
 * 32px column track, below the micro tier's 34px nominal, and the table
 * carries a micro@8x6 layout for it. Every anchor must still land exactly;
 * that is where a bad floor grows them.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_int_subject.h"
#include "ams_state.h"
#include "core/update_queue.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "src/ui/panel_widgets/print_status_widget.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// One authored anchor from assets/config/default_layout.json, in tracks.
struct AuthoredAnchor {
    const char* id;
    int col, row, colspan, rowspan;
};

/// Fixture teardown for the singletons seeding a populate-driven test. See
/// ContentFitsFixture in test_widget_content_fits.cpp for the ordering
/// constraints: the print-status formatter's observers must die before the
/// subjects they sit on, and the token table goes back to the fixture
/// geometry for the rest of the shard.
class DefaultPlacementFixture : public LVGLUITestFixture {
  public:
    ~DefaultPlacementFixture() override {
        PrintStatusWidget::destroy_formatter_for_test();
        ToolState::instance().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain_until_empty();
        if (lv_display_t* d = lv_display_get_default()) {
            theme_manager_refresh_layout_constants(d);
        }
    }

    /// Two tools and one extruder, so the temperature and print-status tiles
    /// have content to measure rather than an empty container that trivially
    /// fits. Same seeding the content-fit sweep uses.
    void seed_topology() {
        ToolState::instance().deinit_subjects();
        ToolState::instance().init_subjects(false);

        ToolTopology topo;
        topo.tool_count = 2;
        topo.active_tool = 0;
        ToolState::instance().set_ams_topology(topo);

        state().temperature_state().init_extruders({"extruder"});
        helix::ui::UpdateQueue::instance().drain();

        // The default table's spool slot resolves off this subject; pin it to
        // 0 so the slot does not take the AMS. Registered here when no earlier
        // case has, because an absent gate subject reads as open. Subjects are
        // process-global.
        if (lv_xml_get_subject(nullptr, "ams_slot_count") == nullptr) {
            helix::AmsState::instance().init_subjects(true);
        }
        lv_subject_t* ams = lv_xml_get_subject(nullptr, "ams_slot_count");
        REQUIRE(ams != nullptr);
        lv_subject_set_int(ams, 0);
    }

    /// A grid container whose CONTENT box is exactly content_w x content_h:
    /// the measured home content box at the panel resolution under test, so
    /// get_dimensions() quantises the same tracks the shipped panel does.
    lv_obj_t* make_grid_container(int content_w, int content_h) {
        lv_obj_t* container = lv_obj_create(test_screen());
        lv_obj_set_size(container, content_w, content_h);
        lv_obj_set_style_pad_all(container, 0, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
        return container;
    }

    /// Fresh-default populate: no saved config exists for a panel id unique
    /// to this case, so the config loads with pending anchors and
    /// populate_widgets() resolves them against this grid.
    lv_obj_t* populate_fresh_defaults(const std::string& panel_id, lv_obj_t* container) {
        auto& mgr = PanelWidgetManager::instance();
        mgr.clear_panel_config(panel_id);
        mgr.populate_widgets(panel_id, container, /*page_index=*/0);
        helix::ui::UpdateQueue::instance().drain_until_empty();
        return container;
    }

    /// Assert a widget's tile sits at an authored cell. LVGL's grid-cell
    /// styles are the oracle: they are exactly what the layout renders from,
    /// with no pixel tolerance to hide a one-track drift behind.
    void check_anchor(lv_obj_t* container, const AuthoredAnchor& a) {
        lv_obj_t* obj = lv_obj_find_by_name(container, a.id);
        if (obj == nullptr) {
            FAIL_CHECK("widget '" << a.id << "' missing from the grid");
            return;
        }
        CAPTURE(a.col, a.row, a.colspan, a.rowspan);
        CHECK(lv_obj_get_style_grid_cell_column_pos(obj, LV_PART_MAIN) == a.col);
        CHECK(lv_obj_get_style_grid_cell_row_pos(obj, LV_PART_MAIN) == a.row);
        CHECK(lv_obj_get_style_grid_cell_column_span(obj, LV_PART_MAIN) == a.colspan);
        CHECK(lv_obj_get_style_grid_cell_row_span(obj, LV_PART_MAIN) == a.rowspan);
    }
};

/// Forces mock backends: NetworkWidget's attach() otherwise builds the real
/// platform WiFi backend against this machine's state. See
/// test_widget_content_fits.cpp.
struct TestModeGuard {
    RuntimeConfig* rc;
    bool prev;
    explicit TestModeGuard(RuntimeConfig* r) : rc(r), prev(r->test_mode) {
        rc->test_mode = true;
    }
    ~TestModeGuard() {
        rc->test_mode = prev;
    }
};

} // namespace

// clang-format off
/// The micro table from assets/config/default_layout.json (base variant,
/// landscape): every anchor the 12x8 grid seats.
const AuthoredAnchor kMicroAnchors[] = {
    {"printer_image",    0,  0,  6, 4},
    {"temperature",      6,  0,  2, 2},
    {"bed_temperature",  8,  0,  2, 2},
    {"notifications",   10,  0,  2, 2},
    {"fan_stack",        6,  2,  2, 2},
    {"filament",         8,  2,  2, 2},
    {"led",             10,  2,  2, 2},
    {"print_status",     0,  4,  8, 4},
    {"temp_graph",       8,  4,  4, 4},
};

/// The medium table's fixed anchors: 800x480 is the regression guard for the
/// larger tiers, where the whole-cell floor does not apply and placement is
/// already correct. Tracks (4,2) to (7,3) are the spool slot, whose occupants
/// depend on the printer's capabilities, so the test checks them separately.
const AuthoredAnchor kMediumAnchors[] = {
    {"printer_image",    0, 0, 4, 4},
    {"temperature",      4, 0, 2, 2},
    {"bed_temperature",  6, 0, 2, 2},
    {"fan_stack",        8, 0, 2, 2},
    {"notifications",   10, 0, 2, 2},
    {"temp_graph",       8, 2, 4, 6},
    {"print_status",     0, 4, 8, 4},
};

/// The micro@8x6 table's fixed anchors. Tracks (4,0) to (5,1) are the readout
/// slot, whose occupant depends on the printer's capabilities.
const AuthoredAnchor kMicro8x6Anchors[] = {
    {"temperature",     0, 0, 2, 2},
    {"bed_temperature", 2, 0, 2, 2},
    {"notifications",   6, 0, 2, 2},
    {"print_status",    0, 2, 8, 4},
};
// clang-format on

TEST_CASE_METHOD(DefaultPlacementFixture, "micro default layout lands exactly at 480x272",
                 "[micro_home][panel_widget_manager]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution res(disp, 480, 272);
    theme_manager_refresh_layout_constants(disp);

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(lv_subject_get_int(bp_subj) == static_cast<int>(UiBreakpoint::Micro));
    REQUIRE(helix::GridLayout::gutter_px() > 0);
    require_font_tokens_distinct();

    TestModeGuard test_mode_guard(get_runtime_config());
    PanelWidgetManager::instance().init_widget_subjects();
    seed_topology();

    // The measured home content box at 480x272 (kShipping in
    // test_widget_content_fits.cpp): a 12x8 grid with 34.0x31.25px tracks;
    // the row track sits below the micro tier's 34px nominal target, which is
    // the condition the whole-cell floor must measure, not assume.
    lv_obj_t* container = make_grid_container(430, 264);
    const GridDimensions dims = GridLayout::get_dimensions(UiBreakpoint::Micro, 430, 264);
    REQUIRE(dims.cols == 12);
    REQUIRE(dims.rows == 8);
    const CellMetrics tracks = grid_cell_metrics(430, 264, dims.cols, dims.rows, 2);
    REQUIRE(tracks.cell_h < 34.0f);

    // The spool slot's occupants follow the capability subjects: no AMS
    // (seed_topology()), a filament sensor and an LED.
    helix::test::ScopedIntSubject dryer("home_dryer_available", 0);
    helix::test::ScopedIntSubject chamber("printer_has_chamber", 0);
    helix::test::ScopedIntSubject enclosed("printer_is_enclosed", 0);
    helix::test::ScopedIntSubject led("led_controllable", 1);
    helix::test::ScopedIntSubject fil("filament_sensor_count", 1);
    helix::test::ScopedIntSubject spoolman("printer_has_spoolman", 0);

    populate_fresh_defaults("test_micro_home_480", container);
    lv_obj_update_layout(container);
    REQUIRE(grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(container, LV_PART_MAIN)) ==
            12);

    for (const auto& a : kMicroAnchors) {
        check_anchor(container, a);
    }

    PanelWidgetManager::instance().clear_panel_config("test_micro_home_480");
}

TEST_CASE_METHOD(DefaultPlacementFixture, "micro default layout lands exactly at 320x240",
                 "[micro_home][panel_widget_manager]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution res(disp, 320, 240);
    theme_manager_refresh_layout_constants(disp);

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(lv_subject_get_int(bp_subj) == static_cast<int>(UiBreakpoint::Micro));
    REQUIRE(helix::GridLayout::gutter_px() > 0);
    require_font_tokens_distinct();

    TestModeGuard test_mode_guard(get_runtime_config());
    PanelWidgetManager::instance().init_widget_subjects();
    seed_topology();

    // Measured home content box at 320x240 (read off a live instance's
    // "[PanelWidgetManager] Grid layout" line): an 8x6 grid with 32x37px
    // tracks. The row axis has room to spare; the 32px column track is what
    // the floor must measure here.
    lv_obj_t* container = make_grid_container(270, 232);
    const GridDimensions dims = GridLayout::get_dimensions(UiBreakpoint::Micro, 270, 232);
    REQUIRE(dims.cols == 8);
    REQUIRE(dims.rows == 6);
    const CellMetrics tracks = grid_cell_metrics(270, 232, dims.cols, dims.rows, 2);
    REQUIRE(tracks.cell_w < 34.0f);

    populate_fresh_defaults("test_micro_home_320", container);
    lv_obj_update_layout(container);
    REQUIRE(grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(container, LV_PART_MAIN)) ==
            8);

    for (const auto& a : kMicro8x6Anchors) {
        check_anchor(container, a);
    }

    // The table switches these off on this grid rather than leaving them to
    // auto-placement, which has no free track to give them.
    for (const char* id : {"printer_image", "fan_stack"}) {
        INFO("widget '" << id << "' placed on a full grid");
        CHECK(lv_obj_find_by_name(container, id) == nullptr);
    }

    PanelWidgetManager::instance().clear_panel_config("test_micro_home_320");
}

TEST_CASE_METHOD(DefaultPlacementFixture, "medium default layout is unchanged at 800x480",
                 "[micro_home][panel_widget_manager]") {
    lv_display_t* disp = lv_display_get_default();
    REQUIRE(disp != nullptr);
    ScopedResolution res(disp, 800, 480);
    theme_manager_refresh_layout_constants(disp);

    lv_subject_t* bp_subj = theme_manager_get_breakpoint_subject();
    REQUIRE(bp_subj != nullptr);
    REQUIRE(lv_subject_get_int(bp_subj) == static_cast<int>(UiBreakpoint::Medium));
    REQUIRE(helix::GridLayout::gutter_px() > 0);
    require_font_tokens_distinct();

    TestModeGuard test_mode_guard(get_runtime_config());
    PanelWidgetManager::instance().init_widget_subjects();
    seed_topology();

    // The measured home content box at 800x480 (kShipping): a 12x8 grid.
    lv_obj_t* container = make_grid_container(710, 466);
    const GridDimensions dims = GridLayout::get_dimensions(UiBreakpoint::Medium, 710, 466);
    REQUIRE(dims.cols == 12);
    REQUIRE(dims.rows == 8);

    // The spool slot's occupants follow the capability subjects, so pin every
    // one it consults: with ams_slot_count at 0 (seed_topology()) it takes its
    // second alternative, filament in the cell beside the graph, then led.
    helix::test::ScopedIntSubject dryer("home_dryer_available", 0);
    helix::test::ScopedIntSubject chamber("printer_has_chamber", 0);
    helix::test::ScopedIntSubject enclosed("printer_is_enclosed", 0);
    helix::test::ScopedIntSubject led("led_controllable", 1);
    helix::test::ScopedIntSubject fil("filament_sensor_count", 1);
    helix::test::ScopedIntSubject spoolman("printer_has_spoolman", 0);

    populate_fresh_defaults("test_medium_home_800", container);
    lv_obj_update_layout(container);
    REQUIRE(grid_count_tracks(lv_obj_get_style_grid_column_dsc_array(container, LV_PART_MAIN)) ==
            12);

    for (const auto& a : kMediumAnchors) {
        check_anchor(container, a);
    }

    CHECK(lv_obj_find_by_name(container, "ams") == nullptr);
    check_anchor(container, {"filament", 6, 2, 2, 2});
    check_anchor(container, {"led", 4, 2, 2, 2});

    PanelWidgetManager::instance().clear_panel_config("test_medium_home_800");
}
