// Copyright 2025 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// LV_SIZE_CONTENT behavior test - demonstrates and validates content sizing
// for nested containers in LVGL 9.4

#include "lvgl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Test display buffer
static lv_color_t buf1[800 * 100];

// Display flush callback (no-op for headless test)
static void disp_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    lv_display_flush_ready(disp);
}

// Initialize LVGL for headless testing
static void init_lvgl_headless() {
    lv_init();

    // Create a simple display
    lv_display_t * disp = lv_display_create(800, 480);
    lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, disp_flush_cb);
}

// Print object dimensions for debugging
static void print_obj_info(const char* name, lv_obj_t* obj) {
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    int32_t x = lv_obj_get_x(obj);
    int32_t y = lv_obj_get_y(obj);
    printf("  %-20s: x=%4d, y=%4d, w=%4d, h=%4d\n", name, x, y, w, h);
}

// Test 1: Simple container with SIZE_CONTENT and label child
static bool test_simple_label_content() {
    printf("\n=== TEST 1: Simple container with SIZE_CONTENT ===\n");

    lv_obj_t* screen = lv_screen_active();

    // Container with SIZE_CONTENT height
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 200, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(container, 10, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);

    // Label inside
    lv_obj_t* label = lv_label_create(container);
    lv_label_set_text(label, "Hello World");

    printf("Before lv_obj_update_layout():\n");
    print_obj_info("container", container);
    print_obj_info("label", label);

    // Force layout update
    lv_obj_update_layout(screen);

    printf("After lv_obj_update_layout():\n");
    print_obj_info("container", container);
    print_obj_info("label", label);

    int32_t container_h = lv_obj_get_height(container);
    int32_t label_h = lv_obj_get_height(label);

    // Container should have height = label height + padding (10 top + 10 bottom)
    bool pass = container_h >= (label_h + 20) && container_h > 0;
    printf("RESULT: %s (container_h=%d, expected >= %d)\n",
           pass ? "PASS" : "FAIL", container_h, label_h + 20);

    lv_obj_delete(container);
    return pass;
}

// Test 2: Flex container with SIZE_CONTENT
static bool test_flex_container() {
    printf("\n=== TEST 2: Flex container with SIZE_CONTENT ===\n");

    lv_obj_t* screen = lv_screen_active();

    // Flex container with SIZE_CONTENT height
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 200, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 10, 0);
    lv_obj_set_style_pad_gap(container, 5, 0);

    // Multiple children
    lv_obj_t* label1 = lv_label_create(container);
    lv_label_set_text(label1, "Line 1");

    lv_obj_t* label2 = lv_label_create(container);
    lv_label_set_text(label2, "Line 2");

    printf("Before lv_obj_update_layout():\n");
    print_obj_info("container", container);
    print_obj_info("label1", label1);
    print_obj_info("label2", label2);

    lv_obj_update_layout(screen);

    printf("After lv_obj_update_layout():\n");
    print_obj_info("container", container);
    print_obj_info("label1", label1);
    print_obj_info("label2", label2);

    int32_t container_h = lv_obj_get_height(container);
    int32_t label1_h = lv_obj_get_height(label1);
    int32_t label2_h = lv_obj_get_height(label2);

    // Container should have height = label1 + gap + label2 + padding
    int32_t expected_h = label1_h + 5 + label2_h + 20;
    bool pass = container_h >= expected_h && container_h > 0;
    printf("RESULT: %s (container_h=%d, expected >= %d)\n",
           pass ? "PASS" : "FAIL", container_h, expected_h);

    lv_obj_delete(container);
    return pass;
}

// Test 3: NESTED containers with SIZE_CONTENT (THE PROBLEMATIC CASE)
static bool test_nested_containers() {
    printf("\n=== TEST 3: NESTED containers with SIZE_CONTENT ===\n");

    lv_obj_t* screen = lv_screen_active();

    // Outer container with SIZE_CONTENT
    lv_obj_t* outer = lv_obj_create(screen);
    lv_obj_set_size(outer, 300, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(outer, 10, 0);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);

    // Inner container with SIZE_CONTENT
    lv_obj_t* inner = lv_obj_create(outer);
    lv_obj_set_size(inner, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(inner, 8, 0);
    lv_obj_set_style_bg_color(inner, lv_color_hex(0x666666), 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, 0);

    // Label inside inner
    lv_obj_t* label = lv_label_create(inner);
    lv_label_set_text(label, "Nested Content");

    printf("Before lv_obj_update_layout():\n");
    print_obj_info("outer", outer);
    print_obj_info("inner", inner);
    print_obj_info("label", label);

    lv_obj_update_layout(screen);

    printf("After lv_obj_update_layout():\n");
    print_obj_info("outer", outer);
    print_obj_info("inner", inner);
    print_obj_info("label", label);

    int32_t outer_h = lv_obj_get_height(outer);
    int32_t inner_h = lv_obj_get_height(inner);
    int32_t label_h = lv_obj_get_height(label);

    // Inner should be: label + padding = label_h + 16
    // Outer should be: inner + padding = (label_h + 16) + 20
    int32_t expected_inner = label_h + 16;
    int32_t expected_outer = expected_inner + 20;

    bool inner_pass = inner_h >= expected_inner && inner_h > 0;
    bool outer_pass = outer_h >= expected_outer && outer_h > 0;

    printf("Inner RESULT: %s (inner_h=%d, expected >= %d)\n",
           inner_pass ? "PASS" : "FAIL", inner_h, expected_inner);
    printf("Outer RESULT: %s (outer_h=%d, expected >= %d)\n",
           outer_pass ? "PASS" : "FAIL", outer_h, expected_outer);

    lv_obj_delete(outer);
    return inner_pass && outer_pass;
}

// Test 4: Triple nesting with SIZE_CONTENT
static bool test_triple_nesting() {
    printf("\n=== TEST 4: Triple nested SIZE_CONTENT ===\n");

    lv_obj_t* screen = lv_screen_active();

    // Level 1: outermost
    lv_obj_t* level1 = lv_obj_create(screen);
    lv_obj_set_size(level1, 300, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(level1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(level1, 10, 0);

    // Level 2: middle
    lv_obj_t* level2 = lv_obj_create(level1);
    lv_obj_set_size(level2, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(level2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(level2, 8, 0);

    // Level 3: innermost
    lv_obj_t* level3 = lv_obj_create(level2);
    lv_obj_set_size(level3, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(level3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(level3, 6, 0);

    // Label at the deepest level
    lv_obj_t* label = lv_label_create(level3);
    lv_label_set_text(label, "Deep nested content with longer text");

    printf("Before lv_obj_update_layout():\n");
    print_obj_info("level1", level1);
    print_obj_info("level2", level2);
    print_obj_info("level3", level3);
    print_obj_info("label", label);

    lv_obj_update_layout(screen);

    printf("After lv_obj_update_layout():\n");
    print_obj_info("level1", level1);
    print_obj_info("level2", level2);
    print_obj_info("level3", level3);
    print_obj_info("label", label);

    int32_t l1_h = lv_obj_get_height(level1);
    int32_t l2_h = lv_obj_get_height(level2);
    int32_t l3_h = lv_obj_get_height(level3);
    int32_t label_h = lv_obj_get_height(label);

    // Expected sizes (each level adds its padding)
    int32_t exp_l3 = label_h + 12;  // 6 + 6 padding
    int32_t exp_l2 = exp_l3 + 16;   // 8 + 8 padding
    int32_t exp_l1 = exp_l2 + 20;   // 10 + 10 padding

    bool l3_pass = l3_h >= exp_l3 && l3_h > 0;
    bool l2_pass = l2_h >= exp_l2 && l2_h > 0;
    bool l1_pass = l1_h >= exp_l1 && l1_h > 0;

    printf("Level3 RESULT: %s (h=%d, expected >= %d)\n",
           l3_pass ? "PASS" : "FAIL", l3_h, exp_l3);
    printf("Level2 RESULT: %s (h=%d, expected >= %d)\n",
           l2_pass ? "PASS" : "FAIL", l2_h, exp_l2);
    printf("Level1 RESULT: %s (h=%d, expected >= %d)\n",
           l1_pass ? "PASS" : "FAIL", l1_h, exp_l1);

    lv_obj_delete(level1);
    return l1_pass && l2_pass && l3_pass;
}

// Test 5: SIZE_CONTENT with button row (common real-world case)
static bool test_button_row() {
    printf("\n=== TEST 5: Container with button row (SIZE_CONTENT) ===\n");

    lv_obj_t* screen = lv_screen_active();

    // Dialog-like container
    lv_obj_t* dialog = lv_obj_create(screen);
    lv_obj_set_size(dialog, 300, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(dialog, 16, 0);
    lv_obj_set_style_pad_gap(dialog, 12, 0);

    // Title
    lv_obj_t* title = lv_label_create(dialog);
    lv_label_set_text(title, "Confirm Action");

    // Message
    lv_obj_t* message = lv_label_create(dialog);
    lv_label_set_text(message, "Are you sure you want to proceed?");

    // Button row (also SIZE_CONTENT)
    lv_obj_t* button_row = lv_obj_create(dialog);
    lv_obj_set_size(button_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(button_row, 0, 0);
    lv_obj_set_style_pad_gap(button_row, 8, 0);
    lv_obj_set_style_bg_opa(button_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button_row, 0, 0);

    // Buttons
    lv_obj_t* btn_cancel = lv_button_create(button_row);
    lv_obj_t* btn_cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(btn_cancel_label, "Cancel");

    lv_obj_t* btn_ok = lv_button_create(button_row);
    lv_obj_t* btn_ok_label = lv_label_create(btn_ok);
    lv_label_set_text(btn_ok_label, "OK");

    printf("Before lv_obj_update_layout():\n");
    print_obj_info("dialog", dialog);
    print_obj_info("button_row", button_row);
    print_obj_info("btn_cancel", btn_cancel);

    lv_obj_update_layout(screen);

    printf("After lv_obj_update_layout():\n");
    print_obj_info("dialog", dialog);
    print_obj_info("button_row", button_row);
    print_obj_info("btn_cancel", btn_cancel);

    int32_t dialog_h = lv_obj_get_height(dialog);
    int32_t button_row_h = lv_obj_get_height(button_row);
    int32_t btn_h = lv_obj_get_height(btn_cancel);

    // Button row should contain buttons
    bool btn_row_pass = button_row_h >= btn_h && button_row_h > 0;
    // Dialog should contain all content
    bool dialog_pass = dialog_h > 0;

    printf("Button row RESULT: %s (h=%d, btn_h=%d)\n",
           btn_row_pass ? "PASS" : "FAIL", button_row_h, btn_h);
    printf("Dialog RESULT: %s (h=%d)\n",
           dialog_pass ? "PASS" : "FAIL", dialog_h);

    lv_obj_delete(dialog);
    return btn_row_pass && dialog_pass;
}

// Test 6: Multiple layout update calls (simulating real-world usage)
static bool test_multiple_layout_updates() {
    printf("\n=== TEST 6: Multiple layout updates ===\n");

    lv_obj_t* screen = lv_screen_active();

    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, 200, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container, 10, 0);

    lv_obj_t* label = lv_label_create(container);
    lv_label_set_text(label, "Initial");

    // First update
    lv_obj_update_layout(screen);
    int32_t h1 = lv_obj_get_height(container);
    printf("After 1st update: h=%d\n", h1);

    // Second update (should be same)
    lv_obj_update_layout(screen);
    int32_t h2 = lv_obj_get_height(container);
    printf("After 2nd update: h=%d\n", h2);

    // Change content
    lv_label_set_text(label, "This is a much longer text\nthat spans multiple lines\nfor testing purposes");
    lv_obj_update_layout(screen);
    int32_t h3 = lv_obj_get_height(container);
    printf("After content change: h=%d\n", h3);

    bool pass = (h1 == h2) && (h3 > h1) && (h1 > 0);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    lv_obj_delete(container);
    return pass;
}

int main() {
    printf("LV_SIZE_CONTENT Behavior Test Suite\n");
    printf("====================================\n");
    printf("LVGL Version: %d.%d.%d\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    init_lvgl_headless();

    int passed = 0;
    int total = 0;

    total++; if (test_simple_label_content()) passed++;
    total++; if (test_flex_container()) passed++;
    total++; if (test_nested_containers()) passed++;
    total++; if (test_triple_nesting()) passed++;
    total++; if (test_button_row()) passed++;
    total++; if (test_multiple_layout_updates()) passed++;

    printf("\n====================================\n");
    printf("SUMMARY: %d/%d tests passed\n", passed, total);
    printf("====================================\n");

    return (passed == total) ? 0 : 1;
}
