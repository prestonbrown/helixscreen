// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_slot_layout.h"

#include "ams_lane_state.h"
#include "core/observer_guard.h"
#include "lvgl/lvgl.h"

namespace helix {

/// Per-widget state of one ams_slot.
struct AmsSlotData {
    int slot_index = -1;
    int total_count = 4; // Total slots being displayed (for stagger calculation)
    helix::ui::AmsSlotHitBand hit_band;

    /// Backend whose subjects the observers below are bound to (captured at
    /// setup_slot_observers time from the active backend). Kept so later
    /// re-reads (apply_slot_label) hit the same backend the
    /// observers are bound to, even if the active backend moved on.
    int backend_index = 0;

    // Last lane classification seen from the per-slot lane_state subject. The
    // material label's text and ghost strength depend on it (an Empty lane
    // reads "Empty", a Ghosted one dims alongside its spool), and lane_state
    // and material arrive on SEPARATE subjects with no ordering guarantee - so
    // the material path reads the classification from here rather than racing
    // for it.
    helix::ui::LaneState last_lane_state = helix::ui::LaneState::Empty;

    // RAII observer handles - automatically removed when this struct is destroyed
    ObserverGuard status_observer;     ///< Per-slot status -> badge color
    ObserverGuard lane_state_observer; ///< Per-slot lane_state -> label text + ghost
    ObserverGuard material_observer;   ///< Per-slot material type label (static subject)
    ObserverGuard name_observer;       ///< Per-slot spool name line
    ObserverGuard remaining_observer;  ///< Per-slot remaining length (primary backend)
    ObserverGuard current_slot_observer;
    ObserverGuard filament_loaded_observer;
    ObserverGuard active_loaded_observer; ///< Per-slot active-loaded (single highlight source)
    ObserverGuard action_observer;
    ObserverGuard target_slot_observer;
    ObserverGuard print_state_observer;        ///< Spool spin: printing or not
    ObserverGuard extruder_velocity_observer;  ///< Spool spin: paced by flow
    ObserverGuard animations_enabled_observer; ///< Spool spin: the setting turns it off
    ObserverGuard slots_version_observer;      ///< Spool table renumber -> badge number

    // Lifetime tokens paired with the observers that bind per-backend subjects
    // (status, lane_state, material, name). Secondary-backend (index > 0) subjects
    // are DYNAMIC - recreated on backend rediscovery - so those observers need
    // tokens that expire when AmsState tears the subject down (L084). For
    // backend 0 the accessors return AmsState's subjects lifetime. Each MUST be
    // reset BEFORE its matching observer (see cleanup paths, #705).
    SubjectLifetime status_lifetime;
    SubjectLifetime lane_state_lifetime;
    SubjectLifetime material_lifetime;
    SubjectLifetime name_lifetime;

    lv_obj_t* spool_container = nullptr; // Container for the lane spool + badges

    // The lane spool graphic (spool, placeholder, error glow) renders itself
    // from the per-slot subjects; ams_slot only points it at a slot index.
    lv_obj_t* lane_spool = nullptr;

    // Other UI elements
    lv_obj_t* name_label = nullptr; // Brand, above the material
    lv_obj_t* material_label = nullptr;
    lv_obj_t* leader_line = nullptr;     // Dotted line connecting label to spool (when staggered)
    lv_point_precise_t leader_points[2]; // Points for leader line (per-slot storage)
    lv_obj_t* status_badge_bg = nullptr; // Status badge background (colored circle)
    lv_obj_t* slot_badge = nullptr;      // Slot number label inside status badge
    lv_obj_t* tool_badge_bg = nullptr;   // Tool badge background (top-left corner)
    lv_obj_t* tool_badge = nullptr;      // Tool badge label (T0, T1, etc.)
    lv_obj_t* container = nullptr;       // The ams_slot widget itself
    lv_obj_t* lane_humidity = nullptr;   // Droplet + value row, under a per-lane lid
    lv_obj_t* lane_humidity_text = nullptr; // The row's value
    lv_obj_t* lane_humidity_drop = nullptr; // The row's drop, filled to the value
    lv_obj_t* lane_temp_row = nullptr;      // Thermometer + value, above the humidity
    lv_obj_t* lane_temp_text = nullptr;
    lv_obj_t* lane_remaining = nullptr; // Measured remaining length, under the material
    bool show_lane_humidity = false;
    lv_obj_t* drying_badge = nullptr;       // Top-right drying-state badge
    lv_obj_t* drying_icons[4] = {};         // Queued, active, complete, cancelled
    lv_subject_t* drying_subject = nullptr; // Subject the badge is bound to

    // Pulsing state - when true, highlight updates are skipped to preserve animation
    bool is_pulsing = false;
};

/// The slot data registered for @p obj, nullptr when it has none.
AmsSlotData* get_slot_data(lv_obj_t* obj);

/// Re-derive this lane's spool rotation from the state it depends on.
void apply_spool_spin(AmsSlotData* data);

/// Observe what turns the spool beyond the slot's own subjects.
void observe_slot_spin(AmsSlotData* data);

} // namespace helix
