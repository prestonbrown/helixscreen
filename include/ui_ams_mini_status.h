// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_ams_mini_status.h
 * @brief Compact AMS status indicator widget for home panel
 *
 * The ams_mini_status widget shows a compact visualization of AMS filament slots
 * as vertical bar indicators. Each bar shows three pieces of information:
 *
 * 1. **Color**: Bar fill color = filament color
 * 2. **Presence**: Empty slots shown as gray/transparent
 * 3. **Fill level**: Bar fills from bottom based on remaining filament %
 *
 * Layout:
 * - Up to 8 vertical bars representing slots/lanes
 * - "+N" overflow indicator when more than max_visible slots exist
 * - Auto-hides when slot_count == 0
 *
 * The spool view never draws a lane below kAmsMiniUsableSpoolPx: lanes that
 * all fit at that size wrap into the rows that give the largest spool, and
 * lanes that do not scroll sideways in as many rows as the height holds. A
 * widget too short for one usable row shows the bar view. XML attribute
 * `fill`: "true" lets the spools grow past the home tray's 56px cap (a host
 * that hands the tray a whole column), "auto" (the home grid's tile) does too
 * and titles each unit's group on a rig with several units.
 *
 * Bar mode renders through ams_lane_bar widgets driven by AmsState's per-slot
 * subjects; the programmatic setters below feed the spool-mode cells and the
 * slot-count-driven layout only.
 *
 * Programmatic usage:
 * @code{.cpp}
 * lv_obj_t* indicator = ui_ams_mini_status_create(parent, 32);  // 32px height
 * ui_ams_mini_status_set_slot_count(indicator, 4);
 * @endcode
 */

/** Maximum number of visible slots (bars) in the compact view */
#define AMS_MINI_STATUS_MAX_VISIBLE 8

/**
 * @brief Create an AMS mini status indicator programmatically
 *
 * @param parent Parent LVGL object
 * @param height Height of the indicator in pixels (bars scale to this)
 * @return Created indicator object, or NULL on failure
 */
lv_obj_t* ui_ams_mini_status_create(lv_obj_t* parent, int32_t height);

/**
 * @brief Set the total number of slots
 *
 * If slot_count > max_visible, a "+N" overflow indicator is shown.
 * If slot_count == 0, the widget is hidden.
 *
 * @param obj The ams_mini_status widget
 * @param slot_count Total number of slots in the AMS system
 */
void ui_ams_mini_status_set_slot_count(lv_obj_t* obj, int slot_count);

/**
 * @brief Set the maximum number of visible slots
 *
 * @param obj The ams_mini_status widget
 * @param max_visible Maximum slots to show (1-8, default 8)
 */
void ui_ams_mini_status_set_max_visible(lv_obj_t* obj, int max_visible);

/**
 * @brief Update one lane's spool-mode label data (material, percent)
 *
 * The spool graphic, fill, ghosting and error dot are not part of this API:
 * they render from AmsState's per-slot subjects inside each cell's embedded
 * ams_lane_spool widget.
 *
 * @param obj The ams_mini_status widget
 * @param slot_index Slot index (0-based)
 * @param material Material name (e.g. "PLA"); "" renders as "Empty"
 * @param remaining_pct Actual percent remaining (0-100), or -1 if unknown
 */
void ui_ams_mini_status_set_slot_label(lv_obj_t* obj, int slot_index, const char* material,
                                       int remaining_pct);

/**
 * @brief Force refresh/redraw of all slots
 *
 * @param obj The ams_mini_status widget
 */
void ui_ams_mini_status_refresh(lv_obj_t* obj);

/**
 * @brief Set the available pixel width, for responsive sizing / mode selection.
 *
 * width_px >= widget_size::w_normal() (include/panel_widget_size.h) selects the
 * wide spool view; narrower widths keep the compact bar view. The band is
 * per-tier, so the same pixel width can mean different things on different
 * panels - what has to fit in the wide view is text. Also drives the bar-width
 * band and the spool count within the wide view.
 *
 * @param obj The ams_mini_status widget
 * @param width_px Available pixel width for the widget
 */
void ui_ams_mini_status_set_width(lv_obj_t* obj, int width_px);

/**
 * @brief Check if this is an ams_mini_status widget
 *
 * @param obj Object to check
 * @return true if this is an ams_mini_status widget
 */
bool ui_ams_mini_status_is_valid(lv_obj_t* obj);

/**
 * @brief Register ams_mini_status as an XML widget
 *
 * Call this once during application initialization to enable
 * using <ams_mini_status/> in XML layouts. The XML widget
 * automatically fills its parent and binds to AmsState.
 */
void ui_ams_mini_status_init(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <vector>

namespace helix::ui {

/// How the spool view uses the widget's height: the XML attribute `fill`.
enum class AmsMiniFill {
    Off,  ///< "false": spools up to the home tray's 56px
    On,   ///< "true": spools up to 120px, the lanes as one block
    Auto, ///< "auto": spools up to 120px, each unit's group titled on a multi-unit rig
};

/**
 * @brief Set how large the spool view's spools grow and whether it titles unit
 * groups (XML `fill`). Every mode keeps lanes usable and scrolls the rest.
 */
void ams_mini_status_set_fill(lv_obj_t* obj, AmsMiniFill fill);

/**
 * @brief The fixed sizes the grid view is laid out with (px).
 *
 * The grid view puts each unit's lanes in a group, the groups wrapped into
 * lines across the widget. A titled group (several units) has a box and a
 * title line; an untitled one (a single unit) has neither.
 */
struct AmsMiniGridMetrics {
    int gap = 0;          ///< between lanes inside a group
    int group_gap = 0;    ///< between groups, across and down
    int group_pad = 0;    ///< inside a titled group's box
    int title_h = 0;      ///< a titled group's title line and its gap; 0 untitled
    int badge_margin = 0; ///< what a lane spool adds around its graphic
    int text_h = 0;       ///< the text block over each spool, gap included
    int max_spool = 0;
    bool bypass = false; ///< one more cell past the lanes, behind a 1px divider
};

/// One unit's block in a grid plan: its lanes as @p cols x @p rows, starting a
/// new line of groups when @p new_line.
struct AmsMiniGridGroup {
    int lanes = 0;
    int cols = 0;
    int rows = 0;
    bool new_line = false;
};

/// A grid layout: every lane cell_w x row_h. spool <= 0 when nothing fits.
/// A plan that @p scrolls lays its groups in one line wider than the box.
struct AmsMiniGridPlan {
    int spool = 0;
    int cell_w = 0;
    int row_h = 0;
    bool scrolls = false;
    std::vector<AmsMiniGridGroup> groups;
};

/**
 * @brief Lay @p unit_lanes (each unit's lane count, in order) out in an
 * @p avail_w x @p avail_h box: each unit's lanes kept together, the units
 * packed into lines, and the line width chosen for the largest spool.
 *
 * Pure: the strip measures the box and the metrics, then builds what this
 * returns.
 */
AmsMiniGridPlan plan_ams_mini_grid(const std::vector<int>& unit_lanes, int avail_w, int avail_h,
                                   const AmsMiniGridMetrics& m);

/**
 * @brief Lay @p unit_lanes out at no less than @p min_spool, scrolling sideways:
 * as many rows as fit the box's height at that size (one line of groups, each
 * unit as rows x columns), every cell at least @p min_cell_w wide so its text
 * fits whole. @p scrollbar_h comes off the height for the scrollbar under it.
 *
 * spool <= 0 when even one row does not fit at @p min_spool.
 */
AmsMiniGridPlan plan_ams_mini_scroll(const std::vector<int>& unit_lanes, int avail_h,
                                     int min_cell_w, int scrollbar_h, int min_spool,
                                     const AmsMiniGridMetrics& m);

/**
 * @brief Whether a multi-unit layout lays its lanes out as one block instead of
 * titled unit groups: the groups' boxes and titles leave no usable spool
 * (@p min_spool) or cost it more than a third of the block's.
 */
bool ams_mini_grid_prefers_flat(int grouped_spool, int flat_spool, int min_spool);

/// The smallest spool any spool layout draws (px). Below it a lane is not usable:
/// the spool, its number and its material stop reading, so the lanes scroll
/// instead.
inline constexpr int kAmsMiniUsableSpoolPx = 36;

/**
 * @brief Whether the tray draws anything: some lanes, and at least one of
 * them in a unit that is present. With nothing to draw the widget hides.
 *
 * @param slot_count The filament system's lane count (ams_slot_count)
 * @param present_lanes Lanes not in an absent unit
 */
bool ams_mini_status_shows_lanes(int slot_count, int present_lanes);

/**
 * @brief ams_mini_status_shows_lanes() over AmsState as it stands now,
 * which is what a tray would draw. False without a backend.
 */
bool ams_mini_status_has_lanes_to_show();

/**
 * @brief Whether a lane tray shows: the printer has a filament system or several
 * heads (`has_filament_system_or_multi_tool`, as TargetTool publishes it) and the
 * tray has lanes to draw (ams_mini_status_has_lanes_to_show()). Print Status's
 * tray and the Filament panel's ultrawide side band ask this one question.
 */
bool has_lane_tray();

} // namespace helix::ui
#endif
