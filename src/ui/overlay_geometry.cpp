// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Overlay panel geometry: the nav-bar width ladder, the width and height each
// overlay class gets, the one site that writes that geometry onto a widget, and
// the ultrawide content width cap applied to overlays and main panels.

#include "ui_breakpoint.h"

#include "content_width_cap.h"
#include "helix-xml/src/xml/lv_xml_style.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_obj_private.h"       // obj->styles: LVGL has no style lookup
#include "lvgl/src/core/lv_obj_style_private.h" // lv_obj_style_t fields
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

namespace helix {

const char* nav_width_suffix(int32_t hor_res, int32_t ver_res) {
    // Nav width is primarily a horizontal concern, but VERTICAL resolution
    // distinguishes micro (480x272) from tiny (480x320), which share a width.
    //
    // Ultrawide displays (e.g. 1920x480) are very wide but short. The nav bar is
    // a full-height vertical strip, so its width must track the short vertical
    // extent — not the horizontal resolution, which would otherwise select the
    // widest 'large' bar and waste the horizontal space the grid wants. Detect
    // ultrawide from the aspect ratio directly rather than via LayoutManager,
    // which is not yet initialised when this runs at startup.
    const bool ultrawide = is_ultrawide_aspect(hor_res, ver_res);
    if (ver_res <= UI_BREAKPOINT_MICRO_MAX)
        return "_micro";
    if (ultrawide)
        // Ultrawide prioritises horizontal content space, so keep the vertical
        // nav strip slim: cap at 'small' and only go narrower on very short
        // panels. (Icons stay legible — they are centred in the strip.)
        return (ver_res <= UI_BREAKPOINT_TINY_MAX) ? "_tiny" : "_small";
    if (hor_res <= 520)
        return "_tiny";
    if (hor_res <= 900)
        return "_small";
    if (hor_res <= 1100)
        return "_medium";
    if (hor_res <= 1400)
        return "_large";
    // The strip keeps climbing above 1400 with the tier that sizes the glyphs
    // inside it, so a 1080p or 4K panel does not get the 132px strip of a
    // 1280x720 one around a 128px glyph.
    if (hor_res <= 1600)
        return "_xlarge";
    return "_xxlarge";
}

OverlayWidths compute_overlay_widths(int32_t hor_res, int32_t ver_res, int32_t nav_width,
                                     int32_t gap) {
    // Classified from raw dimensions rather than LayoutManager::type(): this
    // runs in Application phase 6 and the layout manager is not initialised
    // until phase 8b. detect_layout_type() is the same function that sets
    // ui_is_portrait, so the sizing and the portrait layout cannot
    // disagree. A LayoutManager::set_override() forcing a portrait *layout*
    // onto landscape hardware is deliberately not honoured here — the physical
    // nav bar geometry is what the arithmetic is about.
    const bool portrait = is_portrait_layout(detect_layout_type(hor_res, ver_res));
    if (portrait) {
        // Portrait's nav bar is a bottom strip (compute_overlay_heights), not a
        // side navbar, so it costs an overlay nothing horizontally — and neither
        // does the "you will return from this" gap: that gap belongs on the
        // axis the nav bar occupies. Both classes are full width; the gap is
        // carried by compute_overlay_heights instead. An override in
        // ui_set_overlay_geometry would leave this function stating something
        // false, so the rule lives here, not downstream.
        return {hor_res, hor_res};
    }
    return {hor_res - nav_width - gap, hor_res - nav_width};
}

OverlayHeights compute_overlay_heights(int32_t hor_res, int32_t ver_res, int32_t nav_height,
                                       int32_t gap) {
    // Same classification as compute_overlay_widths, and for the same reason:
    // this can run before LayoutManager::init(), and the threshold that sets
    // ui_is_portrait must never disagree with the one that sizes overlays.
    //
    // Landscape reserves nothing vertically — its nav bar is a full-HEIGHT
    // strip at the leading edge, so overlays span the whole display and the
    // gap is spent horizontally instead.
    const bool portrait = is_portrait_layout(detect_layout_type(hor_res, ver_res));
    if (!portrait) {
        return {ver_res, ver_res};
    }
    return {ver_res - nav_height - gap, ver_res - nav_height};
}

} // namespace helix

// DECLARATIVE_OK: overlay placement is navigation chrome, resolved at push time
// from the live stack — there is no XML expression for "the class this overlay
// got depends on what pushed it". check_imperative_ui.py deliberately excludes
// geometry and layout properties for exactly this reason; see its
// APPEARANCE_PROPS comment. This is the single site that writes overlay
// geometry, which is why 17 overlay XML roots need no portrait variant.
void ui_set_overlay_geometry(lv_obj_t* obj, bool is_destination) {
    if (!obj) {
        spdlog::warn("[Theme] ui_set_overlay_geometry: NULL pointer");
        return;
    }

    lv_obj_t* screen = lv_obj_get_screen(obj);
    const int32_t screen_width = screen ? lv_obj_get_width(screen) : 800;
    const int32_t screen_height = screen ? lv_obj_get_height(screen) : 480;
    const bool portrait =
        helix::is_portrait_layout(helix::detect_layout_type(screen_width, screen_height));

    const char* name = is_destination ? "overlay_width_destination" : "overlay_width_transient";
    const char* width_str = lv_xml_get_const(nullptr, name);
    if (width_str) {
        lv_obj_set_width(obj, std::atoi(width_str));
    } else {
        // Theme not initialized yet — estimate from the screen. Same derivation
        // as theme_manager_register_responsive_spacing(), with medium-breakpoint
        // fallbacks for nav_width and the gap.
        const helix::OverlayWidths widths =
            helix::compute_overlay_widths(screen_width, screen_height, 94, 16);
        lv_obj_set_width(obj, is_destination ? widths.destination : widths.transient);
        spdlog::warn("[Theme] {} not registered, using fallback", name);
    }

    // Landscape leaves height and alignment to XML (height="100%"
    // align="right_mid"). Only portrait overrides them, because there the nav
    // bar is a bottom strip and a full-height overlay would cover it.
    if (!portrait) {
        return;
    }

    const char* nav_h_str = lv_xml_get_const(nullptr, "button_height_lg");
    const char* gap_str = lv_xml_get_const(nullptr, "space_lg");
    const int32_t nav_height = nav_h_str ? std::atoi(nav_h_str) : 70;
    const int32_t gap = gap_str ? std::atoi(gap_str) : 16;

    const helix::OverlayHeights heights =
        helix::compute_overlay_heights(screen_width, screen_height, nav_height, gap);
    lv_obj_set_height(obj, is_destination ? heights.destination : heights.transient);
    lv_obj_set_align(obj, LV_ALIGN_TOP_MID);
}

namespace helix {

int32_t live_content_max_width(lv_obj_t* obj) {
    if (const char* cap_str = lv_xml_get_const(nullptr, "content_max_width")) {
        return std::atoi(cap_str);
    }
    lv_obj_t* screen = obj ? lv_obj_get_screen(obj) : nullptr;
    return content_max_width(screen ? lv_obj_get_height(screen) : 480);
}

const lv_style_t* cap_chrome_style() {
    const lv_xml_style_t* xs = lv_xml_get_style_by_name(nullptr, "styles.cap_chrome");
    return xs ? &xs->style : nullptr;
}

int32_t content_cap_base_pad(lv_obj_t* root) {
    return lv_obj_get_style_pad_right(root, LV_PART_MAIN);
}

namespace {

bool is_cap_chrome(const lv_obj_t* obj, const lv_style_t* chrome) {
    for (uint32_t i = 0; i < obj->style_cnt; i++) {
        if (obj->styles[i].style == chrome) {
            return true;
        }
    }
    return false;
}

// Chrome keeps the root's full width while the content beside it is capped:
// it is widened to reach into the cap's padding, and a matching negative right
// margin keeps that out of the root's scroll extent. Chrome the cap never
// widened is left exactly as its XML built it.
void span_chrome(lv_obj_t* chrome, int32_t full_content_width, int32_t overflow) {
    const bool widened = lv_obj_get_style_margin_right(chrome, LV_PART_MAIN) < 0;
    if (overflow == 0) {
        if (widened) {
            lv_obj_set_width(chrome, lv_pct(100));
            lv_obj_set_style_margin_right(chrome, 0, LV_PART_MAIN);
        }
        return;
    }
    lv_obj_set_width(chrome, full_content_width);
    lv_obj_set_style_margin_right(chrome, -overflow, LV_PART_MAIN);
}

// The width the cap compares against: the root's own pixel width when it has
// one (an overlay's class width), otherwise its laid-out width.
int32_t full_width(lv_obj_t* root) {
    const int32_t style_width = lv_obj_get_style_width(root, LV_PART_MAIN);
    if (LV_COORD_IS_PX(style_width)) {
        return style_width;
    }
    lv_obj_update_layout(root);
    return lv_obj_get_width(root);
}

} // namespace

// DECLARATIVE_OK: the cap depends on the screen, the view's declared opt-out
// and, for overlays, the width class resolved at push time.
void apply_content_cap(lv_obj_t* root, int32_t base_pad_right, bool capped) {
    if (!root) {
        return;
    }
    const int32_t width = full_width(root);
    const int32_t overflow = capped ? content_cap_overflow(width, live_content_max_width(root)) : 0;

    if (const lv_style_t* chrome = cap_chrome_style()) {
        const int32_t full_content_width = width - lv_obj_get_style_pad_left(root, LV_PART_MAIN) -
                                           base_pad_right -
                                           2 * lv_obj_get_style_border_width(root, LV_PART_MAIN);
        const uint32_t count = lv_obj_get_child_count(root);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* child = lv_obj_get_child(root, static_cast<int32_t>(i));
            if (is_cap_chrome(child, chrome)) {
                span_chrome(child, full_content_width, overflow);
            }
        }
    }

    const int32_t pad_right = base_pad_right + overflow;
    // A view that fits keeps whatever its XML or theme gives it; the local
    // style is written only to add the overflow or to take it away again.
    if (lv_obj_get_style_pad_right(root, LV_PART_MAIN) == pad_right) {
        return;
    }
    lv_obj_set_style_pad_right(root, pad_right, LV_PART_MAIN);
    spdlog::trace("[Theme] {} content cap: pad_right {} (overflow {})", (void*)root, pad_right,
                  overflow);
}

} // namespace helix
