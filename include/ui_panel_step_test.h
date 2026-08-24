// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_base.h"
#include "ui_step_progress.h"

/**
 * @file ui_panel_step_test.h
 * @brief Step progress test panel for demonstrating wizard step indicators
 *
 * A test panel showcasing the ui_step_progress widget in both vertical
 * and horizontal orientations. Provides buttons to navigate through
 * wizard steps for visual testing.
 *
 * ## Key Features:
 * - Vertical step progress widget (retract wizard simulation)
 * - Horizontal step progress widget (leveling wizard simulation)
 * - Prev/Next/Complete buttons to manipulate step state
 * - Demonstrates ui_step_progress API usage
 *
 * ## Migration Notes:
 * Third panel migrated to class-based architecture (Phase 2).
 * First panel with event callbacks - uses static trampolines pattern.
 *
 * @see PanelBase for base class documentation
 * @see ui_step_progress for the widget being tested
 */
class StepTestPanel : public PanelBase {
  public:
    /**
     * @brief Construct StepTestPanel with injected dependencies
     *
     * @param printer_state Reference to helix::PrinterState (not actively used)
     * @param api Pointer to IMoonrakerAPI (not actively used)
     *
     * @note Dependencies are passed for interface consistency with PanelBase,
     *       but this panel doesn't require printer connectivity.
     */
    StepTestPanel(helix::PrinterState& printer_state, IMoonrakerAPI* api);

    ~StepTestPanel() override = default;

    //
    // === PanelBase Implementation ===
    //

    /**
     * @brief Register XML event callbacks for button interactions
     *
     * Registers on_step_test_prev, on_step_test_next, and on_step_test_complete
     * callbacks via lv_xml_register_event_cb() before XML is created.
     */
    void init_subjects() override;

    /**
     * @brief Setup the step test panel with progress widgets and button handlers
     *
     * Creates vertical and horizontal step progress widgets, initializes
     * them to step 1, and wires up prev/next/complete button callbacks.
     *
     * @param panel Root panel object from lv_xml_create()
     * @param parent_screen Parent screen (unused for this panel)
     */
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;

    const char* get_name() const override {
        return "Step Test Panel";
    }
    const char* get_xml_component_name() const override {
        return "step_test_panel";
    }

  private:
    //
    // === Instance State ===
    //

    lv_obj_t* vertical_widget_ = nullptr;
    lv_obj_t* horizontal_widget_ = nullptr;
    int vertical_step_ = 0;
    int horizontal_step_ = 0;

    //
    // === Private Helpers ===
    //

    /**
     * @brief Create and configure the step progress widgets
     */
    void create_progress_widgets();

    //
    // === Button Handlers ===
    //

    void handle_prev();
    void handle_next();
    void handle_complete();

    //
    // === Static Event Callbacks ===
    //
    // Registered via lv_xml_register_event_cb() in init_subjects().
    // Use get_global_step_test_panel() to access instance methods.
    //

    static void on_prev_clicked(lv_event_t* e);
    static void on_next_clicked(lv_event_t* e);
    static void on_complete_clicked(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
StepTestPanel& get_global_step_test_panel();
