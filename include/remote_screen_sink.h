// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace helix {

/**
 * @brief Source pixel format of a remote-screen frame.
 *
 * LVGL-independent so sinks need not include lvgl.h. The flush hook maps
 * `lv_color_format_t` onto this. `BGRA8888` is LVGL's native ARGB8888 in
 * little-endian memory (B,G,R,A byte order — matches a 32bpp fbdev directly).
 * `RGB565` is 2 bytes/pixel little-endian and must be expanded on copy.
 */
enum class RemoteScreenPixelFormat {
    Unknown = 0,
    BGRA8888, ///< 4 bytes/px, B,G,R,A — straight copy to a 32bpp fbdev.
    RGB565,   ///< 2 bytes/px, little-endian 0bRRRRRGGGGGGBBBBB.
};

/**
 * @brief A single dirty-area frame update handed to remote-screen sinks.
 *
 * A non-owning view of the rendered pixels for one LVGL flush area. It is
 * built in the display flush path (main thread) and passed by const-ref to
 * every active sink. Coordinates are inclusive LVGL area coords
 * (`x2`/`y2` are the last pixel, not one-past-end).
 *
 * `color_format` carries the `lv_color_format_t` value as a plain int so
 * consumers that don't need LVGL headers can still route frames. This header
 * intentionally does NOT include `lvgl.h`.
 */
struct RemoteScreenFrame {
    const uint8_t* px_map = nullptr; ///< Source pixels for this dirty area.
    /**
     * Readable bytes at `px_map`, or 0 when the producer cannot determine it.
     *
     * A sink MUST NOT read past `px_map + px_map_len`. Zero means "unknown", and
     * a sink then has to infer a bound from `src_stride` and `disp_h` — an
     * inference that is wrong whenever the render buffer is smaller than the
     * display (e.g. a fallback backend after DRM init fails), which is how a
     * mirror blit ends up reading off the end of the draw buffer.
     */
    size_t px_map_len = 0;
    /**
     * Display coordinates of `px_map`'s pixel (0,0) — the source origin.
     *
     * (0,0) in LVGL's DIRECT/FULL render modes, where `px_map` is the whole
     * display buffer and a dirty rect's pixels sit at their absolute
     * coordinates. In PARTIAL mode the draw buffer is reshaped to the dirty
     * area itself (`lv_refr.c` `layer_reshape_draw_buf`) and flushed from its
     * own origin, so the rect's pixels start at row 0 / column 0 and the
     * producer sets these to the area's top-left. A sink must index the
     * source relative to this, not absolutely.
     */
    int32_t px_map_x = 0;
    int32_t px_map_y = 0;
    int32_t x1 = 0;          ///< Inclusive left of the dirty area.
    int32_t y1 = 0;          ///< Inclusive top of the dirty area.
    int32_t x2 = 0;          ///< Inclusive right of the dirty area.
    int32_t y2 = 0;          ///< Inclusive bottom of the dirty area.
    int32_t disp_w = 0;      ///< Full display horizontal resolution.
    int32_t disp_h = 0;      ///< Full display vertical resolution.
    int color_format = 0;    ///< lv_color_format_t as int (raw, for logging).
    uint32_t src_stride = 0; ///< Bytes per row of `px_map`.
    RemoteScreenPixelFormat src_format =
        RemoteScreenPixelFormat::Unknown; ///< Pixel layout of px_map.
};

/**
 * @brief Abstract remote-screen sink — the extension point.
 *
 * A sink consumes dirty-area frames and mirrors them somewhere (fb0 today,
 * a future in-process HTTP server later). Sinks are owned by
 * `RemoteScreenManager`. All calls are on the main (LVGL) thread.
 */
class RemoteScreenSink {
  public:
    virtual ~RemoteScreenSink() = default;

    /** @brief Acquire resources. Return false to stay inactive (UI unaffected). */
    virtual bool start() = 0;

    /** @brief Release resources. Idempotent. */
    virtual void stop() = 0;

    /** @brief Gate: true when this sink wants dirty-area frames right now. */
    virtual bool wants_frames() const = 0;

    /** @brief Consume one dirty-area update. */
    virtual void on_frame(const RemoteScreenFrame& frame) = 0;

    /** @brief Human-readable sink name for logging. */
    virtual const char* name() const = 0;
};

} // namespace helix
