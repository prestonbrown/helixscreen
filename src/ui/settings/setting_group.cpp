// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "setting_group.h"

#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstring>

static uint32_t setting_group_visible_child_count(lv_obj_t* group) {
    uint32_t count = 0;
    uint32_t n = lv_obj_get_child_count(group);
    for (uint32_t i = 0; i < n; i++) {
        if (!lv_obj_has_flag(lv_obj_get_child(group, i), LV_OBJ_FLAG_HIDDEN))
            count++;
    }
    return count;
}

uint32_t setting_group_divider_count(lv_obj_t* group) {
    if (!group)
        return 0;
    uint32_t visible = setting_group_visible_child_count(group);
    return visible > 0 ? visible - 1 : 0;
}

// Pure render: draws a 1px border-colored line at the top edge of every visible
// child except the first visible one. Read-only over the child list — never
// mutates state (threading-safe inside the draw pipeline).
static void setting_group_draw_dividers(lv_event_t* e) {
    lv_obj_t* group = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!group || !layer)
        return;

    lv_area_t coords;
    lv_obj_get_coords(group, &coords);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = theme_manager_get_color("border");
    dsc.width = 1;
    dsc.opa = LV_OPA_COVER;

    uint32_t n = lv_obj_get_child_count(group);
    bool seen_visible = false;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(group, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN))
            continue;
        if (!seen_visible) {
            seen_visible = true; // no line above the first visible child
            continue;
        }
        lv_area_t c;
        lv_obj_get_coords(child, &c);
        dsc.p1.x = coords.x1;
        dsc.p1.y = c.y1;
        dsc.p2.x = coords.x2;
        dsc.p2.y = c.y1;
        lv_draw_line(layer, &dsc);
    }
}

static void setting_group_track_focus(lv_obj_t* group, lv_obj_t* obj);
static void setting_group_sync_focus_clip(lv_obj_t* group);

// setting_group_header's root carries this name, which is how a group tells
// its header from its rows.
static constexpr const char* kHeaderName = "setting_group_header";

// A group that shows no row hides its header and collapses its card chrome
// (LV_STATE_USER_1), so a section whose every row is gated off disappears.
// LVGL sends no event when a child's hidden flag flips, but any row appearing
// or disappearing changes the group's content height, so SIZE_CHANGED is where
// this runs. A row counts only while laid out with some height: a visible
// wrapper whose own contents are all hidden is not a row, which holds only
// while wrappers carry no padding: a padded wrapper keeps its height when
// empty and reads as a row, so section wrappers are style_pad_all="0". Rows
// stay in the layout while the card is collapsed, so a row returning still
// resizes it.
static void setting_group_sync_header(lv_event_t* e) {
    lv_obj_t* group = lv_event_get_target_obj(e);
    // Rows arrive and move with size changes; see setting_group_track_focus.
    setting_group_track_focus(group, group);
    setting_group_sync_focus_clip(group);
    lv_obj_t* header = nullptr;
    bool has_row = false;
    uint32_t n = lv_obj_get_child_count(group);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(group, i);
        const char* name = lv_obj_get_name(child);
        if (!header && name && strcmp(name, kHeaderName) == 0) {
            header = child;
        } else if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_height(child) > 0) {
            has_row = true;
        }
    }
    if (!header) {
        return;
    }
    lv_obj_set_flag(header, LV_OBJ_FLAG_HIDDEN, !has_row);
    lv_obj_set_state(group, LV_STATE_USER_1, !has_row);
}

// True when a focused descendant reaches into one of the group's rounded
// corners. A ring on a middle row never touches a corner, and LVGL focuses the
// row under every scroll drag, so focus alone would keep the clip on nearly
// always.
static bool setting_group_focus_in_corner(lv_obj_t* obj, const lv_area_t& g, int32_t r) {
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (lv_obj_has_state(child, LV_STATE_FOCUSED)) {
            lv_area_t c;
            lv_obj_get_coords(child, &c);
            const bool near_x = c.x1 < g.x1 + r || c.x2 > g.x2 - r;
            const bool near_y = c.y1 < g.y1 + r || c.y2 > g.y2 - r;
            if (near_x && near_y)
                return true;
        }
        if (setting_group_focus_in_corner(child, g, r))
            return true;
    }
    return false;
}

static void setting_group_sync_focus_clip(lv_obj_t* group) {
    lv_area_t g;
    lv_obj_get_coords(group, &g);
    const int32_t short_side = LV_MIN(lv_area_get_width(&g), lv_area_get_height(&g));
    const int32_t r = LV_MIN(lv_obj_get_style_radius(group, LV_PART_MAIN), short_side / 2);
    lv_obj_set_state(group, LV_STATE_USER_2, r > 0 && setting_group_focus_in_corner(group, g, r));
}

// A focused row's ring runs to the card's edge, so a row in a corner needs the
// card's rounded corners clipped onto it.
static void setting_group_row_state_changed(lv_event_t* e) {
    setting_group_sync_focus_clip(static_cast<lv_obj_t*>(lv_event_get_user_data(e)));
}

// LVGL tells no parent when a descendant gains focus, so each clickable
// descendant reports its own state changes.
static void setting_group_track_focus(lv_obj_t* group, lv_obj_t* obj) {
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            bool tracked = false;
            uint32_t events = lv_obj_get_event_count(child);
            for (uint32_t k = 0; k < events && !tracked; k++) {
                tracked = lv_event_dsc_get_cb(lv_obj_get_event_dsc(child, k)) ==
                          setting_group_row_state_changed;
            }
            if (!tracked) {
                lv_obj_add_event_cb(child, setting_group_row_state_changed, LV_EVENT_STATE_CHANGED,
                                    group);
            }
        }
        setting_group_track_focus(group, child);
    }
}

static lv_style_t* setting_group_focus_clip_style() {
    static lv_style_t style;
    static bool initialized = false;
    if (!initialized) {
        lv_style_init(&style);
        lv_style_set_clip_corner(&style, true);
        initialized = true;
    }
    return &style;
}

static lv_style_t* setting_group_collapsed_style() {
    static lv_style_t style;
    static bool initialized = false;
    if (!initialized) {
        lv_style_init(&style);
        lv_style_set_margin_bottom(&style, 0);
        lv_style_set_border_width(&style, 0);
        lv_style_set_outline_width(&style, 0);
        lv_style_set_shadow_width(&style, 0);
        lv_style_set_bg_opa(&style, LV_OPA_TRANSP);
        initialized = true;
    }
    return &style;
}

static void* setting_group_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);
    if (!obj) {
        spdlog::error("[SettingGroup] Failed to create lv_obj");
        return nullptr;
    }

    // Card shell: card_bg fill + theme border + theme border_radius (reactive).
    // Strip the LVGL theme's LV_PART_MAIN styles first so the shared style wins.
    lv_obj_remove_style(obj, nullptr, LV_PART_MAIN);
    lv_obj_add_style(obj, ThemeManager::instance().get_style(StyleRole::Card), LV_PART_MAIN);

    // Clip children to the rounded rect only while a focus ring reaches a corner
    // (LV_STATE_USER_2, see setting_group_sync_focus_clip): rows draw nothing else
    // there, and the clip renders the card's corner bands through extra layers.
    lv_obj_add_style(obj, setting_group_focus_clip_style(), LV_PART_MAIN | LV_STATE_USER_2);

    // Full width; rows own their own internal padding, so the card has none.
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(obj, 0, LV_PART_MAIN); // L055: flex gaps are separate from pad_all
    lv_obj_set_style_pad_column(obj, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);

    // Responsive inset: side margins float the card; bottom margin separates groups.
    // theme_manager_get_spacing() returns the value for the current breakpoint.
    int32_t side = theme_manager_get_spacing("space_sm");
    int32_t gap = theme_manager_get_spacing("space_md");
    lv_obj_set_style_margin_left(obj, side, LV_PART_MAIN);
    lv_obj_set_style_margin_right(obj, side, LV_PART_MAIN);
    lv_obj_set_style_margin_bottom(obj, gap, LV_PART_MAIN);

    // Fixed container, not a scroll area (the overlay content scrolls).
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    spdlog::trace("[SettingGroup] Created <setting_group> with card shell");

    // Auto-managed dividers between visible children (pure render).
    lv_obj_add_event_cb(obj, setting_group_draw_dividers, LV_EVENT_DRAW_POST, nullptr);

    // Hide the header while no row shows (see setting_group_sync_header).
    lv_obj_add_style(obj, setting_group_collapsed_style(), LV_PART_MAIN | LV_STATE_USER_1);
    lv_obj_add_event_cb(obj, setting_group_sync_header, LV_EVENT_SIZE_CHANGED, nullptr);

    return (void*)obj;
}

void setting_group_register(void) {
    lv_xml_register_widget("setting_group", setting_group_xml_create, lv_xml_obj_apply);
    spdlog::trace("[SettingGroup] Registered <setting_group> widget");
}
