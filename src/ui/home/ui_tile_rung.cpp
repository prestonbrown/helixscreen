// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_tile_rung.h"

#include "ui_text.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix/ui/shared_font_style.h"
#include "helix/ui/text_metrics.h"
#include "src/ui/panel_widgets/tile_layout.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace helix::ui {

namespace {

/// One row per TileLadder, one column per rung. The rows move together: one
/// rung index selects the glyph, the value beside it and the label under it,
/// so a tile never draws a 64px glyph next to 12px digits.
///
/// The label row stays at font_xs through lg, the rung every medium-and-up tier
/// draws a one-cell tile at, so a tile at its natural size keeps its authored
/// caption and only a tile given more room than that grows it. font_small is
/// the larger step because it is the same light weight as font_xs.
///
/// At xxl, where the glyph doubles, the value and label take one more step
/// so the text is not left undersized beside it.
constexpr const char* kLadders[3][kTileRungs] = {
    {"icon_font_xs", "icon_font_sm", "icon_font_md", "icon_font_lg", "icon_font_xl",
     "icon_font_xl"},
    {"font_xs", "font_xs", "font_small", "font_body", "font_heading", "font_xl"},
    {"font_xs", "font_xs", "font_xs", "font_xs", "font_small", "font_body"},
};
constexpr int kXxl = kTileRungs - 1;

/// The icon's xxl rung: the largest linked MDI face at or below the tier's
/// #tile_icon_xxl_size, scaled to reach it.
TileFace xxl_icon_face(int32_t max_scale) {
    const TileFace xl{theme_manager_get_font("icon_font_xl"), LV_SCALE_NONE};
    const int target = theme_manager_get_spacing("tile_icon_xxl_size");
    static constexpr int kSizes[] = {128, 96, 80, 64, 48, 32};
    for (int size : kSizes) {
        if (size > target) {
            continue;
        }
        char name[16];
        std::snprintf(name, sizeof(name), "mdi_icons_%d", size);
        const lv_font_t* font = lv_xml_get_font_silent(nullptr, name);
        // A stand-in registered under an MDI name but drawn smaller (the ESP32
        // image aliases the faces it does not carry) is not that face.
        if (!font || static_cast<int>(lv_font_get_line_height(font)) < size) {
            continue;
        }
        return TileFace{font, tile_xxl_scale(target, size, max_scale)};
    }
    return xl;
}

/// Draw @p obj's glyph at @p face's scale. Padding grows its layout box by the
/// difference, so the flex parent spaces the glyph at the size it is drawn, and
/// the transform scales the whole box about its centre to fill it. A transform
/// renders the object through a layer; at 1x there is none.
/// DECLARATIVE_OK: the scale is computed from which faces this build links.
void apply_face_scale(lv_obj_t* obj, const TileFace& face) {
    const TileGlyphBox unscaled = tile_glyph_box(TileFace{face.font, LV_SCALE_NONE});
    const TileGlyphBox scaled = tile_glyph_box(face);
    lv_obj_set_style_pad_hor(obj, (scaled.w - unscaled.w) / 2, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(obj, (scaled.h - unscaled.h) / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(obj, face.scale, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(obj, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(obj, lv_pct(50), LV_PART_MAIN);
}

/// The ladder, offset and flags ride in the observer's user data, so a binding
/// needs no allocation and nothing to free when its object goes away.
constexpr int kOneLineBit = 1 << 12;
constexpr int kAnimatedBit = 1 << 13;

void* pack(TileLadder ladder, int offset, bool one_line, bool animated) {
    return reinterpret_cast<void*>(
        static_cast<intptr_t>(static_cast<int>(ladder) * 256 + (offset + 128) +
                              (one_line ? kOneLineBit : 0) + (animated ? kAnimatedBit : 0)));
}

void rung_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    const auto packed =
        static_cast<int>(reinterpret_cast<intptr_t>(lv_observer_get_user_data(observer)));
    const auto ladder = static_cast<TileLadder>((packed & ~(kOneLineBit | kAnimatedBit)) / 256);
    const int offset = packed % 256 - 128;
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    const int32_t max_scale = (packed & kAnimatedBit) ? kTileAnimatedMaxScale : kTileMaxScale;
    const TileFace face = tile_rung_face(ladder, lv_subject_get_int(subject) + offset, max_scale);
    const lv_font_t* font = face.font;
    if (!font) {
        return;
    }
    if (ladder == TileLadder::Disc) {
        // DECLARATIVE_OK: measured from the computed face, which XML cannot name.
        const int edge = tile_disc_edge(face);
        lv_obj_set_size(obj, edge, edge);
        return;
    }
    apply_font_style(obj, font);
    if (ladder == TileLadder::Icon) {
        apply_face_scale(obj, face);
    }
    // At content height a dotted label wraps instead of ellipsizing; held to one
    // line, its height follows the face it now draws in.
    if (packed & kOneLineBit) {
        helix::ui::label_hold_one_line(obj);
    }
}

void* bind_tile_rung_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs;
    return lv_xml_state_get_parent(state);
}

void bind_tile_rung_apply(lv_xml_parser_state_t* state, const char** attrs) {
    const char* subject_name = lv_xml_get_value_of(attrs, "subject");
    if (!subject_name || subject_name[0] == '\0') {
        return; // an optional subject left at its empty default: no binding
    }
    lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
    if (!subject) {
        spdlog::warn("[TileRung] subject '{}' does not exist; the face stays as authored",
                     subject_name);
        return;
    }

    const char* ladder_name = lv_xml_get_value_of(attrs, "ladder");
    TileLadder ladder = TileLadder::Icon;
    if (ladder_name && std::strcmp(ladder_name, "value") == 0) {
        ladder = TileLadder::Value;
    } else if (ladder_name && std::strcmp(ladder_name, "label") == 0) {
        ladder = TileLadder::Label;
    } else if (ladder_name && std::strcmp(ladder_name, "disc") == 0) {
        ladder = TileLadder::Disc;
    } else if (ladder_name && std::strcmp(ladder_name, "icon") != 0) {
        spdlog::warn("[TileRung] unknown ladder '{}'; binding the icon ladder", ladder_name);
    }

    const char* offset_str = lv_xml_get_value_of(attrs, "offset");
    const int offset = offset_str ? lv_xml_atoi(offset_str) : 0;
    const char* one_line_str = lv_xml_get_value_of(attrs, "one_line");
    const bool one_line = one_line_str && std::strcmp(one_line_str, "true") == 0;
    const char* animated_str = lv_xml_get_value_of(attrs, "animated");
    const bool animated = animated_str && std::strcmp(animated_str, "true") == 0;

    bind_tile_rung(static_cast<lv_obj_t*>(lv_xml_state_get_parent(state)), subject, ladder, offset,
                   one_line, animated);
}

} // namespace

const char* tile_rung_font_token(TileLadder ladder, int rung) {
    int row = static_cast<int>(ladder);
    if (ladder == TileLadder::Disc) {
        row = static_cast<int>(TileLadder::Icon);
    }
    return kLadders[row][std::clamp(rung, 0, kTileRungs - 1)];
}

TileFace tile_rung_face(TileLadder ladder, int rung, int32_t max_scale) {
    rung = std::clamp(rung, 0, kTileRungs - 1);
    const bool icon = ladder == TileLadder::Icon || ladder == TileLadder::Disc;
    if (icon && rung == kXxl) {
        return xxl_icon_face(max_scale);
    }
    return TileFace{theme_manager_get_font(tile_rung_font_token(ladder, rung)), LV_SCALE_NONE};
}

int tile_drawn_rung(int rung, int32_t max_scale) {
    const TileFace xl = tile_rung_face(TileLadder::Icon, kXxl - 1);
    const TileFace xxl = tile_rung_face(TileLadder::Icon, kXxl, max_scale);
    if (!xl.font || !xxl.font) {
        return rung;
    }
    return tile_drawn_rung(rung, static_cast<int>(lv_font_get_line_height(xl.font)),
                           xxl.px(static_cast<int>(lv_font_get_line_height(xxl.font))));
}

TileGlyphBox tile_glyph_box(const TileFace& face) {
    if (!face.font) {
        return {};
    }
    // Every MDI face is monospaced across the icon block, so any codepoint
    // measures the box all of them draw in. Each axis grows by an even number
    // of pixels, the padding split evenly either side of the face's box.
    const int w = ui::text_width("\xF3\xB0\x90\xA5", face.font);
    const int h = static_cast<int>(lv_font_get_line_height(face.font));
    return {w + (face.px(w) - w) / 2 * 2, h + (face.px(h) - h) / 2 * 2};
}

int tile_disc_edge(const TileFace& icon_face) {
    // Half again the glyph's line height: the proportion #icon_badge_size
    // holds to the md glyph on the tiers that author it.
    return icon_face.font
               ? icon_face.px(static_cast<int>(lv_font_get_line_height(icon_face.font))) * 3 / 2
               : 0;
}

void bind_tile_rung(lv_obj_t* obj, lv_subject_t* subject, TileLadder ladder, int offset,
                    bool one_line, bool animated) {
    if (!obj || !subject) {
        return;
    }
    lv_subject_add_observer_obj(subject, rung_observer_cb, obj,
                                pack(ladder, offset, one_line, animated));
}

void register_tile_rung_binding() {
    lv_xml_register_widget("lv_obj-bind_tile_rung", bind_tile_rung_create, bind_tile_rung_apply);
}

} // namespace helix::ui
