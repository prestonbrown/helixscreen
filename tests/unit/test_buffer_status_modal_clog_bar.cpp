// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_status_modal_clog_bar.cpp
 * @brief The Buffer Status modal carries the clog reading, not just its mode.
 *
 * The modal opens off the FlowGuard tile and used to show strictly less than
 * the tile that opened it: one "Clog detection" row printing "Automatic" /
 * "Manual" / "Off" with no value, no threshold and no peak. It now embeds
 * clog_bar_body, the same bar the tile draws.
 *
 * What is worth pinning is the embedding, not the geometry — clog_bar_geometry
 * is tested directly in test_clog_meter_geometry.cpp, and the widget-side
 * layout in test_widget_size_clog_detection.cpp. Here: the dialog really
 * contains the bar (a renamed component would silently embed nothing), the bar
 * is driven off the shared subjects, and it disappears on a printer with no
 * detection hardware at all, which this modal is still reachable on.
 */

#include "ui_clog_bar.h"

#include "../lvgl_ui_test_fixture.h"
#include "ams_state.h"
#include "clog_meter_geometry.h"
#include "helix-xml/src/xml/lv_xml.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

void publish(int mode, int value, int danger, int peak) {
    auto& ams = AmsState::instance();
    lv_subject_set_int(ams.get_clog_meter_mode_subject(), mode);
    lv_subject_set_int(ams.get_clog_meter_value_subject(), value);
    lv_subject_set_int(ams.get_clog_meter_danger_pct_subject(), danger);
    lv_subject_set_int(ams.get_clog_meter_peak_pct_subject(), peak);
}

/// Build the dialog the way Modal::show does, minus the stack plumbing.
lv_obj_t* build_modal(lv_obj_t* parent) {
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "buffer_status_modal", nullptr));
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "buffer_status_modal embeds the clog bar",
                 "[modals][buffer_status][clog][1017]") {
    AmsState::instance().init_subjects(true);
    publish(static_cast<int>(ui::ClogMeterMode::Flowguard), /*value=*/60, /*danger=*/80,
            /*peak=*/70);

    lv_obj_t* dialog = build_modal(test_screen());
    REQUIRE(dialog != nullptr);

    // The component resolved and brought its named objects with it. Without
    // this the modal would build fine and simply contain no bar.
    lv_obj_t* track = lv_obj_find_by_name(dialog, "clog_bar_track");
    REQUIRE(track != nullptr);

    ui::UiClogBar bar(dialog);
    REQUIRE(bar.is_valid());

    lv_obj_update_layout(dialog);
    bar.relayout();

    const int track_w = lv_obj_get_content_width(track);
    REQUIRE(track_w > 0);

    lv_obj_t* fill = lv_obj_find_by_name(track, "clog_bar_fill");
    REQUIRE(fill != nullptr);
    const auto g =
        ui::clog_bar_geometry(static_cast<int>(ui::ClogMeterMode::Flowguard), 60, 80, 70, track_w);
    // Reading the modal's own track, so this fails if the bar is embedded at a
    // width that makes it useless as well as if it is not driven at all.
    CHECK(lv_obj_get_width(fill) == g.fill_w);
    CHECK(g.fill_w > 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "buffer_status_modal hides the bar with no detection",
                 "[modals][buffer_status][clog][1017]") {
    // This is the buffer modal, not the clog modal: it is reachable on a
    // printer whose only filament sensor is the buffer. The bar has to gate on
    // clog_meter_mode rather than on the modal being open.
    AmsState::instance().init_subjects(true);
    publish(static_cast<int>(ui::ClogMeterMode::None), 0, 0, 0);

    lv_obj_t* dialog = build_modal(test_screen());
    REQUIRE(dialog != nullptr);

    lv_obj_t* bar_root = lv_obj_find_by_name(dialog, "clog_bar");
    REQUIRE(bar_root != nullptr); // built, but not shown
    lv_obj_update_layout(dialog);
    CHECK(lv_obj_is_hidden(bar_root));

    // And it comes back when a source appears, so the gate is live rather than
    // decided once at build time.
    publish(static_cast<int>(ui::ClogMeterMode::Encoder), 40, 75, 40);
    lv_obj_update_layout(dialog);
    CHECK_FALSE(lv_obj_is_hidden(bar_root));
}

TEST_CASE_METHOD(LVGLUITestFixture, "clog bar: one status glyph per severity, Off included",
                 "[modals][buffer_status][clog][motion][1823]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    publish(static_cast<int>(ui::ClogMeterMode::Motion), ui::kMotionDisarmedValue, 0, 0);

    lv_obj_t* dialog = build_modal(test_screen());
    REQUIRE(dialog != nullptr);
    struct Glyph {
        const char* name;
        ui::ClogMeterStatus status;
    };
    const Glyph glyphs[] = {{"clog_bar_status_off", ui::ClogMeterStatus::Off},
                            {"clog_bar_status_ok", ui::ClogMeterStatus::Ok},
                            {"clog_bar_status_warning", ui::ClogMeterStatus::Warning},
                            {"clog_bar_status_fault", ui::ClogMeterStatus::Fault}};
    for (const auto& shown : glyphs) {
        lv_subject_set_int(ams.get_clog_meter_status_subject(), static_cast<int>(shown.status));
        for (const auto& g : glyphs) {
            lv_obj_t* obj = lv_obj_find_by_name(dialog, g.name);
            REQUIRE(obj != nullptr);
            INFO("status " << static_cast<int>(shown.status) << ", glyph " << g.name);
            CHECK(lv_obj_is_hidden(obj) == (g.status != shown.status));
        }
    }
    lv_subject_set_int(ams.get_clog_meter_status_subject(), 0);
    publish(static_cast<int>(ui::ClogMeterMode::None), 0, 0, 0);
}
