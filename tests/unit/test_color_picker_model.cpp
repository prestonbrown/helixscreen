// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/color_picker_model.h"
#include "../../include/color_recents.h"
#include "../../include/settings_manager.h"
#include "../helix_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"

#include <algorithm>
#include <set>

#include "../catch_amalgamated.hpp"

using helix::ColorPalette;
using helix::ColorPickerModel;
using helix::ColorRecents;
using helix::HexKeypad;
using helix::Hsv;
using helix::HsvChannel;

namespace {

/// Every test starts from a fresh install: no recents key written.
void forget_recents() {
    auto& data = helix::ConfigTestAccess::data(*helix::Config::get_instance());
    if (data.is_object()) {
        data.erase("color_recents");
    }
}

using Kind = ColorRecents::Kind;

} // namespace

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------

TEST_CASE("Palette tables have the grid sizes their callers lay out", "[color_picker]") {
    CHECK(helix::color_palette(ColorPalette::Filament).size() == 60);
    CHECK(helix::color_palette(ColorPalette::Led).size() == 30);
    CHECK(helix::color_palette(ColorPalette::Theme).size() == 30);
    CHECK(helix::color_palette_columns(ColorPalette::Filament) == 10);
    CHECK(helix::color_palette_columns(ColorPalette::Led) == 10);
    CHECK(helix::color_palette_columns(ColorPalette::Theme) == 6);
}

TEST_CASE("The filament grid has no duplicate cells", "[color_picker]") {
    const auto& grid = helix::color_palette(ColorPalette::Filament);
    CHECK(std::set<uint32_t>(grid.begin(), grid.end()).size() == grid.size());
}

TEST_CASE("The filament grid opens with a white-to-black gray ramp", "[color_picker]") {
    const auto& grid = helix::color_palette(ColorPalette::Filament);
    CHECK(grid.front() == 0xFFFFFF);
    for (int i = 0; i < 10; ++i) {
        INFO("cell " << i);
        const uint32_t c = grid[i];
        CHECK(((c >> 16) & 0xFF) == (c & 0xFF)); // a gray: R == B
        CHECK(((c >> 8) & 0xFF) == (c & 0xFF));  // and G == B
        if (i > 0) {
            CHECK((c & 0xFF) < (grid[i - 1] & 0xFF));
        }
    }
}

TEST_CASE("The LED grid's first row runs warm to cool", "[color_picker]") {
    const auto& led = helix::color_palette(ColorPalette::Led);
    // Warmer white has less blue: blue rises along the row while red stays full.
    for (int i = 1; i < 10; ++i) {
        INFO("cell " << i);
        CHECK((led[i] & 0xFF) > (led[i - 1] & 0xFF));
        CHECK(((led[i] >> 16) & 0xFF) == 0xFF);
    }
}

// ---------------------------------------------------------------------------
// HSV
// ---------------------------------------------------------------------------

TEST_CASE("HSV converts both ways for primaries and grays", "[color_picker]") {
    CHECK(helix::rgb_to_hsv(0xFF0000) == Hsv{0, 100, 100});
    CHECK(helix::rgb_to_hsv(0x00FF00) == Hsv{120, 100, 100});
    CHECK(helix::rgb_to_hsv(0x0000FF) == Hsv{240, 100, 100});
    CHECK(helix::rgb_to_hsv(0x808080) == Hsv{0, 0, 50});
    CHECK(helix::hsv_to_rgb({0, 100, 100}) == 0xFF0000);
    CHECK(helix::hsv_to_rgb({240, 100, 100}) == 0x0000FF);
    CHECK(helix::hsv_to_rgb({229, 0, 100}) == 0xFFFFFF);
    CHECK(helix::hsv_to_rgb({229, 53, 0}) == 0x000000);
}

TEST_CASE("Picking a gray or black keeps the hue the sliders had", "[color_picker]") {
    ColorPickerModel m(0x1E2440);
    const int hue = m.hsv().h;
    const int sat = m.hsv().s;
    REQUIRE(hue > 200);

    m.set_rgb(0x808080);
    CHECK(m.hsv().h == hue);
    CHECK(m.hsv().s == 0);
    CHECK(m.rgb() == 0x808080);

    m.set_rgb(0x1E2440);
    m.set_rgb(0x000000);
    CHECK(m.hsv().h == hue);
    CHECK(m.hsv().s == sat); // black keeps saturation too
    CHECK(m.hsv().v == 0);

    // Brightness back up returns to the hue it came from, not red.
    m.set_channel(HsvChannel::Brightness, 100);
    CHECK(helix::rgb_to_hsv(m.rgb()).h == hue);
}

TEST_CASE("A picked RGB is kept exactly, not rounded through HSV", "[color_picker]") {
    ColorPickerModel m;
    m.set_rgb(0x1E2441);
    CHECK(m.rgb() == 0x1E2441);
}

TEST_CASE("Stepping wraps hue and clamps saturation and brightness", "[color_picker]") {
    ColorPickerModel m(0xFF0000);
    m.step(HsvChannel::Hue, -1);
    CHECK(m.hsv().h == 358);
    m.step(HsvChannel::Hue, +1);
    m.step(HsvChannel::Hue, +1);
    CHECK(m.hsv().h == 2);

    m.step(HsvChannel::Saturation, +1);
    CHECK(m.hsv().s == 100);
    m.set_channel(HsvChannel::Brightness, 0);
    m.step(HsvChannel::Brightness, -1);
    CHECK(m.hsv().v == 0);
    m.step(HsvChannel::Brightness, +1);
    CHECK(m.hsv().v == 1);

    m.set_channel(HsvChannel::Saturation, 250);
    CHECK(m.hsv().s == 100);
    m.set_channel(HsvChannel::Hue, 365);
    CHECK(m.hsv().h == 5);
}

TEST_CASE("A channel change recomputes the RGB", "[color_picker]") {
    ColorPickerModel m(0xFF0000);
    m.set_channel(HsvChannel::Hue, 240);
    CHECK(m.rgb() == 0x0000FF);
}

// ---------------------------------------------------------------------------
// Hex keypad
// ---------------------------------------------------------------------------

TEST_CASE("Hex keypad: OK waits for six digits, backspace removes one", "[color_picker]") {
    HexKeypad k;
    for (char c : std::string("1e244")) {
        CHECK(k.push(c));
    }
    CHECK_FALSE(k.complete());
    CHECK_FALSE(k.result().has_value());
    CHECK(k.push('0'));
    CHECK(k.complete());
    CHECK(k.digits() == "1E2440");
    CHECK_FALSE(k.push('F')); // a seventh is refused
    REQUIRE(k.result().has_value());
    CHECK(*k.result() == 0x1E2440);

    k.backspace();
    CHECK(k.digits() == "1E244");
    CHECK_FALSE(k.complete());
    CHECK_FALSE(k.push('G'));
}

// ---------------------------------------------------------------------------
// Recents
// ---------------------------------------------------------------------------

TEST_CASE_METHOD(HelixTestFixture, "Recents: a fresh install reads the seed lists",
                 "[color_picker]") {
    forget_recents();
    CHECK(ColorRecents::list(Kind::Filament) == ColorRecents::seed(Kind::Filament));
    CHECK(ColorRecents::list(Kind::Led) == ColorRecents::seed(Kind::Led));
}

TEST_CASE_METHOD(HelixTestFixture, "Recents: newest first, and a repeat moves to the front",
                 "[color_picker]") {
    forget_recents();
    ColorRecents::remember(Kind::Filament, 0x112233);
    auto l = ColorRecents::list(Kind::Filament);
    CHECK(l.front() == 0x112233);
    CHECK(l.size() == ColorRecents::seed(Kind::Filament).size() + 1);

    const uint32_t white = 0xFFFFFF; // in the seed, not at its front
    ColorRecents::remember(Kind::Filament, white);
    l = ColorRecents::list(Kind::Filament);
    CHECK(l[0] == white);
    CHECK(l[1] == 0x112233);
    CHECK(std::count(l.begin(), l.end(), white) == 1);
}

TEST_CASE_METHOD(HelixTestFixture, "Recents: the cap drops the oldest", "[color_picker]") {
    forget_recents();
    for (uint32_t i = 1; i <= ColorRecents::kCap + 3; ++i) {
        ColorRecents::remember(Kind::Filament, i);
    }
    const auto l = ColorRecents::list(Kind::Filament);
    REQUIRE(l.size() == ColorRecents::kCap);
    CHECK(l.front() == ColorRecents::kCap + 3);
    CHECK(l.back() == 4);
}

TEST_CASE_METHOD(HelixTestFixture, "Recents: remove, and the kinds are independent",
                 "[color_picker]") {
    forget_recents();
    ColorRecents::remember(Kind::Led, 0xABCDEF);
    ColorRecents::remove(Kind::Filament, 0xFFFFFF);
    const auto fil = ColorRecents::list(Kind::Filament);
    CHECK(std::count(fil.begin(), fil.end(), 0xFFFFFFu) == 0);
    CHECK(std::count(fil.begin(), fil.end(), 0xABCDEFu) == 0);
    const auto led = ColorRecents::list(Kind::Led);
    CHECK(led.front() == 0xABCDEF);
    CHECK(std::count(led.begin(), led.end(), 0xFFFFFFu) == 1);

    // Removing every color leaves an empty list, not the seed again.
    for (uint32_t c : ColorRecents::list(Kind::Led)) {
        ColorRecents::remove(Kind::Led, c);
    }
    CHECK(ColorRecents::list(Kind::Led).empty());
}

TEST_CASE_METHOD(HelixTestFixture, "Recents persist as #RRGGBB strings in settings",
                 "[color_picker]") {
    forget_recents();
    ColorRecents::remember(Kind::Filament, 0x1E2440);
    const auto stored = helix::SettingsManager::instance().get_color_recents("filament");
    REQUIRE(stored.has_value());
    CHECK(stored->front() == "#1E2440");

    // A list written elsewhere (a restored backup) reads back, malformed and
    // repeated entries dropped.
    helix::SettingsManager::instance().set_color_recents(
        "filament", {"#00FF00", "bogus", "#00ff00", "#0000FF"});
    CHECK(ColorRecents::list(Kind::Filament) == std::vector<uint32_t>{0x00FF00, 0x0000FF});
}
