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
 *
 * HelixScreen is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HelixScreen. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "lvgl/lvgl.h"

/**
 * Step states for progress indicator
 */
typedef enum {
    UI_STEP_STATE_PENDING = 0,     // Not yet started (muted color)
    UI_STEP_STATE_ACTIVE = 1,      // Currently in progress (primary color, bold text)
    UI_STEP_STATE_COMPLETED = 2    // Finished (success color, checkmark)
} ui_step_state_t;

/**
 * Step definition structure
 */
typedef struct {
    const char* label;             // Step text (e.g., "Nozzle heating")
    ui_step_state_t state;         // Current state
} ui_step_t;

/**
 * Create a step progress indicator widget
 *
 * @param parent Parent container
 * @param steps Array of step definitions
 * @param step_count Number of steps
 * @param horizontal true for horizontal layout, false for vertical (default)
 * @return Created widget (flex container with step items)
 */
lv_obj_t* ui_step_progress_create(lv_obj_t* parent,
                                   const ui_step_t* steps,
                                   int step_count,
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
