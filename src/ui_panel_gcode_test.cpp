// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 HelixScreen Contributors
 *
 * This file is part of HelixScreen, which is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * See <https://www.gnu.org/licenses/>.
 */

#include "ui_panel_gcode_test.h"

#include "ui_gcode_viewer.h"

#include <spdlog/spdlog.h>

// Panel state
static lv_obj_t* panel_root = nullptr;
static lv_obj_t* gcode_viewer = nullptr;
static lv_obj_t* stats_label = nullptr;

// Path to test G-code file (will be created in assets/)
static const char* TEST_GCODE_PATH = "assets/test.gcode";

// ==============================================
// Event Callbacks
// ==============================================

/**
 * @brief View preset button click handler
 */
static void on_view_preset_clicked(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target_obj(e);
    const char* name = lv_obj_get_name(btn);

    if (!gcode_viewer || !name)
        return;

    spdlog::info("[GCodeTest] View preset clicked: {}", name);

    if (strcmp(name, "btn_isometric") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer, GCODE_VIEWER_VIEW_ISOMETRIC);
    } else if (strcmp(name, "btn_top") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer, GCODE_VIEWER_VIEW_TOP);
    } else if (strcmp(name, "btn_front") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer, GCODE_VIEWER_VIEW_FRONT);
    } else if (strcmp(name, "btn_side") == 0) {
        ui_gcode_viewer_set_view(gcode_viewer, GCODE_VIEWER_VIEW_SIDE);
    } else if (strcmp(name, "btn_reset") == 0) {
        ui_gcode_viewer_reset_camera(gcode_viewer);
    }
}

/**
 * @brief Load test file button click handler
 */
static void on_load_test_file(lv_event_t* e) {
    if (!gcode_viewer)
        return;

    spdlog::info("[GCodeTest] Loading test file: {}", TEST_GCODE_PATH);

    // Load the test G-code file
    ui_gcode_viewer_load_file(gcode_viewer, TEST_GCODE_PATH);

    // Update stats after loading
    // Note: This is synchronous in Phase 1, so state is immediately available
    int layer_count = ui_gcode_viewer_get_layer_count(gcode_viewer);
    gcode_viewer_state_enum_t state = ui_gcode_viewer_get_state(gcode_viewer);

    if (stats_label) {
        if (state == GCODE_VIEWER_STATE_LOADED) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Loaded: %d layers | Drag to rotate", layer_count);
            lv_label_set_text(stats_label, buf);
        } else if (state == GCODE_VIEWER_STATE_ERROR) {
            lv_label_set_text(stats_label, "Error loading file");
        } else {
            lv_label_set_text(stats_label, "Loading...");
        }
    }
}

/**
 * @brief Clear button click handler
 */
static void on_clear(lv_event_t* e) {
    if (!gcode_viewer)
        return;

    spdlog::info("[GCodeTest] Clearing viewer");
    ui_gcode_viewer_clear(gcode_viewer);

    if (stats_label) {
        lv_label_set_text(stats_label, "No file loaded");
    }
}

// ==============================================
// Public API
// ==============================================

lv_obj_t* ui_panel_gcode_test_create(lv_obj_t* parent) {
    // Load XML component (use registered component name, not file path)
    panel_root = (lv_obj_t*)lv_xml_create(parent, "gcode_test_panel", nullptr);
    if (!panel_root) {
        spdlog::error("[GCodeTest] Failed to load XML component");
        return nullptr;
    }

    // Get widget references
    gcode_viewer = lv_obj_find_by_name(panel_root, "gcode_viewer");
    stats_label = lv_obj_find_by_name(panel_root, "stats_label");

    if (!gcode_viewer) {
        spdlog::error("[GCodeTest] Failed to find gcode_viewer widget");
        return panel_root;
    }

    // Register event callbacks
    lv_obj_t* btn_isometric = lv_obj_find_by_name(panel_root, "btn_isometric");
    lv_obj_t* btn_top = lv_obj_find_by_name(panel_root, "btn_top");
    lv_obj_t* btn_front = lv_obj_find_by_name(panel_root, "btn_front");
    lv_obj_t* btn_side = lv_obj_find_by_name(panel_root, "btn_side");
    lv_obj_t* btn_reset = lv_obj_find_by_name(panel_root, "btn_reset");
    lv_obj_t* btn_load = lv_obj_find_by_name(panel_root, "btn_load_test");
    lv_obj_t* btn_clear = lv_obj_find_by_name(panel_root, "btn_clear");

    if (btn_isometric)
        lv_obj_add_event_cb(btn_isometric, on_view_preset_clicked, LV_EVENT_CLICKED, nullptr);
    if (btn_top)
        lv_obj_add_event_cb(btn_top, on_view_preset_clicked, LV_EVENT_CLICKED, nullptr);
    if (btn_front)
        lv_obj_add_event_cb(btn_front, on_view_preset_clicked, LV_EVENT_CLICKED, nullptr);
    if (btn_side)
        lv_obj_add_event_cb(btn_side, on_view_preset_clicked, LV_EVENT_CLICKED, nullptr);
    if (btn_reset)
        lv_obj_add_event_cb(btn_reset, on_view_preset_clicked, LV_EVENT_CLICKED, nullptr);
    if (btn_load)
        lv_obj_add_event_cb(btn_load, on_load_test_file, LV_EVENT_CLICKED, nullptr);
    if (btn_clear)
        lv_obj_add_event_cb(btn_clear, on_clear, LV_EVENT_CLICKED, nullptr);

    spdlog::info("[GCodeTest] Panel created");
    return panel_root;
}

void ui_panel_gcode_test_cleanup(void) {
    // Widgets are automatically cleaned up by LVGL when panel_root is deleted
    panel_root = nullptr;
    gcode_viewer = nullptr;
    stats_label = nullptr;

    spdlog::debug("[GCodeTest] Panel cleaned up");
}
