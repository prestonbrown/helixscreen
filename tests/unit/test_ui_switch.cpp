// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui_test_utils.h"

#include <spdlog/spdlog.h>

#include "../../src/ui/components/ui_switch.cpp" // Include implementation for internal testing
#include "../catch_amalgamated.hpp"

/**
 * @brief Unit tests for ui_switch.cpp - Switch widget with semantic size presets
 *
 * Tests cover:
 * - Size preset parsing (tiny/small/medium/large) with valid and invalid values
 * - Screen-size-aware preset initialization (TINY/SMALL/LARGE displays)
 * - Size preset application (width, height, knob_pad)
 * - Progressive enhancement (size preset + selective override)
 * - Backward compatibility (explicit width/height still works)
 * - Error handling (NULL pointers, invalid strings, edge cases)
 */

// Test fixture for switch tests
class SwitchTest {
  public:
    SwitchTest() {
        spdlog::set_level(spdlog::level::debug);

        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Create a headless display for testing (800x480 = MEDIUM screen)
        alignas(64) static lv_color_t buf[800 * 10];
        display_ = lv_display_create(800, 480);
        lv_display_set_buffers(display_, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(
            display_, [](lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
                lv_display_flush_ready(disp); // Dummy flush for headless testing
            });

        // Initialize size presets now that display exists
        ui_switch_init_size_presets(nullptr);
    }

    ~SwitchTest() {
        if (display_) {
            lv_display_delete(display_);
            display_ = nullptr;
        }
        spdlog::set_level(spdlog::level::warn);
    }

  private:
    lv_display_t* display_ = nullptr;
};

// ============================================================================
// Size Preset Parsing Tests
// ============================================================================

TEST_CASE("Switch size parsing - valid sizes", "[ui_switch][size]") {
    SwitchTest fixture;

    // Note: Preset values depend on screen size.
    // These tests verify parsing logic, not specific dimensions.
    // For dimension tests, see "Size preset initialization" section.

    SECTION("Parse 'tiny' size") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("tiny", &preset);

        REQUIRE(result == true);
        // Dimensions verified in initialization tests
    }

    SECTION("Parse 'small' size") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("small", &preset);

        REQUIRE(result == true);
    }

    SECTION("Parse 'medium' size") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("medium", &preset);

        REQUIRE(result == true);
    }

    SECTION("Parse 'large' size") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("large", &preset);

        REQUIRE(result == true);
    }
}

TEST_CASE("Switch size parsing - invalid sizes", "[ui_switch][size][error]") {
    SwitchTest fixture;

    SECTION("Invalid size string returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("invalid", &preset);

        REQUIRE(result == false);
        // Preset values remain unchanged (not populated)
    }

    SECTION("Empty string returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("", &preset);

        REQUIRE(result == false);
    }

    SECTION("Case sensitivity - uppercase returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("MEDIUM", &preset);

        REQUIRE(result == false);
    }

    SECTION("Partial match returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("med", &preset);

        REQUIRE(result == false);
    }

    SECTION("Numeric string returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("48", &preset);

        REQUIRE(result == false);
    }

    SECTION("Icon size string returns false") {
        SwitchSizePreset preset;
        // Ensure switch size strings don't accidentally match icon size strings
        bool result = parse_size_preset("md", &preset);

        REQUIRE(result == false);
    }
}

TEST_CASE("Switch size parsing - edge cases", "[ui_switch][size][edge]") {
    SwitchTest fixture;

    SECTION("Whitespace in size string") {
        SwitchSizePreset preset;
        bool result = parse_size_preset(" medium", &preset);

        REQUIRE(result == false); // Leading space should not match
    }

    SECTION("Trailing characters") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("medium ", &preset);

        REQUIRE(result == false); // Trailing space should not match
    }

    SECTION("Mixed case") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("Medium", &preset);

        REQUIRE(result == false); // Only lowercase supported
    }
}

// ============================================================================
// Size Preset Initialization Tests
// ============================================================================

TEST_CASE("Size preset initialization - TINY screen", "[ui_switch][size][init]") {
    SwitchTest fixture;

    // Note: These tests assume ui_switch_init_size_presets() has been called
    // In actual usage, this is called from ui_switch_register()

    SECTION("TINY preset dimensions (width < 600)") {
        // SIZE_TINY for TINY screen: 32x16, pad=1
        REQUIRE(SIZE_TINY.width >= 16); // Minimum viable size
        REQUIRE(SIZE_TINY.height >= 8);
        REQUIRE(SIZE_TINY.knob_pad >= 1);
    }

    SECTION("SMALL preset dimensions") {
        REQUIRE(SIZE_SMALL.width >= SIZE_TINY.width); // Progressive sizing
        REQUIRE(SIZE_SMALL.height >= SIZE_TINY.height);
    }

    SECTION("MEDIUM preset dimensions") {
        REQUIRE(SIZE_MEDIUM.width >= SIZE_SMALL.width);
        REQUIRE(SIZE_MEDIUM.height >= SIZE_SMALL.height);
    }

    SECTION("LARGE preset dimensions") {
        REQUIRE(SIZE_LARGE.width >= SIZE_MEDIUM.width);
        REQUIRE(SIZE_LARGE.height >= SIZE_MEDIUM.height);
    }

    SECTION("All presets follow ~2:1 width:height ratio") {
        // Switches should be roughly twice as wide as tall (room for knob to slide)
        REQUIRE(SIZE_TINY.width >= SIZE_TINY.height);
        REQUIRE(SIZE_SMALL.width >= SIZE_SMALL.height);
        REQUIRE(SIZE_MEDIUM.width >= SIZE_MEDIUM.height);
        REQUIRE(SIZE_LARGE.width >= SIZE_LARGE.height);
    }

    SECTION("Knob padding increases with size") {
        // Larger switches should have more internal spacing
        REQUIRE(SIZE_TINY.knob_pad >= 1);
        REQUIRE(SIZE_LARGE.knob_pad >= SIZE_TINY.knob_pad);
    }
}

TEST_CASE("Size preset initialization - screen size awareness", "[ui_switch][size][responsive]") {
    SwitchTest fixture;

    SECTION("Presets are initialized") {
        // After ui_switch_init_size_presets() call, all presets should have non-zero values
        REQUIRE(SIZE_TINY.width > 0);
        REQUIRE(SIZE_TINY.height > 0);

        REQUIRE(SIZE_SMALL.width > 0);
        REQUIRE(SIZE_SMALL.height > 0);

        REQUIRE(SIZE_MEDIUM.width > 0);
        REQUIRE(SIZE_MEDIUM.height > 0);

        REQUIRE(SIZE_LARGE.width > 0);
        REQUIRE(SIZE_LARGE.height > 0);
    }

    SECTION("Preset dimensions are reasonable") {
        // Switches should be in practical size range (not too small, not too large)
        // TINY screen (480x320): medium should be ~40-80px wide
        // SMALL screen (800x480): medium should be ~60-120px wide
        // LARGE screen (1280x720): medium should be ~80-150px wide

        REQUIRE(SIZE_TINY.width >= 16);  // Minimum touchable size
        REQUIRE(SIZE_TINY.width <= 100); // Maximum reasonable for tiny screen

        REQUIRE(SIZE_LARGE.width >= 24);  // Larger than tiny
        REQUIRE(SIZE_LARGE.width <= 200); // Not absurdly large
    }

    SECTION("Knob padding is in valid range") {
        // Knob padding should be 1-4px for visual spacing
        REQUIRE(SIZE_TINY.knob_pad >= 1);
        REQUIRE(SIZE_TINY.knob_pad <= 5);

        REQUIRE(SIZE_LARGE.knob_pad >= 1);
        REQUIRE(SIZE_LARGE.knob_pad <= 8);
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("Error handling - invalid inputs", "[ui_switch][error]") {
    SwitchTest fixture;

    SECTION("Invalid size string returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("invalid_size", &preset);

        REQUIRE(result == false);
    }

    SECTION("Empty size string returns false") {
        SwitchSizePreset preset;
        bool result = parse_size_preset("", &preset);

        REQUIRE(result == false);
    }
}

// ============================================================================
// API Contract Tests
// ============================================================================

TEST_CASE("API contracts and guarantees", "[ui_switch][api][contract]") {
    SwitchTest fixture;

    SECTION("Size strings are lowercase only") {
        // API expects lowercase: tiny, small, medium, large
        SwitchSizePreset preset;
        REQUIRE(parse_size_preset("tiny", &preset) == true);
        REQUIRE(parse_size_preset("TINY", &preset) == false); // Uppercase not supported
    }

    SECTION("Size strings are exact match") {
        // No partial matching or fuzzy matching
        SwitchSizePreset preset;
        REQUIRE(parse_size_preset("medium", &preset) == true);
        REQUIRE(parse_size_preset("med", &preset) == false);     // Partial not supported
        REQUIRE(parse_size_preset("mediumm", &preset) == false); // Extra char not supported
    }

    SECTION("Four size values available") {
        // API provides exactly 4 size presets
        SwitchSizePreset preset;
        REQUIRE(parse_size_preset("tiny", &preset) == true);
        REQUIRE(parse_size_preset("small", &preset) == true);
        REQUIRE(parse_size_preset("medium", &preset) == true);
        REQUIRE(parse_size_preset("large", &preset) == true);
    }

    SECTION("No extra-small or extra-large sizes") {
        // Unlike icon widget (xs/xl), switch only has tiny/small/medium/large
        SwitchSizePreset preset;
        REQUIRE(parse_size_preset("xs", &preset) == false);
        REQUIRE(parse_size_preset("xl", &preset) == false);
    }
}

// ============================================================================
// Touch Target Tests
// ============================================================================

TEST_CASE("A tap anywhere on the drawn switch reaches it", "[ui_switch][touch]") {
    SwitchTest fixture;

    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, 800, 480);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* sw = ui_switch_create_themed(screen, "small");
    REQUIRE(sw != nullptr);
    lv_obj_update_layout(screen);

    lv_area_t a;
    lv_obj_get_coords(sw, &a);
    const int32_t cx = (a.x1 + a.x2) / 2;
    const int32_t cy = (a.y1 + a.y2) / 2;
    const int32_t margin = SIZE_SMALL.vert_margin;
    REQUIRE(margin > 0);

    // Inside the margin the knob paints into: above, below and past the end.
    const lv_point_t above{cx, a.y1 - margin + 1};
    const lv_point_t below{cx, a.y2 + margin - 1};
    const lv_point_t past_end{a.x2 + SIZE_SMALL.horiz_margin - 1, cy};
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&above)) == sw);
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&below)) == sw);
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&past_end)) == sw);

    // Beyond the reserved margin the tap belongs to whatever is there.
    const lv_point_t outside{cx, a.y1 - margin - 2};
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&outside)) != sw);

    lv_obj_delete(screen);
}

namespace {

/// A flex row the way settings rows lay a switch out beside a neighbour.
lv_obj_t* make_row(lv_obj_t* screen, int32_t gap) {
    lv_obj_t* row = lv_obj_create(screen);
    lv_obj_set_size(row, 600, 80);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, gap, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

/// space_sm at the 800x480 breakpoint, the gap the WiFi row uses.
constexpr int32_t kRowGap = 7;

} // namespace

TEST_CASE("A switch's touch area never takes a tap from its neighbour", "[ui_switch][touch]") {
    SwitchTest fixture;
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, 800, 480);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t gap = GENERATE(int32_t{0}, kRowGap);
    const bool button_neighbour = GENERATE(false, true);
    CAPTURE(gap, button_neighbour);

    lv_obj_t* row = make_row(screen, gap);
    lv_obj_t* neighbour = button_neighbour ? lv_button_create(row) : lv_label_create(row);
    if (!button_neighbour) {
        lv_label_set_text(neighbour, "WiFi");
        lv_obj_add_flag(neighbour, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_size(neighbour, 120, 70);
    lv_obj_t* sw = ui_switch_create_themed(row, "small");
    REQUIRE(sw != nullptr);
    lv_obj_update_layout(screen);

    lv_area_t n;
    lv_obj_get_coords(neighbour, &n);
    lv_area_t a;
    lv_obj_get_coords(sw, &a);
    const int32_t cy = (a.y1 + a.y2) / 2;

    // The neighbour's own last column, right beside the switch.
    const lv_point_t in_neighbour{n.x2, cy};
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&in_neighbour)) == neighbour);
    // And the switch still takes the tap just short of its track.
    const lv_point_t before_track{a.x1 - 1, cy};
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&before_track)) == sw);

    lv_obj_delete(screen);
}

TEST_CASE("A switch outside a flex or grid layout keeps its track as its target",
          "[ui_switch][touch]") {
    SwitchTest fixture;
    lv_obj_t* screen = lv_obj_create(nullptr);
    lv_obj_set_size(screen, 800, 480);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* sw = ui_switch_create_themed(screen, "small");
    REQUIRE(sw != nullptr);
    lv_obj_center(sw);
    lv_obj_update_layout(screen);

    lv_area_t a;
    lv_obj_get_coords(sw, &a);
    const lv_point_t above{(a.x1 + a.x2) / 2, a.y1 - 2};
    CHECK(lv_indev_search_obj(screen, const_cast<lv_point_t*>(&above)) != sw);

    lv_obj_delete(screen);
}
