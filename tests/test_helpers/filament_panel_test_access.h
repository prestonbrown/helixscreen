// Copyright (C) 2025-2026 356C LLC
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
    /// Runs the real status-line state machine. update_status() caches which
    /// arm it last rendered so the two constant-text arms can skip a
    /// lv_translation_get() on every temperature tick; calling it directly is
    /// what lets a test drive the arm transitions deterministically.
    static void update_status(FilamentPanel& p) {
        p.update_status();
    }

    static int selected_op_slot(const FilamentPanel& p) {
        return p.selected_op_slot();
    }

    static void execute_load(FilamentPanel& p) {
        p.execute_load();
    }

    /// Stands in for a macro profile that needs a home, which no stock row does.
    static void set_home_before_load_macro(FilamentPanel& p, bool home) {
        p.home_before_load_macro_ = home;
    }

    static void execute_unload(FilamentPanel& p) {
        p.execute_unload();
    }

    static void handle_load_button(FilamentPanel& p) {
        p.handle_load_button();
    }

    static void handle_unload_button(FilamentPanel& p) {
        p.handle_unload_button();
    }
    /// The Unload handler re-entered with the user's answer to the lane question.
    static void handle_unload_button(FilamentPanel& p, int chosen_lane) {
        p.handle_unload_button(chosen_lane);
    }

    static void handle_preset_button(FilamentPanel& p, int slot) {
        p.handle_preset_button(slot);
    }

    static void confirm_custom_nozzle(FilamentPanel& p, float celsius) {
        p.handle_custom_nozzle_confirmed(celsius);
    }
    static void handle_spool_preset_button(FilamentPanel& p) {
        p.handle_spool_preset_button();
    }

    static void update_spool_preset(FilamentPanel& p) {
        p.update_spool_preset();
    }

    static void execute_purge(FilamentPanel& p) {
        p.execute_purge();
    }

    /// The Extrude / Retract / Purge buttons, preheat and all.
    static void handle_extrude_button(FilamentPanel& p) {
        p.handle_extrude_button();
    }
    static void handle_retract_button(FilamentPanel& p) {
        p.handle_retract_button();
    }
    static void handle_purge_button(FilamentPanel& p) {
        p.handle_purge_button();
    }

    /// The preset index the material buttons select (-1 = none), set without
    /// the heater commands a real preset press also sends.
    static void set_selected_material(FilamentPanel& p, int preset) {
        p.selected_material_ = preset;
    }

    static void populate_extruder_dropdown(FilamentPanel& p) {
        p.populate_extruder_dropdown();
    }

    static void handle_extruder_changed(FilamentPanel& p) {
        p.handle_extruder_changed();
    }

    static lv_obj_t* extruder_dropdown(FilamentPanel& p) {
        return p.extruder_dropdown_;
    }

    /// Raw member pointers, for asserting an orientation flip re-finds them
    /// into the rebuilt tree (compare against lv_obj_find_by_name).
    static lv_obj_t* temp_graph_card(FilamentPanel& p) {
        return p.temp_graph_card_;
    }

    static lv_obj_t* btn_temp_graph(FilamentPanel& p) {
        return p.btn_temp_graph_;
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

    // --- Per-heater keypad ceiling (#1355) -------------------------------
    // The value the three temperature keypads are handed as max_value. Reaching
    // the real method is what proves the panel asks TemperatureController
    // rather than reading its compiled-in member.

    static float keypad_max_for(::FilamentPanel& p, helix::HeaterType type, int fallback_deg) {
        return p.keypad_max_for(type, fallback_deg);
    }

    // --- Chamber heater --------------------------------------------------
    // Cool Down's default gcode and a material's chamber target both depend on
    // whether the printer has a chamber heater.

    static void handle_cooldown(::FilamentPanel& p) {
        p.handle_cooldown();
    }

    static int chamber_target(const ::FilamentPanel& p) {
        return p.chamber_target_;
    }
};

} // namespace helix::ui
