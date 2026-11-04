// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_message_widget.cpp
 * @brief The display-message home widget (prestonbrown/helixscreen#1125): the pure
 * wrap/scroll/icon decision, and the bound outcome against the real fonts and XML.
 *
 * Boxes are tile sizes from a live grid: 114x54 (1x0.5), 234x54 (2x0.5), 234x113 (2x1)
 * and 474x113 (4x1) at 800x480, and 70x64 (1x1) at 480x272.
 *
 * Run with: ./build/bin/helix-tests "[display_message]"
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/display_message_layout.h"
#include "src/ui/panel_widgets/display_message_widget.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

// ---------------------------------------------------------------------------
// Pure decision
// ---------------------------------------------------------------------------

namespace {

DisplayMessageInput input(int w, int h) {
    DisplayMessageInput in;
    in.content_w = w;
    in.content_h = h;
    in.line_h = 20;
    in.icon_w = 24;
    in.icon_h = 24;
    in.gap = 8;
    in.min_text_w = 80;
    return in;
}

} // namespace

TEST_CASE("display_message layout: one line scrolls, two lines wrap", "[display_message][layout]") {
    CHECK_FALSE(decide_display_message_layout(input(200, 39)).multiline);
    CHECK(decide_display_message_layout(input(200, 40)).multiline);
    CHECK(decide_display_message_layout(input(200, 200)).multiline);
}

TEST_CASE("display_message layout: fits once one line fits", "[display_message][layout]") {
    CHECK_FALSE(decide_display_message_layout(input(200, 19)).fits);
    CHECK(decide_display_message_layout(input(200, 20)).fits);
    CHECK_FALSE(decide_display_message_layout(input(0, 40)).fits);

    DisplayMessageInput no_font = input(200, 40);
    no_font.line_h = 0;
    CHECK_FALSE(decide_display_message_layout(no_font).fits);
    CHECK_FALSE(decide_display_message_layout(no_font).multiline);
}

TEST_CASE("display_message layout: the icon gives way before the text",
          "[display_message][layout]") {
    // icon 24 + gap 8 + text 80
    CHECK_FALSE(decide_display_message_layout(input(111, 40)).show_icon);
    CHECK(decide_display_message_layout(input(112, 40)).show_icon);
    // A box shorter than the glyph drops it whatever the width.
    CHECK_FALSE(decide_display_message_layout(input(400, 23)).show_icon);
    CHECK(decide_display_message_layout(input(400, 24)).show_icon);
}

// ---------------------------------------------------------------------------
// Bound outcome
// ---------------------------------------------------------------------------

namespace {

struct DisplayMessageFixture : public LVGLUITestFixture {
    DisplayMessageFixture() {
        PanelWidgetManager::instance().init_widget_subjects();
        set_message(nullptr);
    }
    ~DisplayMessageFixture() override {
        set_message(nullptr);
    }

    void set_message(const char* text) {
        nlohmann::json msg = text ? nlohmann::json(text) : nlohmann::json(nullptr);
        state().update_from_status({{"display_status", {{"message", msg}}}});
        helix::ui::UpdateQueue::instance().drain_until_empty();
    }
};

int subject_int(const char* name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name);
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

bool hidden(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

std::string label_text(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_label_get_text(obj);
}

} // namespace

TEST_CASE("display_message widget is registered as an opt-in home widget",
          "[display_message][widget_size]") {
    init_widget_registrations();
    const PanelWidgetDef* def = find_widget_def("display_message");
    REQUIRE(def != nullptr);
    CHECK(def->factory != nullptr);
    CHECK_FALSE(def->default_enabled);
    // The verdict subjects and the live-tile owner are module-level: one tile at a time.
    CHECK_FALSE(def->multi_instance);
    // A half row is a legal status-line height.
    CHECK(def->supports_half_row);
    CHECK(def->min_rowspan == 1);
}

TEST_CASE_METHOD(DisplayMessageFixture, "display_message: no message shows the placeholder",
                 "[display_message][widget_size]") {
    PanelWidgetHarness<DisplayMessageWidget> h(test_screen());
    h.resize(2, 1, 234, 113);
    CHECK_FALSE(hidden(h.child("display_message_empty")));
    CHECK(hidden(h.child("display_message_line")));
    CHECK(hidden(h.child("display_message_wrapped")));

    set_message("");
    CHECK_FALSE(hidden(h.child("display_message_empty")));
    CHECK(hidden(h.child("display_message_empty_line")));

    set_message("Homing...");
    CHECK(hidden(h.child("display_message_empty")));
    CHECK(hidden(h.child("display_message_empty_line")));
}

TEST_CASE_METHOD(DisplayMessageFixture,
                 "display_message: the placeholder on a one-line tile at 480x272 is one line",
                 "[display_message][widget_size]") {
    helix::test::ScopedBreakpoint bp(UiBreakpoint::Micro);
    PanelWidgetHarness<DisplayMessageWidget> h(test_screen());
    // One cell by half a row at 480x272.
    h.resize(2, 1, 70, 32);
    REQUIRE(subject_int("display_message_widget_multiline") == 0);

    lv_obj_t* placeholder = h.child("display_message_empty_line");
    REQUIRE(placeholder != nullptr);
    CHECK_FALSE(hidden(placeholder));
    CHECK(hidden(h.child("display_message_empty")));
    CHECK(label_text(placeholder) == "No message");
    // Scrolls, or clips where the animations setting is off; never cut to "No mess...".
    const lv_label_long_mode_t mode = lv_label_get_long_mode(placeholder);
    CHECK((mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR || mode == LV_LABEL_LONG_MODE_CLIP));
}

TEST_CASE_METHOD(DisplayMessageFixture, "display_message: a half-row tile shows one scrolling line",
                 "[display_message][widget_size]") {
    PanelWidgetHarness<DisplayMessageWidget> h(test_screen());
    h.resize(2, 1, 234, 54);
    set_message("Heating bed to 60C, then probing the mesh");

    CHECK(subject_int("display_message_widget_multiline") == 0);
    lv_obj_t* line = h.child("display_message_line");
    CHECK_FALSE(hidden(line));
    CHECK(hidden(h.child("display_message_wrapped")));
    CHECK(hidden(h.child("display_message_empty")));
    CHECK(label_text(line) == "Heating bed to 60C, then probing the mesh");
    // Scrolls, or clips where the animations setting is off: one line either way.
    const lv_label_long_mode_t mode = lv_label_get_long_mode(line);
    CHECK((mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR || mode == LV_LABEL_LONG_MODE_CLIP));
}

TEST_CASE_METHOD(DisplayMessageFixture, "display_message: a 2x1 and a 4x1 tile wrap the message",
                 "[display_message][widget_size]") {
    PanelWidgetHarness<DisplayMessageWidget> h(test_screen());
    set_message("Heating bed to 60C, then probing the mesh");
    for (int w : {234, 474}) {
        INFO(w << "x113");
        h.resize(w / 117, 2, w, 113);
        CHECK(subject_int("display_message_widget_multiline") == 1);
        lv_obj_t* wrapped = h.child("display_message_wrapped");
        CHECK_FALSE(hidden(wrapped));
        CHECK(hidden(h.child("display_message_line")));
        CHECK(lv_label_get_long_mode(wrapped) == LV_LABEL_LONG_MODE_DOTS);
        // The wrapped text never draws past the tile.
        lv_area_t text_area;
        lv_area_t root_area;
        lv_obj_get_coords(wrapped, &text_area);
        lv_obj_get_coords(h.root(), &root_area);
        CHECK(text_area.y2 <= root_area.y2);
    }
}

TEST_CASE_METHOD(DisplayMessageFixture,
                 "display_message: the icon shows on a wide tile and gives way on a narrow one",
                 "[display_message][widget_size]") {
    PanelWidgetHarness<DisplayMessageWidget> h(test_screen());
    set_message("Homing...");
    h.resize(4, 2, 474, 113);
    CHECK(subject_int("display_message_widget_icon") == 1);
    CHECK_FALSE(hidden(h.child("display_message_icon")));

    h.resize(1, 1, 60, 113);
    CHECK(subject_int("display_message_widget_icon") == 0);
    CHECK(hidden(h.child("display_message_icon")));
}

TEST_CASE_METHOD(DisplayMessageFixture, "display_message: fits_at refuses a box without one line",
                 "[display_message][widget_size]") {
    DisplayMessageWidget widget;
    CHECK(widget.fits_at(234, 54));
    CHECK(widget.fits_at(70, 64));
    CHECK_FALSE(widget.fits_at(234, 12));
}

TEST_CASE_METHOD(DisplayMessageFixture,
                 "display_message: a transient instance does not overwrite the live tile",
                 "[display_message][widget_size]") {
    PanelWidgetHarness<DisplayMessageWidget> live(test_screen());
    live.resize(2, 1, 234, 54);
    REQUIRE(subject_int("display_message_widget_multiline") == 0);
    {
        // A rebuild's incoming instance takes the tile while it is attached...
        PanelWidgetHarness<DisplayMessageWidget> incoming(test_screen());
        incoming.resize(2, 2, 234, 113);
        CHECK(subject_int("display_message_widget_multiline") == 1);
    }
    // ...and the instance it displaced republishes its own verdict when it goes.
    CHECK(subject_int("display_message_widget_multiline") == 0);
}
