// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolhead_art.h"

#include "baked_art.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace helix::ui {

namespace {

constexpr int32_t kHeightPerScale = 8;
constexpr int32_t kPathTierHeight = 72;
constexpr uint32_t kNoFilament = 0xff000000u;
constexpr uint32_t kUnloadedTip = 0x3a3a3a;

unsigned g_builds = 0;

lv_draw_buf_t* build_image(const ToolheadArt& art, int32_t height, int32_t width, uint32_t color) {
    const std::string base = std::string("assets/images/toolheads/") + art.id + "/" +
                             (height <= kPathTierHeight ? "path" : "full");
    BakedPixels body = baked_art_decode(base + ".png", "ToolheadArt");
    const BakedPixels tint = baked_art_decode(base + "_tint.png", "ToolheadArt");
    const BakedPixels shade = baked_art_decode(base + "_shade.png", "ToolheadArt");
    if (body.w == 0 || tint.w == 0 || shade.w == 0)
        return nullptr;
    if (tint.w != body.w || shade.w != body.w || tint.h != body.h || shade.h != body.h) {
        spdlog::warn("[ToolheadArt] {} layers differ in size", art.id);
        return nullptr;
    }
    toolhead_art_compose(body.data.data(), tint.data.data(), shade.data.data(), body.w, body.h,
                         lv_color_hex(color == kNoFilament ? kUnloadedTip : color));
    lv_draw_buf_t* buf = baked_art_buffer(body, width, height);
    if (buf)
        ++g_builds;
    return buf;
}

inline void blend_over(uint8_t* dst, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    if (a == 0)
        return;
    const uint32_t da = dst[3];
    const uint32_t out_a = a * 255u + da * (255u - a);
    if (out_a == 0)
        return;
    const auto mix = [&](uint8_t d, uint8_t s) {
        return static_cast<uint8_t>((s * a * 255u + d * da * (255u - a)) / out_a);
    };
    dst[0] = mix(dst[0], b);
    dst[1] = mix(dst[1], g);
    dst[2] = mix(dst[2], r);
    dst[3] = static_cast<uint8_t>(out_a / 255u);
}

} // namespace

const ToolheadArt* toolhead_art_for(ToolheadStyle style) {
#if defined(HELIX_PLATFORM_ESP32)
    // The firmware image carries no toolhead PNGs.
    (void)style;
    return nullptr;
#else
    for (int i = 0; i < kToolheadArtCount; ++i) {
        if (kToolheadArt[i].style == style)
            return &kToolheadArt[i];
    }
    return nullptr;
#endif
}

ToolheadArtRect toolhead_art_rect(const ToolheadArt& art, int32_t cx, int32_t nozzle_y,
                                  int32_t scale) {
    const int32_t h = std::max<int32_t>(1, std::lround(scale * kHeightPerScale * art.height));
    const int32_t w = std::max<int32_t>(1, std::lround(h * art.aspect));
    return {cx - static_cast<int32_t>(std::lround(w * art.tip_x)), nozzle_y - h / 2, w, h};
}

ToolheadArtPoint toolhead_art_entry(const ToolheadArt& art, int32_t cx, int32_t nozzle_y,
                                    int32_t scale) {
    const ToolheadArtRect r = toolhead_art_rect(art, cx, nozzle_y, scale);
    return {r.x + static_cast<int32_t>(std::lround(r.w * art.entry_x)),
            r.y + static_cast<int32_t>(std::lround(r.h * art.entry_y))};
}

void toolhead_art_compose(uint8_t* body, const uint8_t* mask, const uint8_t* shade, int32_t w,
                          int32_t h, lv_color_t tint) {
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = body + i * 4;
        blend_over(p, tint.blue, tint.green, tint.red, mask[i * 4 + 3]);
        blend_over(p, 0, 0, 0, shade[i * 4 + 3]);
    }
}

bool draw_toolhead_art(lv_layer_t* layer, ToolheadStyle style, int32_t cx, int32_t nozzle_y,
                       std::optional<lv_color_t> filament, int32_t scale, lv_opa_t opa) {
    const ToolheadArt* art = toolhead_art_for(style);
    if (!art || scale <= 0)
        return false;
    const ToolheadArtRect r = toolhead_art_rect(*art, cx, nozzle_y, scale);
    const uint32_t color = filament ? lv_color_to_u32(*filament) & 0xffffffu : kNoFilament;

    const std::string key = std::string("toolhead/") + art->id + "/" + std::to_string(r.h) + "/" +
                            std::to_string(color);
    lv_draw_buf_t* buf = baked_art_cached(key, [&] { return build_image(*art, r.h, r.w, color); });
    if (!buf)
        return false;
    baked_art_draw(layer, buf, r.x, r.y, opa);
    return true;
}

unsigned toolhead_art_build_count() {
    return g_builds;
}

} // namespace helix::ui
