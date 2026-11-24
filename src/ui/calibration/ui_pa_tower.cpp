// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_pa_tower.h"

#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace helix::ui {

namespace {

using helix::pa::SpeedKind;
using helix::pa::SpotKind;
using helix::pa::TowerFace;

/// Below this height the speed strip and its words take room the tower needs.
constexpr int32_t kStripMinHeight = 120;
/// Below this width per face the strip keeps its bars and drops its words.
constexpr int32_t kStripWordsMinFaceWidth = 40;
/// Below this width the ruler is left out.
constexpr int32_t kRulerMinWidth = 140;

struct TowerState {
    PaTowerModel model;
    float tap_mm = 0.0f;
    int tap_face = 0;
    /// A press is down: the face it started on holds for the whole drag.
    bool dragging = false;
    /// The flourish's strength, 255 at its start, fading to 0.
    int32_t glow = 0;
};

/// A ruler height: whole millimeters without a decimal, others to one place.
std::string fmt_mm(float mm) {
    const float r = std::round(mm * 10.0f) / 10.0f;
    char buf[16];
    if (std::abs(r - std::round(r)) < 0.05f) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(r)));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(r));
    }
    return buf;
}

TowerState* state_of(lv_obj_t* obj) {
    return obj ? static_cast<TowerState*>(lv_obj_get_user_data(obj)) : nullptr;
}

/// Where the parts of the tower go inside the widget's content box.
struct Layout {
    lv_area_t box{};
    int32_t ruler_w = 0;
    int32_t labels_w = 0;
    int32_t body_x1 = 0;
    int32_t body_x2 = 0;
    int32_t top = 0;
    int32_t bottom = 0; ///< the plate
    int32_t strip_h = 0;
    bool strip_words = false;
    const lv_font_t* font = nullptr;
    int32_t font_h = 0;

    int32_t y_at(float z, float height) const {
        return bottom - static_cast<int32_t>(std::lround((bottom - top) * z / height));
    }
};

Layout measure(lv_obj_t* obj, const PaTowerModel& m) {
    Layout l;
    lv_obj_get_content_coords(obj, &l.box);
    const int32_t w = lv_area_get_width(&l.box);
    const int32_t h = lv_area_get_height(&l.box);
    l.font = theme_manager_get_font("font_xs");
    l.font_h = theme_manager_get_font_height(l.font);
    const int32_t gap = theme_manager_get_spacing("space_sm");
    l.ruler_w = m.scale && w >= kRulerMinWidth ? l.font_h * 2 : 0;
    const bool labels = m.scale && (m.banded || (!m.tick_labels.empty() && w >= kRulerMinWidth));
    l.labels_w = labels ? std::max<int32_t>(l.font_h * 3, w / 3) : 0;
    l.body_x1 = l.box.x1 + l.ruler_w + (l.ruler_w ? gap : 0);
    l.body_x2 = l.box.x2 - l.labels_w - (l.labels_w ? gap : 0);
    l.top = l.box.y1 + l.font_h / 2;
    l.bottom = l.box.y2 - l.font_h / 2;
    if (!m.banded && m.speed_strip && h >= kStripMinHeight) {
        l.strip_h = std::max<int32_t>(4, theme_manager_get_spacing("space_xs") + 3);
        const size_t faces = std::max<size_t>(1, m.faces.size());
        const int32_t face_w = (l.body_x2 - l.body_x1) / static_cast<int32_t>(faces);
        l.strip_words = face_w >= kStripWordsMinFaceWidth && faces == 1;
        l.bottom -= l.strip_h + gap + (l.strip_words ? l.font_h : 0);
    }
    return l;
}

lv_color_t band_color(float frac) {
    const auto a = theme_manager_get_color("secondary");
    const auto b = theme_manager_get_color("tertiary");
    return lv_color_mix(b, a, static_cast<uint8_t>(std::clamp(frac, 0.0f, 1.0f) * 255.0f));
}

void fill_rect(lv_layer_t* layer, lv_area_t a, lv_color_t c, lv_opa_t opa = LV_OPA_COVER,
               int32_t radius = 0) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, &a);
}

void grad_rect(lv_layer_t* layer, lv_area_t a, lv_color_t top, lv_color_t bottom, lv_opa_t opa) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = opa;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0] = {top, LV_OPA_COVER, 0};
    d.bg_grad.stops[1] = {bottom, LV_OPA_COVER, 255};
    lv_draw_rect(layer, &d, &a);
}

void outline_rect(lv_layer_t* layer, lv_area_t a, lv_color_t c, int32_t width = 1) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = c;
    d.border_width = width;
    d.border_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &d, &a);
}

void line(lv_layer_t* layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c,
          int32_t width, int32_t dash = 0, int32_t gap = 0, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = width;
    d.opa = opa;
    d.dash_width = dash;
    d.dash_gap = gap;
    d.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    d.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    lv_draw_line(layer, &d);
}

void draw_text(lv_obj_t* obj, lv_layer_t* layer, const std::string& text, lv_area_t area,
               lv_color_t c, const lv_font_t* font, lv_text_align_t align = LV_TEXT_ALIGN_LEFT,
               bool chip = false) {
    if (text.empty()) {
        return;
    }
    if (chip) {
        lv_point_t size;
        lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int32_t pad = 2;
        int32_t x1 = area.x1;
        if (align == LV_TEXT_ALIGN_CENTER) {
            x1 = (area.x1 + area.x2) / 2 - size.x / 2;
        } else if (align == LV_TEXT_ALIGN_RIGHT) {
            x1 = area.x2 - size.x;
        }
        fill_rect(layer, {x1 - pad, area.y1, x1 + size.x + pad, area.y1 + size.y},
                  theme_manager_get_color("screen_bg"), LV_OPA_80, 3);
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.base.obj = obj;
    d.text = text.c_str();
    d.text_local = 1;
    d.color = c;
    d.font = font;
    d.align = align;
    lv_draw_label(layer, &d, &area);
}

// ---------------------------------------------------------------- banded

void draw_banded(lv_obj_t* obj, lv_layer_t* layer, const PaTowerModel& m, const Layout& l) {
    const lv_color_t border = theme_manager_get_color("border");
    const lv_color_t text = theme_manager_get_color("text");
    const lv_color_t subtle = theme_manager_get_color("text_subtle");
    const lv_color_t muted = theme_manager_get_color("text_muted");
    const lv_color_t secondary = theme_manager_get_color("secondary");
    const lv_color_t warning = theme_manager_get_color("warning");
    const lv_color_t primary = theme_manager_get_color("primary");
    const lv_color_t base = theme_manager_get_color("elevated_bg");
    const float H = std::max(1.0f, m.height_mm);
    const int32_t lx1 = l.body_x2 + theme_manager_get_spacing("space_sm");
    const bool held = m.fill == PaTowerFill::Live && m.held_from_mm >= 0.0f;
    const int n = static_cast<int>(m.bands.size());

    // The plate and the base under the first band.
    line(layer, l.body_x1 - 4, l.bottom, l.body_x2 + 4, l.bottom, base, 2);
    if (m.base_mm > 0.0f) {
        const lv_area_t a{l.body_x1, l.y_at(m.base_mm, H), l.body_x2, l.bottom};
        if (m.fill == PaTowerFill::Live || m.fill == PaTowerFill::Full) {
            fill_rect(layer, a, base);
        } else {
            outline_rect(layer, a, border);
        }
    }

    for (int i = 0; i < n; ++i) {
        const PaTowerBand& b = m.bands[i];
        const float frac = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        const lv_area_t a{l.body_x1, l.y_at(b.z_high, H) + 1, l.body_x2, l.y_at(b.z_low, H)};
        const int32_t ly = (a.y1 + a.y2) / 2 - l.font_h / 2;
        const lv_area_t lab{lx1, ly, l.box.x2, ly + l.font_h};
        const lv_color_t col = band_color(frac);
        switch (m.fill) {
        case PaTowerFill::Preview:
            fill_rect(layer, a, col, LV_OPA_50);
            line(layer, a.x1, a.y1, a.x2, a.y1, theme_manager_get_color("card_bg"), 1);
            draw_text(obj, layer, b.label, lab, muted, l.font);
            break;
        case PaTowerFill::Outline:
            line(layer, a.x1, a.y1, a.x2, a.y1, border, 1, 3, 3);
            if (i == 0 || i == n - 1) {
                draw_text(obj, layer, b.label, lab, subtle, l.font);
            }
            break;
        case PaTowerFill::Live:
            if (b.state == PaBandState::Printed) {
                fill_rect(layer, a, col);
                draw_text(obj, layer, b.label, lab, text, l.font);
            } else if (b.state == PaBandState::Printing) {
                const int32_t fy = std::clamp(l.y_at(m.printed_mm, H), a.y1, a.y2);
                outline_rect(layer, {a.x1, a.y1, a.x2, fy}, held ? warning : secondary);
                fill_rect(layer, {a.x1, fy, a.x2, a.y2}, col);
                draw_text(obj, layer, b.label, lab, secondary, l.font);
            } else if (!held) {
                outline_rect(layer, a, border);
                draw_text(obj, layer, b.label, lab, subtle, l.font);
            }
            break;
        case PaTowerFill::Full:
        case PaTowerFill::Wall:
            fill_rect(layer, a, col, i == m.selected || m.selected < 0 ? LV_OPA_COVER : LV_OPA_80);
            if (i == m.selected) {
                outline_rect(layer, {l.box.x1, a.y1 - 1, l.box.x2, a.y2 + 1}, primary, 2);
                draw_text(obj, layer, b.label, lab, text, l.font);
            } else {
                draw_text(obj, layer, b.label, lab, text, l.font);
            }
            break;
        }
    }

    if (held) {
        // Everything above the tap holds one value: one dashed region, not the planned bands.
        const int32_t y_top = l.y_at(H, H);
        const int32_t y_from = l.y_at(m.held_from_mm, H);
        outline_rect(layer, {l.body_x1, y_top, l.body_x2, y_from}, warning);
        fill_rect(layer, {l.body_x1, y_top, l.body_x2, y_from}, warning, LV_OPA_10);
        const int32_t my = (y_top + y_from) / 2;
        draw_text(obj, layer, m.held_label, {lx1, my - l.font_h, l.box.x2, my}, warning, l.font);
        draw_text(obj, layer, lv_tr("held to the top"), {lx1, my, l.box.x2, my + l.font_h}, warning,
                  l.font);
    }

    // No bands yet (a pick before anything was reported): no heights to mark.
    if (l.ruler_w && n > 0) {
        for (int i = 0; i <= n; ++i) {
            const float z = i < n ? m.bands[i].z_low : m.bands[n - 1].z_high;
            if (m.fill == PaTowerFill::Live && std::abs(z - m.printed_mm) < H * 0.04f) {
                continue;
            }
            const int32_t y = l.y_at(z, H);
            draw_text(obj, layer, fmt_mm(z),
                      {l.box.x1, y - l.font_h / 2, l.box.x1 + l.ruler_w, y + l.font_h / 2}, subtle,
                      l.font, LV_TEXT_ALIGN_RIGHT);
        }
    }

    if (m.fill == PaTowerFill::Live) {
        const int32_t y = l.y_at(std::clamp(m.printed_mm, 0.0f, H), H);
        line(layer, l.body_x1 - 3, y, l.body_x2 + 3, y, text, 3);
        if (l.ruler_w) {
            draw_text(obj, layer, fmt_mm(m.printed_mm),
                      {l.box.x1, y - l.font_h / 2, l.box.x1 + l.ruler_w, y + l.font_h / 2}, text,
                      l.font, LV_TEXT_ALIGN_RIGHT);
        }
    }
}

// ---------------------------------------------------------------- continuous

const char* speed_word(SpeedKind s) {
    switch (s) {
    case SpeedKind::Fast:
        return lv_tr("fast");
    case SpeedKind::Medium:
        return lv_tr("medium");
    case SpeedKind::Slow:
    case SpeedKind::VerySlow:
        break;
    }
    return lv_tr("slow");
}

/// Bar height and shade for a speed: taller and lighter is faster.
std::pair<float, lv_opa_t> speed_bar(SpeedKind s) {
    switch (s) {
    case SpeedKind::Fast:
        return {1.0f, LV_OPA_COVER};
    case SpeedKind::Medium:
        return {0.66f, LV_OPA_80};
    case SpeedKind::Slow:
        return {0.33f, LV_OPA_60};
    case SpeedKind::VerySlow:
        break;
    }
    return {0.15f, LV_OPA_60};
}

void draw_face(lv_obj_t* obj, lv_layer_t* layer, const PaTowerModel& m, const Layout& l, int index,
               lv_area_t face) {
    const TowerFace which = m.faces[index];
    const helix::pa::FaceLayout fl = helix::pa::tower_face_layout(which, m.size_x_mm, m.size_y_mm);
    const lv_color_t amber = theme_manager_get_color("warning");
    const lv_color_t border = theme_manager_get_color("border");
    const lv_color_t text = theme_manager_get_color("text");
    const lv_color_t top_c = theme_manager_get_color("tertiary");
    const lv_color_t bot_c = theme_manager_get_color("secondary");
    const float H = std::max(1.0f, m.height_mm);
    const int32_t fw = lv_area_get_width(&face);
    const auto x_at = [&](float f) {
        return face.x1 + static_cast<int32_t>(std::lround(f * (fw - 1)));
    };

    switch (m.fill) {
    case PaTowerFill::Preview:
        grad_rect(layer, face, top_c, bot_c, LV_OPA_40);
        break;
    case PaTowerFill::Outline:
        outline_rect(layer, face, border);
        break;
    case PaTowerFill::Live: {
        const int32_t y = std::clamp(l.y_at(m.printed_mm, H), face.y1, face.y2);
        outline_rect(layer, {face.x1, face.y1, face.x2, y}, border);
        grad_rect(layer, {face.x1, y, face.x2, face.y2}, band_color(m.printed_mm / H), bot_c,
                  LV_OPA_COVER);
        break;
    }
    case PaTowerFill::Full:
        grad_rect(layer, face, top_c, bot_c, LV_OPA_COVER);
        break;
    case PaTowerFill::Wall: {
        const lv_color_t wall = theme_manager_get_color("elevated_bg");
        if (m.light_right) {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.bg_opa = LV_OPA_COVER;
            d.bg_grad.dir = LV_GRAD_DIR_HOR;
            d.bg_grad.stops_count = 2;
            d.bg_grad.stops[0] = {wall, LV_OPA_COVER, 0};
            d.bg_grad.stops[1] = {lv_color_mix(amber, text, LV_OPA_30), LV_OPA_COVER, 255};
            lv_draw_rect(layer, &d, &face);
        } else {
            fill_rect(layer, face, wall);
        }
        const int32_t step = std::max<int32_t>(3, l.font_h / 4);
        for (int32_t y = face.y1 + 2; y < face.y2; y += step) {
            line(layer, face.x1, y, face.x2, y, theme_manager_get_color("screen_bg"), 1, 0, 0,
                 m.light_right ? LV_OPA_40 : LV_OPA_20);
        }
        break;
    }
    }

    // The slow middle, shaded, and the spots where the nozzle changes speed, up the full height.
    int32_t slow_x1 = 0;
    int32_t slow_x2 = 0;
    for (const auto& s : fl.segments) {
        if (s.speed == SpeedKind::Slow && slow_x1 == 0) {
            slow_x1 = x_at(s.from);
        }
        if (s.speed == SpeedKind::Slow) {
            slow_x2 = x_at(s.to);
        }
    }
    fill_rect(layer, {slow_x1, face.y1, slow_x2, face.y2}, text, LV_OPA_10);
    for (const auto& spot : fl.spots) {
        const int32_t x = x_at(spot.at);
        switch (spot.kind) {
        case SpotKind::SlowDown:
        case SpotKind::SpeedUp:
            line(layer, x, face.y1, x, face.y2, amber, 2);
            break;
        case SpotKind::Pause:
            line(layer, x, face.y1, x, face.y2, amber, 1, 4, 3);
            break;
        case SpotKind::VerySlowSpot:
            line(layer, x, face.y1, x, face.y2, amber, 1, 1, 3, LV_OPA_80);
            break;
        }
    }

    // Words over the spots, on dark chips so they read over any fill.
    const int32_t lh = l.font_h;
    draw_text(obj, layer, lv_tr("slow"),
              {slow_x1 - lh * 2, face.y1 + 2, slow_x2 + lh * 2, face.y1 + 2 + lh}, amber, l.font,
              LV_TEXT_ALIGN_CENTER, true);
    for (const auto& spot : fl.spots) {
        if (spot.kind == SpotKind::Pause) {
            const int32_t x = x_at(spot.at);
            draw_text(obj, layer, lv_tr("pause"),
                      {x - lh * 2, face.y1 + lh + 4, x + lh * 2, face.y1 + 2 * lh + 4}, amber,
                      l.font, LV_TEXT_ALIGN_CENTER, true);
        }
    }
    const char* name = which == TowerFace::Left ? (fw > lh * 5 ? lv_tr("Left side") : lv_tr("Left"))
                                                : lv_tr("Front");
    draw_text(obj, layer, name, {face.x1 + 2, face.y2 - lh - 2, face.x2, face.y2 - 2}, text, l.font,
              LV_TEXT_ALIGN_LEFT, true);

    for (const auto& mk : m.markers) {
        if (mk.face != index) {
            continue;
        }
        const lv_color_t c = theme_manager_get_color(mk.accent ? "warning" : "primary");
        const int32_t y = l.y_at(std::clamp(mk.z_mm, 0.0f, H), H);
        line(layer, face.x1, y, face.x2, y, c, 3);
        const int32_t r = std::max<int32_t>(6, lh * 2 / 3);
        // The disc stays whole inside the face at 0 and at the top.
        const int32_t cy = std::clamp(y, face.y1 + r, face.y2 - r);
        const int32_t cx =
            which == TowerFace::Front && m.faces.size() > 1 ? face.x2 - r - 1 : face.x1 + r + 1;
        fill_rect(layer, {cx - r, cy - r, cx + r, cy + r}, c, LV_OPA_COVER, LV_RADIUS_CIRCLE);
        draw_text(obj, layer, mk.tag, {cx - r, cy - lh / 2, cx + r, cy + lh / 2}, text, l.font,
                  LV_TEXT_ALIGN_CENTER);
    }

    if (l.strip_h) {
        // The speeds along the face, then where the flaw shows under each change.
        const int32_t sy1 = l.bottom + theme_manager_get_spacing("space_sm");
        const int32_t sy2 = sy1 + l.strip_h;
        fill_rect(layer, {face.x1, sy1, face.x2, sy2}, theme_manager_get_color("overlay_bg"));
        for (const auto& s : fl.segments) {
            const auto [hf, opa] = speed_bar(s.speed);
            const int32_t bh =
                std::max<int32_t>(1, static_cast<int32_t>(std::lround(l.strip_h * hf)));
            int32_t x1 = x_at(s.from);
            int32_t x2 = std::max(x_at(s.to), x1 + 1);
            fill_rect(layer, {x1, sy2 - bh, x2, sy2}, bot_c, opa);
        }
        for (const auto& spot : fl.spots) {
            const int32_t x = x_at(spot.at);
            if (spot.kind == SpotKind::SlowDown || spot.kind == SpotKind::SpeedUp) {
                line(layer, x, sy1 - 2, x, sy2 + 2, amber, 2);
                if (l.strip_words) {
                    const bool down = spot.kind == SpotKind::SlowDown;
                    // Each word runs away from its tick, out to the widget's edge if it needs to.
                    const lv_area_t a = down ? lv_area_t{l.box.x1, sy2 + 2, x - 2, sy2 + 2 + lh}
                                             : lv_area_t{x + 2, sy2 + 2, l.box.x2, sy2 + 2 + lh};
                    draw_text(obj, layer, down ? lv_tr("bulge") : lv_tr("thin"), a, amber, l.font,
                              down ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
                }
            } else {
                line(layer, x, sy1 - 2, x, sy2 + 2, amber, 1, 1, 1);
            }
        }
    }
}

void draw_continuous(lv_obj_t* obj, lv_layer_t* layer, const PaTowerModel& m, const Layout& l) {
    if (m.faces.empty()) {
        return;
    }
    const float H = std::max(1.0f, m.height_mm);
    const lv_color_t subtle = theme_manager_get_color("text_subtle");
    const lv_color_t muted = theme_manager_get_color("text_muted");
    const lv_color_t text = theme_manager_get_color("text");
    const int32_t gap = theme_manager_get_spacing("space_xs");
    float total_mm = 0.0f;
    for (auto f : m.faces) {
        total_mm += helix::pa::tower_face_layout(f, m.size_x_mm, m.size_y_mm).width_mm;
    }
    const int32_t avail = l.body_x2 - l.body_x1 - gap * static_cast<int32_t>(m.faces.size() - 1);
    int32_t x = l.body_x1;
    for (size_t i = 0; i < m.faces.size(); ++i) {
        const float mm =
            helix::pa::tower_face_layout(m.faces[i], m.size_x_mm, m.size_y_mm).width_mm;
        const int32_t fw = i + 1 == m.faces.size()
                               ? l.body_x2 - x
                               : static_cast<int32_t>(std::lround(avail * mm / total_mm));
        draw_face(obj, layer, m, l, static_cast<int>(i), {x, l.y_at(H, H), x + fw, l.bottom});
        x += fw + gap;
    }

    for (int z = 0; z <= static_cast<int>(H); z += 10) {
        const int32_t y = l.y_at(static_cast<float>(z), H);
        if (l.ruler_w) {
            draw_text(obj, layer, std::to_string(z),
                      {l.box.x1, y - l.font_h / 2, l.box.x1 + l.ruler_w, y + l.font_h / 2}, subtle,
                      l.font, LV_TEXT_ALIGN_RIGHT);
        }
        const size_t ti = static_cast<size_t>(z / 10);
        if (l.labels_w && z > 0 && ti < m.tick_labels.size()) {
            const bool printed =
                m.fill != PaTowerFill::Live || static_cast<float>(z) <= m.printed_mm;
            draw_text(obj, layer, m.tick_labels[ti],
                      {l.body_x2 + gap * 2, y - l.font_h / 2, l.box.x2, y + l.font_h / 2},
                      printed ? muted : subtle, l.font);
        }
    }

    if (m.fill == PaTowerFill::Live) {
        // Clamped like the fill: a lift past the top or a Z below the plate stays on the tower.
        const int32_t y = l.y_at(std::clamp(m.printed_mm, 0.0f, H), H);
        line(layer, l.body_x1 - 3, y, l.body_x2 + 3, y, text, 3);
        if (!m.layer_tag.empty()) {
            draw_text(obj, layer, m.layer_tag, {l.body_x1, y - l.font_h - 4, l.body_x2 - 2, y - 4},
                      text, l.font, LV_TEXT_ALIGN_RIGHT, true);
        }
    }
}

void on_draw(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    TowerState* st = state_of(obj);
    if (!layer || !st) {
        return;
    }
    const Layout l = measure(obj, st->model);
    if (l.body_x2 - l.body_x1 < 8 || l.bottom - l.top < 20) {
        return;
    }
    if (st->model.banded) {
        draw_banded(obj, layer, st->model, l);
    } else {
        draw_continuous(obj, layer, st->model, l);
    }
    if (st->glow > 0) {
        // The flourish: a light wash over the tower that fades, and a bright line across its top.
        const lv_color_t c = theme_manager_get_color("text");
        const int32_t top = l.y_at(st->model.height_mm, std::max(1.0f, st->model.height_mm));
        fill_rect(layer, {l.body_x1, top, l.body_x2, l.bottom}, c,
                  static_cast<lv_opa_t>(st->glow / 4));
        line(layer, l.body_x1 - 3, top, l.body_x2 + 3, top, c, 3, 0, 0,
             static_cast<lv_opa_t>(st->glow));
    }
}

void glow_exec(void* var, int32_t v) {
    auto* obj = static_cast<lv_obj_t*>(var);
    if (TowerState* st = state_of(obj)) {
        st->glow = v;
        lv_obj_invalidate(obj);
    }
}

/// Which face screen @p x falls on: the faces split the body by their widths.
int face_at(lv_obj_t* obj, const TowerState& st, int32_t x) {
    const auto& m = st.model;
    if (m.banded || m.faces.size() < 2) {
        return 0;
    }
    const Layout l = measure(obj, m);
    float total_mm = 0.0f;
    for (auto f : m.faces) {
        total_mm += helix::pa::tower_face_layout(f, m.size_x_mm, m.size_y_mm).width_mm;
    }
    float edge = static_cast<float>(l.body_x1);
    const float span = static_cast<float>(l.body_x2 - l.body_x1);
    for (size_t i = 0; i + 1 < m.faces.size(); ++i) {
        edge += span * helix::pa::tower_face_layout(m.faces[i], m.size_x_mm, m.size_y_mm).width_mm /
                total_mm;
        if (static_cast<float>(x) < edge) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(m.faces.size() - 1);
}

/// A press picks the height under the finger, and a drag keeps picking as it
/// moves, on the face the press started on. A click with no press before it
/// (remote control) picks once.
void on_press(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    lv_indev_t* indev = lv_indev_active();
    TowerState* st = state_of(obj);
    if (!indev || !st) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESS_LOST) {
        st->dragging = false;
        return;
    }
    // The click that ends a press lands where the finger lifted, maybe over
    // the other face: the press already picked, so it only ends the drag.
    if (code == LV_EVENT_CLICKED && st->dragging) {
        st->dragging = false;
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED) {
        st->tap_face = face_at(obj, *st, p.x);
        st->dragging = code == LV_EVENT_PRESSED;
    }
    const float mm = pa_tower_height_at(obj, p.y);
    if (code == LV_EVENT_PRESSING && std::abs(mm - st->tap_mm) < 0.05f) {
        return;
    }
    st->tap_mm = mm;
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, nullptr);
}

void on_delete(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    lv_anim_delete(obj, glow_exec);
    delete state_of(obj);
    lv_obj_set_user_data(obj, nullptr);
}

void* tower_xml_create(lv_xml_parser_state_t* state, const char** /*attrs*/) {
    auto* parent = static_cast<lv_obj_t*>(lv_xml_state_get_parent(state));
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_user_data(obj, new TowerState{});
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
    // A drag on the tower picks a height; it never scrolls or swipes the page.
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (lv_event_code_t code :
         {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_PRESS_LOST, LV_EVENT_CLICKED}) {
        lv_obj_add_event_cb(obj, on_press, code, nullptr);
    }
    lv_obj_add_event_cb(obj, on_delete, LV_EVENT_DELETE, nullptr);
    return obj;
}

} // namespace

void register_pa_tower_widget() {
    lv_xml_register_widget("pa_tower", tower_xml_create, lv_xml_obj_apply);
    spdlog::trace("[PaTower] Widget registered with XML system");
}

void pa_tower_set_model(lv_obj_t* tower, PaTowerModel model) {
    TowerState* st = state_of(tower);
    if (!st) {
        return;
    }
    st->model = std::move(model);
    lv_obj_invalidate(tower);
}

void pa_tower_flourish(lv_obj_t* tower) {
    if (!state_of(tower) || !DisplaySettingsManager::instance().get_animations_enabled()) {
        return;
    }
    lv_anim_delete(tower, glow_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tower);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, glow_exec);
    lv_anim_start(&a);
}

float pa_tower_tap_height(lv_obj_t* tower) {
    const TowerState* st = state_of(tower);
    return st ? st->tap_mm : 0.0f;
}

int pa_tower_tap_face(lv_obj_t* tower) {
    const TowerState* st = state_of(tower);
    return st ? st->tap_face : 0;
}

float pa_tower_height_at(lv_obj_t* tower, int32_t y) {
    const TowerState* st = state_of(tower);
    if (!st) {
        return 0.0f;
    }
    const Layout l = measure(tower, st->model);
    const float span = static_cast<float>(std::max<int32_t>(1, l.bottom - l.top));
    const float z = static_cast<float>(l.bottom - y) / span * st->model.height_mm;
    return std::clamp(z, 0.0f, st->model.height_mm);
}

} // namespace helix::ui
