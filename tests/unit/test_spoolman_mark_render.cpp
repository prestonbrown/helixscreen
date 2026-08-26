// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Regression guard for the chip_spoolman_mark badge in ams_edit_overlay.xml.
//
// The badge originally rendered zero visible pixels. Root cause: an lv_image
// with a small explicit widget size (20x20), a downscaling scale transform,
// and the default inner_align (CENTER). LVGL first re-centers the *unscaled*
// 64x64 source area on the 20x20 widget (top-left lands ~22px outside the
// box), then applies the scale around the pivot — so the shrunk result is
// anchored outside the widget's clip rect and nothing draws. See
// lv_image.c draw_image() (image_area is built from img->w/img->h then
// lv_area_align'd before scaling).
//
// The fix renders the mark at the asset's intrinsic size (a pre-scaled
// spoolman_24.png) with recolor, matching the working AMS-logo draw path.
//
// Three layers of guard, in the order that matters:
//   1. the shipped ui_xml/ams_edit_overlay.xml element still declares the
//      intrinsic-size + recolor config and carries no sizing or transform,
//   2. the shipped AssetManager registration still points "spoolman_mark" at
//      the pre-scaled 24px asset, which still decodes at 24x24,
//   3. the LVGL draw path still behaves the way (1) and (2) assume — the
//      intrinsic config draws pixels and the old trap config draws none.

#include "../lvgl_ui_test_fixture.h"
#include "asset_manager.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <fstream>
#include <lvgl.h>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

// Count pixels with non-zero alpha in an ARGB8888 snapshot of the object.
int count_rendered_pixels(lv_obj_t* obj) {
    lv_draw_buf_t* snap = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    if (!snap)
        return -1;
    int count = 0;
    const uint8_t* data = snap->data;
    uint32_t w = snap->header.w;
    uint32_t h = snap->header.h;
    uint32_t stride = snap->header.stride ? snap->header.stride : w * 4;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = data + y * stride;
        for (uint32_t x = 0; x < w; ++x) {
            if (row[x * 4 + 3] != 0)
                count++; // ARGB8888 memory order: B,G,R,A
        }
    }
    lv_draw_buf_destroy(snap);
    return count;
}

lv_obj_t* recolored_image(lv_obj_t* parent, const char* path) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, path);
    lv_obj_set_style_image_recolor(img, lv_color_hex(0x9E9E9E), 0); // ~text_muted
    lv_obj_set_style_image_recolor_opa(img, 255, 0);
    return img;
}

// The src AssetManager::register_images() bound to the "spoolman_mark" name.
// Reading it back means a change in src/application/asset_manager.cpp lands
// here rather than leaving this file asserting on a stale hard-coded path.
std::string shipped_mark_src() {
    AssetManager::register_images(); // no-op if the fixture already did it
    const auto* src = static_cast<const char*>(lv_xml_get_image(nullptr, "spoolman_mark"));
    REQUIRE(src != nullptr);
    return src;
}

// helix-tests runs from the repo root (see MEMORY / tests README).
std::string read_repo_file(const std::string& path) {
    std::ifstream in(path);
    REQUIRE(in.is_open());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The <lv_image name="chip_spoolman_mark" …> open tag, attributes and all.
std::string chip_spoolman_mark_element(const std::string& xml) {
    const size_t name_at = xml.find("name=\"chip_spoolman_mark\"");
    REQUIRE(name_at != std::string::npos);
    const size_t start = xml.rfind('<', name_at);
    REQUIRE(start != std::string::npos);
    const size_t end = xml.find('>', name_at);
    REQUIRE(end != std::string::npos);
    return xml.substr(start, end - start + 1);
}

} // namespace

// The draw-path cases below build their own lv_image objects, so they stay
// green no matter what ams_edit_overlay.xml says. This one is what actually
// fails if the badge is edited back to the trap config.
TEST_CASE("chip_spoolman_mark ships the intrinsic-size recolor config", "[assets][xml]") {
    const std::string el =
        chip_spoolman_mark_element(read_repo_file("ui_xml/ams_edit_overlay.xml"));
    INFO("shipped element: " << el);

    // Intrinsic size means the asset's own dimensions decide the widget box:
    // no explicit width/height, no scale, no inner_align override.
    REQUIRE(el.find("src=\"spoolman_mark\"") != std::string::npos);
    CHECK(el.find("width=") == std::string::npos);
    CHECK(el.find("height=") == std::string::npos);
    CHECK(el.find("scale") == std::string::npos);
    CHECK(el.find("transform") == std::string::npos);
    CHECK(el.find("inner_align") == std::string::npos);

    // Tinting must be recolor — the one tint path the intrinsic draw honours.
    CHECK(el.find("style_image_recolor=") != std::string::npos);
    CHECK(el.find("style_image_recolor_opa=") != std::string::npos);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spoolman mark asset is the pre-scaled 24px one", "[assets]") {
    const std::string src = shipped_mark_src();
    INFO("registered src=" << src);
    // The whole fix is that the asset is pre-scaled, so nothing has to scale it.
    REQUIRE(src.find("spoolman_24.png") != std::string::npos);

    lv_image_header_t header;
    lv_result_t dec = lv_image_decoder_get_info(src.c_str(), &header);
    INFO("decode=" << (int)dec << " w=" << header.w << " h=" << header.h);
    REQUIRE(dec == LV_RESULT_OK);
    REQUIRE(header.w == 24);
    REQUIRE(header.h == 24);
}

TEST_CASE_METHOD(LVGLUITestFixture, "spoolman mark renders at intrinsic size with recolor",
                 "[assets]") {
    // This is the shipped configuration: intrinsic size, recolor, no transform.
    lv_obj_t* img = recolored_image(test_screen(), shipped_mark_src().c_str());
    process_lvgl(50);

    int px = count_rendered_pixels(img);
    INFO("rendered pixels=" << px);
    REQUIRE(px > 0);
}

TEST_CASE_METHOD(LVGLUITestFixture, "small-widget scale transform renders nothing (the trap)",
                 "[assets]") {
    // Reproduces the original broken config to guard against reintroduction:
    // explicit 20x20 size + downscale transform + default CENTER align.
    lv_obj_t* img = lv_image_create(test_screen());
    lv_image_set_src(img, "A:assets/images/ams/spoolman_64.png");

    // Anti-vacuity: if the 64px source stopped decoding, the trap config would
    // "pass" for the wrong reason — nothing to draw rather than drawn outside
    // the clip rect. Prove the source is real before asserting on the trap.
    lv_image_header_t header;
    REQUIRE(lv_image_decoder_get_info("A:assets/images/ams/spoolman_64.png", &header) ==
            LV_RESULT_OK);
    REQUIRE(header.w == 64);

    lv_obj_set_size(img, 20, 20);
    lv_image_set_scale(img, 256 * 20 / 64); // 64px -> 20px
    lv_image_set_pivot(img, 0, 0);
    process_lvgl(50);

    REQUIRE(count_rendered_pixels(img) == 0);
}
