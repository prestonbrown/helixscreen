// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_ENABLE_SCREENSAVER

#include <spdlog/common.h>

#include <cstdint>
#include <lvgl.h>
#include <string>

/**
 * @file screensaver_image.h
 * @brief The images savers decode: sprites, art and the printer picture
 */

namespace helix::ui {

/**
 * @brief `path` decoded once into a draw buffer the caller owns, or nullptr
 *
 * LV_CACHE_DEF_SIZE is 0, so an image handed to LVGL as a path is decoded again on every
 * redraw; a saver decodes once and draws from the buffer. Free it with
 * helix::safe_draw_buf_destroy(). A failure is logged at `failure_level`, naming the image as
 * `what`: a missing bundled asset is worth a warning, a thumbnail that will not decode is not.
 */
lv_draw_buf_t* saver_decode_image(const char* path, const char* what,
                                  spdlog::level::level_enum failure_level = spdlog::level::warn);

/// Whether saver_map_rows() can rewrite a buffer of `cf`: the 32-bit formats saver art decodes to.
inline bool saver_rows_mappable(uint32_t cf) {
    return cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888;
}

/**
 * @brief Writes `map(pixel)` of `src` into `out` for rows [`from`, `to`)
 *
 * Both buffers are the same size in a format saver_rows_mappable() accepts; rows outside the
 * buffer are skipped. `map` takes and returns an lv_color32_t.
 */
template <typename Map>
void saver_map_rows(const lv_draw_buf_t& src, lv_draw_buf_t& out, int32_t from, int32_t to,
                    Map map) {
    const int32_t h = static_cast<int32_t>(src.header.h);
    const int32_t w = static_cast<int32_t>(src.header.w);
    from = from < 0 ? 0 : from;
    to = to > h ? h : to;
    for (int32_t y = from; y < to; y++) {
        const auto* in = reinterpret_cast<const lv_color32_t*>(src.data + y * src.header.stride);
        auto* row = reinterpret_cast<lv_color32_t*>(out.data + y * out.header.stride);
        for (int32_t x = 0; x < w; x++) {
            row[x] = map(in[x]);
        }
    }
}

/// Reads `path`'s header without decoding its pixels; false when it cannot be read.
bool saver_image_info(const char* path, lv_image_header_t& out);

/// The printer picture a saver shows at `screen_w`: the image the user picked, else the one
/// for the configured printer type, else the generic printer. Never empty.
std::string saver_printer_image_path(int32_t screen_w);

} // namespace helix::ui

#endif // HELIX_ENABLE_SCREENSAVER
