// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_gradient_canvas.h"

#include "../lvgl_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/src/draw/lv_draw_buf_private.h" // handler fields: no public setters

#include "../catch_amalgamated.hpp"

namespace {

// Reads a pixel of either buffer as 8-bit RGB, the way the display would show it.
lv_color32_t pixel(const lv_draw_buf_t* buf, int32_t x, int32_t y) {
    const uint8_t* row = buf->data + static_cast<uint32_t>(y) * buf->header.stride;
    if (buf->header.cf == LV_COLOR_FORMAT_RGB565) {
        const uint16_t v = reinterpret_cast<const uint16_t*>(row)[x];
        return lv_color32_t{static_cast<uint8_t>((v & 0x1F) << 3),
                            static_cast<uint8_t>(((v >> 5) & 0x3F) << 2),
                            static_cast<uint8_t>((v >> 11) << 3), 255};
    }
    return reinterpret_cast<const lv_color32_t*>(row)[x];
}

// What a 565 panel or a 32-bit framebuffer keeps of an 8-bit channel.
uint8_t as_shown(uint8_t c, int bits) {
    return LV_COLOR_DEPTH == 16 ? static_cast<uint8_t>((c >> (8 - bits)) << (8 - bits)) : c;
}

} // namespace

TEST_CASE_METHOD(
    LVGLTestFixture,
    "gradient canvas: the opaque buffer is the masked gradient flattened onto the background",
    "[gradient_canvas]") {
    constexpr int32_t W = 160, H = 200, R = 8;
    const lv_color_t behind = lv_color_hex(0x101418);
    const lv_color_t under = lv_color_hex(0x2A2E33);
    lv_draw_buf_t* masked = ui_gradient_canvas_create_buf(W, H, true, R);
    lv_draw_buf_t* opaque =
        helix::ui::gradient_canvas_create_opaque_buf(W, H, true, R, behind, under);
    REQUIRE(masked != nullptr);
    REQUIRE(opaque != nullptr);

    // Native and alpha-free, so LVGL draws it as a plain copy.
    CHECK(opaque->header.cf == LV_COLOR_FORMAT_DEFAULT);
    CHECK_FALSE(lv_color_format_has_alpha(static_cast<lv_color_format_t>(opaque->header.cf)));

    int checked_inside = 0, checked_outside = 0, checked_fringe = 0, mismatches = 0;
    const int tol_rb = LV_COLOR_DEPTH == 16 ? 8 : 1, tol_g = LV_COLOR_DEPTH == 16 ? 4 : 1;
    for (int32_t y = 0; y < H; y++) {
        for (int32_t x = 0; x < W; x++) {
            const lv_color32_t m = pixel(masked, x, y);
            const lv_color32_t o = pixel(opaque, x, y);
            const uint8_t a = m.alpha;
            auto over = [&](int fg, int bg) { return (fg * a + bg * (255 - a) + 127) / 255; };
            auto expect = [&](uint8_t fg, uint8_t card, uint8_t page, int bits) {
                return as_shown(static_cast<uint8_t>(over(fg, over(card, page))), bits);
            };
            const bool ok =
                std::abs(o.red - expect(m.red, under.red, behind.red, 5)) <= tol_rb &&
                std::abs(o.green - expect(m.green, under.green, behind.green, 6)) <= tol_g &&
                std::abs(o.blue - expect(m.blue, under.blue, behind.blue, 5)) <= tol_rb;
            if (!ok && mismatches++ < 3) {
                UNSCOPED_INFO("x=" << x << " y=" << y << " a=" << int(a) << " got " << int(o.red)
                                   << "," << int(o.green) << "," << int(o.blue));
            }
            if (a == 255) {
                checked_inside++;
                // The dithered gradient survives exactly where nothing is masked.
                if (LV_COLOR_DEPTH == 32 &&
                    (o.red != m.red || o.green != m.green || o.blue != m.blue))
                    mismatches++;
            } else if (a == 0) {
                checked_outside++;
            } else {
                checked_fringe++;
            }
        }
    }
    CHECK(mismatches == 0);
    CHECK(checked_inside > 0);
    CHECK(checked_outside > 0); // the corners really were masked
    CHECK(checked_fringe > 0);

    lv_draw_buf_destroy(masked);
    lv_draw_buf_destroy(opaque);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "gradient canvas: the XML widget's buffer is native and alpha-free",
                 "[gradient_canvas]") {
    // The detail view's backdrop size on an 800x480 panel. Every pixel is
    // opaque, so a native buffer costs half an ARGB8888 one on a 16-bit display.
    ui_gradient_canvas_register();
    lv_obj_t* img =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ui_gradient_canvas", nullptr));
    REQUIRE(img != nullptr);
    lv_obj_set_size(img, 512, 406);
    lv_obj_update_layout(img);

    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(img));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.w == 512);
    CHECK(buf->header.h == 406);
    CHECK(buf->header.cf == LV_COLOR_FORMAT_DEFAULT);
    CHECK(buf->data_size == lv_draw_buf_width_to_stride(512, LV_COLOR_FORMAT_DEFAULT) * 406u);

    // Dark theme: bright top-right, dark bottom-left.
    const lv_color32_t tr = pixel(buf, 511, 0), bl = pixel(buf, 0, 405);
    CHECK(tr.green > bl.green + 40);
}

TEST_CASE_METHOD(LVGLTestFixture, "gradient canvas: renders into an RGB565 buffer",
                 "[gradient_canvas]") {
    // A 16-bit display's native format, which host builds never pick on their own.
    constexpr int32_t W = 64, H = 48;
    lv_draw_buf_t* buf = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, 0);
    REQUIRE(buf != nullptr);
    helix::ui::gradient_canvas_render(buf, lv_color_make(200, 100, 50), lv_color_make(40, 160, 248),
                                      false);

    // 565 keeps the top 5/6/5 bits of each channel.
    const lv_color32_t tr = pixel(buf, W - 1, 0), bl = pixel(buf, 0, H - 1);
    CHECK(int(tr.red) == 200);
    CHECK(int(tr.green) == 100);
    CHECK(int(tr.blue) == 48);
    CHECK(int(bl.red) == 40);
    CHECK(int(bl.green) == 160);
    CHECK(int(bl.blue) == 248);

    lv_draw_buf_destroy(buf);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "gradient canvas: a resize the heap cannot fit leaves no freed buffer as the src",
                 "[gradient_canvas]") {
    ui_gradient_canvas_register();
    lv_obj_t* img =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ui_gradient_canvas", nullptr));
    REQUIRE(img != nullptr);
    const void* old_src = lv_image_get_src(img);
    REQUIRE(old_src != nullptr);

    lv_draw_buf_handlers_t* handlers = lv_draw_buf_get_handlers();
    auto* const orig_malloc = handlers->buf_malloc_cb;
    handlers->buf_malloc_cb = [](size_t, lv_color_format_t) -> void* { return nullptr; };
    lv_obj_set_size(img, 400, 300);
    lv_obj_update_layout(img);
    handlers->buf_malloc_cb = orig_malloc;

    // The old buffer is gone; an image still naming it reads freed memory on the next draw.
    CHECK(lv_image_get_src(img) != old_src);
    process_lvgl(30);
    lv_obj_delete(img);
}
