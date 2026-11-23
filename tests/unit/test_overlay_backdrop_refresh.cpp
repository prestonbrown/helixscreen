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

#include "ui_emergency_stop.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/snapshot_backdrops_mode.h"
#include "backdrop_blur.h"
#include "bed_drying_controller.h"
#include "core/update_queue.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_buf_private.h" // handler fields: no public setters
#include "panel_widget_manager.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr lv_color_t kBaseColor = LV_COLOR_MAKE(0xFF, 0x00, 0x00);    // red
constexpr lv_color_t kOverlayColor = LV_COLOR_MAKE(0x00, 0xFF, 0x00); // green

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

    lv_obj_t* base = make_flat_layer(screen, kBaseColor);
    process_lvgl(20);

    // First overlay: snapshot the screen (all red), then hide the base and cover
    // it with a green overlay — exactly what push_overlay() does in that order.
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    lv_obj_set_hidden(base, true);
    lv_obj_t* overlay = make_flat_layer(screen, kOverlayColor);
    lv_obj_move_to_index(overlay, -1);
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

        CHECK(lv_obj_is_hidden(base));
        CHECK_FALSE(lv_obj_is_hidden(overlay));
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

TEST_CASE_METHOD(LVGLTestFixture,
                 "The backdrop snapshot excludes the arriving overlay and a closing dialog",
                 "[navigation][backdrop]") {
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();

    lv_obj_t* base = make_flat_layer(screen, kBaseColor);
    // A confirm button that opens an overlay runs before its dialog finishes
    // closing, so the dialog is still on screen when the backdrop is taken.
    lv_obj_t* dialog_backdrop = make_flat_layer(screen, LV_COLOR_MAKE(0x00, 0x00, 0xFF));
    ModalStack::instance().push(dialog_backdrop, lv_obj_create(dialog_backdrop), "closing");
    // An overlay root left visible at push time, as a caller that skips the
    // create-hidden convention produces: the snapshot must not bake it in.
    lv_obj_t* arriving = make_flat_layer(screen, kOverlayColor);
    process_lvgl(20);

    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen, arriving);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);

    // Green here means the arriving overlay is its own backdrop ghost: a dimmed
    // copy sitting behind the live panel for as long as it is open. Blue is the
    // dialog, kept beside the overlay long after it closed.
    Rgb px = backdrop_center_pixel(backdrop);
    CHECK(px.g < px.r);
    CHECK(px.b < px.r);
    // Both leave the snapshot in the visibility they started in.
    CHECK_FALSE(lv_obj_is_hidden(arriving));
    CHECK_FALSE(lv_obj_is_hidden(dialog_backdrop));
    ModalStack::instance().remove(dialog_backdrop, true);

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

// The E-stop and bed-drying slots appear in the navbar on their own, often
// while an overlay is open (Print Status as a print starts). The live navbar
// re-flows its buttons to make room, so the backdrop's picture of it has to
// follow or the old button spacing shows under the live E-stop.
TEST_CASE_METHOD(LVGLUITestFixture, "a navbar slot appearing under an open overlay re-snapshots it",
                 "[navigation][backdrop][estop_navbar]") {
    const bool drying_slot = GENERATE(false, true);
    CAPTURE(drying_slot);
    SnapshotBackdropsMode snapshots(true);
    auto& nav = NavigationManager::instance();
    lv_obj_t* saved_layout = NavigationManagerTestAccess::app_layout_widget(nav);

    // The navbar binds the drying slot to the controller's subject, so the
    // controller has to be up before the navbar is built.
    std::unique_ptr<BedDryingController> drying;
    lv_subject_t* subject = EmergencyStopOverlay::instance().get_visible_subject();
    if (drying_slot) {
        drying =
            std::make_unique<BedDryingController>(state(), nullptr, nullptr, [] { return 0LL; });
        drying->init_subjects();
        PanelWidgetManager::instance().register_shared_resource<BedDryingController>(drying.get());
        subject = drying->get_state_subject();
    }
    const int32_t saved_value = lv_subject_get_int(subject);
    lv_subject_set_int(subject, 0);

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
    // The navbar's whole column in the backdrop image, as one string of pixels.
    auto navbar_column = [&]() {
        lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
        REQUIRE(backdrop != nullptr);
        std::string pixels;
        for (int32_t y = a.y1; y <= a.y2; ++y) {
            for (int32_t x = a.x1; x <= a.x2; ++x) {
                const Rgb px =
                    backdrop_pixel(backdrop, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                pixels.append(
                    {static_cast<char>(px.r), static_cast<char>(px.g), static_cast<char>(px.b)});
            }
        }
        return pixels;
    };

    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, test_screen());
    const std::string without_slot = navbar_column();

    lv_subject_set_int(subject, 1);
    process_lvgl(20);
    const std::string shown = navbar_column();

    NavigationManagerTestAccess::refresh_overlay_backdrop(nav);
    process_lvgl(20);
    const std::string fresh = navbar_column();

    // The slot re-flows the buttons, so a fresh picture differs from the old one.
    REQUIRE(fresh != without_slot);
    // And the backdrop already showed it, without anyone asking for a refresh.
    CHECK(shown == fresh);

    lv_subject_set_int(subject, saved_value);
    NavigationManagerTestAccess::set_panel_stack(nav, {});
    nav.set_app_layout(saved_layout);
    if (drying) {
        PanelWidgetManager::instance().unregister_shared_resource<BedDryingController>();
    }
}

// ============================================================================
// Memory: one full frame at most, none where snapshots are off
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "a backdrop refresh never holds two full frames",
                 "[navigation][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(true);
    auto& nav = NavigationManager::instance();
    lv_obj_t* screen = test_screen();
    lv_obj_t* base = make_flat_layer(screen, kBaseColor);
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
    lv_obj_t* base = make_flat_layer(screen, kBaseColor);
    process_lvgl(20);

    FrameBufferCounter frames;
    NavigationManagerTestAccess::adopt_overlay_backdrop(nav, screen);
    lv_obj_t* backdrop = NavigationManagerTestAccess::overlay_backdrop(nav);
    REQUIRE(backdrop != nullptr);
    NavigationManagerTestAccess::set_panel_stack(nav, {base});

    CHECK_FALSE(lv_obj_check_type(backdrop, &lv_image_class));
    CHECK(lv_obj_get_style_bg_opa(backdrop, LV_PART_MAIN) == 40);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(backdrop, LV_PART_MAIN), lv_color_black()));
    CHECK(lv_obj_is_clickable(backdrop));
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

namespace {
/// The plainest modal: the shared dialog component, every hook the default.
class PlainModal : public Modal {
  public:
    const char* get_name() const override {
        return "Plain";
    }
    const char* component_name() const override {
        return "modal_dialog";
    }
};
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "a modal opening as another closes leaves it out of its backdrop",
                 "[modal][backdrop]") {
    SnapshotBackdropsMode snapshots(true);
    lv_obj_t* screen = test_screen();
    make_flat_layer(screen, kBaseColor);
    // A dialog that one tap closes and replaces: it is still fading out when the
    // next one snapshots the screen.
    lv_obj_t* closing = make_flat_layer(screen, LV_COLOR_MAKE(0x00, 0x00, 0xFF));
    ModalStack::instance().push(closing, lv_obj_create(closing), "closing");
    REQUIRE(ModalStack::instance().mark_exiting(closing));
    process_lvgl(20);

    PlainModal next;
    REQUIRE(next.show(screen));
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(ModalStack::instance().top_dialog());
    REQUIRE(backdrop != nullptr);
    // Blue is the closing dialog frozen into the new one's backdrop for as long
    // as it stays open.
    const Rgb px = backdrop_center_pixel(backdrop);
    CHECK(px.b < px.r);
    CHECK_FALSE(lv_obj_is_hidden(closing));

    next.hide();
    ModalStack::instance().remove(closing, true);
    process_lvgl(400);
}

// ============================================================================
// A modal over a screen that changes beneath it
// ============================================================================
//
// A modal's backdrop is a picture of the screen taken when it opened. A dialog
// that stays up while navigation changes the screen beneath it (a stacked
// dialog left open while another dialog's button opens a panel) has to have
// that picture re-taken, or the user sees the old panel frozen behind it.

namespace {

constexpr lv_color_t kGreen = LV_COLOR_MAKE(0x00, 0xFF, 0x00);

/// The pixel of @p backdrop's image that sits at screen point (@p sx, @p sy).
/// A blurred backdrop is a half-size image stretched over the screen.
Rgb backdrop_pixel_at_screen(lv_obj_t* backdrop, int32_t sx, int32_t sy) {
    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(backdrop));
    REQUIRE(buf != nullptr);
    lv_display_t* disp = lv_display_get_default();
    const int32_t w = lv_display_get_horizontal_resolution(disp);
    const int32_t h = lv_display_get_vertical_resolution(disp);
    return backdrop_pixel(backdrop, static_cast<uint32_t>(sx * buf->header.w / w),
                          static_cast<uint32_t>(sy * buf->header.h / h));
}

/// A point near the top-left corner, clear of any centered dialog.
Rgb backdrop_corner_pixel(lv_obj_t* backdrop) {
    return backdrop_pixel_at_screen(backdrop, 12, 12);
}

/// Home is a red panel and Controls a green one, Home showing.
class ModalUnderNavFixture : public LVGLUITestFixture {
  public:
    ModalUnderNavFixture() {
        helix::ui::destroy_static_panels();
        home_ = make_flat_layer(test_screen(), kBaseColor);
        controls_ = make_flat_layer(test_screen(), kGreen);
        lv_obj_t* panels[kUiPanelCount] = {nullptr};
        panels[static_cast<int>(PanelId::Home)] = home_;
        panels[static_cast<int>(PanelId::Controls)] = controls_;
        auto& nav = NavigationManager::instance();
        nav.set_panels(panels);
        NavigationManagerTestAccess::switch_to_panel(nav, PanelId::Home);
        settle();
    }

    ~ModalUnderNavFixture() override {
        ModalStack::instance().clear();
        NavigationManagerTestAccess::set_panel_stack(NavigationManager::instance(), {});
        helix::ui::destroy_static_panels();
        settle();
    }

    /// Long enough for queued navigation, its animations and the re-take after.
    void settle() {
        for (int i = 0; i < 6; i++) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(100);
        }
    }

    static lv_obj_t* backdrop_of_top() {
        return ModalStack::instance().backdrop_for(ModalStack::instance().top_dialog());
    }

    lv_obj_t* home_ = nullptr;
    lv_obj_t* controls_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(ModalUnderNavFixture, "a panel switch beneath an open modal re-takes its backdrop",
                 "[modal][backdrop][navigation]") {
    SnapshotBackdropsMode snapshots(true);
    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    REQUIRE(lv_obj_check_type(backdrop, &lv_image_class));
    const Rgb before = backdrop_corner_pixel(backdrop);
    REQUIRE(before.r > before.g);

    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Controls);
    settle();

    REQUIRE(backdrop_of_top() == backdrop);
    // Red is Home frozen behind the dialog after the user went to Controls.
    const Rgb after = backdrop_corner_pixel(backdrop);
    INFO("after " << int(after.r) << "," << int(after.g) << "," << int(after.b));
    CHECK(after.g > after.r);
    CHECK(after.g > 0x40);
    // The capture hid the modal for its shot and showed it again.
    CHECK_FALSE(lv_obj_is_hidden(backdrop));
    modal.hide();
}

TEST_CASE_METHOD(ModalUnderNavFixture,
                 "an overlay pushed and popped beneath an open modal re-takes its backdrop",
                 "[modal][backdrop][navigation]") {
    SnapshotBackdropsMode snapshots(true);
    auto& nav = NavigationManager::instance();
    lv_obj_t* overlay = make_flat_layer(test_screen(), kGreen);
    lv_obj_set_hidden(overlay, true);
    nav.register_overlay_instance(overlay, nullptr);

    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);

    nav.push_overlay(overlay);
    settle();

    lv_area_t a;
    lv_obj_get_coords(overlay, &a);
    const int32_t x = (a.x1 + a.x2) / 2;
    const int32_t y = a.y1 + 12;
    const Rgb pushed = backdrop_pixel_at_screen(backdrop, x, y);
    INFO("pushed " << int(pushed.r) << "," << int(pushed.g) << "," << int(pushed.b));
    CHECK(pushed.g > pushed.r);

    nav.go_back();
    settle();
    const Rgb popped = backdrop_pixel_at_screen(backdrop, x, y);
    INFO("popped " << int(popped.r) << "," << int(popped.g) << "," << int(popped.b));
    CHECK(popped.r > popped.g);

    modal.hide();
    nav.unregister_overlay_instance(overlay);
}

TEST_CASE_METHOD(ModalUnderNavFixture,
                 "stacked modals re-take lowest first, each over what is beneath it",
                 "[modal][backdrop][navigation]") {
    SnapshotBackdropsMode snapshots(true);
    PlainModal lower;
    REQUIRE(lower.show(test_screen()));
    lv_obj_t* lower_backdrop = backdrop_of_top();
    PlainModal upper;
    REQUIRE(upper.show(test_screen()));
    lv_obj_t* upper_backdrop = backdrop_of_top();
    REQUIRE(lower_backdrop != nullptr);
    REQUIRE(upper_backdrop != nullptr);
    REQUIRE(lower_backdrop != upper_backdrop);

    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Controls);
    settle();

    const Rgb low = backdrop_corner_pixel(lower_backdrop);
    INFO("lower " << int(low.r) << "," << int(low.g) << "," << int(low.b));
    CHECK(low.g > low.r);
    // The upper one's corner is the lower backdrop's: red there means it was
    // re-taken over the lower one's stale picture.
    const Rgb up = backdrop_corner_pixel(upper_backdrop);
    INFO("upper " << int(up.r) << "," << int(up.g) << "," << int(up.b));
    CHECK(up.g > up.r);
    // Darker than the lower one: it shows the lower backdrop dimmed again, not
    // the bare panel, so the lower modal was in its shot.
    CHECK(up.g < low.g);
    CHECK_FALSE(lv_obj_is_hidden(lower_backdrop));
    CHECK_FALSE(lv_obj_is_hidden(upper_backdrop));

    upper.hide();
    lower.hide();
}

TEST_CASE_METHOD(ModalUnderNavFixture, "a modal backdrop re-take never holds two full frames",
                 "[modal][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(true);
    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    const void* buf_before = lv_image_get_src(backdrop);

    {
        // The blurred picture it keeps is a quarter frame; the re-take's own
        // full-size shot is the one frame it may hold.
        FrameBufferCounter frames;
        ModalStack::instance().refresh_backdrops();
        CHECK(frames.attempts() >= 1); // it did take a new picture
        CHECK(frames.peak() <= 1);
    }
    // Into the buffer it already had: no second image held for the modal.
    CHECK(lv_image_get_src(backdrop) == buf_before);
    modal.hide();
}

TEST_CASE_METHOD(ModalUnderNavFixture, "a dim-layer modal backdrop has nothing to re-take",
                 "[modal][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(false);
    helix::ui::backdrop_blur_cleanup(); // blur off, as after its circuit breaker trips
    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    REQUIRE_FALSE(lv_obj_check_type(backdrop, &lv_image_class));

    FrameBufferCounter frames;
    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Controls);
    settle();
    CHECK(frames.attempts() == 0);
    CHECK(backdrop_of_top() == backdrop);
    modal.hide();
    helix::ui::detail::reset_circuit_breaker();
}

TEST_CASE_METHOD(LVGLUITestFixture, "a theme switch under an open modal re-takes its backdrop",
                 "[modal][backdrop][theme]") {
    SnapshotBackdropsMode snapshots(true);
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
    const int32_t x = a.x1 + 4;
    const int32_t y = (a.y1 + a.y2) / 2;

    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = ModalStack::instance().backdrop_for(ModalStack::instance().top_dialog());
    REQUIRE(backdrop != nullptr);
    REQUIRE(lv_obj_check_type(backdrop, &lv_image_class));
    const Rgb dark_px = backdrop_pixel_at_screen(backdrop, x, y);

    theme_manager_apply_theme(theme, false);
    for (int i = 0; i < 4; i++) {
        helix::ui::UpdateQueue::instance().drain();
        process_lvgl(100);
    }

    // The dark navbar left behind the dialog after the user picked light mode.
    const Rgb light_px = backdrop_pixel_at_screen(backdrop, x, y);
    INFO("navbar in modal backdrop: dark " << int(dark_px.r) << "," << int(dark_px.g) << ","
                                           << int(dark_px.b) << " light " << int(light_px.r) << ","
                                           << int(light_px.g) << "," << int(light_px.b));
    CHECK(light_px.r + light_px.g + light_px.b > dark_px.r + dark_px.g + dark_px.b + 100);

    modal.hide();
    process_lvgl(400);
    NavigationManagerTestAccess::set_panel_stack(nav, {});
    nav.set_app_layout(saved_layout);
    theme_manager_apply_theme(theme, was_dark);
}

TEST_CASE_METHOD(ModalUnderNavFixture, "a modal backdrop re-take keeps keyboard focus where it was",
                 "[modal][backdrop][navigation]") {
    SnapshotBackdropsMode snapshots(true);
    lv_group_t* saved_group = lv_group_get_default();
    lv_group_t* group = lv_group_create();
    lv_group_set_default(group);
    // A focusable widget on the panel behind: where focus lands if hiding the
    // backdrop for its shot moves it on.
    lv_obj_t* behind = lv_button_create(controls_);
    lv_group_add_obj(group, behind);

    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    lv_group_focus_obj(backdrop);
    REQUIRE(lv_group_get_focused(group) == backdrop);

    NavigationManagerTestAccess::switch_to_panel(NavigationManager::instance(), PanelId::Controls);
    settle();

    // Focus on the panel button sends ESC and Enter to the panel behind the dialog.
    CHECK(lv_group_get_focused(group) == backdrop);

    modal.hide();
    settle();
    lv_group_set_default(saved_group);
    lv_group_delete(group);
}

TEST_CASE_METHOD(ModalUnderNavFixture,
                 "a blurred re-take that cannot allocate keeps its pixels and blur on",
                 "[modal][backdrop][memory]") {
    SnapshotBackdropsMode snapshots(true);
    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    REQUIRE(lv_obj_check_type(backdrop, &lv_image_class));
    const void* buf_before = lv_image_get_src(backdrop);

    lv_obj_set_hidden(home_, true);
    lv_obj_set_hidden(controls_, false);
    {
        FrameBufferCounter frames(/*refuse=*/true);
        ModalStack::instance().refresh_backdrops();
        CHECK(frames.attempts() == 1);
    }
    // A full frame that did not fit this once leaves blur on for the next modal.
    CHECK_FALSE(helix::ui::detail::is_blur_disabled());
    CHECK(lv_image_get_src(backdrop) == buf_before);
    const Rgb px = backdrop_corner_pixel(backdrop);
    CHECK(px.r > px.g); // the old picture, not a half-written one

    modal.hide();
    helix::ui::detail::reset_circuit_breaker();
}

TEST_CASE_METHOD(ModalUnderNavFixture, "clear() cancels a scheduled modal backdrop re-take",
                 "[modal][backdrop][navigation]") {
    SnapshotBackdropsMode snapshots(true);
    PlainModal first;
    REQUIRE(first.show(test_screen()));
    ModalStack::instance().schedule_backdrop_refresh(250);
    ModalStack::instance().clear();

    // A modal shown after the teardown, over a screen that then changes.
    PlainModal next;
    REQUIRE(next.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    lv_obj_set_hidden(home_, true);
    lv_obj_set_hidden(controls_, false);
    process_lvgl(400);

    // Green is the re-take scheduled before the teardown firing anyway.
    const Rgb px = backdrop_corner_pixel(backdrop);
    CHECK(px.r > px.g);
    next.hide();
}

TEST_CASE_METHOD(ModalUnderNavFixture,
                 "back-to-back triggers fold into one re-take timed from the latest",
                 "[modal][backdrop][navigation]") {
    SnapshotBackdropsMode snapshots(true);
    PlainModal modal;
    REQUIRE(modal.show(test_screen()));
    lv_obj_t* backdrop = backdrop_of_top();
    REQUIRE(backdrop != nullptr);
    lv_obj_set_hidden(home_, true);
    lv_obj_set_hidden(controls_, false);

    FrameBufferCounter frames;
    ModalStack::instance().schedule_backdrop_refresh(250);
    process_lvgl(200);
    ModalStack::instance().schedule_backdrop_refresh(250);
    process_lvgl(100);
    // 300ms after the first trigger, 100ms after the second: not yet.
    const Rgb early = backdrop_corner_pixel(backdrop);
    CHECK(early.r > early.g);
    CHECK(frames.attempts() == 0);

    process_lvgl(200);
    const Rgb late = backdrop_corner_pixel(backdrop);
    CHECK(late.g > late.r);
    CHECK(frames.attempts() == 1);
    modal.hide();
}
