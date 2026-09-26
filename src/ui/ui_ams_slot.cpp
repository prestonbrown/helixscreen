// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_slot.h"

#include "ui_ams_lane_spool.h"
#include "ui_fonts.h"
#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_types.h"
#include "data_root_resolver.h"
#include "display_numbering.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "observer_factory.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix;

// ============================================================================
// Per-widget user data (managed via static registry for safe shutdown)
// ============================================================================

/**
 * @brief User data stored on each ams_slot widget
 *
 * Contains the slot index and observer handles. Managed via static registry
 * rather than lv_obj user_data to ensure safe cleanup during lv_deinit().
 */
struct AmsSlotData {
    int slot_index = -1;
    int total_count = 4; // Total slots being displayed (for stagger calculation)

    // Last lane classification seen from the per-slot lane_state subject. The
    // material label's text and ghost strength depend on it (an Empty lane
    // reads "Empty", a Ghosted one dims alongside its spool), and lane_state
    // and material arrive on SEPARATE subjects with no ordering guarantee — so
    // the material path reads the classification from here rather than racing
    // for it.
    helix::ui::LaneState last_lane_state = helix::ui::LaneState::Empty;

    // RAII observer handles - automatically removed when this struct is destroyed
    ObserverGuard status_observer;     ///< Per-slot status -> badge color
    ObserverGuard lane_state_observer; ///< Per-slot lane_state -> label text + ghost
    ObserverGuard material_observer;   ///< Per-slot material type label (static subject)
    ObserverGuard current_slot_observer;
    ObserverGuard filament_loaded_observer;
    ObserverGuard active_loaded_observer; ///< Per-slot active-loaded (single highlight source)
    ObserverGuard action_observer;
    ObserverGuard target_slot_observer;

    // Lifetime token paired with the status observer, the one observer left
    // here that binds a per-backend subject. Secondary-backend (index > 0)
    // status subjects are DYNAMIC —
    // recreated on backend rediscovery — so that observer needs a token
    // that expires when AmsState tears the subject down (L084). For backend 0
    // the accessor returns an empty (always-alive) token; harmless. MUST be
    // reset BEFORE the matching observer (see cleanup paths, #705).
    SubjectLifetime status_lifetime;
    SubjectLifetime lane_state_lifetime;

    lv_obj_t* spool_container = nullptr; // Container for the lane spool + badges

    // The lane spool graphic (spool, placeholder, error dot) renders itself
    // from the per-slot subjects; ams_slot only points it at a slot index.
    lv_obj_t* lane_spool = nullptr;

    // Other UI elements
    lv_obj_t* material_label = nullptr;
    lv_obj_t* leader_line = nullptr;     // Dotted line connecting label to spool (when staggered)
    lv_point_precise_t leader_points[2]; // Points for leader line (per-slot storage)
    lv_obj_t* status_badge_bg = nullptr; // Status badge background (colored circle)
    lv_obj_t* slot_badge = nullptr;      // Slot number label inside status badge
    lv_obj_t* tool_badge_bg = nullptr;   // Tool badge background (top-left corner)
    lv_obj_t* tool_badge = nullptr;      // Tool badge label (T0, T1, etc.)
    lv_obj_t* container = nullptr;       // The ams_slot widget itself

    // Pulsing state - when true, highlight updates are skipped to preserve animation
    bool is_pulsing = false;
};

// Static registry mapping lv_obj_t* -> AmsSlotData*
// Used for safe cleanup during lv_deinit() when user_data may be unreliable
static std::unordered_map<lv_obj_t*, AmsSlotData*> s_slot_registry;

/**
 * @brief Get AmsSlotData for an object from the registry
 */
static AmsSlotData* get_slot_data(lv_obj_t* obj) {
    auto it = s_slot_registry.find(obj);
    return (it != s_slot_registry.end()) ? it->second : nullptr;
}

/**
 * @brief Register slot data in the registry
 */
static void register_slot_data(lv_obj_t* obj, AmsSlotData* data) {
    s_slot_registry[obj] = data;
}

/**
 * @brief Unregister and cleanup slot data
 */
static void unregister_slot_data(lv_obj_t* obj) {
    auto it = s_slot_registry.find(obj);
    if (it != s_slot_registry.end()) {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        std::unique_ptr<AmsSlotData> data(it->second);
        if (data) {
            // Use reset() to properly unsubscribe from subjects (which are alive
            // during normal widget deletion). This frees the LambdaObserverContext,
            // expiring weak_alive tokens so deferred callbacks in the UpdateQueue
            // are safely skipped. Using release() here caused use-after-free:
            // zombie observers would fire on subject changes, queue callbacks with
            // stale widget pointers, and crash in apply_status_badge (#579).
            // Note: cleanup_all_slot_data() uses release() for pre-deinit when
            // subjects may already be destroyed — that path is correct.
            //
            // Reset the dynamic-subject lifetime BEFORE its observer so the
            // observer's weak_ptr is already expired (secondary-backend subjects
            // are dynamic; wrong order = remove on a freed subject, #705).
            data->status_lifetime.reset();
            data->lane_state_lifetime.reset();
            data->status_observer.reset();
            data->lane_state_observer.reset();
            data->current_slot_observer.reset();
            data->filament_loaded_observer.reset();
            data->active_loaded_observer.reset();
            data->action_observer.reset();
            data->target_slot_observer.reset();
        }
        s_slot_registry.erase(it);
    }
}

/**
 * @brief Pre-deinit cleanup: release all slot data while widgets are still alive.
 *
 * Called via StaticSubjectRegistry BEFORE lv_deinit(). Releases ObserverGuards
 * while global subjects are still valid. After this, the DELETE events fired
 * during lv_deinit() find nothing in the registry and are no-ops.
 */
static void cleanup_all_slot_data() {
    for (auto& [obj, data] : s_slot_registry) {
        if (!data)
            continue;

        // Release ObserverGuards while global subjects are still alive. Reset the
        // dynamic-subject lifetime first (same #705 ordering as above).
        data->status_lifetime.reset();
        data->lane_state_lifetime.reset();
        data->status_observer.release();
        data->lane_state_observer.release();
        data->current_slot_observer.release();
        data->filament_loaded_observer.release();
        data->active_loaded_observer.release();
        data->action_observer.release();
        data->target_slot_observer.release();

        delete data;
    }
    s_slot_registry.clear();
    spdlog::debug("[AmsSlot] Pre-deinit cleanup: all slot data released");
}

// ============================================================================
// Material Label
// ============================================================================

/**
 * @brief Apply the material-type label from the per-slot material subject.
 *
 * The widget owns its own material rendering so every ams_slot consumer —
 * AmsPanel, AmsOverviewPanel, AmsDetail — repaints on a material-only change
 * without any container re-reading it imperatively (#1065). The text itself
 * comes from the spool family's shared rule, helix::ui::lane_material_text(),
 * keyed on the same lane_state subject that drives the embedded spool widget
 * so the label and the graphic cannot reach opposite conclusions about one
 * lane. What is ams_slot's own: long names truncate to 4 chars when 5+ slots
 * share a row (overlap guard; the mini strip ellipsizes instead).
 */
static void apply_material_label(AmsSlotData* data, const char* material) {
    if (!data || !data->material_label)
        return;
    const char* text = helix::ui::lane_material_text(data->last_lane_state, material);
    if (data->last_lane_state != helix::ui::LaneState::Empty && data->total_count > 4 &&
        std::strlen(text) > 4) {
        std::string truncated(text, 4);
        lv_label_set_text(data->material_label, truncated.c_str());
        return;
    }
    lv_label_set_text(data->material_label, text);
}

/// Re-apply the material label from the live per-slot material subject.
///
/// Used by apply_lane_state(), whose outcome changes what the label should
/// read. Goes through apply_material_label() rather than writing text itself,
/// so the rule above has exactly one implementation.
static void refresh_slot_material_label(AmsSlotData* data) {
    if (!data || !data->material_label)
        return;
    lv_subject_t* material_subject =
        AmsState::instance().get_slot_material_subject(data->slot_index);
    apply_material_label(data,
                         material_subject ? lv_subject_get_string(material_subject) : nullptr);
}

// ============================================================================

// ============================================================================
// Observer Callbacks
// ============================================================================

// Helper functions for observer logic (called by lambdas and initial triggers)

/**
 * @brief Color the status badge from the per-slot status subject.
 *
 * The badge itself always shows (every physical gate stays numbered/visible);
 * only its color tracks the status. Spool visibility/ghosting is NOT handled
 * here - the embedded ams_lane_spool renders that from the lane_state subject.
 */
static void apply_status_badge(AmsSlotData* data, int status_int) {
    if (!data)
        return;
    auto status = static_cast<SlotStatus>(status_int);
    if (!data->status_badge_bg)
        return;

    lv_color_t badge_bg = theme_manager_get_color("ams_badge_bg");
    switch (status) {
    case SlotStatus::AVAILABLE:
    case SlotStatus::LOADED:
    case SlotStatus::FROM_BUFFER:
        badge_bg = theme_manager_get_color("success");
        break;
    case SlotStatus::BLOCKED:
        badge_bg = theme_manager_get_color("danger");
        break;
    case SlotStatus::EMPTY:
        // Gray for empty, but the badge stays so all physical gates are visible
        badge_bg = theme_manager_get_color("ams_badge_bg");
        break;
    case SlotStatus::UNKNOWN:
    default:
        badge_bg = theme_manager_get_color("ams_badge_bg");
        break;
    }
    lv_obj_remove_flag(data->status_badge_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(data->status_badge_bg, badge_bg, LV_PART_MAIN);

    // Status fills are accents, so the label starts from the palette text colour and
    // shifts toward its pole as 4:1 needs
    if (data->slot_badge) {
        lv_color_t text_color =
            theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), badge_bg);
        lv_obj_set_style_text_color(data->slot_badge, text_color, LV_PART_MAIN);
    }
    spdlog::trace("[AmsSlot] Slot {} status={} badge color applied", data->slot_index,
                  slot_status_to_string(status));
}

/**
 * @brief Apply the lane classification: label text + label ghost strength.
 *
 * The spool graphic's own presentation (hidden vs ghosted vs full) lives in
 * the embedded ams_lane_spool, driven by the same lane_state subject. What the
 * label needs on top of that: an Empty lane reads "Empty" at full strength, a
 * Ghosted lane dims its retained material in lockstep with the spool (#1065/
 * #1071 - the dimming says "assigned, not present").
 */
static void apply_lane_state(AmsSlotData* data, int state_int) {
    if (!data)
        return;
    data->last_lane_state = static_cast<helix::ui::LaneState>(state_int);
    if (data->material_label) {
        lv_obj_set_style_text_opa(data->material_label,
                                  data->last_lane_state == helix::ui::LaneState::Ghosted
                                      ? ams_draw::GHOST_OPA
                                      : LV_OPA_COVER,
                                  LV_PART_MAIN);
    }
    // The label's TEXT depends on the classification just recorded, so
    // re-derive it from the one rule now that last_lane_state is current.
    refresh_slot_material_label(data);
}

/**

/**
 * @brief Apply current slot highlight logic
 *
 * Active slots get a glowing border effect using shadows for visual emphasis.
 * Used by both current_slot and filament_loaded observers.
 */
static void apply_current_slot_highlight(AmsSlotData* data, int current_slot) {
    if (!data || !data->container) {
        return;
    }

    // Skip highlight updates while pulsing - animation takes precedence
    if (data->is_pulsing) {
        spdlog::trace("[AmsSlot] Slot {} skipping highlight update (pulsing)", data->slot_index);
        return;
    }

    // SINGLE SOURCE OF TRUTH for the active-lane highlight: the per-slot
    // active-loaded subject (AmsBackend::slot_is_actively_loaded(i), updated live
    // on every status sync). Previously this read current_slot + the aggregate
    // filament_loaded subject separately, which diverged on unload — the badge
    // stayed lit after an idle unload while the top-right correctly cleared.
    // Observing the per-slot subject keeps both in lockstep.
    lv_subject_t* active_loaded_subject =
        AmsState::instance().get_slot_active_loaded_subject(data->slot_index);
    bool is_active =
        active_loaded_subject ? (lv_subject_get_int(active_loaded_subject) != 0) : false;
    (void)current_slot; // retained for the pulse/observer signature only

    // Apply highlight to spool_container (not container) so it doesn't include label padding area
    lv_obj_t* highlight_target = data->spool_container ? data->spool_container : data->container;

    if (is_active) {
        // Active slot: glowing border effect
        lv_color_t primary = theme_manager_get_color("primary");

        // Border highlight on spool area only
        lv_obj_set_style_border_color(highlight_target, primary, LV_PART_MAIN);
        lv_obj_set_style_border_opa(highlight_target, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(highlight_target, 3, LV_PART_MAIN);

        // Outer glow using shadow
        lv_obj_set_style_shadow_width(highlight_target, 16, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(highlight_target, primary, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(highlight_target, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(highlight_target, 2, LV_PART_MAIN);
    } else {
        // Inactive: no border or glow
        lv_obj_set_style_border_opa(highlight_target, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(highlight_target, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(highlight_target, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(highlight_target, LV_OPA_TRANSP, LV_PART_MAIN);
    }

    spdlog::debug("[AmsSlot] Slot {} highlight active={} (from slot_active_loaded subject)",
                  data->slot_index, is_active);
}

// Forward declaration (defined below in Animation section)
void ui_ams_slot_set_pulsing(lv_obj_t* obj, bool pulsing);

/**
 * @brief Evaluate whether this slot should be pulsing based on ams_action and current_slot.
 *
 * Called by both the action and current_slot observers. Automatically starts/stops
 * the pulse animation so any panel using ams_slot widgets gets consistent feedback
 * during filament operations.
 */
static void evaluate_pulse_state(AmsSlotData* data) {
    if (!data || !data->container) {
        return;
    }

    lv_subject_t* action_subject = AmsState::instance().get_ams_action_subject();
    lv_subject_t* slot_subject = AmsState::instance().get_current_slot_subject();
    lv_subject_t* target_subject = AmsState::instance().get_pending_target_slot_subject();
    if (!action_subject || !slot_subject) {
        return;
    }

    auto action = static_cast<AmsAction>(lv_subject_get_int(action_subject));
    int current_slot = lv_subject_get_int(slot_subject);
    int target_slot = target_subject ? lv_subject_get_int(target_subject) : -1;

    // The "is anything happening" question, defined once in ams_types.h. Broader
    // than what the step bar follows (AmsBackend::action_tracks_step_operation)
    // and deliberately backend-independent: a pulse on a slot that turns out not
    // to move is harmless.
    const bool is_active_operation = ams_action_is_busy(action);

    // Pulse the current slot during operations, AND the target slot during swaps
    // (so the user can see which slot filament is being loaded into)
    bool is_current = (current_slot == data->slot_index);
    bool is_target = (target_slot >= 0 && target_slot == data->slot_index);
    bool should_pulse = is_active_operation && (is_current || is_target);

    if (should_pulse && !data->is_pulsing) {
        if (!DisplaySettingsManager::instance().get_animations_enabled()) {
            return; // Static highlight will handle it
        }
        ui_ams_slot_set_pulsing(data->container, true);
    } else if (!should_pulse && data->is_pulsing) {
        ui_ams_slot_set_pulsing(data->container, false);
    }
}

/**
 * @brief Update tool badge based on slot's mapped_tool value
 *
 * Shows "T0", "T1", etc. when a tool is mapped to this slot.
 * Hidden when mapped_tool == -1 (no tool assigned).
 */
static void apply_tool_badge(AmsSlotData* data, int mapped_tool, bool is_override) {
    if (!data || !data->tool_badge_bg) {
        return;
    }

    // Tool changers: badge is redundant with toolhead label below
    auto* backend = AmsState::instance().get_backend(0);
    if (backend && backend->should_hide_slot_tool_badge()) {
        lv_obj_add_flag(data->tool_badge_bg, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (mapped_tool >= 0) {
        // Tool is mapped - show badge with tool number
        const std::string tool_text = helix::ui::tool_label(mapped_tool);
        lv_label_set_text(data->tool_badge, tool_text.c_str());
        lv_obj_remove_flag(data->tool_badge_bg, LV_OBJ_FLAG_HIDDEN);

        // Use warning color for user overrides, muted for firmware defaults
        if (is_override) {
            lv_color_t warn_color = theme_manager_get_color("warning");
            lv_obj_set_style_bg_color(data->tool_badge_bg, warn_color, LV_PART_MAIN);
        } else {
            lv_color_t muted_color = theme_manager_get_color("text_muted");
            lv_obj_set_style_bg_color(data->tool_badge_bg, muted_color, LV_PART_MAIN);
        }

        // Both fills are accents, so the label starts from the palette text colour and
        // shifts toward its pole as 4:1 needs
        if (data->tool_badge) {
            lv_color_t bg = lv_obj_get_style_bg_color(data->tool_badge_bg, LV_PART_MAIN);
            lv_color_t text_color =
                theme_manager_get_contrast_adjusted_text(theme_manager_get_color("text"), bg);
            lv_obj_set_style_text_color(data->tool_badge, text_color, LV_PART_MAIN);
        }
        spdlog::trace("[AmsSlot] Slot {} tool badge: {} (override={})", data->slot_index, tool_text,
                      is_override);
    } else {
        // No tool mapped - hide badge
        lv_obj_add_flag(data->tool_badge_bg, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[AmsSlot] Slot {} tool badge: hidden", data->slot_index);
    }
}
// ============================================================================
// Widget Event Handler (for cleanup)
// ============================================================================

/**
 * @brief Event handler for widget lifecycle (DELETE event for cleanup)
 */
static void ams_slot_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_DELETE) {
        lv_obj_t* obj = lv_event_get_target_obj(e);
        if (!obj) {
            return;
        }

        // Use the registry for cleanup - more reliable than user_data during lv_deinit()
        unregister_slot_data(obj);
    }
}

// ============================================================================
// Widget Creation (Internal)
// ============================================================================
/**
 * @brief Setup observers for a given slot index
 * Uses observer factory pattern for type-safe lambda observers
 */
static void setup_slot_observers(AmsSlotData* data) {
    if (data->slot_index < 0 || data->slot_index >= AmsState::MAX_SLOTS) {
        spdlog::warn("[AmsSlot] Invalid slot index {}, skipping observers", data->slot_index);
        return;
    }

    using helix::ui::observe_int_sync;
    AmsState& state = AmsState::instance();

    // Get per-slot subjects. Status goes through the token'd overload: for a
    // secondary backend the subject is dynamic (recreated on rediscovery), so
    // the paired SubjectLifetime member keeps the observer from firing on a
    // freed subject. Reset the lifetime BEFORE rebinding (the accessor
    // overwrites it). lane_state is a static-array subject, like the ones the
    // embedded ams_lane_spool observes.
    int backend_idx = state.active_backend_index();
    data->status_lifetime.reset();
    lv_subject_t* status_subject =
        state.get_slot_status_subject(backend_idx, data->slot_index, data->status_lifetime);
    lv_subject_t* lane_state_subject =
        state.get_slot_lane_state_subject(backend_idx, data->slot_index, data->lane_state_lifetime);
    lv_subject_t* current_slot_subject = state.get_current_slot_subject();
    lv_subject_t* filament_loaded_subject = state.get_filament_loaded_subject();

    // Capture container (lv_obj_t*) instead of data pointer to prevent
    // use-after-free when deferred callback executes after widget deletion.
    // The registry lookup acts as a validity check. (fixes #83)
    lv_obj_t* obj = data->container;
    if (status_subject) {
        data->status_observer = observe_int_sync<lv_obj_t>(
            status_subject, obj,
            [](lv_obj_t* o, int status_int) {
                auto* d = get_slot_data(o);
                if (d)
                    apply_status_badge(d, status_int);
            },
            data->status_lifetime);
    }
    if (lane_state_subject) {
        data->lane_state_observer = observe_int_sync<lv_obj_t>(
            lane_state_subject, obj,
            [](lv_obj_t* o, int state_int) {
                auto* d = get_slot_data(o);
                if (d)
                    apply_lane_state(d, state_int);
            },
            backend_idx == 0 ? state.get_subjects_lifetime() : data->lane_state_lifetime);
    }
    // Per-slot material observer: the STRUCTURAL fix for material, mirroring
    // fill. The ams_slot widget owns its material label, so a material-only
    // change (type edited while color is unchanged) repaints on EVERY consumer
    // — AmsPanel, AmsOverviewPanel, AmsDetail — with no container re-reading it
    // imperatively (#1065, native ZMOD AD5X "material stuck, color updates").
    lv_subject_t* material_subject = state.get_slot_material_subject(data->slot_index);
    if (material_subject) {
        data->material_observer = helix::ui::observe_string<lv_obj_t>(
            material_subject, obj,
            [](lv_obj_t* o, const char* mat) {
                auto* d = get_slot_data(o);
                if (!d)
                    return;
                apply_material_label(d, mat);
                // No spool re-derive on an identity-only change: AmsState
                // recomputes the lane classification on the same update, so the
                // lane_state subject fires and the embedded ams_lane_spool
                // ghosts (or un-ghosts) itself.
            },
            state.get_subjects_lifetime());
    }

    if (current_slot_subject) {
        data->current_slot_observer = observe_int_sync<lv_obj_t>(
            current_slot_subject, obj,
            [](lv_obj_t* o, int current_slot) {
                auto* d = get_slot_data(o);
                if (d) {
                    evaluate_pulse_state(d);
                    apply_current_slot_highlight(d, current_slot);
                }
            },
            state.get_subjects_lifetime());
    }
    if (filament_loaded_subject) {
        // When filament_loaded changes, re-evaluate highlight using current_slot value
        data->filament_loaded_observer = observe_int_sync<lv_obj_t>(
            filament_loaded_subject, obj,
            [](lv_obj_t* o, int /*loaded*/) {
                auto* d = get_slot_data(o);
                if (!d)
                    return;
                lv_subject_t* slot_subject = AmsState::instance().get_current_slot_subject();
                if (slot_subject) {
                    apply_current_slot_highlight(d, lv_subject_get_int(slot_subject));
                }
            },
            state.get_subjects_lifetime());
    }

    // Per-slot active-loaded observer: the SINGLE source driving the active-lane
    // highlight. Fires the instant slot_is_actively_loaded(i) flips on a status
    // sync (e.g. an idle unload clears it), so the badge tracks live load state.
    lv_subject_t* active_loaded_subject = state.get_slot_active_loaded_subject(data->slot_index);
    if (active_loaded_subject) {
        data->active_loaded_observer = observe_int_sync<lv_obj_t>(
            active_loaded_subject, obj,
            [](lv_obj_t* o, int /*active*/) {
                auto* d = get_slot_data(o);
                if (d) {
                    evaluate_pulse_state(d);
                    apply_current_slot_highlight(d, d->slot_index);
                }
            },
            state.get_subjects_lifetime());
    }

    // Action observer: auto-pulse this slot during active filament operations
    lv_subject_t* action_subject = state.get_ams_action_subject();
    if (action_subject) {
        data->action_observer = observe_int_sync<lv_obj_t>(
            action_subject, obj,
            [](lv_obj_t* o, int /*action*/) {
                auto* d = get_slot_data(o);
                if (d)
                    evaluate_pulse_state(d);
            },
            state.get_subjects_lifetime());
    }

    // Target slot observer: re-evaluate pulse when swap target changes
    lv_subject_t* target_subject = state.get_pending_target_slot_subject();
    if (target_subject) {
        data->target_slot_observer = observe_int_sync<lv_obj_t>(
            target_subject, obj,
            [](lv_obj_t* o, int /*target*/) {
                auto* d = get_slot_data(o);
                if (d)
                    evaluate_pulse_state(d);
            },
            state.get_subjects_lifetime());
    }

    // Update slot badge with 1-based display number
    if (data->slot_badge) {
        char badge_text[16];
        snprintf(badge_text, sizeof(badge_text), "%d", helix::ui::lane_number(data->slot_index));
        lv_label_set_text(data->slot_badge, badge_text);
    }

    // Trigger initial updates from current subject values. lane_state applies
    // BEFORE material so the label rule reads a current classification on the
    // first paint.
    if (status_subject && data->status_observer) {
        apply_status_badge(data, lv_subject_get_int(status_subject));
    }
    if (lane_state_subject && data->lane_state_observer) {
        apply_lane_state(data, lv_subject_get_int(lane_state_subject));
    }
    if (current_slot_subject && data->current_slot_observer) {
        apply_current_slot_highlight(data, lv_subject_get_int(current_slot_subject));
    }
    if (material_subject && data->material_observer) {
        apply_material_label(data, lv_subject_get_string(material_subject));
    }

    // Update tool badge from backend. Material and the error dot are NOT read
    // here — material flows from the per-slot material subject via the observer
    // above, and the error dot is the embedded ams_lane_spool's, driven by the
    // has_error/severity subjects.
    AmsBackend* backend = state.get_backend();
    if (backend) {
        SlotInfo slot = backend->get_slot_info(data->slot_index);
        apply_tool_badge(data, slot.mapped_tool, slot.tool_mapping_override);
    }

    spdlog::trace("[AmsSlot] Created observers for slot {}", data->slot_index);
}

// ============================================================================
// XML Handlers
// ============================================================================

/**
 * @brief XML create handler for ams_slot
 *
 * Creates the ams_slot widget by instantiating the ams_slot_view XML component
 * and then populating it with dynamic content (spool canvas, observers).
 */
static void* ams_slot_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);

    // Create the XML-defined structure
    lv_obj_t* obj = static_cast<lv_obj_t*>(
        lv_xml_create(static_cast<lv_obj_t*>(parent), "ams_slot_view", nullptr));
    if (!obj) {
        spdlog::error(
            "[AmsSlot] Failed to create from XML - ams_slot_view component may not be registered");
        return nullptr;
    }

    // Allocate user data
    auto data_ptr = std::make_unique<AmsSlotData>();
    data_ptr->slot_index = -1; // Will be set by xml_apply when slot_index attr is parsed
    AmsSlotData* data = data_ptr.get();
    data->container = obj;

    // Find XML-created children by name
    data->material_label = lv_obj_find_by_name(obj, "material_label");
    data->spool_container = lv_obj_find_by_name(obj, "spool_container");
    data->lane_spool = lv_obj_find_by_name(obj, "lane_spool");
    data->status_badge_bg = lv_obj_find_by_name(obj, "status_badge");
    data->slot_badge = lv_obj_find_by_name(obj, "slot_badge_label");
    data->tool_badge_bg = lv_obj_find_by_name(obj, "tool_badge");
    data->tool_badge = lv_obj_find_by_name(obj, "tool_badge_label");

    // Validate required children were found
    if (!data->spool_container || !data->lane_spool) {
        spdlog::error("[AmsSlot] Failed to find spool_container/lane_spool in XML");
        return obj; // Return obj anyway so it gets cleaned up properly
    }

    // Set initial text on labels (direct imperative updates, no subject indirection)
    if (data->material_label) {
        lv_label_set_text(data->material_label, "--");
    }
    if (data->slot_badge) {
        lv_label_set_text(data->slot_badge, "?");
    }

    // Register for cleanup
    register_slot_data(obj, data_ptr.release());
    lv_obj_add_event_cb(obj, ams_slot_event_cb, LV_EVENT_DELETE, nullptr);

    // Apply responsive slot width
    int32_t space_lg = theme_manager_get_spacing("space_lg");
    int32_t slot_width = (space_lg * 5) + 10; // ~90px - fits spool + padding
    lv_obj_set_width(obj, slot_width);

    spdlog::debug("[AmsSlot] Created widget from XML");

    return obj;
}

/**
 * @brief XML apply handler for ams_slot
 */
static void ams_slot_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);

    if (!obj) {
        spdlog::error("[AmsSlot] NULL object in xml_apply");
        return;
    }

    // Apply standard lv_obj properties first
    lv_xml_obj_apply(state, attrs);

    // Get user data
    auto* data = get_slot_data(obj);
    if (!data) {
        spdlog::error("[AmsSlot] No user data in xml_apply");
        return;
    }

    // Parse custom attributes
    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "slot_index") == 0) {
            int new_index = atoi(value);
            if (new_index != data->slot_index) {
                // Clear existing observers
                data->status_lifetime.reset();
                data->lane_state_lifetime.reset();
                data->status_observer.reset();
                data->lane_state_observer.reset();
                data->material_observer.reset();
                data->current_slot_observer.reset();
                data->filament_loaded_observer.reset();

                data->slot_index = new_index;

                // Point the embedded spool widget at the lane first - it owns
                // the color/fill/error observers for the graphic.
                helix::ui::ams_lane_spool_set_index(data->lane_spool, new_index,
                                                    AmsState::instance().active_backend_index());

                // Setup new observers
                setup_slot_observers(data);

                spdlog::debug("[AmsSlot] Set slot_index={}", data->slot_index);
            }
        } else if (strcmp(name, "fill_level") == 0) {
            // Creation-time fill (test panel); a later per-slot fill subject
            // value overrides it. Applied by the embedded spool widget, which
            // clamps.
            float fill = strtof(value, nullptr);
            helix::ui::ams_lane_spool_set_fill_level(data->lane_spool, fill);
            spdlog::trace("[AmsSlot] Set fill_level={:.2f}", fill);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_ams_slot_register(void) {
    // The view embeds <ams_lane_spool>; it must be registered before the view
    // is instantiated.
    ui_ams_lane_spool_register();

    // Register the XML component first (defines the structural template)
    lv_xml_register_component_from_file(
        helix::asset_component_uri("ui_xml/ams_slot_view.xml").c_str());

    // Register the custom widget (uses the XML template + adds dynamic behavior)
    lv_xml_register_widget("ams_slot", ams_slot_xml_create, ams_slot_xml_apply);

    // Self-register cleanup — ensures slot data is released before lv_deinit()
    // so that lv_subject_deinit() can safely remove observers from live widgets
    StaticSubjectRegistry::instance().register_deinit("AmsSlotWidgets", cleanup_all_slot_data);

    spdlog::info("[AmsSlot] Registered ams_slot widget with XML system");
}

int ui_ams_slot_get_index(lv_obj_t* obj) {
    if (!obj) {
        return -1;
    }

    auto* data = get_slot_data(obj);
    if (!data) {
        return -1;
    }

    return data->slot_index;
}

void ui_ams_slot_set_index(lv_obj_t* obj, int slot_index) {
    if (!obj) {
        return;
    }

    auto* data = get_slot_data(obj);
    if (!data) {
        return;
    }

    if (slot_index == data->slot_index) {
        return; // No change
    }

    // Clear existing observers, dynamic-subject tokens first (#705)
    data->status_lifetime.reset();
    data->lane_state_lifetime.reset();
    data->status_observer.reset();
    data->lane_state_observer.reset();
    data->current_slot_observer.reset();
    data->filament_loaded_observer.reset();

    data->slot_index = slot_index;

    // Point the embedded spool widget at the lane first - it owns the
    // color/fill/error observers for the graphic.
    helix::ui::ams_lane_spool_set_index(data->lane_spool, slot_index,
                                        AmsState::instance().active_backend_index());

    // Setup new observers
    setup_slot_observers(data);
}

void ui_ams_slot_refresh(lv_obj_t* obj) {
    if (!obj) {
        return;
    }

    auto* data = get_slot_data(obj);
    if (!data || data->slot_index < 0) {
        return;
    }

    // Only update non-observer properties here. Color, fill, status, lane
    // state, current-slot highlight, material and the error dot are all driven
    // by observers (this widget's or the embedded ams_lane_spool's).
    AmsBackend* backend = AmsState::instance().get_backend();
    if (backend) {
        SlotInfo slot = backend->get_slot_info(data->slot_index);
        apply_tool_badge(data, slot.mapped_tool, slot.tool_mapping_override);
    }

    spdlog::trace("[AmsSlot] Refreshed slot {}", data->slot_index);
}

float ui_ams_slot_get_fill_level(lv_obj_t* obj) {
    if (!obj) {
        return 1.0f; // Default to full
    }

    auto* data = get_slot_data(obj);
    if (!data) {
        return 1.0f;
    }

    // The embedded spool widget owns the fill render; report what it applied.
    return helix::ui::ams_lane_spool_get_fill_level(data->lane_spool);
}

void ui_ams_slot_set_layout_info(lv_obj_t* obj, int slot_index, int total_count) {
    if (!obj) {
        return;
    }

    auto* data = get_slot_data(obj);
    if (!data) {
        return;
    }

    data->total_count = total_count;

    // Calculate stagger parameters based on total gate count
    // Pattern: Low → Medium → High → Low... (cycling)
    int stagger_rows = 1;
    if (total_count >= 7) {
        stagger_rows = 3; // Low, Medium, High
    } else if (total_count >= 5) {
        stagger_rows = 2; // Low, Medium
    }

    // Calculate which row this slot belongs to using triangle wave pattern
    // Pattern: High → Mid → Low → Mid → High → Mid → Low...
    // This creates a more balanced visual distribution of labels
    int row = 0;
    if (stagger_rows > 1) {
        int period = (stagger_rows - 1) * 2; // 4 for 3 rows, 2 for 2 rows
        int pos = slot_index % period;
        if (pos < stagger_rows) {
            // Descending: High(2) → Mid(1) → Low(0)
            row = stagger_rows - 1 - pos;
        } else {
            // Ascending: Mid(1) back up
            row = pos - stagger_rows + 1;
        }
    }

    // Get font for dynamic row height calculation
    const char* font_small_name = lv_xml_get_const(NULL, "font_small");
    const lv_font_t* font_small =
        font_small_name ? lv_xml_get_font(NULL, font_small_name) : &noto_sans_16;
    int32_t line_height = lv_font_get_line_height(font_small);

    // Row height with comfortable spacing (1.5x line height)
    int32_t row_height = (line_height * 3) / 2;

    // For staggered labels, we use absolute positioning
    // Remove label from flex flow and position it at the correct stagger row
    if (data->material_label && stagger_rows > 1) {
        int32_t total_label_height = row_height * stagger_rows;

        // Remove label from flex layout - it will be positioned absolutely
        lv_obj_add_flag(data->material_label, LV_OBJ_FLAG_IGNORE_LAYOUT);

        // Add padding to container top to make room for staggered labels
        lv_obj_set_style_pad_top(obj, total_label_height, LV_PART_MAIN);

        // IMPORTANT: lv_obj_set_pos() positions relative to CONTENT area (after padding)
        // To place label in padding area (ABOVE spool), we need NEGATIVE Y values:
        //   - pad_top creates space above content
        //   - y=0 in content coords = at the spool (wrong!)
        //   - y=-pad_top = at top of container (in padding area)
        //
        // Row 0 (closest to spool): y = -row_height (just above content/spool)
        // Row 1 (middle):           y = -2 * row_height
        // Row 2 (top):              y = -3 * row_height (at top of padding area)
        int32_t label_y = -static_cast<int32_t>((row + 1) * row_height);

        // Center label horizontally, position at stagger row
        lv_obj_set_width(data->material_label, lv_pct(100));
        lv_obj_set_style_text_align(data->material_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(data->material_label, 0, label_y);

        // Create dashed leader line connecting label to spool
        if (!data->leader_line) {
            data->leader_line = lv_line_create(obj);
            lv_obj_add_flag(data->leader_line, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_add_flag(data->leader_line, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(data->leader_line, LV_OBJ_FLAG_EVENT_BUBBLE);

            // Style: dashed line using theme color
            lv_obj_set_style_line_color(data->leader_line, theme_manager_get_color("text_muted"),
                                        LV_PART_MAIN);
            lv_obj_set_style_line_width(data->leader_line, 1, LV_PART_MAIN);
            lv_obj_set_style_line_dash_width(data->leader_line, 4, LV_PART_MAIN);
            lv_obj_set_style_line_dash_gap(data->leader_line, 3, LV_PART_MAIN);
            lv_obj_set_style_line_opa(data->leader_line, LV_OPA_70, LV_PART_MAIN);
        }

        // Ensure container allows overflow for lines in padding area
        lv_obj_add_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        // Position line from label bottom (with small gap) to spool top
        // lv_obj_align() positions relative to CONTENT area (after padding)
        int32_t label_gap = 3; // Small gap between label and line
        int32_t line_start_y = label_y + line_height + label_gap; // Negative (in content coords)
        int32_t line_end_y = 0;                                   // Spool top
        int32_t leader_length = line_end_y - line_start_y;        // Positive length

        // Set line points (relative to line object position)
        data->leader_points[0].x = 0;
        data->leader_points[0].y = 0;
        data->leader_points[1].x = 0;
        data->leader_points[1].y = leader_length;
        lv_line_set_points(data->leader_line, data->leader_points, 2);

        // Position line object at horizontal center, starting below label
        lv_obj_align(data->leader_line, LV_ALIGN_TOP_MID, 0, line_start_y);
        lv_obj_remove_flag(data->leader_line, LV_OBJ_FLAG_HIDDEN);

        spdlog::debug("[AmsSlot] Slot {} layout: row={}/{}, label_y={}, leader_len={}", slot_index,
                      row, stagger_rows, label_y, leader_length);
    } else if (data->material_label) {
        // No staggering - keep label in flex flow at default position
        lv_obj_remove_flag(data->material_label, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_pad_top(obj, theme_manager_get_spacing("space_xxs"),
                                 LV_PART_MAIN); // Original padding

        // Hide leader line if it exists
        if (data->leader_line) {
            lv_obj_add_flag(data->leader_line, LV_OBJ_FLAG_HIDDEN);
        }

        spdlog::debug("[AmsSlot] Slot {} layout: no stagger (count={})", slot_index, total_count);
    }
}

void ui_ams_slot_move_label_to_layer(lv_obj_t* obj, lv_obj_t* labels_layer, int32_t slot_center_x) {
    auto* data = get_slot_data(obj);
    if (!data || !labels_layer) {
        return;
    }

    // Only move if we have a label that's been set up for staggering
    if (!data->material_label) {
        return;
    }

    // Check if label is using staggered positioning (IGNORE_LAYOUT flag set by set_layout_info)
    if (!lv_obj_has_flag(data->material_label, LV_OBJ_FLAG_IGNORE_LAYOUT)) {
        // Not staggered - don't move
        return;
    }

    // The label was positioned with negative Y in the slot's CONTENT coordinate system.
    // Content coords start AFTER padding, so negative Y means "above content, in padding area".
    // To convert to labels_layer coords, we need:
    //   absolute_y = slot_y + slot_pad_top + label_relative_y
    // Where label_relative_y is negative.
    int32_t slot_pad_top = lv_obj_get_style_pad_top(obj, LV_PART_MAIN);
    int32_t label_relative_y = lv_obj_get_y(data->material_label); // Negative
    int32_t label_y = slot_pad_top + label_relative_y;             // e.g., 60 + (-30) = 30

    // Reparent label to labels_layer
    lv_obj_set_parent(data->material_label, labels_layer);

    // Get label width for centering
    lv_obj_update_layout(data->material_label);
    int32_t label_width = lv_obj_get_width(data->material_label);

    // Position at slot center X with converted Y
    int32_t label_x = slot_center_x - label_width / 2;
    lv_obj_set_pos(data->material_label, label_x, label_y);

    // Reparent and reposition leader line if it exists
    if (data->leader_line && !lv_obj_has_flag(data->leader_line, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_parent(data->leader_line, labels_layer);

        // CRITICAL: Clear any stored alignment from set_layout_info() which used LV_ALIGN_TOP_MID
        // After reparenting, the old alignment would reference labels_layer dimensions incorrectly
        lv_obj_set_align(data->leader_line, LV_ALIGN_DEFAULT);

        // Recalculate line position based on label position
        // Line goes from just below label to spool top (slot_pad_top in labels_layer coords)
        lv_obj_update_layout(data->material_label);
        int32_t label_height = lv_obj_get_height(data->material_label);
        int32_t label_gap = 3;
        int32_t line_start_y = label_y + label_height + label_gap;
        int32_t line_end_y = slot_pad_top; // Spool top in labels_layer coords

        // Update line points for new length
        int32_t leader_length = line_end_y - line_start_y;
        data->leader_points[0].x = 0;
        data->leader_points[0].y = 0;
        data->leader_points[1].x = 0;
        data->leader_points[1].y = leader_length;
        lv_line_set_points(data->leader_line, data->leader_points, 2);

        // Position line at slot center X using absolute positioning
        // lv_line draws from its object position, so line at x=slot_center_x draws there
        lv_obj_set_pos(data->leader_line, slot_center_x, line_start_y);

        // Restore normal line styling (dashed, subtle)
        lv_obj_set_style_line_color(data->leader_line, theme_manager_get_color("text_muted"),
                                    LV_PART_MAIN);
        lv_obj_set_style_line_width(data->leader_line, 1, LV_PART_MAIN);
        lv_obj_set_style_line_opa(data->leader_line, LV_OPA_70, LV_PART_MAIN);

        spdlog::debug("[AmsSlot] Slot {} leader: x={}, start_y={}, end_y={}, length={}",
                      data->slot_index, slot_center_x, line_start_y, line_end_y, leader_length);
    }

    spdlog::debug("[AmsSlot] Slot {} label moved to layer at x={}, y={} (pad_top={}, rel_y={})",
                  data->slot_index, label_x, label_y, slot_pad_top, label_relative_y);
}

void ui_ams_slot_detach_layers(lv_obj_t* obj) {
    auto* data = get_slot_data(obj);
    if (!data)
        return;

    // Null out pointers to widgets that were reparented to badge_layer / labels_layer.
    // These widgets will be deleted by lv_obj_clean() on those layers during rebuild,
    // BEFORE this slot's DELETE event fires and unregister_slot_data() runs.
    // Without nulling, deferred observer callbacks find non-null but dangling pointers
    // and crash in apply_status_badge() (#604).
    data->status_badge_bg = nullptr;
    data->slot_badge = nullptr;
    data->material_label = nullptr;
    data->leader_line = nullptr;
}

void ui_ams_slot_move_badge_to_layer(lv_obj_t* obj, lv_obj_t* badge_layer, int32_t slot_center_x) {
    auto* data = get_slot_data(obj);
    if (!data || !badge_layer || !data->status_badge_bg || !data->spool_container) {
        return;
    }

    // Get spool container position relative to the slot widget
    lv_obj_update_layout(data->spool_container);
    int32_t container_w = lv_obj_get_width(data->spool_container);
    int32_t container_h = lv_obj_get_height(data->spool_container);

    // Badge is at bottom_right of spool_container with translate offsets
    // Compute badge position in badge_layer coords using slot_center_x
    lv_obj_update_layout(data->status_badge_bg);
    int32_t badge_w = lv_obj_get_width(data->status_badge_bg);
    int32_t badge_h = lv_obj_get_height(data->status_badge_bg);

    // Bottom-right of spool_container, centered on slot_center_x
    // spool_container is centered in the slot, so its right edge is at slot_center_x +
    // container_w/2
    int32_t slot_pad_top = lv_obj_get_style_pad_top(obj, LV_PART_MAIN);
    int32_t label_h = data->material_label ? lv_obj_get_height(data->material_label) : 0;
    int32_t pad_row = lv_obj_get_style_pad_row(obj, LV_PART_MAIN);

    // Spool container top Y = slot padding + label height + row gap
    int32_t container_top_y = slot_pad_top + label_h + pad_row;
    int32_t badge_x = slot_center_x + container_w / 2 - badge_w - 2; // -2 from translate_x
    int32_t badge_y = container_top_y + container_h - badge_h - 2;   // -2 from translate_y

    // Reparent to badge_layer
    lv_obj_set_parent(data->status_badge_bg, badge_layer);
    lv_obj_set_align(data->status_badge_bg, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(data->status_badge_bg, badge_x, badge_y);

    spdlog::debug("[AmsSlot] Slot {} badge moved to layer at x={}, y={}", data->slot_index, badge_x,
                  badge_y);
}

// ============================================================================
// Pulse Animation for Loading Operations
// ============================================================================

/**
 * @brief Animation callback for spool border opacity pulse
 */
static void spool_border_opa_anim_cb(void* obj, int32_t value) {
    lv_obj_set_style_border_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
}

void ui_ams_slot_set_pulsing(lv_obj_t* obj, bool pulsing) {
    if (!obj) {
        return;
    }

    auto* data = get_slot_data(obj);
    if (!data || !data->spool_container) {
        return;
    }

    lv_obj_t* target = data->spool_container;

    // Always stop existing animation first
    lv_anim_delete(target, spool_border_opa_anim_cb);

    // Update pulsing flag BEFORE applying styles
    data->is_pulsing = pulsing;

    if (!pulsing) {
        // Restore to current static state (active highlight or no highlight)
        lv_subject_t* current_slot_subject = AmsState::instance().get_current_slot_subject();
        if (current_slot_subject) {
            apply_current_slot_highlight(data, lv_subject_get_int(current_slot_subject));
        }
        spdlog::debug("[AmsSlot] Slot {} pulse stopped", data->slot_index);
        return;
    }

    // Ensure border is visible for pulsing
    lv_color_t primary = theme_manager_get_color("primary");
    lv_obj_set_style_border_color(target, primary, LV_PART_MAIN);
    lv_obj_set_style_border_width(target, 3, LV_PART_MAIN);

    // Start continuous pulsing animation
    constexpr int32_t PULSE_DIM_OPA = 100;
    constexpr int32_t PULSE_BRIGHT_OPA = 255;
    constexpr uint32_t PULSE_DURATION_MS = 600;

    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, target);
    lv_anim_set_values(&pulse, PULSE_DIM_OPA, PULSE_BRIGHT_OPA);
    lv_anim_set_time(&pulse, PULSE_DURATION_MS);
    lv_anim_set_playback_time(&pulse, PULSE_DURATION_MS); // Oscillate back
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&pulse, spool_border_opa_anim_cb);
    lv_anim_start(&pulse);

    spdlog::debug("[AmsSlot] Slot {} pulse started on spool_container", data->slot_index);
}

void ui_ams_slot_clear_highlight(lv_obj_t* obj) {
    if (!obj) {
        return;
    }

    auto* data = get_slot_data(obj);
    if (!data || !data->spool_container) {
        return;
    }

    lv_obj_t* target = data->spool_container;

    // Stop any existing animation
    lv_anim_delete(target, spool_border_opa_anim_cb);

    // Set is_pulsing to block automatic highlight restoration from observers
    data->is_pulsing = true;

    // Clear the border completely
    lv_obj_set_style_border_opa(target, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(target, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(target, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(target, LV_OPA_TRANSP, LV_PART_MAIN);

    spdlog::debug("[AmsSlot] Slot {} highlight cleared", data->slot_index);
}
