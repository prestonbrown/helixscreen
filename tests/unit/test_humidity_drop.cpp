// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_humidity_drop.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_fixtures.h"
#include "core/subject.h"
#include "display_settings_manager.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "humidity_drop_geometry.h"
#include "humidity_sensor_manager.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui::humidity_drop;

namespace {

struct Pixel {
    uint8_t b, g, r, a;
};

struct Canvas {
    int w, h;
    std::vector<uint8_t> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_ * h_ * 4), 0xAB) {}
    Pixel at(int x, int y) const {
        const uint8_t* p = &px[static_cast<size_t>((y * w + x) * 4)];
        return {p[0], p[1], p[2], p[3]};
    }
};

// Distinct, saturated colours so a pixel's source is readable from its value.
const DropColors kColors{{255, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 255, 255}};

Canvas render(int w, int h, int humidity_x10, float phase = 0.0f) {
    Canvas c(w, h);
    rasterize(c.px.data(), w, h, w * 4, kColors, humidity_x10, phase);
    return c;
}

// A pixel whose dominant channel is green or blue: painted by a wave, not the outline.
bool is_fill(const Pixel& p) {
    return p.a > 128 && (p.g > p.r + 40 || p.b > p.r + 40);
}

int count_fill(const Canvas& c) {
    int n = 0;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x)
            n += is_fill(c.at(x, y)) ? 1 : 0;
    return n;
}

} // namespace

// ============================================================================
// Shape
// ============================================================================

TEST_CASE("humidity_drop: the drop fits its box, centred", "[humidity_drop]") {
    for (int size : {16, 24, 48, 128}) {
        const DropShape s = fit_drop(size, size);
        CAPTURE(size);
        CHECK(s.cx == Catch::Approx(size / 2.0f));
        CHECK(s.tip_y > 0.0f);
        CHECK(s.bottom() < size);
        CHECK(s.cx - s.r > 0.0f);
        CHECK(s.cx + s.r < size);
        // Centred vertically: the air above the tip matches the air below the bottom.
        CHECK(s.tip_y == Catch::Approx(size - s.bottom()).margin(0.01));
        // Taller than wide, as the glyph is.
        CHECK(s.bottom() - s.tip_y > 2.0f * s.r);
    }
}

TEST_CASE("humidity_drop: a narrow box limits the drop by its width", "[humidity_drop]") {
    const DropShape tall = fit_drop(10, 100);
    CHECK(2.0f * tall.r < 10.0f);
    CHECK(tall.bottom() - tall.tip_y < 100.0f * 0.5f);
}

TEST_CASE("humidity_drop: half width runs from the tip to the round bottom", "[humidity_drop]") {
    const DropShape s = fit_drop(100, 100);
    CHECK(half_width(s, s.tip_y - 1.0f) == 0.0f);
    CHECK(half_width(s, s.bottom() + 1.0f) == 0.0f);
    CHECK(half_width(s, s.tip_y) == Catch::Approx(0.0f).margin(0.05));
    CHECK(half_width(s, s.cy) == Catch::Approx(s.r));
    CHECK(half_width(s, s.cy + s.r * 0.6f) == Catch::Approx(s.r * 0.8f).margin(0.01));
    // The upper part widens all the way down to the equator.
    float prev = 0.0f;
    for (float y = s.tip_y + 0.5f; y < s.cy; y += 0.5f) {
        const float hw = half_width(s, y);
        CHECK(hw >= prev);
        prev = hw;
    }
}

TEST_CASE("humidity_drop: the sides pinch in on the way up to the tip", "[humidity_drop]") {
    const DropShape s = fit_drop(100, 100);
    // Between the row halfway up the point and the tip, the edge runs inside the
    // straight chord joining them: concave, a teardrop rather than a cone.
    const float mid_y = (s.tip_y + s.cy) / 2.0f;
    const float mid_hw = half_width(s, mid_y);
    for (float f = 0.2f; f <= 0.8f; f += 0.1f) {
        const float y = mid_y + (s.tip_y - mid_y) * f;
        CHECK(half_width(s, y) < mid_hw * (1.0f - f));
    }
    // Still a point: just below the tip the sides already stand apart.
    CHECK(half_width(s, s.tip_y + 2.0f) > 0.2f);
}

TEST_CASE("humidity_drop: signed distance is negative inside, positive outside",
          "[humidity_drop]") {
    const DropShape s = fit_drop(100, 100);
    CHECK(signed_distance(s, s.cx, s.cy) == Catch::Approx(-s.r).margin(0.01));
    CHECK(signed_distance(s, s.cx, s.bottom() + 3.0f) == Catch::Approx(3.0f).margin(0.01));
    CHECK(signed_distance(s, s.cx, s.tip_y - 2.0f) == Catch::Approx(2.0f).margin(0.01));
    CHECK(signed_distance(s, s.cx + s.r + 4.0f, s.cy) == Catch::Approx(4.0f).margin(0.01));
    // On the curved upper side, distance tracks the edge, not just the row's half width.
    const float y = (s.tip_y + s.cy) / 2.0f;
    const float edge = s.cx + half_width(s, y);
    CHECK(signed_distance(s, edge, y) == Catch::Approx(0.0f).margin(0.01));
    CHECK(signed_distance(s, edge - 3.0f, y) < 0.0f);
    CHECK(signed_distance(s, edge - 3.0f, y) > -3.0f);
    CHECK(signed_distance(s, edge + 3.0f, y) > 0.0f);
}

TEST_CASE("humidity_drop: stroke and wave scale with the drop, with floors", "[humidity_drop]") {
    const DropShape icon = fit_drop(16, 16);
    const DropShape big = fit_drop(160, 160);
    CHECK(stroke_width(icon) >= 1.0f);
    CHECK(wave_amplitude(icon) >= 0.75f);
    CHECK(stroke_width(big) > stroke_width(icon));
    CHECK(wave_amplitude(big) > wave_amplitude(icon));
    CHECK(stroke_width(big) <= 3.5f);
}

// ============================================================================
// Level
// ============================================================================

TEST_CASE("humidity_drop: fill fraction clamps and flags unknown", "[humidity_drop]") {
    CHECK(fill_fraction(0) == 0.0f);
    CHECK(fill_fraction(500) == Catch::Approx(0.5f));
    CHECK(fill_fraction(150) == Catch::Approx(0.15f));
    CHECK(fill_fraction(1000) == 1.0f);
    CHECK(fill_fraction(1500) == 1.0f);
    CHECK(fill_fraction(kUnknown) < 0.0f);
    CHECK(fill_fraction(-250) < 0.0f);
}

TEST_CASE("humidity_drop: the water line sits at the reading's share of the height",
          "[humidity_drop]") {
    const DropShape s = fit_drop(64, 64);
    const float height = s.bottom() - s.tip_y;
    CHECK(surface_level(s, 0.0f) == Catch::Approx(s.bottom()));
    CHECK(surface_level(s, 0.25f) == Catch::Approx(s.bottom() - 0.25f * height));
    CHECK(surface_level(s, 0.5f) == Catch::Approx(s.bottom() - 0.5f * height));
    CHECK(surface_level(s, 0.75f) == Catch::Approx(s.bottom() - 0.75f * height));
    CHECK(surface_level(s, 1.0f) == Catch::Approx(s.tip_y));
    // Flat at both ends: 0% shows no water above the floor, 100% no gap at the tip.
    CHECK(amplitude_at(s, 0.0f) == 0.0f);
    CHECK(amplitude_at(s, 1.0f) == 0.0f);
}

TEST_CASE("humidity_drop: the wave stays within its amplitude and repeats each drop width",
          "[humidity_drop]") {
    const DropShape s = fit_drop(64, 64);
    const float level = 30.0f;
    const float a = 2.0f;
    for (float x = 0; x < 64; x += 0.25f) {
        const float y = wave_y(s, level, a, 0.3f, x);
        CHECK(y >= level - a - 1e-4f);
        CHECK(y <= level + a + 1e-4f);
        CHECK(wave_y(s, level, a, 0.3f, x + 2.0f * s.r) == Catch::Approx(y).margin(1e-3));
        // Seamless as the phase wraps.
        CHECK(wave_y(s, level, a, 0.3f + 6.2831853f, x) == Catch::Approx(y).margin(1e-3));
    }
    // The phase moves it: some column reads differently a quarter turn later.
    CHECK(wave_y(s, level, a, 0.0f, s.cx) != Catch::Approx(wave_y(s, level, a, 1.57f, s.cx)));
    CHECK(back_phase(0.0f) != Catch::Approx(0.0f));
    CHECK(back_phase(1.0f) - back_phase(0.0f) < 0.0f);
    // The back wave wraps with the front, with no jump.
    CHECK(wave_y(s, level, a, back_phase(6.2831853f), s.cx) ==
          Catch::Approx(wave_y(s, level, a, back_phase(0.0f), s.cx)).margin(1e-3));
}

TEST_CASE("humidity_drop: the swell is full mid-drop and calms at both ends", "[humidity_drop]") {
    CHECK(fill_height(0.0f) == 0.0f);
    CHECK(fill_height(0.5f) == Catch::Approx(0.5f));
    CHECK(fill_height(1.5f) == 1.0f);
    CHECK(fill_height(1.0f) == 1.0f);
    const DropShape s = fit_drop(128, 128);
    // A full swell through the middle, gone by the time the drop is full.
    CHECK(amplitude_at(s, 0.5f) == Catch::Approx(wave_amplitude(s)));
    CHECK(amplitude_at(s, 0.85f) < amplitude_at(s, 0.5f));
    CHECK(amplitude_at(s, 0.85f) > 0.0f);
    CHECK(amplitude_at(s, 0.05f) < amplitude_at(s, 0.5f));
}

TEST_CASE("humidity_drop: a large drop swells; an icon keeps a readable level", "[humidity_drop]") {
    const DropShape big = fit_drop(160, 160);
    const DropShape icon = fit_drop(24, 24);
    CHECK(wave_amplitude(big) / big.r > 0.15f);
    CHECK(wave_amplitude(icon) / icon.r < 0.15f);
    // Two crests across the drop's width, one clearly higher than the other.
    std::vector<float> crest_y;
    int crests = 0;
    for (float x = big.cx - big.r; x < big.cx + big.r; x += 0.5f) {
        const float y0 = wave_y(big, 50.0f, 4.0f, 0.0f, x - 0.5f);
        const float y1 = wave_y(big, 50.0f, 4.0f, 0.0f, x);
        const float y2 = wave_y(big, 50.0f, 4.0f, 0.0f, x + 0.5f);
        if (y1 < y0 && y1 <= y2) {
            ++crests;
            crest_y.push_back(y1);
        }
    }
    CHECK(crests == 2);
    REQUIRE(crest_y.size() == 2);
    CHECK(std::fabs(crest_y[0] - crest_y[1]) > 0.25f * 4.0f);
}

namespace {
int brightness(Rgb c) {
    return c.r * 299 + c.g * 587 + c.b * 114;
}
float hue_of(Rgb c) {
    const float r = c.r, g = c.g, b = c.b;
    return std::atan2(std::sqrt(3.0f) * (g - b), 2.0f * r - g - b);
}
} // namespace

TEST_CASE("humidity_drop: every colour derives from the theme's water colour",
          "[humidity_drop][theme]") {
    const Rgb dark_bg{0x1e, 0x1e, 0x22};
    const Rgb light_bg{0xf0, 0xf0, 0xf0};
    for (Rgb water : {Rgb{0x3A, 0x7C, 0xC8}, Rgb{0x1B, 0x3A, 0x5C}, Rgb{0x31, 0x74, 0x8F},
                      Rgb{0x83, 0xA5, 0x98}, Rgb{0x8B, 0xE9, 0xFD}}) {
        for (Rgb bg : {dark_bg, light_bg}) {
            CAPTURE(int(water.r), int(water.g), int(water.b), int(bg.r));
            const DropColors c = water_colors(water, bg);
            // The theme's hue survives.
            CHECK(std::fabs(hue_of(c.front) - hue_of(water)) < 0.2f);
            CHECK(std::fabs(hue_of(c.back) - hue_of(water)) < 0.2f);
            // Lighter wave in front, darker behind, highlight lighter still.
            CHECK(brightness(c.front) > brightness(c.back));
            CHECK(brightness(c.highlight) > brightness(c.front));
            // The outline stands off its background.
            CHECK(std::abs(brightness(c.outline) - brightness(bg)) > 255 * 1000 / 4);
        }
    }
    // A slate theme colour is lifted toward water, not left grey-blue.
    const DropColors slate = water_colors({0x1B, 0x3A, 0x5C}, light_bg);
    const int spread = std::max({slate.front.r, slate.front.g, slate.front.b}) -
                       std::min({slate.front.r, slate.front.g, slate.front.b});
    CHECK(spread > 110);
    // A theme that chose no hue keeps a grey drop.
    const DropColors grey = water_colors({0x80, 0x80, 0x80}, dark_bg);
    CHECK(grey.front.r == grey.front.g);
    CHECK(grey.front.g == grey.front.b);
    // Background decides which tone outlines: light on dark, dark on light.
    CHECK(brightness(water_colors({0x3A, 0x7C, 0xC8}, dark_bg).outline) >
          brightness(water_colors({0x3A, 0x7C, 0xC8}, light_bg).outline));
}

TEST_CASE("humidity_drop: only a drawn, partial fill animates, and only with animations on",
          "[humidity_drop]") {
    CHECK(should_animate(true, true, 500));
    CHECK(should_animate(true, true, 10));
    CHECK_FALSE(should_animate(false, true, 500));
    CHECK_FALSE(should_animate(true, false, 500));
    CHECK_FALSE(should_animate(true, true, 0));
    CHECK_FALSE(should_animate(true, true, 1000));
    CHECK_FALSE(should_animate(true, true, 1200));
    CHECK_FALSE(should_animate(true, true, kUnknown));
}

// ============================================================================
// Rasterization
// ============================================================================

TEST_CASE("humidity_drop: nothing paints outside the drop, at any size, level or phase",
          "[humidity_drop]") {
    for (int size : {16, 20, 24, 48, 120}) {
        const DropShape s = fit_drop(size, size);
        for (int h : {kUnknown, 0, 150, 500, 850, 1000}) {
            for (float phase : {0.0f, 1.3f, 2.9f, 4.4f}) {
                const Canvas c = render(size, size, h, phase);
                // Counted per render and asserted once, so a per-pixel sweep does
                // not write an assertion per pixel into the --success report.
                int painted_outside = 0;
                int fill_on_outline = 0;
                int first_x = -1;
                int first_y = -1;
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const float d = signed_distance(s, x + 0.5f, y + 0.5f);
                        const bool outside = d > 1.0f && c.at(x, y).a != 0;
                        // Fill keeps clear of the outline's outer edge.
                        const bool on_outline = is_fill(c.at(x, y)) && !(d < 0.0f);
                        if ((outside || on_outline) && first_x < 0) {
                            first_x = x;
                            first_y = y;
                        }
                        painted_outside += outside ? 1 : 0;
                        fill_on_outline += on_outline ? 1 : 0;
                    }
                }
                CAPTURE(size, h, phase, first_x, first_y);
                REQUIRE(painted_outside == 0);
                REQUIRE(fill_on_outline == 0);
            }
        }
    }
}

TEST_CASE("humidity_drop: every pixel is written", "[humidity_drop]") {
    // The buffer starts as 0xAB garbage; a corner the rasterizer skipped would keep it.
    const Canvas c = render(24, 24, 500);
    CHECK(c.at(0, 0).a == 0);
    CHECK(c.at(23, 23).a == 0);
    CHECK(c.at(0, 0).r == 0);
}

TEST_CASE("humidity_drop: 0% and unknown fill nothing; 100% fills the inside", "[humidity_drop]") {
    const int size = 48;
    const DropShape s = fit_drop(size, size);
    CHECK(count_fill(render(size, size, 0)) == 0);
    CHECK(count_fill(render(size, size, kUnknown)) == 0);

    const Canvas full = render(size, size, 1000);
    const Pixel centre = full.at(static_cast<int>(s.cx), static_cast<int>(s.cy));
    CHECK(is_fill(centre));
    const int near_tip_y = static_cast<int>(s.tip_y + stroke_width(s) * 3.0f);
    CHECK(is_fill(full.at(static_cast<int>(s.cx), near_tip_y)));
}

TEST_CASE("humidity_drop: the fill grows with humidity", "[humidity_drop]") {
    const int size = 64;
    const int f15 = count_fill(render(size, size, 150));
    const int f50 = count_fill(render(size, size, 500));
    const int f85 = count_fill(render(size, size, 850));
    const int f100 = count_fill(render(size, size, 1000));
    CHECK(f15 > 0);
    CHECK(f15 < f50);
    CHECK(f50 < f85);
    // Past 85% only the outline-thin tip is left to fill.
    CHECK(f85 <= f100);
    CHECK(count_fill(render(size, size, 750)) < f85);
}

TEST_CASE("humidity_drop: half full fills below the middle, not above it", "[humidity_drop]") {
    const int size = 64;
    const DropShape s = fit_drop(size, size);
    const Canvas c = render(size, size, 500);
    const float mid = (s.tip_y + s.bottom()) / 2.0f;
    const int cx = static_cast<int>(s.cx);
    CHECK(is_fill(c.at(cx, static_cast<int>(mid + 8))));
    CHECK_FALSE(is_fill(c.at(cx, static_cast<int>(mid - 14))));
}

TEST_CASE("humidity_drop: the front wave is the lighter tone on top, the back peeks above it",
          "[humidity_drop]") {
    const int size = 120;
    const Canvas c = render(size, size, 500);
    int front = 0;
    int back = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const Pixel p = c.at(x, y);
            if (p.a < 200)
                continue;
            front += (p.g > 200 && p.b < 60) ? 1 : 0;
            back += (p.b > 200 && p.g < 60) ? 1 : 0;
        }
    }
    CHECK(front > back);
    CHECK(back > 0);
}

TEST_CASE("humidity_drop: unknown draws a faint outline only", "[humidity_drop]") {
    const int size = 48;
    const DropShape s = fit_drop(size, size);
    const Canvas known = render(size, size, 0);
    const Canvas unknown = render(size, size, kUnknown);
    // The outline at the bottom centre.
    const int x = static_cast<int>(s.cx);
    const int y = static_cast<int>(s.bottom() - stroke_width(s) * 0.5f);
    CHECK(known.at(x, y).a > 200);
    CHECK(unknown.at(x, y).a > 0);
    CHECK(unknown.at(x, y).a < known.at(x, y).a);
    // And no interior tint: the centre stays clear.
    CHECK(unknown.at(x, static_cast<int>(s.cy)).a == 0);
}

// ============================================================================
// Widget
// ============================================================================

namespace {
lv_obj_t* make_drop(lv_obj_t* parent, const char* subject, const char* size = "md") {
    const char* attrs[] = {"bind_value", subject, "size", size, nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(parent, "humidity_drop", attrs));
}
} // namespace

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: bind_value tracks the subject",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    helix::Subject subj;
    subj.init_int(425);
    lv_xml_register_subject(nullptr, "test_drop_humidity", &subj);

    lv_obj_t* drop = make_drop(test_screen(), "test_drop_humidity");
    REQUIRE(drop != nullptr);
    process_lvgl(20);
    CHECK(helix::ui::humidity_drop_value(drop) == 425);

    lv_subject_set_int(&subj, 870);
    process_lvgl(20);
    CHECK(helix::ui::humidity_drop_value(drop) == 870);

    lv_subject_set_int(&subj, kUnknown);
    process_lvgl(20);
    CHECK(helix::ui::humidity_drop_value(drop) == kUnknown);

    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: without a binding it shows no reading",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    const char* attrs[] = {"size", "sm", nullptr};
    lv_obj_t* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    CHECK(helix::ui::humidity_drop_value(drop) == kUnknown);
    helix::ui::humidity_drop_set_value(drop, 330);
    CHECK(helix::ui::humidity_drop_value(drop) == 330);
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: size matches the water icon of the same size",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    for (const char* size : {"xs", "sm", "md", "lg"}) {
        CAPTURE(size);
        const char* icon_attrs[] = {"src", "water", "size", size, nullptr};
        auto* icon = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "icon", icon_attrs));
        const char* drop_attrs[] = {"size", size, nullptr};
        auto* drop =
            static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", drop_attrs));
        REQUIRE(icon != nullptr);
        REQUIRE(drop != nullptr);
        lv_obj_update_layout(test_screen());
        CHECK(lv_obj_get_width(drop) == lv_obj_get_width(icon));
        CHECK(lv_obj_get_height(drop) == lv_obj_get_height(icon));
        CHECK(lv_obj_get_width(drop) > 0);
        CHECK(helix::ui::is_humidity_drop(drop));
        CHECK_FALSE(helix::ui::is_humidity_drop(icon));
        lv_obj_delete(icon);
        lv_obj_delete(drop);
    }
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: a font change resizes it with the icon",
                 "[humidity_drop][widget]") {
    // bind_tile_rung re-faces an icon by its text font; the drop follows the same font.
    helix::ui::register_humidity_drop_widget();
    const char* attrs[] = {"size", "xs", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    lv_obj_update_layout(test_screen());
    const int32_t small_h = lv_obj_get_height(drop);
    lv_obj_set_style_text_font(drop, &mdi_icons_48, LV_PART_MAIN);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_height(drop) > small_h);
    CHECK(lv_obj_get_height(drop) == static_cast<int32_t>(lv_font_get_line_height(&mdi_icons_48)));
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: explicit width and height win over the font",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    const char* attrs[] = {"width", "90", "height", "120", "size", "xs", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_width(drop) == 90);
    CHECK(lv_obj_get_height(drop) == 120);
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: the wave ticks only while animations are on",
                 "[humidity_drop][widget]") {
    helix::ui::register_humidity_drop_widget();
    SettingsManager::instance().init_subjects();
    auto& dsm = DisplaySettingsManager::instance();
    const bool was = dsm.get_animations_enabled();
    const char* attrs[] = {"size", "lg", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    helix::ui::humidity_drop_set_value(drop, 500);

    // The test display never runs periodic timers, so step the shared one by hand.
    dsm.set_animations_enabled(false);
    const float frozen = helix::ui::humidity_drop_phase();
    helix::ui::humidity_drop_tick();
    CHECK(helix::ui::humidity_drop_phase() == frozen);

    dsm.set_animations_enabled(true);
    helix::ui::humidity_drop_tick();
    CHECK(helix::ui::humidity_drop_phase() != frozen);

    dsm.set_animations_enabled(was);
    lv_obj_delete(drop);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "humidity_drop: the home humidity tile's drop follows the chamber",
                 "[humidity_drop][widget]") {
    auto& sensors = helix::sensors::HumiditySensorManager::instance();
    sensors.init_subjects();
    lv_subject_t* chamber = sensors.get_chamber_humidity_subject();
    REQUIRE(chamber != nullptr);
    lv_subject_set_int(chamber, 612);

    auto* tile =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "panel_widget_humidity", nullptr));
    REQUIRE(tile != nullptr);
    lv_obj_t* drop = lv_obj_find_by_name(tile, "humidity_icon");
    REQUIRE(drop != nullptr);
    REQUIRE(helix::ui::is_humidity_drop(drop));
    CHECK(helix::ui::humidity_drop_value(drop) == 612);

    lv_subject_set_int(chamber, -1);
    CHECK(helix::ui::humidity_drop_value(drop) == kUnknown);
    lv_obj_delete(tile);
}

TEST_CASE_METHOD(XMLTestFixture, "humidity_drop: a theme change re-tints the drop",
                 "[humidity_drop][widget][theme]") {
    helix::ui::register_humidity_drop_widget();
    auto& tm = ThemeManager::instance();
    const char* attrs[] = {"size", "lg", nullptr};
    auto* drop = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "humidity_drop", attrs));
    REQUIRE(drop != nullptr);
    helix::ui::humidity_drop_set_value(drop, 1000);
    lv_obj_update_layout(test_screen());
    // The drop's centre pixel, as it draws: red minus blue.
    auto centre_warmth = [drop]() {
        lv_draw_buf_t* snap = lv_snapshot_take(drop, LV_COLOR_FORMAT_ARGB8888);
        REQUIRE(snap != nullptr);
        const uint32_t x = snap->header.w / 2;
        const uint32_t y = snap->header.h * 2 / 3;
        const uint8_t* p = snap->data + y * snap->header.stride + x * 4;
        const int warmth = int(p[2]) - int(p[0]);
        lv_draw_buf_destroy(snap);
        return warmth;
    };

    const DropColors before = helix::ui::humidity_drop_colors();
    const int cool = centre_warmth();
    ThemePalette warm = tm.current_palette();
    warm.info = lv_color_hex(0xC8503A);
    tm.preview_palette(warm);
    const DropColors during = helix::ui::humidity_drop_colors();
    CHECK(during.front.r > during.front.b);
    CHECK((during.front.r != before.front.r || during.front.b != before.front.b));
    // The same drop, redrawn with no restart, now in the warm water.
    CHECK(centre_warmth() > cool + 60);
    tm.cancel_preview();
    CHECK(centre_warmth() == cool);
    lv_obj_delete(drop);
    const DropColors after = helix::ui::humidity_drop_colors();
    CHECK(after.front.r == before.front.r);
    CHECK(after.front.b == before.front.b);
}
