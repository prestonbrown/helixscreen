// SPDX-License-Identifier: GPL-3.0-or-later

#include "toolhead_art.h"

#include "core/data_root_resolver.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <image/lv_image_decoder_private.h>
#include <string>
#include <vector>

namespace helix::ui {

namespace {

constexpr int32_t kHeightPerScale = 8;
constexpr int32_t kPathTierHeight = 72;
constexpr uint32_t kNoFilament = 0xff000000u;
constexpr uint32_t kUnloadedTip = 0x3a3a3a;
constexpr size_t kMaxCached = 12;
// A draw task holds its image only until the refresh that queued it ends, far inside this.
constexpr uint32_t kEvictAfterMs = 1000;

struct Entry {
    ToolheadStyle style;
    int32_t height;
    uint32_t color;
    lv_draw_buf_t* buf;
    uint32_t last_used;
};

std::vector<Entry>& cache() {
    static std::vector<Entry> entries;
    return entries;
}

unsigned g_builds = 0;

void evict_stale() {
    auto& entries = cache();
    while (entries.size() >= kMaxCached) {
        auto oldest =
            std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return lv_tick_elaps(a.last_used) > lv_tick_elaps(b.last_used);
            });
        if (lv_tick_elaps(oldest->last_used) < kEvictAfterMs)
            return;
        lv_image_cache_drop(oldest->buf);
        lv_draw_buf_destroy(oldest->buf);
        entries.erase(oldest);
    }
}

struct Decoded {
    lv_image_decoder_dsc_t dsc{};
    bool open = false;
    explicit Decoded(const std::string& path) {
        open = lv_image_decoder_open(&dsc, path.c_str(), nullptr) == LV_RESULT_OK && dsc.decoded &&
               dsc.decoded->header.cf == LV_COLOR_FORMAT_ARGB8888;
        if (!open)
            spdlog::warn("[ToolheadArt] Failed to decode {}", path);
    }
    ~Decoded() {
        if (dsc.decoder)
            lv_image_decoder_close(&dsc);
    }
    Decoded(const Decoded&) = delete;
    Decoded& operator=(const Decoded&) = delete;
};

/// A layer's pixels with no row padding.
std::vector<uint8_t> packed(const lv_draw_buf_t* buf) {
    const int32_t w = buf->header.w, h = buf->header.h;
    std::vector<uint8_t> out(static_cast<size_t>(w * h * 4));
    for (int32_t y = 0; y < h; ++y)
        std::memcpy(&out[static_cast<size_t>(y * w * 4)], buf->data + y * buf->header.stride,
                    static_cast<size_t>(w * 4));
    return out;
}

lv_draw_buf_t* build_image(const ToolheadArt& art, int32_t height, int32_t width, uint32_t color) {
    const std::string base =
        helix::asset_component_uri(std::string("assets/images/toolheads/") + art.id + "/" +
                                   (height <= kPathTierHeight ? "path" : "full"));
    Decoded body(base + ".png"), tint(base + "_tint.png"), shade(base + "_shade.png");
    if (!body.open || !tint.open || !shade.open)
        return nullptr;
    const int32_t src_w = body.dsc.decoded->header.w, src_h = body.dsc.decoded->header.h;
    if (tint.dsc.decoded->header.w != src_w || shade.dsc.decoded->header.w != src_w ||
        tint.dsc.decoded->header.h != src_h || shade.dsc.decoded->header.h != src_h) {
        spdlog::warn("[ToolheadArt] {} layers differ in size", art.id);
        return nullptr;
    }
    std::vector<uint8_t> pixels = packed(body.dsc.decoded);
    toolhead_art_compose(pixels.data(), packed(tint.dsc.decoded).data(),
                         packed(shade.dsc.decoded).data(), src_w, src_h,
                         lv_color_hex(color == kNoFilament ? kUnloadedTip : color));

    lv_draw_buf_t* buf = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf)
        return nullptr;
    std::vector<uint8_t> scaled(static_cast<size_t>(width * height * 4));
    toolhead_art_resample(pixels.data(), src_w, src_h, scaled.data(), width, height);
    for (int32_t y = 0; y < height; ++y)
        std::memcpy(buf->data + y * buf->header.stride, &scaled[static_cast<size_t>(y * width * 4)],
                    static_cast<size_t>(width * 4));
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

void toolhead_art_resample(const uint8_t* src, int32_t src_w, int32_t src_h, uint8_t* dst,
                           int32_t dst_w, int32_t dst_h) {
    const float fx = static_cast<float>(src_w) / static_cast<float>(dst_w);
    const float fy = static_cast<float>(src_h) / static_cast<float>(dst_h);
    for (int32_t y = 0; y < dst_h; ++y) {
        const float y0 = static_cast<float>(y) * fy, y1 = y0 + fy;
        for (int32_t x = 0; x < dst_w; ++x) {
            const float x0 = static_cast<float>(x) * fx, x1 = x0 + fx;
            float b = 0, g = 0, r = 0, a = 0, area = 0;
            for (int32_t sy = static_cast<int32_t>(y0);
                 sy < std::min(src_h, static_cast<int32_t>(std::ceil(y1))); ++sy) {
                const float wy = std::min(y1, sy + 1.0f) - std::max(y0, static_cast<float>(sy));
                for (int32_t sx = static_cast<int32_t>(x0);
                     sx < std::min(src_w, static_cast<int32_t>(std::ceil(x1))); ++sx) {
                    const float wt =
                        wy * (std::min(x1, sx + 1.0f) - std::max(x0, static_cast<float>(sx)));
                    const uint8_t* p = src + (sy * src_w + sx) * 4;
                    const float pa = p[3] * wt;
                    b += p[0] * pa;
                    g += p[1] * pa;
                    r += p[2] * pa;
                    a += pa;
                    area += wt;
                }
            }
            uint8_t* out = dst + (y * dst_w + x) * 4;
            if (a <= 0 || area <= 0) {
                std::memset(out, 0, 4);
                continue;
            }
            out[0] = static_cast<uint8_t>(b / a + 0.5f);
            out[1] = static_cast<uint8_t>(g / a + 0.5f);
            out[2] = static_cast<uint8_t>(r / a + 0.5f);
            out[3] = static_cast<uint8_t>(a / area + 0.5f);
        }
    }
}

bool draw_toolhead_art(lv_layer_t* layer, ToolheadStyle style, int32_t cx, int32_t nozzle_y,
                       std::optional<lv_color_t> filament, int32_t scale, lv_opa_t opa) {
    const ToolheadArt* art = toolhead_art_for(style);
    if (!art || scale <= 0)
        return false;
    const ToolheadArtRect r = toolhead_art_rect(*art, cx, nozzle_y, scale);
    const uint32_t color = filament ? lv_color_to_u32(*filament) & 0xffffffu : kNoFilament;

    auto& entries = cache();
    auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) {
        return e.style == style && e.height == r.h && e.color == color;
    });
    if (it == entries.end()) {
        evict_stale();
        lv_draw_buf_t* buf = build_image(*art, r.h, r.w, color);
        if (!buf)
            return false;
        entries.push_back({style, r.h, color, buf, lv_tick_get()});
        it = entries.end() - 1;
    }
    it->last_used = lv_tick_get();

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = it->buf;
    dsc.opa = opa;
    const lv_area_t coords{r.x, r.y, r.x + r.w - 1, r.y + r.h - 1};
    lv_draw_image(layer, &dsc, &coords);
    return true;
}

unsigned toolhead_art_build_count() {
    return g_builds;
}

} // namespace helix::ui
