// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_test.h"

#include "app_globals.h"
#include "lv_markdown.h"
#include "printer_state.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>

using namespace helix;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

TestPanel::TestPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    // TestPanel doesn't use PrinterState or IMoonrakerAPI, but we accept
    // them for interface consistency with other panels
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

void TestPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // TestPanel has no subjects to initialize
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized (none required)", get_name());
}

void TestPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    // Populate diagnostic labels
    populate_labels();

    // Populate markdown viewer with sample content exercising all elements
    populate_markdown();
}

// ============================================================================
// PRIVATE HELPERS
// ============================================================================

void TestPanel::populate_labels() {
    // Get screen dimensions using custom breakpoints optimized for our hardware
    lv_display_t* display = lv_display_get_default();
    int32_t hor_res = lv_display_get_horizontal_resolution(display);
    int32_t ver_res = lv_display_get_vertical_resolution(display);
    int32_t greater_res = LV_MAX(hor_res, ver_res);

    // Determine screen size category
    const char* size_category;
    int switch_width, switch_height;
    int row_height;

    if (greater_res <= UI_BREAKPOINT_MICRO_MAX) { // ≤272: 480x272
        size_category = "MICRO";
        switch_width = 28;
        switch_height = 14;
        row_height = 22;
    } else if (greater_res <= UI_BREAKPOINT_SMALL_MAX) { // 273-460
        // TINY/SMALL share presets — switch elements are too small to benefit from separate tiers
        size_category = "TINY/SMALL";
        switch_width = 36;
        switch_height = 18;
        row_height = 26;
    } else if (greater_res <= UI_BREAKPOINT_MEDIUM_MAX) { // 481-800: 800x480
        size_category = "MEDIUM";
        switch_width = 64;
        switch_height = 32;
        row_height = 40;
    } else { // >800: 1024x600+
        size_category = "LARGE";
        switch_width = 88;
        switch_height = 44;
        row_height = 56;
    }

    // Find info labels
    lv_obj_t* screen_size_label = lv_obj_find_by_name(panel_, "screen_size_label");
    lv_obj_t* switch_size_label = lv_obj_find_by_name(panel_, "switch_size_label");
    lv_obj_t* row_height_label = lv_obj_find_by_name(panel_, "row_height_label");

    // Populate labels
    char buffer[128];

    if (screen_size_label) {
        snprintf(buffer, sizeof(buffer), "Screen Size: %s (%dx%d, max=%d)", size_category, hor_res,
                 ver_res, greater_res);
        lv_label_set_text(screen_size_label, buffer);
    }

    if (switch_size_label) {
        snprintf(buffer, sizeof(buffer), "Switch Size: %dx%dpx (knob padding varies)", switch_width,
                 switch_height);
        lv_label_set_text(switch_size_label, buffer);
    }

    if (row_height_label) {
        snprintf(buffer, sizeof(buffer), "Row Height: %dpx (fits switch + padding)", row_height);
        lv_label_set_text(row_height_label, buffer);
    }

    spdlog::info("[{}] Setup complete: {} ({}x{}, max={}), switch={}x{}, row={}px", get_name(),
                 size_category, hor_res, ver_res, greater_res, switch_width, switch_height,
                 row_height);
}

void TestPanel::populate_markdown() {
    lv_obj_t* md_widget = lv_obj_find_by_name(lv_screen_active(), "markdown_test");
    if (!md_widget) {
        spdlog::warn("[{}] markdown_test widget not found", get_name());
        return;
    }

    // Sample markdown exercising every supported element
    static const char* sample_md =
        "# Heading 1\n"
        "## Heading 2\n"
        "### Heading 3\n"
        "#### Heading 4\n"
        "##### Heading 5\n"
        "###### Heading 6\n"
        "\n"
        "Regular paragraph text with **bold**, *italic*, and ***bold italic*** "
        "emphasis styles. Also some `inline code` in the middle.\n"
        "\n"
        "---\n"
        "\n"
        "## Lists\n"
        "\n"
        "Unordered list:\n"
        "\n"
        "- First item\n"
        "- Second item with **bold**\n"
        "- Third item with `code`\n"
        "  - Nested item one\n"
        "  - Nested item two\n"
        "- Back to top level\n"
        "\n"
        "Ordered list:\n"
        "\n"
        "1. Step one\n"
        "2. Step two\n"
        "3. Step three\n"
        "   1. Sub-step A\n"
        "   2. Sub-step B\n"
        "\n"
        "---\n"
        "\n"
        "## Code\n"
        "\n"
        "Inline: use `lv_markdown_set_text()` to render.\n"
        "\n"
        "Fenced code block:\n"
        "\n"
        "```\n"
        "void setup() {\n"
        "    auto* widget = lv_markdown_create(parent);\n"
        "    lv_markdown_set_text(widget, md);\n"
        "}\n"
        "```\n"
        "\n"
        "## Blockquote\n"
        "\n"
        "> This is a blockquote with **bold** text.\n"
        "> It can span multiple lines.\n"
        "\n"
        "---\n"
        "\n"
        "End of test. All supported elements exercised above.\n";

    lv_markdown_set_text(md_widget, sample_md);
    spdlog::info("[{}] Markdown test content set", get_name());
}

// ============================================================================
// GLOBAL INSTANCE (needed by main.cpp)
// ============================================================================

static std::unique_ptr<TestPanel> g_test_panel;

TestPanel& get_global_test_panel() {
    if (!g_test_panel) {
        g_test_panel = std::make_unique<TestPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("TestPanel",
                                                         []() { g_test_panel.reset(); });
    }
    return *g_test_panel;
}
