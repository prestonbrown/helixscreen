// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <atomic>
#include <cstdint>

namespace helix {

/**
 * @brief Per-channel gamma + warmth color correction for the framebuffer.
 *
 * Applied in the display flush path to compensate for panel-specific
 * gamma curves and color temperature. Used primarily for cheap LCDs
 * (CC1, AD5M) where dark colors look tinted (e.g. purple) due to
 * uneven channel response.
 *
 * Two knobs:
 *   - gamma: 0.5 .. 2.0 (default 1.0)  — uniform gamma curve
 *   - warmth: -50 .. +50 (default 0)   — shifts R/B balance.
 *     positive = warmer (more R, less B); negative = cooler.
 *
 * Builds three 256-entry LUTs (one per channel) when settings change.
 * The flush hook applies them in-place to the rendered buffer right
 * before scanout. Identity transform (gamma=1, warmth=0) skips the
 * walk entirely so the cost is zero on platforms that don't need it.
 */
class ColorTransform {
  public:
    /** @brief Reset to identity (no transform). */
    void reset();

    /** @brief Update LUTs from gamma, warmth, and tint.
     *
     *  warmth: R↔B balance (positive = warmer/more red, negative = cooler/more blue)
     *  tint:   G axis     (positive = more green, negative = more magenta)
     *  Together these form a standard 2-axis white-balance correction.
     */
    void set(float gamma, int warmth, int tint);

    /** @brief Set per-channel factory panel gain (baseline, applied before
     *  user warmth/tint). Values are in 0..1 (1.0 = no attenuation). Use to
     *  bake panel-specific white-point correction into a preset (e.g. cc1.json).
     *  Call before set() — set() rebuilds the LUTs honoring these gains.
     */
    void set_panel_gain(float r_gain, float g_gain, float b_gain);

    /** @brief True if the transform is identity (no-op). */
    bool is_identity() const {
        return identity_;
    }

    /** @brief Apply LUT in-place to a rendered buffer. Format-aware. */
    void apply(uint8_t* buf, int width, int height, int stride_bytes, lv_color_format_t cf) const;

    /** @brief Apply LUT in-place to ONLY a sub-rectangle of a buffer. */
    void apply_area(uint8_t* buf, int buf_stride_bytes, int x, int y, int w, int h,
                    lv_color_format_t cf) const;

    /**
     * @brief The slice of a flush's px_map that apply_area() must walk. Pure.
     *
     * The DRM backend renders DIRECT into dumb buffers whose pitch is
     * kernel-aligned and can exceed width * bpp, and px_map is then the whole
     * screen: the walk must start at the dirty rect's absolute coordinates and
     * step rows by the buffer's real stride. In PARTIAL render mode LVGL
     * reshapes the draw buffer to the dirty area, so px_map starts at the
     * area's own origin and the offset is zero. Walking the area width's pitch
     * from the buffer origin instead recolours a skewed parallelogram at the
     * top-left corner (prestonbrown/helixscreen#1592).
     */
    struct FlushRegion {
        uint32_t stride_bytes = 0; ///< Row pitch of the buffer px_map points into
        int x = 0;                 ///< Dirty-rect column offset within px_map
        int y = 0;                 ///< Dirty-rect row offset within px_map
    };

    static FlushRegion select_flush_region(const lv_draw_buf_t* active_buf, const lv_area_t& area,
                                           lv_color_format_t cf,
                                           lv_display_render_mode_t render_mode);

  private:
    uint8_t r_lut_[256] = {};
    uint8_t g_lut_[256] = {};
    uint8_t b_lut_[256] = {};
    float panel_r_gain_ = 1.0f;
    float panel_g_gain_ = 1.0f;
    float panel_b_gain_ = 1.0f;
    bool identity_ = true;
};

} // namespace helix
