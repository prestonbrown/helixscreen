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
 * Draws the endless-spool relation above the slot row, one mark per group:
 *   - Unordered group (any member substitutes for any other): a BRACKET, one
 *     horizontal bar spanning the members with a tick down to each member,
 *     ending in a dot. No arrowheads, because the group has no order.
 *   - Ordered group (a directed hand-off, e.g. AFC's SET_RUNOUT): a routed
 *     arrow per hand-off, up from the source, over, and down into the target.
 *   - Overlapping marks stack at different heights; a bar is broken where
 *     another mark's vertical passes through it, so membership stays readable.
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
 *   - slot_count: Number of slots (1 to helix::kAmsMaxSlots) - default 4
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
 * @param count Number of slots, clamped to 1..helix::kAmsMaxSlots
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
 * @brief Clear all endless spool connections, hiding the widget
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

#include "ams_types.h"

#include <vector>

/**
 * @brief Set the endless-spool relation to draw
 *
 * The widget decides bracket vs arrow from each group's `ordered` flag.
 * Members at or beyond the slot count are not drawn. Set the slot count
 * first: the widget hides itself when the relation draws no mark over it.
 *
 * @param obj The endless_spool_arrows widget
 * @param config The backend's group relation
 */
void ui_endless_spool_arrows_set_groups(lv_obj_t* obj,
                                        const helix::printer::EndlessSpoolConfig& config);

namespace helix::ui {

/// What one endless-spool mark draws as.
enum class EndlessSpoolMarkKind {
    Bracket, ///< Unordered group: bar over the members, a tick + dot per member
    Arrow,   ///< One ordered hand-off: routed line with an arrowhead at the target
};

/// Where a vertical meets the slot row: a slot, nudged sideways by whole lanes.
struct EndlessSpoolColumn {
    int slot = 0;
    /// 0 = the slot's centre. An arrow's source sits one lane left of it and
    /// its arrowhead one lane right, so a slot that is both the target of one
    /// hop and the source of the next shows two separate lines. A second
    /// arrowhead into the same slot lands one lane further right.
    int lane = 0;
    /// `lane` in pixels: lane times the lane width the limits allow.
    int32_t offset_px = 0;

    bool operator==(const EndlessSpoolColumn& o) const {
        return slot == o.slot && lane == o.lane;
    }
    bool operator!=(const EndlessSpoolColumn& o) const {
        return !(*this == o);
    }
};

/// One mark above the slot row, in slot and lane units plus the pixel sizes
/// that depend on them.
struct EndlessSpoolMark {
    EndlessSpoolMarkKind kind = EndlessSpoolMarkKind::Bracket;
    /// Bracket: members, ascending. Arrow: {source, target}.
    std::vector<int> slots;
    /// One per entry of `slots`: where that vertical drops. Bracket ticks are
    /// centred; arrow ends take their own lane.
    std::vector<EndlessSpoolColumn> columns;
    int min_slot = 0; ///< Leftmost slot the mark touches
    int max_slot = 0; ///< Rightmost slot the mark touches
    /// 0 = the bar furthest from the slots; each level steps toward them.
    /// Never deeper than EndlessSpoolMarkLimits::max_height_level.
    int height_level = 0;
    /// Columns where another mark's vertical crosses this bar; the bar is broken there.
    std::vector<EndlessSpoolColumn> bar_gaps;
    /// Arrow: half-width of the arrowhead, small enough that arrowheads sharing
    /// a slot never overlap. Bracket: 0.
    int32_t head_px = 0;
};

/// What the canvas can hold. The defaults impose nothing: unlimited levels and
/// lanes with no width.
struct EndlessSpoolMarkLimits {
    /// Deepest height level the canvas fits; deeper marks share that bar. -1 = no limit.
    int max_height_level = -1;
    /// Preferred lane width in pixels.
    int32_t lane_px = 0;
    /// Distance between neighbouring slot centres in pixels; 0 = do not narrow lanes.
    /// Lanes narrow so the outermost one stays inside its slot's half of it.
    int32_t slot_spacing_px = 0;
};

/**
 * @brief Lay out the marks for an endless-spool relation
 *
 * Pure geometry, no LVGL: which marks exist, which column each vertical drops
 * in, their height levels, and where each bar is broken. No two arrow ends
 * share a column, and marks whose extents overlap (lanes included, so two hops
 * meeting at a slot overlap) never share a height level unless the canvas
 * limit forces them onto its last one. A mark sits below every mark that
 * contains its extent. Members outside [0, slot_count) are dropped; a group
 * left with fewer than two members draws nothing.
 *
 * @param config     The group relation
 * @param slot_count Number of slots on the row
 * @param limits     What the canvas can hold
 * @return Marks, in no particular order
 */
[[nodiscard]] std::vector<EndlessSpoolMark>
endless_spool_marks(const helix::printer::EndlessSpoolConfig& config, int slot_count,
                    const EndlessSpoolMarkLimits& limits = {});

} // namespace helix::ui
#endif
