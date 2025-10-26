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

#include "ui_panel_step_test.h"
#include "ui_step_progress.h"
#include <stdio.h>

// Widget references
static lv_obj_t* vertical_widget = nullptr;
static lv_obj_t* horizontal_widget = nullptr;
static int vertical_step = 0;
static int horizontal_step = 0;

// Step definitions for vertical progress (retract wizard)
static const ui_step_t vertical_steps[] = {
    {"Nozzle heating", UI_STEP_STATE_COMPLETED},
    {"Prepare to retract", UI_STEP_STATE_ACTIVE},
    {"Retracting", UI_STEP_STATE_PENDING},
    {"Retract done", UI_STEP_STATE_PENDING}
};
static const int vertical_step_count = sizeof(vertical_steps) / sizeof(vertical_steps[0]);

// Step definitions for horizontal progress (leveling wizard)
static const ui_step_t horizontal_steps[] = {
    {"Homing", UI_STEP_STATE_COMPLETED},
    {"Leveling", UI_STEP_STATE_ACTIVE},
    {"Vibration test", UI_STEP_STATE_PENDING},
    {"Completed", UI_STEP_STATE_PENDING}
};
static const int horizontal_step_count = sizeof(horizontal_steps) / sizeof(horizontal_steps[0]);

// Event handlers
static void on_prev_clicked(lv_event_t* e) {
    (void)e;

    // Move both wizards back one step
    if (vertical_step > 0) {
        vertical_step--;
        ui_step_progress_set_current(vertical_widget, vertical_step);
    }

    if (horizontal_step > 0) {
        horizontal_step--;
        ui_step_progress_set_current(horizontal_widget, horizontal_step);
    }

    LV_LOG_USER("Previous step: vertical=%d, horizontal=%d", vertical_step, horizontal_step);
}

static void on_next_clicked(lv_event_t* e) {
    (void)e;

    // Move both wizards forward one step
    if (vertical_step < vertical_step_count - 1) {
        vertical_step++;
        ui_step_progress_set_current(vertical_widget, vertical_step);
    }

    if (horizontal_step < horizontal_step_count - 1) {
        horizontal_step++;
        ui_step_progress_set_current(horizontal_widget, horizontal_step);
    }

    LV_LOG_USER("Next step: vertical=%d, horizontal=%d", vertical_step, horizontal_step);
}

static void on_complete_clicked(lv_event_t* e) {
    (void)e;

    // Complete all steps for both wizards
    vertical_step = vertical_step_count - 1;
    horizontal_step = horizontal_step_count - 1;

    ui_step_progress_set_current(vertical_widget, vertical_step);
    ui_step_progress_set_current(horizontal_widget, horizontal_step);

    LV_LOG_USER("All steps completed");
}

void ui_panel_step_test_setup(lv_obj_t* panel_root) {
    if (!panel_root) {
        LV_LOG_ERROR("Cannot setup step test panel: panel_root is null");
        return;
    }

    // Find container widgets
    lv_obj_t* vertical_container = lv_obj_find_by_name(panel_root, "vertical_progress_container");
    lv_obj_t* horizontal_container = lv_obj_find_by_name(panel_root, "horizontal_progress_container");

    LV_LOG_USER("Found containers: vertical=%p, horizontal=%p", vertical_container, horizontal_container);

    if (!vertical_container || !horizontal_container) {
        LV_LOG_ERROR("Failed to find progress containers in step test panel");
        return;
    }


    // Create vertical progress widget
    vertical_widget = ui_step_progress_create(vertical_container, vertical_steps,
                                               vertical_step_count, false);
    if (!vertical_widget) {
        LV_LOG_ERROR("Failed to create vertical progress widget");
        return;
    }

    // Create horizontal progress widget
    horizontal_widget = ui_step_progress_create(horizontal_container, horizontal_steps,
                                                 horizontal_step_count, true);
    if (!horizontal_widget) {
        LV_LOG_ERROR("Failed to create horizontal progress widget");
        return;
    }

    // Initialize current steps (step 1 = index 1) and apply styling
    vertical_step = 1;
    horizontal_step = 1;
    ui_step_progress_set_current(vertical_widget, vertical_step);
    ui_step_progress_set_current(horizontal_widget, horizontal_step);

    // Wire up button event handlers
    lv_obj_t* btn_prev = lv_obj_find_by_name(panel_root, "btn_prev");
    lv_obj_t* btn_next = lv_obj_find_by_name(panel_root, "btn_next");
    lv_obj_t* btn_complete = lv_obj_find_by_name(panel_root, "btn_complete");

    if (btn_prev) {
        lv_obj_add_event_cb(btn_prev, on_prev_clicked, LV_EVENT_CLICKED, nullptr);
    }
    if (btn_next) {
        lv_obj_add_event_cb(btn_next, on_next_clicked, LV_EVENT_CLICKED, nullptr);
    }
    if (btn_complete) {
        lv_obj_add_event_cb(btn_complete, on_complete_clicked, LV_EVENT_CLICKED, nullptr);
    }

    LV_LOG_USER("Step progress test panel initialized");
}
