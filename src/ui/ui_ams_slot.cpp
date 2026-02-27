// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_slot.h"

#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_observer_guard.h"
#include "ui_spool_canvas.h"

#include "ams_state.h"
#include "ams_types.h"
#include "config.h"
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

#include <cstring>
#include <memory>
#include <unordered_map>

using namespace helix;

// ============================================================================
// Per-widget user data (managed via static registry for safe shutdown)
// ============================================================================

/**
 * @brief Check if 3D spool visualization is enabled in config
 * @return true if "3d" style, false for "flat" style
 */
static bool is_3d_spool_style() {
    Config* cfg = Config::get_instance();
    std::string style = cfg->get<std::string>("/ams/spool_style", "3d");
    return (style == "3d");
}

/**
 * @brief User data stored on each ams_slot widget
 *
 * Contains the slot index and observer handles. Managed via static registry
 * rather than lv_obj user_data to ensure safe cleanup during lv_deinit().
 */
struct AmsSlotData {
    int slot_index = -1;
    int total_count = 4;      // Total slots being displayed (for stagger calculation)
    bool use_3d_style = true; // Cached style setting

    // RAII observer handles - automatically removed when this struct is destroyed
    ObserverGuard color_observer;
    ObserverGuard status_observer;
    ObserverGuard current_slot_observer;
    ObserverGuard filament_loaded_observer;
    ObserverGuard action_observer;
    ObserverGuard target_slot_observer;

    // Skeuomorphic spool visualization layers (flat style)
    lv_obj_t* spool_container = nullptr; // Container for all spool elements
    lv_obj_t* spool_outer = nullptr;     // Outer ring (flange - darker shade)
    lv_obj_t* color_swatch = nullptr;    // Main filament color ring (flat) or spool_canvas (3D)
    lv_obj_t* spool_hub = nullptr;       // Center hub (dark) - only for flat style

    // 3D spool canvas widget (when use_3d_style is true)
    lv_obj_t* spool_canvas = nullptr;

    // Other UI elements
    lv_obj_t* material_label = nullptr;
    lv_obj_t* leader_line = nullptr;     // Dotted line connecting label to spool (when staggered)
    lv_point_precise_t leader_points[2]; // Points for leader line (per-slot storage)
    lv_obj_t* status_badge_bg = nullptr; // Status badge background (colored circle)
    lv_obj_t* slot_badge = nullptr;      // Slot number label inside status badge
    lv_obj_t* tool_badge_bg = nullptr;   // Tool badge background (top-left corner)
    lv_obj_t* tool_badge = nullptr;      // Tool badge label (T0, T1, etc.)
    lv_obj_t* container = nullptr;       // The ams_slot widget itself

    // Fill level for Spoolman integration (0.0 = empty, 1.0 = full)
    float fill_level = 1.0f;

    // Empty slot placeholder (dashed-style circle shown when no filament assigned)
    lv_obj_t* empty_placeholder = nullptr;

    // Error/health indicators (dynamic overlays on spool_container)
    lv_obj_t* error_indicator = nullptr; // Error icon badge at top-right of spool

    // Pulsing state - when true, highlight updates are skipped to preserve animation
    bool is_pulsing = false;
};

// Note: Icons are accessed via ui_icon::lookup_codepoint() from ui_icon_codepoints.h

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
        std::unique_ptr<AmsSlotData> data(it->second);
        if (data) {
            // Release ObserverGuard observers before delete to prevent destructors
            // from calling lv_observer_remove() on already-destroyed subjects
            data->color_observer.release();
            data->status_observer.release();
            data->current_slot_observer.release();
            data->filament_loaded_observer.release();
            data->action_observer.release();
            data->target_slot_observer.release();
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

        // Release ObserverGuards while global subjects are still alive
        data->color_observer.release();
        data->status_observer.release();
        data->current_slot_observer.release();
        data->filament_loaded_observer.release();
        data->action_observer.release();
        data->target_slot_observer.release();

        delete data;
    }
    s_slot_registry.clear();
    spdlog::debug("[AmsSlot] Pre-deinit cleanup: all slot data released");
}

// ============================================================================
// Fill Level Helpers
// ============================================================================

/**
 * @brief Update the filament visualization based on fill level
 *
 * Simulates remaining filament on spool:
 * - 3D style: Updates spool_canvas fill_level
 * - Flat style: Resizes concentric ring
 */
static void update_filament_ring_size(AmsSlotData* data) {
    if (!data) {
        return;
    }

    // Clamp fill level to valid range
    float fill = data->fill_level;
    if (fill < 0.0f)
        fill = 0.0f;
    if (fill > 1.0f)
        fill = 1.0f;

    if (data->use_3d_style && data->spool_canvas) {
        // 3D style: Use spool_canvas fill level
        ui_spool_canvas_set_fill_level(data->spool_canvas, fill);
        spdlog::trace("[AmsSlot] Slot {} 3D fill={:.0f}%", data->slot_index, fill * 100.0f);
    } else if (data->color_swatch && data->spool_container && data->spool_hub) {
        // Flat style: Resize the concentric ring
        lv_obj_update_layout(data->spool_container);

        int32_t spool_size = lv_obj_get_width(data->spool_container);
        int32_t hub_size = lv_obj_get_width(data->spool_hub);

        int32_t min_ring = hub_size + 4;   // Minimum: slightly larger than hub
        int32_t max_ring = spool_size - 8; // Maximum: smaller than outer flange

        int32_t ring_size = min_ring + static_cast<int32_t>((max_ring - min_ring) * fill);

        lv_obj_set_size(data->color_swatch, ring_size, ring_size);
        lv_obj_align(data->color_swatch, LV_ALIGN_CENTER, 0, 0);

        spdlog::trace("[AmsSlot] Slot {} flat fill={:.0f}% → ring_size={}px", data->slot_index,
                      fill * 100.0f, ring_size);
    }
}

// ============================================================================
// Observer Callbacks
// ============================================================================

// Helper functions for observer logic (called by lambdas and initial triggers)

/**
 * @brief Update slot color visualization
 */
static void apply_slot_color(AmsSlotData* data, int color_int) {
    lv_color_t filament_color = lv_color_hex(static_cast<uint32_t>(color_int));
    if (data->use_3d_style && data->spool_canvas) {
        ui_spool_canvas_set_color(data->spool_canvas, filament_color);
    } else if (data->color_swatch) {
        lv_obj_set_style_bg_color(data->color_swatch, filament_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(data->color_swatch, LV_OPA_COVER, LV_PART_MAIN);
        if (data->spool_outer) {
            lv_color_t darker = ams_draw::darken_color(filament_color, 50);
            lv_obj_set_style_bg_color(data->spool_outer, darker, LV_PART_MAIN);
        }
    }
    spdlog::trace("[AmsSlot] Slot {} color updated to 0x{:06X}", data->slot_index,
                  static_cast<uint32_t>(color_int));
}

/**
 * @brief Update slot status badge and opacity
 */
static void apply_slot_status(AmsSlotData* data, int status_int) {
    if (!data->status_badge_bg)
        return;
    auto status = static_cast<SlotStatus>(status_int);

    lv_color_t badge_bg = theme_manager_get_color("ams_badge_bg");
    bool show_badge = true;
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
        // Always show badge so all physical gates are visible (gray for empty)
        badge_bg = theme_manager_get_color("ams_badge_bg");
        break;
    case SlotStatus::UNKNOWN:
    default:
        badge_bg = theme_manager_get_color("ams_badge_bg");
        break;
    }
    if (show_badge) {
        lv_obj_remove_flag(data->status_badge_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(data->status_badge_bg, badge_bg, LV_PART_MAIN);

        // Auto-contrast text color based on badge background brightness
        if (data->slot_badge) {
            lv_color_t text_color = theme_manager_get_contrast_color(badge_bg);
            lv_obj_set_style_text_color(data->slot_badge, text_color, LV_PART_MAIN);
        }
    } else {
        lv_obj_add_flag(data->status_badge_bg, LV_OBJ_FLAG_HIDDEN);
    }
    // Handle spool visibility based on status and assignment
    lv_opa_t spool_opa = LV_OPA_COVER;
    bool show_spool = true;
    bool show_empty_placeholder = false;

    if (status == SlotStatus::EMPTY) {
        // Check if slot is assigned (has Spoolman data or material)
        AmsBackend* backend = AmsState::instance().get_backend();
        bool is_assigned = false;
        if (backend && data->slot_index >= 0) {
            SlotInfo slot_info = backend->get_slot_info(data->slot_index);
            is_assigned = (slot_info.spoolman_id > 0 || !slot_info.material.empty());
        }

        if (is_assigned) {
            // Assigned but empty: ghosted spool at 20%
            spool_opa = LV_OPA_20;
        } else {
            // Unassigned and empty: hide spool, show empty placeholder circle
            show_spool = false;
            show_empty_placeholder = true;
        }
    }

    // Apply visibility and opacity to spool elements
    // Always keep spool_container visible for click targeting
    if (show_spool) {
        if (data->color_swatch)
            lv_obj_remove_flag(data->color_swatch, LV_OBJ_FLAG_HIDDEN);
        if (data->spool_outer)
            lv_obj_remove_flag(data->spool_outer, LV_OBJ_FLAG_HIDDEN);
        if (data->spool_hub)
            lv_obj_remove_flag(data->spool_hub, LV_OBJ_FLAG_HIDDEN);
        if (data->spool_canvas)
            lv_obj_remove_flag(data->spool_canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (data->color_swatch)
            lv_obj_add_flag(data->color_swatch, LV_OBJ_FLAG_HIDDEN);
        if (data->spool_outer)
            lv_obj_add_flag(data->spool_outer, LV_OBJ_FLAG_HIDDEN);
        if (data->spool_hub)
            lv_obj_add_flag(data->spool_hub, LV_OBJ_FLAG_HIDDEN);
        if (data->spool_canvas)
            lv_obj_add_flag(data->spool_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (data->color_swatch)
        lv_obj_set_style_bg_opa(data->color_swatch, spool_opa, LV_PART_MAIN);
    if (data->spool_outer)
        lv_obj_set_style_bg_opa(data->spool_outer, spool_opa, LV_PART_MAIN);
    if (data->spool_canvas)
        lv_obj_set_style_opa(data->spool_canvas, spool_opa, LV_PART_MAIN);

    // Show/hide empty slot placeholder
    if (data->empty_placeholder) {
        if (show_empty_placeholder) {
            lv_obj_remove_flag(data->empty_placeholder, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(data->empty_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }

    spdlog::trace("[AmsSlot] Slot {} status={} badge={} spool={}", data->slot_index,
                  slot_status_to_string(status), show_badge ? "visible" : "hidden",
                  show_empty_placeholder ? "placeholder"
                  : show_spool           ? (spool_opa == LV_OPA_COVER ? "full" : "ghosted")
                                         : "hidden");
}

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

    // Also check filament_loaded to only highlight when actually loaded
    lv_subject_t* loaded_subject = AmsState::instance().get_filament_loaded_subject();
    bool filament_loaded = loaded_subject ? (lv_subject_get_int(loaded_subject) != 0) : false;

    bool is_active = (current_slot == data->slot_index) && filament_loaded;

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

    spdlog::trace("[AmsSlot] Slot {} active={} (current_slot={}, loaded={})", data->slot_index,
                  is_active, current_slot, filament_loaded);
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

    bool is_active_operation = (action == AmsAction::HEATING || action == AmsAction::LOADING ||
                                action == AmsAction::UNLOADING || action == AmsAction::CUTTING ||
                                action == AmsAction::FORMING_TIP || action == AmsAction::PURGING ||
                                action == AmsAction::SELECTING);

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
static void apply_tool_badge(AmsSlotData* data, int mapped_tool) {
    if (!data || !data->tool_badge_bg) {
        return;
    }

    // Tool changers: badge is redundant with toolhead label below
    auto* backend = AmsState::instance().get_backend(0);
    if (backend && backend->get_type() == AmsType::TOOL_CHANGER) {
        lv_obj_add_flag(data->tool_badge_bg, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (mapped_tool >= 0) {
        // Tool is mapped - show badge with tool number
        char tool_text[8];
        snprintf(tool_text, sizeof(tool_text), "T%d", mapped_tool);
        lv_label_set_text(data->tool_badge, tool_text);
        lv_obj_remove_flag(data->tool_badge_bg, LV_OBJ_FLAG_HIDDEN);

        // Auto-contrast text color based on badge background
        if (data->tool_badge) {
            lv_color_t bg = lv_obj_get_style_bg_color(data->tool_badge_bg, LV_PART_MAIN);
            lv_color_t text_color = theme_manager_get_contrast_color(bg);
            lv_obj_set_style_text_color(data->tool_badge, text_color, LV_PART_MAIN);
        }
        spdlog::trace("[AmsSlot] Slot {} tool badge: {}", data->slot_index, tool_text);
    } else {
        // No tool mapped - hide badge
        lv_obj_add_flag(data->tool_badge_bg, LV_OBJ_FLAG_HIDDEN);
        spdlog::trace("[AmsSlot] Slot {} tool badge: hidden", data->slot_index);
    }
}

/**
 * @brief Update error indicator based on SlotInfo.error
 *
 * Shows a small colored dot at top-right of spool_container when the slot
 * has an error. Color varies by severity: red for ERROR, amber for WARNING.
 * Optionally pulsates when animations are enabled.
 */
static void apply_slot_error(AmsSlotData* data, const SlotInfo& slot) {
    if (!data || !data->error_indicator) {
        return;
    }

    if (slot.error.has_value()) {
        lv_color_t badge_color = ams_draw::severity_color(slot.error->severity);
        lv_obj_set_style_bg_color(data->error_indicator, badge_color, LV_PART_MAIN);
        lv_obj_remove_flag(data->error_indicator, LV_OBJ_FLAG_HIDDEN);

        // Start pulsating animation if animations are enabled
        if (DisplaySettingsManager::instance().get_animations_enabled()) {
            ams_draw::start_pulse(data->error_indicator, badge_color);
        } else {
            ams_draw::stop_pulse(data->error_indicator);
        }

        spdlog::trace("[AmsSlot] Slot {} error indicator: severity={}, msg='{}'", data->slot_index,
                      static_cast<int>(slot.error->severity), slot.error->message);
    } else {
        ams_draw::stop_pulse(data->error_indicator);
        lv_obj_add_flag(data->error_indicator, LV_OBJ_FLAG_HIDDEN);
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
 * @brief Create spool visualization inside spool_container
 *
 * Creates either 3D canvas or flat concentric rings based on config.
 * The spool_container is created by XML; this function populates it.
 */
static void create_spool_visualization(AmsSlotData* data) {
    if (!data || !data->spool_container) {
        spdlog::error("[AmsSlot] create_spool_visualization: missing spool_container");
        return;
    }

    // Check config for visualization style
    data->use_3d_style = is_3d_spool_style();

    // Spool size adapts to available space - scales with screen size
    int32_t space_lg = theme_manager_get_spacing("space_lg");
    int32_t spool_size = (space_lg * 4); // Responsive: 64px at 16px, 80px at 20px

    // Update spool_container size to match responsive sizing
    int32_t container_size = spool_size + 8; // Extra room for badge
    lv_obj_set_size(data->spool_container, container_size, container_size);

    if (data->use_3d_style) {
        // ====================================================================
        // 3D SPOOL CANVAS (Bambu-style pseudo-3D with gradients + AA)
        // ====================================================================
        lv_obj_t* canvas = ui_spool_canvas_create(data->spool_container, spool_size);
        if (canvas) {
            lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
            // Prevent flex layout from resizing the canvas
            lv_obj_set_style_min_width(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_min_height(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_max_width(canvas, spool_size, LV_PART_MAIN);
            lv_obj_set_style_max_height(canvas, spool_size, LV_PART_MAIN);
            ui_spool_canvas_set_color(canvas, lv_color_hex(AMS_DEFAULT_SLOT_COLOR));
            ui_spool_canvas_set_fill_level(canvas, data->fill_level);
            lv_obj_add_flag(canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
            data->spool_canvas = canvas;

            spdlog::debug("[AmsSlot] Created 3D spool_canvas ({}x{})", spool_size, spool_size);
        }
    } else {
        // ====================================================================
        // FLAT STYLE (skeuomorphic concentric rings)
        // ====================================================================
        int32_t filament_ring_size = spool_size - 8;
        int32_t hub_size = spool_size / 3;

        // Add shadow to spool_container for flat style
        lv_obj_set_style_radius(data->spool_container, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(data->spool_container, 8, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(data->spool_container, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(data->spool_container, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(data->spool_container, lv_color_black(), LV_PART_MAIN);

        // Layer 1: Outer ring (flange - darker shade of filament color)
        lv_obj_t* outer_ring = lv_obj_create(data->spool_container);
        lv_obj_set_size(outer_ring, spool_size, spool_size);
        lv_obj_align(outer_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(outer_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_color_t default_darker =
            ams_draw::darken_color(lv_color_hex(AMS_DEFAULT_SLOT_COLOR), 50);
        lv_obj_set_style_bg_color(outer_ring, default_darker, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(outer_ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(outer_ring, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(outer_ring, theme_manager_get_color("ams_hub"), LV_PART_MAIN);
        lv_obj_set_style_border_opa(outer_ring, LV_OPA_50, LV_PART_MAIN);
        lv_obj_remove_flag(outer_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(outer_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
        data->spool_outer = outer_ring;

        // Layer 2: Main filament color ring
        lv_obj_t* filament_ring = lv_obj_create(data->spool_container);
        lv_obj_set_size(filament_ring, filament_ring_size, filament_ring_size);
        lv_obj_align(filament_ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(filament_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(filament_ring, lv_color_hex(AMS_DEFAULT_SLOT_COLOR),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(filament_ring, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(filament_ring, 0, LV_PART_MAIN);
        lv_obj_remove_flag(filament_ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(filament_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
        data->color_swatch = filament_ring;

        // Layer 3: Center hub
        lv_obj_t* hub = lv_obj_create(data->spool_container);
        lv_obj_set_size(hub, hub_size, hub_size);
        lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(hub, theme_manager_get_color("ams_hub"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(hub, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(hub, theme_manager_get_color("ams_hub_border"), LV_PART_MAIN);
        lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(hub, LV_OBJ_FLAG_EVENT_BUBBLE);
        data->spool_hub = hub;

        spdlog::debug("[AmsSlot] Created flat spool rings ({}x{})", spool_size, spool_size);
    }

    // Create empty slot placeholder (muted circle outline, initially hidden)
    {
        lv_obj_t* ph = lv_obj_create(data->spool_container);
        lv_obj_set_size(ph, spool_size - 4, spool_size - 4);
        lv_obj_align(ph, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ph, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ph, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(ph, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(ph, theme_manager_get_color("text_muted"), LV_PART_MAIN);
        lv_obj_set_style_border_opa(ph, LV_OPA_40, LV_PART_MAIN);
        lv_obj_remove_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ph, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN);
        data->empty_placeholder = ph;
    }

    // Create error indicator dot (top-right of spool_container, initially hidden)
    {
        lv_obj_t* err = lv_obj_create(data->spool_container);
        lv_obj_set_size(err, 14, 14);
        lv_obj_set_style_radius(err, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(err, theme_manager_get_color("danger"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(err, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(err, 0, LV_PART_MAIN);
        lv_obj_set_align(err, LV_ALIGN_TOP_RIGHT);
        lv_obj_set_style_translate_x(err, -2, LV_PART_MAIN);
        lv_obj_set_style_translate_y(err, 2, LV_PART_MAIN);
        lv_obj_remove_flag(err, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(err, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(err, LV_OBJ_FLAG_HIDDEN);
        data->error_indicator = err;
    }

    // Move badges and indicators to front so they render on top of the spool visualization
    // (badges are created by XML before spool canvas/rings are added in C++)
    // Note: status_badge_bg is reparented to badge_layer by ams_detail_update_badges()
    if (data->tool_badge_bg) {
        lv_obj_move_to_index(data->tool_badge_bg, -1);
    }
    if (data->error_indicator) {
        lv_obj_move_to_index(data->error_indicator, -1);
    }
}

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

    // Get per-slot subjects (using active backend for multi-backend systems)
    int backend_idx = state.active_backend_index();
    lv_subject_t* color_subject = state.get_slot_color_subject(backend_idx, data->slot_index);
    lv_subject_t* status_subject = state.get_slot_status_subject(backend_idx, data->slot_index);
    lv_subject_t* current_slot_subject = state.get_current_slot_subject();
    lv_subject_t* filament_loaded_subject = state.get_filament_loaded_subject();

    // Capture container (lv_obj_t*) instead of data pointer to prevent
    // use-after-free when deferred callback executes after widget deletion.
    // The registry lookup acts as a validity check. (fixes #83)
    lv_obj_t* obj = data->container;
    if (color_subject) {
        data->color_observer =
            observe_int_sync<lv_obj_t>(color_subject, obj, [](lv_obj_t* o, int color_int) {
                auto* d = get_slot_data(o);
                if (d)
                    apply_slot_color(d, color_int);
            });
    }
    if (status_subject) {
        data->status_observer =
            observe_int_sync<lv_obj_t>(status_subject, obj, [](lv_obj_t* o, int status_int) {
                auto* d = get_slot_data(o);
                if (d)
                    apply_slot_status(d, status_int);
            });
    }
    if (current_slot_subject) {
        data->current_slot_observer = observe_int_sync<lv_obj_t>(
            current_slot_subject, obj, [](lv_obj_t* o, int current_slot) {
                auto* d = get_slot_data(o);
                if (d) {
                    evaluate_pulse_state(d);
                    apply_current_slot_highlight(d, current_slot);
                }
            });
    }
    if (filament_loaded_subject) {
        // When filament_loaded changes, re-evaluate highlight using current_slot value
        data->filament_loaded_observer = observe_int_sync<lv_obj_t>(
            filament_loaded_subject, obj, [](lv_obj_t* o, int /*loaded*/) {
                auto* d = get_slot_data(o);
                if (!d)
                    return;
                lv_subject_t* slot_subject = AmsState::instance().get_current_slot_subject();
                if (slot_subject) {
                    apply_current_slot_highlight(d, lv_subject_get_int(slot_subject));
                }
            });
    }

    // Action observer: auto-pulse this slot during active filament operations
    lv_subject_t* action_subject = state.get_ams_action_subject();
    if (action_subject) {
        data->action_observer =
            observe_int_sync<lv_obj_t>(action_subject, obj, [](lv_obj_t* o, int /*action*/) {
                auto* d = get_slot_data(o);
                if (d)
                    evaluate_pulse_state(d);
            });
    }

    // Target slot observer: re-evaluate pulse when swap target changes
    lv_subject_t* target_subject = state.get_pending_target_slot_subject();
    if (target_subject) {
        data->target_slot_observer =
            observe_int_sync<lv_obj_t>(target_subject, obj, [](lv_obj_t* o, int /*target*/) {
                auto* d = get_slot_data(o);
                if (d)
                    evaluate_pulse_state(d);
            });
    }

    // Update slot badge with 1-based display number
    if (data->slot_badge) {
        char badge_text[16];
        snprintf(badge_text, sizeof(badge_text), "%d", data->slot_index + 1);
        lv_label_set_text(data->slot_badge, badge_text);
    }

    // Trigger initial updates from current subject values
    if (color_subject && data->color_observer) {
        apply_slot_color(data, lv_subject_get_int(color_subject));
    }
    if (status_subject && data->status_observer) {
        apply_slot_status(data, lv_subject_get_int(status_subject));
    }
    if (current_slot_subject && data->current_slot_observer) {
        apply_current_slot_highlight(data, lv_subject_get_int(current_slot_subject));
    }

    // Update material label, tool badge, error indicator from backend
    AmsBackend* backend = state.get_backend();
    if (backend) {
        SlotInfo slot = backend->get_slot_info(data->slot_index);
        if (data->material_label) {
            lv_label_set_text(data->material_label,
                              slot.material.empty() ? "--" : slot.material.c_str());
        }
        // Update tool badge based on slot's mapped_tool
        apply_tool_badge(data, slot.mapped_tool);
        // Update error indicator from slot data
        apply_slot_error(data, slot);
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
    data->status_badge_bg = lv_obj_find_by_name(obj, "status_badge");
    data->slot_badge = lv_obj_find_by_name(obj, "slot_badge_label");
    data->tool_badge_bg = lv_obj_find_by_name(obj, "tool_badge");
    data->tool_badge = lv_obj_find_by_name(obj, "tool_badge_label");

    // Validate required children were found
    if (!data->spool_container) {
        spdlog::error("[AmsSlot] Failed to find spool_container in XML");
        return obj; // Return obj anyway so it gets cleaned up properly
    }

    // Create spool visualization (stays in C++)
    create_spool_visualization(data);

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
                data->color_observer.reset();
                data->status_observer.reset();
                data->current_slot_observer.reset();
                data->filament_loaded_observer.reset();

                data->slot_index = new_index;

                // Setup new observers
                setup_slot_observers(data);

                spdlog::debug("[AmsSlot] Set slot_index={}", data->slot_index);
            }
        } else if (strcmp(name, "fill_level") == 0) {
            // Parse fill level (0.0 = empty, 1.0 = full)
            float fill = strtof(value, nullptr);
            if (fill < 0.0f)
                fill = 0.0f;
            if (fill > 1.0f)
                fill = 1.0f;
            data->fill_level = fill;
            update_filament_ring_size(data);
            spdlog::trace("[AmsSlot] Set fill_level={:.2f}", data->fill_level);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_ams_slot_register(void) {
    // Register the XML component first (defines the structural template)
    lv_xml_register_component_from_file("A:ui_xml/ams_slot_view.xml");

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

    // Clear existing observers
    data->color_observer.reset();
    data->status_observer.reset();
    data->current_slot_observer.reset();
    data->filament_loaded_observer.reset();

    data->slot_index = slot_index;

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

    AmsState& state = AmsState::instance();
    int backend_idx = state.active_backend_index();

    // Trigger updates with current values (using helper functions instead of callbacks)
    lv_subject_t* color_subject = state.get_slot_color_subject(backend_idx, data->slot_index);
    if (color_subject) {
        apply_slot_color(data, lv_subject_get_int(color_subject));
    }

    lv_subject_t* status_subject = state.get_slot_status_subject(backend_idx, data->slot_index);
    if (status_subject) {
        apply_slot_status(data, lv_subject_get_int(status_subject));
    }

    lv_subject_t* current_slot_subject = state.get_current_slot_subject();
    if (current_slot_subject && data->current_slot_observer) {
        apply_current_slot_highlight(data, lv_subject_get_int(current_slot_subject));
    }

    // Update material, tool badge, error indicator from backend
    AmsBackend* backend = state.get_backend();
    if (backend) {
        SlotInfo slot = backend->get_slot_info(data->slot_index);
        if (data->material_label) {
            lv_label_set_text(data->material_label,
                              slot.material.empty() ? "--" : slot.material.c_str());
        }
        // Update tool badge based on slot's mapped_tool
        apply_tool_badge(data, slot.mapped_tool);
        // Update error indicator from slot data
        apply_slot_error(data, slot);
    }

    spdlog::trace("[AmsSlot] Refreshed slot {}", data->slot_index);
}

void ui_ams_slot_set_fill_level(lv_obj_t* obj, float fill_level) {
    if (!obj) {
        return;
    }

    auto* data = get_slot_data(obj);
    if (!data) {
        return;
    }

    // Clamp to valid range
    if (fill_level < 0.0f)
        fill_level = 0.0f;
    if (fill_level > 1.0f)
        fill_level = 1.0f;

    data->fill_level = fill_level;
    update_filament_ring_size(data);

    spdlog::trace("[AmsSlot] Slot {} fill_level set to {:.2f}", data->slot_index, fill_level);
}

float ui_ams_slot_get_fill_level(lv_obj_t* obj) {
    if (!obj) {
        return 1.0f; // Default to full
    }

    auto* data = get_slot_data(obj);
    if (!data) {
        return 1.0f;
    }

    return data->fill_level;
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
        lv_obj_set_style_pad_top(obj, 2, LV_PART_MAIN); // Original padding

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
