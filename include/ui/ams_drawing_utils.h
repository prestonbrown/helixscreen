// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "lvgl/lvgl.h"

#include <string>
#include <unordered_map>
#include <vector>

class AmsBackend; // Forward declaration for compute_system_tool_layout()

/**
 * @brief Shared AMS drawing utilities
 *
 * Consolidates duplicated drawing code used by ui_ams_mini_status,
 * ui_panel_ams_overview, ui_ams_slot, and ui_spool_canvas.
 */
namespace ams_draw {

// ============================================================================
// Color Utilities
// ============================================================================

/** Lighten a color by adding amount to each channel (clamped to 255) */
lv_color_t lighten_color(lv_color_t c, uint8_t amount);

/** Darken a color by subtracting amount from each channel (clamped to 0) */
lv_color_t darken_color(lv_color_t c, uint8_t amount);

/** Blend two colors: factor=0 -> c1, factor=1 -> c2 (clamped to [0,1]) */
lv_color_t blend_color(lv_color_t c1, lv_color_t c2, float factor);

// ============================================================================
// Severity & Error Helpers
// ============================================================================

/** Map error severity to theme color (danger/warning/text_muted) */
lv_color_t severity_color(SlotError::Severity severity);

/** Get worst error severity across all slots in a unit */
SlotError::Severity worst_unit_severity(const AmsUnit& unit);

// ============================================================================
// Data Helpers
// ============================================================================

/** Calculate fill percentage from SlotInfo weight data (returns min_pct..100, or 100 if unknown) */
int fill_percent_from_slot(const SlotInfo& slot, int min_pct = 5);

/**
 * Calculate bar width to fit slot_count bars in container_width.
 * @param container_pct Percentage of container_width to use (default 100)
 */
int32_t calc_bar_width(int32_t container_width, int slot_count, int32_t gap, int32_t min_width,
                       int32_t max_width, int container_pct = 100);

// ============================================================================
// Presentation Helpers
// ============================================================================

/** Get display name for a unit (uses unit.name, falls back to "Unit N") */
std::string get_unit_display_name(const AmsUnit& unit, int unit_index);

// ============================================================================
// LVGL Widget Factories
// ============================================================================

/** Create a transparent container (no bg, no border, no padding, no scroll, event bubble) */
lv_obj_t* create_transparent_container(lv_obj_t* parent);

// ============================================================================
// Spool Visualization (shared by the AMS overlay slot and the home widget)
// ============================================================================

/** Widget handles produced by create_spool_visual() */
struct SpoolVisual {
    lv_obj_t* container = nullptr;
    bool use_3d = true;
    int32_t spool_size = 0;
    lv_obj_t* canvas = nullptr;            ///< 3D-only: pseudo-3D spool canvas
    lv_obj_t* spool_outer = nullptr;       ///< flat-only: outer flange ring
    lv_obj_t* color_swatch = nullptr;      ///< flat-only: filament color ring
    lv_obj_t* spool_hub = nullptr;         ///< flat-only: center hub
    lv_obj_t* empty_placeholder = nullptr; ///< dashed-circle "empty" placeholder (hidden)
    lv_obj_t* error_indicator = nullptr;   ///< error dot, top-right (hidden)
};

/// Slack create_spool_visual() adds around the spool graphic so the lane badge,
/// which is aligned to the container's bottom-right corner, is not clipped.
///
/// Exported because it is the difference between the spool size a caller asks
/// for and the width the container actually occupies in a flex row. A caller
/// laying out a fixed-width cell around the spool has to subtract it to know
/// what is left for anything beside it; ui_ams_mini_status.cpp's spool cells do
/// exactly that, and used to carry their own copy of the literal.
inline constexpr int32_t SPOOL_VISUAL_BADGE_MARGIN_PX = 8;

/**
 * @brief Build a spool visualization into @p container, honoring /ams/spool_style.
 * @param container Parent to populate (its size is set to
 *        spool_size + SPOOL_VISUAL_BADGE_MARGIN_PX, square).
 * @param spool_size Spool graphic size in px; <= 0 uses the "ams_slot_spool_size" token.
 */
SpoolVisual create_spool_visual(lv_obj_t* container, int32_t spool_size = 0);

/** Update spool color (3D canvas or flat color_swatch + darkened outer flange) */
void spool_visual_set_color(const SpoolVisual& sv, lv_color_t color);

/** Update spool fill level 0.0-1.0 (3D canvas fill or flat concentric ring size) */
void spool_visual_set_fill(const SpoolVisual& sv, float fill_level);

/** Toggle the empty-slot placeholder vs. the spool graphic */
void spool_visual_set_empty(const SpoolVisual& sv, bool empty);

/** Toggle the error indicator dot */
void spool_visual_set_error(const SpoolVisual& sv, bool has_error);

/** Create a circular lane-number badge (1-based) using AMS badge tokens.
 *  @param parent typically the spool container; caller may re-align.
 *  @param active when true, uses the "success" accent color for the active (loaded) lane. */
lv_obj_t* create_lane_badge(lv_obj_t* parent, int lane_number, int32_t size, bool active = false);

// Shared dashed-circle draw callback for the empty-slot placeholder (moved here from
// ui_ams_slot.cpp so both the overlay and the home widget share it).
void draw_dashed_circle_cb(lv_event_t* e);

// ============================================================================
// Pulse Animation
// ============================================================================

/// Pulse animation constants (shared by error badges and error dots)
constexpr int32_t PULSE_SCALE_MIN = 180; ///< ~70% scale
constexpr int32_t PULSE_SCALE_MAX = 256; ///< 100% scale
constexpr int32_t PULSE_SAT_MIN = 80;    ///< Washed out
constexpr int32_t PULSE_SAT_MAX = 255;   ///< Full vivid
constexpr uint32_t PULSE_DURATION_MS = 800;

/** Start scale+saturation pulse animation on an object. Stores base_color in border_color. */
void start_pulse(lv_obj_t* dot, lv_color_t base_color);

/** Stop pulse animation and restore defaults (scale=256, no shadow) */
void stop_pulse(lv_obj_t* dot);

// ============================================================================
// Error Badge
// ============================================================================

/** Create a circular error badge (hidden by default, caller positions it) */
lv_obj_t* create_error_badge(lv_obj_t* parent, int32_t size);

/** Update badge visibility, color, and pulse based on error state */
void update_error_badge(lv_obj_t* badge, bool has_error, SlotError::Severity severity,
                        bool animate);

// ============================================================================
// Slot Bar Column (mini bar with fill + status line)
// ============================================================================

/** Return type for create_slot_column */
struct SlotColumn {
    lv_obj_t* container = nullptr;   ///< Column flex wrapper (bar + status line)
    lv_obj_t* bar_bg = nullptr;      ///< Background/outline container
    lv_obj_t* bar_fill = nullptr;    ///< Colored fill (child of bar_bg)
    lv_obj_t* status_line = nullptr; ///< Bottom indicator line
};

/** Parameters for styling a slot bar */
struct BarStyleParams {
    uint32_t color_rgb = 0x808080;
    int fill_pct = 100;
    bool is_present = false;
    bool is_loaded = false;
    bool has_error = false;
    SlotError::Severity severity = SlotError::INFO;
};

/// Status line dimensions
constexpr int32_t STATUS_LINE_HEIGHT_PX = 3;
constexpr int32_t STATUS_LINE_GAP_PX = 2;

/** Create slot column: bar_bg (with bar_fill child) + status_line in a column flex container */
SlotColumn create_slot_column(lv_obj_t* parent, int32_t bar_width, int32_t bar_height,
                              int32_t bar_radius);

/**
 * Style an existing slot bar (update colors, borders, fill, status line).
 * Visual style matches the overview cards:
 * - Loaded: 2px border, text color, 80% opa
 * - Present: 1px border, text_muted, 50% opa
 * - Empty: 1px border, text_muted, 20% opa (ghosted)
 * - Error: status line with severity color
 * - Non-error: status line hidden
 */
void style_slot_bar(const SlotColumn& col, const BarStyleParams& params, int32_t bar_radius);

// ============================================================================
// Logo Helpers
// ============================================================================

/** Apply logo to image widget: try unit name -> type name -> hide */
void apply_logo(lv_obj_t* image, const AmsUnit& unit, const AmsSystemInfo& info);

/** Apply logo to image widget: try type name -> hide */
void apply_logo(lv_obj_t* image, const std::string& type_name);

// ============================================================================
// System Tool Layout (physical nozzle mapping for mixed topologies)
// ============================================================================

/**
 * @brief Per-unit tool layout result
 */
struct UnitToolLayout {
    int first_physical_tool = 0; ///< Physical nozzle position for this unit
    int tool_count = 0;          ///< Number of physical nozzles (1 for HUB, N for PARALLEL)
    int min_virtual_tool = -1;   ///< Minimum mapped_tool value (for labeling)
    int hub_tool_label =
        -1; ///< Override label for HUB units (from extruder index, -1 = use min_virtual_tool)
    /// Extruder this unit's single nozzle belongs to, as an opaque name. Set
    /// only for one-nozzle units whose lanes all agree; empty otherwise. Two
    /// units naming the same extruder feed one nozzle — that is string
    /// identity, so it holds for names no numbering scheme can parse.
    std::string extruder_identity;
};

/**
 * @brief System-wide tool layout result
 *
 * Maps AFC virtual tool numbers to sequential physical nozzle positions.
 * HUB units always get 1 physical nozzle regardless of per-lane mapped_tool values.
 * PARALLEL units get 1 nozzle per lane.
 */
struct SystemToolLayout {
    std::vector<UnitToolLayout> units;
    int total_physical_tools = 0;

    /// Map AFC virtual tool number -> physical nozzle index (for active tool highlighting)
    std::unordered_map<int, int> virtual_to_physical;

    /// Map physical nozzle index -> virtual tool label number (for badge labels)
    std::vector<int> physical_to_virtual_label;

    /// Map physical nozzle index -> Klipper extruder object name ("extruder",
    /// "extruder5", …). Empty string means "this nozzle's extruder is unknown".
    ///
    /// Deliberately the NAME and not a number. The name is the identity AFC and
    /// Klipper both publish; the number is a rendering detail derived at the
    /// badge edge via helix::tool_number_for_extruder(). Keeping the string here
    /// means a future change to the badge format touches only the renderer, and
    /// the derivation plus its tests survive untouched.
    std::vector<std::string> physical_to_extruder_name;
};

/**
 * @brief True when @p layout carries a usable extruder identity for every nozzle
 *
 * The gate for rendering `E<n>` toolhead badges instead of the legacy `T<n>`
 * lane-alias labels. Requires one entry per physical tool, every entry
 * non-empty, and every entry parseable by helix::tool_number_for_extruder().
 * Anything less falls back to the legacy labels — a partial answer would badge
 * some toolheads with an extruder number and others with a lane alias, which is
 * worse than being uniformly wrong.
 */
[[nodiscard]] bool layout_has_extruder_identity(const SystemToolLayout& layout);

/// Badge text for the overview's toolhead nodes: one number per physical nozzle
/// plus the letter that says which numbering system those numbers belong to.
struct ToolBadgeLabels {
    std::vector<int> numbers; ///< Indexed by physical nozzle; empty = no badges
    char prefix = 'T';        ///< 'T' = AFC lane alias, 'E' = Klipper extruder
};

/**
 * @brief Decide the toolhead badge numbers + prefix for a system layout
 *
 * With full extruder identity every toolhead is badged `E<n>` straight from its
 * extruder name. Otherwise the legacy `T<n>` lane-alias labels are used, with
 * the active slot's own alias substituted onto the active nozzle.
 *
 * That substitution is deliberately confined to the legacy path (#1229): a lane
 * alias written onto a toolhead is exactly the collision this exists to end, and
 * it stays only because removing it would change long-standing behaviour on the
 * backends that have no extruder names to offer.
 *
 * @param layout               Layout from compute_system_tool_layout()
 * @param info                 System info, for the active slot's mapped_tool
 * @param current_slot         Globally-indexed active slot, or <0 for none
 * @param active_physical_tool Physical nozzle the active slot feeds, or <0
 */
[[nodiscard]] ToolBadgeLabels compute_tool_badge_labels(const SystemToolLayout& layout,
                                                        const AmsSystemInfo& info, int current_slot,
                                                        int active_physical_tool);

/**
 * @brief Compute physical tool layout from AMS system info
 *
 * Assigns sequential physical nozzle positions to each unit:
 * - HUB/LINEAR units: always 1 physical nozzle, regardless of mapped_tool values
 * - PARALLEL units: 1 physical nozzle per slot
 *
 * Builds virtual-to-physical mapping so active tool highlighting works correctly
 * even when AFC assigns unique virtual tool numbers to each HUB lane.
 *
 * @param info AMS system info with units and slot data
 * @param backend Backend for per-unit topology queries (nullable, falls back to unit.topology)
 * @return SystemToolLayout with physical positions and virtual mappings
 */
SystemToolLayout compute_system_tool_layout(const AmsSystemInfo& info, const AmsBackend* backend);

} // namespace ams_draw
