// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "flush_stride.h"

#include <lvgl/lvgl.h>

#include "../catch_amalgamated.hpp"

// ============================================================================
// helix::flush_px_map_stride: the one dbuf-stride-or-fallback rule every
// flush-path px_map consumer derives its row pitch from. These tests pin the
// rule's behaviour; the lint gate in test_code_lint.bats (no inline copy at
// the call sites) is what keeps a second copy from appearing
// (prestonbrown/helixscreen#1610).
// ============================================================================

namespace {

// Minimal stand-in for the display's active draw buffer: the stride rule only
// reads header.stride.
lv_draw_buf_t dbuf_with_stride(uint32_t stride) {
    lv_draw_buf_t db{};
    db.header.stride = stride;
    return db;
}

} // namespace

TEST_CASE("flush_px_map_stride returns the active buffer's kernel-aligned pitch",
          "[display][1610]") {
    // A 100px-wide XRGB8888 row is 400 bytes tight; a DRM dumb buffer hands
    // back 832. The helper must hand on the buffer's own value, so assert it
    // differs from the computed one too: that proves the buffer branch ran,
    // not that both branches happen to agree.
    lv_draw_buf_t dbuf = dbuf_with_stride(832);
    const uint32_t computed = lv_draw_buf_width_to_stride(100, LV_COLOR_FORMAT_XRGB8888);
    REQUIRE(computed != 832);

    REQUIRE(helix::flush_px_map_stride(&dbuf, 100, LV_COLOR_FORMAT_XRGB8888) == 832);
}

TEST_CASE("flush_px_map_stride falls back to the computed stride without a buffer",
          "[display][1610]") {
    REQUIRE(helix::flush_px_map_stride(nullptr, 40, LV_COLOR_FORMAT_XRGB8888) ==
            lv_draw_buf_width_to_stride(40, LV_COLOR_FORMAT_XRGB8888));
    REQUIRE(helix::flush_px_map_stride(nullptr, 30, LV_COLOR_FORMAT_RGB565) ==
            lv_draw_buf_width_to_stride(30, LV_COLOR_FORMAT_RGB565));
}

TEST_CASE("flush_px_map_stride distrusts a zero-stride draw buffer", "[display][1610]") {
    // stride == 0 means the header is untrustworthy, not that rows are
    // packed tight: the rule must take the computed fallback, never hand on
    // the zero (a walk stepping 0 bytes per row recolours one row forever).
    lv_draw_buf_t dbuf = dbuf_with_stride(0);
    const uint32_t computed = lv_draw_buf_width_to_stride(30, LV_COLOR_FORMAT_RGB565);
    REQUIRE(computed > 0);

    REQUIRE(helix::flush_px_map_stride(&dbuf, 30, LV_COLOR_FORMAT_RGB565) == computed);
}
