// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"

/**
 * @file ui_panel_test.h
 * @brief Test panel for debugging screen dimensions and widget sizing
 *
 * A simple diagnostic panel that displays screen resolution information
 * and validates switch/row sizing across different breakpoints.
 *
 * ## Key Features:
 * - Displays current screen dimensions and size category
 * - Shows calculated switch and row heights for current resolution
 * - Provides keyboard test textarea for input validation
 *
 * ## Migration Notes:
 * This is the first panel migrated to the class-based architecture.
 * It serves as a template for simple panels with no subjects.
 *
 * @see PanelBase for base class documentation
 */
class TestPanel : public PanelBase {
  public:
    /**
     * @brief Construct TestPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState (not actively used)
     * @param api Pointer to IMoonrakerAPI (not actively used)
     *
     * @note Dependencies are passed for interface consistency with PanelBase,
     *       but this panel doesn't require printer connectivity.
     */
    TestPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~TestPanel() override = default;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief No-op for TestPanel (no subjects to initialize)
     */
    void init_subjects() override;

    /**
     * @brief Setup the test panel with diagnostic information
     *
     * Populates screen size labels and registers keyboard for textarea.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen (unused for this panel)
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Test Panel";
    }
    const char* get_xml_component_name() const override {
        return "test_panel";
    }

  private:
    /**
     * @brief Populate diagnostic labels with screen size information
     *
     * Determines breakpoint category and calculates appropriate
     * switch/row dimensions based on screen resolution.
     */
    void populate_labels();

    /**
     * @brief Populate markdown viewer with sample content
     *
     * Sets a comprehensive markdown sample exercising all supported
     * elements (headings, emphasis, code, lists, blockquotes, HR).
     */
    void populate_markdown();
};

// Global instance accessor (needed by main.cpp)
TestPanel& get_global_test_panel();
