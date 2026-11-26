// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_ENABLE_SCREENSAVER

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/panel_widget_size_harness.h"
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
                 "a clock face frees its canvas and timer on detach and off screen, and comes back "
                 "on the next tree",
                 "[clock_widget][clock_face]") {
    PanelWidgetHarness<ClockWidget> h(test_screen(), face_config("station"),
                                      std::string("clock:8"));
    ClockWidget& w = h.widget();
    w.on_activate();
    h.resize(4, 4, 240, 240);
    REQUIRE(w.face_view().showing());

    w.on_deactivate();
    CHECK_FALSE(w.face_view().showing());
    CHECK(w.face_view().heap_bytes() == 0);
    w.on_activate();
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

#endif // HELIX_ENABLE_SCREENSAVER
