// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_format_utils.h"
#include "ui_image_helpers.h"
#include "ui_utils.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "theme_manager.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using helix::ui::format_filament_weight;
using helix::ui::format_file_size;
using helix::ui::format_modified_date;
using helix::ui::format_print_time;
using helix::ui::format_relative_time;
using helix::ui::image_scale_to_contain;
using helix::ui::image_scale_to_cover;

// ============================================================================
// format_print_time() Tests
// ============================================================================

TEST_CASE("UI Utils: format_print_time - minutes only", "[ui_utils][format]") {
    REQUIRE(format_print_time(0) == "0 min");
    REQUIRE(format_print_time(5) == "5 min");
    REQUIRE(format_print_time(59) == "59 min");
}

TEST_CASE("UI Utils: format_print_time - hours and minutes", "[ui_utils][format]") {
    REQUIRE(format_print_time(60) == "1h");
    REQUIRE(format_print_time(90) == "1h 30m");
    REQUIRE(format_print_time(125) == "2h 5m");
    REQUIRE(format_print_time(785) == "13h 5m");
}

TEST_CASE("UI Utils: format_print_time - exact hours", "[ui_utils][format]") {
    REQUIRE(format_print_time(120) == "2h");
    REQUIRE(format_print_time(180) == "3h");
    REQUIRE(format_print_time(1440) == "24h");
}

TEST_CASE("UI Utils: format_print_time - edge cases", "[ui_utils][format][edge]") {
    SECTION("Zero minutes") {
        REQUIRE(format_print_time(0) == "0 min");
    }

    SECTION("Very large values") {
        REQUIRE(format_print_time(10000) == "166h 40m");
    }

    SECTION("One minute") {
        REQUIRE(format_print_time(1) == "1 min");
    }

    SECTION("One hour exactly") {
        REQUIRE(format_print_time(60) == "1h");
    }

    SECTION("Almost two hours") {
        REQUIRE(format_print_time(119) == "1h 59m");
    }
}

// ============================================================================
// format_filament_weight() Tests
// ============================================================================

TEST_CASE("UI Utils: format_filament_weight - less than 1 gram", "[ui_utils][format]") {
    REQUIRE(format_filament_weight(0.0f) == "0.0 g");
    REQUIRE(format_filament_weight(0.5f) == "0.5 g");
    REQUIRE(format_filament_weight(0.9f) == "0.9 g");
}

TEST_CASE("UI Utils: format_filament_weight - 1-10 grams", "[ui_utils][format]") {
    REQUIRE(format_filament_weight(1.0f) == "1.0 g");
    REQUIRE(format_filament_weight(2.5f) == "2.5 g");
    REQUIRE(format_filament_weight(9.9f) == "9.9 g");
}

TEST_CASE("UI Utils: format_filament_weight - 10+ grams", "[ui_utils][format]") {
    REQUIRE(format_filament_weight(10.0f) == "10 g");
    REQUIRE(format_filament_weight(45.7f) == "46 g");
    REQUIRE(format_filament_weight(120.3f) == "120 g");
    REQUIRE(format_filament_weight(999.9f) == "1000 g");
}

TEST_CASE("UI Utils: format_filament_weight - edge cases", "[ui_utils][format][edge]") {
    SECTION("Exactly 1 gram boundary") {
        REQUIRE(format_filament_weight(0.99f) == "1.0 g");
        REQUIRE(format_filament_weight(1.0f) == "1.0 g");
    }

    SECTION("Exactly 10 gram boundary") {
        REQUIRE(format_filament_weight(9.99f) == "10.0 g");
        REQUIRE(format_filament_weight(10.0f) == "10 g");
    }

    SECTION("Very large values") {
        REQUIRE(format_filament_weight(10000.0f) == "10000 g");
    }
}

// ============================================================================
// format_file_size() Tests
// ============================================================================

TEST_CASE("UI Utils: format_file_size - bytes", "[ui_utils][format]") {
    REQUIRE(format_file_size(0) == "0 B");
    REQUIRE(format_file_size(512) == "512 B");
    REQUIRE(format_file_size(1023) == "1023 B");
}

TEST_CASE("UI Utils: format_file_size - kilobytes", "[ui_utils][format]") {
    REQUIRE(format_file_size(1024) == "1.0 KB");
    REQUIRE(format_file_size(1536) == "1.5 KB");
    REQUIRE(format_file_size(10240) == "10.0 KB");
    REQUIRE(format_file_size(1048575) == "1024.0 KB");
}

TEST_CASE("UI Utils: format_file_size - megabytes", "[ui_utils][format]") {
    REQUIRE(format_file_size(1048576) == "1.0 MB");
    REQUIRE(format_file_size(5242880) == "5.0 MB");
    REQUIRE(format_file_size(52428800) == "50.0 MB");
}

TEST_CASE("UI Utils: format_file_size - gigabytes", "[ui_utils][format]") {
    REQUIRE(format_file_size(1073741824) == "1.00 GB");
    REQUIRE(format_file_size(2147483648) == "2.00 GB");
    REQUIRE(format_file_size(5368709120) == "5.00 GB");
}

TEST_CASE("UI Utils: format_file_size - edge cases", "[ui_utils][format][edge]") {
    SECTION("Exactly at boundaries") {
        REQUIRE(format_file_size(1024) == "1.0 KB");
        REQUIRE(format_file_size(1048576) == "1.0 MB");
        REQUIRE(format_file_size(1073741824) == "1.00 GB");
    }

    SECTION("One byte before boundaries") {
        REQUIRE(format_file_size(1023) == "1023 B");
        REQUIRE(format_file_size(1048575) == "1024.0 KB");
    }

    SECTION("Common G-code file sizes") {
        REQUIRE(format_file_size(125000) == "122.1 KB"); // ~125 KB file
        REQUIRE(format_file_size(5800000) == "5.5 MB");  // ~5.8 MB file
    }
}

// ============================================================================
// format_modified_date() Tests
// ============================================================================

TEST_CASE("UI Utils: format_modified_date - valid timestamps", "[ui_utils][format]") {
    // January 15, 2025 14:30:00
    time_t timestamp = 1736954400; // Approximate timestamp

    std::string result = format_modified_date(timestamp);

    // Result should be in format "Jan 15 HH:MM" or similar
    // Just verify it's not empty and has expected length
    REQUIRE(!result.empty());
    REQUIRE(result.length() > 5);
}

TEST_CASE("UI Utils: format_modified_date - edge cases", "[ui_utils][format][edge]") {
    SECTION("Zero timestamp (epoch)") {
        std::string result = format_modified_date(0);
        REQUIRE(!result.empty());
    }

    SECTION("Recent timestamp") {
        time_t now = time(nullptr);
        std::string result = format_modified_date(now);
        REQUIRE(!result.empty());
    }
}

// ============================================================================
// format_relative_time() Tests
// ============================================================================

TEST_CASE("UI Utils: format_relative_time", "[ui_utils][format][i18n]") {
    SECTION("Just now - under 1 minute") {
        REQUIRE(format_relative_time(0) == "Just now");
        REQUIRE(format_relative_time(30000) == "Just now"); // 30s
        REQUIRE(format_relative_time(59999) == "Just now"); // 59.999s
    }

    SECTION("Minutes ago") {
        REQUIRE(format_relative_time(60000) == "1 min ago");    // exactly 1 min
        REQUIRE(format_relative_time(120000) == "2 min ago");   // 2 min
        REQUIRE(format_relative_time(300000) == "5 min ago");   // 5 min
        REQUIRE(format_relative_time(3599999) == "59 min ago"); // just under 1 hour
    }

    SECTION("Hours ago") {
        REQUIRE(format_relative_time(3600000) == "1 hour ago");    // exactly 1 hour
        REQUIRE(format_relative_time(7200000) == "2 hours ago");   // 2 hours
        REQUIRE(format_relative_time(86399999) == "23 hours ago"); // just under 1 day
    }

    SECTION("Days ago") {
        REQUIRE(format_relative_time(86400000) == "1 day ago");   // exactly 1 day
        REQUIRE(format_relative_time(172800000) == "2 days ago"); // 2 days
        REQUIRE(format_relative_time(604800000) == "7 days ago"); // 1 week
    }
}

// ============================================================================
// ui_get_header_content_padding() Tests
// ============================================================================

// NOTE: ui_get_header_content_padding() returns theme_manager_get_spacing("space_lg"),
// a responsive value the theme system scales per display breakpoint at theme init.
// Responsiveness comes from the theme breakpoint, not a caller-supplied height — the
// function takes no arguments.

TEST_CASE("UI Utils: ui_get_header_content_padding - returns space_lg value",
          "[ui_utils][responsive]") {
    SECTION("Returns positive value") {
        lv_coord_t padding = ui_get_header_content_padding();
        REQUIRE(padding > 0);
    }

    SECTION("Returns a valid space_lg value") {
        // space_lg at breakpoints: tiny=8, small=12, medium=16, large=20,
        // xlarge=24, xxlarge=32; fallback is 16 when the theme is uninitialized.
        lv_coord_t padding = ui_get_header_content_padding();
        REQUIRE((padding == 8 || padding == 12 || padding == 16 || padding == 20 || padding == 24 ||
                 padding == 32));
    }
}

// ============================================================================
// ui_get_responsive_header_height() Tests
// ============================================================================

TEST_CASE("UI Utils: ui_get_responsive_header_height - screen sizes", "[ui_utils][responsive]") {
    SECTION("Tiny screen (320px)") {
        REQUIRE(ui_get_responsive_header_height(320) == 40);
    }

    SECTION("Small screen (480px)") {
        REQUIRE(ui_get_responsive_header_height(480) == 60);
    }

    SECTION("Medium screen (599px)") {
        REQUIRE(ui_get_responsive_header_height(599) == 60);
    }

    SECTION("Medium screen (600px)") {
        REQUIRE(ui_get_responsive_header_height(600) == 60);
    }

    SECTION("Large screen (800px)") {
        REQUIRE(ui_get_responsive_header_height(800) == 60);
    }

    SECTION("Extra large screen (1080px)") {
        REQUIRE(ui_get_responsive_header_height(1080) == 60);
    }
}

TEST_CASE("UI Utils: ui_get_responsive_header_height - boundary values",
          "[ui_utils][responsive][edge]") {
    SECTION("One pixel before small threshold (399px)") {
        REQUIRE(ui_get_responsive_header_height(399) == 40);
    }

    SECTION("Exactly at small threshold (400px)") {
        REQUIRE(ui_get_responsive_header_height(400) == 48);
    }

    SECTION("One pixel before medium threshold (479px)") {
        REQUIRE(ui_get_responsive_header_height(479) == 48);
    }

    SECTION("Exactly at medium threshold (480px)") {
        REQUIRE(ui_get_responsive_header_height(480) == 60);
    }
}

// ============================================================================
// Image Scaling Tests (require LVGL)
// ============================================================================

TEST_CASE("UI Utils: image_scale_to_cover - null widget", "[ui_utils][image][error]") {
    REQUIRE(image_scale_to_cover(nullptr, 100, 100) == false);
}

TEST_CASE("UI Utils: image_scale_to_contain - null widget", "[ui_utils][image][error]") {
    REQUIRE(image_scale_to_contain(nullptr, 100, 100) == false);
}

// Note: Testing actual image scaling requires creating LVGL image widgets
// with valid image data, which is more complex. The basic error handling
// is tested above. Full integration tests would go in a separate test file.

// ============================================================================
// ui_brightness_to_lightbulb_icon() Tests
// ============================================================================

TEST_CASE("UI Utils: ui_brightness_to_lightbulb_icon - off state", "[ui_utils][led]") {
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(0), "lightbulb_outline") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(-10), "lightbulb_outline") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(-100), "lightbulb_outline") == 0);
}

TEST_CASE("UI Utils: ui_brightness_to_lightbulb_icon - graduated levels", "[ui_utils][led]") {
    // Test each brightness band
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(1), "lightbulb_on_10") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(14), "lightbulb_on_10") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(15), "lightbulb_on_20") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(24), "lightbulb_on_20") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(25), "lightbulb_on_30") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(34), "lightbulb_on_30") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(35), "lightbulb_on_40") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(44), "lightbulb_on_40") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(45), "lightbulb_on_50") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(54), "lightbulb_on_50") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(55), "lightbulb_on_60") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(64), "lightbulb_on_60") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(65), "lightbulb_on_70") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(74), "lightbulb_on_70") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(75), "lightbulb_on_80") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(84), "lightbulb_on_80") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(85), "lightbulb_on_90") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(94), "lightbulb_on_90") == 0);
}

TEST_CASE("UI Utils: ui_brightness_to_lightbulb_icon - full brightness", "[ui_utils][led]") {
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(95), "lightbulb_on") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(100), "lightbulb_on") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(150), "lightbulb_on") == 0);
    REQUIRE(strcmp(ui_brightness_to_lightbulb_icon(255), "lightbulb_on") == 0);
}

// ============================================================================
// disable_widget_clicks_recursive() Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: disable_widget_clicks_recursive - removes CLICKABLE from children",
                 "[ui_utils][widget_flags]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child1 = lv_obj_create(parent);
    lv_obj_t* child2 = lv_obj_create(parent);

    // LVGL objects are clickable by default
    REQUIRE(lv_obj_has_flag(child1, LV_OBJ_FLAG_CLICKABLE));
    REQUIRE(lv_obj_has_flag(child2, LV_OBJ_FLAG_CLICKABLE));

    helix::ui::disable_widget_clicks_recursive(parent);

    REQUIRE_FALSE(lv_obj_has_flag(child1, LV_OBJ_FLAG_CLICKABLE));
    REQUIRE_FALSE(lv_obj_has_flag(child2, LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: disable_widget_clicks_recursive - recurses into grandchildren",
                 "[ui_utils][widget_flags]") {
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(root);
    lv_obj_t* grandchild = lv_obj_create(child);

    REQUIRE(lv_obj_has_flag(grandchild, LV_OBJ_FLAG_CLICKABLE));

    helix::ui::disable_widget_clicks_recursive(root);

    REQUIRE_FALSE(lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE));
    REQUIRE_FALSE(lv_obj_has_flag(grandchild, LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: disable_widget_clicks_recursive - does not modify parent",
                 "[ui_utils][widget_flags]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_create(parent); // child

    REQUIRE(lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE));

    helix::ui::disable_widget_clicks_recursive(parent);

    // Parent itself should NOT have CLICKABLE removed — only descendants
    REQUIRE(lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: disable_widget_clicks_recursive - null is safe",
                 "[ui_utils][widget_flags][edge]") {
    helix::ui::disable_widget_clicks_recursive(nullptr); // Should not crash
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: disable_widget_clicks_recursive - no children is no-op",
                 "[ui_utils][widget_flags][edge]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    helix::ui::disable_widget_clicks_recursive(parent); // Should not crash
    REQUIRE(lv_obj_has_flag(parent, LV_OBJ_FLAG_CLICKABLE));
}

// ============================================================================
// clear_pressed_state_recursive() Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: clear_pressed_state_recursive - clears PRESSED from tree",
                 "[ui_utils][widget_flags]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_t* grandchild = lv_obj_create(child);

    // Manually set PRESSED state on all
    lv_obj_add_state(parent, LV_STATE_PRESSED);
    lv_obj_add_state(child, LV_STATE_PRESSED);
    lv_obj_add_state(grandchild, LV_STATE_PRESSED);

    helix::ui::clear_pressed_state_recursive(parent);

    REQUIRE_FALSE(lv_obj_has_state(parent, LV_STATE_PRESSED));
    REQUIRE_FALSE(lv_obj_has_state(child, LV_STATE_PRESSED));
    REQUIRE_FALSE(lv_obj_has_state(grandchild, LV_STATE_PRESSED));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: clear_pressed_state_recursive - does not clear other states",
                 "[ui_utils][widget_flags]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_add_state(obj, LV_STATE_PRESSED);
    lv_obj_add_state(obj, LV_STATE_CHECKED);

    helix::ui::clear_pressed_state_recursive(obj);

    REQUIRE_FALSE(lv_obj_has_state(obj, LV_STATE_PRESSED));
    REQUIRE(lv_obj_has_state(obj, LV_STATE_CHECKED));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: clear_pressed_state_recursive - null is safe",
                 "[ui_utils][widget_flags][edge]") {
    helix::ui::clear_pressed_state_recursive(nullptr); // Should not crash
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: clear_pressed_state_recursive - no PRESSED state is no-op",
                 "[ui_utils][widget_flags][edge]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    // Not pressed — should be a no-op
    helix::ui::clear_pressed_state_recursive(obj);
    REQUIRE_FALSE(lv_obj_has_state(obj, LV_STATE_PRESSED));
}

// ============================================================================
// has_sane_parent_chain() Tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: has_sane_parent_chain - null is sane",
                 "[ui_utils][parent_chain]") {
    REQUIRE(helix::ui::has_sane_parent_chain(nullptr));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: has_sane_parent_chain - screen is sane",
                 "[ui_utils][parent_chain]") {
    // test_screen() has parent == NULL (it's a top-level screen)
    REQUIRE(helix::ui::has_sane_parent_chain(test_screen()));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: has_sane_parent_chain - typical tree is sane",
                 "[ui_utils][parent_chain]") {
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(parent);
    lv_obj_t* grandchild = lv_obj_create(child);
    REQUIRE(helix::ui::has_sane_parent_chain(parent));
    REQUIRE(helix::ui::has_sane_parent_chain(child));
    REQUIRE(helix::ui::has_sane_parent_chain(grandchild));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: has_sane_parent_chain - deep-but-bounded tree is sane",
                 "[ui_utils][parent_chain]") {
    // 128 is the cap; 100 levels should walk cleanly
    lv_obj_t* cur = test_screen();
    for (int i = 0; i < 100; ++i) {
        cur = lv_obj_create(cur);
    }
    REQUIRE(helix::ui::has_sane_parent_chain(cur));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: has_sane_parent_chain - cycle returns false",
                 "[ui_utils][parent_chain][edge]") {
    // Simulate the v0.99.35 bug: construct an obj whose parent pointer forms a
    // cycle. We can't reach this state through normal LVGL APIs (lv_obj_set_parent
    // rejects self-as-parent), so we poke obj->parent directly. This mirrors the
    // real-world UAF / memory-reuse case that produced the SonicPad hang.
    lv_obj_t* a = lv_obj_create(test_screen());
    lv_obj_t* b = lv_obj_create(a);
    // Force b->parent = a and a->parent = b (cycle of length 2)
    // lv_obj_t is a private struct; use lv_obj_private.h-style access via the
    // shim of reparenting through the API isn't possible, so we write directly.
    // Offset of `parent` in lv_obj_t is 4 on 32-bit / 8 on 64-bit — use the
    // field name via a reinterpret cast is UB but acceptable for this test.
    // Simpler: lv_obj_set_parent cannot create cycles, so manually corrupt.
    struct LvObjPublic {
        void* class_p;
        lv_obj_t* parent;
    };
    reinterpret_cast<LvObjPublic*>(a)->parent = b;
    // Now a->parent = b, b->parent = a → cycle.
    REQUIRE_FALSE(helix::ui::has_sane_parent_chain(a));
    REQUIRE_FALSE(helix::ui::has_sane_parent_chain(b));
    // Restore before teardown so LVGL cleanup doesn't spin.
    reinterpret_cast<LvObjPublic*>(a)->parent = test_screen();
}

// ============================================================================
// is_on_active_screen() Tests (#1001 trigger guard)
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: is_on_active_screen - null is false",
                 "[ui_utils][active_screen]") {
    REQUIRE_FALSE(helix::ui::is_on_active_screen(nullptr));
}

TEST_CASE_METHOD(LVGLTestFixture, "UI Utils: is_on_active_screen - widget on active screen",
                 "[ui_utils][active_screen]") {
    // test_screen() is the active screen for this fixture's display.
    lv_obj_t* parent = lv_obj_create(test_screen());
    lv_obj_t* child = lv_obj_create(parent);
    REQUIRE(helix::ui::is_on_active_screen(parent));
    REQUIRE(helix::ui::is_on_active_screen(child));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: is_on_active_screen - condemned widget on layer_top is false",
                 "[ui_utils][active_screen][edge]") {
    // Reproduces the #1001 teardown state: safe_clean_children() reparents a
    // condemned subtree onto lv_layer_top() before async deletion. A widget rooted
    // at a layer (not the active screen) must read as "not on the active screen" so
    // PrintStatusWidget::reset_print_card_to_idle() skips the relayout that crashed.
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(helix::ui::is_on_active_screen(obj));
    lv_obj_set_parent(obj, lv_layer_top());
    REQUIRE_FALSE(helix::ui::is_on_active_screen(obj));
    // Reparent back so fixture teardown cleans it with the screen.
    lv_obj_set_parent(obj, test_screen());
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "UI Utils: is_on_active_screen - widget on inactive screen is false",
                 "[ui_utils][active_screen][edge]") {
    // A widget on a loaded-but-not-active screen is also not on the active screen.
    lv_obj_t* other_screen = lv_obj_create(nullptr);
    lv_obj_t* obj = lv_obj_create(other_screen);
    REQUIRE_FALSE(helix::ui::is_on_active_screen(obj));
    lv_obj_delete(other_screen);
}
