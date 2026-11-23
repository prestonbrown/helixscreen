// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_icon.h"
#include "ui_icon_codepoints.h"
#include "ui_icon_image.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "lodepng_encode.h"
#include "lvgl/src/misc/lv_text_private.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::icon;
using helix::plugin::test::TempDir;

namespace {

/// Writes a w x h PNG whose left half is opaque and right half transparent.
void write_half_png(const std::string& path, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w / 2; ++x)
            rgba[(static_cast<size_t>(y) * w + x) * 4 + 3] = 255;
    }
    unsigned char* out = nullptr;
    size_t size = 0;
    REQUIRE(lodepng_encode32(&out, &size, rgba.data(), w, h) == 0);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    free(out);
}

/// The A8 mask `font` draws for its icon glyph, or empty when the glyph resolves to
/// another font (the fallback).
std::vector<uint8_t> drawn_mask(const lv_font_t* font, const char* text, uint32_t* side) {
    uint32_t i = 0;
    const uint32_t letter = lv_text_encoded_next(text, &i);
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, letter, 0) || g.resolved_font != font)
        return {};
    lv_draw_buf_t* buf = lv_draw_buf_create(g.box_w, g.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    REQUIRE(lv_font_get_glyph_bitmap(&g, buf) == buf);
    std::vector<uint8_t> out(static_cast<size_t>(g.box_w) * g.box_h);
    for (uint32_t y = 0; y < g.box_h; ++y)
        std::copy_n(buf->data + y * buf->header.stride, g.box_w, out.data() + y * g.box_w);
    lv_draw_buf_destroy(buf);
    *side = g.box_w;
    return out;
}

const lv_font_t* icon_font(lv_obj_t* icon) {
    return lv_obj_get_style_text_font(icon, LV_PART_MAIN);
}

} // namespace

TEST_CASE("scale_alpha_mask fits and centers the image", "[icons][icon_image]") {
    // A 4x2 image: left column opaque, the rest 100.
    const std::vector<uint8_t> src = {255, 100, 100, 100, 255, 100, 100, 100};
    auto out = scale_alpha_mask(src.data(), 4, 2, 4);
    REQUIRE(out.size() == 16);
    // Rows 0 and 3 are the letterbox; rows 1 and 2 hold the image at full width.
    for (int x = 0; x < 4; ++x) {
        CHECK(out[x] == 0);
        CHECK(out[12 + x] == 0);
    }
    CHECK(out[4] == 255);
    CHECK(out[5] == 100);
    CHECK(out[8] == 255);
    CHECK(out[11] == 100);

    // Halving averages each 2x2 block.
    const std::vector<uint8_t> quad = {0, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto half = scale_alpha_mask(quad.data(), 4, 4, 2);
    CHECK(half == std::vector<uint8_t>{191, 0, 0, 0});

    // Doubling repeats pixels.
    const std::vector<uint8_t> one = {7};
    CHECK(scale_alpha_mask(one.data(), 1, 1, 2) == std::vector<uint8_t>{7, 7, 7, 7});
    CHECK(scale_alpha_mask(nullptr, 1, 1, 2) == std::vector<uint8_t>(4, 0));
}

TEST_CASE_METHOD(LVGLUITestFixture, "an image icon draws its PNG's alpha through the icon widget",
                 "[icons][icon_image]") {
    TempDir dir;
    write_half_png(dir.file("oil.png"), 32, 32);
    REQUIRE(register_image_icon("t-icons__oil", dir.file("oil.png"), "water") ==
            ImageIconStatus::Ok);
    CHECK(image_icon_available("t-icons__oil"));

    const char* attrs[] = {"src", "t-icons__oil", "size", "lg", "variant", "primary", nullptr};
    auto* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon);
    const lv_font_t* lg = font_for_size("lg");
    REQUIRE(image_icon_base_font(icon_font(icon)) == lg);
    CHECK(std::string(lv_label_get_text(icon)) == lookup_codepoint("water"));

    uint32_t side = 0;
    auto mask = drawn_mask(icon_font(icon), lv_label_get_text(icon), &side);
    REQUIRE(side == static_cast<uint32_t>(lg->line_height));
    REQUIRE(mask.size() == side * side);
    const uint32_t mid = side / 2;
    CHECK(mask[mid * side + 1] == 255);      // left half opaque
    CHECK(mask[mid * side + side - 2] == 0); // right half clear

    // A new size keeps the image, sized from the new MDI face.
    set_size(icon, "sm");
    const lv_font_t* sm = font_for_size("sm");
    REQUIRE(image_icon_base_font(icon_font(icon)) == sm);
    drawn_mask(icon_font(icon), lv_label_get_text(icon), &side);
    CHECK(side == static_cast<uint32_t>(sm->line_height));

    // An app icon source returns the widget to its MDI face.
    set_source(icon, "home");
    CHECK(icon_font(icon) == sm);
    CHECK(std::string(lv_label_get_text(icon)) == lookup_codepoint("home"));

    lv_obj_delete(icon);
    unregister_image_icon("t-icons__oil");
}

TEST_CASE_METHOD(LVGLUITestFixture, "a withdrawn or unreadable image icon draws its fallback",
                 "[icons][icon_image]") {
    TempDir dir;
    write_half_png(dir.file("oil.png"), 16, 16);
    REQUIRE(register_image_icon("t-icons__gone", dir.file("oil.png"), "water") ==
            ImageIconStatus::Ok);
    const char* attrs[] = {"src", "t-icons__gone", "size", "md", nullptr};
    auto* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", attrs));
    REQUIRE(icon);
    uint32_t side = 0;
    REQUIRE_FALSE(drawn_mask(icon_font(icon), lv_label_get_text(icon), &side).empty());

    // The live label keeps its glyph font, which hands the fallback codepoint to the
    // MDI face: the fallback icon at the same size.
    unregister_image_icon("t-icons__gone");
    CHECK_FALSE(image_icon_available("t-icons__gone"));
    CHECK(drawn_mask(icon_font(icon), lv_label_get_text(icon), &side).empty());
    uint32_t i = 0;
    const uint32_t letter = lv_text_encoded_next(lv_label_get_text(icon), &i);
    lv_font_glyph_dsc_t g;
    REQUIRE(lv_font_get_glyph_dsc(icon_font(icon), &g, letter, 0));
    CHECK(g.resolved_font == font_for_size("md"));
    CHECK_FALSE(g.is_placeholder);

    // Registering again brings the image back to the same label.
    REQUIRE(register_image_icon("t-icons__gone", dir.file("oil.png"), "water") ==
            ImageIconStatus::Ok);
    CHECK_FALSE(drawn_mask(icon_font(icon), lv_label_get_text(icon), &side).empty());
    lv_obj_delete(icon);
    unregister_image_icon("t-icons__gone");

    CHECK(register_image_icon("t-icons__missing", dir.file("nope.png"), "water") ==
          ImageIconStatus::Missing);
    std::ofstream(dir.file("junk.png")) << "not a png";
    CHECK(register_image_icon("t-icons__junk", dir.file("junk.png"), "water") ==
          ImageIconStatus::Invalid);
    write_half_png(dir.file("big.png"), kMaxImageIconSide + 1, 4);
    CHECK(register_image_icon("t-icons__big", dir.file("big.png"), "water") ==
          ImageIconStatus::TooLarge);
    CHECK_FALSE(image_icon_available("t-icons__missing"));

    const char* missing_attrs[] = {"src", "t-icons__missing", "size", "md", nullptr};
    auto* fallback = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", missing_attrs));
    CHECK(std::string(lv_label_get_text(fallback)) == lookup_codepoint("water"));
    CHECK(drawn_mask(icon_font(fallback), lv_label_get_text(fallback), &side).empty());
    lv_obj_delete(fallback);

    // A name never registered is an ordinary unknown icon.
    CHECK(image_icon_glyph("t-icons__never", font_for_size("md")).font == nullptr);
}

#endif // HELIX_HAS_PLUGINS
