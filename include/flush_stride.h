// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <cstdint>

namespace helix {

/**
 * @brief Row pitch, in bytes, to read a flush callback's px_map with.
 *
 * One rule shared by every px_map consumer in the flush path, the colour
 * transform walk (ColorTransform::select_flush_region) and the remote-screen
 * mirror (DisplayManager's flush hook): the active draw buffer's own stride
 * when a buffer with a positive stride is queryable, else a stride computed
 * from the flushed area's width and colour format. Both callers must derive
 * their stride here; a re-inlined copy would drift (prestonbrown/helixscreen#1610).
 *
 * The buffer's real pitch matters because the DRM backend renders into dumb
 * buffers whose pitch is kernel-aligned and can exceed width * bpp (direct/full
 * render mode): reading px_map with the computed stride mis-tracks rows.
 */
inline uint32_t flush_px_map_stride(const lv_draw_buf_t* active_buf, int32_t area_w,
                                    lv_color_format_t cf) {
    return (active_buf && active_buf->header.stride > 0) ? active_buf->header.stride
                                                         : lv_draw_buf_width_to_stride(area_w, cf);
}

} // namespace helix
