// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_endless_spool_arrows.h
 * @brief Canvas widget for visualizing endless spool backup relationships
 *
 * Draws routed arrow lines connecting slots that have backup relationships.
 * The arrows show chains at a glance (e.g., Slot 1 -> Slot 3 -> Slot 5).
 *
 * Visual design:
 *   - Routed lines: up from source slot, over horizontally, down to target slot
 *   - Arrowheads pointing down at target end
 *   - Multiple connections draw at different vertical heights to avoid overlap
 *   - Uses subtle color (#text_muted) to avoid overwhelming filament colors
 *
 * XML usage:
 * @code{.xml}
 * <endless_spool_arrows name="arrows_canvas"
 *                       width="100%" height="40"
 *                       slot_count="4"/>
 * @endcode
 *
 * XML attributes:
 *   - slot_count: Number of slots (1-16) - default 4
 *   - slot_width: Width of each slot cell in pixels - default 80
 */

/**
 * @brief Register the endless_spool_arrows widget with LVGL's XML system
 *
 * Must be called before any XML files using <endless_spool_arrows> are registered.
 */
void ui_endless_spool_arrows_register(void);

/**
 * @brief Create an endless spool arrows widget programmatically
 *
 * @param parent Parent LVGL object
 * @return Created widget or NULL on failure
 */
lv_obj_t* ui_endless_spool_arrows_create(lv_obj_t* parent);

/**
 * @brief Set the number of slots
 *
 * @param obj The endless_spool_arrows widget
 * @param count Number of slots (1-16)
 */
void ui_endless_spool_arrows_set_slot_count(lv_obj_t* obj, int count);

/**
 * @brief Set the width of each slot cell (for position calculations)
 *
 * @param obj The endless_spool_arrows widget
 * @param width Width in pixels
 */
void ui_endless_spool_arrows_set_slot_width(lv_obj_t* obj, int32_t width);

/**
 * @brief Set the overlap between adjacent slots (for 5+ slots)
 *
 * When slot_count > 4, slots overlap visually. The overlap value should
 * match the value used by the slot grid (typically 50% of slot_width).
 *
 * @param obj The endless_spool_arrows widget
 * @param overlap Overlap in pixels (0 = no overlap)
 */
void ui_endless_spool_arrows_set_slot_overlap(lv_obj_t* obj, int32_t overlap);

/**
 * @brief Set the slot row's left offset (the slot grid's centering or lead-in)
 *
 * @param obj The endless_spool_arrows widget
 * @param offset Pixels from the widget's left edge to the first slot's left edge
 */
// NAMESPACE_OK: the widget's C setter API, beside its siblings
void ui_endless_spool_arrows_set_slot_offset(lv_obj_t* obj, int32_t offset);

/**
 * @brief Set endless spool configuration
 *
 * Provides the backup slot mappings for visualization.
 * Each element in the array represents one slot's backup:
 *   - Index is the source slot
 *   - Value is the backup slot (-1 = no backup)
 *
 * @param obj The endless_spool_arrows widget
 * @param backup_slots Array of backup slot indices (one per slot)
 * @param count Number of elements in the array
 */
void ui_endless_spool_arrows_set_config(lv_obj_t* obj, const int* backup_slots, int count);

/**
 * @brief Clear all endless spool connections
 *
 * @param obj The endless_spool_arrows widget
 */
void ui_endless_spool_arrows_clear(lv_obj_t* obj);

/**
 * @brief Force redraw of the arrows
 *
 * @param obj The endless_spool_arrows widget
 */
void ui_endless_spool_arrows_refresh(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
