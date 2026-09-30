// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The EGL DRM flush decides per flushed area whether the display texture gets that
// area, the whole buffer, or nothing. The driver itself only builds for the EGL
// binary; the decision is a header both it and this test include.
//
// TEST_MIRROR_OK: exercises patches/lvgl-egl-partial-upload.patch. The code under test is
// LVGL C that the EGL driver includes, which cannot include a header of ours.

#include "drivers/display/drm/lv_linux_drm_egl_upload.h"

#include "../catch_amalgamated.hpp"

namespace {

constexpr unsigned int TEXTURE = 7;
constexpr int32_t W = 800;
constexpr int32_t H = 480;

lv_area_t rect(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    lv_area_t a;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    return a;
}

/// Partial upload on, and TEXTURE already filled at W x H.
lv_linux_drm_egl_upload_state_t filled_state() {
    lv_linux_drm_egl_upload_state_t s{};
    s.partial = true;
    lv_linux_drm_egl_upload_filled(&s, TEXTURE, W, H);
    return s;
}

lv_linux_drm_egl_upload_t plan(const lv_linux_drm_egl_upload_state_t& s, bool last,
                               const lv_area_t& area, lv_area_t* clipped = nullptr,
                               unsigned int texture = TEXTURE, int32_t w = W, int32_t h = H,
                               bool in_place = true) {
    lv_area_t scratch = rect(-1, -1, -1, -1);
    return lv_linux_drm_egl_upload_plan(&s, texture, w, h, in_place, last, &area,
                                        clipped ? clipped : &scratch);
}

} // namespace

TEST_CASE("EGL upload: switched off, only the last flush sends the whole buffer",
          "[display][egl_upload]") {
    lv_linux_drm_egl_upload_state_t s = filled_state();
    s.partial = false;
    const lv_area_t area = rect(10, 10, 20, 20);

    CHECK(plan(s, false, area) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    CHECK(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_FULL);
}

TEST_CASE("EGL upload: a texture never filled gets the whole buffer first",
          "[display][egl_upload]") {
    lv_linux_drm_egl_upload_state_t s{};
    s.partial = true;
    const lv_area_t area = rect(10, 10, 20, 20);

    CHECK(plan(s, false, area) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    CHECK(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_FULL);

    lv_linux_drm_egl_upload_filled(&s, TEXTURE, W, H);
    CHECK(plan(s, false, area) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
    CHECK(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
}

TEST_CASE("EGL upload: a filled texture gets each flushed area, last or not",
          "[display][egl_upload]") {
    const lv_linux_drm_egl_upload_state_t s = filled_state();
    const lv_area_t area = rect(100, 40, 299, 79);
    lv_area_t clipped = rect(0, 0, 0, 0);

    REQUIRE(plan(s, false, area, &clipped) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
    CHECK(clipped.x1 == 100);
    CHECK(clipped.y1 == 40);
    CHECK(clipped.x2 == 299);
    CHECK(clipped.y2 == 79);

    clipped = rect(0, 0, 0, 0);
    REQUIRE(plan(s, true, rect(0, 0, W - 1, H - 1), &clipped) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
    CHECK(clipped.x2 == W - 1);
    CHECK(clipped.y2 == H - 1);
}

TEST_CASE("EGL upload: a requested full upload waits for the last flush and happens once",
          "[display][egl_upload]") {
    lv_linux_drm_egl_upload_state_t s = filled_state();
    const lv_area_t area = rect(10, 10, 20, 20);
    s.full_pending = true;

    CHECK(plan(s, false, area) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    REQUIRE(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_FULL);

    lv_linux_drm_egl_upload_filled(&s, TEXTURE, W, H);
    CHECK_FALSE(s.full_pending);
    CHECK(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
}

TEST_CASE("EGL upload: a different texture or size gets the whole buffer",
          "[display][egl_upload]") {
    const lv_linux_drm_egl_upload_state_t s = filled_state();
    const lv_area_t area = rect(10, 10, 20, 20);
    REQUIRE(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_AREA);

    SECTION("new texture") {
        CHECK(plan(s, false, area, nullptr, TEXTURE + 1) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
        CHECK(plan(s, true, area, nullptr, TEXTURE + 1) == LV_LINUX_DRM_EGL_UPLOAD_FULL);
    }
    SECTION("new width") {
        CHECK(plan(s, false, area, nullptr, TEXTURE, W + 1) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
        CHECK(plan(s, true, area, nullptr, TEXTURE, W + 1) == LV_LINUX_DRM_EGL_UPLOAD_FULL);
    }
    SECTION("new height") {
        CHECK(plan(s, false, area, nullptr, TEXTURE, W, H + 1) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
        CHECK(plan(s, true, area, nullptr, TEXTURE, W, H + 1) == LV_LINUX_DRM_EGL_UPLOAD_FULL);
    }
}

TEST_CASE("EGL upload: areas not at display coordinates get the whole buffer",
          "[display][egl_upload]") {
    const lv_linux_drm_egl_upload_state_t s = filled_state();
    const lv_area_t area = rect(10, 10, 20, 20);

    CHECK(plan(s, false, area, nullptr, TEXTURE, W, H, false) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    CHECK(plan(s, true, area, nullptr, TEXTURE, W, H, false) == LV_LINUX_DRM_EGL_UPLOAD_FULL);
}

TEST_CASE("EGL upload: a flushed area is clipped to the texture", "[display][egl_upload]") {
    const lv_linux_drm_egl_upload_state_t s = filled_state();
    lv_area_t clipped = rect(0, 0, 0, 0);

    REQUIRE(plan(s, false, rect(-5, -3, 40, 30), &clipped) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
    CHECK(clipped.x1 == 0);
    CHECK(clipped.y1 == 0);
    CHECK(clipped.x2 == 40);
    CHECK(clipped.y2 == 30);

    REQUIRE(plan(s, false, rect(W - 10, H - 4, W + 50, H + 9), &clipped) ==
            LV_LINUX_DRM_EGL_UPLOAD_AREA);
    CHECK(clipped.x1 == W - 10);
    CHECK(clipped.y1 == H - 4);
    CHECK(clipped.x2 == W - 1);
    CHECK(clipped.y2 == H - 1);
}

TEST_CASE("EGL upload: an area wholly outside the texture sends nothing", "[display][egl_upload]") {
    const lv_linux_drm_egl_upload_state_t s = filled_state();

    CHECK(plan(s, false, rect(W, 0, W + 10, 10)) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    CHECK(plan(s, true, rect(0, H, 10, H + 10)) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    CHECK(plan(s, false, rect(-20, 0, -1, 10)) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
    CHECK(plan(s, false, rect(0, -20, 10, -1)) == LV_LINUX_DRM_EGL_UPLOAD_NONE);
}

TEST_CASE(
    "EGL upload: only a direct-mode buffer drawn without matrix rotation holds areas in place",
    "[display][egl_upload]") {
    // A display rotation alone keeps areas in place: LVGL draws at the rotated
    // resolution the texture is reshaped to, and the presentation pass rotates.
    CHECK(lv_linux_drm_egl_upload_in_place(LV_DISPLAY_RENDER_MODE_DIRECT, false));

    CHECK_FALSE(lv_linux_drm_egl_upload_in_place(LV_DISPLAY_RENDER_MODE_DIRECT, true));
    CHECK_FALSE(lv_linux_drm_egl_upload_in_place(LV_DISPLAY_RENDER_MODE_FULL, false));
    CHECK_FALSE(lv_linux_drm_egl_upload_in_place(LV_DISPLAY_RENDER_MODE_PARTIAL, false));
}

TEST_CASE("EGL upload: an area starts a stride per row and a pixel per column into the buffer",
          "[display][egl_upload]") {
    // A stride wider than width * pixel size tells a row step apart from a recomputed pitch.
    constexpr uint32_t STRIDE_32 = W * 4 + 4;
    constexpr uint32_t STRIDE_16 = W * 2 + 2;

    const lv_area_t origin = rect(0, 0, 10, 10);
    CHECK(lv_linux_drm_egl_upload_first_byte(&origin, STRIDE_32, 4) == 0);

    const lv_area_t row_only = rect(0, 3, 10, 10);
    CHECK(lv_linux_drm_egl_upload_first_byte(&row_only, STRIDE_32, 4) == 3 * STRIDE_32);

    const lv_area_t column_only = rect(5, 0, 10, 10);
    CHECK(lv_linux_drm_egl_upload_first_byte(&column_only, STRIDE_32, 4) == 5 * 4);

    const lv_area_t both = rect(7, 2, 10, 10);
    CHECK(lv_linux_drm_egl_upload_first_byte(&both, STRIDE_32, 4) == 2 * STRIDE_32 + 7 * 4);
    CHECK(lv_linux_drm_egl_upload_first_byte(&both, STRIDE_16, 2) == 2 * STRIDE_16 + 7 * 2);

    const lv_area_t far = rect(W - 1, H - 1, W - 1, H - 1);
    CHECK(lv_linux_drm_egl_upload_first_byte(&far, STRIDE_32, 4) ==
          static_cast<size_t>(H - 1) * STRIDE_32 + static_cast<size_t>(W - 1) * 4);
}

TEST_CASE("EGL upload: only a whole-buffer upload marks the texture filled",
          "[display][egl_upload]") {
    const lv_area_t area = rect(10, 10, 20, 20);

    SECTION("nothing sent on an early flush leaves the texture unfilled") {
        lv_linux_drm_egl_upload_state_t s{};
        s.partial = true;
        REQUIRE(plan(s, false, area) == LV_LINUX_DRM_EGL_UPLOAD_NONE);

        lv_linux_drm_egl_upload_record(&s, LV_LINUX_DRM_EGL_UPLOAD_NONE, TEXTURE, W, H);
        CHECK(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_FULL);
    }

    SECTION("a sub-area upload records nothing") {
        lv_linux_drm_egl_upload_state_t s = filled_state();
        s.full_pending = true;

        lv_linux_drm_egl_upload_record(&s, LV_LINUX_DRM_EGL_UPLOAD_AREA, TEXTURE + 1, W + 1, H + 1);
        CHECK(s.full_pending);
        CHECK(s.texture_id == TEXTURE);
        CHECK(s.width == W);
        CHECK(s.height == H);
    }

    SECTION("a whole-buffer upload fills the texture and settles a request") {
        lv_linux_drm_egl_upload_state_t s{};
        s.partial = true;
        s.full_pending = true;
        REQUIRE(plan(s, true, area) == LV_LINUX_DRM_EGL_UPLOAD_FULL);

        lv_linux_drm_egl_upload_record(&s, LV_LINUX_DRM_EGL_UPLOAD_FULL, TEXTURE, W, H);
        CHECK_FALSE(s.full_pending);
        CHECK(plan(s, false, area) == LV_LINUX_DRM_EGL_UPLOAD_AREA);
    }
}
