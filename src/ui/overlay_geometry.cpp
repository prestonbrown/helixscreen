// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Overlay panel geometry: the nav-bar width ladder, the width and height each
// overlay class gets, and the one site that writes that geometry onto a widget.

#include "ui_breakpoint.h"

#include "layout_manager.h"
#include "lvgl/lvgl.h"
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
    const lv_coord_t screen_width = screen ? lv_obj_get_width(screen) : 800;
    const lv_coord_t screen_height = screen ? lv_obj_get_height(screen) : 480;
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
