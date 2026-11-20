// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

/**
 * Step states for progress indicator
 */
namespace helix {
enum class StepState { Pending = 0, Active = 1, Completed = 2 };
} // namespace helix

/**
 * Step definition structure
 */
typedef struct {
    const char* label;      // Step text (e.g., "Nozzle heating")
    helix::StepState state; // Current state
} ui_step_t;

/**
 * Create a step progress indicator widget
 *
 * @param parent Parent container
 * @param steps Array of step definitions
 * @param step_count Number of steps
 * @param horizontal true for horizontal layout, false for vertical (default)
 * @return Created widget: a step_progress component holding one step_progress_step_h
 *         or step_progress_step_v per step (ui_xml/components/step_progress*.xml)
 */
lv_obj_t* ui_step_progress_create(lv_obj_t* parent, const ui_step_t* steps, int step_count,
                                  bool horizontal);

/**
 * Update the current active step (automatically sets previous steps to completed)
 *
 * @param widget Step progress widget
 * @param step_index 0-based step index to mark as active
 */
void ui_step_progress_set_current(lv_obj_t* widget, int step_index);

/**
 * Mark a specific step as completed
 *
 * @param widget Step progress widget
 * @param step_index 0-based step index to mark as completed
 */
void ui_step_progress_set_completed(lv_obj_t* widget, int step_index);

/**
 * Update a step's label text
 *
 * @param widget Step progress widget
 * @param step_index 0-based step index
 * @param new_label New label text
 */
void ui_step_progress_set_label(lv_obj_t* widget, int step_index, const char* new_label);

/**
 * Show only the current step's label: a single caption centred under the row,
 * every step's own label hidden. For a horizontal row too narrow for one label
 * per step. The caption follows set_current(), set_completed() and set_label().
 *
 * @param widget Step progress widget
 * @param on true for the caption, false for a label under every step
 */
// NAMESPACE_OK: the widget's C API, beside its siblings
void ui_step_progress_set_current_label_only(lv_obj_t* widget, bool on);

/// The caption label in current-label-only mode, or nullptr.
// NAMESPACE_OK: the widget's C API, beside its siblings
lv_obj_t* ui_step_progress_get_caption(lv_obj_t* widget);

/**
 * Which step the caption names: the active step; with none active, the last one
 * completed; with none completed either, the first. -1 for no steps.
 */
// NAMESPACE_OK: the widget's C API, beside its siblings
int ui_step_progress_caption_step(const helix::StepState* states, int step_count);

/**
 * Test-only introspection: return the connector line object drawn between step
 * circle @p index and @p index+1, or nullptr if it has not been created yet.
 * Exposed for rendering tests that assert connector geometry after layout.
 *
 * @param widget Step progress widget
 * @param index 0-based connector index (0 .. step_count-2)
 * @return Connector object, or nullptr
 */
lv_obj_t* ui_step_progress_test_get_connector(lv_obj_t* widget, int index);
