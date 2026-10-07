// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_size_clog_detection.cpp
 * @brief clog_detection ships as a 2x1 FlowGuard bar, and the bar's pieces land
 *        where clog_bar_geometry() says (prestonbrown/helixscreen#1017).
 *
 * The widget is authored two cells wide and one tall, and draws the horizontal
 * scale from clog_bar_body.xml.
 *
 * Two things are worth pinning. The registry half — the default and minimum
 * really are 2x1 in tracks, so a fresh placement cannot come up cramped and a
 * drag cannot take it back there. And the widget half — the bar's fill, marker,
 * ticks and danger shading are positioned from the track's measured width, so a
 * layout that silently failed to find them (a renamed object in the XML) would
 * leave everything stacked at x=0 rather than erroring.
 */

#include "ui_clog_bar.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "ams_state.h"
#include "clog_detection_config_modal.h"
#include "clog_meter_geometry.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/clog_detection_widget.h"

#include <string_view>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int kCell = GridLayout::TRACKS_PER_CELL;

/// x of a named child, in track-local pixels, or -1 when it is hidden — which
/// is how the bar says "this piece has nothing to show".
int piece_x(lv_obj_t* track, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(track, name);
    REQUIRE(obj != nullptr);
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return -1;
    }
    return lv_obj_get_x(obj);
}

int piece_w(lv_obj_t* track, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(track, name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? 0 : lv_obj_get_width(obj);
}

/// Push one sample through the subjects the bar observes.
void publish(int mode, int value, int danger, int peak, int warning = 0) {
    auto set = [](const char* name, int v) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        lv_subject_set_int(s, v);
    };
    set("clog_meter_mode", mode);
    set("clog_meter_value", value);
    set("clog_meter_danger_pct", danger);
    set("clog_meter_peak_pct", peak);
    set("clog_meter_warning", warning);
}
} // namespace

TEST_CASE("clog_detection is authored two cells wide and one tall",
          "[widget_size][clog_detection][1017]") {
    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);

    // Spans are tracks; a cell is TRACKS_PER_CELL of them.
    CHECK(def->colspan == 2 * kCell);
    CHECK(def->rowspan == 1 * kCell);

    // The minimum matches the default on the width axis, so the cramped size
    // the issue was filed about is not reachable by dragging either.
    CHECK(def->effective_min_colspan() == 2 * kCell);
    CHECK(def->effective_min_rowspan() == 1 * kCell);

    // Still growable — the scale gets better with width, it does not stop
    // being useful.
    CHECK(def->effective_max_colspan() > def->colspan);
    CHECK(def->effective_max_rowspan() > def->rowspan);
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog_detection lays the bar out from the measured track",
                 "[widget_size][clog_detection][1017]") {
    PanelWidgetManager::instance().init_widget_subjects();
    // The bar reads AmsState's clog_meter_* subjects, and clog_bar_body.xml
    // binds its labels to them, so they must exist before the page is built.
    AmsState::instance().init_subjects(true);

    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<ClogDetectionWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);

    // The 2x1 default at a mid-tier cell.
    h.resize(def->colspan, def->rowspan, /*width_px=*/240, /*height_px=*/112);

    lv_obj_t* track = h.child("clog_bar_track");
    REQUIRE(track != nullptr);

    // Read the track AFTER publishing, never before: the end labels only
    // render in Flowguard, so the mode decides how much width is left for the
    // track. Measuring once up front compares a Flowguard layout against an
    // encoder-width expectation and misses by the labels.
    auto measure = [&] {
        lv_obj_update_layout(track);
        const int w = lv_obj_get_content_width(track);
        REQUIRE(w > 0);
        return w;
    };

    SECTION("a linear mode fills from the left and shades the far end") {
        publish(static_cast<int>(ui::ClogMeterMode::Encoder), /*value=*/40, /*danger=*/75,
                /*peak=*/60);
        const int track_w = measure();

        const auto g = ui::clog_bar_geometry(static_cast<int>(ui::ClogMeterMode::Encoder), 40, 75,
                                             60, track_w);
        CHECK(piece_x(track, "clog_bar_fill") == g.fill_x);
        CHECK(piece_w(track, "clog_bar_fill") == g.fill_w);
        CHECK(piece_x(track, "clog_bar_marker") == g.marker_x);
        CHECK(piece_x(track, "clog_bar_peak") == g.peak_x);

        // Only the far end is dangerous in a 0..100 mode.
        CHECK(piece_x(track, "clog_bar_danger_lo") == -1);
        CHECK(piece_w(track, "clog_bar_danger_hi") == g.danger_hi_w);

        // The threshold rule marks where that shading starts, and there is no
        // low end to mark.
        CHECK(piece_x(track, "clog_bar_threshold_hi") == g.danger_hi_x);
        CHECK(piece_x(track, "clog_bar_threshold_lo") == -1);
    }

    SECTION("Flowguard grows out from the centre and shades both ends") {
        const int mode = static_cast<int>(ui::ClogMeterMode::Flowguard);
        publish(mode, /*value=*/-45, /*danger=*/80, /*peak=*/62);
        const int track_w = measure();

        const auto g = ui::clog_bar_geometry(mode, -45, 80, 62, track_w);
        // Fill ends at the centre and runs back toward the tangle end.
        CHECK(piece_x(track, "clog_bar_fill") == g.fill_x);
        CHECK(piece_x(track, "clog_bar_fill") + piece_w(track, "clog_bar_fill") == track_w / 2);

        // Both ends shaded, by the same amount.
        CHECK(piece_w(track, "clog_bar_danger_lo") == g.danger_lo_w);
        CHECK(piece_w(track, "clog_bar_danger_hi") == g.danger_hi_w);
        CHECK(piece_w(track, "clog_bar_danger_lo") == piece_w(track, "clog_bar_danger_hi"));

        // The peak went to the side the reading is leaning toward.
        CHECK(piece_x(track, "clog_bar_peak") < track_w / 2);

        // A symmetrical mode marks both thresholds, each on the inside edge of
        // its shading — the rule has to sit inside the zone it opens, or it
        // hangs off the end of the track at an extreme threshold.
        CHECK(piece_x(track, "clog_bar_threshold_lo") ==
              g.danger_lo_x + g.danger_lo_w - ui::kClogBarTickW);
        CHECK(piece_x(track, "clog_bar_threshold_hi") == g.danger_hi_x);
    }

    SECTION("nothing to report draws no fill and no peak") {
        publish(static_cast<int>(ui::ClogMeterMode::Buffer), /*value=*/0, /*danger=*/75,
                /*peak=*/0);
        measure();

        CHECK(piece_w(track, "clog_bar_fill") == 0);
        CHECK(piece_x(track, "clog_bar_marker") == -1); // no fill to lead
        CHECK(piece_x(track, "clog_bar_peak") == -1);   // no worst case recorded
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog_detection relays out when the widget is widened",
                 "[widget_size][clog_detection][1017]") {
    // Growing the widget widens the track, and every position is a fraction of
    // it — a bar that only measured once would keep the narrow geometry.
    PanelWidgetManager::instance().init_widget_subjects();
    AmsState::instance().init_subjects(true);

    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<ClogDetectionWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);

    const int mode = static_cast<int>(ui::ClogMeterMode::Encoder);
    publish(mode, /*value=*/50, /*danger=*/75, /*peak=*/50);

    h.resize(def->colspan, def->rowspan, 240, 112);
    lv_obj_t* track = h.child("clog_bar_track");
    REQUIRE(track != nullptr);
    lv_obj_update_layout(track);
    const int narrow_track = lv_obj_get_content_width(track);
    const int narrow_fill = piece_w(track, "clog_bar_fill");

    h.resize(def->effective_max_colspan(), def->rowspan, 480, 112);
    lv_obj_update_layout(track);
    const int wide_track = lv_obj_get_content_width(track);
    const int wide_fill = piece_w(track, "clog_bar_fill");

    REQUIRE(wide_track > narrow_track);
    CHECK(wide_fill > narrow_fill);
    // Still half the track at 50%, which is what "it re-measured" means.
    CHECK(wide_fill == ui::clog_bar_geometry(mode, 50, 75, 50, wide_track).fill_w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog_detection re-measures when the mode changes the track",
                 "[widget_size][clog_detection][1017]") {
    // The end labels only render in Flowguard, so switching modes changes how
    // much width is left for the track. The mode observer relayouts against
    // the width it can see at that instant, which is still the old one — only
    // the track's own SIZE_CHANGED catches the labels appearing. Without that
    // second pass the fill keeps geometry scaled to the wrong track.
    PanelWidgetManager::instance().init_widget_subjects();
    AmsState::instance().init_subjects(true);

    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<ClogDetectionWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);
    h.resize(def->colspan, def->rowspan, 240, 112);

    lv_obj_t* track = h.child("clog_bar_track");
    REQUIRE(track != nullptr);

    const int encoder = static_cast<int>(ui::ClogMeterMode::Encoder);
    publish(encoder, /*value=*/50, /*danger=*/75, /*peak=*/50);
    lv_obj_update_layout(track);
    const int linear_track = lv_obj_get_content_width(track);

    const int flowguard = static_cast<int>(ui::ClogMeterMode::Flowguard);
    publish(flowguard, /*value=*/50, /*danger=*/80, /*peak=*/50);
    lv_obj_update_layout(track);
    const int labelled_track = lv_obj_get_content_width(track);

    // The labels really did take width, or this test proves nothing.
    REQUIRE(labelled_track < linear_track);

    // And the fill was re-measured against the narrower track, not left at the
    // wider one's geometry.
    CHECK(piece_w(track, "clog_bar_fill") ==
          ui::clog_bar_geometry(flowguard, 50, 80, 50, labelled_track).fill_w);

    // Back the other way: the labels go, the track grows, the fill follows.
    publish(encoder, /*value=*/50, /*danger=*/75, /*peak=*/50);
    lv_obj_update_layout(track);
    CHECK(lv_obj_get_content_width(track) == linear_track);
    CHECK(piece_w(track, "clog_bar_fill") ==
          ui::clog_bar_geometry(encoder, 50, 75, 50, linear_track).fill_w);
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog_detection draws the threshold over the fill",
                 "[widget_size][clog_detection][1017]") {
    // The danger shading is danger at 30% opacity and a warning fill is drawn
    // in danger too, so a reading past the threshold used to merge the two into
    // one red block — "how far past" was least readable exactly when it
    // mattered most. The rule is a separate object, added after the fill so the
    // fill cannot paint over it.
    PanelWidgetManager::instance().init_widget_subjects();
    AmsState::instance().init_subjects(true);

    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<ClogDetectionWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);
    h.resize(def->colspan, def->rowspan, 240, 112);

    lv_obj_t* track = h.child("clog_bar_track");
    REQUIRE(track != nullptr);

    // Well past the threshold, and warning set: the worst case for legibility.
    const int mode = static_cast<int>(ui::ClogMeterMode::Encoder);
    publish(mode, /*value=*/94, /*danger=*/59, /*peak=*/95);
    lv_obj_set_state(track, LV_STATE_DEFAULT, true);
    lv_obj_update_layout(track);

    const int track_w = lv_obj_get_content_width(track);
    const auto g = ui::clog_bar_geometry(mode, 94, 59, 95, track_w);

    // The fill really does run past the threshold, or this proves nothing.
    REQUIRE(g.fill_w > g.danger_hi_x);

    lv_obj_t* rule = lv_obj_find_by_name(track, "clog_bar_threshold_hi");
    lv_obj_t* fill = lv_obj_find_by_name(track, "clog_bar_fill");
    REQUIRE(rule != nullptr);
    REQUIRE(fill != nullptr);

    CHECK_FALSE(lv_obj_has_flag(rule, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_get_x(rule) == g.danger_hi_x);

    // Drawn after the fill, which is what keeps it visible through one.
    CHECK(lv_obj_get_index(rule) > lv_obj_get_index(fill));
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog_detection is the bar alone",
                 "[widget_size][clog_detection]") {
    PanelWidgetManager::instance().init_widget_subjects();
    AmsState::instance().init_subjects(true);

    PanelWidgetHarness<ClogDetectionWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);
    CHECK(h.child("filament_health_carousel") == nullptr);
    CHECK(h.child("clog_bar_track") != nullptr);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "clog_detection: a pressure sensor alone is not a clog detector",
                 "[widget_size][clog_detection]") {
    PanelWidgetManager::instance().init_widget_subjects();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);

    // The gate hides the widget when its subject reads 0 (gate_hint_if_gated),
    // so the decision is the subject plus the def's gate name.
    const auto* def = find_widget_def("clog_detection");
    REQUIRE(def != nullptr);
    REQUIRE(def->hardware_gate_subject != nullptr);
    lv_subject_t* gate = lv_xml_get_subject(nullptr, def->hardware_gate_subject);
    REQUIRE(gate != nullptr);
    CHECK(std::string_view(def->hardware_gate_subject) == "clog_meter_mode");

    AmsStateTestAccess::sync_clog_meter(ams, test::fps_units({0.71f}));
    CHECK(lv_subject_get_int(gate) == 0);
}
