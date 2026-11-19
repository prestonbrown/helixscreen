// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The unit view's labels over a drawn unit, their ticks, the lane numbers under its art, and
// the size a drawn unit takes to leave its path room.

#include "ui_ams_detail_labels.h"

#include "ui_ams_detail.h"
#include "ui_ams_detail_tray.h"
#include "ui_ams_lane_spool.h"
#include "ui_ams_slot.h"
#include "ui_ams_slot_layout.h"
#include "ui_filament_path_canvas.h"
#include "ui_toolhead_view.h"
#include "ui_unit_art.h"
#include "ui_utils.h"

#include "ams_lane_state.h"
#include "ams_state.h"
#include "ams_tray_projection.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix/ui/text_metrics.h"
#include "panel_widget_size.h"
#include "path_view_scale.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using helix::ui::ams_tray::s_tray;
namespace tray = helix::ui::tray;

namespace helix::ui::ams_detail_layout {

namespace {
/// A drawn unit too tall for the path below it at full size takes smaller spools: the size
/// ams_detail_sync_lane_entry() found for one unit, slot count, width and screen.
struct SpoolHeightCap {
    int unit = -2;
    int slot_count = 0;
    int32_t available_width = 0, screen_w = 0, screen_h = 0;
    int32_t spool = 0;
};
SpoolHeightCap s_spool_cap;
int s_layout_unit = -2;         // the unit ams_detail_create_slots() lays out
bool s_layout_path_art = false; // that unit's selector is drawn in the path, under its tray
// A widget of the view it lays out; it lets go when the view is deleted.
helix::ui::WidgetRef s_layout_view;
// Under a view scale: the size a unit settled at for its row and screen, and the smallest seen
// too tall for it (fit_scaled_unit()). A new view starts them afresh.
SpoolHeightCap s_settled, s_too_big;
/// The last size tried for a row and how far its path ran over: with the size now drawn, a
/// line through the two gives the next size better than the one-step estimate.
SpoolHeightCap s_tried;
int32_t s_tried_overflow = 0;
helix::ui::PathViewScale s_view_scale;

/// The column the unit view and its path stand in, or null where they have none: the AMS
/// panel's column (a single unit), or the overview's page for one unit.
lv_obj_t* view_column(lv_obj_t* from) {
    for (lv_obj_t* o = from; o; o = lv_obj_get_parent(o)) {
        const char* name = lv_obj_get_name(o);
        if (name && (std::strcmp(name, "left_column") == 0 ||
                     std::strcmp(name, "unit_detail_container") == 0))
            return o;
    }
    return nullptr;
}

/// A column sized to its content (portrait, the sidebar below it) takes this share of the
/// viewport it scrolls in.
constexpr float kStackedColumnShare = 0.7f;

/// The height the column may take: its own where the screen sizes it (landscape); where it is
/// sized to its content (portrait), a fixed share of the viewport it scrolls in, so what the
/// sidebar shows for one unit or another never changes the scale.
int32_t view_column_height(lv_obj_t* column) {
    if (lv_obj_get_style_height(column, LV_PART_MAIN) != LV_SIZE_CONTENT ||
        lv_obj_get_style_flex_grow(column, LV_PART_MAIN) > 0)
        return lv_obj_get_content_height(column);
    for (lv_obj_t* o = lv_obj_get_parent(column); o; o = lv_obj_get_parent(o)) {
        if (lv_obj_get_style_height(o, LV_PART_MAIN) != LV_SIZE_CONTENT)
            return (int32_t)std::lround((float)lv_obj_get_content_height(o) * kStackedColumnShare);
    }
    return lv_obj_get_content_height(column);
}

int32_t head_glyph_width(int32_t scale) {
    const helix::ui::GlyphBounds b = helix::ui::toolhead_glyph_bounds(0, 0, scale);
    return b.right - b.left;
}

/// Recomputes the view scale from the column the view stands in.
/// Recomputes the view scale from the column @p from (a live widget of the view) stands in.
const helix::ui::PathViewScale& refresh_view_scale(lv_obj_t* from) {
    s_view_scale = {};
    lv_obj_t* column = from ? view_column(from) : nullptr;
    if (!column)
        return s_view_scale;
    const int32_t h = view_column_height(column);
    const char* font_name = lv_xml_get_const(nullptr, "font_small");
    const lv_font_t* font = font_name ? lv_xml_get_font(nullptr, font_name) : nullptr;
    s_view_scale = helix::ui::path_view_scale(h, helix::ui::kPathTubeLimits, head_glyph_width,
                                              font ? lv_font_get_line_height(font) : 0);
    spdlog::debug("[AmsDetail] View scale: column {} unit {:.2f} tube {} sensor_r {} hub_h {} "
                  "cap_h {} head {} (w {}) bracket {} hub_min_w {} spool_max {} in_hub {}",
                  h, s_view_scale.unit, s_view_scale.tube_gauge, s_view_scale.sensor_r,
                  s_view_scale.hub_h, s_view_scale.cap_h, s_view_scale.head_scale,
                  head_glyph_width(s_view_scale.head_scale), s_view_scale.bracket_h,
                  s_view_scale.hub_min_w, s_view_scale.spool_max, s_view_scale.buffer_in_hub);
    return s_view_scale;
}

/// What @p unit's spools in a row @p available_width wide of @p slot_count slots size to on
/// this screen with no cap: the key a cap is kept under, and the size it caps.
SpoolHeightCap natural_spool_layout(int unit, int32_t available_width, int slot_count,
                                    lv_obj_t* view) {
    SpoolHeightCap k;
    k.unit = unit;
    k.slot_count = slot_count;
    k.available_width = available_width;
    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        k.screen_w = lv_display_get_horizontal_resolution(disp);
        k.screen_h = lv_display_get_vertical_resolution(disp);
    }
    const int32_t shorter = std::min(k.screen_w, k.screen_h);
    const helix::ui::PathViewScale& scale = refresh_view_scale(view);
    // Units grow to the view scale's spool, except a tray-drawn one whose selector the path
    // draws: it keeps the screen's share as well, which leaves that drawing its room.
    const bool path_art = unit == s_layout_unit && s_layout_path_art;
    const int32_t share = shorter > 0 ? shorter / 5 : helix::ui::ams_slot_spool_size();
    const int32_t cap = scale.spool_max <= 0 ? share
                        : path_art           ? std::min(scale.spool_max, share)
                                             : scale.spool_max;
    k.spool = tray::fit_spool_size((float)available_width, slot_count, cap);
    // Under a view scale a unit already drawn takes the width its drawing allows: the drawing
    // scales with its spools, often narrower than the generic tray the fit above measures.
    lv_area_t drawn;
    if (scale.unit > 0 && s_tray.art && s_tray.art_obj && s_tray.unit_index == unit &&
        s_tray.lane_count == slot_count && s_tray.spool_size > 0 &&
        helix::ui::unit_art_drawn_area(s_tray.art_obj, drawn) && lv_area_get_width(&drawn) > 0) {
        const float per_spool = (float)lv_area_get_width(&drawn) / s_tray.spool_size;
        k.spool = std::min<int32_t>(cap, (int32_t)((float)available_width / per_spool));
    }
    return k;
}

bool same_key(const SpoolHeightCap& a, const SpoolHeightCap& b) {
    return a.unit == b.unit && a.slot_count == b.slot_count &&
           a.available_width == b.available_width && a.screen_w == b.screen_w &&
           a.screen_h == b.screen_h;
}
} // namespace

namespace {
/// The width the whole text of @p slot's material label takes.
int32_t label_whole(lv_obj_t* slot) {
    lv_obj_t* label = ui_ams_slot_get_material_label(slot);
    return helix::ui::text_width(ui_ams_slot_get_material_text(slot).c_str(),
                                 lv_obj_get_style_text_font(label, LV_PART_MAIN),
                                 lv_obj_get_style_text_letter_space(label, LV_PART_MAIN)) +
           1;
}

/// Characters a label over a drawn unit keeps before its ellipsis, at the least.
constexpr int kLabelReadableChars = 3;

/// The width @p slot's material label over a drawn unit asks for: its whole text, or at least
/// its first kLabelReadableChars characters and an ellipsis.
int32_t label_need(lv_obj_t* slot) {
    lv_obj_t* label = ui_ams_slot_get_material_label(slot);
    const std::string text = ui_ams_slot_get_material_text(slot);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const int32_t space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const int32_t whole = helix::ui::text_width(text.c_str(), font, space);
    std::string head;
    int chars = 0;
    for (const char* p = text.c_str(); *p && chars <= kLabelReadableChars; ++p) {
        if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80 && ++chars > kLabelReadableChars)
            break;
        head += *p;
    }
    if (chars <= kLabelReadableChars)
        return whole;
    return std::min(whole, helix::ui::text_width((head + "...").c_str(), font, space));
}

/// Which of @p slot's lane sensors its backend reports a fault at.
helix::LaneSensor lane_sensor_fault(lv_obj_t* slot) {
    const int global = slot ? (int)reinterpret_cast<intptr_t>(lv_obj_get_user_data(slot)) : -1;
    helix::AmsBackend* backend = helix::AmsState::instance().get_backend();
    if (!backend || global < 0)
        return helix::LaneSensor::NONE;
    const helix::SlotInfo info = backend->get_slot_info(global);
    return info.error ? info.error->sensor : helix::LaneSensor::NONE;
}

/// Whether a lane over a drawn unit shows its label: a lane with no spool shows a "+" in its
/// bay instead; a ghost keeps the name it remembers. Read from the backend, which the spool's
/// own drawing may not have caught up with yet.
bool lane_shows_label(lv_obj_t* slot) {
    if (!slot)
        return false;
    const int global = (int)reinterpret_cast<intptr_t>(lv_obj_get_user_data(slot));
    helix::AmsBackend* backend = helix::AmsState::instance().get_backend();
    if (backend && global >= 0)
        return helix::ui::classify_lane(backend->get_slot_info(global)) !=
               helix::ui::LaneState::Empty;
    lv_obj_t* spool = lv_obj_find_by_name(slot, "lane_spool");
    return !(spool && helix::ui::ams_lane_spool_empty(spool));
}
} // namespace

void note_drawn_spool_size(int32_t size) {
    // A cap the row draws a pixel short of (its rounding) is the size it shows.
    if (s_spool_cap.spool > 0 && std::abs(s_spool_cap.spool - size) <= 1)
        s_spool_cap.spool = size;
}

void set_layout_unit(int unit_index, lv_obj_t* view, bool path_art) {
    s_layout_unit = unit_index;
    s_layout_path_art = path_art;
    if (!s_layout_view.get() || view != s_layout_view.get()) {
        s_settled = {};
        s_too_big = {};
        s_tried = {};
        s_spool_cap = {};
    }
    s_layout_view = view;
}

int over_lane_label_rows(const helix::ui::WidgetRef slot_widgets[], int count,
                         const AmsSlotLayout& layout, lv_obj_t* slot_area) {
    const int32_t gap = theme_manager_get_spacing("space_xs");
    // Rows estimated at the slots' pitch, to reserve their height; the tray places them
    // over the drawn spools.
    float x[kAmsDetailMaxSlots];
    int32_t whole[kAmsDetailMaxSlots], need[kAmsDetailMaxSlots];
    int shown = 0;
    for (int i = 0; i < count && shown < kAmsDetailMaxSlots; ++i) {
        lv_obj_t* label =
            slot_widgets[i] ? ui_ams_slot_get_material_label(slot_widgets[i]) : nullptr;
        if (!label || !lane_shows_label(slot_widgets[i]))
            continue;
        x[shown] = (float)(i * (layout.slot_width - layout.overlap));
        whole[shown] = label_whole(slot_widgets[i]);
        need[shown++] = label_need(slot_widgets[i]);
    }
    const float row_w = (float)lv_obj_get_content_width(slot_area);
    const float start = (float)(layout.centering_offset + layout.slot_width / 2);
    return helix::ui::ams_over_lane_label_rows(x, whole, need, shown, gap, -start, row_w - start);
}

/// How far below where a lane's tube leaves a drawn unit's bottom edge its sensor band stands:
/// just under the foot, the lane's number standing on the unit's face above it.
int32_t art_lane_band_drop() {
    return std::max<int32_t>(2, theme_manager_get_spacing("space_xs"));
}

namespace {
/// Where a drawn unit's bottom edge crosses screen column @p x, from its two ends.
float edge_y_at(lv_point_t a, lv_point_t b, float x) {
    return b.x == a.x ? (float)a.y
                      : (float)a.y + (float)(b.y - a.y) * (x - (float)a.x) / (float)(b.x - a.x);
}

/// How tall @p obj's content stands, though a flex parent may have squeezed it shorter:
/// down to the lowest visible descendant, and its bottom padding.
int32_t natural_height(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t lowest = a.y2;
    std::vector<lv_obj_t*> pending{obj};
    while (!pending.empty()) {
        lv_obj_t* o = pending.back();
        pending.pop_back();
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
            lv_obj_t* c = lv_obj_get_child(o, (int32_t)i);
            if (lv_obj_is_hidden(c))
                continue;
            lv_area_t ca;
            lv_obj_get_coords(c, &ca);
            lowest = std::max(lowest, ca.y2);
            pending.push_back(c);
        }
    }
    return lowest - a.y1 + 1 + lv_obj_get_style_pad_bottom(obj, LV_PART_MAIN);
}

/// The lowest the diagram holding @p obj may reach on screen: the bottom of the nearest
/// ancestor that scrolls (what lies below it there is reached by scrolling), else of its
/// topmost ancestor below the screen; less, in each column that does not scroll, what stands
/// below the diagram at its natural height.
int32_t on_screen_bottom(lv_obj_t* obj) {
    int32_t below = 0;
    lv_obj_t* top = obj;
    for (lv_obj_t* o = obj; lv_obj_get_parent(o) && lv_obj_get_parent(lv_obj_get_parent(o));
         o = lv_obj_get_parent(o)) {
        lv_obj_t* parent = lv_obj_get_parent(o);
        top = parent;
        if (lv_obj_is_scrollable(parent) && (lv_obj_get_scroll_dir(parent) & LV_DIR_VER))
            break;
        if (lv_obj_get_style_flex_flow(parent, LV_PART_MAIN) != LV_FLEX_FLOW_COLUMN)
            continue;
        bool after = false;
        for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
            lv_obj_t* sibling = lv_obj_get_child(parent, (int32_t)i);
            if (sibling == o)
                after = true;
            else if (after && !lv_obj_is_hidden(sibling) && !lv_obj_is_floating(sibling))
                below += natural_height(sibling);
        }
    }
    lv_area_t a;
    lv_obj_get_coords(top, &a);
    return a.y2 - below;
}

/// Under a view scale the drawing takes what its row and the column leave: as wide as its row
/// allows (@p layout), as tall as the path at its least height leaves, growing as well as
/// shrinking, solved in one step. @p overflow is how far the path at its least height runs
/// past the column's foot, negative where it leaves room; @p fan_growth is how much taller
/// the path grows as the unit grows by a fraction (ui_filament_path_canvas_fan_growth()).
/// True when the unit must be laid out again at a new size.
bool fit_scaled_unit(int32_t overflow, float fan_growth, const SpoolHeightCap& layout) {
    // A path a pixel or two past the foot still shows whole.
    constexpr int32_t kOverflowSlack = 2;
    const int32_t art_h = s_tray.art_drawn_h;
    const int32_t now = (int32_t)s_tray.spool_size;
    if (art_h <= 0 || now <= 0)
        return false;
    // Once a unit has settled at a size for its row and screen, later passes (a lane filling,
    // a label changing) leave it there unless the path no longer fits: it does not hunt.
    if (same_key(s_settled, layout) && overflow <= kOverflowSlack) {
        s_spool_cap = layout;
        s_spool_cap.spool = now;
        return false;
    }
    // Growing the unit by a fraction e moves the path's foot down by its own height times e
    // and by what its lanes' wider spread asks of the fan: the size whose overflow is zero.
    const float e = -(float)overflow / ((float)art_h + fan_growth);
    // The drawing scales with its spools only roughly: a size seen too tall for this row is
    // never grown back to, so the size settles instead of trading a pixel back and forth.
    if (!same_key(s_too_big, layout))
        s_too_big = layout, s_too_big.spool = INT32_MAX;
    if (overflow > kOverflowSlack)
        s_too_big.spool = std::min(s_too_big.spool, now);
    // It never shrinks below a third of its row's size, and never grows to get there.
    const bool over = overflow > kOverflowSlack;
    const int32_t floor_spool = over ? std::min(layout.spool / 3, now) : layout.spool / 3;
    float estimate = (float)now * (1.0f + e);
    // A second size tried at this row: the overflow it gave against the one now gives the
    // path's real response to the unit's size.
    if (same_key(s_tried, layout) && s_tried.spool != now) {
        const float per_px = (float)(s_tried_overflow - overflow) / (float)(s_tried.spool - now);
        if (per_px > 0.05f)
            estimate = (float)now - (float)overflow / per_px;
    }
    s_tried = layout;
    s_tried.spool = now;
    s_tried_overflow = overflow;
    int32_t spool =
        std::clamp<int32_t>((int32_t)std::floor(estimate), floor_spool,
                            std::max(floor_spool, std::min(layout.spool, s_too_big.spool - 1)));
    // A row that rounds to a size above the cap it was given draws no smaller for the same
    // cap again: ask below what it drew by as much as it overshot.
    if (over && same_key(s_spool_cap, layout) && s_spool_cap.spool > 0 && now > s_spool_cap.spool &&
        spool >= s_spool_cap.spool)
        spool = std::max(floor_spool, s_spool_cap.spool - (now - s_spool_cap.spool));
    if (spool == now || (over && spool > now) || (!over && spool <= now + 1)) {
        // Settled: the cap is the size it shows.
        s_settled = layout;
        s_spool_cap = layout;
        s_spool_cap.spool = now;
        return false;
    }
    s_settled = {};
    s_spool_cap = layout;
    s_spool_cap.spool = spool;
    spdlog::debug("[AmsDetail] Unit {} drawn at {} for a path {} px {} (fan {:.0f}): spools at {}",
                  layout.unit, now, std::abs(overflow), overflow > 0 ? "over" : "under", fan_growth,
                  spool);
    return true;
}

/// The path under a drawn unit gets its full-size height; see ams_detail_sync_lane_entry().
/// @p layout is the shown unit's row at its natural size, which a cap is kept under.
bool ams_detail_fit_unit_to_path(lv_obj_t* canvas, int32_t reach, const SpoolHeightCap& layout) {
    const int32_t need = ui_filament_path_canvas_required_height(canvas);
    if (need <= 0 || !s_tray.art || s_tray.art_drawn_h <= 0)
        return false;
    lv_obj_t* box = lv_obj_get_parent(canvas);
    const bool fills_column = lv_obj_get_style_flex_grow(box, LV_PART_MAIN) > 0;
    if (!fills_column) {
        // DECLARATIVE_OK: measured layout; a fixed-height path grows to what its stack needs
        const int32_t token = theme_manager_get_spacing("ams_portrait_path_h");
        // Under a view scale the path is exactly as tall as it needs: the unit takes the rest.
        const int32_t want = s_view_scale.unit > 0 ? need - reach : std::max(token, need - reach);
        if (lv_obj_get_height(box) != want) {
            lv_obj_set_height(box, want);
            lv_obj_update_layout(lv_obj_get_parent(box));
            const int32_t h = lv_obj_get_content_height(box) + reach;
            if (lv_obj_get_style_height(canvas, LV_PART_MAIN) != h && reach > 0)
                lv_obj_set_height(canvas, h);
            lv_obj_update_layout(lv_obj_get_parent(box));
        }
    }
    lv_area_t cc, bc;
    lv_obj_get_coords(canvas, &cc);
    lv_obj_get_coords(box, &bc);
    const int32_t bottom = cc.y1 + std::max(lv_area_get_height(&cc), need) - 1;
    int32_t limit = on_screen_bottom(canvas);
    if (fills_column)
        limit = std::min(limit, bc.y2);
    const int32_t overflow = bottom - limit;
    // Under a view scale the unit is sized from the path once the path is set up for this
    // unit's slots: a path still laid out for the slots before them would size it wrongly.
    if (s_view_scale.unit > 0)
        return ui_filament_path_canvas_get_slot_grid(canvas) == s_tray.slot_grid &&
               fit_scaled_unit(cc.y1 + need - 1 - limit, ui_filament_path_canvas_fan_growth(canvas),
                               layout);
    if (overflow <= 0)
        return false;
    // The drawing scales with its spools: shrinking it by the overflow gives the path that room.
    const int32_t art_h = s_tray.art_drawn_h;
    // A unit keeps at least half the size its width gives it, so its drawing still
    // reads; where that is not enough the head below gives up the rest.
    const int32_t floor_spool = layout.spool / 2;
    const int32_t spool =
        std::max<int32_t>(floor_spool, (int32_t)((float)s_tray.spool_size *
                                                 (float)(art_h - overflow) / (float)art_h));
    const bool same = same_key(s_spool_cap, layout);
    if (spool >= s_tray.spool_size || (same && s_spool_cap.spool > 0 && spool >= s_spool_cap.spool))
        return false;
    s_spool_cap = layout;
    s_spool_cap.spool = spool;
    spdlog::debug("[AmsDetail] Unit {} overflows its path by {} px: spools capped at {}",
                  s_spool_cap.unit, overflow, spool);
    return true;
}
} // namespace

/// Marks each lane of the unit view's art as its slot shows it: the slot's glow round the
/// spool's outline, a bay that is not there dimmed, and an empty lane's "+" and error dot on
/// its number badge.
void refresh_unit_view_marks() {
    lv_obj_t* art = s_tray.art_obj;
    if (!s_tray.art || !art)
        return;
    std::vector<helix::ui::UnitArtLaneMark> marks(static_cast<size_t>(s_tray.lane_count));
    for (int i = 0; i < s_tray.lane_count; ++i) {
        lv_obj_t* slot = s_tray.slot[i];
        if (!slot)
            continue;
        lv_obj_t* spool = lv_obj_find_by_name(slot, "lane_spool");
        helix::ui::UnitArtLaneMark& m = marks[static_cast<size_t>(i)];
        m.dim = lv_obj_has_state(slot, LV_STATE_DISABLED);
        m.glow = spool && helix::ui::ams_lane_spool_glow(spool, m.color);
        // A bay with no spool draws none (a "+" stands in it); a remembered one is a ghost.
        m.ghost = spool && helix::ui::ams_lane_spool_ghosted(spool);
        m.absent = spool && helix::ui::ams_lane_spool_empty(spool);
        m.prep_fault = lane_sensor_fault(slot) == helix::LaneSensor::PREP;
        // A lane a finger is down on is outlined whole: a bright ring round its spool (or its
        // empty bay) and one on its number.
        const bool pressed = i == s_tray.pressed_lane;
        m.outline = pressed;
        if (lv_obj_t* badge = ui_ams_slot_get_status_badge(slot)) {
            // DECLARATIVE_OK: per-lane press state with no subject behind it
            lv_obj_set_style_outline_width(badge, pressed ? 2 : 0, LV_PART_MAIN);
            lv_obj_set_style_outline_color(badge, theme_manager_get_color("text"), LV_PART_MAIN);
            lv_obj_set_style_outline_pad(badge, 1, LV_PART_MAIN);
        }
        helix::SlotError::Severity severity = helix::SlotError::Severity::INFO;
        const bool error = spool && helix::ui::ams_lane_spool_error_dot(spool, severity);
        ui_ams_slot_set_badge_marks(
            slot, s_tray.badge_plus && spool && helix::ui::ams_lane_spool_empty(spool), error,
            severity, m.absent);
    }
    helix::ui::unit_art_set_lane_marks(art, marks);
}

/// Lays the labels over the unit out from s_tray: which lanes show one (an empty lane's "+"
/// badge says it), each over its own spool, centered where the room allows, in the fewest rows
/// (cycling label by label) that give every label its text or at least a few readable
/// characters, each clear of the ticks rising past it; the lanes' climate readings over their
/// labels; and each label's tick straight down to its spool. Returns the highest top any of
/// them reaches, INT32_MAX with none.
int32_t place_unit_labels() {
    lv_obj_t* layer = s_tray.labels_layer;
    lv_obj_t* container = s_tray.container;
    if (!layer || !container)
        return INT32_MAX;
    const int n = s_tray.lane_count;
    lv_area_t c;
    lv_obj_get_coords(container, &c);
    const lv_point_t origin{c.x1, c.y1};
    const int32_t label_bottom = s_tray.label_bottom;
    float shown_x[kAmsDetailMaxSlots];
    int32_t need[kAmsDetailMaxSlots], text_w[kAmsDetailMaxSlots];
    int lane_of[kAmsDetailMaxSlots];
    int shown = 0;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* slot = s_tray.slot[i];
        lv_obj_t* label = slot ? ui_ams_slot_get_material_label(slot) : nullptr;
        s_tray.label_shown[i] = false;
        if (!label || lv_obj_get_parent(label) != layer)
            continue;
        const bool show = lane_shows_label(slot);
        s_tray.label_shown[i] = show;
        // DECLARATIVE_OK: measured layout; an empty lane's badge stands in for its label
        lv_obj_set_hidden(label, !show);
        if (!show)
            continue;
        shown_x[shown] = s_tray.label_x[i];
        need[shown] = label_need(slot);
        text_w[shown] = label_whole(slot);
        lane_of[shown++] = i;
    }
    const int32_t label_gap = theme_manager_get_spacing("space_xs");
    // Labels keep inside the unit's row.
    const float row_w = (float)lv_obj_get_width(container);
    const int rows =
        helix::ui::ams_over_lane_label_rows(shown_x, text_w, need, shown, label_gap, 0.0f, row_w);
    int row_of[kAmsDetailMaxSlots];
    helix::ui::AmsLabelSpan span[kAmsDetailMaxSlots];
    for (int k = 0; k < shown; ++k)
        row_of[k] = helix::ui::ams_alternate_row(k, rows).row;
    // Each label takes the room its whole text asks, as far as its neighbours allow.
    helix::ui::ams_over_lane_label_spans(shown_x, row_of, text_w, shown, label_gap, 0.0f, row_w,
                                         span);
    // A lane's climate reading stands over its label, so a row of labels steps up past the
    // tallest reading too.
    int32_t reading_h = 0;
    for (int k = 0; k < shown && rows > 1; ++k) {
        lv_obj_t* reading = ui_ams_slot_get_lane_humidity(s_tray.slot[lane_of[k]]);
        if (reading && !lv_obj_is_hidden(reading)) {
            lv_obj_update_layout(reading);
            reading_h = std::max(reading_h, lv_obj_get_height(reading) + 1);
        }
    }
    for (int k = 0; k < shown; ++k) {
        lv_obj_t* label = ui_ams_slot_get_material_label(s_tray.slot[lane_of[k]]);
        // A centered label takes all the room either side allows evenly, so a longer name
        // that fits needs no new layout; a slid one keeps to its span.
        const bool centered = span[k].left == span[k].right;
        const int32_t even = 2 * std::min(span[k].room_left, span[k].room_right);
        // DECLARATIVE_OK: measured layout; the label's width and place follow its drawn spool
        lv_obj_set_width(
            label, std::max<int32_t>(1, centered ? std::max(even, span[k].left + span[k].right)
                                                 : span[k].left + span[k].right));
        clear_translate(label);
        lv_obj_update_layout(label);
        lv_area_t la;
        lv_obj_get_coords(label, &la);
        const int32_t row_step =
            lv_area_get_height(&la) + reading_h + theme_manager_get_spacing("space_xxs");
        // Its middle over the spool, or slid toward the side with room.
        const float middle = shown_x[k] + (float)(span[k].right - span[k].left) / 2.0f;
        lv_obj_set_style_translate_x(
            label, origin.x + (int32_t)std::lround(middle) - (la.x1 + la.x2) / 2, LV_PART_MAIN);
        // The nearest row stands space_md over the unit, each other a row above.
        lv_obj_set_style_translate_y(label, label_bottom - row_of[k] * row_step - la.y2,
                                     LV_PART_MAIN);
    }
    int32_t highest = INT32_MAX;
    lv_obj_update_layout(layer);
    for (int i = 0; i < n; ++i) {
        lv_obj_t* slot = s_tray.slot[i];
        lv_obj_t* label = slot ? ui_ams_slot_get_material_label(slot) : nullptr;
        if (!label || lv_obj_get_parent(label) != layer)
            continue;
        lv_area_t la;
        lv_obj_get_coords(label, &la);
        highest = std::min<int32_t>(highest, la.y1);
        // A lane's own climate reading stands directly over its label.
        lv_obj_t* row = ui_ams_slot_get_lane_humidity(slot);
        if (!row || lv_obj_is_hidden(row))
            continue;
        clear_translate(row);
        lv_obj_update_layout(row);
        lv_area_t ra;
        lv_obj_get_coords(row, &ra);
        lv_obj_set_style_translate_x(row, (la.x1 + la.x2 - ra.x1 - ra.x2) / 2, LV_PART_MAIN);
        lv_obj_set_style_translate_y(row, la.y1 - 1 - ra.y2, LV_PART_MAIN);
        highest = std::min<int32_t>(highest, la.y1 - 1 - (ra.y2 - ra.y1));
    }
    // Each label's tick drops straight from just under it to the top of its spool, which
    // stands directly below: a lower row's labels keep clear of the ticks rising past them.
    s_tray.leader_count = 0;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* slot = s_tray.slot[i];
        if (!slot)
            continue;
        ui_ams_slot_hide_leader(slot);
        lv_obj_t* label = ui_ams_slot_get_material_label(slot);
        if (!label || lv_obj_get_parent(label) != layer || lv_obj_is_hidden(label))
            continue;
        lv_area_t la;
        lv_obj_get_coords(label, &la);
        const float x = s_tray.label_x[i];
        const float y = (float)(la.y2 + 2 - origin.y);
        tray::PointF* leader = s_tray.leader[s_tray.leader_count++];
        leader[0] = {x, y};
        leader[1] = {x, y};
        leader[2] = {x, std::max(y, s_tray.tick_end_y[i])};
    }
    lv_obj_invalidate(layer);
    return highest;
}

/// Over the art each lane's tool badge stands just right of its label, off the drawing.
void place_tool_badges() {
    const int32_t tool_inset = theme_manager_get_spacing("ams_slot_tool_badge_inset");
    for (int i = 0; i < s_tray.lane_count; ++i) {
        lv_obj_t* slot = s_tray.slot[i];
        lv_obj_t* tool = slot ? lv_obj_find_by_name(slot, "tool_badge") : nullptr;
        if (!tool)
            continue;
        // DECLARATIVE_OK: measured layout; the badge's place depends on the drawn unit
        lv_obj_set_style_translate_x(tool, tool_inset, LV_PART_MAIN);
        lv_obj_set_style_translate_y(tool, tool_inset, LV_PART_MAIN);
        lv_obj_t* label = ui_ams_slot_get_material_label(slot);
        if (!s_tray.art || !label || lv_obj_is_hidden(tool))
            continue;
        lv_obj_update_layout(tool);
        lv_area_t ta, la;
        lv_obj_get_coords(tool, &ta);
        lv_obj_get_coords(label, &la);
        const int32_t text_right =
            (la.x1 + la.x2) / 2 +
            helix::ui::text_width(lv_label_get_text(label),
                                  lv_obj_get_style_text_font(label, LV_PART_MAIN),
                                  lv_obj_get_style_text_letter_space(label, LV_PART_MAIN)) /
                2;
        lv_obj_set_style_translate_x(tool, tool_inset + std::min(text_right, la.x2) + 2 - ta.x1,
                                     LV_PART_MAIN);
        lv_obj_set_style_translate_y(tool, tool_inset + (la.y1 + la.y2) / 2 - (ta.y1 + ta.y2) / 2,
                                     LV_PART_MAIN);
    }
}

/// A lane filled or emptied while the unit is shown: its label comes or goes, and the labels
/// and ticks are laid out again. Where the labels then rise past the unit row's top, or the
/// numbers under the art need another row or lose their "+", the whole tray is laid out again
/// (ams_detail_update_tray), which makes the room; otherwise only the labels move.
void relabel_if_lanes_changed() {
    if (!s_tray.valid || !s_tray.labels_layer || !s_tray.container)
        return;
    const int n = s_tray.lane_count;
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* slot = s_tray.slot[i];
        lv_obj_t* label = slot ? ui_ams_slot_get_material_label(slot) : nullptr;
        if (label && lv_obj_get_parent(label) == s_tray.labels_layer)
            changed |= lane_shows_label(slot) != s_tray.label_shown[i];
    }
    if (!changed)
        return;
    const int32_t highest = place_unit_labels();
    lv_area_t c;
    lv_obj_get_coords(s_tray.container, &c);
    bool relayout = highest != INT32_MAX && highest < c.y1;
    if (relayout && s_tray.slot_grid && s_tray.slot_tray) {
        AmsDetailWidgets w;
        w.slot_grid = s_tray.slot_grid;
        w.slot_tray = s_tray.slot_tray;
        w.labels_layer = s_tray.labels_layer;
        w.badge_layer = s_tray.badge_layer;
        w.env_indicator = s_tray.env_indicator;
        helix::ui::WidgetRef slots[kAmsDetailMaxSlots];
        for (int i = 0; i < n; ++i)
            slots[i].reset(s_tray.slot[i]);
        ams_detail_update_tray(w, slots, n, s_tray.unit_index);
        return;
    }
    place_tool_badges();
    lv_obj_invalidate(s_tray.container);
}

bool s_relabel_queued = false;

/// A lane's spool changed what it shows: its label comes or goes; on the art, its color and fill
/// reach the drawing and its glow the marks. The labels are laid out on the next UI update,
/// once the lane's label text, which the same backend update sets after its spool, is in.
void unit_view_lane_changed(void* /*user*/) {
    if (!s_relabel_queued) {
        s_relabel_queued = true;
        helix::ui::queue_update("AmsDetail::relabel", [] {
            s_relabel_queued = false;
            relabel_if_lanes_changed();
        });
    }
    lv_obj_t* art = s_tray.art_obj;
    if (!s_tray.art || !art)
        return;
    const auto info = helix::AmsState::instance().system_info();
    if (!info)
        return;
    const int u = view_unit(*info, s_tray.unit_index);
    if (u < 0 || u >= static_cast<int>(info->units.size()))
        return;
    helix::ui::unit_art_set_unit(art, *info, u, helix::ui::unit_reports_climate(s_tray.unit_index));
    refresh_unit_view_marks();
}

} // namespace helix::ui::ams_detail_layout

using namespace helix::ui::ams_detail_layout;

bool helix::ui::ams_detail_sync_lane_entry(lv_obj_t* canvas, lv_obj_t* slot_grid) {
    if (!canvas)
        return false;
    {
        // The path and the linked-lanes bracket take the view's scale.
        const helix::ui::PathViewScale& scale = refresh_view_scale(canvas);
        ui_filament_path_canvas_set_view_scale(canvas, scale);
        lv_obj_t* column = view_column(canvas);
        lv_obj_t* bracket = column ? lv_obj_find_by_name(column, "endless_arrows") : nullptr;
        if (bracket && scale.bracket_h > 0 && lv_obj_get_height(bracket) != scale.bracket_h) {
            // DECLARATIVE_OK: measured layout; the bracket's height is a share of the column
            lv_obj_set_height(bracket, scale.bracket_h);
        }
    }
    int32_t offset = INT32_MIN;
    if (slot_grid && s_tray.valid && s_tray.container == lv_obj_get_parent(slot_grid)) {
        lv_area_t c, g;
        lv_obj_get_coords(s_tray.container, &c);
        lv_obj_get_coords(slot_grid, &g);
        offset = c.y1 + (int32_t)std::lround(s_tray.box.fb) - g.y1;
    } else if (slot_grid && !draws_tray()) {
        // With no housing the lanes leave from under the spools' flanges.
        lv_obj_update_layout(slot_grid);
        lv_area_t g;
        lv_obj_get_coords(slot_grid, &g);
        float x = 0, y = 0;
        int32_t size = 0;
        if (spool_centre(slot_grid, {g.x1, g.y1}, x, y, size))
            offset = (int32_t)std::lround(y + tray::kSpoolFlangeRadius * size);
    }
    const bool art = slot_grid && s_tray.valid && s_tray.art && s_tray.art_obj &&
                     s_tray.container == lv_obj_get_parent(slot_grid);
    lv_point_t edge_a{}, edge_b{};
    // The canvas fills its container's height; under a drawn unit it also reaches up into the
    // unit, so it is that much taller and still ends at the container's bottom. Each change
    // is made only when it changes something: a resize repaints the canvas.
    const auto fill_container = [canvas](int32_t reach) {
        // DECLARATIVE_OK: measured layout; the canvas reaches up only under a drawn unit
        if (reach <= 0) {
            if (lv_obj_get_style_flex_grow(canvas, LV_PART_MAIN) != 1)
                lv_obj_set_style_flex_grow(canvas, 1, LV_PART_MAIN);
            return;
        }
        const int32_t h = lv_obj_get_content_height(lv_obj_get_parent(canvas)) + reach;
        if (lv_obj_get_style_flex_grow(canvas, LV_PART_MAIN) != 0)
            lv_obj_set_style_flex_grow(canvas, 0, LV_PART_MAIN);
        if (lv_obj_get_style_height(canvas, LV_PART_MAIN) != h)
            lv_obj_set_height(canvas, h);
    };
    if (!art || !helix::ui::unit_art_bottom_edge(s_tray.art_obj, edge_a, edge_b)) {
        ui_filament_path_canvas_set_art_sensors(canvas, false, false, 0.0f, nullptr, 0);
        fill_container(0);
        // DECLARATIVE_OK: measured layout; the canvas reaches up only under a drawn unit
        lv_obj_set_style_margin_top(canvas, 0, LV_PART_MAIN);
        ui_filament_path_canvas_set_lane_entry(canvas, offset);
        for (int i = 0; i < kAmsDetailMaxSlots; ++i) {
            ui_filament_path_canvas_set_lane_entry_rise(canvas, i, 0);
            ui_filament_path_canvas_set_lane_band_drop(canvas, i, 0);
        }
        return false;
    }
    // Under a drawn unit each lane's tube starts on the unit's bottom edge, slanted as drawn,
    // where it crosses the lane's column; its band and number stand on a line parallel to it.
    lv_area_t art_area, g;
    lv_obj_get_coords(s_tray.art_obj, &art_area);
    lv_obj_get_coords(slot_grid, &g);
    edge_a = {edge_a.x + art_area.x1, edge_a.y + art_area.y1};
    edge_b = {edge_b.x + art_area.x1, edge_b.y + art_area.y1};
    const int n = std::min<int>((int)lv_obj_get_child_count(slot_grid), kAmsDetailMaxSlots);
    float start[kAmsDetailMaxSlots] = {};
    float lowest = -INFINITY, highest = INFINITY;
    for (int i = 0; i < n; ++i) {
        lv_obj_t* holder = ui_ams_slot_get_spool_container(lv_obj_get_child(slot_grid, i));
        lv_area_t ha{};
        if (holder)
            lv_obj_get_coords(holder, &ha);
        start[i] = edge_y_at(edge_a, edge_b, (float)(ha.x1 + ha.x2) / 2.0f);
        lowest = std::max(lowest, start[i]);
        highest = std::min(highest, start[i]);
    }
    if (n <= 0)
        return false;
    const int32_t entry = (int32_t)std::lround(lowest);
    ui_filament_path_canvas_set_lane_entry(canvas, entry - g.y1);
    {
        // Each lane's load sensor marks where its tube leaves the drawing, along its foot.
        bool load_fault[kAmsDetailMaxSlots] = {};
        for (int i = 0; i < n; ++i)
            load_fault[i] =
                lane_sensor_fault(lv_obj_get_child(slot_grid, i)) == helix::LaneSensor::LOAD;
        const float slope = edge_b.x != edge_a.x
                                ? (float)(edge_b.y - edge_a.y) / (float)(edge_b.x - edge_a.x)
                                : 0.0f;
        ui_filament_path_canvas_set_art_sensors(
            canvas, true, helix::ui::unit_art_draws_prep(s_tray.art_obj), slope, load_fault, n);
    }
    for (int i = 0; i < kAmsDetailMaxSlots; ++i) {
        ui_filament_path_canvas_set_lane_entry_rise(
            canvas, i, i < n ? entry - (int32_t)std::lround(start[i]) : 0);
        ui_filament_path_canvas_set_lane_band_drop(canvas, i, i < n ? art_lane_band_drop() : 0);
    }
    // The canvas reaches up to the edge's highest crossing, so it draws every lane from there.
    // DECLARATIVE_OK: measured layout; the canvas reaches up only under a drawn unit
    lv_obj_set_style_margin_top(canvas, 0, LV_PART_MAIN);
    lv_obj_update_layout(lv_obj_get_parent(canvas));
    lv_area_t cc;
    lv_obj_get_coords(canvas, &cc);
    const int32_t reach = std::max<int32_t>(0, cc.y1 - (int32_t)std::floor(highest) + 1);
    // DECLARATIVE_OK: as above
    lv_obj_set_style_margin_top(canvas, -reach, LV_PART_MAIN);
    lv_obj_refresh_ext_draw_size(lv_obj_get_parent(canvas));
    fill_container(reach);
    lv_obj_update_layout(lv_obj_get_parent(canvas));
    lv_obj_update_layout(s_tray.container);
    return ams_detail_fit_unit_to_path(
        canvas, reach,
        natural_spool_layout(s_tray.unit_index, lv_obj_get_content_width(s_tray.container),
                             s_tray.lane_count, canvas));
}

int32_t helix::ui::ams_detail_spool_size(int32_t available_width, int slot_count) {
    if (!draws_tray() || slot_count <= 0 || available_width <= 0)
        return helix::ui::ams_slot_spool_size();
    const SpoolHeightCap natural =
        natural_spool_layout(s_layout_unit, available_width, slot_count, s_layout_view.get());
    if (s_spool_cap.spool > 0 && same_key(s_spool_cap, natural))
        return std::min(natural.spool, s_spool_cap.spool);
    return natural.spool;
}

AmsSlotLayout helix::ui::ams_detail_slot_layout(int32_t available_width, int slot_count) {
    if (!unit_has_physical_tray())
        return calculate_ams_slot_layout(available_width, slot_count);
    const float spool = (float)helix::ui::ams_detail_spool_size(available_width, slot_count);
    if (spool <= 0)
        return calculate_ams_slot_layout(available_width, slot_count);
    // Flat spools keep the unit's compact row with no box to clear: a ring,
    // its badge slack and a gap apart.
    if (!draws_tray())
        return calculate_ams_slot_layout(available_width, slot_count,
                                         (int32_t)spool + helix::ui::kAmsLaneSpoolBadgeMarginPx +
                                             theme_manager_get_spacing("space_xs"));
    // Spools stand at the real units' pitch, each slot a spool wide, so neighbours
    // overlap. The drawn unit runs from the first lane's end cap to the last's plus
    // the right side face, and is centered in the width.
    const float pitch = tray::spool_pitch(spool);
    const float skew = tray::kDepthSkew * tray::box_depth(spool);
    const float cap = tray::end_cap_width(nullptr, spool);
    AmsSlotLayout layout;
    if (slot_count <= 0 || available_width <= 0)
        return layout;
    layout.slot_width = (int32_t)spool;
    layout.overlap = std::max<int32_t>(0, layout.slot_width - (int32_t)pitch);
    const float drawn = (float)(slot_count - 1) * pitch + 2 * cap + skew;
    const float left = ((float)available_width - drawn) / 2.0f;
    // A spool's front-plane centre sits skew/2 left of its slot's centre.
    layout.centering_offset =
        (int32_t)std::lround(left + cap + skew / 2 - (float)layout.slot_width / 2);
    return layout;
}

void ams_detail_update_labels(AmsDetailWidgets& w, helix::ui::WidgetRef slot_widgets[],
                              int slot_count, const AmsSlotLayout& layout) {
    if (!w.labels_layer || slot_count <= 0)
        return;

    helix::ui::safe_clean_children(w.labels_layer);

    int32_t slot_spacing = layout.slot_width - layout.overlap;
    const int32_t gap = theme_manager_get_spacing("space_xs");
    int32_t label_width = helix::ui::ams_stagger_label_width(
        slot_spacing, helix::widget_size::current_breakpoint(), gap);
    // A drawn unit's labels stand over their own lanes (ams_detail_update_tray() centers
    // each over its spool), keeping to what their row leaves them.
    if (layout.label_rows > 0)
        label_width = layout.label_rows * slot_spacing - gap;

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i]) {
            // Formula matches slot_grid flex positions, plus centering offset
            int32_t center_x = layout.centering_offset + layout.slot_width / 2 + i * slot_spacing;
            ui_ams_slot_move_label_to_layer(slot_widgets[i], w.labels_layer, center_x, label_width);
        }
    }

    spdlog::debug("[AmsDetail] Moved {} labels to overlay layer", slot_count);
}

void ams_detail_update_badges(AmsDetailWidgets& w, helix::ui::WidgetRef slot_widgets[],
                              int slot_count, const AmsSlotLayout& layout) {
    if (!w.badge_layer)
        return;

    // Clean stale badges from previous unit view (badges are reparented here
    // from slot widgets, so they persist across unit switches if not cleaned)
    helix::ui::safe_clean_children(w.badge_layer);

    int32_t slot_spacing = layout.slot_width - layout.overlap;
    lv_obj_update_layout(w.badge_layer);

    for (int i = 0; i < slot_count; ++i) {
        if (slot_widgets[i]) {
            int32_t slot_center_x =
                layout.centering_offset + layout.slot_width / 2 + i * slot_spacing;
            ui_ams_slot_move_badge_to_layer(slot_widgets[i], w.badge_layer, slot_center_x);
        }
    }

    spdlog::debug("[AmsDetail] Moved {} badges to overlay layer", slot_count);
}
