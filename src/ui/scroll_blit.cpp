// SPDX-License-Identifier: GPL-3.0-or-later
#include "scroll_blit.h"

#include "lvgl/src/core/lv_obj_draw_private.h"   // lv_obj_get_layer_type
#include "lvgl/src/core/lv_obj_private.h"        // obj->coords
#include "lvgl/src/core/lv_refr_private.h"       // lv_refr_get_top_obj
#include "lvgl/src/display/lv_display_private.h" // inv_areas, prev_scr
#include "lvgl/src/misc/lv_area_private.h"       // lv_area_intersect
#include "lvgl/src/misc/lv_event_private.h"      // lv_event_dsc_t::filter

#include <cstring>

namespace helix {

namespace {

struct ScrollTrack {
    int32_t x;
    int32_t y;
};

struct State {
    lv_display_t* disp = nullptr;
    RetainedFrame frame;
    // The scroll whose invalidation has not arrived yet: LVGL sends
    // LV_EVENT_SCROLL, then invalidates the whole scroller.
    lv_obj_t* pending_obj = nullptr;
    int32_t pending_dy = 0;
    lv_area_t pending_area{};
    bool in_invalidate = false;
    // Moves decided since the last render, applied as it starts drawing: after
    // layout, which can scroll too, and without holding the frame from the
    // presenter for longer than the render itself.
    struct Shift {
        lv_area_t region;
        int32_t dy;
    };
    static constexpr int kMaxShifts = 4;
    Shift shifts[kMaxShifts];
    int n_shifts = 0;
    // Regions a refused frame left unmoved, redrawn whole by the next refresh.
    Shift repairs[kMaxShifts];
    int n_repairs = 0;
};

State s_state;

bool areas_equal(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

bool clip(lv_area_t* a, const lv_area_t& by) {
    return lv_area_intersect(a, a, &by);
}

bool is_visible(lv_obj_t* o) {
    return !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/// The area `o` paints, its shadow or outline included.
lv_area_t paint_area(lv_obj_t* o) {
    lv_area_t a = o->coords;
    lv_area_increase(&a, lv_obj_get_ext_draw_size(o), lv_obj_get_ext_draw_size(o));
    return a;
}

/// Widgets that paint inside the region without moving with it. Their pixels
/// are redrawn where they are and where the copy carried them; past a few, or
/// past half the region, rendering in full is cheaper.
struct Statics {
    static constexpr int kMax = 8;
    lv_area_t areas[kMax];
    int n = 0;
    uint64_t px = 0;

    /// False when `o` makes the blit not worth it.
    bool add(lv_obj_t* o, const lv_area_t& r) {
        return !is_visible(o) || add_area(paint_area(o), r);
    }

    bool add_area(lv_area_t a, const lv_area_t& r) {
        if (!lv_area_intersect(&a, &a, &r))
            return true;
        if (n == kMax)
            return false;
        areas[n++] = a;
        px += lv_area_get_size(&a);
        return px * 2 <= lv_area_get_size(&r);
    }
};

bool has_draw_handler(lv_obj_t* o) {
    uint32_t n = lv_obj_get_event_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_event_dsc_t* dsc = lv_obj_get_event_dsc(o, i);
        uint32_t code = dsc->filter & ~static_cast<uint32_t>(LV_EVENT_PREPROCESS);
        if (code == LV_EVENT_ALL ||
            (code >= LV_EVENT_DRAW_MAIN_BEGIN && code <= LV_EVENT_DRAW_TASK_ADDED)) {
            return true;
        }
    }
    return false;
}

bool border_reaches(lv_obj_t* o, const lv_area_t& r);

/// Whether `o` paints the same pixels at every row of `r`, so pixels moved
/// within `r` land on the same background they left.
bool background_is_uniform(lv_obj_t* o, const lv_area_t& r) {
    if (lv_obj_get_style_bg_opa(o, LV_PART_MAIN) > LV_OPA_TRANSP) {
        if (lv_obj_get_style_bg_grad_dir(o, LV_PART_MAIN) != LV_GRAD_DIR_NONE)
            return false;
        int32_t radius = lv_obj_get_style_radius(o, LV_PART_MAIN);
        if (radius > 0 && !lv_area_is_in(&r, &o->coords, radius))
            return false;
    }
    if (lv_obj_get_style_bg_image_src(o, LV_PART_MAIN) != nullptr &&
        lv_obj_get_style_bg_image_opa(o, LV_PART_MAIN) > LV_OPA_TRANSP)
        return false;
    return !border_reaches(o, r);
}

/// Whether `o`'s border is drawn inside `r`.
bool border_reaches(lv_obj_t* o, const lv_area_t& r) {
    int32_t bw = lv_obj_get_style_border_width(o, LV_PART_MAIN);
    if (bw <= 0 || lv_obj_get_style_border_opa(o, LV_PART_MAIN) == LV_OPA_TRANSP ||
        lv_obj_get_style_border_side(o, LV_PART_MAIN) == LV_BORDER_SIDE_NONE)
        return false;
    lv_area_t inner = o->coords;
    lv_area_increase(&inner, -bw, -bw);
    return !lv_area_is_in(&r, &inner, 0);
}

/// What an enclosing widget draws over its children reaches into `r` and stays
/// put: its scrollbars become static areas; a post-drawn border or a custom
/// draw handler, whose extent is unknown, rules the blit out.
bool overlays_allow_blit(lv_obj_t* o, const lv_area_t& r, Statics& statics) {
    if (has_draw_handler(o))
        return false;
    if (lv_obj_get_style_border_post(o, LV_PART_MAIN) && border_reaches(o, r))
        return false;
    lv_area_t hor, ver;
    lv_obj_get_scrollbar_area(o, &hor, &ver);
    return (lv_area_get_width(&hor) <= 0 || statics.add_area(hor, r)) &&
           (lv_area_get_width(&ver) <= 0 || statics.add_area(ver, r));
}

bool composited(lv_obj_t* o) {
    return lv_obj_get_layer_type(o) != LV_LAYER_TYPE_NONE ||
           lv_obj_get_style_opa(o, LV_PART_MAIN) < LV_OPA_COVER;
}

bool add_layer_children(Statics& statics, lv_obj_t* layer, const lv_area_t& r) {
    if (!layer || !is_visible(layer))
        return true;
    uint32_t n = lv_obj_get_child_count(layer);
    for (uint32_t i = 0; i < n; i++) {
        if (!statics.add(lv_obj_get_child(layer, static_cast<int32_t>(i)), r))
            return false;
    }
    return true;
}

void invalidate_clipped(lv_display_t* disp, lv_area_t a, const lv_area_t& r) {
    if (clip(&a, r))
        lv_inv_area(disp, &a);
}

/// A static part was copied along with the content: both where it is drawn
/// and where its old pixels landed need drawing again.
void invalidate_static(lv_display_t* disp, lv_area_t a, const lv_area_t& r, int32_t dy) {
    invalidate_clipped(disp, a, r);
    lv_area_move(&a, 0, dy);
    invalidate_clipped(disp, a, r);
}

/// Everything in `r` that does not move with the content: the scroller's own
/// rounded corners and border, and its scrollbars.
void invalidate_static_parts(lv_display_t* disp, lv_obj_t* obj, const lv_area_t& r, int32_t dy) {
    const lv_area_t& c = obj->coords;
    int32_t edge = lv_obj_get_style_radius(obj, LV_PART_MAIN);
    int32_t bw = 0;
    if (lv_obj_get_style_border_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP)
        bw = lv_obj_get_style_border_width(obj, LV_PART_MAIN);
    if (bw > edge)
        edge = bw;
    if (edge > 0) {
        invalidate_static(disp, {c.x1, c.y1, c.x2, c.y1 + edge - 1}, r, dy);
        invalidate_static(disp, {c.x1, c.y2 - edge + 1, c.x2, c.y2}, r, dy);
    }
    if (bw > 0) {
        invalidate_static(disp, {c.x1, c.y1, c.x1 + bw - 1, c.y2}, r, dy);
        invalidate_static(disp, {c.x2 - bw + 1, c.y1, c.x2, c.y2}, r, dy);
    }

    if (lv_obj_get_scrollbar_mode(obj) == LV_SCROLLBAR_MODE_OFF)
        return;
    // The bars' whole tracks, from the scrollbar styles: a bar shown or hidden
    // by this scroll is covered either way.
    const int32_t sb_w = lv_obj_get_style_width(obj, LV_PART_SCROLLBAR);
    const int32_t sb_x2 = c.x2 - lv_obj_get_style_pad_right(obj, LV_PART_SCROLLBAR);
    const int32_t sb_y2 = c.y2 - lv_obj_get_style_pad_bottom(obj, LV_PART_SCROLLBAR);
    lv_area_t col = {sb_x2 - sb_w - 1, c.y1, sb_x2 + 1, c.y2};
    lv_area_t row = {c.x1, sb_y2 - sb_w - 1, c.x2, sb_y2 + 1};
    lv_area_t hor, ver;
    lv_obj_get_scrollbar_area(obj, &hor, &ver);
    if (lv_area_get_width(&ver) > 0)
        lv_area_join(&col, &col, &ver);
    if (lv_area_get_width(&hor) > 0)
        lv_area_join(&row, &row, &hor);
    invalidate_static(disp, col, r, dy);
    invalidate_static(disp, row, r, dy);
}

/// The area `lv_obj_invalidate(obj)` hands to the display.
bool invalidation_area(lv_obj_t* obj, lv_area_t* out) {
    *out = paint_area(obj);
    if (!lv_obj_area_is_visible(obj, out))
        return false;
    lv_display_t* disp = lv_obj_get_display(obj);
    lv_area_t scr = {0, 0, lv_display_get_horizontal_resolution(disp) - 1,
                     lv_display_get_vertical_resolution(disp) - 1};
    return clip(out, scr);
}

/// The region whose pixels move with `obj`'s content, and the widgets painting
/// in it that stay put. False when the blit cannot be right or would not pay.
bool plan_blit(lv_obj_t* obj, lv_area_t* out, Statics& statics) {
    lv_display_t* disp = lv_obj_get_display(obj);
    if (!disp || disp->prev_scr || lv_display_get_rotation(disp) != LV_DISPLAY_ROTATION_0)
        return false;
    if (obj->class_p != &lv_obj_class || !is_visible(obj) || has_draw_handler(obj) ||
        lv_obj_has_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE) ||
        lv_obj_get_style_base_dir(obj, LV_PART_MAIN) == LV_BASE_DIR_RTL)
        return false;
    lv_obj_t* screen = lv_display_get_screen_active(disp);
    if (lv_obj_get_screen(obj) != screen)
        return false;

    // Where the content is drawn: the scroller clipped by every ancestor that clips.
    lv_area_t r = obj->coords;
    for (lv_obj_t* o = obj; o; o = lv_obj_get_parent(o)) {
        if (!is_visible(o) || composited(o))
            return false;
        lv_obj_t* parent = lv_obj_get_parent(o);
        if (parent && !lv_obj_has_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE) &&
            !clip(&r, parent->coords))
            return false;
    }
    lv_area_t scr = {0, 0, lv_display_get_horizontal_resolution(disp) - 1,
                     lv_display_get_vertical_resolution(disp) - 1};
    if (!clip(&r, scr))
        return false;

    // Floating children stay put while the rest scrolls.
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING) && !statics.add(child, r))
            return false;
    }

    // LVGL draws `r` starting from the topmost widget that covers it; nothing
    // beneath that one shows. A cover inside the scroller moves with it.
    lv_obj_t* top = lv_refr_get_top_obj(&r, screen);
    if (!top)
        return false;
    for (lv_obj_t* o = top; o; o = lv_obj_get_parent(o)) {
        if (o == obj) {
            top = obj;
            break;
        }
    }
    bool top_is_ancestor = false;
    for (lv_obj_t* o = lv_obj_get_parent(obj); o; o = lv_obj_get_parent(o)) {
        if (o == top)
            top_is_ancestor = true;
    }
    if (top != obj && !top_is_ancestor)
        return false;

    // The scroller's own corners and border are redrawn as strips; the rest of
    // its background must look the same at every row.
    if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) > LV_OPA_TRANSP &&
        lv_obj_get_style_bg_grad_dir(obj, LV_PART_MAIN) != LV_GRAD_DIR_NONE)
        return false;
    if (lv_obj_get_style_bg_image_src(obj, LV_PART_MAIN) != nullptr)
        return false;

    // From the scroller up to the cover, everything drawn in `r` besides the
    // scroller's subtree must be a uniform background; above the cover, only
    // what is drawn later (younger siblings) can reach `r`. Every enclosing
    // widget, cover or not, draws its scrollbars and overlays after its children.
    bool below_cover = top_is_ancestor;
    for (lv_obj_t* o = obj; o != screen; o = lv_obj_get_parent(o)) {
        lv_obj_t* parent = lv_obj_get_parent(o);
        const int32_t idx = lv_obj_get_index(o);
        const int32_t count = static_cast<int32_t>(lv_obj_get_child_count(parent));
        for (int32_t i = below_cover ? 0 : idx + 1; i < count; i++) {
            if (i != idx && !statics.add(lv_obj_get_child(parent, i), r))
                return false;
        }
        if (below_cover && !background_is_uniform(parent, r))
            return false;
        if (!overlays_allow_blit(parent, r, statics))
            return false;
        if (parent == top)
            below_cover = false;
    }
    if (!add_layer_children(statics, lv_display_get_layer_top(disp), r) ||
        !add_layer_children(statics, lv_display_get_layer_sys(disp), r))
        return false;

    *out = r;
    return true;
}

/// Whether the scroll that moved `obj`'s content by `dy` was blitted; on true,
/// `area` (the scroller's own invalidation) has become the exposed strip.
bool blit(lv_obj_t* obj, int32_t dy, lv_area_t* area) {
    lv_display_t* disp = s_state.disp;
    lv_area_t r;
    Statics statics;
    if (!plan_blit(obj, &r, statics))
        return false;
    if (LV_ABS(dy) >= lv_area_get_height(&r))
        return false;

    // Copy first: lv_inv_area below appends to this list.
    const int32_t inv_n = disp->inv_p;
    lv_area_t inv[LV_INV_BUF_SIZE];
    std::memcpy(inv, disp->inv_areas, sizeof(lv_area_t) * static_cast<size_t>(inv_n));
    for (int32_t i = 0; i < inv_n; i++) {
        // Already being redrawn whole: moving its pixels would buy nothing.
        if (lv_area_is_in(&r, &inv[i], 0))
            return false;
    }

    if (!s_state.frame.claim_rows || s_state.n_shifts == State::kMaxShifts)
        return false;
    s_state.shifts[s_state.n_shifts++] = {r, dy};

    // Stale pixels already awaiting a redraw moved with the rest.
    for (int32_t i = 0; i < inv_n; i++) {
        lv_area_t moved;
        if (!lv_area_intersect(&moved, &inv[i], &r))
            continue;
        lv_area_move(&moved, 0, dy);
        invalidate_clipped(disp, moved, r);
    }
    invalidate_static_parts(disp, obj, r, dy);
    for (int i = 0; i < statics.n; i++)
        invalidate_static(disp, statics.areas[i], r, dy);

    if (dy > 0)
        *area = {r.x1, r.y1, r.x2, r.y1 + dy - 1};
    else
        *area = {r.x1, r.y2 + dy + 1, r.x2, r.y2};
    return true;
}

void on_scroll(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* track = static_cast<ScrollTrack*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (s_state.pending_obj == obj)
            s_state.pending_obj = nullptr;
        delete track;
        return;
    }
    const int32_t x = lv_obj_get_scroll_x(obj);
    const int32_t y = lv_obj_get_scroll_y(obj);
    // Content moves opposite to the scroll position.
    const int32_t dx = track->x - x;
    const int32_t dy = track->y - y;
    track->x = x;
    track->y = y;
    if (!s_state.disp || dx != 0 || dy == 0 || lv_obj_get_display(obj) != s_state.disp)
        return;
    lv_area_t a;
    if (!invalidation_area(obj, &a))
        return;
    s_state.pending_obj = obj;
    s_state.pending_dy = dy;
    s_state.pending_area = a;
}

bool tracked(lv_obj_t* obj) {
    uint32_t n = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == on_scroll)
            return true;
    }
    return false;
}

/// A scroller is tracked from its first drag on; the drag's first frame
/// renders in full, since the scroll position it started from is unknown.
void track_dragged_scrollers() {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_obj_t* obj = lv_indev_get_scroll_obj(indev))
            scroll_blit_track(obj);
    }
}

/// Moves the frame's pixels for this render's scrolls. The areas to draw are
/// fixed by now, so a region whose frame cannot be taken shows one frame of
/// stale pixels and is redrawn whole by the next refresh.
void apply_shifts() {
    const int n = s_state.n_shifts;
    s_state.n_shifts = 0;
    if (n == 0)
        return;
    const uint32_t px_bytes = lv_color_format_get_size(lv_display_get_color_format(s_state.disp));
    for (int i = 0; i < n; i++) {
        const State::Shift& sh = s_state.shifts[i];
        if (s_state.frame.claim_rows(sh.region.y1, sh.region.y2)) {
            shift_rows(s_state.frame.buf, s_state.frame.stride, px_bytes, sh.region, sh.dy,
                       s_state.frame.scratch, s_state.frame.scratch_bytes);
        } else {
            s_state.repairs[s_state.n_repairs++] = sh;
        }
    }
}

void apply_repairs() {
    const int n = s_state.n_repairs;
    s_state.n_repairs = 0;
    for (int i = 0; i < n; i++)
        lv_inv_area(s_state.disp, &s_state.repairs[i].region);
}

void on_display_event(lv_event_t* e) {
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:
        s_state.pending_obj = nullptr;
        return;
    case LV_EVENT_RENDER_START:
        apply_shifts();
        return;
    case LV_EVENT_REFR_READY:
        apply_repairs();
        return;
    default:
        break;
    }
    if (s_state.in_invalidate)
        return;
    s_state.in_invalidate = true;
    auto* area = static_cast<lv_area_t*>(lv_event_get_param(e));
    if (s_state.pending_obj && areas_equal(*area, s_state.pending_area)) {
        lv_obj_t* obj = s_state.pending_obj;
        s_state.pending_obj = nullptr;
        blit(obj, s_state.pending_dy, area);
    }
    track_dragged_scrollers();
    s_state.in_invalidate = false;
}

} // namespace

void shift_rows(uint8_t* buf, size_t stride, uint32_t px_bytes, const lv_area_t& area, int32_t dy,
                uint8_t* scratch, size_t scratch_bytes) {
    const int32_t first = dy > 0 ? area.y1 + dy : area.y1; // destination rows
    const int32_t last = dy > 0 ? area.y2 : area.y2 + dy;
    if (dy == 0 || last < first)
        return;
    const size_t row_bytes = static_cast<size_t>(lv_area_get_width(&area)) * px_bytes;
    const size_t x_off = static_cast<size_t>(area.x1) * px_bytes;
    int32_t chunk = scratch ? static_cast<int32_t>(scratch_bytes / row_bytes) : 0;
    auto row = [&](int32_t y) { return buf + static_cast<size_t>(y) * stride + x_off; };

    // Each destination row's source has not been overwritten yet when rows are
    // walked away from the direction of travel.
    if (chunk < 1) {
        for (int32_t i = 0; i <= last - first; i++) {
            int32_t d = dy > 0 ? last - i : first + i;
            std::memcpy(row(d), row(d - dy), row_bytes);
        }
        return;
    }
    for (int32_t i = 0; i <= last - first; i += chunk) {
        int32_t n = LV_MIN(chunk, last - first - i + 1);
        int32_t d0 = dy > 0 ? last - i - n + 1 : first + i;
        for (int32_t k = 0; k < n; k++)
            std::memcpy(scratch + static_cast<size_t>(k) * row_bytes, row(d0 + k - dy), row_bytes);
        for (int32_t k = 0; k < n; k++)
            std::memcpy(row(d0 + k), scratch + static_cast<size_t>(k) * row_bytes, row_bytes);
    }
}

void scroll_blit_track(lv_obj_t* obj) {
    if (tracked(obj))
        return;
    // DECLARATIVE_OK: scroll position tracking has no declarative equivalent.
    auto* track = new ScrollTrack{lv_obj_get_scroll_x(obj), lv_obj_get_scroll_y(obj)};
    lv_obj_add_event_cb(obj, on_scroll, LV_EVENT_SCROLL, track);
    lv_obj_add_event_cb(obj, on_scroll, LV_EVENT_DELETE, track);
}

bool scroll_blit_region(lv_obj_t* obj, lv_area_t* out) {
    Statics statics;
    return plan_blit(obj, out, statics);
}

void scroll_blit_install(lv_display_t* disp, const RetainedFrame& frame) {
    scroll_blit_uninstall();
    s_state.disp = disp;
    s_state.frame = frame;
    lv_display_add_event_cb(disp, on_display_event, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_add_event_cb(disp, on_display_event, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(disp, on_display_event, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(disp, on_display_event, LV_EVENT_REFR_READY, nullptr);
}

void scroll_blit_uninstall() {
    if (s_state.disp) {
        lv_display_remove_event_cb_with_user_data(s_state.disp, on_display_event, nullptr);
    }
    s_state = State{};
}

} // namespace helix
