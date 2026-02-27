// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_ams_slot_layout.h"

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_detail.h
 * @brief Shared helper functions for AMS spool grid rendering
 *
 * Eliminates duplication between AmsPanel and AmsOverviewPanel.
 * Both panels embed an <ams_unit_detail/> XML component and call
 * these free functions to manage slot creation, tray sizing,
 * label management, and path canvas setup.
 */

/// Maximum slots supported in a single detail view
static constexpr int AMS_DETAIL_MAX_SLOTS = 16;

/// Widget pointers resolved from an ams_unit_detail component
struct AmsDetailWidgets {
    lv_obj_t* root = nullptr;         ///< The ams_unit_detail root object
    lv_obj_t* slot_grid = nullptr;    ///< Flex row container for ams_slot widgets
    lv_obj_t* slot_tray = nullptr;    ///< Visual "holder" in front of spool bottoms
    lv_obj_t* labels_layer = nullptr; ///< Overlay for material labels (5+ slots)
    lv_obj_t* badge_layer = nullptr;  ///< Overlay for slot badges (in front of tray)
};

/**
 * @brief Resolve child widget pointers from an ams_unit_detail root
 * @param root The ams_unit_detail object (from lv_obj_find_by_name or lv_xml_create)
 * @return Populated AmsDetailWidgets (members may be nullptr if not found)
 */
AmsDetailWidgets ams_detail_find_widgets(lv_obj_t* root);

/**
 * @brief Create slot widgets in the grid from backend data
 *
 * Clears existing slots, creates new ams_slot widgets via XML, applies
 * layout sizing (width, overlap), and wires click handlers.
 *
 * @param w           Widget pointers from ams_detail_find_widgets()
 * @param slot_widgets Output array of created slot widget pointers
 * @param max_slots   Size of slot_widgets array (use AMS_DETAIL_MAX_SLOTS)
 * @param unit_index  Backend unit index (-1 = whole backend for single-unit panels)
 * @param click_cb    Per-slot click callback (panel-specific)
 * @param user_data   User data for click callback (typically panel pointer)
 * @return Number of slots created, and populated AmsSlotLayout via out param
 */
struct AmsDetailSlotResult {
    int slot_count = 0;
    AmsSlotLayout layout;
};

AmsDetailSlotResult ams_detail_create_slots(AmsDetailWidgets& w, lv_obj_t* slot_widgets[],
                                            int max_slots, int unit_index, lv_event_cb_t click_cb,
                                            void* user_data);

/**
 * @brief Destroy all slot widgets in the grid
 * @param w           Widget pointers
 * @param slot_widgets Array of slot widget pointers to clear
 * @param slot_count  Number of slots to destroy (reset to 0)
 */
void ams_detail_destroy_slots(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int& slot_count);

/**
 * @brief Size tray to 1/3 of slot grid height (minimum 20px)
 * @param w Widget pointers — uses slot_grid height and positions slot_tray
 */
void ams_detail_update_tray(AmsDetailWidgets& w);

/**
 * @brief Move material labels to overlay layer for 5+ overlapping slots
 * @param w           Widget pointers
 * @param slot_widgets Array of slot widget pointers
 * @param slot_count  Number of active slots
 * @param layout      Slot layout (width + overlap) from create_slots result
 */
void ams_detail_update_labels(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int slot_count,
                              const AmsSlotLayout& layout);

/**
 * @brief Move slot badges to overlay layer so they render in front of the tray
 * @param w           Widget pointers
 * @param slot_widgets Array of slot widget pointers
 * @param slot_count  Number of active slots
 * @param layout      Slot layout (width + overlap) from create_slots result
 */
void ams_detail_update_badges(AmsDetailWidgets& w, lv_obj_t* slot_widgets[], int slot_count,
                              const AmsSlotLayout& layout);

/**
 * @brief Configure a path canvas from backend state
 *
 * Sets slot count, topology, active slot, filament segments, colors,
 * slot sizing, and Voron toolhead mode.
 *
 * @param canvas     The filament_path_canvas widget
 * @param slot_grid  The slot grid (for sizing sync) — may be nullptr
 * @param unit_index Backend unit index (-1 = whole backend)
 * @param hub_only   If true, only draw slots → hub (skip downstream)
 */
void ams_detail_setup_path_canvas(lv_obj_t* canvas, lv_obj_t* slot_grid, int unit_index,
                                  bool hub_only);
