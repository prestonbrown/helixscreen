// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// Object names come from the slicer in any script, while the UI fonts carry
// only what the active language needs. A name the label's font cannot draw is
// shown as "Object <n>", numbered like the object's badge.

#include "ui_exclude_object_badges.h"
#include "ui_fonts.h"
#include "ui_text.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/zero_lane_numbering.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

// The compiled UI font. CJK glyphs reach it only through the fallback the
// zh/ja languages attach, which a test never loads.
const lv_font_t* const kFont = &noto_sans_14;

constexpr const char* kCjk = "\xE7\xAB\x8B\xE6\x96\xB9\xE4\xBD\x93"; // 立方体
constexpr const char* kMixed = "Cube_\xE7\xAB\x8B\xE6\x96\xB9";      // Cube_立方
constexpr const char* kCyrillic = "\xD0\x9A\xD1\x83\xD0\xB1_1";      // Куб_1

} // namespace

TEST_CASE("font_renders_text accepts text whose every glyph the font has",
          "[object_display_name][ui_text]") {
    CHECK(font_renders_text(kFont, "Cube_id_1 copy (2)"));
    CHECK(font_renders_text(kFont, kCyrillic));
    CHECK(font_renders_text(kFont, ""));
    CHECK(font_renders_text(kFont, "two\nlines\t"));
    CHECK(font_renders_text(nullptr, kCjk));
}

TEST_CASE("font_renders_text rejects text with any glyph the font lacks",
          "[object_display_name][ui_text]") {
    CHECK_FALSE(font_renders_text(kFont, kCjk));
    CHECK_FALSE(font_renders_text(kFont, kMixed));
}

TEST_CASE("font_renders_text rejects malformed UTF-8", "[object_display_name][ui_text]") {
    CHECK_FALSE(font_renders_text(kFont, "Pi\xE8"
                                         "ce"));       // lead byte with no continuation
    CHECK_FALSE(font_renders_text(kFont, "Part\x80")); // lone continuation byte
}

TEST_CASE("font_renders_text follows the font's fallback chain", "[object_display_name][ui_text]") {
    // A copy whose own glyph table has nothing, falling back to the real font.
    lv_font_t empty = noto_sans_14;
    empty.get_glyph_dsc = [](const lv_font_t*, lv_font_glyph_dsc_t*, uint32_t, uint32_t) {
        return false;
    };
    empty.fallback = nullptr;
    CHECK_FALSE(font_renders_text(&empty, "A"));

    empty.fallback = kFont;
    CHECK(font_renders_text(&empty, "A"));
    CHECK_FALSE(font_renders_text(&empty, kCjk));
}

TEST_CASE_METHOD(LVGLTestFixture, "object_display_name keeps a name the font draws",
                 "[object_display_name]") {
    CHECK(object_display_name("Cube_id_1", 0, kFont) == "Cube_id_1");
    CHECK(object_display_name(kCyrillic, 4, kFont) == kCyrillic);
}

TEST_CASE_METHOD(LVGLTestFixture, "object_display_name numbers a name the font cannot draw",
                 "[object_display_name]") {
    CHECK(object_display_name(kCjk, 0, kFont) == "Object 1");
    CHECK(object_display_name(kMixed, 2, kFont) == "Object 3");
    CHECK(object_display_name("", 1, kFont) == "Object 2");
    CHECK(object_display_name(kCjk, -1, kFont) == "Object");

    ScopedZeroLaneNumbering zero; // printed objects are counted, not numbered positions
    CHECK(object_display_name(kCjk, 0, kFont) == "Object 1");
}

TEST_CASE_METHOD(LVGLTestFixture, "object_display_name numbers by position in the defined list",
                 "[object_display_name]") {
    const std::vector<std::string> defined = {"Cube_id_0", kCjk, kMixed};
    CHECK(object_display_name(defined, "Cube_id_0", kFont) == "Cube_id_0");
    CHECK(object_display_name(defined, kCjk, kFont) == "Object 2");
    CHECK(object_display_name(defined, kMixed, kFont) == "Object 3");
    CHECK(object_display_name(defined, "\xE6\x9C\xAA\xE7\x9F\xA5", kFont) == "Object"); // 未知
}

TEST_CASE("defined_object_index matches names case-insensitively, as Klipper stores them",
          "[object_display_name][pre_start_exclude]") {
    const std::vector<std::string> defined = {"CONE_ID_0", "CUBE_\xE7\xAB\x8B\xE6\x96\xB9"};
    CHECK(defined_object_index(defined, "CONE_ID_0") == 0);
    CHECK(defined_object_index(defined, "Cone_id_0") == 0);
    CHECK(defined_object_index(defined, kMixed) == 1);
    CHECK(defined_object_index(defined, "Cylinder") == -1);
    CHECK(defined_object_index({}, "Cone_id_0") == -1);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "object_display_name numbers a viewer-cased name Klipper upper-cased",
                 "[object_display_name]") {
    const std::vector<std::string> defined = {"CONE_ID_0", "CUBE_\xE7\xAB\x8B\xE6\x96\xB9"};
    CHECK(object_display_name(defined, kMixed, kFont) == "Object 2");
}
