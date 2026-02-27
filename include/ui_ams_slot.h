// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_slot.h
 * @brief Custom LVGL XML widget for AMS filament slot display
 *
 * The ams_slot widget encapsulates a single AMS slot with:
 * - Color swatch showing filament color (bound to ams_slot_N_color subject)
 * - Status icon (available, loaded, blocked, etc.)
 * - Material label (PLA, PETG, etc.)
 * - Slot number badge
 * - Active slot highlight border
 *
 * XML usage:
 * @code{.xml}
 * <ams_slot slot_index="0"/>
 * <ams_slot slot_index="1"/>
 * @endcode
 *
 * The widget automatically creates observers on AmsState subjects based on
 * slot_index and cleans them up when the widget is deleted.
 */

/**
 * @brief Register the ams_slot widget with LVGL's XML system
 *
 * Must be called AFTER AmsState::init_subjects() and BEFORE any XML files
 * using <ams_slot> are registered.
 */
void ui_ams_slot_register(void);

/**
 * @brief Get the slot index from an ams_slot widget
 *
 * @param obj The ams_slot widget
 * @return Slot index (0-15), or -1 if not an ams_slot widget
 */
int ui_ams_slot_get_index(lv_obj_t* obj);

/**
 * @brief Set the slot index on an ams_slot widget
 *
 * This re-creates the observers for the new slot index.
 * Generally only called during initial XML parsing.
 *
 * @param obj The ams_slot widget
 * @param slot_index Slot index (0-15)
 */
void ui_ams_slot_set_index(lv_obj_t* obj, int slot_index);

/**
 * @brief Force refresh of all visual elements from current AmsState
 *
 * Useful after backend sync or when slot becomes visible.
 *
 * @param obj The ams_slot widget
 */
void ui_ams_slot_refresh(lv_obj_t* obj);

/**
 * @brief Set the fill level of the spool visualization
 *
 * Used to show remaining filament when integrated with Spoolman.
 * The filament ring scales from full (near outer edge) to empty (near hub).
 *
 * @param obj        The ams_slot widget
 * @param fill_level Fill level from 0.0 (empty) to 1.0 (full)
 */
void ui_ams_slot_set_fill_level(lv_obj_t* obj, float fill_level);

/**
 * @brief Get the current fill level of a slot
 *
 * @param obj The ams_slot widget
 * @return Fill level (0.0-1.0), or 1.0 if not an ams_slot widget
 */
float ui_ams_slot_get_fill_level(lv_obj_t* obj);

/**
 * @brief Set layout info for staggered label positioning
 *
 * When there are many slots (>4), labels are staggered vertically to avoid
 * overlap. This function tells the slot its position in the sequence so it
 * can position its label at Low/Medium/High height.
 *
 * @param obj         The ams_slot widget
 * @param slot_index  This slot's index (0-based)
 * @param total_count Total number of slots being displayed
 */
void ui_ams_slot_set_layout_info(lv_obj_t* obj, int slot_index, int total_count);

/**
 * @brief Move label and leader line to an external container for z-ordering
 *
 * When slots overlap visually, labels can be obscured by adjacent slots.
 * This function reparents the label and leader line to an overlay container
 * that renders on top of all slots.
 *
 * @param obj           The ams_slot widget
 * @param labels_layer  Target container for label/leader (should be above slots in z-order)
 * @param slot_center_x X position of slot center in labels_layer coords
 */
void ui_ams_slot_move_label_to_layer(lv_obj_t* obj, lv_obj_t* labels_layer, int32_t slot_center_x);

/**
 * @brief Move status badge to an external container for z-ordering
 *
 * Reparents the status badge (slot number indicator) to a badge layer
 * that renders in front of the tray.
 *
 * @param obj           The ams_slot widget
 * @param badge_layer   Target container (should be above tray in z-order)
 * @param slot_center_x X position of slot center in badge_layer coords
 */
void ui_ams_slot_move_badge_to_layer(lv_obj_t* obj, lv_obj_t* badge_layer, int32_t slot_center_x);

/**
 * @brief Set pulsing animation state for a slot (during loading operations)
 *
 * When enabled, the spool_container border pulses continuously to indicate
 * the slot is the target of an ongoing operation (loading/unloading).
 * When disabled, restores the border to its static state (highlighted if active,
 * hidden if not).
 *
 * @param obj     The ams_slot widget
 * @param pulsing true to start continuous pulse, false to stop
 */
void ui_ams_slot_set_pulsing(lv_obj_t* obj, bool pulsing);

/**
 * @brief Forcibly clear the highlight ring on a slot
 *
 * Removes the border highlight and blocks automatic re-highlighting until
 * set_pulsing() is called. Use this during swap operations where the slot
 * being unloaded should lose its highlight but NOT pulse.
 *
 * @param obj The ams_slot widget
 */
void ui_ams_slot_clear_highlight(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
