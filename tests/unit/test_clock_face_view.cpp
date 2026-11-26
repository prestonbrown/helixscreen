// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "display_sleep_listener.h"
#include "screensaver_clock.h"
#include "src/ui/panel_widgets/clock_face_picker.h"
#include "src/ui/panel_widgets/clock_face_view.h"
#include "src/ui/panel_widgets/clock_widget.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

int subject_int(const std::string& name) {
    lv_subject_t* subject = lv_xml_get_subject(nullptr, name.c_str());
    REQUIRE(subject != nullptr);
    return lv_subject_get_int(subject);
}

HarnessConfig face_config(const char* face) {
    return HarnessConfig{nlohmann::json{{"face", face}}};
}

/// The tile's content box for a `size` x `size` tile.
int32_t content_side(lv_obj_t* root, int32_t size) {
    return size - static_cast<int32_t>(lv_obj_get_style_pad_left(root, LV_PART_MAIN) +
                                       lv_obj_get_style_pad_right(root, LV_PART_MAIN));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a clock tile paints its face on a canvas of the tile, and Text where it does not "
                 "fit",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> h(test_screen(), face_config("flip"), std::string("clock:7"));
    ClockWidget& w = h.widget();
    w.on_activate();
    h.resize(4, 4, 240, 240);

    const ClockFaceView& view = w.face_view();
    REQUIRE(view.showing());
    CHECK(view.face() == ClockFace::FLIP);
    CHECK(lv_obj_get_parent(view.canvas_obj()) == h.root());
    CHECK(view.width() == content_side(h.root(), 240));
    CHECK(view.heap_bytes() > 0);
    CHECK(view.timer() != nullptr);
    CHECK(subject_int(w.face_subject_name()) == 1);
    CHECK(lv_obj_is_hidden(h.child("clock_time")));

    // Below the flip clock's minimum the tile shows the time as text, and holds no canvas.
    h.resize(2, 2, 60, 60);
    CHECK_FALSE(view.showing());
    CHECK(view.heap_bytes() == 0);
    CHECK(view.timer() == nullptr);
    CHECK(subject_int(w.face_subject_name()) == 0);
    CHECK_FALSE(lv_obj_is_hidden(h.child("clock_time")));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a hidden clock face paints nothing and keeps its canvas, and comes back without "
                 "a rebuild",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> h(test_screen(), face_config("station"),
                                      std::string("clock:8"));
    ClockWidget& w = h.widget();
    w.on_activate();
    h.resize(4, 4, 240, 240);
    REQUIRE(w.face_view().showing());
    REQUIRE(w.face_view().timer() != nullptr);
    lv_obj_t* canvas = w.face_view().canvas_obj();
    const size_t bytes = w.face_view().heap_bytes();
#if LV_COLOR_DEPTH == 32
    // The card shows through round the dial; the dial itself is opaque.
    CHECK(lv_canvas_get_px(canvas, 0, 0).alpha == LV_OPA_TRANSP);
    const int32_t mid = w.face_view().width() / 2;
    CHECK(lv_canvas_get_px(canvas, mid, mid / 3).alpha == LV_OPA_COVER);
#endif

    SECTION("off the active panel, as under a screensaver") {
        w.on_deactivate();
        CHECK(w.face_view().timer() == nullptr);
        CHECK(w.face_view().paused());
        CHECK(w.face_view().heap_bytes() == bytes);
        w.on_activate();
    }
    SECTION("while the display sleeps") {
        DisplaySleepListener::notify(true);
        CHECK(w.face_view().timer() == nullptr);
        CHECK(w.face_view().paused());
        // A sleeping display stays dark when the panel is activated under it.
        w.on_deactivate();
        w.on_activate();
        DisplaySleepListener::notify(true);
        CHECK(w.face_view().timer() == nullptr);
        DisplaySleepListener::notify(false);
    }
    // Back on screen at the same size: the same canvas, running again.
    CHECK(w.face_view().timer() != nullptr);
    CHECK(w.face_view().canvas_obj() == canvas);
    CHECK(w.face_view().heap_bytes() == bytes);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a clock face frees its canvas and timer on detach, and comes back on the next "
                 "tree",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> h(test_screen(), face_config("station"),
                                      std::string("clock:10"));
    ClockWidget& w = h.widget();
    w.on_activate();
    h.resize(4, 4, 240, 240);
    REQUIRE(w.face_view().showing());

    w.detach_tile();
    CHECK_FALSE(w.face_view().showing());
    CHECK(w.face_view().timer() == nullptr);
    CHECK(w.face_view().heap_bytes() == 0);
    process_lvgl(20);

    // A recycled instance on a new tree puts its face back at the size it was granted.
    h.create_tile(test_screen());
    REQUIRE(w.face_view().showing());
    CHECK(lv_obj_get_parent(w.face_view().canvas_obj()) == h.root());
}

TEST_CASE_METHOD(LVGLUITestFixture, "two clocks keep their own faces and size modes",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> a(test_screen(), face_config("neon"), std::string("clock:1"));
    PanelWidgetHarness<ClockWidget> b(test_screen(), face_config("nixie"), std::string("clock:2"));
    a.widget().on_activate();
    b.widget().on_activate();
    a.resize(4, 4, 240, 240);
    b.resize(4, 2, 240, 110);

    CHECK(a.widget().face_view().face() == ClockFace::NEON);
    CHECK(b.widget().face_view().face() == ClockFace::NIXIE);
    CHECK(a.widget().size_subject_name() != b.widget().size_subject_name());
    CHECK(subject_int(a.widget().size_subject_name()) !=
          subject_int(b.widget().size_subject_name()));
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the face picker previews every painted face, and a pick shows it on the tile",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> h(test_screen(), face_config("text"), std::string("clock:9"));
    ClockWidget& w = h.widget();
    w.on_activate();
    h.resize(4, 4, 240, 240);
    REQUIRE_FALSE(w.face_view().showing());

    ClockFacePicker picker;
    ClockFace picked = ClockFace::TEXT;
    picker.show_for(test_screen(), h.root(), w.face(), [&](ClockFace f) { picked = f; });
    REQUIRE(picker.is_visible());
    for (size_t i = 1; i < kClockFaceCount; ++i) {
        const auto face = static_cast<ClockFace>(i);
        CAPTURE(clock_face_id(face));
        REQUIRE(picker.preview(face) != nullptr);
        CHECK(picker.preview(face)->showing());
        CHECK(picker.preview(face)->timer() == nullptr);
    }

    lv_obj_t* row = lv_obj_find_by_name(test_screen(), "clock_face_row_5");
    REQUIRE(row != nullptr);
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    CHECK(picked == ClockFace::FLIP);
    CHECK_FALSE(picker.is_visible());
    CHECK_FALSE(picker.preview(ClockFace::FLIP)->showing());

    w.select_face(picked);
    CHECK(w.face() == ClockFace::FLIP);
    CHECK(w.face_view().face() == ClockFace::FLIP);
    process_lvgl(20);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "the face picker's previews are freed on every path that closes it",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> h(test_screen(), face_config("text"), std::string("clock:11"));
    ClockWidget& w = h.widget();
    w.on_activate();
    h.resize(4, 4, 240, 240);

    const auto all_freed = [](const ClockFacePicker& picker) {
        for (size_t i = 1; i < kClockFaceCount; ++i) {
            const ClockFaceView* view = picker.preview(static_cast<ClockFace>(i));
            if (view == nullptr || view->showing() || view->heap_bytes() != 0) {
                return false;
            }
        }
        return true;
    };

    SECTION("the backdrop") {
        ClockFacePicker picker;
        picker.show_for(test_screen(), h.root(), ClockFace::TEXT, [](ClockFace) {});
        REQUIRE_FALSE(all_freed(picker));
        lv_obj_t* backdrop = lv_obj_find_by_name(test_screen(), "clock_face_picker");
        REQUIRE(backdrop != nullptr);
        lv_obj_send_event(backdrop, LV_EVENT_CLICKED, nullptr);
        CHECK_FALSE(picker.is_visible());
        CHECK(all_freed(picker));
    }
    SECTION("a screen swap deleting the menu without hide()") {
        ClockFacePicker picker;
        picker.show_for(test_screen(), h.root(), ClockFace::TEXT, [](ClockFace) {});
        REQUIRE_FALSE(all_freed(picker));
        lv_obj_t* menu = lv_obj_find_by_name(test_screen(), "clock_face_list");
        REQUIRE(menu != nullptr);
        while (lv_obj_get_parent(menu) != test_screen()) {
            menu = lv_obj_get_parent(menu);
        }
        lv_obj_delete(menu);
        CHECK(all_freed(picker));
    }
    SECTION("the widget's teardown") {
        w.on_edit_configure();
        REQUIRE(w.face_picker().is_visible());
        REQUIRE_FALSE(all_freed(w.face_picker()));
        w.detach_tile();
        CHECK_FALSE(w.face_picker().is_visible());
        CHECK(all_freed(w.face_picker()));
    }
    process_lvgl(20);
}

#endif // HELIX_ENABLE_SCREENSAVER
