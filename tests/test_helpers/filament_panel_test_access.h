// tests/test_helpers/filament_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_filament.h"

#include <lvgl.h>

namespace helix::ui {

// Test-only access to FilamentPanel's private op-slot resolution and the Load /
// Unload executors. Lets an integration test drive the ACTUAL production methods
// (never a mirror) and observe which slot argument reaches the AMS backend, so
// the single-source-of-truth fix (execute_load/execute_unload act on the
// dropdown-selected slot, not a divergent current_slot) is regression-guarded.
struct FilamentPanelTestAccess {
    static int selected_op_slot(const FilamentPanel& p) {
        return p.selected_op_slot();
    }

    static void execute_load(FilamentPanel& p) {
        p.execute_load();
    }

    static void execute_unload(FilamentPanel& p) {
        p.execute_unload();
    }

    static void handle_load_button(FilamentPanel& p) {
        p.handle_load_button();
    }

    static void populate_extruder_dropdown(FilamentPanel& p) {
        p.populate_extruder_dropdown();
    }

    static void handle_extruder_changed(FilamentPanel& p) {
        p.handle_extruder_changed();
    }

    static void restore_heater_after_preheat(FilamentPanel& p) {
        p.restore_heater_after_preheat();
    }

    static lv_obj_t* extruder_dropdown(FilamentPanel& p) {
        return p.extruder_dropdown_;
    }

    // --- Operation timeout / on-button op state (#1183) ---------------------
    // The op-state subjects are the ones filament_panel.xml binds to each op
    // button's bind_op_state: 0 = idle, 1 = busy/spinner, 2 = done/checkmark.

    static void execute_extrude(FilamentPanel& p) {
        p.execute_extrude();
    }

    /// The guard's armed one-shot timer, so a test can fire the real timeout
    /// handler installed by the real callsite without waiting out 120s.
    static lv_timer_t* operation_timer(FilamentPanel& p) {
        return p.operation_guard_.pending_timer();
    }

    static bool operation_active(const FilamentPanel& p) {
        return p.operation_guard_.is_active();
    }

    static int op_load_state(FilamentPanel& p) {
        return lv_subject_get_int(&p.op_load_state_subject_);
    }

    static int op_extrude_state(FilamentPanel& p) {
        return lv_subject_get_int(&p.op_extrude_state_subject_);
    }

    static bool backend_op_active(const FilamentPanel& p) {
        return p.backend_op_active_;
    }

    static bool op_in_flight(const FilamentPanel& p) {
        return p.op_in_flight_.has_value();
    }

    // --- Unknown-command abort -------------------------------------------
    // op_succeeded() is the callback a macro's `ok` lands on. Reaching it
    // directly is the only way to reproduce the ordering that matters: Klipper
    // reports the abort DURING the script, Moonraker acknowledges the script
    // afterwards.

    static void op_succeeded_extrude(FilamentPanel& p) {
        p.op_succeeded(FilamentPanel::FilamentOp::Extrude);
    }
};

} // namespace helix::ui
