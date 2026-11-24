// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_text.cpp
 * @brief Unit tests for ui_text.cpp - Semantic text widgets with stroke support
 *
 * Tests cover:
 * - Public API (ui_text_set_stroke) with valid and invalid inputs
 * - NULL pointer handling
 * - Stroke style property application
 */

#include "../../include/ui_text.h"
#include "../ui_test_utils.h"

#include <spdlog/spdlog.h>

#include <string>

#include "../catch_amalgamated.hpp"

// Test fixture for text widget tests
class TextTest {
  public:
    TextTest() {
        spdlog::set_level(spdlog::level::debug);

        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a headless display for testing (800x480 = MEDIUM screen)
        alignas(64) static lv_color_t buf[800 * 10];
        display_ = lv_display_create(800, 480);
        lv_display_set_buffers(display_, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(
            display_, [](lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
                LV_UNUSED(area);
                LV_UNUSED(px_map);
                lv_display_flush_ready(disp); // Dummy flush for headless testing
            });

        // Create a screen to hold test objects
        screen_ = lv_obj_create(nullptr);
        lv_screen_load(screen_);
    }

    ~TextTest() {
        if (screen_) {
            lv_obj_delete(screen_);
            screen_ = nullptr;
        }
        if (display_) {
            lv_display_delete(display_);
            display_ = nullptr;
        }
        spdlog::set_level(spdlog::level::warn);
    }

    lv_obj_t* get_screen() {
        return screen_;
    }

  private:
    lv_display_t* display_ = nullptr;
    lv_obj_t* screen_ = nullptr;
};

// Public API Tests - NULL pointer handling

TEST_CASE("ui_text_set_stroke handles NULL label", "[ui_text][api][error]") {
    TextTest fixture;

    // Should log warning and return without crashing
    REQUIRE_NOTHROW(ui_text_set_stroke(nullptr, 2, lv_color_black(), LV_OPA_COVER));
}

// Stroke Property Application Tests

TEST_CASE("ui_text_set_stroke applies stroke properties", "[ui_text][stroke]") {
    TextTest fixture;

    lv_obj_t* label = lv_label_create(fixture.get_screen());
    lv_label_set_text(label, "Test");

    SECTION("Sets stroke width") {
        ui_text_set_stroke(label, 2, lv_color_black(), LV_OPA_COVER);

        int32_t width = lv_obj_get_style_text_outline_stroke_width(label, LV_PART_MAIN);
        REQUIRE(width == 2);
    }

    SECTION("Sets stroke color") {
        lv_color_t red = lv_color_hex(0xFF0000);
        ui_text_set_stroke(label, 2, red, LV_OPA_COVER);

        lv_color_t color = lv_obj_get_style_text_outline_stroke_color(label, LV_PART_MAIN);
        // Compare RGB values (LVGL may store colors differently internally)
        REQUIRE(lv_color_eq(color, red));
    }

    SECTION("Sets stroke opacity") {
        ui_text_set_stroke(label, 2, lv_color_black(), LV_OPA_50);

        lv_opa_t opa = lv_obj_get_style_text_outline_stroke_opa(label, LV_PART_MAIN);
        REQUIRE(opa == LV_OPA_50);
    }

    SECTION("Zero width disables stroke") {
        // First apply a stroke
        ui_text_set_stroke(label, 2, lv_color_black(), LV_OPA_COVER);
        REQUIRE(lv_obj_get_style_text_outline_stroke_width(label, LV_PART_MAIN) == 2);

        // Then disable it
        ui_text_set_stroke(label, 0, lv_color_black(), LV_OPA_COVER);
        REQUIRE(lv_obj_get_style_text_outline_stroke_width(label, LV_PART_MAIN) == 0);
    }

    lv_obj_delete(label);
}

// Edge Cases

TEST_CASE("ui_text_set_stroke edge cases", "[ui_text][stroke][edge]") {
    TextTest fixture;

    lv_obj_t* label = lv_label_create(fixture.get_screen());
    lv_label_set_text(label, "Test");

    SECTION("Large stroke width is accepted") {
        ui_text_set_stroke(label, 10, lv_color_black(), LV_OPA_COVER);
        REQUIRE(lv_obj_get_style_text_outline_stroke_width(label, LV_PART_MAIN) == 10);
    }

    SECTION("Negative stroke width is stored") {
        // LVGL may accept negative values - test that we don't crash
        REQUIRE_NOTHROW(ui_text_set_stroke(label, -1, lv_color_black(), LV_OPA_COVER));
    }

    SECTION("Zero opacity makes stroke invisible") {
        ui_text_set_stroke(label, 2, lv_color_black(), LV_OPA_TRANSP);
        REQUIRE(lv_obj_get_style_text_outline_stroke_opa(label, LV_PART_MAIN) == LV_OPA_TRANSP);
    }

    SECTION("Full opacity makes stroke fully visible") {
        ui_text_set_stroke(label, 2, lv_color_black(), LV_OPA_COVER);
        REQUIRE(lv_obj_get_style_text_outline_stroke_opa(label, LV_PART_MAIN) == LV_OPA_COVER);
    }

    lv_obj_delete(label);
}

// Text Transform Tests

TEST_CASE("text_transform uppercase via event callback", "[ui_text][transform]") {
    TextTest fixture;

    lv_obj_t* label = lv_label_create(fixture.get_screen());

    SECTION("Uppercases ASCII text") {
        lv_label_set_text(label, "Hello World");
        ui_text_apply_transform(label, "uppercase");

        REQUIRE(std::string(lv_label_get_text(label)) == "HELLO WORLD");
    }

    SECTION("Already uppercase text is unchanged") {
        lv_label_set_text(label, "ALREADY UPPER");
        ui_text_apply_transform(label, "uppercase");

        REQUIRE(std::string(lv_label_get_text(label)) == "ALREADY UPPER");
    }

    SECTION("Mixed case and numbers") {
        lv_label_set_text(label, "Test 123 mixedCase");
        ui_text_apply_transform(label, "uppercase");

        REQUIRE(std::string(lv_label_get_text(label)) == "TEST 123 MIXEDCASE");
    }

    SECTION("Empty text does not crash") {
        lv_label_set_text(label, "");
        REQUIRE_NOTHROW(ui_text_apply_transform(label, "uppercase"));
    }

    SECTION("No transform without attribute") {
        lv_label_set_text(label, "Hello");
        REQUIRE(std::string(lv_label_get_text(label)) == "Hello");
    }

    SECTION("Text set after transform is also uppercased") {
        ui_text_apply_transform(label, "uppercase");
        lv_label_set_text(label, "new text");

        REQUIRE(std::string(lv_label_get_text(label)) == "NEW TEXT");
    }

    SECTION("Accented Latin characters are uppercased") {
        ui_text_apply_transform(label, "uppercase");
        lv_label_set_text(label, "caf\xC3\xA9 na\xC3\xAFve"); // café naïve

        REQUIRE(std::string(lv_label_get_text(label)) == "CAF\xC3\x89 NA\xC3\x8FVE"); // CAFÉ NAÏVE
    }

    SECTION("Cyrillic, Greek and Latin Extended letters are uppercased") {
        ui_text_apply_transform(label, "uppercase");
        const auto upper = [&](const char* text) {
            lv_label_set_text(label, text);
            return std::string(lv_label_get_text(label));
        };
        CHECK(upper("Дисплей") == "ДИСПЛЕЙ");
        CHECK(upper("ёж ѓ") == "ЁЖ Ѓ");
        CHECK(upper("ґ і ї") == "Ґ І Ї");
        CHECK(upper("ελληνικά ς") == "ΕΛΛΗΝΙΚΆ Σ");
        CHECK(upper("łódź őz ÿ") == "ŁÓDŹ ŐZ Ÿ");
        CHECK(upper("tiếng việt") == "TIẾNG VIỆT");
    }

    SECTION("A letter whose uppercase is longer, or has none, is left alone") {
        ui_text_apply_transform(label, "uppercase");
        lv_label_set_text(label, "Größe 中文 ı");
        CHECK(std::string(lv_label_get_text(label)) == "GRÖßE 中文 ı");
    }

    SECTION("A sequence cut off at the end of the text is not read past") {
        ui_text_apply_transform(label, "uppercase");
        lv_label_set_text(label, "ab\xD0");
        CHECK(std::string(lv_label_get_text(label)) == "AB\xD0");
        lv_label_set_text(label, "ab\xE1\xBA");
        CHECK(std::string(lv_label_get_text(label)) == "AB\xE1\xBA");
    }

    lv_obj_delete(label);
}

// API Contract Tests

TEST_CASE("ui_text API contracts", "[ui_text][api][contract]") {
    TextTest fixture;

    SECTION("ui_text_init is callable") {
        // Should not crash, even if called multiple times
        REQUIRE_NOTHROW(ui_text_init());
        REQUIRE_NOTHROW(ui_text_init()); // Idempotent
    }

    SECTION("Stroke can be applied to any label") {
        // The API should work on any lv_label, not just text_* widgets
        lv_obj_t* label = lv_label_create(fixture.get_screen());
        lv_label_set_text(label, "Regular label");

        REQUIRE_NOTHROW(ui_text_set_stroke(label, 2, lv_color_black(), LV_OPA_COVER));
        REQUIRE(lv_obj_get_style_text_outline_stroke_width(label, LV_PART_MAIN) == 2);

        lv_obj_delete(label);
    }
}
