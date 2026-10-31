// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_button.cpp
 * @brief Unit tests for ui_button XML widget
 *
 * Tests bind_icon attribute functionality and other ui_button features.
 */

#include "ui_button.h"
#include "ui_fonts.h"
#include "ui_icon_codepoints.h"
#include "ui_update_queue.h"
#include "ui_variant.h"

#include "../test_fixtures.h"
#include "../test_helpers/scoped_breakpoint.h"
#include "../test_helpers/scoped_theme_mode.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../test_helpers/widget_in_tree.h"
#include "../ui_test_utils.h"
#include "theme_manager.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Fixture with ui_button registered
// ============================================================================

class UiButtonTestFixture : public XMLTestFixture {
  public:
    UiButtonTestFixture() : XMLTestFixture() {
        // Initialize icon name buffer with initial value
        strncpy(icon_buf_, "light", sizeof(icon_buf_) - 1);
        icon_buf_[sizeof(icon_buf_) - 1] = '\0';

        // Register a test subject for bind_icon tests
        lv_subject_init_string(&icon_subject_, icon_buf_, nullptr, sizeof(icon_buf_), icon_buf_);
        lv_xml_register_subject(nullptr, "test_icon_subject", &icon_subject_);

        // Register a test subject for bind_text tests
        lv_subject_init_string(&text_subject_, text_buf_, nullptr, sizeof(text_buf_), "Close");
        lv_xml_register_subject(nullptr, "test_text_subject", &text_subject_);

        spdlog::debug("[UiButtonTestFixture] Initialized with test subjects");
    }

    ~UiButtonTestFixture() override {
        lv_subject_deinit(&icon_subject_);
        lv_subject_deinit(&text_subject_);
        spdlog::debug("[UiButtonTestFixture] Cleaned up");
    }

    /**
     * @brief Get the test icon subject for bind_icon tests
     */
    lv_subject_t* icon_subject() {
        return &icon_subject_;
    }

    /**
     * @brief Set the icon subject value
     */
    void set_icon_name(const char* name) {
        lv_subject_copy_string(&icon_subject_, name);
    }

    /**
     * @brief Set the text subject value
     */
    void set_text(const char* text) {
        lv_subject_copy_string(&text_subject_, text);
    }

    /**
     * @brief Find the label child of a button (first lv_label child)
     */
    lv_obj_t* find_button_label(lv_obj_t* btn) {
        uint32_t count = lv_obj_get_child_count(btn);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* child = lv_obj_get_child(btn, i);
            if (lv_obj_check_type(child, &lv_label_class)) {
                return child;
            }
        }
        return nullptr;
    }

    /**
     * @brief Index of the first lv_label child whose text equals @p text
     * @return Child index, or -1 if no such child exists
     */
    int32_t find_child_index_with_text(lv_obj_t* btn, const char* text) {
        if (!text) {
            return -1;
        }
        uint32_t count = lv_obj_get_child_count(btn);
        for (uint32_t i = 0; i < count; i++) {
            lv_obj_t* child = lv_obj_get_child(btn, i);
            if (!lv_obj_check_type(child, &lv_label_class)) {
                continue;
            }
            const char* child_text = lv_label_get_text(child);
            if (child_text && strcmp(child_text, text) == 0) {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }

    /**
     * @brief Create a ui_button via XML with given attributes
     * @param attrs NULL-terminated array of key-value attribute pairs
     * @return Created button, or nullptr on failure
     */
    lv_obj_t* create_button(const char** attrs) {
        return static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ui_button", attrs));
    }

  protected:
    lv_subject_t icon_subject_;
    char icon_buf_[64];

    lv_subject_t text_subject_;
    char text_buf_[64];
};

// ============================================================================
// bind_icon Tests
// ============================================================================

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button can be created via XML",
                 "[ui_button][xml][quick]") {
    // Simple test to verify button creation works
    const char* attrs[] = {"text", "Test", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    REQUIRE(helix::test::widget_in_tree(btn));
}

// A solid variant is an accent fill: the label starts from the palette text
// colour and shifts toward its own pole only as far as 4:1 contrast needs. On
// fills where no tint on that side can reach 4:1 (the light theme's dark text
// on its dark navy primary), the label falls back to the readable pole.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button solid variant label is readable on its fill",
                 "[ui_button][contrast][quick][1648]") {
    ScopedThemeMode mode;
    for (bool dark : {false, true}) {
        mode.set(dark);
        for (const char* variant : {"primary", "warning", "success"}) {
            const char* attrs[] = {"text", "Go", "variant", variant, nullptr};
            lv_obj_t* btn = create_button(attrs);
            REQUIRE(btn != nullptr);
            helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

            lv_obj_t* label = nullptr;
            for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
                lv_obj_t* child = lv_obj_get_child(btn, i);
                if (lv_obj_check_type(child, &lv_label_class)) {
                    label = child;
                    break;
                }
            }
            REQUIRE(label != nullptr);

            lv_color_t fill = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
            lv_color_t text = lv_obj_get_style_text_color(label, LV_PART_MAIN);
            lv_color_t palette_text = theme_manager_get_color("text");
            CAPTURE(dark, variant, lv_color_to_u32(fill) & 0xFFFFFF,
                    lv_color_to_u32(text) & 0xFFFFFF, lv_color_to_u32(palette_text) & 0xFFFFFF);
            CHECK(wcag::contrast(text, fill) >= 4.0);
            // The shift stays on the palette text's side of the fill whenever
            // that side can reach 4:1 at all; flipping to the opposite pole is
            // what a pure black-or-white picker does.
            if (wcag::own_pole_reaches_4_1(palette_text, fill)) {
                CHECK((wcag::luminance(text) > wcag::luminance(fill)) ==
                      (wcag::luminance(palette_text) > wcag::luminance(fill)));
            }
        }
    }
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button bind_icon basic creation works",
                 "[ui_button][xml][quick]") {
    // Just test that we can create a button with bind_icon without hanging
    const char* attrs[] = {"text", "Test", "bind_icon", "test_icon_subject", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    REQUIRE(helix::test::widget_in_tree(btn));
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button bind_icon updates icon from subject",
                 "[ui_button][xml][slow]") { // Marked .slow - hangs in CI environment
    // Create button with bind_icon attribute bound to test subject
    const char* attrs[] = {"text", "Test", "bind_icon", "test_icon_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    // Process LVGL to apply bindings - use shorter time
    process_lvgl(10);

    // Check child count
    uint32_t child_count = lv_obj_get_child_count(btn);
    REQUIRE(child_count >= 2); // Should have label + icon

    // Find the icon child
    lv_obj_t* icon = nullptr;
    const char* expected_codepoint = helix::ui::icon::lookup_codepoint("light");

    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* text = lv_label_get_text(child);
            if (text && expected_codepoint && strcmp(text, expected_codepoint) == 0) {
                icon = child;
                break;
            }
        }
    }

    REQUIRE(icon != nullptr);
    INFO("Initial icon should be 'light' codepoint");
    REQUIRE(strcmp(lv_label_get_text(icon), expected_codepoint) == 0);

    // Update subject to different icon
    set_icon_name("light_off");
    process_lvgl(10);

    // Verify icon changed
    const char* new_expected = helix::ui::icon::lookup_codepoint("light_off");
    INFO("Icon should update to 'light_off' codepoint after subject change");
    REQUIRE(strcmp(lv_label_get_text(icon), new_expected) == 0);
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button bind_icon creates icon if none exists",
                 "[ui_button][xml][slow]") { // Marked .slow - hangs in CI environment
    // Create button with NO initial icon, but with bind_icon
    const char* attrs[] = {"text", "No Icon", "bind_icon", "test_icon_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    // Process LVGL to apply bindings
    process_lvgl(50);

    // Should have created an icon from the subject value
    lv_obj_t* icon = nullptr;
    uint32_t child_count = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* text = lv_label_get_text(child);
            const char* expected_codepoint = helix::ui::icon::lookup_codepoint("light");
            if (text && expected_codepoint && strcmp(text, expected_codepoint) == 0) {
                icon = child;
                break;
            }
        }
    }

    REQUIRE(icon != nullptr);
    INFO("bind_icon should create icon widget when none exists");
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button bind_icon handles missing subject gracefully",
                 "[ui_button][xml][slow]") { // Marked .slow - hangs in CI environment
    // Create button with bind_icon pointing to non-existent subject
    const char* attrs[] = {"text", "Test", "bind_icon", "nonexistent_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    // Should not crash, button should still be created
    process_lvgl(50);

    // Button should exist and be valid
    REQUIRE(helix::test::widget_in_tree(btn));
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button bind_icon handles empty string value",
                 "[ui_button][xml][slow]") { // Marked .slow - hangs in CI environment
    // Set subject to empty string first
    set_icon_name("");

    const char* attrs[] = {"text", "Test", "bind_icon", "test_icon_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    // Should not crash
    process_lvgl(50);

    // Button should exist and be valid
    REQUIRE(helix::test::widget_in_tree(btn));
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_icon works with existing icon attribute overrides",
                 "[ui_button][xml][slow]") { // Marked .slow - hangs in CI environment
    // Create button with both static icon and bind_icon
    // bind_icon should override the static icon
    const char* attrs[] = {"text", "Test", "icon", "settings", "bind_icon", "test_icon_subject",
                           nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    process_lvgl(50);

    // Find icon - should show "light" (from subject), not "settings" (from static attr)
    lv_obj_t* icon = nullptr;
    uint32_t child_count = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* text = lv_label_get_text(child);
            // Check for icon codepoint (either could be valid during creation)
            const char* light_cp = helix::ui::icon::lookup_codepoint("light");
            if (text && light_cp && strcmp(text, light_cp) == 0) {
                icon = child;
                break;
            }
        }
    }

    // After bind_icon processing, icon should show subject value
    REQUIRE(icon != nullptr);
    INFO("bind_icon should override static icon attribute");
    REQUIRE(strcmp(lv_label_get_text(icon), helix::ui::icon::lookup_codepoint("light")) == 0);
}

// ============================================================================
// bind_op_state Tests — int subject drives idle/busy/done icon-slot rendering
//
// 0 = idle  → original icon glyph visible, no spinner
// 1 = busy  → glyph label hidden, animated spinner arc visible in icon slot
// 2 = done  → "check" glyph visible, no spinner
// ============================================================================

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button bind_op_state renders idle/busy/done states",
                 "[ui_button][xml][op_state][quick]") {
    // Dedicated int subject for op-state binding (0 idle / 1 busy / 2 done)
    lv_subject_t op_subject;
    lv_subject_init_int(&op_subject, 0);
    lv_xml_register_subject(nullptr, "test_op_subject", &op_subject);

    const char* idle_cp = helix::ui::icon::lookup_codepoint("light");
    const char* check_cp = helix::ui::icon::lookup_codepoint("check");
    REQUIRE(idle_cp != nullptr);
    REQUIRE(check_cp != nullptr); // 'check' glyph must exist in the MDI font

    const char* attrs[] = {"text",          "Light",           "icon", "light",
                           "bind_op_state", "test_op_subject", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    process_lvgl(10);

    // The widget exposes its icon glyph label and (lazily-created) spinner arc.
    lv_obj_t* icon = ui_button_get_icon(btn);
    REQUIRE(icon != nullptr);

    // --- State 0: idle — original icon glyph visible, spinner hidden/absent ---
    INFO("idle: icon glyph should be the original 'light' codepoint");
    REQUIRE(strcmp(lv_label_get_text(icon), idle_cp) == 0);
    REQUIRE_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    {
        lv_obj_t* arc = ui_button_get_op_spinner(btn);
        // Spinner may not be created yet, or if created must be hidden in idle.
        if (arc) {
            REQUIRE(lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));
        }
    }

    // --- State 1: busy — glyph label hidden, spinner arc visible ---
    lv_subject_set_int(&op_subject, 1);
    process_lvgl(10);
    INFO("busy: glyph label must be hidden");
    REQUIRE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t* arc = ui_button_get_op_spinner(btn);
    INFO("busy: spinner arc must exist and be visible");
    REQUIRE(arc != nullptr);
    REQUIRE(lv_obj_check_type(arc, &lv_arc_class));
    REQUIRE_FALSE(lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));

    // --- State 2: done — 'check' glyph visible, spinner hidden ---
    lv_subject_set_int(&op_subject, 2);
    process_lvgl(10);
    INFO("done: icon glyph should be the 'check' codepoint");
    REQUIRE_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(strcmp(lv_label_get_text(icon), check_cp) == 0);
    REQUIRE(lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));

    // --- Back to idle — original glyph restored, spinner hidden ---
    lv_subject_set_int(&op_subject, 0);
    process_lvgl(10);
    INFO("idle (return): original 'light' glyph restored");
    REQUIRE_FALSE(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    REQUIRE(strcmp(lv_label_get_text(icon), idle_cp) == 0);
    REQUIRE(lv_obj_has_flag(arc, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(btn);
    process_lvgl(5);
    lv_subject_deinit(&op_subject);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button without bind_op_state has no spinner and behaves normally",
                 "[ui_button][xml][op_state][quick]") {
    // Backward compatibility: a plain icon button must not gain a spinner.
    const char* attrs[] = {"text", "Light", "icon", "light", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    process_lvgl(10);

    REQUIRE(ui_button_get_op_spinner(btn) == nullptr);

    lv_obj_t* icon = ui_button_get_icon(btn);
    REQUIRE(icon != nullptr);
    REQUIRE(strcmp(lv_label_get_text(icon), helix::ui::icon::lookup_codepoint("light")) == 0);
}

// ============================================================================
// bind_text Tests — bind_text always resolves as subject name
// ============================================================================

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_text with non-existent subject falls back to literal",
                 "[ui_button][xml][bind_text][quick]") {
    // No subject named "Save" exists — falls back to literal text
    const char* attrs[] = {"bind_text", "Save", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    process_lvgl(10);

    lv_obj_t* label = find_button_label(btn);
    REQUIRE(label != nullptr);
    REQUIRE(strcmp(lv_label_get_text(label), "Save") == 0);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_text resolves subject by name and reacts to changes",
                 "[ui_button][xml][bind_text][quick]") {
    // bind_text always tries to resolve as a subject name (LVGL standard)
    const char* attrs[] = {"bind_text", "test_text_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    process_lvgl(10);

    // Label should show initial subject value
    lv_obj_t* label = find_button_label(btn);
    REQUIRE(label != nullptr);
    REQUIRE(strcmp(lv_label_get_text(label), "Close") == 0);

    // Update subject — label should react
    set_text("Save");
    process_lvgl(10);
    REQUIRE(strcmp(lv_label_get_text(label), "Save") == 0);

    // Change back
    set_text("Close");
    process_lvgl(10);
    REQUIRE(strcmp(lv_label_get_text(label), "Close") == 0);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_text with @ prefix strips it and resolves subject",
                 "[ui_button][xml][bind_text][quick]") {
    // @ prefix is stripped for backward compatibility with text="@subject" convention
    const char* attrs[] = {"bind_text", "@test_text_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    process_lvgl(10);

    lv_obj_t* label = find_button_label(btn);
    REQUIRE(label != nullptr);
    REQUIRE(strcmp(lv_label_get_text(label), "Close") == 0);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_text with @ prefix for missing subject uses name as fallback",
                 "[ui_button][xml][bind_text][quick]") {
    const char* attrs[] = {"bind_text", "@nonexistent_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    process_lvgl(10);

    // Should gracefully fall back to using the subject name (without @) as literal text
    lv_obj_t* label = find_button_label(btn);
    REQUIRE(label != nullptr);
    REQUIRE(strcmp(lv_label_get_text(label), "nonexistent_subject") == 0);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_text with text attr creates label then bind_text binds it",
                 "[ui_button][xml][bind_text][quick]") {
    // text= creates the label during create, bind_text= binds it during apply
    const char* attrs[] = {"text", "Initial", "bind_text", "test_text_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    process_lvgl(10);

    // bind_text should have overridden the initial text with subject value
    lv_obj_t* label = find_button_label(btn);
    REQUIRE(label != nullptr);
    REQUIRE(strcmp(lv_label_get_text(label), "Close") == 0);
}

// ============================================================================
// Deferred contrast after delete (#924)
//
// Integration-level coverage of the deferred-contrast path: a STYLE_CHANGED
// event defers update_button_text_contrast through the UpdateQueue; if the
// button is destroyed before the queue drains, the deferred recompute must be
// skipped rather than crashing. test_ui_button_defer_reuse.cpp covers the same
// path through the anonymous-namespace internals.
// ============================================================================

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button deferred contrast on a deleted button is safely skipped",
                 "[ui_button][crash][quick]") {
    const char* attrs[] = {"text", "Doomed", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    // Drain any create-time deferred contrast passes first.
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    // STYLE_CHANGED enqueues a deferred contrast recompute; destroy the button
    // before the queue drains. helix::test::widget_in_tree() must reject the stale pointer.
    lv_obj_send_event(btn, LV_EVENT_STYLE_CHANGED, nullptr);
    lv_obj_delete(btn);

    // Must not crash; the captured (now invalid) widget pointer is skipped.
    REQUIRE_NOTHROW(
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance()));
}

// ============================================================================
// icon_position + bind_icon Tests
// ============================================================================
//
// A button that declares icon_position="top"/"bottom" but has no static icon=
// attribute gets its icon created later, by the bind_icon apply pass. That pass
// must honour the declared position rather than forcing a row.

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_icon honours icon_position=top without a static icon",
                 "[ui_button][xml][quick]") {
    const char* attrs[] = {"text",          "Light", "bind_icon", "test_icon_subject",
                           "icon_position", "top",   nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    process_lvgl(10);

    // Icon must exist and the button must stack its children vertically.
    const int32_t icon_idx =
        find_child_index_with_text(btn, helix::ui::icon::lookup_codepoint("light"));
    const int32_t label_idx = find_child_index_with_text(btn, "Light");
    INFO("icon child index=" << icon_idx << " label child index=" << label_idx);
    REQUIRE(icon_idx >= 0);
    REQUIRE(label_idx >= 0);

    INFO("icon_position=top must produce a COLUMN flex flow, not a ROW");
    REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);

    INFO("icon must be stacked above the label");
    REQUIRE(icon_idx < label_idx);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_icon honours icon_position=bottom without a static icon",
                 "[ui_button][xml][quick]") {
    const char* attrs[] = {"text",          "Light",  "bind_icon", "test_icon_subject",
                           "icon_position", "bottom", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    process_lvgl(10);

    const int32_t icon_idx =
        find_child_index_with_text(btn, helix::ui::icon::lookup_codepoint("light"));
    const int32_t label_idx = find_child_index_with_text(btn, "Light");
    REQUIRE(icon_idx >= 0);
    REQUIRE(label_idx >= 0);

    REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);

    INFO("icon must be stacked below the label");
    REQUIRE(icon_idx > label_idx);
}

// Regression guard: the default (no icon_position) must keep producing a row
// with the icon to the LEFT of the label, exactly as before.
TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_icon without icon_position still lays out as a row",
                 "[ui_button][xml][quick]") {
    const char* attrs[] = {"text", "Light", "bind_icon", "test_icon_subject", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    process_lvgl(10);

    const int32_t icon_idx =
        find_child_index_with_text(btn, helix::ui::icon::lookup_codepoint("light"));
    const int32_t label_idx = find_child_index_with_text(btn, "Light");
    REQUIRE(icon_idx >= 0);
    REQUIRE(label_idx >= 0);

    REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    REQUIRE(icon_idx < label_idx);
}

// icon_position="right" with bind_icon must keep the row flow and place the
// icon after the label — the one position the pre-existing code already honoured.
TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button bind_icon honours icon_position=right without a static icon",
                 "[ui_button][xml][quick]") {
    const char* attrs[] = {"text",          "Light", "bind_icon", "test_icon_subject",
                           "icon_position", "right", nullptr};

    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    process_lvgl(10);

    const int32_t icon_idx =
        find_child_index_with_text(btn, helix::ui::icon::lookup_codepoint("light"));
    const int32_t label_idx = find_child_index_with_text(btn, "Light");
    REQUIRE(icon_idx >= 0);
    REQUIRE(label_idx >= 0);

    REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    REQUIRE(icon_idx > label_idx);
}

// ============================================================================
// label_hidden_subject / label_hidden_if_bp_eq
// ============================================================================

namespace {
/// The button's text label: the label child that is not its icon glyph.
lv_obj_t* text_label(lv_obj_t* btn) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn); ++i) {
        lv_obj_t* child = lv_obj_get_child(btn, static_cast<int32_t>(i));
        if (lv_obj_check_type(child, &lv_label_class) && child != ui_button_get_icon(btn)) {
            return child;
        }
    }
    return nullptr;
}
} // namespace

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button label follows label_hidden_subject",
                 "[ui_button][xml][label_hidden]") {
    // Static: the XML scope keeps the name after the test, so the storage must outlive it.
    static lv_subject_t labels{};
    lv_subject_init_int(&labels, 1);
    lv_xml_register_subject(nullptr, "test_labels_subject", &labels);

    const char* attrs[] = {"icon",
                           "stop",
                           "text",
                           "Stop",
                           "label_hidden_subject",
                           "test_labels_subject",
                           "label_hidden_if_eq",
                           "0",
                           nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    lv_obj_t* label = text_label(btn);
    REQUIRE(label != nullptr);

    CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(&labels, 0);
    CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(&labels, 1);
    CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(btn);
    lv_subject_deinit(&labels);
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button label follows label_hidden_cond",
                 "[ui_button][xml][label_hidden]") {
    // Static: the XML scope keeps the names after the test, so the storage must outlive it.
    static lv_subject_t portrait{};
    static lv_subject_t tier{};
    lv_subject_init_int(&portrait, 0);
    lv_subject_init_int(&tier, 2);
    lv_xml_register_subject(nullptr, "test_cond_portrait", &portrait);
    lv_xml_register_subject(nullptr, "test_cond_tier", &tier);

    const char* attrs[] = {"icon",
                           "stop",
                           "text",
                           "Stop",
                           "label_hidden_cond",
                           "test_cond_portrait eq 1 and test_cond_tier lt 4",
                           nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    lv_obj_t* label = text_label(btn);
    REQUIRE(label != nullptr);

    CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(&portrait, 1);
    CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(&tier, 5);
    CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(btn);
    lv_subject_deinit(&portrait);
    lv_subject_deinit(&tier);
}

TEST_CASE_METHOD(UiButtonTestFixture, "ui_button label_hidden_if_bp_eq follows ui_breakpoint",
                 "[ui_button][xml][label_hidden]") {
    lv_subject_t* bp = lv_xml_get_subject(nullptr, "ui_breakpoint");
    REQUIRE(bp != nullptr);
    const int32_t saved = lv_subject_get_int(bp);

    const char* attrs[] = {"icon", "stop", "text", "Stop", "label_hidden_if_bp_eq", "1", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    lv_obj_t* label = text_label(btn);
    REQUIRE(label != nullptr);

    lv_subject_set_int(bp, 1);
    CHECK(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));
    lv_subject_set_int(bp, 3);
    CHECK_FALSE(lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN));

    lv_obj_delete(btn);
    lv_subject_set_int(bp, saved);
}

// stacked_subject + stacked_if_eq: stacked while an int subject equals the ref,
// row layout otherwise, following the subject live.
TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button stacked_subject restacks while the subject equals the ref",
                 "[ui_button][xml][quick]") {
    static lv_subject_t stack_when;
    lv_subject_init_int(&stack_when, 1);
    lv_xml_register_subject(nullptr, "test_button_stack_when", &stack_when);
    const char* attrs[] = {
        "icon",          "tune", "text", "Tune", "stacked_subject", "test_button_stack_when",
        "stacked_if_eq", "1",    nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    lv_obj_t* label = UITest::button_label(btn);
    REQUIRE(label != nullptr);
    CHECK(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    CHECK(lv_obj_get_index(label) == 1);

    lv_subject_set_int(&stack_when, 0);
    CHECK(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_style_height(btn, LV_PART_MAIN) == theme_manager_get_spacing("button_height"));

    lv_subject_set_int(&stack_when, 1);
    CHECK(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    lv_obj_delete(btn);
    lv_subject_deinit(&stack_when);
}

// stacked_if_bp_lte: at ui_breakpoint <= N the button restacks icon-over-label
// and fills its parent's height; above N it keeps the row layout it declared at
// create time. Pinned by the print status action grid at 480x320, where the
// row layout clips the labels.
TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button stacked_if_bp_lte restacks at or below threshold and restores above",
                 "[ui_button][xml][quick]") {
    const char* attrs[] = {"icon", "tune", "text", "Tune", "stacked_if_bp_lte", "1", nullptr};

    // Created while tiny: stacked from the first layout pass. The text label
    // is the LAST lv_label child: the icon glyph is an lv_label too and sits
    // first once stacked.
    {
        helix::test::ScopedBreakpoint tiny(UiBreakpoint::Tiny);
        lv_obj_t* btn = create_button(attrs);
        REQUIRE(btn != nullptr);
        lv_obj_t* label = UITest::button_label(btn);
        REQUIRE(label != nullptr);

        REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
        REQUIRE(lv_obj_get_index(label) == 1); // icon first, label under it
        REQUIRE(lv_obj_get_style_height(btn, LV_PART_MAIN) == lv_pct(100));

        // Crossing the threshold live restacks and restores the same button.
        {
            helix::test::ScopedBreakpoint medium(UiBreakpoint::Medium);
            REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
            REQUIRE(lv_obj_get_style_height(btn, LV_PART_MAIN) ==
                    theme_manager_get_spacing("button_height"));
        }
        REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_COLUMN);
    }

    // Created while medium: the row layout it declared at create time.
    {
        helix::test::ScopedBreakpoint medium(UiBreakpoint::Medium);
        lv_obj_t* btn = create_button(attrs);
        REQUIRE(btn != nullptr);
        REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
        REQUIRE(lv_obj_get_style_height(btn, LV_PART_MAIN) ==
                theme_manager_get_spacing("button_height"));
    }
}

// A style applied after the button's own setup (a component's styles, a
// binding) queues its contrast pass. That pass lands before the next frame
// draws, so the frame after does not repaint every button of a freshly built
// panel.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button: the next frame draws the contrast text colour",
                 "[ui_button][contrast]") {
    const char* attrs[] = {"text", "Go", "variant", "primary", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    lv_obj_t* label = nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(btn) && !label; ++i)
        if (lv_obj_check_type(lv_obj_get_child(btn, i), &lv_label_class))
            label = lv_obj_get_child(btn, i);
    REQUIRE(label != nullptr);
    const uint32_t before = lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN));

    static lv_style_t white_fill;
    lv_style_init(&white_fill);
    lv_style_set_bg_color(&white_fill, lv_color_white());
    lv_style_set_bg_opa(&white_fill, LV_OPA_COVER);
    lv_obj_add_style(btn, &white_fill, LV_PART_MAIN);

    lv_refr_now(nullptr); // no UpdateQueue drain
    const uint32_t drawn = lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN));
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    const uint32_t settled = lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN));
    CAPTURE(before & 0xFFFFFF, drawn & 0xFFFFFF, settled & 0xFFFFFF);
    REQUIRE(settled != before); // the new fill does change the text colour
    CHECK(drawn == settled);
    lv_obj_remove_style(btn, &white_fill, LV_PART_MAIN);
}

// A shell button that lays its content out through a wrapper still gives that
// content contrast; an icon with an accent variant of its own keeps it.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button: contrast reaches content inside a layout wrapper",
                 "[ui_button][contrast]") {
    const char* attrs[] = {"variant", "primary", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    lv_obj_t* wrap = lv_obj_create(btn);
    lv_obj_t* label = lv_label_create(wrap);
    lv_obj_t* text_icon = lv_label_create(wrap);
    lv_obj_set_style_text_font(text_icon, &mdi_icons_24, LV_PART_MAIN);
    helix::ui::apply_variant_text_style(text_icon, helix::ui::Variant::NONE);
    lv_obj_t* accent_icon = lv_label_create(wrap);
    lv_obj_set_style_text_font(accent_icon, &mdi_icons_24, LV_PART_MAIN);
    helix::ui::apply_variant_text_style(accent_icon, helix::ui::Variant::SUCCESS);
    const uint32_t accent_before =
        lv_color_to_u32(lv_obj_get_style_text_color(accent_icon, LV_PART_MAIN));

    static lv_style_t white_fill;
    lv_style_init(&white_fill);
    lv_style_set_bg_color(&white_fill, lv_color_white());
    lv_style_set_bg_opa(&white_fill, LV_OPA_COVER);
    lv_obj_add_style(btn, &white_fill, LV_PART_MAIN);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());

    const uint32_t expected = lv_color_to_u32(theme_manager_get_contrast_adjusted_text(
        theme_manager_get_color("text"), lv_color_white()));
    CHECK(lv_obj_has_style_prop(label, LV_PART_MAIN, LV_STYLE_TEXT_COLOR));
    CHECK(lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)) == expected);
    CHECK(lv_color_to_u32(lv_obj_get_style_text_color(text_icon, LV_PART_MAIN)) == expected);
    CHECK(lv_color_to_u32(lv_obj_get_style_text_color(accent_icon, LV_PART_MAIN)) == accent_before);
    lv_obj_remove_style(btn, &white_fill, LV_PART_MAIN);
}

namespace {
int s_invalidations = 0;
void count_invalidation(lv_event_t* /*e*/) {
    ++s_invalidations;
}
} // namespace

// The contrast pass runs on every style and state change; one that finds the
// colours already applied must not repaint the button.
TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button: a contrast pass with nothing to change repaints nothing",
                 "[ui_button][contrast]") {
    const char* attrs[] = {"text", "Go", "variant", "primary", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    lv_refr_now(nullptr);

    lv_display_t* disp = lv_obj_get_display(btn);
    s_invalidations = 0;
    lv_display_add_event_cb(disp, count_invalidation, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_obj_send_event(btn, LV_EVENT_STATE_CHANGED, nullptr);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    lv_display_remove_event_cb_with_user_data(disp, count_invalidation, nullptr);

    CHECK(s_invalidations == 0);
}

// ============================================================================
// Rows that share space by content (prestonbrown/helixscreen#1307)
// ============================================================================

namespace {
/// A flex row of fixed width, the shape of the Home and filament-op rows.
lv_obj_t* make_button_row(lv_obj_t* parent, int32_t width) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    return row;
}

lv_obj_t* make_sharing_button(lv_obj_t* row, const char* text) {
    const char* attrs[] = {"width",   "0",    "flex_grow", "1",    "style_min_width",
                           "content", "text", text,        nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(row, "ui_button", attrs));
}

bool label_inside(lv_obj_t* btn) {
    lv_area_t b;
    lv_area_t l;
    lv_obj_get_coords(btn, &b);
    lv_obj_get_coords(UITest::button_label(btn), &l);
    return l.x1 >= b.x1 && l.x2 <= b.x2;
}
} // namespace

// Labels that fit split the row evenly, exactly as plain flex_grow buttons.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button row shares evenly while every label fits",
                 "[ui_button][xml][quick]") {
    lv_obj_t* row = make_button_row(test_screen(), 300);
    lv_obj_t* a = make_sharing_button(row, "Go");
    lv_obj_t* b = make_sharing_button(row, "Stop");
    lv_obj_t* c = make_sharing_button(row, "X");
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_width(a) == 100);
    CHECK(lv_obj_get_width(b) == 100);
    CHECK(lv_obj_get_width(c) == 100);
}

// A label longer than its even share takes its width from the row, and the
// short neighbours give it up, so no label crosses its button's border.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button row gives a long label room from short neighbours",
                 "[ui_button][xml][quick]") {
    lv_obj_t* row = make_button_row(test_screen(), 420);
    lv_obj_t* a = make_sharing_button(row, "Go");
    lv_obj_t* b = make_sharing_button(row, "Firmware Restart Now");
    lv_obj_t* c = make_sharing_button(row, "X");
    lv_obj_update_layout(test_screen());
    REQUIRE(lv_obj_get_width(UITest::button_label(b)) > 140);
    CHECK(lv_obj_get_width(b) > 140);
    CHECK(label_inside(a));
    CHECK(label_inside(b));
    CHECK(label_inside(c));
    CHECK(lv_obj_get_width(a) < 140);
    CHECK(lv_obj_get_width(a) + lv_obj_get_width(b) + lv_obj_get_width(c) == 420);
}

// The row re-shares when a label grows after layout (a language switch, a
// bound text), not only when it is first built.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button row re-shares when a label grows",
                 "[ui_button][xml][quick]") {
    lv_obj_t* row = make_button_row(test_screen(), 420);
    lv_obj_t* a = make_sharing_button(row, "Go");
    lv_obj_t* b = make_sharing_button(row, "Stop");
    make_sharing_button(row, "X");
    lv_obj_update_layout(test_screen());
    REQUIRE(lv_obj_get_width(b) == 140);

    ui_button_set_text(b, "Firmware Restart Now");
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_width(b) > 140);
    CHECK(label_inside(b));
    CHECK(lv_obj_get_width(a) < 140);
}

// Unstacking returns a container-sized button to the height it declared,
// not the fixed button_height.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button unstacks to its declared height",
                 "[ui_button][xml][quick]") {
    static lv_subject_t stack_when;
    lv_subject_init_int(&stack_when, 0);
    lv_xml_register_subject(nullptr, "test_button_stack_height", &stack_when);
    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 200, 90);
    const char* attrs[] = {"icon",
                           "tune",
                           "text",
                           "Load",
                           "height",
                           "100%",
                           "stacked_subject",
                           "test_button_stack_height",
                           "stacked_if_eq",
                           "1",
                           nullptr};
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(box, "ui_button", attrs));
    REQUIRE(btn != nullptr);
    CHECK(lv_obj_get_style_height(btn, LV_PART_MAIN) == lv_pct(100));

    lv_subject_set_int(&stack_when, 1);
    lv_subject_set_int(&stack_when, 0);
    CHECK(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) == LV_FLEX_FLOW_ROW);
    CHECK(lv_obj_get_style_height(btn, LV_PART_MAIN) == lv_pct(100));
    lv_obj_delete(box);
    lv_subject_deinit(&stack_when);
}

namespace {

/// One line of the label's font plus its own vertical padding.
int32_t one_line_height(lv_obj_t* label) {
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    return lv_font_get_line_height(font) + lv_obj_get_style_space_top(label, LV_PART_MAIN) +
           lv_obj_get_style_space_bottom(label, LV_PART_MAIN);
}

/// A button in a 60px box whose stacking follows @p subject_name.
lv_obj_t* make_flip_button(lv_obj_t* box, const char* subject_name, const char* long_mode) {
    const char* attrs[] = {
        "icon",          "tune",      "text",    "Firmware Restart Now", "width",
        "100%",          "long_mode", long_mode, "stacked_subject",      subject_name,
        "stacked_if_eq", "1",         nullptr};
    return static_cast<lv_obj_t*>(lv_xml_create(box, "ui_button", attrs));
}

} // namespace

// A dotted label holds one line of its font beside its icon and under it, and
// keeps holding it as the button stacks and unstacks, so text too long for the
// button ellipsizes there instead of wrapping out of the bottom.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button dotted label stays one line across a restack",
                 "[ui_button][xml][quick]") {
    static lv_subject_t stack_when;
    lv_subject_init_int(&stack_when, 0);
    lv_xml_register_subject(nullptr, "test_button_stack_dots", &stack_when);
    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 60, 90);
    lv_obj_t* btn = make_flip_button(box, "test_button_stack_dots", "dots");
    REQUIRE(btn != nullptr);
    lv_obj_t* label = UITest::button_label(btn);

    for (const int stacked : {1, 0, 1}) {
        CAPTURE(stacked);
        lv_subject_set_int(&stack_when, stacked);
        lv_obj_update_layout(test_screen());
        REQUIRE(lv_obj_get_style_flex_flow(btn, LV_PART_MAIN) ==
                (stacked ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW));
        CHECK(lv_obj_get_height(label) == one_line_height(label));
        CHECK(label_inside(btn));
    }
    lv_obj_delete(box);
    lv_subject_deinit(&stack_when);
}

// Under its icon a wrap-mode label takes its text's own height, including after
// it has been held to one line beside the icon.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button stacked wrap label keeps content height",
                 "[ui_button][xml][quick]") {
    static lv_subject_t stack_when;
    lv_subject_init_int(&stack_when, 0);
    lv_xml_register_subject(nullptr, "test_button_stack_wrap", &stack_when);
    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_size(box, 60, 200);
    lv_obj_t* btn = make_flip_button(box, "test_button_stack_wrap", "wrap");
    REQUIRE(btn != nullptr);
    lv_obj_t* label = UITest::button_label(btn);
    lv_obj_update_layout(test_screen());
    REQUIRE(lv_obj_get_height(label) == one_line_height(label));

    lv_subject_set_int(&stack_when, 1);
    // A style landing after the restack must not pull the label back to one line.
    lv_obj_set_style_text_letter_space(label, 1, LV_PART_MAIN);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_style_height(label, LV_PART_MAIN) == LV_SIZE_CONTENT);
    CHECK(lv_obj_get_height(label) > one_line_height(label));
    lv_obj_delete(box);
    lv_subject_deinit(&stack_when);
}

// A stacked button that asked for a content minimum holds the wider of its
// icon and label as a measured width (a column cannot measure its own content
// without feeding its width back in), and returns to the content minimum when
// it unstacks.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button stacked keeps a measured minimum width",
                 "[ui_button][xml][quick]") {
    static lv_subject_t stack_when;
    lv_subject_init_int(&stack_when, 0);
    lv_xml_register_subject(nullptr, "test_button_stack_min", &stack_when);
    lv_obj_t* row = make_button_row(test_screen(), 300);
    const char* attrs[] = {"width",
                           "0",
                           "flex_grow",
                           "1",
                           "style_min_width",
                           "content",
                           "icon",
                           "tune",
                           "text",
                           "Firmware Restart Now",
                           "stacked_subject",
                           "test_button_stack_min",
                           "stacked_if_eq",
                           "1",
                           nullptr};
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(row, "ui_button", attrs));
    make_sharing_button(row, "X");
    make_sharing_button(row, "Y");
    REQUIRE(btn != nullptr);

    lv_subject_set_int(&stack_when, 1);
    lv_obj_update_layout(test_screen());
    lv_obj_t* label = UITest::button_label(btn);
    const int32_t expected = lv_obj_get_width(label) +
                             lv_obj_get_style_space_left(btn, LV_PART_MAIN) +
                             lv_obj_get_style_space_right(btn, LV_PART_MAIN);
    CHECK(lv_obj_get_style_min_width(btn, LV_PART_MAIN) == expected);
    CHECK(lv_obj_get_width(btn) >= expected);
    CHECK(label_inside(btn));

    lv_subject_set_int(&stack_when, 0);
    lv_obj_update_layout(test_screen());
    CHECK(lv_obj_get_style_min_width(btn, LV_PART_MAIN) == LV_SIZE_CONTENT);
    lv_obj_delete(row);
    lv_subject_deinit(&stack_when);
}

// A column button cannot hold style_min_width="content" (LVGL measures a
// column's content from its centered items, which feeds back into the width).
// However the minimum arrives, at create or from a style added later, the
// button holds the wider of its icon and label plus its padding instead.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button column holds a measured minimum, not content",
                 "[ui_button][xml][quick]") {
    lv_obj_t* row = make_button_row(test_screen(), 300);
    const auto measured = [](lv_obj_t* btn) {
        return lv_obj_get_width(UITest::button_label(btn)) +
               lv_obj_get_style_space_left(btn, LV_PART_MAIN) +
               lv_obj_get_style_space_right(btn, LV_PART_MAIN);
    };

    SECTION("from the XML attribute") {
        const char* attrs[] = {"width",
                               "0",
                               "flex_grow",
                               "1",
                               "icon",
                               "tune",
                               "icon_position",
                               "top",
                               "text",
                               "Firmware Restart Now",
                               "style_min_width",
                               "content",
                               nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(row, "ui_button", attrs));
        make_sharing_button(row, "X");
        lv_obj_update_layout(test_screen());
        CHECK(lv_obj_get_style_min_width(btn, LV_PART_MAIN) == measured(btn));
        CHECK(label_inside(btn));
    }

    SECTION("from a style added after create") {
        const char* attrs[] = {"width",         "0",
                               "flex_grow",     "1",
                               "icon",          "tune",
                               "icon_position", "top",
                               "text",          "Firmware Restart Now",
                               nullptr};
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_xml_create(row, "ui_button", attrs));
        make_sharing_button(row, "X");
        lv_obj_update_layout(test_screen());
        static lv_style_t content_min;
        lv_style_init(&content_min);
        lv_style_set_min_width(&content_min, LV_SIZE_CONTENT);
        lv_obj_add_style(btn, &content_min, LV_PART_MAIN);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        lv_refr_now(nullptr);
        lv_obj_update_layout(test_screen());
        CHECK(lv_obj_get_style_min_width(btn, LV_PART_MAIN) == measured(btn));
        CHECK(label_inside(btn));
        lv_obj_remove_style(btn, &content_min, LV_PART_MAIN);
    }
    lv_obj_delete(row);
}

TEST_CASE_METHOD(UiButtonTestFixture,
                 "ui_button centres XML children laid out by flex_flow on both axes",
                 "[ui_button][layout]") {
    const char* flow = GENERATE("row", "column");
    CAPTURE(flow);
    const char* attrs[] = {"width", "200", "height", "80", "flex_flow", flow, nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);
    lv_obj_t* a = lv_obj_create(btn);
    lv_obj_set_size(a, 20, 20);
    lv_obj_t* b = lv_obj_create(btn);
    lv_obj_set_size(b, 30, 20);
    lv_obj_update_layout(test_screen());

    lv_area_t box;
    lv_obj_get_content_coords(btn, &box);
    int32_t top = INT32_MAX, bottom = INT32_MIN, left = INT32_MAX, right = INT32_MIN;
    for (lv_obj_t* child : {a, b}) {
        lv_area_t c;
        lv_obj_get_coords(child, &c);
        top = std::min(top, c.y1);
        bottom = std::max(bottom, c.y2);
        left = std::min(left, c.x1);
        right = std::max(right, c.x2);
    }
    // The children's bounding box sits in the middle of the content box, within
    // a pixel of rounding, so a row of icon + caption lines up with a text button.
    CHECK(std::abs((top - box.y1) - (box.y2 - bottom)) <= 1);
    CHECK(std::abs((left - box.x1) - (box.x2 - right)) <= 1);
    lv_obj_delete(btn);
}

// A bound style (bind_style) dims an enabled button: the contrast pass keeps a
// local opacity only for the disabled look, which would otherwise outrank it.
TEST_CASE_METHOD(UiButtonTestFixture, "ui_button honors a style's opacity while enabled",
                 "[ui_button][contrast][quick]") {
    const char* attrs[] = {"text", "PLA", nullptr};
    lv_obj_t* btn = create_button(attrs);
    REQUIRE(btn != nullptr);

    static lv_style_t dim;
    static bool dim_ready = false;
    if (!dim_ready) {
        lv_style_init(&dim);
        lv_style_set_opa(&dim, LV_OPA_40);
        dim_ready = true;
    }
    lv_obj_add_style(btn, &dim, LV_PART_MAIN);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    process_lvgl(10);
    CHECK(lv_obj_get_style_opa(btn, LV_PART_MAIN) == LV_OPA_40);

    // Disabled still reads as disabled, and enabling hands opacity back.
    lv_obj_add_state(btn, LV_STATE_DISABLED);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    process_lvgl(10);
    CHECK(lv_obj_get_style_opa(btn, LV_PART_MAIN) == LV_OPA_50);
    lv_obj_remove_state(btn, LV_STATE_DISABLED);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    process_lvgl(10);
    CHECK(lv_obj_get_style_opa(btn, LV_PART_MAIN) == LV_OPA_40);
    lv_obj_remove_style(btn, &dim, LV_PART_MAIN);
}
