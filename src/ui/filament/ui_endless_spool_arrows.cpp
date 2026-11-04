// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_endless_spool_arrows.h"

#include "ui_widget_memory.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "sort_small.h"
#include "theme_manager.h"
#include "widget_data_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// ============================================================================
// Constants
// ============================================================================

// Default dimensions
static constexpr int32_t kDefaultWidth = 300;
static constexpr int32_t kDefaultHeight = 50;
static constexpr int kDefaultSlotCount = 4;
static constexpr int32_t kDefaultSlotWidth = 80;

// Line styling
static constexpr int32_t LINE_WIDTH = 2;
static constexpr int32_t kDefaultLaneStep = 10;
static constexpr int32_t kArrowSize = 6;      // Arrowhead half-width, room permitting
static constexpr int32_t kArrowWingWidth = 2; // Stroke of an arrowhead's wings
static constexpr int32_t kDotRadius = 3;      // Bracket tick's end dot
static constexpr int32_t kBarGap = 4;         // Half-width of a break in a crossed bar

// Vertical spacing for multiple overlapping marks
// Bars at height 0 are at the top, height N is further down
static constexpr int32_t kBaseHeightOffset = 10;
static constexpr int32_t kHeightStep = 8; // Separation between overlapping lines

// Maximum backup connections supported
static constexpr int kMaxSlots = helix::kAmsMaxSlots;

// ============================================================================
// Widget State
// ============================================================================

struct EndlessSpoolArrowsData {
    int slot_count = kDefaultSlotCount;
    int32_t slot_width = kDefaultSlotWidth;
    int32_t slot_overlap = 0; // Overlap between slots in pixels (for 5+ slots)
    int32_t slot_offset = 0;  // Left offset of the slot row, as the slot grid lays it out

    // The relation to draw; the draw callback lays it out against slot_count
    helix::printer::EndlessSpoolConfig config;

    // Theme-derived colors
    lv_color_t arrow_color;

    // Theme-derived sizes
    int32_t line_width = LINE_WIDTH;
    int32_t lane_step = kDefaultLaneStep; // Sideways offset of one arrow-end lane
};

// Registry of widget data
static helix::WidgetDataRegistry<EndlessSpoolArrowsData> s_registry;

static EndlessSpoolArrowsData* get_data(lv_obj_t* obj) {
    return s_registry.get(obj);
}

// Load theme-aware colors
static void load_theme_colors(EndlessSpoolArrowsData* data) {
    // Use text_muted for subtle arrow color
    data->arrow_color = theme_manager_get_color("text_muted");

    // Get responsive sizing from theme
    int32_t space_xxs = theme_manager_get_spacing("space_xxs");
    data->line_width = LV_MAX(2, space_xxs);
    data->lane_step = LV_MAX(data->line_width + 2, theme_manager_get_spacing("space_md"));

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

using helix::ui::EndlessSpoolColumn;
using helix::ui::EndlessSpoolMark;
using helix::ui::EndlessSpoolMarkKind;

// A column's left-to-right position. Lanes are far narrower than a slot, so
// any lane offset stays between its slot's neighbours.
static constexpr int kLanesPerSlot = 64;
static int column_pos(const EndlessSpoolColumn& c) {
    return c.slot * kLanesPerSlot + c.lane;
}

// The bar's drawn extent, lanes included
static int mark_min_pos(const EndlessSpoolMark& m) {
    return std::min(column_pos(m.columns.front()), column_pos(m.columns.back()));
}
static int mark_max_pos(const EndlessSpoolMark& m) {
    return std::max(column_pos(m.columns.front()), column_pos(m.columns.back()));
}

// Assign height levels so marks whose bars overlap horizontally stack instead
// of sharing a line. Two hops meeting at one slot do overlap, because the
// arriving end sits right of the slot centre and the leaving end left of it;
// on one bar they would read as one line with two arrowheads.
//
// A mark always sits below every mark whose extent contains its own, so no
// container's tick drops through the group it spans. Marks are placed widest
// first, which places every container before what it contains; each then takes
// the first level from the top that is below all its containers and free of
// every mark it overlaps, so a chain alternates between two levels rather than
// stepping down per hop.
static void assign_height_levels(std::vector<EndlessSpoolMark>& marks) {
    helix::sort_small(marks.begin(), marks.end(),
                      [](const EndlessSpoolMark& a, const EndlessSpoolMark& b) {
                          const int span_a = mark_max_pos(a) - mark_min_pos(a);
                          const int span_b = mark_max_pos(b) - mark_min_pos(b);
                          if (span_a != span_b) {
                              return span_a > span_b;
                          }
                          return mark_min_pos(a) < mark_min_pos(b);
                      });

    for (size_t i = 0; i < marks.size(); ++i) {
        EndlessSpoolMark& mark = marks[i];
        int min_level = 0;
        std::vector<bool> taken;
        for (size_t j = 0; j < i; ++j) {
            const EndlessSpoolMark& prev = marks[j];
            const bool contains = mark_min_pos(prev) <= mark_min_pos(mark) &&
                                  mark_max_pos(mark) <= mark_max_pos(prev);
            if (contains) {
                min_level = std::max(min_level, prev.height_level + 1);
            }
            const int overlap_start = std::max(mark_min_pos(mark), mark_min_pos(prev));
            const int overlap_end = std::min(mark_max_pos(mark), mark_max_pos(prev));
            if (overlap_start < overlap_end) {
                const auto level = static_cast<size_t>(prev.height_level);
                if (taken.size() <= level) {
                    taken.resize(level + 1, false);
                }
                taken[level] = true;
            }
        }
        int level = min_level;
        while (static_cast<size_t>(level) < taken.size() && taken[static_cast<size_t>(level)]) {
            ++level;
        }
        mark.height_level = level;
    }
}

// Break each bar wherever another mark's vertical passes through it. A vertical
// runs from its own bar down to the slots, so it crosses only bars drawn below
// its own, and only strictly inside their extent.
static void assign_bar_gaps(std::vector<EndlessSpoolMark>& marks) {
    for (auto& lower : marks) {
        for (const auto& upper : marks) {
            if (upper.height_level >= lower.height_level) {
                continue;
            }
            for (const auto& column : upper.columns) {
                const int pos = column_pos(column);
                const bool inside = pos > mark_min_pos(lower) && pos < mark_max_pos(lower);
                if (inside && std::find(lower.bar_gaps.begin(), lower.bar_gaps.end(), column) ==
                                  lower.bar_gaps.end()) {
                    lower.bar_gaps.push_back(column);
                }
            }
        }
        helix::sort_small(lower.bar_gaps.begin(), lower.bar_gaps.end(),
                          [](const EndlessSpoolColumn& a, const EndlessSpoolColumn& b) {
                              return column_pos(a) < column_pos(b);
                          });
    }
}

namespace helix::ui {

std::vector<EndlessSpoolMark> endless_spool_marks(const helix::printer::EndlessSpoolConfig& config,
                                                  int slot_count,
                                                  const EndlessSpoolMarkLimits& limits) {
    std::vector<EndlessSpoolMark> marks;
    const auto in_range = [slot_count](int slot) { return slot >= 0 && slot < slot_count; };
    const size_t n = slot_count > 0 ? static_cast<size_t>(slot_count) : 0;

    // An ordered group draws one arrow per hand-off; a slot keeps only its first
    // successor, the same rule helix::printer::endless_spool_backup_for() applies.
    // So a slot is the source of at most one arrow, which leaves one lane left of
    // centre; arrowheads into one slot land one lane further right each.
    std::vector<bool> has_successor(n, false);
    std::vector<int> targets_at(n, 0);

    for (const auto& group : config.groups) {
        if (group.ordered) {
            for (size_t i = 0; i + 1 < group.members.size(); ++i) {
                const int from = group.members[i];
                const int to = group.members[i + 1];
                if (!in_range(from) || !in_range(to) || from == to ||
                    has_successor[static_cast<size_t>(from)]) {
                    continue;
                }
                has_successor[static_cast<size_t>(from)] = true;
                EndlessSpoolMark mark;
                mark.kind = EndlessSpoolMarkKind::Arrow;
                mark.slots = {from, to};
                mark.columns = {{from, -1}, {to, ++targets_at[static_cast<size_t>(to)]}};
                mark.min_slot = std::min(from, to);
                mark.max_slot = std::max(from, to);
                marks.push_back(std::move(mark));
            }
            continue;
        }

        EndlessSpoolMark mark;
        mark.kind = EndlessSpoolMarkKind::Bracket;
        for (int member : group.members) {
            if (in_range(member)) {
                mark.slots.push_back(member);
            }
        }
        std::sort(mark.slots.begin(), mark.slots.end());
        mark.slots.erase(std::unique(mark.slots.begin(), mark.slots.end()), mark.slots.end());
        if (mark.slots.size() < 2) {
            continue; // A group of one backs nothing up
        }
        for (int slot : mark.slots) {
            mark.columns.push_back({slot, 0});
        }
        mark.min_slot = mark.slots.front();
        mark.max_slot = mark.slots.back();
        marks.push_back(std::move(mark));
    }

    assign_height_levels(marks);

    // Past the deepest level the canvas fits, marks share its bar. Gaps follow
    // the clamped levels: marks on one bar do not break each other.
    if (limits.max_height_level >= 0) {
        for (auto& mark : marks) {
            mark.height_level = std::min(mark.height_level, limits.max_height_level);
        }
    }
    assign_bar_gaps(marks);

    // Lanes narrow on a crowded row so the outermost one stays inside its
    // slot's half of the spacing and never drifts into the neighbouring slot.
    // Separate arrowheads come first: where two share a slot, a lane never
    // narrows below what two of the smallest heads need.
    int max_lane = 0;
    for (const auto& mark : marks) {
        for (const auto& column : mark.columns) {
            max_lane = std::max(max_lane, std::abs(column.lane));
        }
    }
    const bool heads_share_a_slot =
        std::any_of(targets_at.begin(), targets_at.end(), [](int count) { return count > 1; });
    const int32_t min_lane_px = heads_share_a_slot ? 2 * (1 + kArrowWingWidth / 2) + 1 : 1;
    int32_t lane_px = std::max<int32_t>(0, limits.lane_px);
    if (max_lane > 0 && limits.slot_spacing_px > 0 && lane_px > 0) {
        lane_px =
            std::max(min_lane_px, std::min(lane_px, limits.slot_spacing_px / (2 * (max_lane + 1))));
    }

    for (auto& mark : marks) {
        for (auto& column : mark.columns) {
            column.offset_px = column.lane * lane_px;
        }
        for (auto& column : mark.bar_gaps) {
            column.offset_px = column.lane * lane_px;
        }
        if (mark.kind != EndlessSpoolMarkKind::Arrow) {
            continue;
        }
        // Arrowheads one lane apart must not overlap. A wing's stroke reaches
        // kArrowWingWidth / 2 past the half-width, so two whole heads fit in
        // less than a lane.
        const bool shared = targets_at[static_cast<size_t>(mark.slots[1])] > 1;
        const int32_t fits = (lane_px - 1) / 2 - kArrowWingWidth / 2;
        mark.head_px = shared ? std::max<int32_t>(1, std::min(kArrowSize, fits)) : kArrowSize;
    }
    return marks;
}

} // namespace helix::ui

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
    lv_draw_line(layer, &line_dsc);
}

// Draw a horizontal bar over its junctions (where its own verticals drop,
// ascending), broken around each gap x so a vertical passing through it reads
// as crossing, not joining. A break never reaches one of the bar's own
// junctions, or the junction would come loose from its bar.
static void draw_bar(lv_layer_t* layer, const std::vector<int32_t>& junction_xs, int32_t y,
                     const std::vector<int32_t>& gap_xs, lv_color_t color, int32_t width) {
    const int32_t x1 = junction_xs.front();
    const int32_t x2 = junction_xs.back();
    int32_t start = x1;
    for (int32_t gap_x : gap_xs) {
        int32_t half_gap = kBarGap + width / 2;
        for (int32_t junction_x : junction_xs) {
            half_gap = std::min(half_gap, std::abs(gap_x - junction_x) - width);
        }
        if (half_gap <= 0) {
            continue;
        }
        if (gap_x - half_gap > start) {
            draw_line(layer, start, y, gap_x - half_gap, y, color, width);
        }
        start = std::max(start, gap_x + half_gap);
    }
    if (x2 > start) {
        draw_line(layer, start, y, x2, y, color, width);
    }
}

// Draw a filled dot centred on (x, y)
static void draw_dot(lv_layer_t* layer, int32_t x, int32_t y, lv_color_t color, int32_t radius) {
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = color;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = LV_RADIUS_CIRCLE;
    lv_area_t area = {x - radius, y - radius, x + radius, y + radius};
    lv_draw_rect(layer, &rect_dsc, &area);
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
    line_dsc.width = kArrowWingWidth;
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
                              int32_t y_route, int32_t head_size,
                              const std::vector<int32_t>& gap_xs, lv_color_t color,
                              int32_t line_width) {
    // Path: horizontal at route height -> vertical down from source -> vertical down to target with
    // arrow

    // Horizontal line at route height
    draw_bar(layer, {std::min(src_x, dst_x), std::max(src_x, dst_x)}, y_route, gap_xs, color,
             line_width);

    // Vertical line from route down to source slot position (no arrow - this is the "from" side)
    draw_line(layer, src_x, y_route, src_x, y_bottom, color, line_width);

    // Vertical line from route height down toward target
    // Leave room for arrowhead at target
    int32_t arrow_top = y_bottom - head_size;
    draw_line(layer, dst_x, y_route, dst_x, arrow_top, color, line_width);

    // Draw arrowhead at target pointing down
    draw_arrow_down(layer, dst_x, y_bottom, color, head_size);
}

// Draw a bracket: one bar over the members, a tick down to each, ending in a dot.
// No arrowhead - an unordered group has no direction.
static void draw_bracket(lv_layer_t* layer, const std::vector<int32_t>& member_xs, int32_t y_bottom,
                         int32_t y_route, const std::vector<int32_t>& gap_xs, lv_color_t color,
                         int32_t line_width) {
    draw_bar(layer, member_xs, y_route, gap_xs, color, line_width);
    const int32_t dot_y = y_bottom - kDotRadius;
    for (int32_t x : member_xs) {
        draw_line(layer, x, y_route, x, dot_y, color, line_width);
        draw_dot(layer, x, dot_y, color, kDotRadius);
    }
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
    int32_t x_off = obj_coords.x1 + data->slot_offset;
    int32_t y_off = obj_coords.y1;

    // Widget is positioned ABOVE slots, so:
    // - y_bottom = bottom of widget (where it meets the slots below)
    // - y_route = bar height, level 0 near the top of the widget
    int32_t y_bottom = y_off + height - 2; // Bottom edge with small margin

    // Lowest bar that still leaves room for the arrowhead below it. Deeper
    // levels would walk y_route past y_bottom and draw the "vertical" segments
    // inverted, outside the widget, so the layout stacks the deepest marks on
    // this bar instead - crowded, but inside the canvas.
    const int32_t max_route_y = y_bottom - kArrowSize - 2;
    helix::ui::EndlessSpoolMarkLimits limits;
    limits.max_height_level =
        max_route_y > y_off + kBaseHeightOffset
            ? static_cast<int>((max_route_y - (y_off + kBaseHeightOffset)) / kHeightStep)
            : 0;
    limits.lane_px = data->lane_step;
    limits.slot_spacing_px =
        data->slot_count > 1 ? data->slot_width - data->slot_overlap : data->slot_width;

    const auto marks = helix::ui::endless_spool_marks(data->config, data->slot_count, limits);
    if (marks.empty()) {
        spdlog::trace("[EndlessSpoolArrows] No marks to draw");
        return;
    }

    const auto column_x = [&](const EndlessSpoolColumn& column) {
        return get_slot_center_x(column.slot, data->slot_count, data->slot_width,
                                 data->slot_overlap, x_off) +
               column.offset_px;
    };

    for (const auto& mark : marks) {
        const int32_t y_route = y_off + kBaseHeightOffset + mark.height_level * kHeightStep;

        std::vector<int32_t> gap_xs;
        gap_xs.reserve(mark.bar_gaps.size());
        for (const auto& column : mark.bar_gaps) {
            gap_xs.push_back(column_x(column));
        }

        if (mark.kind == EndlessSpoolMarkKind::Arrow) {
            draw_routed_arrow(layer, column_x(mark.columns[0]), column_x(mark.columns[1]), y_bottom,
                              y_route, mark.head_px, gap_xs, data->arrow_color, data->line_width);
        } else {
            std::vector<int32_t> member_xs;
            member_xs.reserve(mark.columns.size());
            for (const auto& column : mark.columns) {
                member_xs.push_back(column_x(column));
            }
            draw_bracket(layer, member_xs, y_bottom, y_route, gap_xs, data->arrow_color,
                         data->line_width);
        }
    }

    spdlog::trace("[EndlessSpoolArrows] Drew {} marks", marks.size());
}

// ============================================================================
// Event Handlers
// ============================================================================

static void endless_spool_arrows_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    if (EndlessSpoolArrowsData* registered = s_registry.take(obj)) {
        std::unique_ptr<EndlessSpoolArrowsData> data(registered);
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
    s_registry.set(obj, data_ptr.get());
    auto* data = data_ptr.release();

    // Load theme-aware colors
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, kDefaultWidth, kDefaultHeight);
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
            data->slot_count = LV_CLAMP(1, atoi(value), kMaxSlots);
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
    s_registry.set(obj, data_ptr.get());
    auto* data = data_ptr.release();

    // Load theme-aware colors
    load_theme_colors(data);

    // Configure object
    lv_obj_set_size(obj, kDefaultWidth, kDefaultHeight);
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
        data->slot_count = LV_CLAMP(1, count, kMaxSlots);
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

// NAMESPACE_OK: the widget's C setter API, beside its siblings
void ui_endless_spool_arrows_set_slot_offset(lv_obj_t* obj, int32_t offset) {
    auto* data = get_data(obj);
    if (data && data->slot_offset != offset) {
        data->slot_offset = offset;
        lv_obj_invalidate(obj);
    }
}

void ui_endless_spool_arrows_set_groups(lv_obj_t* obj,
                                        const helix::printer::EndlessSpoolConfig& config) {
    auto* data = get_data(obj);
    if (!data) {
        return;
    }
    data->config = config;
    spdlog::debug("[EndlessSpoolArrows] Config updated with {} group(s)", config.groups.size());
    lv_obj_invalidate(obj);
}

void ui_endless_spool_arrows_clear(lv_obj_t* obj) {
    auto* data = get_data(obj);
    if (data) {
        data->config.groups.clear();
        lv_obj_invalidate(obj);
    }
}

void ui_endless_spool_arrows_refresh(lv_obj_t* obj) {
    lv_obj_invalidate(obj);
}
