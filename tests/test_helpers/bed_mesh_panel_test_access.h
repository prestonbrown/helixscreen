// tests/test_helpers/bed_mesh_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_bed_mesh.h"

namespace helix {
namespace ui {

// Test-only access to BedMeshPanel's private canvas pointer.
//
// Used by test_bed_mesh_canvas_wiring.cpp to reach wire_canvas_and_content()
// and the cached canvas_ pointer directly. That is the fix for the canvas_
// use-after-free: bed_mesh_panel.xml's reactive <if> teardown condemns the old
// overlay_content/bed_mesh_canvas on every ui_is_portrait flip, and nothing
// else nulled the cached raw pointer.
//
// The rewire path itself (setup_orientation_rewire_observer ->
// rewire_after_orientation_flip, which relies on LVGL notifying observers in
// registration order so ours runs after the XML's own <if> observer) has no
// automated coverage — driving a real flip needs the whole app's XML registry.
// It was verified by hand instead, over repeated live flips via `ctl set`.
struct BedMeshPanelTestAccess {
    static lv_obj_t* canvas(const BedMeshPanel& p) {
        return p.canvas_;
    }

    /// Invokes the private wiring method under test directly, without going
    /// through create() (which needs a fully XML-registered app + Moonraker).
    static bool wire(BedMeshPanel& p, lv_obj_t* overlay_content) {
        return p.wire_canvas_and_content(overlay_content);
    }

    /// Drives the private SAVE_CONFIG initiation so the expected-restart flow
    /// tests can exercise it without the panel's full XML UI.
    static void save_config(BedMeshPanel& p) {
        p.execute_save_config();
    }

    /// The SIZE_CHANGED handler wire_canvas_and_content() installs on
    /// overlay_content, so a test can assert whether it is still registered.
    /// Its user_data is the panel, so a registration outliving the panel is a
    /// use-after-free waiting for the next layout pass.
    static lv_event_cb_t content_size_changed_cb() {
        return &BedMeshPanel::on_content_size_changed;
    }
};

} // namespace ui
} // namespace helix
