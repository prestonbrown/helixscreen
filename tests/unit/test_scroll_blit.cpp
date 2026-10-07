// SPDX-License-Identifier: GPL-3.0-or-later

// A blitted scroll must leave the retained frame exactly as a full render
// would, while rendering only the strip that scrolled into view.

#include "ui_virtual_list.h"

#include "../lvgl_test_fixture.h"
#include "lvgl/src/display/lv_display_private.h"
#include "scroll_blit.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

struct Capture {
    std::vector<uint8_t> frame;
    size_t stride = 0;
    uint32_t px_bytes = 0;
    uint64_t flushed_px = 0;
};

Capture* g_capture = nullptr;

// The firmware's flush: every rendered chunk lands in a retained full frame.
void capture_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const size_t row_bytes = static_cast<size_t>(lv_area_get_width(area)) * g_capture->px_bytes;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        std::memcpy(g_capture->frame.data() + static_cast<size_t>(y) * g_capture->stride +
                        static_cast<size_t>(area->x1) * g_capture->px_bytes,
                    px_map + static_cast<size_t>(y - area->y1) * row_bytes, row_bytes);
    }
    g_capture->flushed_px += static_cast<uint64_t>(lv_area_get_size(area));
    lv_display_flush_ready(disp);
}

bool g_claim_ok = true;
int g_claims = 0;

bool claim_rows(int32_t, int32_t) {
    g_claims++;
    return g_claim_ok;
}

lv_obj_t* plain_box(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    return o;
}

class ScrollBlitFixture : public LVGLTestFixture {
  public:
    ScrollBlitFixture() : disp_(lv_display_get_default()) {
        capture_.px_bytes = lv_color_format_get_size(lv_display_get_color_format(disp_));
        capture_.stride =
            static_cast<size_t>(lv_display_get_horizontal_resolution(disp_)) * capture_.px_bytes;
        capture_.frame.assign(
            capture_.stride * static_cast<size_t>(lv_display_get_vertical_resolution(disp_)), 0);
        g_capture = &capture_;
        saved_flush_ = disp_->flush_cb;
        lv_display_set_flush_cb(disp_, capture_flush);

        lv_obj_t* screen = create_test_screen();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        panel_ = plain_box(screen, 100, 40, 500, 400, 0x303848);

        list_ = lv_obj_create(panel_);
        lv_obj_remove_style_all(list_);
        lv_obj_set_pos(list_, 20, 20);
        lv_obj_set_size(list_, 400, 300);
        lv_obj_set_style_radius(list_, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(list_, lv_color_hex(0x404040), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_SCROLLBAR);
        lv_obj_set_style_width(list_, 4, LV_PART_SCROLLBAR);
        lv_obj_set_style_pad_right(list_, 3, LV_PART_SCROLLBAR);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ON);
        lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list_, 6, LV_PART_MAIN);
        lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 30; i++) {
            lv_obj_t* row = plain_box(list_, 0, 0, 360, 40, 0x506070 + 0x0a0503 * i);
            lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
            lv_obj_t* label = lv_label_create(row);
            lv_label_set_text_fmt(label, "Row %d", i);
            lv_obj_center(label);
            labels_.push_back(label);
        }
        render();
        scroll_blit_install(disp_, RetainedFrame{capture_.frame.data(), capture_.stride, scratch_,
                                                 sizeof(scratch_), claim_rows});
        scroll_blit_track(list_);
        g_claim_ok = true;
        g_claims = 0;
    }

    ~ScrollBlitFixture() override {
        scroll_blit_uninstall();
        lv_display_set_flush_cb(disp_, saved_flush_);
        g_capture = nullptr;
    }

    /// Pixels flushed by one render.
    uint64_t render() {
        capture_.flushed_px = 0;
        lv_refr_now(disp_);
        return capture_.flushed_px;
    }

    /// The frame a full redraw produces, leaving the retained frame as it was.
    std::vector<uint8_t> full_render() {
        std::vector<uint8_t> kept = capture_.frame;
        lv_obj_invalidate(lv_screen_active());
        render();
        std::vector<uint8_t> full = capture_.frame;
        capture_.frame = kept;
        return full;
    }

    /// Pixels where the retained frame differs from a full redraw, with the first one.
    std::string mismatch() {
        const std::vector<uint8_t> full = full_render();
        int count = 0;
        size_t x1 = SIZE_MAX, y1 = SIZE_MAX, x2 = 0, y2 = 0;
        for (size_t i = 0; i < full.size(); i += capture_.px_bytes) {
            if (std::memcmp(&full[i], &capture_.frame[i], capture_.px_bytes) != 0) {
                count++;
                const size_t x = (i % capture_.stride) / capture_.px_bytes, y = i / capture_.stride;
                x1 = std::min(x1, x), x2 = std::max(x2, x), y1 = std::min(y1, y),
                y2 = std::max(y2, y);
            }
        }
        if (count == 0)
            return "0 px differ";
        return std::to_string(count) + " px differ in x " + std::to_string(x1) + ".." +
               std::to_string(x2) + " y " + std::to_string(y1) + ".." + std::to_string(y2);
    }

    uint64_t region_px() {
        lv_area_t r;
        REQUIRE(scroll_blit_region(list_, &r));
        return static_cast<uint64_t>(lv_area_get_size(&r));
    }

    lv_display_t* disp_;
    Capture capture_;
    lv_display_flush_cb_t saved_flush_ = nullptr;
    uint8_t scratch_[4 * 800 * 4 + 7]{}; // a few rows, not a whole number of them
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* list_ = nullptr;
    std::vector<lv_obj_t*> labels_;
};

} // namespace

TEST_CASE("shift_rows moves rows within an area in either direction", "[scroll_blit]") {
    // 6 x 8 frame, one byte per pixel; the area is columns 1..4, rows 1..6.
    auto make = [] {
        std::vector<uint8_t> f(6 * 8);
        for (size_t i = 0; i < f.size(); i++)
            f[i] = static_cast<uint8_t>(i);
        return f;
    };
    const lv_area_t area = {1, 1, 4, 6};
    uint8_t scratch[9]; // two rows of four, plus slack

    for (bool use_scratch : {false, true}) {
        CAPTURE(use_scratch);
        for (int32_t dy : {2, -3, 5, -1}) {
            CAPTURE(dy);
            std::vector<uint8_t> f = make();
            const std::vector<uint8_t> orig = make();
            shift_rows(f.data(), 6, 1, area, dy, use_scratch ? scratch : nullptr, sizeof(scratch));
            for (int32_t y = 0; y < 8; y++) {
                for (int32_t x = 0; x < 6; x++) {
                    const int32_t src = y - dy;
                    const bool in_area = x >= 1 && x <= 4 && y >= 1 && y <= 6;
                    uint8_t want = orig[static_cast<size_t>(y * 6 + x)];
                    if (in_area && src >= 1 && src <= 6)
                        want = orig[static_cast<size_t>(src * 6 + x)];
                    REQUIRE(f[static_cast<size_t>(y * 6 + x)] == want);
                }
            }
        }
    }
}

TEST_CASE_METHOD(ScrollBlitFixture, "scroll blit matches a full render and draws only the strip",
                 "[scroll_blit]") {
    const uint64_t region = region_px();

    SECTION("one scroll each way") {
        for (int32_t dy : {-37, -60, 25, 50}) {
            CAPTURE(dy);
            lv_obj_scroll_by(list_, 0, dy, LV_ANIM_OFF);
            const uint64_t drawn = render();
            CHECK(drawn < region / 3);
            REQUIRE(mismatch() == "0 px differ");
        }
    }

    SECTION("two scrolls between renders, with a row changing in between") {
        lv_obj_scroll_by(list_, 0, -45, LV_ANIM_OFF);
        lv_label_set_text(labels_[3], "Changed");
        lv_obj_scroll_by(list_, 0, -20, LV_ANIM_OFF);
        CHECK(render() < region / 2);
        REQUIRE(mismatch() == "0 px differ");
    }
}

TEST_CASE_METHOD(ScrollBlitFixture, "scroll blit redraws widgets that stay put", "[scroll_blit]") {
    const uint64_t region = region_px();

    SECTION("a sibling drawn on top") {
        plain_box(panel_, 300, 200, 80, 80, 0xff0000);
    }
    SECTION("a sibling drawn beneath a transparent scroller") {
        lv_obj_t* under = plain_box(panel_, 300, 200, 80, 80, 0x00ff00);
        lv_obj_move_to_index(under, 0);
    }
    SECTION("a floating child") {
        lv_obj_t* fab = plain_box(list_, 300, 200, 40, 40, 0x0000ff);
        lv_obj_add_flag(fab, LV_OBJ_FLAG_FLOATING);
    }
    SECTION("a toast on the top layer") {
        plain_box(lv_layer_top(), 150, 100, 200, 40, 0xffff00);
    }
    render();
    for (int32_t dy : {-40, 30}) {
        CAPTURE(dy);
        lv_obj_scroll_by(list_, 0, dy, LV_ANIM_OFF);
        CHECK(render() < region / 2);
        REQUIRE(mismatch() == "0 px differ");
    }
}

TEST_CASE_METHOD(ScrollBlitFixture, "scroll blit renders in full where a copy cannot be exact",
                 "[scroll_blit]") {
    lv_area_t r;
    REQUIRE(scroll_blit_region(list_, &r));
    CHECK(r.x1 == 120);
    CHECK(r.y1 == 60);
    CHECK(r.x2 == 519);
    CHECK(r.y2 == 359);
    const uint64_t whole = static_cast<uint64_t>(lv_area_get_size(&r));

    SECTION("a gradient behind the content") {
        lv_obj_set_style_bg_grad_color(panel_, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(panel_, LV_GRAD_DIR_VER, LV_PART_MAIN);
    }
    SECTION("a translucent scroller") {
        lv_obj_set_style_opa(list_, LV_OPA_50, LV_PART_MAIN);
    }
    SECTION("a sibling over most of the region") {
        plain_box(panel_, 10, 10, 400, 250, 0xff0000);
    }
    SECTION("a custom draw handler on the scroller") {
        lv_obj_add_event_cb(list_, [](lv_event_t*) {}, LV_EVENT_DRAW_POST, nullptr);
    }
    lv_obj_update_layout(lv_screen_active());
    CHECK_FALSE(scroll_blit_region(list_, &r));

    render();
    lv_obj_scroll_by(list_, 0, -40, LV_ANIM_OFF);
    CHECK(render() >= whole / 2);
    REQUIRE(mismatch() == "0 px differ");
}

// A virtual list reorders its pool with invalidation off; the layout that
// follows must still redraw every slot that moved.
TEST_CASE_METHOD(ScrollBlitFixture,
                 "virtual list reorder leaves the frame a full render would draw",
                 "[scroll_blit][virtual_list]") {
    std::vector<ssize_t> items;
    for (ssize_t i = 0; i < static_cast<ssize_t>(labels_.size()); i++)
        items.push_back(i);
    int configured = 0;
    auto show = [&](int first) {
        helix::ui::show_window(
            list_, items, first, first + static_cast<int>(labels_.size()), false,
            [&](size_t slot) { return lv_obj_get_parent(labels_[slot]); },
            [&](size_t slot, ssize_t item) {
                lv_label_set_text_fmt(labels_[slot], "Item %d", static_cast<int>(item));
                configured++;
            },
            [&](size_t slot) {
                lv_obj_add_flag(lv_obj_get_parent(labels_[slot]), LV_OBJ_FLAG_HIDDEN);
            });
    };
    for (int first : {2, 5, 4}) {
        CAPTURE(first);
        configured = 0;
        show(first);
        CHECK(configured == std::abs(first - (first == 2 ? 0 : first == 5 ? 2 : 5)));
        render();
        REQUIRE(mismatch() == "0 px differ");
    }
}

TEST_CASE_METHOD(ScrollBlitFixture, "scroll blit edge cases still match a full render",
                 "[scroll_blit]") {
    const uint64_t whole = region_px();

    SECTION("an enclosing scroller's scrollbar over an opaque list") {
        lv_obj_set_style_bg_color(list_, lv_color_hex(0x283038), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(list_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_x(list_, 100); // right edge on the panel's own scrollbar
        plain_box(panel_, 0, 0, 10, 1000, 0x303848);
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(panel_, LV_SCROLLBAR_MODE_ON);
        lv_obj_set_style_width(panel_, 6, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0xffffff), LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, LV_PART_SCROLLBAR);
        render();
        lv_obj_scroll_by(list_, 0, -40, LV_ANIM_OFF);
        CHECK(render() < whole / 2);
    }
    SECTION("a bordered scroller") {
        lv_obj_set_style_border_width(list_, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(list_, lv_color_hex(0xe0e0e0), LV_PART_MAIN);
        lv_obj_set_style_border_opa(list_, LV_OPA_COVER, LV_PART_MAIN);
        render();
        lv_obj_scroll_by(list_, 0, -40, LV_ANIM_OFF);
        CHECK(render() < whole / 2);
    }
    SECTION("a scroll as tall as the region") {
        lv_obj_scroll_by(list_, 0, -300, LV_ANIM_OFF);
        CHECK(render() >= whole);
    }
    SECTION("a scroller already waiting for a full redraw") {
        lv_obj_invalidate(list_);
        lv_obj_scroll_by(list_, 0, -40, LV_ANIM_OFF);
        render();
        CHECK(g_claims == 0);
    }
    SECTION("a frame that cannot be taken") {
        g_claim_ok = false;
        lv_obj_scroll_by(list_, 0, -40, LV_ANIM_OFF);
        render();
        CHECK(g_claims > 0);
        CHECK(render() >= whole); // the refresh after repairs the region
    }
    SECTION("more static widgets than are worth tracking") {
        for (int i = 0; i < 9; i++)
            plain_box(panel_, 30 + 40 * i, 200, 10, 10, 0xff00ff);
        lv_obj_update_layout(lv_screen_active());
        lv_area_t r;
        CHECK_FALSE(scroll_blit_region(list_, &r));
        render();
        lv_obj_scroll_by(list_, 0, -40, LV_ANIM_OFF);
        CHECK(render() >= whole);
    }
    REQUIRE(mismatch() == "0 px differ");
}

// Layout runs after the refresh starts, and content that shrinks under a
// scroller parked at its end scrolls it back from inside that layout.
TEST_CASE_METHOD(ScrollBlitFixture, "scroll blit handles a scroll that layout makes",
                 "[scroll_blit]") {
    lv_obj_scroll_to_y(list_, LV_COORD_MAX, LV_ANIM_OFF);
    render();
    REQUIRE(lv_obj_get_scroll_bottom(list_) == 0);
    const int32_t y_before = lv_obj_get_scroll_y(list_);

    for (int i = 0; i < 3; i++)
        lv_obj_delete(lv_obj_get_parent(labels_[labels_.size() - 1 - i]));
    labels_.resize(labels_.size() - 3);
    render();
    REQUIRE(lv_obj_get_scroll_y(list_) < y_before); // the layout did scroll it
    REQUIRE(mismatch() == "0 px differ");
    render();
    REQUIRE(mismatch() == "0 px differ");
}
