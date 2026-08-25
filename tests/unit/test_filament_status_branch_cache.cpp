// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_status_branch_cache.cpp
 * @brief update_status() may skip re-rendering an arm, but never a CHANGED arm.
 *
 * update_status() runs off the chamber-temperature observer, so it fires several
 * times a second for as long as the app is up — panel on screen or not. Two of
 * its arms render a constant string, and re-rendering those costs a
 * lv_translation_get(), which is a linear scan of the whole translation table,
 * plus two imperative icon writes. It now remembers which arm it last rendered
 * and returns early when a constant arm repeats.
 *
 * The failure that buys is a stuck status line: a cache is only safe if every
 * arm records itself. Miss one, and the next return to a constant arm early-outs
 * against a stale marker and the panel keeps showing the arm it left. The
 * interpolating arms are the easy ones to forget, because they return early and
 * never reach the bottom of the function.
 *
 * Reported shape (bundle TPVTQKBM, a Kalico Voron with min_extrude_temp: 10):
 * that printer sits in the Ready arm permanently, so the constant arm is the
 * steady state rather than a transient — 10,103 of the 13,310 lines the bundle
 * shipped were the one warning this lookup emitted.
 */

#include "ui_panel_filament.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/filament_panel_test_access.h"
#include "ams_state.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"
#include "tool_state.h"

#include <lvgl.h>
#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using TA = helix::ui::FilamentPanelTestAccess;

namespace {

/// A FilamentPanel built from real XML, which is what supplies status_icon_ and
/// the status label. Constructing the panel without it would exercise the arm
/// logic against widgets that do not exist.
struct StatusHarness {
    explicit StatusHarness(LVGLUITestFixture& f) : fx(f) {
        // The panel wires observers on ToolState + AmsState in its ctor.
        ToolState::instance().init_subjects(true);
        AmsState::instance().init_subjects(true);

        panel = std::make_unique<FilamentPanel>(fx.state(), fx.api());
        panel->init_subjects();

        root = static_cast<lv_obj_t*>(lv_xml_create(fx.test_screen(), "filament_panel", nullptr));
        REQUIRE(root != nullptr);
        panel->setup(root, fx.test_screen());
        fx.process_lvgl(30);
    }

    /// Drive the nozzle through PrinterState, the way Moonraker does, so the
    /// panel's own temperature observers set the members update_status() reads.
    void set_nozzle(double current, double target) {
        nlohmann::json status = {{"extruder", {{"temperature", current}, {"target", target}}}};
        fx.state().update_from_status(status);
        helix::ui::UpdateQueue::instance().drain();
        fx.process_lvgl(10);
    }

    /// What a user would actually be reading off the status line.
    std::string status_text() const {
        lv_subject_t* s = lv_xml_get_subject(nullptr, "filament_status");
        REQUIRE(s != nullptr);
        const char* v = lv_subject_get_string(s);
        return v ? v : "";
    }

    LVGLUITestFixture& fx;
    std::unique_ptr<FilamentPanel> panel;
    lv_obj_t* root = nullptr;
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "FilamentPanel: a repeated status arm is not re-rendered",
                 "[filament][status][panel]") {
    StatusHarness h(*this);

    // Hot and idle: the Ready arm, whose text is constant.
    h.set_nozzle(210.0, 0.0);
    TA::update_status(*h.panel);
    const std::string ready = h.status_text();
    REQUIRE_FALSE(ready.empty());

    // The steady state: temperatures keep ticking, the arm does not change.
    // Whatever the early-out skips, it must not disturb what is on screen.
    for (int i = 0; i < 5; ++i) {
        TA::update_status(*h.panel);
    }
    CHECK(h.status_text() == ready);
}

TEST_CASE_METHOD(LVGLUITestFixture, "FilamentPanel: returning to a cached arm still repaints",
                 "[filament][status][panel]") {
    StatusHarness h(*this);

    // Ready -> the constant arm records itself.
    h.set_nozzle(210.0, 0.0);
    TA::update_status(*h.panel);
    const std::string ready = h.status_text();
    REQUIRE_FALSE(ready.empty());

    // Heating: an interpolating arm, which returns early from the middle of the
    // function. If it does not record itself, the marker still reads "Ready".
    h.set_nozzle(40.0, 210.0);
    TA::update_status(*h.panel);
    const std::string heating = h.status_text();
    REQUIRE(heating != ready);

    // Back to Ready. This is the assertion the cache can break: with a stale
    // marker the early-out fires and the panel is stranded on the heating text
    // for the rest of the session.
    h.set_nozzle(210.0, 0.0);
    TA::update_status(*h.panel);
    CHECK(h.status_text() == ready);
}
