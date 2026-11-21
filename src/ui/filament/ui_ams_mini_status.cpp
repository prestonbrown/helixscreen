// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_mini_status.h"

#include "ui_ams_lane_bar.h"
#include "ui_ams_lane_spool.h"
#include "ui_bypass_spool_widget.h"
#include "ui_fonts.h"
#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"
#include "ui_row_text.h"
#include "ui_spool_canvas.h"
#include "ui_text.h"
#include "ui_utils.h"

#include "ams_backend.h"
#include "ams_lane_state.h"
#include "ams_shared_spool_lanes.h"
#include "ams_state.h"
#include "core/observer_factory.h"
#include "core/observer_guard.h"
#include "core/update_queue.h"
#include "display_numbering.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "panel_widget_size.h"
#include "settings_manager.h"
#include "theme_manager.h"
#include "ui/ams_drawing_utils.h"
#include "widget_data_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ============================================================================
// Layout constants
// ============================================================================

/** Minimum bar width in pixels (prevents bars from becoming invisible) */
static constexpr int32_t kMinBarWidthPx = 3;

/** Maximum bar width in pixels (prevents bars from becoming too wide) */
static constexpr int32_t kMaxBarWidthPx = 24;

/** Bar width cap when the caller never reported a width (non-home callers) */
static constexpr int32_t kDefaultMaxBarWidthPx = 16;

/**
 * Cell width per unit of bar width cap. Four lanes then span about half the
 * cell, and the cap scales with the tier instead of stepping at fixed pixel
 * widths. More lanes shrink below the cap through calc_bar_width().
 */
static constexpr int32_t kBarWidthDivisor = 8;

/**
 * Smallest spool graphic the wide view will draw (px).
 *
 * A floor on a vector drawing, not on type: below roughly this size the spool
 * rings and the lane badge stop reading as a spool at all, whatever the tier's
 * font ladder is doing. Deliberately not derived from a spacing or font token -
 * see min_spool_cell_w() for what IS derived from it.
 */
static constexpr int kMinSpoolImgPx = 24;

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
static constexpr int kMaxSpoolImgPx = 56;

/**
 * Largest spool graphic a `fill` tray draws (px): a host that hands the tray a
 * whole column wants the spools to carry it, so they grow past
 * kMaxSpoolImgPx, to the ams_slot_spool_size scale of the full AMS panel and
 * a little past it.
 */
static constexpr int kFillMaxSpoolImgPx = 120;

/// The lane number badge on a spool of @p spool_size: two fifths of it, up to
/// the home tray's largest spool, so a fill tray's larger spools keep a badge
/// that reads without covering the spool.
static int lane_badge_px(int spool_size) {
    return std::min(spool_size, kMaxSpoolImgPx) * 2 / 5;
}

/**
 * Minimum width of one spool cell in the wide spool view (px), and the divisor
 * used to derive how many spool cells fit across a row:
 * visible = (avail_w + gap) / (min_spool_cell_w(...) + gap).
 *
 * Derived rather than chosen: a cell is [spool graphic][gap][text column], so
 * the narrowest useful cell is the smallest spool that still reads as a spool,
 * plus the badge slack the ams_lane_spool widget adds around it, plus the gap,
 * plus enough room for the widest material string actually about to be drawn.
 *
 * Deriving it is what keeps the two halves of the layout consistent. They were
 * two independent constants before - a flat 60px divisor here and a flat 34px
 * text reservation below - so on tiers where font_small is 16-26px rather than
 * the ~12px they were calibrated against, the divisor packed in cells too
 * narrow for the reservation the same function then tried to honor. The cell
 * won, the reservation lost, and LV_LABEL_LONG_MODE_WRAP broke "PLA" into one glyph
 * per line. Sharing one `min_text` makes that disagreement unrepresentable.
 */
static int min_spool_cell_w(int min_text, int gap) {
    return kMinSpoolImgPx + helix::ui::kAmsLaneSpoolBadgeMarginPx + gap + min_text;
}

// ============================================================================
// Per-widget user data
// ============================================================================

/** Magic number to identify ams_mini_status widgets ("AMS1" as ASCII) */
static constexpr uint32_t kAmsMiniStatusMagic = 0x414D5331;

/** Render mode: narrow bars vs. wide spool cells (selected by width_px) */
enum class AmsMiniMode { BAR, SPOOL };

/**
 * @brief Per-lane data for the wide spool render mode
 *
 * Only what the strip still renders by hand: the text column and the lane
 * badge. The spool graphic, its fill, ghosting and error dot render from the
 * per-slot subjects inside each cell's embedded ams_lane_spool widget, so
 * they carry no cache here.
 */
struct SpoolCellData {
    helix::ui::LaneState lane_state = helix::ui::LaneState::Empty;
    std::string material;   // "" => label reads "--"
    int remaining_pct = -1; // actual % remaining; -1 = unknown (blank label)
    bool active = false;    // actively-loaded lane (success-colored badge)
    int slot = -1;          // global slot index; -1 = the cell's own position

    bool operator==(const SpoolCellData& o) const {
        return lane_state == o.lane_state && material == o.material &&
               remaining_pct == o.remaining_pct && active == o.active && slot == o.slot;
    }
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
 * @brief What the strip draws for the bypass: whether it is on the strip at all
 * (bypass_node_visible_for, the AMS panel's rule), the external spool on it,
 * and whether it is the node feeding the toolhead.
 */
namespace {
struct BypassView {
    bool shown = false;
    std::optional<helix::SlotInfo> spool; // the external spool; none = hollow
    bool active = false;

    bool operator==(const BypassView& o) const {
        if (shown != o.shown || active != o.active || spool.has_value() != o.spool.has_value())
            return false;
        if (!spool)
            return true;
        return spool->color_rgb == o.spool->color_rgb &&
               spool->multi_color_hexes == o.spool->multi_color_hexes &&
               spool->material == o.spool->material;
    }
    bool operator!=(const BypassView& o) const {
        return !(*this == o);
    }
};
} // namespace

/**
 * @brief User data stored on each ams_mini_status widget
 */
struct AmsMiniStatusData {
    uint32_t magic = kAmsMiniStatusMagic;
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

    // Draw the spool view on its own card surface (XML attribute "card").
    // Hosts that already supply a surface set it false.
    bool card = true;

    // Size the spools from the height as well as the width (XML attribute
    // "fill"): lanes wrap into the rows that give the largest spool, up to
    // kFillMaxSpoolImgPx. Off, one row of spools capped at kMaxSpoolImgPx.
    bool fill = false;

    // Where the lane bars sit along the widget's width (XML attribute
    // "bars_align": "center", the default, or "start").
    lv_flex_align_t bars_align = LV_FLEX_ALIGN_CENTER;

    // Child objects
    lv_obj_t* container = nullptr;        // Main container
    lv_obj_t* bars_container = nullptr;   // Container for slot bars
    lv_obj_t* spools_row = nullptr;       // Spool view: lane scroller + pinned bypass
    lv_obj_t* spools_container = nullptr; // The lanes' horizontal scroller, in spools_row
    lv_obj_t* overflow_label = nullptr;   // "+N" overflow indicator

    // Bar-mode lane pool: one ams_lane_bar widget per visible slot, created
    // lazily, reparented between unit rows and resized as the measured layout
    // moves. Rendering inside each bar comes from the AmsState per-slot subjects.
    lv_obj_t* lane_bars[AMS_MINI_STATUS_MAX_VISIBLE] = {};

    // Per-lane data for the spool render mode (sized to slot_count; uncapped for multi-unit)
    std::vector<SpoolCellData> spool_cells;

    // Pooled spool cells, one per entry of spool_cells. Created once per lane
    // and updated in place on every sync; only a lane count change adds or
    // removes cells.
    std::vector<lv_obj_t*> spool_cell_objs;
    // The row size the spool cells were last sized against.
    int spools_built_w = 0;
    int spools_built_h = 0;
    std::vector<int> spool_cell_obj_slots; // slot each pooled cell was built for

    // Slots inside an absent unit (a box that is not on the bus): neither drawn
    // nor counted in the "+N" overflow.
    std::vector<int> absent_slots;

    // The bypass, drawn past a divider after every unit's lanes. In bar mode it
    // stands in its own column beside bars_container, so on a stacked
    // multi-unit strip it spans every row rather than joining one unit's.
    BypassView bypass;
    lv_obj_t* bypass_column = nullptr;  // bar mode: divider + bar
    lv_obj_t* bypass_bar = nullptr;     // bar mode: unbound ams_lane_bar
    lv_obj_t* bypass_divider = nullptr; // spool mode: divider past the lanes' scroller
    lv_obj_t* bypass_cell = nullptr;    // spool mode: spool + material, pinned at the right

    // Auto-binding observer (observe AmsState slots_version subject)
    // Uses ObserverGuard for RAII lifecycle management
    // Note: slots_version is always bumped after slot_count changes, so one observer suffices
    ObserverGuard slots_version_observer;
    ObserverGuard current_slot_observer;
    // The bypass's inputs: none of them bumps slots_version.
    ObserverGuard supports_bypass_observer;
    ObserverGuard bypass_active_observer;
    ObserverGuard external_spool_observer;
    ObserverGuard always_show_bypass_observer;

    // Re-entrancy guard: slots_version and current_slot observers can both fire
    // in one UpdateQueue batch. Without this, the second rebuild_bars would try
    // to reparent rows the first call already moved to its condemned_parent.
    bool rebuilding = false;
    bool synced = false; // the widget shows what the last sync_from_ams_state() read
};

// Static registry for safe cleanup
static helix::WidgetDataRegistry<AmsMiniStatusData> s_registry;

static AmsMiniStatusData* get_data(lv_obj_t* obj) {
    return s_registry.get(obj);
}

// Forward declarations for internal functions
static void rebuild_bars(AmsMiniStatusData* data);
static void rebuild_spools(AmsMiniStatusData* data);
static void rebuild(AmsMiniStatusData* data);
static void sync_from_ams_state(AmsMiniStatusData* data);

// ============================================================================
// Internal helpers
// ============================================================================

/** The XML flex-place name of the strip's bars_align. */
static const char* bars_align_name(lv_flex_align_t align) {
    return align == LV_FLEX_ALIGN_START ? "start" : "center";
}

/**
 * @brief Create or get a unit row container for multi-unit stacked layout
 */
static lv_obj_t* ensure_unit_row(AmsMiniStatusData* data, int unit_index) {
    UnitRowInfo* row = &data->unit_rows[unit_index];
    if (!row->row_container) {
        const char* attrs[] = {"main_place", bars_align_name(data->bars_align), nullptr};
        row->row_container =
            static_cast<lv_obj_t*>(lv_xml_create(data->bars_container, "ams_mini_unit_row", attrs));
        if (!row->row_container)
            spdlog::error("[AmsMiniStatus] ams_mini_unit_row creation failed for unit {}",
                          unit_index);
    }
    return row->row_container;
}

/**
 * @brief Compute effective max bar width based on available width
 *
 * Proportional to the cell, so the same lane count fills the same fraction of
 * its box on every panel.
 */
static int32_t effective_max_bar_width(const AmsMiniStatusData* data) {
    // width_px <= 0 is the struct's default before ui_ams_mini_status_set_width()
    // has ever run - set_slot_count()/set_slot_label() can trigger a rebuild in
    // that state, and callers outside the home grid never set it.
    if (data->width_px <= 0)
        return kDefaultMaxBarWidthPx;
    return std::clamp(data->width_px / kBarWidthDivisor, kMinBarWidthPx, kMaxBarWidthPx);
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
    if (data->width_px < 100) {
        // Tight: show max 6 bars, the bypass's among them
        return std::min(data->max_visible, data->bypass.shown ? 5 : 6);
    }
    // width_px >= 100 shows every configured slot: max_visible is already
    // clamped to <= AMS_MINI_STATUS_MAX_VISIBLE (8) by the setter, so a
    // second min(max_visible, 8) here was always a no-op.
    return data->max_visible;
}

/** Create one pooled lane bar bound to its slot's subjects. */
static lv_obj_t* create_lane_bar(lv_obj_t* parent, int slot_index, int32_t bar_width,
                                 int32_t bar_height) {
    const std::string idx = std::to_string(slot_index);
    const std::string w = std::to_string(bar_width);
    const std::string h = std::to_string(bar_height);
    const char* attrs[] = {"slot_index", idx.c_str(), "bar_width", w.c_str(),
                           "bar_height", h.c_str(),   nullptr};
    lv_obj_t* bar = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_lane_bar", attrs));
    if (!bar)
        spdlog::error("[AmsMiniStatus] ams_lane_bar creation failed for slot {}", slot_index);
    return bar;
}

/** The material a bypass cell names: the external spool's, or "External" with none. */
static const char* bypass_label_text(const BypassView& bv) {
    if (bv.spool && !bv.spool->material.empty())
        return bv.spool->material.c_str();
    return lv_tr("External");
}

/** What the divider and its gap take from the lanes' width, beyond one more bar and gap. */
static int32_t bypass_width_overhead(const AmsMiniStatusData* data) {
    return data->bypass.shown ? theme_manager_get_spacing("space_xs") + 1 : 0;
}

/**
 * @brief Bar mode's bypass: a divider and one unbound lane bar in a column
 * beside bars_container, standing as tall as every unit row together.
 *
 * @param bar_width The lanes' bar width (the narrowest row's, stacked)
 * @param bar_height The bar body's height, status line excluded
 * @param stretch The body also takes the status line's room while that line is
 *        hidden, as a stacked row's lanes do
 */
static void layout_bypass_bar(AmsMiniStatusData* data, int32_t bar_width, int32_t bar_height,
                              bool stretch) {
    if (!data->bypass.shown || data->slot_count <= 0) {
        if (data->bypass_column)
            lv_obj_set_hidden(data->bypass_column, true);
        return;
    }
    if (!data->bypass_column) {
        const std::string w = std::to_string(bar_width);
        const std::string h = std::to_string(bar_height);
        const char* attrs[] = {"bar_width", w.c_str(), "bar_height", h.c_str(), nullptr};
        data->bypass_column =
            static_cast<lv_obj_t*>(lv_xml_create(data->container, "ams_mini_bypass_column", attrs));
        if (!data->bypass_column) {
            spdlog::error("[AmsMiniStatus] ams_mini_bypass_column creation failed");
            return;
        }
        data->bypass_bar = lv_obj_find_by_name(data->bypass_column, "bypass_bar");
    }
    // Content-tall: the widget's row centers it as it centers the lanes, so a bar
    // as tall as theirs stands level with them.
    lv_obj_t* col = data->bypass_column;
    // Past the "+N" overflow, which counts the unit's lanes.
    if (lv_obj_get_index(col) != static_cast<int32_t>(lv_obj_get_child_count(data->container)) - 1)
        lv_obj_move_to_index(col, -1);

    const int32_t line_room = ams_draw::kStatusLineHeightPx + ams_draw::kStatusLineGapPx;
    if (lv_obj_t* div = lv_obj_find_by_name(col, "bypass_divider")) {
        // Inset a tenth top and bottom of the bar body.
        const int32_t body = stretch ? bar_height + line_room : bar_height;
        lv_obj_set_height(div, std::max<int32_t>(1, body * 8 / 10));
        lv_obj_set_style_margin_bottom(div, (stretch ? 0 : line_room) + body / 10, LV_PART_MAIN);
    }
    if (data->bypass_bar) {
        helix::ui::ams_lane_bar_resize(data->bypass_bar, bar_width, bar_height);
        if (lv_obj_t* bg = lv_obj_find_by_name(data->bypass_bar, "bar_bg"))
            lv_obj_set_style_flex_grow(bg, stretch ? 1 : 0, LV_PART_MAIN);
        const auto& spool = data->bypass.spool;
        helix::ui::ams_lane_bar_paint_unbound(data->bypass_bar, spool.has_value(),
                                              spool ? spool->color_rgb : 0, data->bypass.active);
    }
    lv_obj_set_hidden(col, false);
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
    int overflow_count = 0;
    for (int i = visible_count; i < data->slot_count; ++i) {
        if (std::find(data->absent_slots.begin(), data->absent_slots.end(), i) ==
            data->absent_slots.end()) {
            ++overflow_count;
        }
    }

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
    int32_t bypass_bar_width = max_bw;
    int32_t bypass_bar_height = 0;

    // Collect condemned row containers here; batch-delete at end via a single
    // temporary parent to avoid cascading lv_obj_delete_async() calls that
    // corrupt LVGL's event linked list during rapid rebuild cycles.
    lv_obj_t* condemned_parent = nullptr;
    auto condemn_row = [&](lv_obj_t* row) {
        if (!row)
            return;
        // The pool outlives rows: a bar left inside a condemned row is freed
        // with it while lane_bars[] still points at it.
        for (lv_obj_t* bar : data->lane_bars) {
            if (bar && lv_obj_get_parent(bar) == row) {
                lv_obj_set_parent(bar, data->bars_container);
                lv_obj_set_hidden(bar, true);
            }
        }
        if (!condemned_parent) {
            condemned_parent = lv_obj_create(lv_screen_active());
            if (!condemned_parent)
                return; // teardown: no active screen to host the condemned rows
            lv_obj_set_hidden(condemned_parent, true);
            lv_obj_set_scrollable(condemned_parent, false);
            lv_obj_set_size(condemned_parent, 0, 0);
        }
        lv_obj_set_parent(row, condemned_parent);
    };

    if (is_multi_unit) {
        // === Multi-unit stacked layout ===
        // bars_container becomes a column, each unit gets its own row

        lv_obj_set_flex_flow(data->bars_container, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(data->bars_container, LV_FLEX_ALIGN_CENTER, data->bars_align,
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
            per_row_height - ams_draw::kStatusLineHeightPx - ams_draw::kStatusLineGapPx;
        if (bar_height < 6)
            bar_height = 6;
        // The bypass belongs to no unit: it stands as tall as the rows together,
        // which grow to fill the container, from the top row's bar to the bottom
        // row's status line.
        const int32_t rows_height = container_height >= 20 ? container_height : effective_height;
        bypass_bar_height = std::max<int32_t>(6, rows_height - ams_draw::kStatusLineHeightPx -
                                                     ams_draw::kStatusLineGapPx);

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
            const int bypass_bars = data->bypass.shown ? 1 : 0;
            int32_t bar_width =
                ams_draw::calc_bar_width(container_width - bypass_width_overhead(data),
                                         row_slots + bypass_bars, gap, kMinBarWidthPx, max_bw, 90);
            if (row_slots > 0)
                bypass_bar_width = std::min(bypass_bar_width, bar_width);

            // Create/update slots in this unit row
            for (int s = 0; s < row_info->slot_count; ++s) {
                int global_idx = row_info->first_slot + s;
                if (global_idx >= AMS_MINI_STATUS_MAX_VISIBLE)
                    break;

                lv_obj_t* bar = data->lane_bars[global_idx];

                if (global_idx < visible_count) {
                    if (!bar) {
                        bar = create_lane_bar(row, global_idx, bar_width, bar_height);
                        data->lane_bars[global_idx] = bar;
                    } else {
                        if (lv_obj_get_parent(bar) != row) {
                            // Reparent the pooled bar into its unit row
                            lv_obj_set_parent(bar, row);
                        }
                        helix::ui::ams_lane_bar_resize(bar, bar_width, bar_height);
                    }

                    // Override to fill row height (multi-unit responsive mode)
                    lv_obj_set_height(bar, LV_PCT(100));
                    lv_obj_set_style_flex_grow(lv_obj_find_by_name(bar, "bar_bg"), 1, LV_PART_MAIN);

                    lv_obj_set_hidden(bar, false);
                } else if (bar) {
                    lv_obj_set_hidden(bar, true);
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
        for (int u = 0; u < 8; ++u) {
            if (data->unit_rows[u].row_container) {
                condemn_row(data->unit_rows[u].row_container);
                data->unit_rows[u].row_container = nullptr;
            }
        }

        // Restore bars_container to row flex flow
        lv_obj_set_flex_flow(data->bars_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(data->bars_container, data->bars_align, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(data->bars_container, gap, LV_PART_MAIN);
        lv_obj_set_style_pad_row(data->bars_container, 0, LV_PART_MAIN);

        int32_t bar_height =
            effective_height - ams_draw::kStatusLineHeightPx - ams_draw::kStatusLineGapPx;

        // Use 90% of container width for bars (leave 10% margin for centering);
        // the bypass is one more bar of the same width, past its divider.
        int32_t max_bw = effective_max_bar_width(data);
        const int bypass_bars = data->bypass.shown ? 1 : 0;
        int32_t bar_width =
            ams_draw::calc_bar_width(container_width - bypass_width_overhead(data),
                                     visible_count + bypass_bars, gap, kMinBarWidthPx, max_bw, 90);
        bypass_bar_width = bar_width;
        bypass_bar_height = bar_height;

        // Create/update bars
        for (int i = 0; i < AMS_MINI_STATUS_MAX_VISIBLE; i++) {
            lv_obj_t* bar = data->lane_bars[i];

            if (i < visible_count) {
                if (!bar) {
                    bar = create_lane_bar(data->bars_container, i, bar_width, bar_height);
                    data->lane_bars[i] = bar;
                } else {
                    // Update existing bar dimensions (size may have changed)
                    helix::ui::ams_lane_bar_resize(bar, bar_width, bar_height);
                }

                lv_obj_set_hidden(bar, false);
            } else if (bar) {
                lv_obj_set_hidden(bar, true);
            }
        }
    }

    layout_bypass_bar(data, bypass_bar_width, bypass_bar_height, is_multi_unit);

    // Batch-delete all condemned rows via a single deferred deletion
    if (condemned_parent) {
        helix::ui::safe_delete_deferred(condemned_parent);
    }

    // Update overflow label
    if (data->overflow_label) {
        if (overflow_count > 0) {
            lv_label_set_text_fmt(data->overflow_label, "+%d", overflow_count);
            lv_obj_set_hidden(data->overflow_label, false);
        } else {
            lv_obj_set_hidden(data->overflow_label, true);
        }
    }

    // Force layout recalculation for flex centering
    lv_obj_update_layout(data->container);

    // Hide entire widget if no slots
    if (!helix::ui::ams_mini_status_shows_lanes(data->slot_count,
                                                static_cast<int>(data->spool_cells.size()))) {
        lv_obj_set_hidden(data->container, true);
    } else {
        lv_obj_set_hidden(data->container, false);
    }
}

/**
 * @brief Add or remove the spool container's card surface per the "card" attribute.
 *
 * Default (card="true"): layer the shared Card style (bg_color, bg_opa, border, radius -
 * reactive to theme changes) over the flex layout rather than setting transparent locals,
 * plus a small inset that keeps cells off the card's corners.
 *
 * card="false": the host already paints a surface behind the widget - the home grid draws
 * one fused card behind a whole group of widgets - and a second card inside it reads as a
 * visible box within a box. Plain lv_obj containers are transparent with zero padding by
 * default (StyleRole::ObjBase, applied by the theme), so dropping the Card style and the
 * inset leaves the spool cells sitting directly on the host's surface.
 *
 * Safe to call on a container that already has the style applied - the attribute can be
 * parsed after the container exists, so toggling must add or remove, not only build.
 */
static void apply_spools_card_surface(AmsMiniStatusData* data) {
    if (!data || !data->spools_row)
        return;
    lv_obj_t* sc = data->spools_row;
    lv_style_t* card_style = ThemeManager::instance().get_style(StyleRole::Card);
    if (data->card) {
        lv_obj_add_style(sc, card_style, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sc, theme_manager_get_spacing("space_xs"), LV_PART_MAIN);
    } else {
        lv_obj_remove_style(sc, card_style, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sc, 0, LV_PART_MAIN);
    }
}

/**
 * @brief The string a spool cell draws in its material label.
 *
 * The spool family's shared rule (helix::ui::lane_material_text), shared by
 * the width measurement below and the cell update loop, so what the layout
 * reserves room for cannot drift from what actually gets drawn.
 */
static const char* spool_material_text(const SpoolCellData& cd) {
    return helix::ui::lane_material_text(cd.lane_state, cd.material.c_str());
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
    auto measure = [&](const char* text) {
        lv_point_t size;
        lv_text_get_size(&size, text, font, letter_space, line_space, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        widest = std::max(widest, static_cast<int>(size.x));
    };
    for (const auto& cd : data->spool_cells)
        measure(spool_material_text(cd));
    if (data->bypass.shown)
        measure(bypass_label_text(data->bypass));
    return widest;
}

/**
 * @brief Narrowest text column worth keeping in a squeezed cell (px).
 *
 * Measured rather than chosen, for the same reason min_spool_cell_w()'s text
 * term is: font_small runs 10-26px across the tiers, so any flat number is
 * wrong at one end of that ladder. A column narrower than an already-shortened
 * name plus its ellipsis shows punctuation and no material, at which point the
 * column is costing width and returning nothing - the cell is better off
 * spending that width on the spool.
 *
 * Falls back to the same flat 34px measure_widest_material() uses when the font
 * token cannot be resolved.
 */
static int min_readable_text_w(lv_obj_t* sc) {
    const lv_font_t* font = theme_manager_get_font("font_small");
    if (!font || !sc)
        return 34;
    lv_point_t size;
    lv_text_get_size(&size, "ABC...", font, lv_obj_get_style_text_letter_space(sc, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(sc, LV_PART_MAIN), LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return static_cast<int>(size.x);
}

/// Detach and batch-delete every pooled cell (the strip went empty; rebuild
/// may run inside a queued callback, so deletion stays deferred and batched).
static void drop_spool_cells(AmsMiniStatusData* data) {
    if (data->spool_cell_objs.empty())
        return;
    lv_obj_t* dead = lv_obj_create(lv_screen_active());
    if (dead) {
        lv_obj_set_hidden(dead, true);
        lv_obj_set_scrollable(dead, false);
        lv_obj_set_size(dead, 0, 0);
        for (lv_obj_t* cell : data->spool_cell_objs)
            if (cell)
                lv_obj_set_parent(cell, dead);
        helix::ui::safe_delete_deferred(dead);
    }
    data->spool_cell_objs.clear();
    data->spool_cell_obj_slots.clear();
}

/**
 * @brief Create one pooled spool cell for lane @p slot_index.
 *
 * A transparent wrap holding the lane's embedded ams_lane_spool widget
 * (created before the badge so the badge renders above it) plus the
 * lane-number badge, and the material/percent text column. Created once per
 * lane; rebuild_spools() updates everything data- or layout-dependent in
 * place, so creation bakes only the structure and the names.
 */
static int spool_badge_number(int slot_index) {
    helix::AmsBackend* backend = helix::AmsState::instance().get_backend();
    return backend ? helix::spool_display_number(*backend, slot_index)
                   : helix::ui::lane_number(slot_index);
}

static lv_obj_t* create_spool_cell(lv_obj_t* parent, int slot_index, int spool_size) {
    const std::string slot = std::to_string(slot_index);
    const std::string sz = std::to_string(spool_size);
    const std::string badge = std::to_string(lane_badge_px(spool_size));
    const char* attrs[] = {"slot",       slot.c_str(),  "spool_size", sz.c_str(),
                           "badge_size", badge.c_str(), nullptr};
    lv_obj_t* cell = static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_mini_spool_cell", attrs));
    if (!cell) {
        spdlog::error("[AmsMiniStatus] ams_mini_spool_cell creation failed for slot {}",
                      slot_index);
        return nullptr;
    }
    // The lane's spool number, so a lane sharing another's spool wears that
    // spool's number; the plain 1-based lane number when no backend answers.
    char nm[32];
    snprintf(nm, sizeof(nm), "spool_badge_%d", slot_index);
    ams_draw::set_lane_badge_number(lv_obj_find_by_name(cell, nm), spool_badge_number(slot_index));
    return cell;
}

/**
 * @brief Spool mode's bypass: a divider and one more cell pinned past the
 * lanes' scroller, sized like a lane's, its spool painted from the external
 * spool and its label naming the material, or "External" with none.
 */
static void layout_bypass_cell(AmsMiniStatusData* data, lv_obj_t* row, int cell_px, int spool_size,
                               int text_w, bool stacked, bool ellipsize, bool show_pct,
                               int scrollbar_room) {
    const bool show = data->bypass.shown && !data->spool_cells.empty();
    if (!show) {
        if (data->bypass_divider)
            lv_obj_set_hidden(data->bypass_divider, true);
        if (data->bypass_cell)
            lv_obj_set_hidden(data->bypass_cell, true);
        return;
    }
    const int badge_margin = helix::ui::kAmsLaneSpoolBadgeMarginPx;
    if (!data->bypass_cell) {
        data->bypass_divider =
            static_cast<lv_obj_t*>(lv_xml_create(row, "ams_mini_bypass_divider", nullptr));
        const std::string sz = std::to_string(spool_size);
        const char* attrs[] = {"spool_size", sz.c_str(), nullptr};
        data->bypass_cell =
            static_cast<lv_obj_t*>(lv_xml_create(row, "ams_mini_bypass_cell", attrs));
        if (!data->bypass_divider || !data->bypass_cell) {
            spdlog::error("[AmsMiniStatus] bypass cell creation failed");
            return;
        }
    }
    lv_obj_set_hidden(data->bypass_divider, false);

    lv_obj_t* cell = data->bypass_cell;
    lv_obj_set_size(cell, cell_px, lv_pct(100));
    // Level with the lane cells, which stand above the scrollbar's room.
    lv_obj_set_style_pad_bottom(cell, scrollbar_room, LV_PART_MAIN);
    lv_obj_set_flex_flow(cell, stacked ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_ROW);

    // The same footprint a lane's spool takes with its badge slack, so the
    // bypass spool lines up with theirs.
    lv_obj_t* wrap = lv_obj_find_by_name(cell, "bypass_spool_wrap");
    if (wrap) {
        lv_obj_set_size(wrap, spool_size + badge_margin, spool_size + badge_margin);
        ams_draw::set_active_ring(wrap, data->bypass.active);
    }
    if (lv_obj_t* canvas = lv_obj_find_by_name(cell, "bypass_spool")) {
        ui_spool_canvas_set_size(canvas, spool_size);
        const auto& spool = data->bypass.spool;
        if (spool) {
            helix::ui::spool_canvas_set_filament(canvas, *spool);
            ui_spool_canvas_set_fill_level(canvas, 1.0f);
        } else {
            ui_spool_canvas_set_color(canvas, lv_color_hex(helix::ui::kBypassNoSpoolRgb));
            ui_spool_canvas_set_fill_level(canvas, 0.0f);
        }
        ui_spool_canvas_redraw(canvas);
    }

    lv_obj_t* col = lv_obj_find_by_name(cell, "bypass_text");
    if (col) {
        lv_obj_set_width(col, text_w);
        lv_obj_set_height(col, stacked ? LV_SIZE_CONTENT : lv_pct(100));
        lv_obj_set_hidden(col, text_w <= 0);
    }
    if (lv_obj_t* mat = lv_obj_find_by_name(cell, "bypass_material")) {
        lv_obj_set_width(mat, text_w);
        lv_label_set_long_mode(mat, ellipsize ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_max_lines(mat, ellipsize ? 1 : 0);
    }
    helix::ui::set_row_label_text(cell, "bypass_material", bypass_label_text(data->bypass));
    if (lv_obj_t* pct = lv_obj_find_by_name(cell, "bypass_pct"))
        lv_obj_set_hidden(pct, !show_pct);
    lv_obj_set_hidden(cell, false);
}

/** The height a horizontal scrollbar takes along @p scroller's bottom edge, with @p gap above it.
 */
static int scrollbar_room_px(lv_obj_t* scroller, int gap) {
    return static_cast<int>(lv_obj_get_style_width(scroller, LV_PART_SCROLLBAR) +
                            lv_obj_get_style_pad_bottom(scroller, LV_PART_SCROLLBAR)) +
           gap;
}

static void on_spools_size_changed(lv_event_t* e) {
    lv_obj_t* sc = lv_event_get_target_obj(e);
    auto* data = get_data(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
    const int w = lv_obj_get_content_width(sc);
    const int h = lv_obj_get_content_height(sc);
    // A content-sized host leaves the row no height of its own (h <= 0), and
    // the cells are then sized from the create-time height instead.
    if (data && data->mode == AmsMiniMode::SPOOL && w > 0 &&
        (w != data->spools_built_w || (h > 0 && h != data->spools_built_h)))
        rebuild_spools(data);
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

    const bool cur_3d = helix::ui::ams_spool_style_3d();

    if (!data->spools_row) {
        lv_obj_t* row =
            static_cast<lv_obj_t*>(lv_xml_create(data->container, "ams_mini_spools_row", nullptr));
        lv_obj_t* sc = row ? lv_obj_find_by_name(row, "ams_spools_container") : nullptr;
        if (!sc) {
            spdlog::error("[AmsMiniStatus] ams_mini_spools_row creation failed");
            if (row)
                helix::ui::safe_delete_deferred(row);
            return;
        }
        data->spools_row = row;
        data->spools_container = sc;
        apply_spools_card_surface(data);
        // The container's own SIZE_CHANGED fires before its children are laid
        // out, so a rebuild there reads this row at its old size, or at zero
        // and the width_px hint, which runs a few px wide. The row's real size
        // arrives here.
        lv_obj_add_event_cb(row, on_spools_size_changed, LV_EVENT_SIZE_CHANGED, data->container);
    }
    lv_obj_t* row = data->spools_row;
    lv_obj_t* sc = data->spools_container;
    lv_obj_set_hidden(row, false);
    lv_obj_set_hidden(sc, false);
    // A fill tray wraps its lanes into rows (ams_mini_spools_row's fill_rows).
    lv_obj_set_state(sc, LV_STATE_USER_1, data->fill);

    // Give each lane an equal share of the width, gaps subtracted so the row
    // fits exactly. The spool+label group is centered within each share.
    lv_obj_update_layout(row);
    int avail_h = lv_obj_get_content_height(row);
    if (avail_h <= 0)
        avail_h = data->height; // before layout resolves
    // Use the REAL laid-out content width (not data->width_px, which is the manager's
    // cell_w*colspan estimate and runs a few px wide → spurious scrollbar).
    int avail_w = lv_obj_get_content_width(row);
    if (avail_w <= 0)
        avail_w = data->width_px; // before layout resolves
    data->spools_built_w = avail_w;
    data->spools_built_h = avail_h;
    int gap = theme_manager_get_spacing("space_xxs");
    // What the ams_lane_spool widget adds around the graphic for the lane
    // badge, so a cell is this much wider than the spool size asked for.
    const int badge_margin = helix::ui::kAmsLaneSpoolBadgeMarginPx;
    // The bypass takes one more cell's share, and its divider and gap come off
    // the row; the lanes' scroller gets the rest.
    const bool bypass_cell = data->bypass.shown;
    const int lanes = std::max(1, data->slot_count) + (bypass_cell ? 1 : 0);
    if (bypass_cell)
        avail_w -= 1 + gap;
    // The widest material string this render will draw, in the font that will
    // draw it. Both the cell count and the text reservation come from it, so the
    // divisor and the reservation cannot disagree the way two independently
    // chosen constants did.
    const int min_text = measure_widest_material(data, sc);
    const int min_cell = min_spool_cell_w(min_text, gap);
    // Below a spool and its badge a cell no longer identifies its lane.
    const int floor_cell = kMinSpoolImgPx + badge_margin;
    // Every lane's equal share of the row. -2px so sub-pixel rounding cannot
    // tip a row that fits into a spurious scrollbar.
    int share = (avail_w - (lanes - 1) * gap - 2) / lanes;
    const lv_font_t* mat_font = theme_manager_get_font("font_small");
    const int mat_h = mat_font ? static_cast<int>(lv_font_get_line_height(mat_font)) : 14;
    // A fill tray wraps the lanes into the rows that give the largest spool,
    // the name over each; the bypass keeps its own column past the lanes.
    const int max_spool = data->fill ? kFillMaxSpoolImgPx : kMaxSpoolImgPx;
    int fill_row_h = 0;
    if (data->fill) {
        const int lane_count = std::max(1, data->slot_count);
        int best = -1;
        for (int rows = 1; rows <= lane_count; ++rows) {
            const int cols = (lane_count + rows - 1) / rows + (bypass_cell ? 1 : 0);
            const int cell_w = (avail_w - (cols - 1) * gap - 2) / cols;
            const int row_h = (avail_h - (rows - 1) * gap) / rows;
            const int spool =
                std::min({row_h - badge_margin - mat_h - gap, cell_w - badge_margin, max_spool});
            if (spool > best) {
                best = spool;
                share = cell_w;
                fill_row_h = row_h;
            }
        }
        avail_h = fill_row_h;
    }
    // Every sizing branch below widens a cell past its share only when the share
    // is under the floor, so that is exactly when the lanes scroll. The scrollbar
    // then runs along the scroller's bottom, and the cells stand above it: its
    // room comes off the height every spool is sized against.
    const bool lanes_scroll = share < floor_cell;
    const int scrollbar_room = lanes_scroll ? scrollbar_room_px(sc, gap) : 0;
    lv_obj_set_style_pad_bottom(sc, scrollbar_room, LV_PART_MAIN);
    avail_h -= scrollbar_room;

    // The name goes above the spool or beside it, whichever leaves the larger
    // spool, and above on a tie. A tall row maxes the spool either way, so it
    // stacks; a short row with width for the name in full keeps a full-height
    // spool beside it. Width decides only whether the row scrolls.
    const lv_font_t* pct_font = theme_manager_get_font("font_xs");
    const int pct_h = pct_font ? static_cast<int>(lv_font_get_line_height(pct_font)) : 12;
    // The spool's wrap is badge_margin taller than the spool, and the cell
    // spaces the wrap and the text block by one gap.
    const int floor_h = kMinSpoolImgPx + badge_margin + gap;
    const int row_spool = std::min(avail_h - 4, max_spool); // square spool fits the row
    const int stacked_cell = std::max(share, floor_cell);
    auto stacked_spool_with = [&](int text_block) {
        return std::min(
            {avail_h - badge_margin - text_block - gap, max_spool, stacked_cell - badge_margin});
    };
    // The spool already draws how full it is, so the percent line under the
    // name stays only when it costs the spool nothing.
    const bool stacked_pct = stacked_spool_with(mat_h + pct_h) == stacked_spool_with(mat_h);
    const int stacked_spool = stacked_spool_with(mat_h);
    const int beside_spool =
        share >= min_cell ? std::min(row_spool, share - badge_margin - gap - min_text) : 0;
    // Whether the name fits above the spool is the row's question, scrollbar or
    // not: the scrollbar's room shrinks the spool, never flips a stacked row to
    // the wide beside cells, which would show fewer lanes.
    const bool stacked =
        avail_h + scrollbar_room >= floor_h + mat_h && stacked_spool >= beside_spool;

    int cell_px = 0;
    int text_w = 0;
    bool ellipsize = false;
    int spool_size = row_spool;
    if (stacked) {
        // Every lane gets its share, down to the floor; past that the row
        // scrolls at the floor. The name owns the full cell width and is
        // shortened rather than wrapped when the share is narrower than it.
        cell_px = stacked_cell;
        spool_size = stacked_spool;
        text_w = cell_px;
        ellipsize = true;
    } else if (share >= floor_cell && share < min_cell) {
        // Beside the spool, every lane, the name shortened into what is left
        // next to the spool or dropped when too little is left to read.
        cell_px = share;
        spool_size = std::max(std::min(spool_size, cell_px - badge_margin), kMinSpoolImgPx);
        const int leftover = cell_px - (spool_size + badge_margin) - gap;
        if (leftover >= min_readable_text_w(sc)) {
            text_w = leftover;
            ellipsize = true;
        }
    } else {
        // Beside the spool with the widest name in full: every lane's share
        // when that holds it, else a scrolling row of cells that do. The
        // column is the exact leftover, so the material and percent never
        // overflow the cell and a long name wraps inside it.
        cell_px = std::max(share, min_cell);
        spool_size = std::min(spool_size, cell_px - badge_margin - gap - min_text);
        text_w = std::max(cell_px - (std::max(spool_size, kMinSpoolImgPx) + badge_margin) - gap,
                          min_text);
    }
    spool_size = std::max(spool_size, kMinSpoolImgPx);

    const int n = static_cast<int>(data->spool_cells.size());
    if (!helix::ui::ams_mini_status_shows_lanes(data->slot_count, n)) {
        lv_obj_set_hidden(data->container, true);
        drop_spool_cells(data);
        return;
    }
    lv_obj_set_hidden(data->container, false);

    // Pool to size: cells come and go only with the lane count. Extras are
    // reparented to a hidden donor and batch-deleted at the end (a rebuild can
    // run inside a queued callback, and cascading per-cell deletes corrupt
    // LVGL's event list during rapid rebuild cycles).
    lv_obj_t* condemned = nullptr;
    auto condemn = [&](lv_obj_t* o) {
        if (!o)
            return;
        if (!condemned) {
            condemned = lv_obj_create(lv_screen_active());
            if (!condemned)
                return;
            lv_obj_set_hidden(condemned, true);
            lv_obj_set_scrollable(condemned, false);
            lv_obj_set_size(condemned, 0, 0);
        }
        lv_obj_set_parent(o, condemned);
    };
    if (static_cast<int>(data->spool_cell_objs.size()) > n) {
        for (int i = n; i < static_cast<int>(data->spool_cell_objs.size()); ++i)
            condemn(data->spool_cell_objs[i]);
        data->spool_cell_objs.resize(n);
    } else {
        data->spool_cell_objs.resize(n, nullptr);
    }
    data->spool_cell_obj_slots.resize(n, -1);

    char nm[32];
    auto cell_part = [&](lv_obj_t* root, const char* fmt, int idx) -> lv_obj_t* {
        snprintf(nm, sizeof(nm), fmt, idx);
        return lv_obj_find_by_name(root, nm);
    };

    for (int i = 0; i < n; ++i) {
        const SpoolCellData& cd = data->spool_cells[i];
        // A cell binds its slot's subjects when it is built, so a pooled cell
        // now standing for a different slot (a box left the chain) is rebuilt.
        const int slot = cd.slot >= 0 ? cd.slot : i;
        if (data->spool_cell_objs[i] && data->spool_cell_obj_slots[i] != slot) {
            condemn(data->spool_cell_objs[i]);
            data->spool_cell_objs[i] = nullptr;
        }
        if (!data->spool_cell_objs[i]) {
            data->spool_cell_objs[i] = create_spool_cell(sc, slot, spool_size);
            data->spool_cell_obj_slots[i] = slot;
        }
        lv_obj_t* cell = data->spool_cell_objs[i];
        if (!cell)
            continue;

        // Stacked cells put the name over the spool (the text column is the
        // cell's second child); short rows sit it alongside.
        lv_obj_set_size(cell, cell_px, data->fill ? fill_row_h : lv_pct(100));
        // A column-flow cell spaces its children by pad_row, which the
        // component sets to the same space_xxs `gap` the stacked height budget
        // counts once between the spool and the text block.
        lv_obj_set_flex_flow(cell, stacked ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_ROW);

        // The graphic renders itself from the per-slot subjects; its size is
        // the one thing the cell still owes it (no-op unless the measured
        // size or the spool style moved).
        if (lv_obj_t* lspool = lv_obj_find_by_name(cell, "lane_spool"))
            helix::ui::ams_lane_spool_set_size(lspool, spool_size);

        if (lv_obj_t* badge = cell_part(cell, "spool_badge_%d", slot)) {
            const int badge_size = lane_badge_px(spool_size);
            lv_obj_set_size(badge, badge_size, badge_size);
            ams_draw::set_lane_badge_active(badge, cd.active);
            ams_draw::set_lane_badge_number(badge, spool_badge_number(slot));
        }

        // Text column (vertically centered), material over percent. A cell
        // squeezed hard enough to fit every lane has no column at all: the
        // spool and its lane badge carry the row on their own.
        const bool show_text = text_w > 0;
        const bool show_pct = !stacked || stacked_pct;
        // Ghost the whole text group together: a full-strength percent beside a
        // dimmed material would read as two different lanes.
        const bool ghosted = cd.lane_state == helix::ui::LaneState::Ghosted;
        lv_obj_t* col = cell_part(cell, "spool_text_%d", slot);
        if (col) {
            lv_obj_set_width(col, text_w);
            // Stacked, the column is one line under the spool and must not claim the
            // row's full height or it pushes the spool out of the cell.
            lv_obj_set_height(col, stacked ? LV_SIZE_CONTENT : lv_pct(100));
            if (show_text)
                lv_obj_set_hidden(col, false);
            else
                lv_obj_set_hidden(col, true);
        }

        lv_obj_t* mat = cell_part(cell, "spool_material_%d", slot);
        if (mat) {
            lv_obj_set_width(mat, text_w);
            // A squeezed column is sized for a shortened name, not the widest one,
            // so it ellipsizes on one line rather than wrapping a name across lines
            // the row has no height for.
            lv_label_set_long_mode(mat,
                                   ellipsize ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP);
            lv_label_set_max_lines(mat, ellipsize ? 1 : 0);
            lv_obj_set_state(mat, LV_STATE_USER_1, ghosted);
            // Same helper measure_widest_material() sized text_w from, so the
            // string drawn here is the one the column was reserved for.
            helix::ui::set_row_label_text(cell, nm, spool_material_text(cd));
        }

        lv_obj_t* pct = cell_part(cell, "spool_pct_%d", slot);
        if (pct) {
            lv_obj_set_width(pct, text_w);
            char pb[16] = "";
            if (cd.remaining_pct >= 0)
                snprintf(pb, sizeof(pb), "%d%%", cd.remaining_pct);
            helix::ui::set_row_label_text(cell, nm, pb);
            lv_obj_set_state(pct, LV_STATE_USER_1, ghosted);
            if (show_text && show_pct)
                lv_obj_set_hidden(pct, false);
            else
                lv_obj_set_hidden(pct, true);
        }
    }

    layout_bypass_cell(data, row, cell_px, spool_size, text_w, stacked, ellipsize,
                       !stacked || stacked_pct, scrollbar_room);

    if (condemned)
        helix::ui::safe_delete_deferred(condemned);
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
            lv_obj_set_hidden(data->bars_container, true);
        if (data->bypass_column)
            lv_obj_set_hidden(data->bypass_column, true);
        rebuild_spools(data);
    } else {
        if (data->spools_row)
            lv_obj_set_hidden(data->spools_row, true);
        if (data->spools_container)
            lv_obj_set_hidden(data->spools_container, true);
        if (data->bars_container)
            lv_obj_set_hidden(data->bars_container, false);
        rebuild_bars(data);
    }
}

/** Cleanup callback when widget is deleted */
static void on_delete(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    if (AmsMiniStatusData* registered = s_registry.get(obj)) {
        std::unique_ptr<AmsMiniStatusData> data(registered);
        if (data) {
            data->slots_version_observer.reset();
            data->current_slot_observer.reset();
            data->supports_bypass_observer.reset();
            data->bypass_active_observer.reset();
            data->external_spool_observer.reset();
            data->always_show_bypass_observer.reset();
        }
        // data automatically freed when unique_ptr goes out of scope
        s_registry.take(obj);
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

/** Tap on the strip (ams_mini_status_frame): open the AMS panel, or the overview for multi-unit */
static void on_click(lv_event_t* e) {
    (void)e;
    spdlog::debug("[AmsMiniStatus] Clicked - navigating to AMS panel");
    navigate_to_ams_panel();
}

/** Observer handler: re-read AmsState, if the widget still stands. */
static void resync_widget(lv_obj_t* obj, int /*value*/) {
    if (auto* d = get_data(obj))
        sync_from_ams_state(d);
}

/**
 * @brief Follow the bypass's inputs. None of them bumps slots_version: the
 * always-show setting changes nothing the backend publishes, and engaging
 * bypass touches no slot.
 */
static void bind_bypass_observers(AmsMiniStatusData* data, lv_obj_t* container) {
    using helix::ui::observe;
    auto& ams = helix::AmsState::instance();
    data->supports_bypass_observer = observe<int>(ams.get_supports_bypass_subject(), container,
                                                  resync_widget, ams.get_subjects_lifetime());
    data->bypass_active_observer = observe<int>(ams.get_bypass_active_subject(), container,
                                                resync_widget, ams.get_subjects_lifetime());
    data->external_spool_observer = observe<int>(ams.get_external_spool_color_subject(), container,
                                                 resync_widget, ams.get_subjects_lifetime());
    auto& settings = helix::SettingsManager::instance();
    data->always_show_bypass_observer =
        observe<int>(settings.subject_ams_always_show_bypass_spool(), container, resync_widget,
                     settings.get_subjects_lifetime());
}

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Build the strip from the ams_mini_status_frame component and bind it
 * to AmsState. Shared by the C++ factory and the XML create callback, which
 * differ only in the frame's sizing props and the fixed @p height (0 = sized
 * from the parent).
 */
static lv_obj_t* create_mini_status(lv_obj_t* parent, int32_t height, const char** frame_attrs) {
    lv_obj_t* container =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "ams_mini_status_frame", frame_attrs));
    if (!container) {
        spdlog::error("[AmsMiniStatus] ams_mini_status_frame creation failed");
        return nullptr;
    }

    auto data_ptr = std::make_unique<AmsMiniStatusData>();
    data_ptr->height = height;
    data_ptr->container = container;
    data_ptr->bars_container = lv_obj_find_by_name(container, "ams_bars_container");
    data_ptr->overflow_label = lv_obj_find_by_name(container, "ams_overflow_label");

    // Register and set up cleanup
    AmsMiniStatusData* data = data_ptr.release();
    s_registry.set(container, data);
    lv_obj_add_event_cb(container, on_delete, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(container, on_size_changed, LV_EVENT_SIZE_CHANGED, nullptr);

    // Auto-bind to AmsState: observe slots_version changes
    // slots_version is always bumped after slot_count changes, so one observer suffices
    // This makes the widget self-updating - no external wiring needed
    using helix::ui::observe;

    lv_subject_t* slots_version_subject = helix::AmsState::instance().get_slots_version_subject();
    if (slots_version_subject) {
        // Capture container (lv_obj_t*) instead of data pointer to prevent
        // use-after-free when deferred callback executes after widget deletion.
        // The registry lookup acts as a validity check. (#83)
        data->slots_version_observer =
            observe<int>(slots_version_subject, container, resync_widget,
                         helix::AmsState::instance().get_subjects_lifetime());

        // Sync now, so the frame that first shows the widget draws its lanes.
        // Layout has not sized the container yet: rebuild_bars waits for that,
        // and on_size_changed rebuilds once it lands, still before that frame.
        lv_subject_t* slot_count_subject = helix::AmsState::instance().get_slot_count_subject();
        if (slot_count_subject && lv_subject_get_int(slot_count_subject) > 0) {
            sync_from_ams_state(data);
        }
    }

    // Observe current_slot as a re-sync trigger. The highlight itself comes
    // from the per-slot active-loaded subject (read in sync_from_ams_state), and
    // every delta on that subject bumps slots_version, so the observer above is
    // what keeps the strip live; this one covers a current_slot move that leaves
    // the active-loaded flags untouched.
    lv_subject_t* current_slot_subject = helix::AmsState::instance().get_current_slot_subject();
    if (current_slot_subject) {
        data->current_slot_observer =
            observe<int>(current_slot_subject, container, resync_widget,
                         helix::AmsState::instance().get_subjects_lifetime());
    }

    bind_bypass_observers(data, container);
    return container;
}

lv_obj_t* ui_ams_mini_status_create(lv_obj_t* parent, int32_t height) {
    if (!parent || height <= 0) {
        return nullptr;
    }
    // Content-tall around bars of the given height, which grow into the row
    // the bypass column leaves them.
    const std::string h = std::to_string(height);
    const char* attrs[] = {"height",  "content",   "bars_width", "100%", "bars_height",
                           h.c_str(), "bars_grow", "1",          nullptr};
    lv_obj_t* container = create_mini_status(parent, height, attrs);
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

void ui_ams_mini_status_set_slot_label(lv_obj_t* obj, int slot_index, const char* material,
                                       int remaining_pct) {
    auto* data = get_data(obj);
    if (!data || slot_index < 0)
        return;

    // Spool-mode label cache (uncapped; multi-unit safe). Programmatic path
    // for embedders/tests: the AmsState path in sync_from_ams_state() is the
    // real one, and the graphics render from the per-slot subjects.
    if (static_cast<int>(data->spool_cells.size()) <= slot_index)
        data->spool_cells.resize(slot_index + 1);
    SpoolCellData& c = data->spool_cells[slot_index];
    c.material = material ? material : "";
    c.remaining_pct = remaining_pct;
    // No classification source on this path: a lane carrying a material reads
    // as present, an empty one as Empty (the label rule's two visible halves).
    c.lane_state = c.material.empty() ? helix::ui::LaneState::Empty : helix::ui::LaneState::Present;
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

void helix::ui::ams_mini_status_set_fill(lv_obj_t* obj, bool fill) {
    auto* data = get_data(obj);
    if (!data || data->fill == fill)
        return;
    data->fill = fill;
    if (data->slot_count > 0)
        rebuild(data);
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

bool helix::ui::ams_mini_status_shows_lanes(int slot_count, int present_lanes) {
    return slot_count > 0 && present_lanes > 0;
}

bool helix::ui::ams_mini_status_has_lanes_to_show() {
    auto& ams = helix::AmsState::instance();
    if (!ams.are_subjects_initialized() || !ams.get_backend())
        return false;
    const int slot_count = lv_subject_get_int(ams.get_slot_count_subject());
    const auto snapshot = ams.system_info();
    int present = 0;
    for (int i = 0; i < slot_count; ++i) {
        if (!snapshot->slot_absent(i))
            ++present;
    }
    return helix::ui::ams_mini_status_shows_lanes(slot_count, present);
}

bool ui_ams_mini_status_is_valid(lv_obj_t* obj) {
    auto* data = get_data(obj);
    return data && data->magic == kAmsMiniStatusMagic;
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

    helix::AmsBackend* backend = helix::AmsState::instance().get_backend();
    if (!backend) {
        // No backend - hide widget
        data->slot_count = 0;
        data->spool_cells.clear();
        data->bypass = {};
        rebuild(data);
        return;
    }

    // A sync that reads nothing new skips the rebuild: a rebuild re-applies the
    // layout styles and flags, and each one repaints the widget.
    const int prev_slot_count = data->slot_count;
    const int prev_unit_count = data->unit_count;
    std::array<std::pair<int, int>, 8> prev_rows;
    for (int u = 0; u < 8; ++u)
        prev_rows[u] = {data->unit_rows[u].first_slot, data->unit_rows[u].slot_count};
    const std::vector<SpoolCellData> prev_cells = data->spool_cells;
    const BypassView prev_bypass = data->bypass;

    int slot_count = lv_subject_get_int(helix::AmsState::instance().get_slot_count_subject());
    data->slot_count = slot_count;

    // Get multi-unit info from system info
    const auto snapshot = helix::AmsState::instance().system_info();
    const helix::AmsSystemInfo& info = *snapshot;
    data->unit_count = static_cast<int>(info.units.size());
    data->absent_slots.clear();
    for (int u = 0; u < data->unit_count && u < 8; ++u) {
        data->unit_rows[u].first_slot = info.units[u].first_slot_global_index;
        // An absent unit's bays are not drawn: the row is condemned for having
        // no bars, like the card above it that says Not connected.
        data->unit_rows[u].slot_count = info.units[u].absent ? 0 : info.units[u].slot_count;
    }
    for (int i = 0; i < slot_count; ++i) {
        if (info.slot_absent(i)) {
            data->absent_slots.push_back(i);
        }
    }
    // Clear any stale unit rows beyond current count
    for (int u = data->unit_count; u < 8; ++u) {
        data->unit_rows[u].first_slot = 0;
        data->unit_rows[u].slot_count = 0;
    }

    // Spool-mode label/badge cache (uncapped, so the wide spool view sees
    // every lane on multi-unit systems). The spool graphic, fill, ghosting
    // and error dot are NOT collected: each cell's embedded ams_lane_spool
    // renders them from the per-slot subjects. The lane classification and
    // the active-loaded flag are read from those same subjects rather than
    // re-derived here, so the strip's labels and its graphics cannot disagree
    // about one lane (out-of-range lanes have no subject and read
    // Empty/inactive, ams_slot's fallback).
    data->spool_cells.clear();
    data->spool_cells.reserve(static_cast<size_t>(slot_count));
    for (int i = 0; i < slot_count; ++i) {
        if (info.slot_absent(i)) {
            continue;
        }
        helix::SlotInfo slot = backend->get_slot_info(i);
        int rem = -1;
        float p = slot.get_remaining_percent();
        if (p >= 0.0f)
            rem = static_cast<int>(p + 0.5f);

        lv_subject_t* lane_state_subject =
            helix::AmsState::instance().get_slot_lane_state_subject(i);
        lv_subject_t* active_subject =
            helix::AmsState::instance().get_slot_active_loaded_subject(i);

        SpoolCellData& c = data->spool_cells.emplace_back();
        c.slot = i;
        c.lane_state =
            lane_state_subject
                ? static_cast<helix::ui::LaneState>(lv_subject_get_int(lane_state_subject))
                : helix::ui::LaneState::Empty;
        c.material = slot.material;
        c.remaining_pct = rem;
        c.active = active_subject && lv_subject_get_int(active_subject) != 0;
    }

    // The AMS panel's rule for whether the bypass is on the path at all.
    data->bypass = {};
    data->bypass.shown = helix::ui::bypass_node_visible_for(backend, info);
    if (data->bypass.shown) {
        data->bypass.spool = helix::AmsState::instance().get_external_spool_info();
        data->bypass.active =
            lv_subject_get_int(helix::AmsState::instance().get_bypass_active_subject()) != 0;
    }

    bool unchanged = data->synced && prev_slot_count == slot_count &&
                     prev_unit_count == data->unit_count && prev_cells == data->spool_cells &&
                     prev_bypass == data->bypass;
    for (int u = 0; unchanged && u < 8; ++u)
        unchanged = prev_rows[u] ==
                    std::make_pair(data->unit_rows[u].first_slot, data->unit_rows[u].slot_count);
    data->synced = true;
    if (unchanged)
        return;

    rebuild(data);
    spdlog::trace("[AmsMiniStatus] Synced from AmsState: {} slots", slot_count);
}

// ============================================================================
// XML Widget Registration
// ============================================================================

/**
 * @brief XML create callback - creates widget with responsive sizing
 */
static void* ui_ams_mini_status_xml_create(lv_xml_parser_state_t* state, const char** /* attrs */) {
    // The frame's defaults fill the parent, which must have a definite height
    // for the bars to render correctly; height=0 sizes them from it.
    lv_obj_t* container =
        create_mini_status(static_cast<lv_obj_t*>(lv_xml_state_get_parent(state)), 0, nullptr);
    spdlog::trace("[AmsMiniStatus] Created via XML (responsive height)");
    return container;
}

/**
 * @brief XML apply callback - handles XML attributes
 */
static void ui_ams_mini_status_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    // Apply standard object attributes (width, height, align, etc.)
    lv_xml_obj_apply(state, attrs);

    auto* obj = static_cast<lv_obj_t*>(lv_xml_state_get_item(state));
    AmsMiniStatusData* data = obj ? get_data(obj) : nullptr;
    if (!data)
        return;

    // Parse card attribute (default: true - the spool view carries its own card)
    const char* card_str = lv_xml_get_value_of(attrs, "card");
    if (card_str) {
        data->card = lv_xml_to_bool(card_str);
        apply_spools_card_surface(data);
    }

    if (const char* fill_str = lv_xml_get_value_of(attrs, "fill")) {
        helix::ui::ams_mini_status_set_fill(obj, lv_xml_to_bool(fill_str));
    }

    const char* bars_align_str = lv_xml_get_value_of(attrs, "bars_align");
    if (bars_align_str) {
        data->bars_align =
            lv_streq(bars_align_str, "start") ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER;
        // The bars sit in a content-wide container that the widget's own row
        // places, and the multi-unit layout fills that container; both follow.
        lv_obj_set_style_flex_main_place(obj, data->bars_align, LV_PART_MAIN);
        lv_obj_set_style_flex_main_place(data->bars_container, data->bars_align, LV_PART_MAIN);
    }
}

void ui_ams_mini_status_init(void) {
    ui_ams_lane_bar_register();
    ui_ams_lane_spool_register();
    ui_spool_canvas_register(); // the spool view's bypass cell draws one
    lv_xml_register_event_cb(nullptr, "ams_mini_status_clicked", on_click);
    lv_xml_register_widget("ams_mini_status", ui_ams_mini_status_xml_create,
                           ui_ams_mini_status_xml_apply);
    spdlog::trace("[AmsMiniStatus] Registered ams_mini_status XML widget");
}
