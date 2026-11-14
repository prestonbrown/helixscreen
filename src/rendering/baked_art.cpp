// SPDX-License-Identifier: GPL-3.0-or-later

#include "baked_art.h"

#include "core/data_root_resolver.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <image/lv_image_decoder_private.h>

namespace helix::ui {

namespace {

constexpr size_t kMaxCached = 16;
// A draw task holds its image only until the refresh that queued it ends, far inside this.
constexpr uint32_t kEvictAfterMs = 1000;

struct Entry {
    std::string key;
    lv_draw_buf_t* buf;
    uint32_t last_used;
};

// Never destroyed: a static destructor would run after lv_deinit() has gone, so the images
// stay owned (and reachable) until the process ends.
std::vector<Entry>& cache() {
    static auto* entries = new std::vector<Entry>();
    return *entries;
}

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

} // namespace

BakedPixels baked_art_decode(const std::string& path, const char* who) {
    BakedPixels out;
    const std::string uri = helix::asset_component_uri(path);
    lv_image_decoder_dsc_t dsc{};
    const bool open = lv_image_decoder_open(&dsc, uri.c_str(), nullptr) == LV_RESULT_OK &&
                      dsc.decoded && dsc.decoded->header.cf == LV_COLOR_FORMAT_ARGB8888;
    if (open) {
        const lv_draw_buf_t* buf = dsc.decoded;
        out.w = buf->header.w;
        out.h = buf->header.h;
        out.data.resize(static_cast<size_t>(out.w * out.h * 4));
        for (int32_t y = 0; y < out.h; ++y)
            std::memcpy(&out.data[static_cast<size_t>(y * out.w * 4)],
                        buf->data + y * buf->header.stride, static_cast<size_t>(out.w * 4));
    } else {
        spdlog::warn("[{}] Failed to decode {}", who, uri);
    }
    if (dsc.decoder)
        lv_image_decoder_close(&dsc);
    return out;
}

void baked_art_resample(const uint8_t* src, int32_t src_w, int32_t src_h, uint8_t* dst,
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

lv_draw_buf_t* baked_art_buffer(const BakedPixels& src, int32_t w, int32_t h) {
    if (src.w <= 0 || w <= 0 || h <= 0)
        return nullptr;
    lv_draw_buf_t* buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf)
        return nullptr;
    std::vector<uint8_t> scaled(static_cast<size_t>(w * h * 4));
    baked_art_resample(src.data.data(), src.w, src.h, scaled.data(), w, h);
    for (int32_t y = 0; y < h; ++y)
        std::memcpy(buf->data + y * buf->header.stride, &scaled[static_cast<size_t>(y * w * 4)],
                    static_cast<size_t>(w * 4));
    return buf;
}

lv_draw_buf_t* baked_art_cached(const std::string& key,
                                const std::function<lv_draw_buf_t*()>& build) {
    auto& entries = cache();
    auto it =
        std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.key == key; });
    if (it == entries.end()) {
        evict_stale();
        lv_draw_buf_t* buf = build();
        if (!buf)
            return nullptr;
        entries.push_back({key, buf, lv_tick_get()});
        it = entries.end() - 1;
    }
    it->last_used = lv_tick_get();
    return it->buf;
}

void baked_art_draw(lv_layer_t* layer, lv_draw_buf_t* buf, int32_t x, int32_t y, lv_opa_t opa) {
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = buf;
    dsc.opa = opa;
    const lv_area_t coords{x, y, x + static_cast<int32_t>(buf->header.w) - 1,
                           y + static_cast<int32_t>(buf->header.h) - 1};
    lv_draw_image(layer, &dsc, &coords);
}

} // namespace helix::ui
