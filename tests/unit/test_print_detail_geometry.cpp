// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_detail_geometry.cpp
 * @brief Screen-edge geometry of the print file detail view across canvases
 *
 * The layout contract this file pins, per canvas:
 *   - Delete and Print sit OUTSIDE the options scroll area, so both stay
 *     fully on screen whatever the option tiles do to the content height.
 *   - Every option tile keeps its label inside the tile outline (long names
 *     wrap; they may not run under a neighbour or past the border), and the
 *     corner check tab never covers the label.
 *   - The more-below cue's subject equals "scroll bottom > 0" of the options
 *     scroll area it reports on.
 *   - In portrait, the measured preview height (fit_portrait_preview) leaves
 *     the first tile row fully visible whenever the width/3 floor did not
 *     clamp the card.
 *
 * The option set is the real Snapmaker U1 database entry (four options, two
 * tile rows), injected into the global PrinterState the view reads.
 */

#include "ui_callback_helpers.h"
#include "ui_print_select_detail_view.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/printer_state_test_access.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "pre_print_option.h"
#include "printer_detector.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <memory>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct Canvas {
    int w;
    int h;
};

/// No-op stand-ins for the print_file_detail.xml event callbacks (normally
/// registered by PrintSelectPanel's init_subjects). The XML references them
/// at create() time; the handlers themselves don't matter to geometry.
void detail_noop_cb(lv_event_t* /*e*/) {}

void register_detail_noops() {
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
        {"on_print_select_detail_objects", detail_noop_cb},
    });
}

struct Rect {
    int32_t x1, y1, x2, y2;
};

Rect coords_of(lv_obj_t* o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return {a.x1, a.y1, a.x2, a.y2};
}

bool rect_intersects(const Rect& a, const Rect& b) {
    return !(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

bool rect_contains(const Rect& outer, const Rect& inner) {
    return inner.x1 >= outer.x1 && inner.y1 >= outer.y1 && inner.x2 <= outer.x2 &&
           inner.y2 <= outer.y2;
}

/// A started mock backend on AmsState, torn down on scope exit. AmsState is a
/// singleton, so a test that installs a backend and walks away leaves it live for
/// every test that runs after it. Mirrors the wiring in
/// test_print_select_detail_subjects.cpp.
struct ScopedAmsBackend {
    helix::AmsBackendMock* backend = nullptr;

    explicit ScopedAmsBackend(int slot_count) {
        auto& ams = helix::AmsState::instance();
        ams.init_subjects(false);
        auto owned = std::make_unique<helix::AmsBackendMock>(slot_count);
        backend = owned.get();
        backend->set_operation_delay(0);
        ams.set_backend(std::move(owned));
        backend->start();
    }

    ~ScopedAmsBackend() {
        helix::ui::UpdateQueue::instance().drain();
        if (backend) {
            backend->stop();
        }
        auto& ams = helix::AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
    }
};

/// Hide the view and run everything show() deferred, so the tree is gone
/// before the next canvas builds a fresh one.
struct CloseOnExit {
    helix::ui::PrintSelectDetailView& v;
    ~CloseOnExit() {
        v.hide();
        helix::ui::UpdateQueue::instance().drain();
    }
};

/// Build the view for @p ps at the fixture display's current resolution,
/// show a file, and let every deferred layout pass run. Fails the case
/// (REQUIRE) if the tree cannot be built.
lv_obj_t* build_and_show_detail(LVGLTestFixture& fx, helix::ui::PrintSelectDetailView& view,
                                PrinterState& ps, lv_obj_t* parent,
                                const std::vector<std::string>& filament_colors = {},
                                const std::vector<std::string>& filament_materials = {}) {
    register_detail_noops();
    view.set_dependencies(nullptr, &ps);
    view.init_subjects();
    lv_obj_t* const root = view.create(parent);
    REQUIRE(root != nullptr);
    view.show("benchy.gcode", "", "PLA", filament_colors, filament_materials);
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_update_layout(root);
    // Settle: a deferred fit (fit_portrait_preview) changes the card height,
    // which re-lays the tree and defers the next fit, so pump layout + queue
    // until two rounds produce no further change.
    int prev_h = -1;
    for (int i = 0; i < 6; ++i) {
        fx.process_lvgl(20);
        helix::ui::UpdateQueue::instance().drain();
        lv_obj_update_layout(root);
        const int h = lv_obj_get_height(lv_obj_find_by_name(root, "detail_card"));
        if (h == prev_h)
            break;
        prev_h = h;
    }
    return root;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "print file detail keeps its buttons on screen and its tiles intact at five "
                 "canvases",
                 "[print_select][detail][geometry][print_detail_geometry]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::set_option_set(
        ps, PrinterDetector::get_pre_print_option_set("Snapmaker U1"));
    PrinterStateTestAccess::refresh_option_visibility(ps);
    // Setup reached the branch: four options make has_any_preprint_options 1,
    // so the options card (and its tiles) exist to measure.
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "has_any_preprint_options")) == 1);

    // A multi-lane filament system with a two-tool palette, so the scroll
    // column carries the filament card too. Without it the options column is
    // short enough that even a broken layout fits on screen and the
    // buttons-pinned contract below has nothing to catch.
    ScopedAmsBackend ams(4);
    const std::vector<std::string> palette{"#FF0000", "#00FF00"};
    const std::vector<std::string> materials{"PLA", "PETG"};

    const Canvas canvases[] = {{800, 480}, {480, 320}, {480, 272}, {480, 800}, {272, 480}};
    for (const Canvas& c : canvases) {
        INFO(c.w << "x" << c.h);
        ScopedResolution res(lv_display_get_default(), c.w, c.h);
        theme_manager_refresh_layout_constants(lv_display_get_default());

        helix::ui::PrintSelectDetailView view;
        lv_obj_t* const root =
            build_and_show_detail(*this, view, ps, test_screen(), palette, materials);
        CloseOnExit closer{view};

        // The filament card is on screen: the content weight the geometry
        // below is measured against.
        CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "filament_mapping_visible")) == 1);

        lv_obj_t* const container = lv_obj_find_by_name(root, "pre_print_options_container");
        REQUIRE(container != nullptr);
        const uint32_t tile_count = lv_obj_get_child_count(container);
        INFO("tile_count " << tile_count);
        REQUIRE(tile_count == 4);

        // Delete and Print are pinned outside the scroll area: fully on
        // screen at every canvas, whatever the options column holds.
        for (const char* name : {"print_button", "delete_button"}) {
            INFO(name);
            lv_obj_t* const btn = lv_obj_find_by_name(root, name);
            REQUIRE(btn != nullptr);
            const Rect r = coords_of(btn);
            CHECK(r.x1 >= 0);
            CHECK(r.y1 >= 0);
            CHECK(r.x2 < c.w);
            CHECK(r.y2 < c.h);
        }

        // The metadata strip compacts in portrait at every size and at micro
        // in both orientations: rows 2 and 3 and the history wrap fold away
        // so the preview and the options own the column. Landscape above
        // micro keeps the full strip. Read the same subjects the XML does.
        {
            lv_subject_t* const bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
            lv_subject_t* const portrait_sub = lv_xml_get_subject(nullptr, "ui_is_portrait");
            REQUIRE(bp != nullptr);
            REQUIRE(portrait_sub != nullptr);
            const bool compact =
                lv_subject_get_int(bp) == 0 || lv_subject_get_int(portrait_sub) == 1;
            INFO("compact " << compact);
            for (const char* name : {"metadata_row_2", "metadata_row_3", "detail_history_wrap"}) {
                INFO(name);
                lv_obj_t* const row = lv_obj_find_by_name(root, name);
                REQUIRE(row != nullptr);
                if (compact) {
                    CHECK(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
                } else {
                    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
                }
            }
        }

        // Labels wrap inside their tile; the check tab never covers one.
        for (uint32_t i = 0; i < tile_count; ++i) {
            INFO("tile " << i);
            lv_obj_t* const tile = lv_obj_get_child(container, i);
            const Rect tile_r = coords_of(tile);
            lv_obj_t* const label = lv_obj_find_by_name(tile, "label");
            lv_obj_t* const tab = lv_obj_find_by_name(tile, "check_tab");
            REQUIRE(label != nullptr);
            REQUIRE(tab != nullptr);
            CHECK(rect_contains(tile_r, coords_of(label)));
            CHECK_FALSE(rect_intersects(coords_of(tab), coords_of(label)));
        }

        // The cue reports the scroll area it belongs to, no more, no less.
        lv_obj_t* const scroll = lv_obj_find_by_name(root, "detail_options_scroll");
        REQUIRE(scroll != nullptr);
        lv_subject_t* const more = lv_xml_get_subject(nullptr, "detail_options_more_below");
        REQUIRE(more != nullptr);
        const int scroll_bottom = lv_obj_get_scroll_bottom(scroll);
        const int expected = scroll_bottom > 0 ? 1 : 0;
        INFO("scroll_bottom " << scroll_bottom);
        CHECK(lv_subject_get_int(more) == expected);
        // The overflow case the cue exists for must actually occur: at the
        // smallest landscape canvas the column outgrows the screen, so the
        // checked state above is reached, not just the unchecked one.
        if (c.w == 480 && c.h == 272) {
            CHECK(scroll_bottom > 0);
        }

        // Portrait: the measured preview card height trades itself for option
        // rows. Where the width/3 floor did NOT clamp the card, row 1 must be
        // fully inside the scroll area's visible bottom edge.
        lv_subject_t* const portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
        REQUIRE(portrait != nullptr);
        if (lv_subject_get_int(portrait) == 1) {
            lv_obj_t* const card = lv_obj_find_by_name(root, "detail_card");
            REQUIRE(card != nullptr);
            const int card_h = lv_obj_get_height(card);
            const int card_w = lv_obj_get_width(card);
            INFO("portrait card " << card_w << "x" << card_h << " floor " << card_w / 3);
            if (card_h > card_w / 3) {
                const Rect sr = coords_of(scroll);
                const int32_t visible_bottom = sr.y2 -
                                               lv_obj_get_style_pad_bottom(scroll, LV_PART_MAIN) -
                                               lv_obj_get_style_border_width(scroll, LV_PART_MAIN);
                for (uint32_t i = 0; i < 2 && i < tile_count; ++i) {
                    INFO("row-1 tile " << i);
                    CHECK(coords_of(lv_obj_get_child(container, i)).y2 <= visible_bottom);
                }
            }
        }
    }
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "an empty option set hides the options card, not the Print button",
                 "[print_select][detail][geometry][print_detail_geometry]") {
    PrinterState& ps = get_printer_state();
    PrinterStateTestAccess::set_option_set(ps, PrePrintOptionSet{});
    PrinterStateTestAccess::refresh_option_visibility(ps);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "has_any_preprint_options")) == 0);

    ScopedResolution res(lv_display_get_default(), 800, 480);
    theme_manager_refresh_layout_constants(lv_display_get_default());

    helix::ui::PrintSelectDetailView view;
    lv_obj_t* const root = build_and_show_detail(*this, view, ps, test_screen());
    CloseOnExit closer{view};

    lv_obj_t* const card = lv_obj_find_by_name(root, "options_card");
    REQUIRE(card != nullptr);
    CHECK(lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));

    lv_obj_t* const print = lv_obj_find_by_name(root, "print_button");
    REQUIRE(print != nullptr);
    CHECK_FALSE(lv_obj_has_flag(print, LV_OBJ_FLAG_HIDDEN));
    const Rect r = coords_of(print);
    CHECK(r.x1 >= 0);
    CHECK(r.y1 >= 0);
    CHECK(r.x2 < 800);
    CHECK(r.y2 < 480);
}
