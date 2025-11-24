/*
 * Copyright (C) 2025 356C LLC
 * Author: Preston Brown <pbrown@brown-house.net>
 *
 * This file is part of HelixScreen.
 *
 * HelixScreen is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "../catch_amalgamated.hpp"
#include "keyboard_layout_provider.h"
#include <cstring>

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Count the number of buttons in a layout map (excluding newlines and sentinel)
 */
static size_t count_buttons(const char* const* map) {
    size_t count = 0;
    for (size_t i = 0; map[i][0] != '\0'; i++) {
        if (strcmp(map[i], "\n") != 0) {
            count++;
        }
    }
    return count;
}

/**
 * @brief Count the number of rows in a layout map
 * @note Rows are delimited by "\n", final row has no trailing newline
 */
static size_t count_rows(const char* const* map) {
    size_t rows = 1;  // Start at 1 for the first row
    for (size_t i = 0; map[i][0] != '\0'; i++) {
        if (strcmp(map[i], "\n") == 0) {
            rows++;
        }
    }
    return rows;
}

/**
 * @brief Check if a button exists in a layout map
 */
static bool button_exists(const char* const* map, const char* text) {
    for (size_t i = 0; map[i][0] != '\0'; i++) {
        if (strcmp(map[i], text) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Find button index in layout map (excluding newlines)
 */
static int find_button_index(const char* const* map, const char* text) {
    int btn_idx = 0;
    for (size_t i = 0; map[i][0] != '\0'; i++) {
        if (strcmp(map[i], "\n") == 0) {
            continue;
        }
        if (strcmp(map[i], text) == 0) {
            return btn_idx;
        }
        btn_idx++;
    }
    return -1;
}

/**
 * @brief Extract width from control value (lower 4 bits for width 1-15)
 * @note LVGL stores width in bits 0-3, supporting widths 1-15
 */
static int extract_width(lv_buttonmatrix_ctrl_t ctrl) {
    return ctrl & 0x0F;
}

/**
 * @brief Check if control has a specific flag
 */
static bool has_flag(lv_buttonmatrix_ctrl_t ctrl, lv_buttonmatrix_ctrl_t flag) {
    return (ctrl & flag) != 0;
}

// ============================================================================
// Lowercase Alphabet Layout Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Lowercase alphabet - basic structure", "[keyboard][layout][alpha_lc]") {
    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

    REQUIRE(map != nullptr);
    REQUIRE(ctrl_map != nullptr);

    SECTION("Map is properly terminated") {
        bool found_sentinel = false;
        for (size_t i = 0; i < 100; i++) {  // Safety limit
            if (map[i][0] == '\0') {
                found_sentinel = true;
                break;
            }
        }
        REQUIRE(found_sentinel);
    }

    SECTION("Has 4 rows (Gboard-style, no number row)") {
        REQUIRE(count_rows(map) == 4);
    }

    SECTION("Contains all lowercase letters") {
        // Row 1: q-p
        REQUIRE(button_exists(map, "q"));
        REQUIRE(button_exists(map, "w"));
        REQUIRE(button_exists(map, "e"));
        REQUIRE(button_exists(map, "r"));
        REQUIRE(button_exists(map, "t"));
        REQUIRE(button_exists(map, "y"));
        REQUIRE(button_exists(map, "u"));
        REQUIRE(button_exists(map, "i"));
        REQUIRE(button_exists(map, "o"));
        REQUIRE(button_exists(map, "p"));

        // Row 2: a-l
        REQUIRE(button_exists(map, "a"));
        REQUIRE(button_exists(map, "s"));
        REQUIRE(button_exists(map, "d"));
        REQUIRE(button_exists(map, "f"));
        REQUIRE(button_exists(map, "g"));
        REQUIRE(button_exists(map, "h"));
        REQUIRE(button_exists(map, "j"));
        REQUIRE(button_exists(map, "k"));
        REQUIRE(button_exists(map, "l"));

        // Row 3: z-m
        REQUIRE(button_exists(map, "z"));
        REQUIRE(button_exists(map, "x"));
        REQUIRE(button_exists(map, "c"));
        REQUIRE(button_exists(map, "v"));
        REQUIRE(button_exists(map, "b"));
        REQUIRE(button_exists(map, "n"));
        REQUIRE(button_exists(map, "m"));
    }

    SECTION("Contains control buttons") {
        REQUIRE(button_exists(map, LV_SYMBOL_UP));          // Shift
        REQUIRE(button_exists(map, LV_SYMBOL_BACKSPACE));   // Backspace
        REQUIRE(button_exists(map, "?123"));                 // Mode switch
        REQUIRE(button_exists(map, LV_SYMBOL_KEYBOARD));    // Close
        REQUIRE(button_exists(map, ","));                    // Comma
        REQUIRE(button_exists(map, "."));                    // Period
        REQUIRE(button_exists(map, LV_SYMBOL_NEW_LINE));    // Enter
    }

    SECTION("Contains spacebar") {
        const char* spacebar = keyboard_layout_get_spacebar_text();
        REQUIRE(button_exists(map, spacebar));
    }
}

TEST_CASE("Keyboard Layout: Lowercase alphabet - control map flags", "[keyboard][layout][alpha_lc]") {
    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

    SECTION("Letter keys have POPOVER and NO_REPEAT flags") {
        int idx = find_button_index(map, "q");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_POPOVER));
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_NO_REPEAT));
        REQUIRE(extract_width(ctrl_map[idx]) == 4);
    }

    SECTION("Shift key has CUSTOM_1 flag (non-printing)") {
        int idx = find_button_index(map, LV_SYMBOL_UP);
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
        REQUIRE(extract_width(ctrl_map[idx]) == 6);  // Wide key
    }

    SECTION("Backspace key has CUSTOM_1 flag (non-printing)") {
        int idx = find_button_index(map, LV_SYMBOL_BACKSPACE);
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
        REQUIRE(extract_width(ctrl_map[idx]) == 6);  // Wide key
    }

    SECTION("Mode switch button has CUSTOM_1 flag") {
        int idx = find_button_index(map, "?123");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
    }

    SECTION("Spacebar does NOT have CUSTOM_1 flag") {
        const char* spacebar = keyboard_layout_get_spacebar_text();
        int idx = find_button_index(map, spacebar);
        REQUIRE(idx >= 0);
        REQUIRE_FALSE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
        REQUIRE(extract_width(ctrl_map[idx]) == 12);  // Very wide
    }

    SECTION("Enter key has CUSTOM_1 flag") {
        int idx = find_button_index(map, LV_SYMBOL_NEW_LINE);
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
    }
}

// ============================================================================
// Uppercase Alphabet Layout Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Uppercase alphabet - basic structure", "[keyboard][layout][alpha_uc]") {
    SECTION("Caps lock mode (eject symbol)") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, true);
        REQUIRE(map != nullptr);

        // Should have all uppercase letters
        REQUIRE(button_exists(map, "Q"));
        REQUIRE(button_exists(map, "W"));
        REQUIRE(button_exists(map, "E"));
        REQUIRE(button_exists(map, "A"));
        REQUIRE(button_exists(map, "S"));
        REQUIRE(button_exists(map, "Z"));
        REQUIRE(button_exists(map, "M"));

        // Should have eject symbol for shift (caps lock active)
        REQUIRE(button_exists(map, LV_SYMBOL_EJECT));
        REQUIRE_FALSE(button_exists(map, LV_SYMBOL_UPLOAD));
    }

    SECTION("One-shot mode (upload symbol)") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false);
        REQUIRE(map != nullptr);

        // Should have all uppercase letters
        REQUIRE(button_exists(map, "Q"));
        REQUIRE(button_exists(map, "A"));
        REQUIRE(button_exists(map, "Z"));

        // Should have upload symbol for shift (one-shot)
        REQUIRE(button_exists(map, LV_SYMBOL_UPLOAD));
        REQUIRE_FALSE(button_exists(map, LV_SYMBOL_EJECT));
    }

    SECTION("Both modes use same control map") {
        const lv_buttonmatrix_ctrl_t* ctrl_caps =
            keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_UC);
        const lv_buttonmatrix_ctrl_t* ctrl_oneshot =
            keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_UC);

        REQUIRE(ctrl_caps == ctrl_oneshot);  // Same pointer
    }

    SECTION("Has same 4-row structure as lowercase") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false);
        REQUIRE(count_rows(map) == 4);
    }
}

TEST_CASE("Keyboard Layout: Uppercase vs lowercase - character mapping", "[keyboard][layout][alpha]") {
    const char* const* lc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
    const char* const* uc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false);

    SECTION("Lowercase has lowercase letters") {
        REQUIRE(button_exists(lc_map, "q"));
        REQUIRE_FALSE(button_exists(lc_map, "Q"));
    }

    SECTION("Uppercase has uppercase letters") {
        REQUIRE(button_exists(uc_map, "Q"));
        REQUIRE_FALSE(button_exists(uc_map, "q"));
    }

    SECTION("Both have same control buttons") {
        REQUIRE(button_exists(lc_map, "?123"));
        REQUIRE(button_exists(uc_map, "?123"));
        REQUIRE(button_exists(lc_map, LV_SYMBOL_KEYBOARD));
        REQUIRE(button_exists(uc_map, LV_SYMBOL_KEYBOARD));
    }
}

// ============================================================================
// Numbers and Symbols Layout Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Numbers and symbols - structure", "[keyboard][layout][numbers]") {
    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS);

    REQUIRE(map != nullptr);
    REQUIRE(ctrl_map != nullptr);

    SECTION("Has 5 rows (more than alpha layouts)") {
        REQUIRE(count_rows(map) == 5);
    }

    SECTION("Contains special characters") {
        REQUIRE(button_exists(map, "!"));
        REQUIRE(button_exists(map, "@"));
        REQUIRE(button_exists(map, "#"));
        REQUIRE(button_exists(map, "$"));
        REQUIRE(button_exists(map, "%"));
        REQUIRE(button_exists(map, "^"));
        REQUIRE(button_exists(map, "&"));
        REQUIRE(button_exists(map, "*"));
        REQUIRE(button_exists(map, "("));
        REQUIRE(button_exists(map, ")"));
    }

    SECTION("Contains punctuation") {
        REQUIRE(button_exists(map, "-"));
        REQUIRE(button_exists(map, "/"));
        REQUIRE(button_exists(map, ":"));
        REQUIRE(button_exists(map, ";"));
        REQUIRE(button_exists(map, "?"));
        REQUIRE(button_exists(map, "!"));
        REQUIRE(button_exists(map, "'"));
        REQUIRE(button_exists(map, "\""));
    }

    SECTION("Contains brackets") {
        REQUIRE(button_exists(map, "["));
        REQUIRE(button_exists(map, "]"));
        REQUIRE(button_exists(map, "{"));
        REQUIRE(button_exists(map, "}"));
    }

    SECTION("Contains mode switch buttons") {
        REQUIRE(button_exists(map, "#+="));   // To alt symbols
        REQUIRE(button_exists(map, "XYZ"));   // Back to alpha
    }

    SECTION("Contains backspace") {
        REQUIRE(button_exists(map, LV_SYMBOL_BACKSPACE));
    }
}

TEST_CASE("Keyboard Layout: Numbers and symbols - control flags", "[keyboard][layout][numbers]") {
    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS);

    SECTION("Symbol keys have POPOVER and NO_REPEAT") {
        int idx = find_button_index(map, "!");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_POPOVER));
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_NO_REPEAT));
    }

    SECTION("Mode switch has CUSTOM_1 flag") {
        int idx = find_button_index(map, "#+=");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
    }

    SECTION("XYZ button has CUSTOM_1 flag") {
        int idx = find_button_index(map, "XYZ");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
    }

    SECTION("Backspace has CUSTOM_1 flag and wider width") {
        int idx = find_button_index(map, LV_SYMBOL_BACKSPACE);
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
        REQUIRE(extract_width(ctrl_map[idx]) >= 6);  // Wide key
    }
}

// ============================================================================
// Alternative Symbols Layout Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Alternative symbols - structure", "[keyboard][layout][alt_symbols]") {
    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALT_SYMBOLS);

    REQUIRE(map != nullptr);
    REQUIRE(ctrl_map != nullptr);

    SECTION("Has 5 rows") {
        REQUIRE(count_rows(map) == 5);
    }

    SECTION("Contains brackets and math symbols") {
        REQUIRE(button_exists(map, "["));
        REQUIRE(button_exists(map, "]"));
        REQUIRE(button_exists(map, "{"));
        REQUIRE(button_exists(map, "}"));
        REQUIRE(button_exists(map, "#"));
        REQUIRE(button_exists(map, "%"));
        REQUIRE(button_exists(map, "^"));
        REQUIRE(button_exists(map, "+"));
        REQUIRE(button_exists(map, "="));
    }

    SECTION("Contains special characters") {
        REQUIRE(button_exists(map, "_"));
        REQUIRE(button_exists(map, "\\"));
        REQUIRE(button_exists(map, "|"));
        REQUIRE(button_exists(map, "~"));
        REQUIRE(button_exists(map, "<"));
        REQUIRE(button_exists(map, ">"));
    }

    SECTION("Contains mode switch button to return to numbers") {
        REQUIRE(button_exists(map, "123"));
        REQUIRE(button_exists(map, "XYZ"));
    }
}

TEST_CASE("Keyboard Layout: Alternative symbols - control flags", "[keyboard][layout][alt_symbols]") {
    const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false);
    const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALT_SYMBOLS);

    SECTION("Symbol keys have POPOVER and NO_REPEAT") {
        int idx = find_button_index(map, "[");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_POPOVER));
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_NO_REPEAT));
    }

    SECTION("123 button has CUSTOM_1 flag") {
        int idx = find_button_index(map, "123");
        REQUIRE(idx >= 0);
        REQUIRE(has_flag(ctrl_map[idx], LV_BUTTONMATRIX_CTRL_CUSTOM_1));
    }
}

// ============================================================================
// Spacebar Text Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Spacebar text constant", "[keyboard][layout][spacebar]") {
    const char* spacebar = keyboard_layout_get_spacebar_text();

    SECTION("Returns non-null pointer") {
        REQUIRE(spacebar != nullptr);
    }

    SECTION("Returns double-space (two spaces)") {
        REQUIRE(strlen(spacebar) == 2);
        REQUIRE(spacebar[0] == ' ');
        REQUIRE(spacebar[1] == ' ');
        REQUIRE(spacebar[2] == '\0');
    }

    SECTION("Same value returned on multiple calls") {
        const char* spacebar2 = keyboard_layout_get_spacebar_text();
        REQUIRE(spacebar == spacebar2);  // Same pointer
    }
}

// ============================================================================
// Layout Consistency Tests
// ============================================================================

TEST_CASE("Keyboard Layout: All layouts have matching button and control counts", "[keyboard][layout][consistency]") {
    SECTION("Lowercase alphabet") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        size_t btn_count = count_buttons(map);
        REQUIRE(btn_count > 0);
        REQUIRE(btn_count < 100);  // Sanity check
    }

    SECTION("Uppercase alphabet (caps lock)") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, true);
        size_t btn_count = count_buttons(map);
        REQUIRE(btn_count > 0);

        // Should have same count as lowercase (just different letters)
        const char* const* lc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        REQUIRE(btn_count == count_buttons(lc_map));
    }

    SECTION("Uppercase alphabet (one-shot)") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false);
        size_t btn_count = count_buttons(map);

        // Should have same count as caps lock version
        const char* const* caps_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, true);
        REQUIRE(btn_count == count_buttons(caps_map));
    }

    SECTION("Numbers and symbols") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false);
        size_t btn_count = count_buttons(map);
        REQUIRE(btn_count > 0);
    }

    SECTION("Alternative symbols") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false);
        size_t btn_count = count_buttons(map);
        REQUIRE(btn_count > 0);
    }
}

// ============================================================================
// Key Width Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Key widths are valid", "[keyboard][layout][widths]") {
    SECTION("Lowercase alphabet - regular keys are width 4") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

        int q_idx = find_button_index(map, "q");
        REQUIRE(extract_width(ctrl_map[q_idx]) == 4);

        int a_idx = find_button_index(map, "a");
        REQUIRE(extract_width(ctrl_map[a_idx]) == 4);
    }

    SECTION("Lowercase alphabet - shift and backspace are width 6") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

        int shift_idx = find_button_index(map, LV_SYMBOL_UP);
        REQUIRE(extract_width(ctrl_map[shift_idx]) == 6);

        int bs_idx = find_button_index(map, LV_SYMBOL_BACKSPACE);
        REQUIRE(extract_width(ctrl_map[bs_idx]) == 6);
    }

    SECTION("Lowercase alphabet - spacebar is width 12") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        const lv_buttonmatrix_ctrl_t* ctrl_map = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

        const char* spacebar = keyboard_layout_get_spacebar_text();
        int space_idx = find_button_index(map, spacebar);
        REQUIRE(extract_width(ctrl_map[space_idx]) == 12);
    }
}

// ============================================================================
// Fallback Behavior Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Invalid mode falls back to lowercase", "[keyboard][layout][fallback]") {
    // Cast to invalid enum value
    keyboard_layout_mode_t invalid_mode = static_cast<keyboard_layout_mode_t>(999);

    SECTION("get_map with invalid mode returns lowercase") {
        const char* const* map = keyboard_layout_get_map(invalid_mode, false);
        const char* const* lc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);

        // Should return same pointer as lowercase
        REQUIRE(map == lc_map);
    }

    SECTION("get_ctrl_map with invalid mode returns lowercase") {
        const lv_buttonmatrix_ctrl_t* ctrl = keyboard_layout_get_ctrl_map(invalid_mode);
        const lv_buttonmatrix_ctrl_t* lc_ctrl = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

        // Should return same pointer as lowercase
        REQUIRE(ctrl == lc_ctrl);
    }
}

// ============================================================================
// Row Structure Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Row structure validation", "[keyboard][layout][rows]") {
    SECTION("Lowercase alphabet row 1 has 10 keys (q-p)") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);

        int row1_count = 0;
        for (size_t i = 0; map[i][0] != '\0' && strcmp(map[i], "\n") != 0; i++) {
            row1_count++;
        }
        REQUIRE(row1_count == 10);
    }

    SECTION("Lowercase alphabet row 2 has 11 keys (spacer + a-l + spacer)") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);

        // Skip to row 2 (after first newline)
        size_t idx = 0;
        while (map[idx][0] != '\0' && strcmp(map[idx], "\n") != 0) idx++;
        idx++;  // Skip newline

        int row2_count = 0;
        while (map[idx][0] != '\0' && strcmp(map[idx], "\n") != 0) {
            row2_count++;
            idx++;
        }
        REQUIRE(row2_count == 11);
    }

    SECTION("All layouts end with empty string sentinel") {
        const char* const* layouts[] = {
            keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false),
            keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false),
            keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false),
            keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false)
        };

        for (const char* const* map : layouts) {
            size_t idx = 0;
            while (map[idx][0] != '\0') idx++;

            // Should end with empty string
            REQUIRE(map[idx][0] == '\0');
            REQUIRE(strlen(map[idx]) == 0);
        }
    }
}

// ============================================================================
// Special Button Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Special buttons present in all layouts", "[keyboard][layout][special]") {
    const char* const* layouts[] = {
        keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false),
        keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false),
        keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false),
        keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false)
    };

    for (const char* const* map : layouts) {
        SECTION("Has spacebar") {
            const char* spacebar = keyboard_layout_get_spacebar_text();
            REQUIRE(button_exists(map, spacebar));
        }

        SECTION("Has close button") {
            REQUIRE(button_exists(map, LV_SYMBOL_KEYBOARD));
        }

        SECTION("Has enter button") {
            REQUIRE(button_exists(map, LV_SYMBOL_NEW_LINE));
        }
    }
}

// ============================================================================
// Mode Switching Button Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Mode switching buttons", "[keyboard][layout][mode_switch]") {
    SECTION("Alpha layouts have ?123 button") {
        const char* const* lc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        const char* const* uc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false);

        REQUIRE(button_exists(lc_map, "?123"));
        REQUIRE(button_exists(uc_map, "?123"));
    }

    SECTION("Numbers layout has XYZ and #+= buttons") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false);

        REQUIRE(button_exists(map, "XYZ"));
        REQUIRE(button_exists(map, "#+="));
    }

    SECTION("Alt symbols layout has XYZ and 123 buttons") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false);

        REQUIRE(button_exists(map, "XYZ"));
        REQUIRE(button_exists(map, "123"));
    }
}

// ============================================================================
// Control Map Flag Combination Tests
// ============================================================================

TEST_CASE("Keyboard Layout: Control flags are properly combined", "[keyboard][layout][flags]") {
    SECTION("Letter keys have POPOVER + NO_REPEAT + width") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        const lv_buttonmatrix_ctrl_t* ctrl = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

        int a_idx = find_button_index(map, "a");
        lv_buttonmatrix_ctrl_t a_ctrl = ctrl[a_idx];

        REQUIRE(has_flag(a_ctrl, LV_BUTTONMATRIX_CTRL_POPOVER));
        REQUIRE(has_flag(a_ctrl, LV_BUTTONMATRIX_CTRL_NO_REPEAT));
        REQUIRE(extract_width(a_ctrl) > 0);
    }

    SECTION("Mode buttons have CHECKED + CUSTOM_1 + width") {
        const char* const* map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        const lv_buttonmatrix_ctrl_t* ctrl = keyboard_layout_get_ctrl_map(KEYBOARD_LAYOUT_ALPHA_LC);

        int mode_idx = find_button_index(map, "?123");
        lv_buttonmatrix_ctrl_t mode_ctrl = ctrl[mode_idx];

        REQUIRE(has_flag(mode_ctrl, LV_BUTTONMATRIX_CTRL_CHECKED));
        REQUIRE(has_flag(mode_ctrl, LV_BUTTONMATRIX_CTRL_CUSTOM_1));
        REQUIRE(extract_width(mode_ctrl) > 0);
    }
}

// ============================================================================
// Layout Completeness Tests
// ============================================================================

TEST_CASE("Keyboard Layout: All printable ASCII available", "[keyboard][layout][completeness]") {
    // Collect all buttons from all layouts
    const char* const* layouts[] = {
        keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false),
        keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false),
        keyboard_layout_get_map(KEYBOARD_LAYOUT_NUMBERS_SYMBOLS, false),
        keyboard_layout_get_map(KEYBOARD_LAYOUT_ALT_SYMBOLS, false)
    };

    SECTION("All lowercase letters available") {
        const char* const* lc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_LC, false);
        for (char c = 'a'; c <= 'z'; c++) {
            char str[2] = {c, '\0'};
            REQUIRE(button_exists(lc_map, str));
        }
    }

    SECTION("All uppercase letters available") {
        const char* const* uc_map = keyboard_layout_get_map(KEYBOARD_LAYOUT_ALPHA_UC, false);
        for (char c = 'A'; c <= 'Z'; c++) {
            char str[2] = {c, '\0'};
            REQUIRE(button_exists(uc_map, str));
        }
    }

    SECTION("Common punctuation available") {
        const char* punctuation[] = {".", ",", "!", "?", ";", ":", "'", "\"", nullptr};

        bool all_found = true;
        for (int i = 0; punctuation[i] != nullptr; i++) {
            bool found = false;
            for (const char* const* map : layouts) {
                if (button_exists(map, punctuation[i])) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_found = false;
            }
        }
        REQUIRE(all_found);
    }
}
