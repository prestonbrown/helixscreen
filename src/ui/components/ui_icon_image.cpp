// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_icon_image.h"

#include "ui_icon_codepoints.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <image/lv_image_decoder_private.h>
#include <memory>
#include <unordered_map>

namespace helix::ui::icon {

namespace {

struct Entry;

// One image icon at one MDI size. The lv_font_t is first so a font pointer is the glyph
// font's address; dsc points back at it as well.
struct GlyphFont {
    lv_font_t font{};
    Entry* entry = nullptr;
    const lv_font_t* base = nullptr;
    uint32_t side = 0;
    std::vector<uint8_t> mask; ///< side x side A8; empty while the image is unavailable
};

struct Entry {
    std::string fallback_text;
    bool available = false;
    std::vector<uint8_t> alpha;
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<std::unique_ptr<GlyphFont>> fonts;
};

// Never destroyed: a label deleted during process teardown may still name a glyph font.
std::unordered_map<std::string, std::unique_ptr<Entry>>& registry() {
    static auto* map = new std::unordered_map<std::string, std::unique_ptr<Entry>>();
    return *map;
}

bool glyph_dsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc, uint32_t, uint32_t) {
    const auto* g = static_cast<const GlyphFont*>(font->dsc);
    if (!g->entry->available || g->mask.empty())
        return false; // the base font draws the fallback's codepoint
    const auto side = static_cast<uint16_t>(g->side);
    dsc->adv_w = side;
    dsc->box_w = side;
    dsc->box_h = side;
    dsc->ofs_x = 0;
    // The box spans the whole line: its top at the line's top.
    dsc->ofs_y = static_cast<int16_t>(font->line_height - font->base_line - side);
    dsc->stride = side;
    dsc->format = LV_FONT_GLYPH_FORMAT_A8;
    dsc->is_placeholder = 0;
    dsc->gid.index = 0;
    return true;
}

const void* glyph_bitmap(lv_font_glyph_dsc_t* dsc, lv_draw_buf_t* draw_buf) {
    const auto* g = static_cast<const GlyphFont*>(dsc->resolved_font->dsc);
    if (g->mask.empty() || !draw_buf)
        return nullptr;
    const uint32_t stride = draw_buf->header.stride;
    auto* out = static_cast<uint8_t*>(draw_buf->data);
    for (uint32_t y = 0; y < g->side; ++y)
        std::copy_n(g->mask.data() + y * g->side, g->side, out + y * stride);
    return draw_buf;
}

const GlyphFont* as_glyph(const lv_font_t* font) {
    if (!font || font->get_glyph_dsc != &glyph_dsc)
        return nullptr;
    return static_cast<const GlyphFont*>(font->dsc);
}

void build_mask(GlyphFont& g) {
    const Entry& e = *g.entry;
    if (e.available)
        g.mask = scale_alpha_mask(e.alpha.data(), e.w, e.h, g.side);
    else
        g.mask.clear();
}

GlyphFont& glyph_font(Entry& e, const lv_font_t* base) {
    for (auto& g : e.fonts) {
        if (g->base == base)
            return *g;
    }
    auto g = std::make_unique<GlyphFont>();
    g->entry = &e;
    g->base = base;
    g->side = static_cast<uint32_t>(std::max<int32_t>(1, base->line_height));
    g->font.get_glyph_dsc = &glyph_dsc;
    g->font.get_glyph_bitmap = &glyph_bitmap;
    g->font.line_height = base->line_height;
    g->font.base_line = base->base_line;
    g->font.dsc = g.get();
    g->font.fallback = base;
    build_mask(*g);
    e.fonts.push_back(std::move(g));
    return *e.fonts.back();
}

void redraw_all() {
    lv_display_t* disp = lv_display_get_default();
    if (!disp)
        return;
    if (lv_obj_t* scr = lv_display_get_screen_active(disp))
        lv_obj_invalidate(scr);
    if (lv_obj_t* top = lv_display_get_layer_top(disp))
        lv_obj_invalidate(top);
}

ImageIconStatus decode_alpha(const std::string& path, Entry& e) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec)
        return ImageIconStatus::Missing;
    if (bytes > kMaxImageIconFileBytes)
        return ImageIconStatus::TooLarge;
    const std::string src = "A:" + path;
    lv_image_header_t header{};
    if (lv_image_decoder_get_info(src.c_str(), &header) != LV_RESULT_OK)
        return ImageIconStatus::Invalid;
    if (header.w == 0 || header.h == 0)
        return ImageIconStatus::Invalid;
    if (header.w > kMaxImageIconSide || header.h > kMaxImageIconSide)
        return ImageIconStatus::TooLarge;
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, src.c_str(), nullptr) != LV_RESULT_OK)
        return ImageIconStatus::Invalid;
    const lv_draw_buf_t* buf = dsc.decoded;
    const bool argb = buf && buf->header.cf == LV_COLOR_FORMAT_ARGB8888;
    if (argb) {
        e.w = buf->header.w;
        e.h = buf->header.h;
        e.alpha.resize(static_cast<size_t>(e.w) * e.h);
        for (uint32_t y = 0; y < e.h; ++y) {
            const uint8_t* row = buf->data + static_cast<size_t>(y) * buf->header.stride;
            for (uint32_t x = 0; x < e.w; ++x)
                e.alpha[static_cast<size_t>(y) * e.w + x] = row[x * 4 + 3];
        }
    }
    lv_image_decoder_close(&dsc);
    return argb ? ImageIconStatus::Ok : ImageIconStatus::Invalid;
}

} // namespace

std::vector<uint8_t> scale_alpha_mask(const uint8_t* alpha, uint32_t src_w, uint32_t src_h,
                                      uint32_t side) {
    std::vector<uint8_t> out(static_cast<size_t>(side) * side, 0);
    if (!alpha || src_w == 0 || src_h == 0 || side == 0)
        return out;
    const uint32_t longest = std::max(src_w, src_h);
    const uint32_t dst_w = std::max<uint32_t>(1, (src_w * side + longest / 2) / longest);
    const uint32_t dst_h = std::max<uint32_t>(1, (src_h * side + longest / 2) / longest);
    const uint32_t ox = (side - dst_w) / 2;
    const uint32_t oy = (side - dst_h) / 2;
    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy0 = y * src_h / dst_h;
        const uint32_t sy1 = std::max(sy0 + 1, (y + 1) * src_h / dst_h);
        for (uint32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx0 = x * src_w / dst_w;
            const uint32_t sx1 = std::max(sx0 + 1, (x + 1) * src_w / dst_w);
            uint32_t sum = 0;
            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                for (uint32_t sx = sx0; sx < sx1; ++sx)
                    sum += alpha[static_cast<size_t>(sy) * src_w + sx];
            }
            const uint32_t n = (sy1 - sy0) * (sx1 - sx0);
            out[static_cast<size_t>(oy + y) * side + ox + x] = static_cast<uint8_t>(sum / n);
        }
    }
    return out;
}

ImageIconStatus register_image_icon(const std::string& name, const std::string& path,
                                    const std::string& fallback) {
    auto& slot = registry()[name];
    if (!slot)
        slot = std::make_unique<Entry>();
    Entry& e = *slot;
    const char* cp = lookup_codepoint(fallback.c_str());
    e.fallback_text = cp ? cp : lookup_codepoint("image_broken_variant");
    e.alpha.clear();
    const ImageIconStatus status = decode_alpha(path, e);
    e.available = status == ImageIconStatus::Ok;
    if (!e.available)
        e.alpha.clear();
    for (auto& g : e.fonts)
        build_mask(*g);
    redraw_all();
    return status;
}

void unregister_image_icon(const std::string& name) {
    auto it = registry().find(name);
    if (it == registry().end())
        return;
    Entry& e = *it->second;
    e.available = false;
    e.alpha.clear();
    e.alpha.shrink_to_fit();
    for (auto& g : e.fonts) {
        g->mask.clear();
        g->mask.shrink_to_fit();
    }
    redraw_all();
}

bool image_icon_available(const std::string& name) {
    auto it = registry().find(name);
    return it != registry().end() && it->second->available;
}

ImageIconGlyph image_icon_glyph(const char* name, const lv_font_t* base) {
    if (!name || !base)
        return {};
    auto it = registry().find(name);
    if (it == registry().end())
        return {};
    if (const GlyphFont* g = as_glyph(base))
        base = g->base;
    Entry& e = *it->second;
    return {&glyph_font(e, base).font, e.fallback_text.c_str()};
}

const lv_font_t* image_icon_base_font(const lv_font_t* font) {
    const GlyphFont* g = as_glyph(font);
    return g ? g->base : nullptr;
}

const lv_font_t* image_icon_rebase(const lv_font_t* font, const lv_font_t* base) {
    const GlyphFont* g = as_glyph(font);
    if (!g || !base)
        return base;
    return &glyph_font(*g->entry, base).font;
}

} // namespace helix::ui::icon
