// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_endless_spool_arrows.h"

#include "ui_widget_memory.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

// ============================================================================
// Constants
// ============================================================================

// Default dimensions
static constexpr int32_t DEFAULT_WIDTH = 300;
static constexpr int32_t DEFAULT_HEIGHT = 50;
static constexpr int DEFAULT_SLOT_COUNT = 4;
static constexpr int32_t DEFAULT_SLOT_WIDTH = 80;

// Line styling
static constexpr int32_t LINE_WIDTH = 2;
static constexpr int32_t ARROW_SIZE = 6;    // Size of arrowhead
static constexpr int32_t CORNER_RADIUS = 6; // Radius for rounded corners

// Vertical spacing for multiple overlapping connections
// Lines at height 0 are at the top, height N is further down
static constexpr int32_t BASE_HEIGHT_OFFSET = 10;
static constexpr int32_t HEIGHT_STEP = 8; // Separation between overlapping lines

// Maximum backup connections supported
static constexpr int MAX_SLOTS = 16;

// ============================================================================
// Widget State
// ============================================================================

struct EndlessSpoolArrowsData {
    int slot_count = DEFAULT_SLOT_COUNT;
    int32_t slot_width = DEFAULT_SLOT_WIDTH;
    int32_t slot_overlap = 0; // Overlap between slots in pixels (for 5+ slots)

    // Backup slot configuration: backup_slots[source] = target (-1 = no backup)
    int backup_slots[MAX_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    // Theme-derived colors
    lv_color_t arrow_color;

    // Theme-derived sizes
    int32_t line_width = LINE_WIDTH;
};

// Registry of widget data
static std::unordered_map<lv_obj_t*, EndlessSpoolArrowsData*> s_registry;

static EndlessSpoolArrowsData* get_data(lv_obj_t* obj) {
    auto it = s_registry.find(obj);
    return (it != s_registry.end()) ? it->second : nullptr;
}

// Load theme-aware colors
static void load_theme_colors(EndlessSpoolArrowsData* data) {
    // Use text_muted for subtle arrow color
    data->arrow_color = theme_manager_get_color("text_muted");

    // Get responsive sizing from theme
    int32_t space_xxs = theme_manager_get_spacing("space_xxs");
    data->line_width = LV_MAX(2, space_xxs);

    spdlog::trace("[EndlessSpoolArrows] Theme colors loaded");
}

// ============================================================================
// Helper Functions
// ============================================================================

// Calculate X position for a slot's center
// Uses ABSOLUTE positioning with dynamic slot width from AmsPanel:
//   slot_center[i] = card_padding + slot_width/2 + i * (slot_width - overlap)
// Both slot_width and overlap are set by AmsPanel to match actual slot layout.
static int32_t get_slot_center_x(int slot_index, int slot_count, int32_t slot_width,
                                 int32_t overlap, int32_t x_off) {
    (void)slot_count; // Unused, but kept for API consistency

    if (slot_count <= 1) {
        return x_off + slot_width / 2;
    }

    // Slot spacing = slot_width - overlap (slots move closer together with overlap)
    int32_t slot_spacing = slot_width - overlap;

    return x_off + slot_width / 2 + slot_index * slot_spacing;
}

// Structure to track line segments for overlap detection
struct ArrowConnection {
    int source;
    int target;
    int min_slot;     // Left-most slot involved
    int max_slot;     // Right-most slot involved
    int height_level; // 0 = lowest, higher = further up
};

// Assign height levels to connections to avoid overlap
// Connections that span overlapping horizontal ranges get different heights
static void assign_height_levels(std::vector<ArrowConnection>& connections) {
    if (connections.empty()) {
        return;
    }

    // Sort by span width (smaller spans get lower heights - closer to slots)
    std::sort(connections.begin(), connections.end(),
              [](const ArrowConnection& a, const ArrowConnection& b) {
                  int span_a = a.max_slot - a.min_slot;
                  int span_b = b.max_slot - b.min_slot;
                  return span_a < span_b;
              });

    // Assign heights, checking for horizontal overlap
    for (size_t i = 0; i < connections.size(); ++i) {
        ArrowConnection& conn = connections[i];
        conn.height_level = 0;

        // Check against all previously assigned connections
        for (size_t j = 0; j < i; ++j) {
            const ArrowConnection& prev = connections[j];

            // Check if horizontal ranges overlap
            // Ranges overlap if: max(min1, min2) < min(max1, max2)
            int overlap_start = std::max(conn.min_slot, prev.min_slot);
            int overlap_end = std::min(conn.max_slot, prev.max_slot);

            if (overlap_start < overlap_end) {
                // There is overlap - need to use a different height
                conn.height_level = std::max(conn.height_level, prev.height_level + 1);
            }
        }
    }
}

// ============================================================================
// Drawing Functions
// ============================================================================

// Draw a single line segment
static void draw_line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      lv_color_t color, int32_t width) {
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = width;
    line_dsc.p1.x = x1;
    line_dsc.p1.y = y1;
    line_dsc.p2.x = x2;
    line_dsc.p2.y = y2;
    line_dsc.round_start = true;
    line_dsc.round_end = true;
    // Use dashed lines for subtle backup indication
    line_dsc.dash_width = 4;
    line_dsc.dash_gap = 3;
    lv_draw_line(layer, &line_dsc);
}

// Draw an arrowhead pointing downward
static void draw_arrow_down(lv_layer_t* layer, int32_t tip_x, int32_t tip_y, lv_color_t color,
                            int32_t size) {
    // Draw two lines forming a V pointing down
    // Left wing: from (tip_x - size, tip_y - size) to (tip_x, tip_y)
    // Right wing: from (tip_x + size, tip_y - size) to (tip_x, tip_y)

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = 2;
    line_dsc.round_start = true;
    line_dsc.round_end = true;

    // Left wing
    line_dsc.p1.x = tip_x - size;
    line_dsc.p1.y = tip_y - size;
    line_dsc.p2.x = tip_x;
    line_dsc.p2.y = tip_y;
    lv_draw_line(layer, &line_dsc);

    // Right wing
    line_dsc.p1.x = tip_x + size;
    line_dsc.p1.y = tip_y - size;
    line_dsc.p2.x = tip_x;
    line_dsc.p2.y = tip_y;
    lv_draw_line(layer, &line_dsc);
}

// Draw a routed connection from top: down from route, over horizontally, down to bottom
// Widget is now positioned ABOVE the slots, so arrows go:
// - Route line at top of widget (y_route)
// - Vertical lines down to bottom edge (y_bottom) which is just above slots
// - Arrowheads point down toward slots
static void draw_routed_arrow(lv_layer_t* layer, int32_t src_x, int32_t dst_x, int32_t y_bottom,
                              int32_t y_route, lv_color_t color, int32_t line_width) {
    // Path: horizontal at route height -> vertical down from source -> vertical down to target with
    // arrow

    // Horizontal line at route height (top of widget)
    draw_line(layer, src_x, y_route, dst_x, y_route, color, line_width);

    // Vertical line from route down to source slot position (no arrow - this is the "from" side)
    draw_line(layer, src_x, y_route, src_x, y_bottom, color, line_width);

    // Vertical line from route height down toward target
    // Leave room for arrowhead at target
    int32_t arrow_top = y_bottom - ARROW_SIZE;
    draw_line(layer, dst_x, y_route, dst_x, arrow_top, color, line_width);

    // Draw arrowhead at target pointing down
    draw_arrow_down(layer, dst_x, y_bottom, color, ARROW_SIZE);
}

// ============================================================================
// Main Draw Callback
// ============================================================================

static void endless_spool_arrows_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    EndlessSpoolArrowsData* data = get_data(obj);
    if (!data) {
        return;
    }

    // Get widget dimensions
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int32_t height = lv_area_get_height(&obj_coords);
    int32_t x_off = obj_coords.x1;
    int32_t y_off = obj_coords.y1;

    // Collect all connections
    std::vector<ArrowConnection> connections;
    for (int i = 0; i < data->slot_count; ++i) {
        int backup = data->backup_slots[i];
        if (backup >= 0 && backup < data->slot_count && backup != i) {
            ArrowConnection conn;
            conn.source = i;
            conn.target = backup;
            conn.min_slot = std::min(i, backup);
            conn.max_slot = std::max(i, backup);
            conn.height_level = 0;
            connections.push_back(conn);
        }
    }

    if (connections.empty()) {
        spdlog::trace("[EndlessSpoolArrows] No connections to draw");
        return;
    }

    // Assign height levels to avoid overlap
    assign_height_levels(connections);

    // Calculate Y positions
    // Widget is positioned ABOVE slots, so:
    // - y_bottom = bottom of widget (where it meets the slots below)
    // - y_route = near top of widget (routing lines are at top)
    int32_t y_bottom = y_off + height - 2; // Bottom edge with small margin

    // Highest route line that still leaves room for the arrowhead below it. An
    // N-member group now projects to N arrows (a ring, see
    // endless_spool_backup_edges), all of them overlapping, so a wide group can
    // ask for more height levels than the canvas has. Unclamped, y_route walks
    // past y_bottom and the "vertical" segments draw inverted, outside the
    // widget. Clamping stacks the deepest connections on one shared line
    // instead - crowded, but inside the canvas and readable.
    const int32_t max_route_y = y_bottom - ARROW_SIZE - 2;
    const int max_height_level =
        max_route_y > y_off + BASE_HEIGHT_OFFSET
            ? static_cast<int>((max_route_y - (y_off + BASE_HEIGHT_OFFSET)) / HEIGHT_STEP)
            : 0;

    // Draw each connection
    for (const auto& conn : connections) {
        int32_t src_x = get_slot_center_x(conn.source, data->slot_count, data->slot_width,
                                          data->slot_overlap, x_off);
        int32_t dst_x = get_slot_center_x(conn.target, data->slot_count, data->slot_width,
                                          data->slot_overlap, x_off);

        // Route height based on height_level
        // Higher levels = closer to top of widget = lower Y value
        // Level 0 is closest to bottom (slots), higher levels stack upward
        const int level = std::min(conn.height_level, max_height_level);
        int32_t y_route = y_off + BASE_HEIGHT_OFFSET + level * HEIGHT_STEP;

        draw_routed_arrow(layer, src_x, dst_x, y_bottom, y_route, data->arrow_color,
                          data->line_width);

        spdlog::trace("[EndlessSpoolArrows] Drew arrow: {} -> {} at height level {}", conn.source,
                      conn.target, conn.height_level);
    }

    spdlog::trace("[EndlessSpoolArrows] Drew {} arrows", connections.size());
}

// ============================================================================
// Event Handlers
// ============================================================================

static void endless_spool_arrows_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    auto it = s_registry.find(obj);
    if (it != s_registry.end()) {
        std::unique_ptr<EndlessSpoolArrowsData> data(it->second);
        s_registry.erase(it);
        // data automatically freed when unique_ptr goes out of scope
    }
}

// ============================================================================
// XML Widget Interface
// ============================================================================

static void* endless_spool_arrows_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create(static_cast<lv_obj_t*>(parent));
    if (!obj) {
        return nullptr;
    }

    auto data_ptr = std::make_unique<EndlessSpoolArrowsData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, endless_spool_arrows_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, endless_spool_arrows_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[EndlessSpoolArrows] Created widget via XML");
    return obj;
}

static void endless_spool_arrows_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = static_cast<lv_obj_t*>(item);
    if (!obj) {
        return;
    }

    lv_xml_obj_apply(state, attrs);

    auto* data = get_data(obj);
    if (!data) {
        return;
    }

    bool needs_redraw = false;

    for (int i = 0; attrs[i]; i += 2) {
        const char* name = attrs[i];
        const char* value = attrs[i + 1];

        if (strcmp(name, "slot_count") == 0) {
            data->slot_count = LV_CLAMP(atoi(value), 1, MAX_SLOTS);
            needs_redraw = true;
        } else if (strcmp(name, "slot_width") == 0) {
            data->slot_width = LV_MAX(atoi(value), 20);
            needs_redraw = true;
        }
    }

    if (needs_redraw) {
        lv_obj_invalidate(obj);
    }
}

// ============================================================================
// Public API
// ============================================================================

void ui_endless_spool_arrows_register(void) {
    lv_xml_register_widget("endless_spool_arrows", endless_spool_arrows_xml_create,
                           endless_spool_arrows_xml_apply);
    spdlog::info("[EndlessSpoolArrows] Registered endless_spool_arrows widget with XML system");
}

lv_obj_t* ui_endless_spool_arrows_create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[EndlessSpoolArrows] Cannot create: parent is null");
        return nullptr;
    }

    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) {
        spdlog::error("[EndlessSpoolArrows] Failed to create object");
        return nullptr;
    }

    auto data_ptr = std::make_unique<EndlessSpoolArrowsData>();
    s_registry[obj] = data_ptr.get();
    auto* data = data_ptr.release();

    // Load theme-aware colors
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, endless_spool_arrows_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, endless_spool_arrows_delete_cb, LV_EVENT_DELETE, nullptr);

    spdlog::debug("[EndlessSpoolArrows] Created widget programmatically");
    return obj;
}

void ui_endless_spool_arrows_set_slot_count(lv_obj_t* obj, int count) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_count = LV_CLAMP(count, 1, MAX_SLOTS);
        lv_obj_invalidate(obj);
    }
}

void ui_endless_spool_arrows_set_slot_width(lv_obj_t* obj, int32_t width) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_width = LV_MAX(width, 20);
        lv_obj_invalidate(obj);
    }
}

void ui_endless_spool_arrows_set_slot_overlap(lv_obj_t* obj, int32_t overlap) {
    auto* data = get_data(obj);
    if (data) {
        data->slot_overlap = LV_MAX(overlap, 0);
        spdlog::trace("[EndlessSpoolArrows] Slot overlap set to {}px", data->slot_overlap);
        lv_obj_invalidate(obj);
    }
}

void ui_endless_spool_arrows_set_config(lv_obj_t* obj, const int* backup_slots, int count) {
    auto* data = get_data(obj);
    if (!data) {
        return;
    }

    // Clear existing config
    for (int i = 0; i < MAX_SLOTS; ++i) {
        data->backup_slots[i] = -1;
    }

    // Copy new config
    int copy_count = std::min(count, MAX_SLOTS);
    for (int i = 0; i < copy_count; ++i) {
        data->backup_slots[i] = backup_slots[i];
    }

    spdlog::debug("[EndlessSpoolArrows] Config updated with {} slots", copy_count);
    lv_obj_invalidate(obj);
}

void ui_endless_spool_arrows_clear(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (data) {
        for (int i = 0; i < MAX_SLOTS; ++i) {
            data->backup_slots[i] = -1;
        }
        lv_obj_invalidate(obj);
    }
}

void ui_endless_spool_arrows_refresh(lv_obj_t* obj) {
    lv_obj_invalidate(obj);
}
