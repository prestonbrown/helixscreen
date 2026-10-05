// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_backdrop_refresh.cpp
 * @brief The overlay backdrop is a frozen bitmap, and it has to be re-takeable
 *
 * push_overlay() darkens an lv_snapshot_take() of the whole screen and parks it
 * in front of everything, so every pixel outside the overlay stops tracking the
 * widgets beneath it. The navigation bar is the part of that snapshot the user
 * can still see, so a setting that adds or removes a navbar element while its
 * own toggle sits inside an overlay changed nothing visible until the stack
 * popped — the widget un-hid instantly, the photo of it did not.
 *
 * refresh_overlay_backdrop() re-takes the shot. The contract that matters is
 * WHAT the new snapshot contains: the live base content, and none of the
 * overlays or the outgoing backdrop that are stacked on top of it by the time
 * the refresh runs. Get that wrong and the backdrop turns into a photograph of
 * itself.
 */

#include "ui_nav_manager.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/snapshot_backdrops_mode.h"
#include "backdrop_blur.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_buf_private.h" // handler fields: no public setters
#include "settings_manager.h"
#include "theme_manager.h"

#include <algorithm>
#include <unordered_set>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr lv_color_t BASE_COLOR = LV_COLOR_MAKE(0xFF, 0x00, 0x00);    // red
constexpr lv_color_t OVERLAY_COLOR = LV_COLOR_MAKE(0x00, 0xFF, 0x00); // green

/// Full-screen opaque child in a known flat color.
lv_obj_t* make_flat_layer(lv_obj_t* parent, lv_color_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    return obj;
}

/// Center pixel of the image a backdrop widget is displaying.
/// The backdrop's source is the lv_draw_buf_t built by create_darkened_backdrop.
struct Rgb {
    uint8_t r, g, b;
};

Rgb backdrop_pixel(lv_obj_t* backdrop, uint32_t x, uint32_t y) {
    REQUIRE(backdrop != nullptr);
    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(backdrop));
    REQUIRE(buf != nullptr);
    REQUIRE(buf->header.cf == LV_COLOR_FORMAT_ARGB8888);
    REQUIRE(x < buf->header.w);
    REQUIRE(y < buf->header.h);

    const auto* row = static_cast<const uint8_t*>(buf->data) + y * buf->header.stride;
    const uint8_t* px = row + x * 4; // BGRA byte order in LVGL's ARGB8888
    return Rgb{px[2], px[1], px[0]};
}

/// Tracks full-frame buffers alive through LVGL's default draw-buf handlers, and
/// can refuse them the way the K-Touch's exhausted PSRAM does.
class FrameBufferCounter {
  public:
    explicit FrameBufferCounter(bool refuse = false) : refuse_(refuse) {
        lv_display_t* disp = lv_display_get_default();
        // Half an ARGB8888 frame: catches RGB565 and ARGB8888 shots alike.
        frame_bytes_ = static_cast<size_t>(lv_display_get_horizontal_resolution(disp)) *
                       static_cast<size_t>(lv_display_get_vertical_resolution(disp)) * 2;
        auto* h = lv_draw_buf_get_handlers();
        orig_malloc_ = h->buf_malloc_cb;
        orig_free_ = h->buf_free_cb;
        h->buf_malloc_cb = &on_malloc;
        h->buf_free_cb = &on_free;
        s_self = this;
    }
    ~FrameBufferCounter() {
        auto* h = lv_draw_buf_get_handlers();
        h->buf_malloc_cb = orig_malloc_;
        h->buf_free_cb = orig_free_;
        s_self = nullptr;
    }
    FrameBufferCounter(const FrameBufferCounter&) = delete;
    FrameBufferCounter& operator=(const FrameBufferCounter&) = delete;

    /// Count a buffer allocated before the counter was installed.
    void track(const lv_draw_buf_t* buf) {
        live_.insert(buf->unaligned_data);
        peak_ = std::max(peak_, live_.size());
    }
    size_t peak() const {
        return peak_;
    }
    size_t attempts() const {
        return attempts_;
    }

  private:
    static void* on_malloc(size_t size, lv_color_format_t cf) {
        FrameBufferCounter& self = *s_self;
        const bool frame = size >= self.frame_bytes_;
        if (frame) {
            self.attempts_++;
            if (self.refuse_)
                return nullptr;
        }
        void* p = self.orig_malloc_(size, cf);
        if (p && frame) {
            self.live_.insert(p);
            self.peak_ = std::max(self.peak_, self.live_.size());
        }
        return p;
    }
    static void on_free(void* p) {
        s_self->live_.erase(p);
        s_self->orig_free_(p);
    }

    static inline FrameBufferCounter* s_self = nullptr;
    bool refuse_;
    size_t frame_bytes_ = 0;
    lv_draw_buf_malloc_cb_t orig_malloc_ = nullptr;
    lv_draw_buf_free_cb_t orig_free_ = nullptr;
    std::unordered_set<void*> live_;
    size_t peak_ = 0;
    size_t attempts_ = 0;
};

Rgb backdrop_center_pixel(lv_obj_t* backdrop) {
    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(backdrop));
    REQUIRE(buf != nullptr);
    return backdrop_pixel(backdrop, buf->header.w / 2, buf->header.h / 2);
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Backdrop refresh re-snapshots the live base content",
                 "[navigation][backdrop][refresh]") {
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();

    lv_obj_t* base = make_flat_layer(screen, BASE_COLOR);
    process_lvgl(20);

    // First overlay: snapshot the screen (all red), then hide the base and cover
    // it with a green overlay — exactly what push_overlay() does in that order.
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    lv_obj_add_flag(base, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* overlay = make_flat_layer(screen, OVERLAY_COLOR);
    lv_obj_move_foreground(overlay);
    NavigationManagerTestAccess::set_panel_stack(nav, {base, overlay});
    process_lvgl(20);

    Rgb before = backdrop_center_pixel(backdrop);
    CHECK(before.r > before.g); // the snapshot is of the red base

    SECTION("the replacement captures the base, not the overlay stacked over it") {
        // The base changes color while hidden, so only a fresh shot is blue.
        lv_obj_set_style_bg_color(base, LV_COLOR_MAKE(0x00, 0x00, 0xFF), LV_PART_MAIN);
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);

        lv_obj_t* refreshed = NavigationManagerTestAccess::overlay_backdrop(nav);
        REQUIRE(refreshed != nullptr);

        Rgb after = backdrop_center_pixel(refreshed);
        // Green here means the overlay was left visible during the snapshot and
        // the backdrop is now a photo of the thing it is supposed to sit behind.
        CHECK(after.g < after.b);
        // Red is the stale shot. Blue means the hidden base panel was un-hidden
        // for the new one: without that it is of an empty screen.
        CHECK(after.r < after.b);
        CHECK(after.b > 0x40);
    }

    SECTION("the overlay and base end up in the visibility state they started in") {
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);

        CHECK(lv_obj_has_flag(base, LV_OBJ_FLAG_HIDDEN));
        CHECK_FALSE(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
    }

    SECTION("the replacement stays below the overlay it backs") {
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);

        lv_obj_t* refreshed = NavigationManagerTestAccess::overlay_backdrop(nav);
        REQUIRE(refreshed != nullptr);
        CHECK(lv_obj_get_index(refreshed) < lv_obj_get_index(overlay));
    }

    NavigationManagerTestAccess::set_panel_stack(nav, {});
}

TEST_CASE_METHOD(LVGLTestFixture, "The backdrop snapshot excludes the arriving overlay",
                 "[navigation][backdrop]") {
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();

    lv_obj_t* base = make_flat_layer(screen, BASE_COLOR);
    // An overlay root left visible at push time, as a caller that skips the
    // create-hidden convention produces: the snapshot must not bake it in.
    lv_obj_t* arriving = make_flat_layer(screen, OVERLAY_COLOR);
    process_lvgl(20);

    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen, arriving);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    // Green here means the arriving overlay is its own backdrop ghost: a dimmed
    // copy sitting behind the live panel for as long as it is open.
    Rgb px = backdrop_center_pixel(backdrop);
    CHECK(px.g < px.r);
    // The arriving overlay leaves the snapshot in the visibility it started in.
    CHECK_FALSE(lv_obj_has_flag(arriving, LV_OBJ_FLAG_HIDDEN));

    NavigationManagerTestAccess::set_panel_stack(nav, {});
}

TEST_CASE_METHOD(LVGLTestFixture, "Backdrop refresh is a no-op with no backdrop live",
                 "[navigation][backdrop][refresh]") {
    auto& nav = NavigationManager::instance();
    REQUIRE(NavigationManagerTestAccess::overlay_backdrop(nav) == nullptr);

    NavigationManagerTestAccess::refresh_overlay_backdrop(nav);

    // No overlay is open, so there is nothing to re-photograph — refreshing must
    // not conjure a backdrop that would then dim the whole screen.
    CHECK(NavigationManagerTestAccess::overlay_backdrop(nav) == nullptr);
}

// The navbar beside an open overlay is the backdrop's picture of it, so a live
// mode switch from inside an overlay (Settings > Appearance) has to re-take it.
TEST_CASE_METHOD(LVGLUITestFixture, "a theme switch under an open overlay re-snapshots the navbar",
                 "[navigation][backdrop][theme]") {
    auto& nav = NavigationManager::instance();
    const helix::ThemeData theme = theme_manager_get_active_theme();
    const bool was_dark = theme_manager_is_dark_mode();
    lv_obj_t* saved_layout = NavigationManagerTestAccess::app_layout_widget(nav);
    theme_manager_apply_theme(theme, true);

    lv_obj_t* layout = lv_obj_create(test_screen());
    lv_obj_remove_style_all(layout);
    lv_obj_set_size(layout, LV_PCT(100), LV_PCT(100));
    lv_obj_t* navbar = static_cast<lv_obj_t*>(lv_xml_create(layout, "navigation_bar", nullptr));
    REQUIRE(navbar != nullptr);
    nav.set_app_layout(layout);
    nav.wire_events(navbar);
    lv_obj_update_layout(test_screen());
    process_lvgl(20);

    lv_area_t a;
    lv_obj_get_coords(navbar, &a);
    const auto x = static_cast<uint32_t>(a.x1 + 2);
    const auto y = static_cast<uint32_t>((a.y1 + a.y2) / 2);

    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, test_screen());
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);
    const Rgb dark_px = backdrop_pixel(backdrop, x, y);

    theme_manager_apply_theme(theme, false);
    process_lvgl(20);

    lv_obj_t* after = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(after != nullptr);
    const Rgb light_px = backdrop_pixel(after, x, y);
    INFO("navbar in backdrop: dark " << int(dark_px.r) << "," << int(dark_px.g) << ","
                                     << int(dark_px.b) << " light " << int(light_px.r) << ","
                                     << int(light_px.g) << "," << int(light_px.b));
    CHECK(light_px.r + light_px.g + light_px.b > dark_px.r + dark_px.g + dark_px.b + 150);

    NavigationManagerTestAccess::set_panel_stack(nav, {});
    nav.set_app_layout(saved_layout);
    theme_manager_apply_theme(theme, was_dark);
}

// ============================================================================
// Memory: one full frame at most, none where snapshots are off
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a backdrop refresh never holds two full frames",
                 "[navigation][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(true);
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();
    lv_obj_t* base = make_flat_layer(screen, BASE_COLOR);
    process_lvgl(20);

    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);
    REQUIRE(lv_obj_check_type(backdrop, &lv_image_class));
    NavigationManagerTestAccess::set_panel_stack(nav, {base});

    {
        FrameBufferCounter frames;
        frames.track(static_cast<const lv_draw_buf_t*>(lv_image_get_src(backdrop)));
        NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
        process_lvgl(20);
        // 2 is 768KB extra on an 800x480 RGB565 panel, the allocation that
        // fails on an 8MB-PSRAM ESP32 with a print open.
        CHECK(frames.peak() == 1);
    }

    NavigationManagerTestAccess::set_panel_stack(nav, {});
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "re-applying the same theme under an overlay keeps the snapshot",
                 "[navigation][backdrop][theme]") {
    SnapshotBackdropsMode snapshots(true);
    auto& nav = NavigationManager::instance();
    const helix::ThemeData theme = theme_manager_get_active_theme();
    const bool dark = theme_manager_is_dark_mode();
    lv_obj_t* saved_layout = NavigationManagerTestAccess::app_layout_widget(nav);

    lv_obj_t* layout = lv_obj_create(test_screen());
    lv_obj_remove_style_all(layout);
    lv_obj_set_size(layout, LV_PCT(100), LV_PCT(100));
    lv_obj_t* navbar = static_cast<lv_obj_t*>(lv_xml_create(layout, "navigation_bar", nullptr));
    REQUIRE(navbar != nullptr);
    nav.set_app_layout(layout);
    nav.wire_events(navbar);
    process_lvgl(20);

    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, test_screen());
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    // A pixel no snapshot of this screen produces: a re-take overwrites it.
    auto* buf = static_cast<lv_draw_buf_t*>(const_cast<void*>(lv_image_get_src(backdrop)));
    REQUIRE(buf != nullptr);
    REQUIRE(buf->header.cf == LV_COLOR_FORMAT_ARGB8888);
    buf->data[0] = 0x12;
    buf->data[1] = 0x34;
    buf->data[2] = 0x56;

    theme_manager_apply_theme(theme, dark);
    process_lvgl(20);

    lv_obj_t* after = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(after != nullptr);
    const Rgb px = backdrop_pixel(after, 0, 0);
    CHECK(px.r == 0x56);
    CHECK(px.g == 0x34);
    CHECK(px.b == 0x12);

    NavigationManagerTestAccess::set_panel_stack(nav, {});
    nav.set_app_layout(saved_layout);
}

TEST_CASE_METHOD(LVGLTestFixture, "with snapshots off the backdrop is a dim layer and no frame",
                 "[navigation][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(false);
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();
    lv_obj_t* base = make_flat_layer(screen, BASE_COLOR);
    process_lvgl(20);

    FrameBufferCounter frames;
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);
    NavigationManagerTestAccess::set_panel_stack(nav, {base});

    CHECK_FALSE(lv_obj_check_type(backdrop, &lv_image_class));
    CHECK(lv_obj_get_style_bg_opa(backdrop, LV_PART_MAIN) == 40);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(backdrop, LV_PART_MAIN), lv_color_black()));
    CHECK(lv_obj_has_flag(backdrop, LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_index(backdrop) > lv_obj_get_index(base));

    // Translucent over the live navbar, so there is nothing to re-take.
    NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
    process_lvgl(20);
    CHECK(NavigationManagerTestAccess::overlay_backdrop(nav) == backdrop);
    CHECK(frames.attempts() == 0);

    NavigationManagerTestAccess::set_panel_stack(nav, {});
}

TEST_CASE_METHOD(LVGLTestFixture, "a snapshot that cannot allocate falls back once and for good",
                 "[navigation][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(true);
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();
    process_lvgl(20);

    FrameBufferCounter frames(/*refuse=*/true);
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);
    CHECK_FALSE(lv_obj_check_type(backdrop, &lv_image_class));
    CHECK(frames.attempts() == 1);

    lv_obj_t* next = helix::ui::create_darkened_backdrop(screen, 40);
    REQUIRE(next != nullptr);
    CHECK_FALSE(lv_obj_check_type(next, &lv_image_class));
    CHECK(frames.attempts() == 1);
    lv_obj_delete(next);
}
