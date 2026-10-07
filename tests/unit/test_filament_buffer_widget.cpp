// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_buffer_widget.cpp
 * @brief The Filament Buffer home widget: its registry row, its gate, and
 *        what it shows at 1x1 and 2x1.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "ams_state.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/filament_buffer_widget.h"

#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int kCell = GridLayout::TRACKS_PER_CELL;

bool hidden(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
std::string text(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_label_get_text(obj);
}
bool shown(lv_obj_t* obj) {
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}
/// At least a pixel of clear space between the two.
bool apart(lv_obj_t* a, lv_obj_t* b) {
    lv_area_t p;
    lv_area_t q;
    lv_obj_get_coords(a, &p);
    lv_obj_get_coords(b, &q);
    return q.x1 > p.x2 + 1 || q.x2 < p.x1 - 1 || q.y1 > p.y2 + 1 || q.y2 < p.y1 - 1;
}
} // namespace

TEST_CASE("filament_buffer is one cell, growable to two wide", "[widget_size][filament_buffer]") {
    const auto* def = find_widget_def("filament_buffer");
    REQUIRE(def != nullptr);
    CHECK(def->colspan == 1 * kCell);
    CHECK(def->rowspan == 1 * kCell);
    CHECK(def->effective_min_colspan() == 1 * kCell);
    CHECK(def->effective_max_colspan() == 2 * kCell);
    CHECK(def->effective_max_rowspan() == 1 * kCell);
    REQUIRE(def->hardware_gate_subject != nullptr);
    CHECK(std::string_view(def->hardware_gate_subject) == "buffer_present");
    CHECK_FALSE(def->default_enabled);
}

TEST_CASE_METHOD(LVGLUITestFixture, "filament_buffer: what each size and reading shows",
                 "[filament_buffer]") {
    PanelWidgetManager::instance().init_widget_subjects();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    const auto* def = find_widget_def("filament_buffer");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<FilamentBufferWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);

    AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}), 0);

    SECTION("1x1: slider, number and label, no trace or target") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        CHECK_FALSE(hidden(h.child("buffer_slider_box")));
        CHECK(hidden(h.child("buffer_trace")));
        CHECK(hidden(h.child("buffer_lean")));
        CHECK(hidden(h.child("buffer_target")));
        CHECK(text(h.child("buffer_label")) == "FPS");
        CHECK(text(h.child("buffer_value_short")) == "71%");
    }

    SECTION("2x1: the trace and the lean in words") {
        h.resize(2 * kCell, def->rowspan, 240, 112);
        CHECK_FALSE(hidden(h.child("buffer_trace")));
        CHECK_FALSE(hidden(h.child("buffer_lean")));
        CHECK(text(h.child("buffer_lean")) == "Running loose");
        CHECK_FALSE(hidden(h.child("buffer_target")));
        CHECK(text(h.child("buffer_target")) == "target 50%");
    }

    SECTION("no set point: the number alone") {
        h.resize(2 * kCell, def->rowspan, 240, 112);
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}, -1.0f), 0);
        CHECK(hidden(h.child("buffer_slider_box")));
        CHECK(hidden(h.child("buffer_trace")));
        CHECK(hidden(h.child("buffer_target")));
        CHECK(hidden(h.child("buffer_label")));
        CHECK(text(h.child("buffer_value")) == "Pressure: 71%");
    }

    SECTION("1x1 with a set point: the short number is the one showing") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        CHECK_FALSE(hidden(h.child("buffer_value_short")));
        CHECK(text(h.child("buffer_value_short")) == "71%");
        CHECK(hidden(h.child("buffer_value")));
    }

    SECTION("1x1 with no set point: the short number alone") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}, -1.0f), 0);
        CHECK(hidden(h.child("buffer_slider_box")));
        CHECK_FALSE(hidden(h.child("buffer_label")));
        CHECK(text(h.child("buffer_label")) == "FPS");
        CHECK_FALSE(hidden(h.child("buffer_value_short")));
        CHECK(text(h.child("buffer_value_short")) == "71%");
        CHECK(hidden(h.child("buffer_value")));
    }

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}

TEST_CASE_METHOD(LVGLUITestFixture, "filament_buffer: nothing is drawn over or against the slider",
                 "[filament_buffer]") {
    PanelWidgetManager::instance().init_widget_subjects();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    PanelWidgetHarness<FilamentBufferWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);
    AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.32f}), 0);

    // The measured cells at 800x480 and 480x320, one and two wide.
    struct Size {
        int cols, w, h;
    };
    for (const Size size :
         {Size{1, 114, 113}, Size{1, 82, 76}, Size{2, 233, 113}, Size{2, 166, 76}}) {
        CAPTURE(size.cols, size.w, size.h);
        h.resize(size.cols * kCell, kCell, size.w, size.h);
        lv_obj_t* slider = h.child("buffer_slider_box");
        REQUIRE(shown(slider));
        CHECK(shown(h.child("buffer_trace")) == (size.cols == 2));
        for (const char* name : {"buffer_trace", "buffer_value_short", "buffer_value",
                                 "buffer_target", "buffer_label", "buffer_lean"}) {
            lv_obj_t* obj = h.child(name);
            if (obj && shown(obj)) {
                CAPTURE(name);
                CHECK(apart(slider, obj));
            }
        }
        if (size.cols == 2 && size.h == 76) {
            // The tile's padding holds on its right and bottom edges too.
            lv_area_t box;
            lv_obj_get_content_coords(h.root(), &box);
            lv_area_t trace;
            lv_area_t caption;
            lv_obj_get_coords(h.child("buffer_trace"), &trace);
            lv_obj_get_coords(h.child("buffer_caption"), &caption);
            CHECK(trace.x2 <= box.x2);
            CHECK(caption.y2 <= box.y2);
        }
    }

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}
