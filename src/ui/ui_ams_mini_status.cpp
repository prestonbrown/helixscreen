// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_mini_status.h"

#include "ui_fonts.h"
#include "ui_nav_manager.h"
#include "ui_observer_guard.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_state.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "observer_factory.h"
#include "panel_widget_size.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Layout constants
// ============================================================================

/** Minimum bar width in pixels (prevents bars from becoming invisible) */
static constexpr int32_t MIN_BAR_WIDTH_PX = 3;

/** Maximum bar width in pixels (prevents bars from becoming too wide) */
static constexpr int32_t MAX_BAR_WIDTH_PX = 16;

/** Border radius for bar corners in pixels (very rounded appearance) */
static constexpr int32_t BAR_BORDER_RADIUS_PX = 8;

/**
 * Smallest spool graphic the wide view will draw (px).
 *
 * A floor on a vector drawing, not on type: below roughly this size the spool
 * rings and the lane badge stop reading as a spool at all, whatever the tier's
 * font ladder is doing. Deliberately not derived from a spacing or font token -
 * see min_spool_cell_w() for what IS derived from it.
 */
static constexpr int MIN_SPOOL_IMG_PX = 24;

/**
 * Largest spool graphic the wide view will draw (px).
 *
 * The spool is otherwise sized to the row (`avail_h - 4`), and a home-widget
 * row can be tall; past this the graphic dominates the cell and crowds the text
 * column for no added legibility. Left flat on purpose: the row height it caps
 * already scales with the grid, and the only token in the same shape
 * (ams_slot_spool_size, the full AMS panel's spool) runs 48/48/48/64/80/80/80,
 * so adopting it would shrink the graphic on the three cramped tiers and grow
 * it on the rest for reasons unrelated to fitting anything.
 */
static constexpr int MAX_SPOOL_IMG_PX = 56;

/**
 * Minimum width of one spool cell in the wide spool view (px), and the divisor
 * used to derive how many spool cells fit across a row:
 * visible = (avail_w + gap) / (min_spool_cell_w(...) + gap).
 *
 * Derived rather than chosen: a cell is [spool graphic][gap][text column], so
 * the narrowest useful cell is the smallest spool that still reads as a spool,
 * plus the badge slack create_spool_visual() adds around it, plus the gap, plus
 * enough room for the widest material string actually about to be drawn.
 *
 * Deriving it is what keeps the two halves of the layout consistent. They were
 * two independent constants before - a flat 60px divisor here and a flat 34px
 * text reservation below - so on tiers where font_small is 16-26px rather than
 * the ~12px they were calibrated against, the divisor packed in cells too
 * narrow for the reservation the same function then tried to honor. The cell
 * won, the reservation lost, and LV_LABEL_LONG_WRAP broke "PLA" into one glyph
 * per line. Sharing one `min_text` makes that disagreement unrepresentable.
 */
static int min_spool_cell_w(int min_text, int gap) {
    return MIN_SPOOL_IMG_PX + ams_draw::SPOOL_VISUAL_BADGE_MARGIN_PX + gap + min_text;
}

// ============================================================================
// Per-widget user data
// ============================================================================

/** Magic number to identify ams_mini_status widgets ("AMS1" as ASCII) */
static constexpr uint32_t AMS_MINI_STATUS_MAGIC = 0x414D5331;

/** Render mode: narrow bars vs. wide spool cells (selected by width_px) */
enum class AmsMiniMode { BAR, SPOOL };

/**
 * @brief Per-slot data for the wide spool render mode
 */
struct SpoolCellData {
    uint32_t color_rgb = 0x808080;
    float fill_level = 1.0f; // 0.0-1.0 for the spool graphic
    int remaining_pct = -1;  // actual % remaining; -1 = unknown (blank label)
    std::string material;    // "" => render "--"
    bool present = false;
    int lane_number = 1;   // 1-based, for the badge
    bool active = false;   // actively-loaded lane (success-colored badge)
    bool assigned = false; // lane still carries identity while ejected (#1071)

    bool operator==(const SpoolCellData& o) const {
        return color_rgb == o.color_rgb && fill_level == o.fill_level &&
               remaining_pct == o.remaining_pct && material == o.material && present == o.present &&
               lane_number == o.lane_number && active == o.active && assigned == o.assigned;
    }
};

/**
 * @brief Is this lane the one firmware considers seated and loaded?
 *
 * SINGLE SOURCE OF TRUTH for the active-lane highlight: the per-slot
 * active-loaded subject (AmsBackend::slot_is_actively_loaded(i)) — the same read
 * apply_current_slot_highlight() makes in ui_ams_slot.cpp. Deriving it here from
 * `i == current_slot` instead diverged from the AMS panel: after an idle unload
 * the strip kept the lane badged while the panel's glow had already cleared, and
 * on AFC/CFS — where firmware's current_slot can name a lane the per-slot parse
 * disagrees with (#1194) — the two surfaces could light DIFFERENT lanes.
 *
 * Unlike the ams_slot widget (one lane, per-slot observer) this widget needs no
 * per-slot observer: AmsState bumps slots_version on every slot_active_loaded
 * delta (sync_from_backend and update_slot both do), and the slots_version
 * observer re-reads every lane. A per-slot observer would race the wholesale
 * rebuild — the same reasoning that keeps fill on the snapshot (#705/#776).
 *
 * Out-of-range lanes (beyond AmsState::MAX_SLOTS) have no subject and report
 * inactive, matching ams_slot's fallback.
 */
static bool slot_is_active_loaded(int slot_index) {
    lv_subject_t* subject = AmsState::instance().get_slot_active_loaded_subject(slot_index);
    return subject && lv_subject_get_int(subject) != 0;
}

/**
 * @brief Dim a spool visual to the "assigned but ejected" ghost strength.
 *
 * Mirrors apply_slot_status() in ui_ams_slot.cpp: an empty lane that still
 * carries identity (Spoolman link, material, brand, or spool name — deliberately
 * NOT cleared on eject, #1071) renders its retained spool at LV_OPA_20 so it
 * reads as "assigned, not present" rather than "still loaded" (#1065). Call
 * AFTER spool_visual_set_color(), which resets bg_opa to COVER.
 */
static void spool_visual_set_ghost_opa(const ams_draw::SpoolVisual& sv, lv_opa_t opa) {
    if (sv.color_swatch)
        lv_obj_set_style_bg_opa(sv.color_swatch, opa, LV_PART_MAIN);
    if (sv.spool_outer)
        lv_obj_set_style_bg_opa(sv.spool_outer, opa, LV_PART_MAIN);
    if (sv.canvas)
        lv_obj_set_style_opa(sv.canvas, opa, LV_PART_MAIN);
}

/** Map an integer fill percent (0-100) to the spool graphic's 0.0-1.0 level. */
static inline float fill_level_from_pct(int fill_pct) {
    if (fill_pct <= 0)
        return 0.0f;
    if (fill_pct >= 100)
        return 1.0f;
    return fill_pct / 100.0f;
}

/**
 * @brief Per-slot data stored for each bar
 */
struct SlotBarData {
    ams_draw::SlotColumn col; // Shared slot column (container, bar_bg, bar_fill, status_line)
    uint32_t color_rgb = 0x808080;
    int fill_pct = 100;
    bool present = false;                           // Filament present in slot
    bool loaded = false;                            // Filament loaded to toolhead
    bool has_error = false;                         // Slot is in error/blocked state
    SlotError::Severity severity = SlotError::INFO; // Error severity level
};

/**
 * @brief Per-unit row info for multi-unit stacked display
 */
struct UnitRowInfo {
    int first_slot = 0; // Index into slots[] array
    int slot_count = 0;
    lv_obj_t* row_container = nullptr; // Row container for this unit's bars
};

/**
 * @brief User data stored on each ams_mini_status widget
 */
struct AmsMiniStatusData {
    uint32_t magic = AMS_MINI_STATUS_MAGIC;
    int32_t height = 32;
    int slot_count = 0;
    int max_visible = AMS_MINI_STATUS_MAX_VISIBLE;

    // Available pixel width for responsive sizing
    int width_px = 0; // 0 = unknown/default, set by set_width()

    // Multi-unit support
    int unit_count = 0;       // Number of AMS units (0 or 1 = single row, 2+ = stacked rows)
    UnitRowInfo unit_rows[8]; // Max 8 units

    // Render mode selection (BAR for narrow, SPOOL for width_px >= w_normal())
    AmsMiniMode mode = AmsMiniMode::BAR;

    // Child objects
    lv_obj_t* container = nullptr;        // Main container
    lv_obj_t* bars_container = nullptr;   // Container for slot bars
    lv_obj_t* spools_container = nullptr; // Container for wide spool cells
    lv_obj_t* overflow_label = nullptr;   // "+N" overflow indicator

    // Per-slot data
    SlotBarData slots[AMS_MINI_STATUS_MAX_VISIBLE];

    // Per-slot data for the spool render mode (sized to slot_count; uncapped for multi-unit)
    std::vector<SpoolCellData> spool_cells;

    // Cached signature of what spools_container currently renders. rebuild_spools
    // skips the expensive clean+recreate when these still match the live inputs,
    // avoiding constant canvas alloc/free churn on every AmsState sync.
    std::vector<SpoolCellData> rendered_cells;
    int rendered_width_px = -1;
    bool rendered_3d = true;
    int rendered_height = -1;
    // Measured width of the widest material label at the last render. Not
    // implied by any of the above: a breakpoint change moves font_small (and so
    // every cell's text column) without moving width_px or the row height.
    int rendered_min_text = -1;

    // Auto-binding observer (observe AmsState slots_version subject)
    // Uses ObserverGuard for RAII lifecycle management
    // Note: slots_version is always bumped after slot_count changes, so one observer suffices
    ObserverGuard slots_version_observer;
    ObserverGuard current_slot_observer;

    // Re-entrancy guard: slots_version and current_slot observers can both fire
    // in one UpdateQueue batch. Without this, the second rebuild_bars would try
    // to reparent rows the first call already moved to its condemned_parent.
    bool rebuilding = false;
};

// Static registry for safe cleanup
static std::unordered_map<lv_obj_t*, AmsMiniStatusData*> s_registry;

static AmsMiniStatusData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// Forward declarations for internal functions
static void rebuild_bars(AmsMiniStatusData* data);
static void rebuild_spools(AmsMiniStatusData* data);
static void rebuild(AmsMiniStatusData* data);
static void sync_from_ams_state(AmsMiniStatusData* data);

// ============================================================================
// Internal helpers
// ============================================================================

/** Helper to style a SlotBarData using shared drawing utils */
static void apply_slot_style(SlotBarData* slot) {
    ams_draw::BarStyleParams params;
    params.color_rgb = slot->color_rgb;
    params.fill_pct = slot->fill_pct;
    params.is_present = slot->present;
    params.is_loaded = slot->loaded;
    params.has_error = slot->has_error;
    params.severity = slot->severity;
    ams_draw::style_slot_bar(slot->col, params, BAR_BORDER_RADIUS_PX);
}

/**
 * @brief Create or get a unit row container for multi-unit stacked layout
 */
static lv_obj_t* ensure_unit_row(AmsMiniStatusData* data, int unit_index) {
    UnitRowInfo* row = &data->unit_rows[unit_index];
    if (!row->row_container) {
        row->row_container = ams_draw::create_transparent_container(data->bars_container);
        lv_obj_set_flex_flow(row->row_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row->row_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row->row_container, theme_manager_get_spacing("space_xxs"),
                                    LV_PART_MAIN);
        lv_obj_set_width(row->row_container, LV_SIZE_CONTENT);
        lv_obj_set_style_flex_grow(row->row_container, 1, LV_PART_MAIN);
    }
    return row->row_container;
}

/**
 * @brief Compute effective max bar width based on available width
 *
 * When squeezed into a narrow cell, bars shrink to stay proportional.
 */
static int32_t effective_max_bar_width(const AmsMiniStatusData* data) {
    // width_px <= 0 is the struct's default before ui_ams_mini_status_set_width()
    // has ever run — set_slot_count()/set_slot_full() can trigger a rebuild in
    // that state, so this is a real, reachable path, not just a >=150 fallback.
    if (data->width_px <= 0)
        return MAX_BAR_WIDTH_PX; // Default: 16
    if (data->width_px < 100)
        return 8; // Tight layout: narrow bars
    if (data->width_px < 150)
        return 10; // Medium layout: slightly reduced
    return MAX_BAR_WIDTH_PX;
}

/**
 * @brief Compute effective max visible slots based on available width
 *
 * In narrow cells, reduce visible slots to avoid overflow/clipping.
 */
static int effective_max_visible(const AmsMiniStatusData* data) {
    // width_px <= 0 is the struct's default before set_width() has ever run
    // (see effective_max_bar_width) — falls through to the unclamped default,
    // same as width_px >= 100.
    if (data->width_px <= 0)
        return data->max_visible;
    if (data->width_px < 100)
        return std::min(data->max_visible, 6); // Tight: show max 6 bars
    // width_px >= 100 shows every configured slot: max_visible is already
    // clamped to <= AMS_MINI_STATUS_MAX_VISIBLE (8) by the setter, so a
    // second min(max_visible, 8) here was always a no-op.
    return data->max_visible;
}

/** Rebuild the bars based on slot_count, max_visible, and unit_count */
static void rebuild_bars(AmsMiniStatusData* data) {
    if (!data || !data->bars_container)
        return;
    if (data->rebuilding)
        return;
    // During lv_deinit, the default display is unset BEFORE screens are deleted
    // (lv_init.c: lv_display_set_default(NULL) precedes lv_cleanup_devices), yet
    // the destruct cascade still delivers LV_EVENT_SIZE_CHANGED here via flex
    // re-layout. With no active screen, condemn_row's lv_obj_create() returns
    // NULL and lv_obj_add_flag(NULL) crashes. Nothing to rebuild at teardown.
    if (!lv_screen_active())
        return;
    data->rebuilding = true;
    struct ResetGuard {
        AmsMiniStatusData* d;
        ~ResetGuard() {
            d->rebuilding = false;
        }
    } reset{data};

    // Guard: ensure overflow label has a valid font before any layout calculation.
    // A NULL font causes SEGV in lv_font_set_kerning during lv_obj_update_layout.
    if (data->overflow_label) {
        const lv_font_t* cur_font = lv_obj_get_style_text_font(data->overflow_label, LV_PART_MAIN);
        if (!cur_font) {
            spdlog::error("[AmsMiniStatus] NULL font on overflow label — applying fallback");
            const lv_font_t* fallback = theme_manager_get_font("font_xs");
            if (!fallback)
                fallback = &noto_sans_12;
            lv_obj_set_style_text_font(data->overflow_label, fallback, LV_PART_MAIN);
        }
    }

    int max_vis = effective_max_visible(data);
    int visible_count = std::min(data->slot_count, max_vis);
    int overflow_count = data->slot_count - visible_count;

    // Calculate dimensions from container
    // Don't force layout — read current dimensions. If they're not resolved
    // yet, LV_EVENT_SIZE_CHANGED will trigger a rebuild when they are.
    int32_t container_width = lv_obj_get_content_width(data->container);
    int32_t container_height = lv_obj_get_content_height(data->container);
    spdlog::trace("[AmsMiniStatus] rebuild_bars: container={}x{}, slots={}, width_px={}",
                  container_width, container_height, data->slot_count, data->width_px);

    // Skip if dimensions aren't resolved yet — size_changed will call us back.
    if (container_width < 20 && data->width_px == 0) {
        return;
    }

    int32_t gap = theme_manager_get_spacing("space_xxs");

    // Calculate bar height - use container height if data->height is 0 (XML-based responsive)
    int32_t effective_height = data->height > 0 ? data->height : container_height;
    if (effective_height < 20) {
        effective_height = 32; // Minimum fallback height
    }
    // Cap bars at 80% of container height so they don't fill the entire widget
    effective_height = effective_height * 80 / 100;

    // Cap bar height by aspect ratio — bars taller than 4× their width look awkward.
    // Compute max bar width first to get the aspect limit.
    int32_t max_bw = effective_max_bar_width(data);
    int32_t max_bar_height = max_bw * 4;
    if (effective_height > max_bar_height) {
        effective_height = max_bar_height;
    }

    bool is_multi_unit = (data->unit_count >= 2);

    // Collect condemned row containers here; batch-delete at end via a single
    // temporary parent to avoid cascading lv_obj_delete_async() calls that
    // corrupt LVGL's event linked list during rapid rebuild cycles.
    lv_obj_t* condemned_parent = nullptr;
    auto condemn_row = [&](lv_obj_t* row) {
        if (!row)
            return;
        if (!condemned_parent) {
            condemned_parent = lv_obj_create(lv_screen_active());
            if (!condemned_parent)
                return; // teardown: no active screen to host the condemned rows
            lv_obj_add_flag(condemned_parent, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(condemned_parent, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(condemned_parent, 0, 0);
        }
        lv_obj_set_parent(row, condemned_parent);
    };

    if (is_multi_unit) {
        // === Multi-unit stacked layout ===
        // bars_container becomes a column, each unit gets its own row

        lv_obj_set_flex_flow(data->bars_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(data->bars_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(data->bars_container, gap, LV_PART_MAIN);
        // Reset column padding (not used in column flow)
        lv_obj_set_style_pad_column(data->bars_container, 0, LV_PART_MAIN);

        // Count visible rows (units with bars within max visible)
        int visible_rows = 0;
        for (int u = 0; u < data->unit_count && u < 8; ++u) {
            int row_slots =
                std::min(data->unit_rows[u].slot_count, max_vis - data->unit_rows[u].first_slot);
            if (row_slots > 0)
                ++visible_rows;
        }
        if (visible_rows < 1)
            visible_rows = 1;

        // Reduce bar height per row to fit stacked rows
        int32_t row_gap_total = (visible_rows - 1) * gap;
        int32_t available_height = effective_height - row_gap_total;
        int32_t per_row_height = available_height / visible_rows;
        if (per_row_height < 12)
            per_row_height = 12; // Minimum per-row height

        int32_t bar_height =
            per_row_height - ams_draw::STATUS_LINE_HEIGHT_PX - ams_draw::STATUS_LINE_GAP_PX;
        if (bar_height < 6)
            bar_height = 6;

        // Condemn any row containers beyond current unit_count
        for (int u = data->unit_count; u < 8; ++u) {
            if (data->unit_rows[u].row_container) {
                condemn_row(data->unit_rows[u].row_container);
                data->unit_rows[u].row_container = nullptr;
            }
        }

        // Process each unit row
        for (int u = 0; u < data->unit_count && u < 8; ++u) {
            UnitRowInfo* row_info = &data->unit_rows[u];
            lv_obj_t* row = ensure_unit_row(data, u);

            // Calculate bar width for this row based on its slot count
            int row_slots = std::min(row_info->slot_count, max_vis - row_info->first_slot);
            if (row_slots < 0)
                row_slots = 0;

            int32_t max_bw = effective_max_bar_width(data);
            int32_t bar_width = ams_draw::calc_bar_width(container_width, row_slots, gap,
                                                         MIN_BAR_WIDTH_PX, max_bw, 90);

            // Create/update slots in this unit row
            for (int s = 0; s < row_info->slot_count; ++s) {
                int global_idx = row_info->first_slot + s;
                if (global_idx >= AMS_MINI_STATUS_MAX_VISIBLE)
                    break;

                SlotBarData* slot = &data->slots[global_idx];

                if (global_idx < visible_count) {
                    if (!slot->col.container) {
                        // Create new slot column in this row
                        slot->col = ams_draw::create_slot_column(row, bar_width, bar_height,
                                                                 BAR_BORDER_RADIUS_PX);
                    } else {
                        if (lv_obj_get_parent(slot->col.container) != row) {
                            // Reparent slot container into correct unit row
                            lv_obj_set_parent(slot->col.container, row);
                        }
                        // Update existing bar dimensions
                        lv_obj_set_width(slot->col.container, bar_width);
                        lv_obj_set_width(slot->col.bar_bg, bar_width);
                        lv_obj_set_width(slot->col.status_line, bar_width);
                    }

                    // Override to fill row height (multi-unit responsive mode)
                    lv_obj_set_height(slot->col.container, LV_PCT(100));
                    lv_obj_set_style_flex_grow(slot->col.bar_bg, 1, LV_PART_MAIN);

                    lv_obj_remove_flag(slot->col.container, LV_OBJ_FLAG_HIDDEN);
                    apply_slot_style(slot);
                } else {
                    if (slot->col.container) {
                        lv_obj_add_flag(slot->col.container, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }

            // Condemn row if no visible bars (hidden rows still consume flex gap)
            if (row_slots <= 0) {
                condemn_row(row);
                data->unit_rows[u].row_container = nullptr;
            }
        }

        spdlog::debug("[AmsMiniStatus] Multi-unit layout: {} units, {} total slots",
                      data->unit_count, visible_count);

    } else {
        // === Single-unit layout (original behavior) ===

        // Clean up any leftover unit row containers from a previous multi-unit state
        // Reparent any slot containers back to bars_container first
        for (int u = 0; u < 8; ++u) {
            if (data->unit_rows[u].row_container) {
                // Move children back to bars_container before deleting the row
                for (int i = 0; i < AMS_MINI_STATUS_MAX_VISIBLE; ++i) {
                    SlotBarData* slot = &data->slots[i];
                    if (slot->col.container && lv_obj_get_parent(slot->col.container) ==
                                                   data->unit_rows[u].row_container) {
                        lv_obj_set_parent(slot->col.container, data->bars_container);
                    }
                }
                condemn_row(data->unit_rows[u].row_container);
                data->unit_rows[u].row_container = nullptr;
            }
        }

        // Restore bars_container to row flex flow
        lv_obj_set_flex_flow(data->bars_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(data->bars_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(data->bars_container, gap, LV_PART_MAIN);
        lv_obj_set_style_pad_row(data->bars_container, 0, LV_PART_MAIN);

        int32_t bar_height =
            effective_height - ams_draw::STATUS_LINE_HEIGHT_PX - ams_draw::STATUS_LINE_GAP_PX;

        // Use 90% of container width for bars (leave 10% margin for centering)
        int32_t max_bw = effective_max_bar_width(data);
        int32_t bar_width = ams_draw::calc_bar_width(container_width, visible_count, gap,
                                                     MIN_BAR_WIDTH_PX, max_bw, 90);

        // Create/update bars
        for (int i = 0; i < AMS_MINI_STATUS_MAX_VISIBLE; i++) {
            SlotBarData* slot = &data->slots[i];

            if (i < visible_count) {
                if (!slot->col.container) {
                    slot->col = ams_draw::create_slot_column(data->bars_container, bar_width,
                                                             bar_height, BAR_BORDER_RADIUS_PX);
                } else {
                    // Update existing bar dimensions (size may have changed)
                    lv_obj_set_width(slot->col.container, bar_width);
                    lv_obj_set_height(slot->col.container, bar_height +
                                                               ams_draw::STATUS_LINE_HEIGHT_PX +
                                                               ams_draw::STATUS_LINE_GAP_PX);
                    lv_obj_set_width(slot->col.bar_bg, bar_width);
                    lv_obj_set_height(slot->col.bar_bg, bar_height);
                    lv_obj_set_width(slot->col.status_line, bar_width);
                }

                lv_obj_remove_flag(slot->col.container, LV_OBJ_FLAG_HIDDEN);
                apply_slot_style(slot);
            } else {
                if (slot->col.container) {
                    lv_obj_add_flag(slot->col.container, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Batch-delete all condemned rows via a single deferred deletion
    if (condemned_parent) {
        helix::ui::safe_delete_deferred(condemned_parent);
    }

    // Update overflow label
    if (data->overflow_label) {
        if (overflow_count > 0) {
            lv_label_set_text_fmt(data->overflow_label, "+%d", overflow_count);
            lv_obj_remove_flag(data->overflow_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(data->overflow_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Force layout recalculation for flex centering
    lv_obj_update_layout(data->container);

    // Hide entire widget if no slots
    if (data->slot_count <= 0) {
        lv_obj_add_flag(data->container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(data->container, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief The string a spool cell draws in its material label.
 *
 * Shared by the width measurement below and the render loop, so what the layout
 * reserves room for cannot drift from what actually gets drawn.
 */
static const char* spool_material_text(const SpoolCellData& cd) {
    if (!cd.present && !cd.assigned) {
        // Unassigned empty lane: name its purpose instead of showing "--",
        // matching the ams_slot material label (translated; "Empty" is UI copy,
        // not a material name).
        return lv_tr("Empty");
    }
    return cd.material.empty() ? "--" : cd.material.c_str(); // material: no i18n
}

/**
 * @brief Width of the widest material label this render is about to draw, in
 * the font it will be drawn in.
 *
 * This is the number the text column has to be at least as wide as, and it is
 * measured rather than assumed for two reasons: the label uses font_small,
 * which runs 10/11/12/16/18/20/26px across the tiers, and the strings vary
 * ("PLA", "PETG-CF", a translated "Empty"). The flat 34px this replaces was
 * right for the ~12px rung and about half of what "PLA" needs at 26px, which is
 * how the label ended up wrapping one glyph per line on the roomy tiers.
 *
 * Letter and line spacing are read off @p sc: both are inherited text styles
 * and the cells set neither, so the labels will be laid out with exactly these
 * values.
 *
 * @return 0 when there is nothing to draw. If the font token cannot be
 *         resolved (a theme that never initialized), falls back to the old flat
 *         34px rather than 0 - a wrong reservation still lays out, a zero one
 *         packs unreadable cells.
 */
static int measure_widest_material(const AmsMiniStatusData* data, lv_obj_t* sc) {
    if (!data || data->spool_cells.empty())
        return 0;
    const lv_font_t* font = theme_manager_get_font("font_small");
    if (!font || !sc)
        return 34;

    const int32_t letter_space = lv_obj_get_style_text_letter_space(sc, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(sc, LV_PART_MAIN);
    int widest = 0;
    for (const auto& cd : data->spool_cells) {
        lv_point_t size;
        lv_text_get_size(&size, spool_material_text(cd), font, letter_space, line_space,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > widest)
            widest = static_cast<int>(size.x);
    }
    return widest;
}

/**
 * @brief Render the wide spool view (width_px >= w_normal()).
 *
 * Lazily creates the horizontally-scrollable spool container, then renders one
 * cell per entry in data->spool_cells (spool graphic with lane badge, material
 * label, remaining percent), sizing cells to fit the available width.
 */
static void rebuild_spools(AmsMiniStatusData* data) {
    if (!data || !data->container)
        return;
    if (data->rebuilding)
        return;
    if (!lv_screen_active())
        return; // teardown guard (mirrors rebuild_bars)
    data->rebuilding = true;
    struct ResetGuard {
        AmsMiniStatusData* d;
        ~ResetGuard() {
            d->rebuilding = false;
        }
    } rg{data};

    // Resolve the current spool style once (matches ams_draw's /ams/spool_style read).
    const bool cur_3d =
        helix::Config::get_instance()->get<std::string>("/ams/spool_style", "3d") == "3d";

    if (!data->spools_container) {
        lv_obj_t* sc = lv_obj_create(data->container);
        lv_obj_set_name(sc, "ams_spools_container");
        lv_obj_set_width(sc, lv_pct(100));
        lv_obj_set_height(sc, lv_pct(100));
        lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(sc, theme_manager_get_spacing("space_xxs"), LV_PART_MAIN);
        // Card surface for the wide multi-cell view: layer the shared Card style (bg_color,
        // bg_opa, border, radius — reactive to theme changes) over the flex layout set above
        // rather than setting transparent locals. A small inset keeps cells off the corners.
        lv_obj_add_style(sc, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);
        lv_obj_set_style_pad_all(sc, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);
        lv_obj_add_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(sc, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_add_flag(sc, LV_OBJ_FLAG_EVENT_BUBBLE); // tap/long-press bubble to widget root
        data->spools_container = sc;
    }
    lv_obj_t* sc = data->spools_container;
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_HIDDEN);

    // Give each visible spool an equal share of the width. Subtract the
    // (visible-1) inter-cell gaps so they fit exactly — no sliver of the next
    // spool, and no scrollbar until there are more than `visible` spools. The
    // spool+label group is centered within each share.
    lv_obj_update_layout(sc);
    int avail_h = lv_obj_get_content_height(sc);
    if (avail_h <= 0)
        avail_h = data->height; // before layout resolves
    // Use the REAL laid-out content width (not data->width_px, which is the manager's
    // cell_w*colspan estimate and runs a few px wide → spurious scrollbar).
    int avail_w = lv_obj_get_content_width(sc);
    if (avail_w <= 0)
        avail_w = data->width_px; // before layout resolves
    int gap = theme_manager_get_spacing("space_xxs");
    // The widest material string this render will draw, in the font that will
    // draw it. Both the cell count and the text reservation below come from it,
    // so the divisor and the reservation cannot disagree the way two
    // independently-chosen constants did.
    const int min_text = measure_widest_material(data, sc);
    const int min_cell = min_spool_cell_w(min_text, gap);
    // How many spool cells fit across the row at min_cell each, capped to
    // the actual number of slots so a wide, sparsely-filled widget doesn't
    // reserve blank trailing columns for spools that don't exist.
    int visible = (avail_w + gap) / (min_cell + gap);
    visible = std::clamp(visible, 1, std::max(1, data->slot_count));
    // -2px safety so sub-pixel rounding can't tip the row into a spurious scrollbar
    // (a real scrollbar still appears when there are MORE than `visible` spools).
    int cell_px = (avail_w - (visible - 1) * gap - 2) / visible;
    if (cell_px < min_cell)
        cell_px = min_cell;
    // What create_spool_visual() adds around the graphic for the lane badge, so
    // the wrap object is this much wider than the spool size asked for.
    const int badge_margin = ams_draw::SPOOL_VISUAL_BADGE_MARGIN_PX;
    int spool_size = avail_h - 4; // square spool fits the row height
    if (spool_size > MAX_SPOOL_IMG_PX)
        spool_size = MAX_SPOOL_IMG_PX;
    // Reserve the measured text column from the cell, shrinking the spool if needed.
    // text_w is the EXACT leftover, so a cell's content == cell_px: the material/
    // percent never overflow (no chopped text, no scrollbar), and long names wrap
    // within text_w instead of being clipped.
    if (spool_size > cell_px - badge_margin - gap - min_text)
        spool_size = cell_px - badge_margin - gap - min_text;
    if (spool_size < MIN_SPOOL_IMG_PX)
        spool_size = MIN_SPOOL_IMG_PX;
    int text_w = cell_px - (spool_size + badge_margin) - gap;
    // Defensive only, and derived rather than flat: cell_px is floored at
    // min_cell, which IS MIN_SPOOL_IMG_PX + badge_margin + gap + min_text, so
    // the subtraction above already leaves min_text on the tightest cell there
    // is. Clamping to min_text keeps that a guarantee instead of a coincidence
    // the way a flat 20 did.
    if (text_w < min_text)
        text_w = min_text;

    // Dirty-check: the render is fully determined by the cell data, available
    // width, the spool style, and the measured text width. If none changed
    // since the last real render and the cells already exist on screen, skip
    // the costly clean+recreate (and its transient 2x canvas-memory peak). The
    // container was un-hidden above, so a bar->spool switch with identical data
    // still shows the existing cells.
    //
    // min_text is in the signature because it is a render input the other four
    // do not cover: a breakpoint change moves font_small without necessarily
    // moving width_px or avail_h, and that alone re-sizes every cell.
    bool unchanged = (data->rendered_width_px == data->width_px) && (data->rendered_3d == cur_3d) &&
                     (data->rendered_height == avail_h) && (data->rendered_min_text == min_text) &&
                     (data->rendered_cells == data->spool_cells) &&
                     (lv_obj_get_child_count(sc) > 0);
    if (unchanged)
        return; // identical render already on screen — skip churn

    // Safe teardown of previous cells (rebuild may run inside a queued callback).
    helix::ui::safe_clean_children(sc);

    int n = static_cast<int>(data->spool_cells.size());
    if (n <= 0) {
        lv_obj_add_flag(data->container, LV_OBJ_FLAG_HIDDEN);
        // Container is now empty — invalidate the cache so a later non-empty
        // render with otherwise-matching inputs isn't wrongly skipped.
        data->rendered_cells.clear();
        data->rendered_width_px = -1;
        data->rendered_height = -1;
        data->rendered_min_text = -1;
        return;
    }
    lv_obj_remove_flag(data->container, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < n; ++i) {
        const SpoolCellData& cd = data->spool_cells[i];

        lv_obj_t* cell = lv_obj_create(sc);
        char nm[32];
        snprintf(nm, sizeof(nm), "spool_cell_%d", i);
        lv_obj_set_name(cell, nm);
        lv_obj_set_size(cell, cell_px, lv_pct(100));
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(cell, theme_manager_get_spacing("space_xxs"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        // Bubble taps/long-presses to the widget root: tap -> AMS overlay,
        // long-press -> grid edit mode (matches the bar view).
        lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Spool wrap (square) holds spool visual + lane badge.
        lv_obj_t* wrap = lv_obj_create(cell);
        lv_obj_set_style_pad_all(wrap, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(wrap, 0, LV_PART_MAIN);
        lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(wrap, LV_OBJ_FLAG_EVENT_BUBBLE);
        // Empty-lane presentation, ported from apply_slot_status() in
        // ui_ams_slot.cpp so a lane reads the same on both surfaces:
        //   present              -> full-strength spool, material text
        //   ejected + assigned   -> spool KEPT, ghosted at LV_OPA_20, label
        //                           ghosted with it ("assigned, not present")
        //   ejected + unassigned -> spool hidden, dashed placeholder, "Empty"
        const bool ghosted = !cd.present && cd.assigned;
        ams_draw::SpoolVisual sv = ams_draw::create_spool_visual(wrap, spool_size);
        ams_draw::spool_visual_set_color(sv, lv_color_hex(cd.color_rgb));
        ams_draw::spool_visual_set_fill(sv, cd.fill_level);
        ams_draw::spool_visual_set_empty(sv, !cd.present && !cd.assigned);
        spool_visual_set_ghost_opa(sv, ghosted ? LV_OPA_20 : LV_OPA_COVER);
        lv_obj_t* badge =
            ams_draw::create_lane_badge(wrap, cd.lane_number, spool_size * 2 / 5, cd.active);
        if (badge) {
            snprintf(nm, sizeof(nm), "spool_badge_%d", i);
            lv_obj_set_name(badge, nm);
        }

        // Text column (vertically centered), material over percent.
        lv_obj_t* col = lv_obj_create(cell);
        lv_obj_set_width(col, text_w);
        lv_obj_set_height(col, lv_pct(100));
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(col, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(col, 0, LV_PART_MAIN);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t* mat = lv_label_create(col);
        snprintf(nm, sizeof(nm), "spool_material_%d", i);
        lv_obj_set_name(mat, nm);
        lv_obj_set_width(mat, text_w);
        lv_label_set_long_mode(mat, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(mat, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_flag(mat, LV_OBJ_FLAG_EVENT_BUBBLE);
        // Same helper measure_widest_material() sized text_w from, so the
        // string drawn here is the one the column was reserved for.
        lv_label_set_text(mat, spool_material_text(cd));
        const lv_font_t* fs = theme_manager_get_font("font_small");
        if (fs)
            lv_obj_set_style_text_font(mat, fs, LV_PART_MAIN);
        lv_obj_set_style_text_color(mat, theme_manager_get_color("text"), LV_PART_MAIN);
        lv_obj_set_style_text_opa(mat, ghosted ? LV_OPA_20 : LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* pct = lv_label_create(col);
        snprintf(nm, sizeof(nm), "spool_pct_%d", i);
        lv_obj_set_name(pct, nm);
        lv_obj_set_width(pct, text_w);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_flag(pct, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (cd.remaining_pct >= 0) {
            char p[16];
            snprintf(p, sizeof(p), "%d%%", cd.remaining_pct);
            lv_label_set_text(pct, p);
        } else {
            lv_label_set_text(pct, "");
        }
        const lv_font_t* fxs = theme_manager_get_font("font_xs");
        if (fxs)
            lv_obj_set_style_text_font(pct, fxs, LV_PART_MAIN);
        lv_obj_set_style_text_color(pct, theme_manager_get_color("text_muted"), LV_PART_MAIN);
        // Ghost the whole text group together — a full-strength percent beside a
        // dimmed material would read as two different lanes.
        lv_obj_set_style_text_opa(pct, ghosted ? LV_OPA_20 : LV_OPA_COVER, LV_PART_MAIN);
    }

    // Record the rendered signature so an identical subsequent sync can be skipped.
    data->rendered_cells = data->spool_cells;
    data->rendered_width_px = data->width_px;
    data->rendered_3d = cur_3d;
    data->rendered_height = avail_h;
    data->rendered_min_text = min_text;
}

/**
 * @brief Render dispatcher: select bar vs. spool mode by physical width.
 *
 * width_px >= w_normal() (the same "has room for two columns" threshold every
 * other home widget migrated to) selects the wide spool view; otherwise the
 * narrow bar view. The inactive mode's container is hidden so only one is
 * visible at a time.
 */
static void rebuild(AmsMiniStatusData* data) {
    if (!data)
        return;
    data->mode =
        (data->width_px >= helix::widget_size::w_normal()) ? AmsMiniMode::SPOOL : AmsMiniMode::BAR;
    if (data->mode == AmsMiniMode::SPOOL) {
        if (data->bars_container)
            lv_obj_add_flag(data->bars_container, LV_OBJ_FLAG_HIDDEN);
        rebuild_spools(data);
    } else {
        if (data->spools_container)
            lv_obj_add_flag(data->spools_container, LV_OBJ_FLAG_HIDDEN);
        if (data->bars_container)
            lv_obj_remove_flag(data->bars_container, LV_OBJ_FLAG_HIDDEN);
        rebuild_bars(data);
    }
}

/** Cleanup callback when widget is deleted */
static void on_delete(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        std::unique_ptr<AmsMiniStatusData> data(it->second);
        if (data) {
            data->slots_version_observer.reset();
            data->current_slot_observer.reset();
        }
        // data automatically freed when unique_ptr goes out of scope
        s_registry.erase(it);
    }
}

/** Size changed callback - rebuild bars when layout resolves */
static void on_size_changed(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto* data = get_data(obj);
    if (data && data->slot_count > 0) {
        rebuild(data);
    }
}

/** Click callback to open AMS panel (routes to overview for multi-unit) */
static void on_click(lv_event_t* e) {
    (void)e;
    spdlog::debug("[AmsMiniStatus] Clicked - navigating to AMS panel");
    navigate_to_ams_panel();
}

// ============================================================================
// Public API
// ============================================================================

lv_obj_t* ui_ams_mini_status_create(lv_obj_t* parent, int32_t height) {
    if (!parent || height <= 0) {
        return nullptr;
    }

    // Create main container
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);

    // Fill parent width, content height
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);

    // Use flex layout to center children (bars_container + overflow_label) horizontally
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(container, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);

    // Create user data
    auto data_ptr = std::make_unique<AmsMiniStatusData>();
    data_ptr->height = height;
    data_ptr->container = container;

    // Create bars container (holds the slot bars)
    data_ptr->bars_container = lv_obj_create(container);
    lv_obj_set_name(data_ptr->bars_container, "ams_bars_container");
    lv_obj_remove_flag(data_ptr->bars_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(data_ptr->bars_container, LV_OBJ_FLAG_EVENT_BUBBLE); // Pass clicks to parent
    lv_obj_set_style_bg_opa(data_ptr->bars_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(data_ptr->bars_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_ptr->bars_container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(data_ptr->bars_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data_ptr->bars_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(data_ptr->bars_container, theme_manager_get_spacing("space_xxs"),
                                LV_PART_MAIN);
    lv_obj_set_size(data_ptr->bars_container, LV_PCT(100), height);

    // Create overflow label (hidden by default) - use responsive font
    data_ptr->overflow_label = lv_label_create(container);
    lv_obj_add_flag(data_ptr->overflow_label, LV_OBJ_FLAG_EVENT_BUBBLE); // Pass clicks to parent
    lv_label_set_text(data_ptr->overflow_label, "+0");
    lv_obj_set_style_text_color(data_ptr->overflow_label, theme_manager_get_color("text_muted"),
                                LV_PART_MAIN);
    const lv_font_t* font_xs = theme_manager_get_font("font_xs");
    if (!font_xs)
        font_xs = &noto_sans_12;
    lv_obj_set_style_text_font(data_ptr->overflow_label, font_xs, LV_PART_MAIN);
    lv_obj_add_flag(data_ptr->overflow_label, LV_OBJ_FLAG_HIDDEN);

    // Register and set up cleanup
    AmsMiniStatusData* data = data_ptr.release();
    s_registry[container] = data;
    lv_obj_add_event_cb(container, on_delete, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(container, on_size_changed, LV_EVENT_SIZE_CHANGED, nullptr);

    // Make clickable to open AMS panel
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(container, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(container, on_click, LV_EVENT_CLICKED, nullptr);

    // Initially hidden (no slots)
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);

    // Auto-bind to AmsState: observe slots_version changes
    // slots_version is always bumped after slot_count changes, so one observer suffices
    // This makes the widget self-updating - no external wiring needed
    using helix::ui::observe_int_sync;

    lv_subject_t* slots_version_subject = AmsState::instance().get_slots_version_subject();
    if (slots_version_subject) {
        // Capture container (lv_obj_t*) instead of data pointer to prevent
        // use-after-free when deferred callback executes after widget deletion.
        // The registry lookup acts as a validity check. (fixes #83)
        data->slots_version_observer = observe_int_sync<lv_obj_t>(
            slots_version_subject, container, [](lv_obj_t* obj, int /* version */) {
                auto* d = get_data(obj);
                if (d)
                    sync_from_ams_state(d);
            });

        // Sync initial state if AMS already has data — defer so layout is
        // fully resolved before rebuild_bars queries container dimensions.
        lv_subject_t* slot_count_subject = AmsState::instance().get_slot_count_subject();
        if (slot_count_subject && lv_subject_get_int(slot_count_subject) > 0) {
            helix::ui::queue_update([container]() {
                auto* d = get_data(container);
                if (d)
                    sync_from_ams_state(d);
            });
        }
        spdlog::debug("[AmsMiniStatus] Auto-bound to AmsState slots_version subject");
    }

    // Observe current_slot as a re-sync trigger. The highlight itself now comes
    // from the per-slot active-loaded subject (slot_is_active_loaded above), and
    // every delta on that subject bumps slots_version, so the observer above is
    // what keeps the strip live; this one covers a current_slot move that leaves
    // the active-loaded flags untouched.
    lv_subject_t* current_slot_subject = AmsState::instance().get_current_slot_subject();
    if (current_slot_subject) {
        data->current_slot_observer = observe_int_sync<lv_obj_t>(current_slot_subject, container,
                                                                 [](lv_obj_t* obj, int /* slot */) {
                                                                     auto* d = get_data(obj);
                                                                     if (d)
                                                                         sync_from_ams_state(d);
                                                                 });
    }

    spdlog::trace("[AmsMiniStatus] Created (height={})", height);
    return container;
}

void ui_ams_mini_status_set_slot_count(lv_obj_t* obj, int slot_count) {
    auto* data = get_data(obj);
    if (!data)
        return;

    slot_count = std::max(0, slot_count);
    if (data->slot_count == slot_count)
        return;

    data->slot_count = slot_count;
    data->spool_cells.resize(slot_count);
    rebuild(data);

    spdlog::debug("[AmsMiniStatus] slot_count={}", slot_count);
}

void ui_ams_mini_status_set_max_visible(lv_obj_t* obj, int max_visible) {
    auto* data = get_data(obj);
    if (!data)
        return;

    max_visible = std::max(1, std::min(AMS_MINI_STATUS_MAX_VISIBLE, max_visible));
    if (data->max_visible == max_visible)
        return;

    data->max_visible = max_visible;
    rebuild(data);
}

void ui_ams_mini_status_set_slot_full(lv_obj_t* obj, int slot_index, uint32_t color_rgb,
                                      int fill_pct, bool present, const char* material,
                                      int remaining_pct) {
    auto* data = get_data(obj);
    if (!data || slot_index < 0)
        return;

    // Bar-mode cache (capped to the visible bar array). Preserve the existing
    // immediate restyle so bar rendering is unchanged.
    if (slot_index < AMS_MINI_STATUS_MAX_VISIBLE) {
        SlotBarData* slot = &data->slots[slot_index];
        slot->color_rgb = color_rgb;
        slot->fill_pct = std::clamp(fill_pct, 0, 100);
        slot->present = present;
        apply_slot_style(slot);
    }

    // Spool-mode cache (uncapped; multi-unit safe).
    if (static_cast<int>(data->spool_cells.size()) <= slot_index)
        data->spool_cells.resize(slot_index + 1);
    SpoolCellData& c = data->spool_cells[slot_index];
    c.color_rgb = color_rgb;
    c.fill_level = fill_level_from_pct(fill_pct);
    c.remaining_pct = remaining_pct;
    c.material = material ? material : "";
    c.present = present;
    // This programmatic path carries no Spoolman/brand handles, so a retained
    // material is the only "assigned" evidence it can offer (the AmsState path
    // in sync_from_ams_state() sees the full predicate).
    c.assigned = !c.material.empty();
    c.lane_number = slot_index + 1;
}

void ui_ams_mini_status_set_slot(lv_obj_t* obj, int slot_index, uint32_t color_rgb, int fill_pct,
                                 bool present) {
    ui_ams_mini_status_set_slot_full(obj, slot_index, color_rgb, fill_pct, present, "", -1);
}

/** Timer callback for deferred refresh */
static void deferred_refresh_cb(lv_timer_t* timer) {
    lv_obj_t* container = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    if (!container || !lv_is_initialized()) {
        lv_timer_delete(timer);
        return;
    }

    // Registry lookup validates the widget is still alive — on_delete() removes
    // the entry before freeing data, so get_data() returns nullptr for dead widgets.
    auto* data = get_data(container);
    if (data) {
        rebuild(data);
        spdlog::debug("[AmsMiniStatus] Deferred refresh complete");
    }
    lv_timer_delete(timer);
}

void ui_ams_mini_status_refresh(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (!data || !data->container)
        return;

    // Check if we have valid dimensions yet
    lv_obj_update_layout(data->bars_container);
    int32_t width = lv_obj_get_content_width(data->bars_container);

    if (width > 0) {
        // We have dimensions - rebuild immediately
        rebuild(data);
    } else {
        // Container still has zero width (likely just unhidden)
        // Defer refresh to next LVGL tick when layout will be recalculated
        lv_timer_t* timer = lv_timer_create(deferred_refresh_cb, 1, data->container);
        lv_timer_set_repeat_count(timer, 1);
        spdlog::debug("[AmsMiniStatus] Deferring refresh (container has zero width)");
    }
}

void ui_ams_mini_status_set_width(lv_obj_t* obj, int width_px) {
    auto* data = get_data(obj);
    if (!data)
        return;

    if (data->width_px == width_px)
        return;

    data->width_px = width_px;
    spdlog::debug("[AmsMiniStatus] Width set to {}px", width_px);

    // Rebuild if we already have slots (width affects mode, bar width, and spool count)
    if (data->slot_count > 0)
        rebuild(data);
}

bool ui_ams_mini_status_is_valid(lv_obj_t* obj) {
    auto* data = get_data(obj);
    return data && data->magic == AMS_MINI_STATUS_MAGIC;
}

// ============================================================================
// Auto-binding to AmsState
// ============================================================================

/**
 * @brief Sync widget state from AmsState backend
 *
 * Reads slot count and per-slot info from AmsState and updates the widget.
 * Called on initial creation and when slot_count changes.
 */
static void sync_from_ams_state(AmsMiniStatusData* data) {
    if (!data)
        return;

    AmsBackend* backend = AmsState::instance().get_backend();
    if (!backend) {
        // No backend - hide widget
        data->slot_count = 0;
        data->spool_cells.clear();
        rebuild(data);
        return;
    }

    int slot_count = lv_subject_get_int(AmsState::instance().get_slot_count_subject());
    data->slot_count = slot_count;

    // Get multi-unit info from system info
    AmsSystemInfo info = backend->get_system_info();
    data->unit_count = static_cast<int>(info.units.size());
    for (int u = 0; u < data->unit_count && u < 8; ++u) {
        data->unit_rows[u].first_slot = info.units[u].first_slot_global_index;
        data->unit_rows[u].slot_count = info.units[u].slot_count;
    }
    // Clear any stale unit rows beyond current count
    for (int u = data->unit_count; u < 8; ++u) {
        data->unit_rows[u].first_slot = 0;
        data->unit_rows[u].slot_count = 0;
    }

    // Populate both render-mode caches from backend slot info. The bar cache is
    // capped at AMS_MINI_STATUS_MAX_VISIBLE; the spool cache is uncapped (sized to
    // slot_count) so the wide spool view sees every lane on multi-unit systems.
    data->spool_cells.assign(slot_count, SpoolCellData{});
    for (int i = 0; i < slot_count; ++i) {
        SlotInfo slot = backend->get_slot_info(i);
        // Fill is read from this slots_version snapshot on purpose. This widget
        // rebuilds all bars/cells wholesale on every sync; a per-slot fill
        // subject observer (as in the ams_slot widget) would race that rebuild
        // and touch just-recreated objects — the #705/#776 family. Semantics are
        // still unified: fill_percent_from_slot uses SlotInfo::display_fill_pct,
        // with the SHARED default min_pct so a present-but-spent lane keeps the
        // same visible sliver the overview's mini bars give it (passing 0 here
        // made the identical lane render empty on one surface and not the
        // other). -1 = "no data" → floored to 0 so the bar/spool renders empty
        // rather than a phantom fill (was 100/full for weightless slots).
        int fill_pct = ams_draw::fill_percent_from_slot(slot);
        if (fill_pct < 0)
            fill_pct = 0;
        int rem = -1;
        float p = slot.get_remaining_percent();
        if (p >= 0.0f)
            rem = static_cast<int>(p + 0.5f);

        const bool active = slot_is_active_loaded(i);
        // "Assigned" = the lane still carries identity after an eject — the
        // override is deliberately NOT cleared (#1071). Same predicate as
        // apply_slot_status() in ui_ams_slot.cpp; brand/spool_name cover
        // IFS-style backends with a user override but no Spoolman id.
        const bool assigned = slot.spoolman_id > 0 || !slot.material.empty() ||
                              !slot.brand.empty() || !slot.spool_name.empty();

        // Bar-mode cache (capped at MAX_VISIBLE).
        if (i < AMS_MINI_STATUS_MAX_VISIBLE) {
            SlotBarData* slot_bar = &data->slots[i];
            slot_bar->color_rgb = slot.color_rgb;
            slot_bar->fill_pct = fill_pct;
            slot_bar->present = slot.is_present();
            slot_bar->loaded = active;
            slot_bar->has_error = (slot.status == SlotStatus::BLOCKED || slot.error.has_value());
            slot_bar->severity = slot.error.has_value() ? slot.error->severity : SlotError::INFO;
        }

        // Spool-mode cache (uncapped).
        SpoolCellData& c = data->spool_cells[i];
        c.color_rgb = slot.color_rgb;
        c.fill_level = fill_level_from_pct(fill_pct);
        c.remaining_pct = rem;
        c.material = slot.material;
        c.present = slot.is_present();
        c.lane_number = i + 1;
        c.active = active;
        c.assigned = assigned;
    }

    rebuild(data);
    spdlog::trace("[AmsMiniStatus] Synced from AmsState: {} slots", slot_count);
}

// Observer callbacks migrated to lambda observers in ui_ams_mini_status_create()

// ============================================================================
// XML Widget Registration
// ============================================================================

/**
 * @brief XML create callback - creates widget with responsive sizing
 */
static void* ui_ams_mini_status_xml_create(lv_xml_parser_state_t* state, const char** /* attrs */) {
    void* parent = lv_xml_state_get_parent(state);

    // Create main container that fills parent
    lv_obj_t* container = lv_obj_create((lv_obj_t*)parent);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);

    // Fill parent (parent must have a definite height for bars to render correctly)
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));

    // Use flex layout to center children
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(container, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);

    // Create user data with height=0 (will be set dynamically)
    auto data_ptr = std::make_unique<AmsMiniStatusData>();
    data_ptr->height = 0; // Will be calculated from parent
    data_ptr->container = container;

    // Create bars container (holds the slot bars)
    data_ptr->bars_container = lv_obj_create(container);
    lv_obj_set_name(data_ptr->bars_container, "ams_bars_container");
    lv_obj_remove_flag(data_ptr->bars_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(data_ptr->bars_container, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_bg_opa(data_ptr->bars_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(data_ptr->bars_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_ptr->bars_container, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(data_ptr->bars_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(data_ptr->bars_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(data_ptr->bars_container, theme_manager_get_spacing("space_xxs"),
                                LV_PART_MAIN);
    // Fill parent height (responsive)
    lv_obj_set_size(data_ptr->bars_container, LV_SIZE_CONTENT, LV_PCT(100));

    // Create overflow label (hidden by default)
    data_ptr->overflow_label = lv_label_create(container);
    lv_obj_add_flag(data_ptr->overflow_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_text(data_ptr->overflow_label, "+0");
    lv_obj_set_style_text_color(data_ptr->overflow_label, theme_manager_get_color("text_muted"),
                                LV_PART_MAIN);
    const lv_font_t* font_xs = theme_manager_get_font("font_xs");
    if (!font_xs)
        font_xs = &noto_sans_12;
    lv_obj_set_style_text_font(data_ptr->overflow_label, font_xs, LV_PART_MAIN);
    lv_obj_add_flag(data_ptr->overflow_label, LV_OBJ_FLAG_HIDDEN);

    // Register and set up cleanup
    AmsMiniStatusData* data = data_ptr.release();
    s_registry[container] = data;
    lv_obj_add_event_cb(container, on_delete, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(container, on_size_changed, LV_EVENT_SIZE_CHANGED, nullptr);

    // Make clickable to open AMS panel
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(container, LV_OPA_70, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(container, on_click, LV_EVENT_CLICKED, nullptr);

    // Initially hidden (no slots)
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);

    // Auto-bind to AmsState: observe slots_version changes
    // slots_version is always bumped after slot_count changes, so one observer suffices
    using helix::ui::observe_int_sync;

    lv_subject_t* slots_version_subject = AmsState::instance().get_slots_version_subject();
    if (slots_version_subject) {
        // Capture container (lv_obj_t*) instead of data pointer to prevent
        // use-after-free when deferred callback executes after widget deletion.
        // The registry lookup acts as a validity check. (fixes #83)
        data->slots_version_observer = observe_int_sync<lv_obj_t>(
            slots_version_subject, container, [](lv_obj_t* obj, int /* version */) {
                auto* d = get_data(obj);
                if (d)
                    sync_from_ams_state(d);
            });

        // Sync initial state if AMS already has data — defer so layout is
        // fully resolved before rebuild_bars queries container dimensions.
        lv_subject_t* slot_count_subject = AmsState::instance().get_slot_count_subject();
        if (slot_count_subject && lv_subject_get_int(slot_count_subject) > 0) {
            helix::ui::queue_update([container]() {
                auto* d = get_data(container);
                if (d)
                    sync_from_ams_state(d);
            });
        }
    }

    // Observe current_slot as a re-sync trigger. The highlight itself now comes
    // from the per-slot active-loaded subject (slot_is_active_loaded above), and
    // every delta on that subject bumps slots_version, so the observer above is
    // what keeps the strip live; this one covers a current_slot move that leaves
    // the active-loaded flags untouched.
    lv_subject_t* current_slot_subject = AmsState::instance().get_current_slot_subject();
    if (current_slot_subject) {
        data->current_slot_observer = observe_int_sync<lv_obj_t>(current_slot_subject, container,
                                                                 [](lv_obj_t* obj, int /* slot */) {
                                                                     auto* d = get_data(obj);
                                                                     if (d)
                                                                         sync_from_ams_state(d);
                                                                 });
    }

    spdlog::trace("[AmsMiniStatus] Created via XML (responsive height)");
    return container;
}

/**
 * @brief XML apply callback - handles XML attributes
 */
static void ui_ams_mini_status_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    // Apply standard object attributes (width, height, align, etc.)
    lv_xml_obj_apply(state, attrs);
}

void ui_ams_mini_status_init(void) {
    lv_xml_register_widget("ams_mini_status", ui_ams_mini_status_xml_create,
                           ui_ams_mini_status_xml_apply);
    spdlog::trace("[AmsMiniStatus] Registered ams_mini_status XML widget");
}
