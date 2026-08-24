// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"

/**
 * @file ui_panel_glyphs.h
 * @brief Glyphs panel displaying all LVGL symbols with their names
 *
 * A diagnostic panel that displays the complete set of LVGL symbol glyphs
 * from lv_symbol_def.h, useful for reference when selecting icons for UI.
 *
 * ## Key Features:
 * - Scrollable vertical list of all ~60 LVGL symbols
 * - Each entry shows the icon + symbolic constant name (e.g., "LV_SYMBOL_AUDIO")
 * - Header displays total symbol count
 * - Proper theming via globals.xml constants
 *
 * ## Migration Notes:
 * Second panel migrated to class-based architecture (Phase 2).
 * Display-only panel with no subjects or printer connectivity.
 *
 * @see PanelBase for base class documentation
 * @see TestPanel for similar simple panel pattern
 */
class GlyphsPanel : public PanelBase {
  public:
    /**
     * @brief Construct GlyphsPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState (not actively used)
     * @param api Pointer to IMoonrakerAPI (not actively used)
     *
     * @note Dependencies are passed for interface consistency with PanelBase,
     *       but this panel doesn't require printer connectivity.
     */
    GlyphsPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~GlyphsPanel() override = default;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief No-op for GlyphsPanel (no subjects to initialize)
     */
    void init_subjects() override;

    /**
     * @brief Setup the glyphs panel and populate with symbol entries
     *
     * Updates the symbol count label and creates glyph display items
     * for all LVGL symbols in the scrollable content area.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen (unused for this panel)
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Glyphs Panel";
    }
    const char* get_xml_component_name() const override {
        return "glyphs_panel";
    }

  private:
    /**
     * @brief Populate the content area with glyph display items
     *
     * Creates a styled row for each LVGL symbol showing the icon
     * and its constant name. Items are added to the scrollable
     * content area found within the panel.
     */
    void populate_glyphs();
};

// Global instance accessor (needed by main.cpp)
GlyphsPanel& get_global_glyphs_panel();

// Legacy create wrapper (test panel - still used by main.cpp)
lv_obj_t* ui_panel_glyphs_create(lv_obj_t* parent);
