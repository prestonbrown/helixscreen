// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_bed_mesh_tile.cpp
 * @brief The bed mesh tile draws what its ladder promises at each rung: which parts carry
 * HIDDEN, what its texts say, how its canvas is fed, and what its buttons send.
 *
 * Boxes are the pixel budgets measured on a live grid (55x54, 114x54, 55x113, 114x113 at
 * medium; 51x53 / 51x112 / 108x53 at large; 70x64 at micro). The decision is
 * bed_mesh_tile_layout.h's; these tests pin its wiring to the real fonts and XML.
 *
 * Run with: ./build/bin/helix-tests "[widget_size][bed_mesh_tile]"
 */

#include "ui_bed_mesh.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/tile_boxes.h"
#include "app_globals.h"
#include "bed_mesh_state.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/bed_mesh_tile_widget.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::test::Box;
using helix::test::kCell;
using helix::test::kColumn;
using helix::test::kFull;
using helix::test::kGlance;
using helix::test::kLarge;
using helix::test::kStrip;
using helix::test::kWide;

namespace {

BedMeshProfile make_profile(float spread, const char* name = "default", int n = 5) {
    BedMeshProfile p;
    p.name = name;
    for (int r = 0; r < n; ++r) {
        std::vector<float> row;
        for (int c = 0; c < n; ++c) {
            // a plane tilted across the bed, so the extremes sit in opposite corners
            row.push_back(spread * (static_cast<float>(r + c) / static_cast<float>(2 * (n - 1))));
        }
        p.probed_matrix.push_back(row);
    }
    p.x_count = p.y_count = n;
    p.mesh_min[0] = p.mesh_min[1] = 20.0f;
    p.mesh_max[0] = p.mesh_max[1] = 200.0f;
    return p;
}

struct BedMeshTileFixture : public LVGLUITestFixture {
    BedMeshTileFixture()
        : mock(MoonrakerClientMock::PrinterType::VORON_24), mock_api(mock, state()) {
        state().set_klippy_state_sync(KlippyState::READY);
        mock.connect("ws://mock/websocket", []() {}, []() {});
        previous_api = get_moonraker_api();
        set_moonraker_api(&mock_api);

        BedMeshState::instance().deinit_subjects();
        BedMeshState::instance().init_subjects();
        PanelWidgetManager::instance().init_widget_subjects();
        set_bed_mesh_tile_full_tier_for_test(true);
        set_bed_mesh_tile_open_hook_for_test([this]() { ++opened; });
        reset_state();
        sentinel();
    }
    ~BedMeshTileFixture() override {
        set_bed_mesh_tile_open_hook_for_test({});
        set_bed_mesh_tile_full_tier_for_test(std::nullopt);
        set_print_active(false);
        set_machine_blocked(false);
        set_moonraker_api(previous_api);
        mock.stop_temperature_simulation();
        mock.disconnect();
        BedMeshState::instance().deinit_subjects();
        drain();
    }

    void reset_state() {
        set_print_active(false);
        set_machine_blocked(false);
        BedMeshState::instance().apply(nullptr, 0);
        lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "xyz");
        drain();
    }
    void load_mesh(float spread, int profiles = 2, const char* name = "default") {
        const BedMeshProfile p = make_profile(spread, name);
        BedMeshState::instance().apply(&p, profiles);
        drain();
    }
    void clear_mesh(int profiles = 0) {
        BedMeshState::instance().apply(nullptr, profiles);
        drain();
    }
    void set_print_active(bool on) {
        lv_subject_set_int(state().print_state().get_print_active_subject(), on ? 1 : 0);
        drain();
    }
    void set_machine_blocked(bool on) {
        lv_subject_set_int(state().print_state().get_machine_motion_blocked_subject(), on ? 1 : 0);
        drain();
    }
    void drain() {
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
    /// A gcode no action sends, so "nothing was sent" is distinguishable.
    void sentinel() {
        mock_api.execute_gcode("M117 sentinel", nullptr, nullptr);
        REQUIRE(last_sent() == "M117 sentinel");
    }
    const std::string& last_sent() const {
        return mock.last_send_script();
    }
    void tap(lv_obj_t* obj) {
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }

    MoonrakerClientMock mock;
    MoonrakerAPI mock_api;
    IMoonrakerAPI* previous_api = nullptr;
    int opened = 0;
};

using Harness = PanelWidgetHarness<BedMeshTileWidget>;

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

std::string subject_text(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_string(subject);
}

int rung() {
    return subject_int("bed_mesh_tile_rung");
}

lv_obj_t* part(Harness& h, const char* name) {
    lv_obj_t* obj = h.child(name);
    REQUIRE(obj != nullptr);
    return obj;
}

bool hidden(lv_obj_t* obj) {
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/// Whether an object draws: it and every ancestor up to the widget root is unhidden.
bool shown(lv_obj_t* obj) {
    for (lv_obj_t* o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
        if (hidden(o))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: medium-tier budgets land on their rung",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    const Box boxes[] = {kGlance, kStrip, kColumn, kCell, kWide, kFull, kLarge};
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: with no mesh the budgets still land on a rung",
                 "[widget_size][bed_mesh_tile]") {
    Harness h(test_screen(), state());
    // Strip and Column would draw what Glance does, so a half cell is Glance.
    const Box boxes[] = {
        {55, 54, SizeRung::Glance},
        {114, 54, SizeRung::Glance},
        {55, 113, SizeRung::Glance},
        {114, 113, SizeRung::Cell},
        kWide,
        kFull,
        kLarge,
    };
    for (const auto& b : boxes) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(rung() == static_cast<int>(b.rung));
    }
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: with no mesh a box draws the same parts whatever it was before",
                 "[widget_size][bed_mesh_tile]") {
    Harness h(test_screen(), state());
    CHECK(h.unsettled_boxes(box_sweep(114, 114, 40, 240)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(234, 234, 40, 240)) == std::vector<std::string>{});
    CHECK(h.unsettled_boxes(box_sweep(20, 300, 113, 113)) == std::vector<std::string>{});
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: each rung shows exactly its parts, mesh loaded",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(shown(part(h, "bed_mesh_tile_glyph")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_slot")));
    CHECK(shown(part(h, "bed_mesh_tile_range")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_glyph_off")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_details")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_scale")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_footer")));

    for (const Box b : {kStrip, kColumn, kCell}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_glyph")));
        CHECK(shown(part(h, "bed_mesh_tile_slot")));
        CHECK(shown(part(h, "bed_mesh_tile_range")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_details")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_scale")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_footer")));
    }

    h.resize(1, 1, kWide.w, kWide.h);
    CHECK(shown(part(h, "bed_mesh_tile_slot")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_range")));
    CHECK(shown(part(h, "bed_mesh_tile_details")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_scale")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_footer")));

    for (const Box b : {kFull, kLarge}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(shown(part(h, "bed_mesh_tile_slot")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_range")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_details")));
        CHECK(shown(part(h, "bed_mesh_tile_scale")));
        CHECK(shown(part(h, "bed_mesh_tile_footer")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_actions")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_empty_text")));
    }
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: with no mesh the heatmap gives way to a glyph",
                 "[widget_size][bed_mesh_tile]") {
    Harness h(test_screen(), state());

    for (const Box b : {kGlance, kStrip, kColumn}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(shown(part(h, "bed_mesh_tile_glyph_off")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_glyph")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_glyph_empty")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_slot")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_range")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_empty_text")));
    }

    for (const Box b : {kCell, kWide, kFull}) {
        INFO(b.w << "x" << b.h);
        h.resize(1, 1, b.w, b.h);
        CHECK(shown(part(h, "bed_mesh_tile_glyph_empty")));
        CHECK(shown(part(h, "bed_mesh_tile_empty_text")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_glyph_off")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_slot")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_details")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_scale")));
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_footer")));
    }
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: Calibrate and Load profile show on an idle printer with no mesh",
                 "[widget_size][bed_mesh_tile]") {
    clear_mesh(2);
    Harness h(test_screen(), state());

    for (const Box b : {kCell, kWide}) {
        h.resize(1, 1, b.w, b.h);
        CHECK_FALSE(shown(part(h, "bed_mesh_tile_actions")));
    }

    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(shown(part(h, "bed_mesh_tile_calibrate")));
    CHECK(shown(part(h, "bed_mesh_tile_load")));

    // No stored profile: there is nothing to load.
    clear_mesh(0);
    CHECK(shown(part(h, "bed_mesh_tile_calibrate")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_load")));

    // A mesh that is loaded needs neither.
    load_mesh(0.10f);
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_actions")));
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: a print hides the buttons and re-decides the rung",
                 "[widget_size][bed_mesh_tile]") {
    clear_mesh(2);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(shown(part(h, "bed_mesh_tile_actions")));

    set_print_active(true);
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_actions")));
    CHECK(shown(part(h, "bed_mesh_tile_empty_text")));

    set_print_active(false);
    CHECK(shown(part(h, "bed_mesh_tile_actions")));
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: the texts follow the mesh",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f, 3, "adaptive");
    Harness h(test_screen(), state());

    h.resize(1, 1, 55, 54);
    CHECK(subject_text("bed_mesh_tile_range_text") == "0.10");
    h.resize(1, 1, 114, 113);
    CHECK(subject_text("bed_mesh_tile_range_text") == "0.100");
    CHECK(subject_text("bed_mesh_tile_extent_text") == "0.00..0.10");
    CHECK(subject_text("bed_mesh_tile_scale_low_text") == "0.00");
    CHECK(subject_text("bed_mesh_tile_scale_high_text") == "0.10");
    CHECK(subject_text("bed_mesh_tile_count_text") == "3 profiles");

    load_mesh(0.40f, 1, "cold");
    CHECK(subject_text("bed_mesh_tile_range_text") == "0.400");
    CHECK(subject_text("bed_mesh_tile_count_text") == "1 profile");

    clear_mesh(0);
    CHECK(subject_text("bed_mesh_tile_range_text").empty());
    CHECK(subject_text("bed_mesh_tile_extent_text").empty());
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: flatness status tints the range",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    const auto range_color = [&] {
        return lv_obj_get_style_text_color(part(h, "bed_mesh_tile_range"), LV_PART_MAIN);
    };

    CHECK(subject_int("bed_mesh_tile_status") == 0);
    CHECK(lv_color_eq(range_color(), theme_manager_get_color("success")));
    load_mesh(0.20f);
    CHECK(subject_int("bed_mesh_tile_status") == 1);
    CHECK(lv_color_eq(range_color(), theme_manager_get_color("warning")));
    load_mesh(0.50f);
    CHECK(subject_int("bed_mesh_tile_status") == 2);
    CHECK(lv_color_eq(range_color(), theme_manager_get_color("danger")));
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: the canvas is static, 2D below Large, fed once per mesh",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    lv_obj_t* map = part(h, "bed_mesh_tile_map");

    CHECK_FALSE(ui_bed_mesh_is_async_mode(map));
    CHECK(ui_bed_mesh_has_data(map));
    CHECK(ui_bed_mesh_get_render_mode(map) == BedMeshRenderMode::Force2D);
    // A static canvas draws nothing interactive: it takes no press, so a tap reaches the tile.
    CHECK_FALSE(lv_obj_has_flag(map, LV_OBJ_FLAG_CLICKABLE));

    tap(map);
    CHECK(opened == 1);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: the canvas renders once per mesh and per size, not per tick",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    lv_obj_t* map = part(h, "bed_mesh_tile_map");
    const auto draw = [&] {
        lv_obj_invalidate(map);
        lv_refr_now(nullptr);
    };

    draw();
    CHECK(ui_bed_mesh_static_render_count(map) == 1);
    draw();
    draw();
    CHECK(ui_bed_mesh_static_render_count(map) == 1);

    // Readings that are not a new mesh do not re-feed it.
    set_print_active(true);
    set_print_active(false);
    draw();
    CHECK(ui_bed_mesh_static_render_count(map) == 1);

    load_mesh(0.30f, 2, "cold");
    draw();
    CHECK(ui_bed_mesh_static_render_count(map) == 2);

    h.resize(1, 1, kWide.w, kWide.h);
    draw();
    CHECK(ui_bed_mesh_static_render_count(map) == 3);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: the bed outline arriving feeds the canvas again",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    lv_obj_t* map = part(h, "bed_mesh_tile_map");
    lv_refr_now(nullptr);
    REQUIRE(ui_bed_mesh_static_render_count(map) == 1);

    lv_subject_t* version = mock_api.get_build_volume_version_subject();
    lv_subject_set_int(version, lv_subject_get_int(version) + 1);
    drain();
    lv_obj_invalidate(map);
    lv_refr_now(nullptr);
    CHECK(ui_bed_mesh_static_render_count(map) == 2);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: a rung without the map feeds and renders nothing",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, 55, 54);
    lv_obj_t* map = part(h, "bed_mesh_tile_map");
    load_mesh(0.30f, 2, "cold");
    lv_refr_now(nullptr);
    CHECK_FALSE(ui_bed_mesh_has_data(map));
    CHECK(ui_bed_mesh_static_render_count(map) == 0);

    // The mesh is fed the moment a rung shows the map.
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK(ui_bed_mesh_has_data(map));
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: Large draws 3D on a full-tier board and 2D elsewhere",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    {
        Harness h(test_screen(), state());
        h.resize(1, 1, kLarge.w, kLarge.h);
        CHECK(ui_bed_mesh_get_render_mode(part(h, "bed_mesh_tile_map")) ==
              BedMeshRenderMode::Force3D);
        h.resize(1, 1, kFull.w, kFull.h);
        CHECK(ui_bed_mesh_get_render_mode(part(h, "bed_mesh_tile_map")) ==
              BedMeshRenderMode::Force2D);
    }
    set_bed_mesh_tile_full_tier_for_test(false);
    {
        Harness h(test_screen(), state());
        h.resize(1, 1, kLarge.w, kLarge.h);
        REQUIRE(rung() == static_cast<int>(SizeRung::Large));
        CHECK(ui_bed_mesh_get_render_mode(part(h, "bed_mesh_tile_map")) ==
              BedMeshRenderMode::Force2D);
    }
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: fits_at refuses what Glance cannot draw",
                 "[widget_size][bed_mesh_tile]") {
    BedMeshTileWidget widget(state());
    CHECK(widget.fits_at(55, 54));
    CHECK(widget.fits_at(55, 113));
    CHECK(widget.fits_at(114, 54));
    CHECK_FALSE(widget.fits_at(20, 20));
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        CHECK(widget.fits_at(51, 53));
        CHECK(widget.fits_at(51, 112));
        CHECK(widget.fits_at(108, 53));
    }
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
        CHECK(widget.fits_at(70, 64));
    }

    bool fitted = false;
    for (int w = 0; w <= 200; ++w) {
        const bool now = widget.fits_at(w, 113);
        if (fitted)
            CHECK(now);
        fitted = fitted || now;
    }
    bool fitted_h = false;
    for (int hgt = 0; hgt <= 200; ++hgt) {
        const bool now = widget.fits_at(114, hgt);
        if (fitted_h)
            CHECK(now);
        fitted_h = fitted_h || now;
    }
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: half-cell boxes at large and micro land on a rung",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Large);
        Harness h(test_screen(), state());
        h.resize(1, 1, 51, 53);
        CHECK(rung() == static_cast<int>(SizeRung::Glance));
        h.resize(1, 1, 51, 112);
        CHECK(rung() == static_cast<int>(SizeRung::Column));
        h.resize(1, 1, 108, 53);
        CHECK(rung() == static_cast<int>(SizeRung::Strip));
    }
    {
        helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
        Harness h(test_screen(), state());
        h.resize(1, 1, 70, 64);
        CHECK(rung() >= static_cast<int>(SizeRung::Glance));
    }
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: a transient instance does not publish over the live tile",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(rung() == static_cast<int>(SizeRung::Full));

    BedMeshTileWidget transient(state());
    transient.notify_size_changed(1, 1, 55, 54);
    CHECK(rung() == static_cast<int>(SizeRung::Full));
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: a transient instance leaving hands the tile back",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(2);
    Harness live(test_screen(), state());
    live.resize(1, 1, kFull.w, kFull.h);
    {
        Harness transient(test_screen(), state());
        transient.resize(1, 1, 55, 54);
        REQUIRE(rung() == static_cast<int>(SizeRung::Glance));
    }

    // The live tile owns its subjects again and still acts.
    CHECK(rung() == static_cast<int>(SizeRung::Full));
    tap(part(live, "bed_mesh_tile_load"));
    CHECK(opened == 1);
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: a tap anywhere opens the Bed Mesh panel",
                 "[widget_size][bed_mesh_tile][actions]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    for (const Box b : {kGlance, kCell, kWide, kFull}) {
        opened = 0;
        h.resize(1, 1, b.w, b.h);
        tap(h.root());
        CHECK(opened == 1);
    }
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: Load profile opens the panel and not twice",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(2);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    tap(part(h, "bed_mesh_tile_load"));
    // The button's tap does not bubble into the tile's own tap.
    CHECK(opened == 1);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: Calibrate homes first when the toolhead is not homed",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(0);
    lv_subject_copy_string(state().motion_state().get_homed_axes_subject(), "");
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    tap(part(h, "bed_mesh_tile_calibrate"));
    CHECK(opened == 0);
    CHECK(last_sent() == "G28");
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: Calibrate on a homed toolhead sends the mesh calibration",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(0);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    tap(part(h, "bed_mesh_tile_calibrate"));
    CHECK(opened == 0);
    CHECK(last_sent().find("BED_MESH_CALIBRATE") != std::string::npos);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: Calibrate disables while a job holds the machine",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(0);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    CHECK_FALSE(lv_obj_has_state(part(h, "bed_mesh_tile_calibrate"), LV_STATE_DISABLED));
    set_machine_blocked(true);
    CHECK(lv_obj_has_state(part(h, "bed_mesh_tile_calibrate"), LV_STATE_DISABLED));

    // The callback refuses too, whatever the button's state.
    tap(part(h, "bed_mesh_tile_calibrate"));
    CHECK(last_sent() == "M117 sentinel");
    set_machine_blocked(false);
    CHECK_FALSE(lv_obj_has_state(part(h, "bed_mesh_tile_calibrate"), LV_STATE_DISABLED));
}

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: hidden buttons send nothing",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(0);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    set_print_active(true);
    REQUIRE_FALSE(shown(part(h, "bed_mesh_tile_calibrate")));

    // A tap on a hidden button cannot happen on a device; the callback must still refuse.
    tap(part(h, "bed_mesh_tile_calibrate"));
    CHECK(last_sent() == "M117 sentinel");
    CHECK(opened == 0);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: Calibrate sends once while the probe runs and is hidden meanwhile",
                 "[widget_size][bed_mesh_tile][actions]") {
    clear_mesh(0);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    REQUIRE(shown(part(h, "bed_mesh_tile_calibrate")));

    tap(part(h, "bed_mesh_tile_calibrate"));
    REQUIRE(last_sent().find("BED_MESH_CALIBRATE") != std::string::npos);
    CHECK(h.widget().calibrating());
    CHECK(subject_int("bed_mesh_tile_calibrating") == 1);
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_calibrate")));

    sentinel();
    tap(part(h, "bed_mesh_tile_calibrate"));
    CHECK(last_sent() == "M117 sentinel");

    // The finished mesh ends the wait.
    load_mesh(0.10f);
    CHECK_FALSE(h.widget().calibrating());
    CHECK(subject_int("bed_mesh_tile_calibrating") == 0);
}

TEST_CASE_METHOD(
    BedMeshTileFixture,
    "bed_mesh_tile: without the Bed Mesh panel Load profile is gone and taps do nothing",
    "[widget_size][bed_mesh_tile][actions]") {
    set_bed_mesh_tile_panel_available_for_test(false);
    clear_mesh(2);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);

    CHECK(shown(part(h, "bed_mesh_tile_calibrate")));
    CHECK_FALSE(shown(part(h, "bed_mesh_tile_load")));
    tap(part(h, "bed_mesh_tile_load"));
    tap(h.root());
    CHECK(opened == 0);

    set_bed_mesh_tile_panel_available_for_test(std::nullopt);
}

TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: a covered home panel skips mesh updates until it returns",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, kFull.w, kFull.h);
    lv_obj_t* map = part(h, "bed_mesh_tile_map");
    lv_refr_now(nullptr);
    REQUIRE(ui_bed_mesh_static_render_count(map) == 1);
    const std::string before = subject_text("bed_mesh_tile_range_text");

    h.widget().on_deactivate();
    load_mesh(0.30f, 2, "cold");
    lv_obj_invalidate(map);
    lv_refr_now(nullptr);
    CHECK(ui_bed_mesh_static_render_count(map) == 1);
    CHECK(subject_text("bed_mesh_tile_range_text") == before);

    h.widget().on_activate();
    lv_obj_invalidate(map);
    lv_refr_now(nullptr);
    CHECK(ui_bed_mesh_static_render_count(map) == 2);
    CHECK(subject_text("bed_mesh_tile_range_text") != before);
}

#if HELIX_HAS_BED_MESH_3D
TEST_CASE_METHOD(BedMeshTileFixture,
                 "bed_mesh_tile: after a slow 3D render a rung change does not bring 3D back",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.10f);
    Harness h(test_screen(), state());
    h.resize(1, 1, kLarge.w, kLarge.h);
    lv_obj_t* map = part(h, "bed_mesh_tile_map");
    REQUIRE(ui_bed_mesh_get_render_mode(map) == BedMeshRenderMode::Force3D);

    ui_bed_mesh_set_static_render_budget_ms_for_test(0.0001f);
    lv_obj_invalidate(map);
    lv_refr_now(nullptr);
    ui_bed_mesh_set_static_render_budget_ms_for_test(0.0f);
    REQUIRE(ui_bed_mesh_static_3d_banned(map));

    h.resize(1, 1, kFull.w, kFull.h);
    h.resize(1, 1, kLarge.w, kLarge.h);
    CHECK(ui_bed_mesh_get_render_mode(map) == BedMeshRenderMode::Force2D);
}
#endif

TEST_CASE_METHOD(BedMeshTileFixture, "bed_mesh_tile: clearing the mesh clears the flatness status",
                 "[widget_size][bed_mesh_tile]") {
    load_mesh(0.50f);
    Harness h(test_screen(), state());
    h.resize(1, 1, 114, 113);
    REQUIRE(subject_int("bed_mesh_tile_status") == 2);

    clear_mesh(0);

    CHECK(subject_int("bed_mesh_tile_status") == 0);
}
