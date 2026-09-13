// Copyright (C) 2025-2026 356C LLC
// tests/test_helpers/controls_panel_test_access.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_observer_guard.h" // ObserverGuard, SubjectLifetime
#include "ui_panel_controls.h"

#include <string>
#include <utility>
#include <vector>

namespace helix::ui {

// Test-only access to ControlsPanel's private secondary-fan internals.
//
// Locks in the SubjectLifetime member-vector doctrine (L084/L077): the per-fan
// speed subjects are dynamic (freed + recreated on fan rediscovery), so each
// observer's lifetime token must live in a member vector that outlives the
// paired ObserverGuard — never a stack local. These accessors let the
// conformance test inspect that the member vectors exist, stay aligned, and
// that the retained token copies observe subject death on rediscovery.
struct ControlsPanelTestAccess {
    using SecondaryFanRow = ControlsPanel::SecondaryFanRow;

    static std::vector<SecondaryFanRow>& rows(ControlsPanel& p) {
        return p.secondary_fan_rows_;
    }

    static std::vector<ObserverGuard>& observers(ControlsPanel& p) {
        return p.secondary_fan_observers_;
    }

    static std::vector<SubjectLifetime>& lifetimes(ControlsPanel& p) {
        return p.secondary_fan_lifetimes_;
    }

    /// Seed the tracked rows directly from a list of Klipper object names.
    /// subscribe_to_secondary_fan_speeds() only reads row.object_name, so the
    /// speed_label may stay null.
    static void set_rows(ControlsPanel& p, const std::vector<std::string>& object_names) {
        p.secondary_fan_rows_.clear();
        for (const auto& name : object_names) {
            p.secondary_fan_rows_.push_back({name, nullptr});
        }
    }

    /// Invoke the private subscribe path under test.
    static void subscribe(ControlsPanel& p) {
        p.subscribe_to_secondary_fan_speeds();
    }

    /// Wire the container the real teardown/rebuild path (populate_secondary_fans)
    /// requires, so the production cleanup can be exercised end-to-end.
    static void set_fans_list(ControlsPanel& p, lv_obj_t* list) {
        p.secondary_fans_list_ = list;
    }

    /// Invoke the real production teardown + rebuild path.
    static void populate(ControlsPanel& p) {
        p.populate_secondary_fans();
    }

    // --- Secondary temperature rows -----------------------------------------
    //
    // Subject temperatures are DECIdegrees (x10). update_secondary_temp() is the
    // observer-side formatter for the overflow temperature list; these accessors
    // let a test seed a row with a real label and assert the rendered string.
    using SecondaryTempRow = ControlsPanel::SecondaryTempRow;

    static std::vector<SecondaryTempRow>& temp_rows(ControlsPanel& p) {
        return p.secondary_temp_rows_;
    }

    /// Seed one tracked temperature row bound to a caller-owned label.
    static void set_temp_rows(ControlsPanel& p,
                              const std::vector<std::pair<std::string, lv_obj_t*>>& rows) {
        p.secondary_temp_rows_.clear();
        for (const auto& [name, label] : rows) {
            p.secondary_temp_rows_.push_back({name, label});
        }
    }

    /// Invoke the private observer-side temperature formatter under test.
    static void update_temp(ControlsPanel& p, const std::string& klipper_name, int decidegrees) {
        p.update_secondary_temp(klipper_name, decidegrees);
    }

    // --- Quick actions (homing, QGL, Z-Tilt) ---------------------------------
    //
    // The seven quick-action buttons share one guarded body. These accessors let
    // a test drive that body with a dispatch it controls, so the guard contract
    // can be asserted without a printer: who gets dispatched, and when the guard
    // is released.
    using QuickActionText = ControlsPanel::QuickActionText;

    static void run_quick_action(ControlsPanel& p, uint32_t timeout_ms, const QuickActionText& text,
                                 const ControlsPanel::QuickActionDispatch& dispatch) {
        p.run_quick_action(timeout_ms, text, dispatch);
    }

    static QuickActionText homing_text(const char* started) {
        return ControlsPanel::homing_text(started);
    }

    static bool guard_active(ControlsPanel& p) {
        return p.operation_guard_.is_active();
    }

    // --- Target-edit keypad ceiling ------------------------------------------
    //
    // The three heater keypads derive their maximum from the shared
    // keypad-ceiling authority; this lets a test ask the panel the same
    // question the keypad will be shown with (#1619).
    static int target_edit_max(ControlsPanel& p, helix::HeaterType type, int fallback_deg) {
        return p.target_edit_max(type, fallback_deg);
    }
};

} // namespace helix::ui
