// Copyright 2025 HelixScreen
// SPDX-License-Identifier: GPL-3.0-or-later

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

#include "ui_bed_mesh.h"

#include "bed_mesh_renderer.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/xml/lv_xml.h"
#include "lvgl/src/xml/lv_xml_parser.h"
#include "lvgl/src/xml/lv_xml_widget.h"
#include "lvgl/src/xml/parsers/lv_xml_obj_parser.h"

#include <spdlog/spdlog.h>

#include <cstdlib>

// Canvas dimensions and rotation defaults are now in ui_bed_mesh.h

/**
 * Widget instance data stored in user_data
 */
typedef struct {
    bed_mesh_renderer_t* renderer; // 3D renderer instance
    int rotation_x;                // Current tilt angle (degrees)
    int rotation_z;                // Current spin angle (degrees)

    // Touch drag state
    bool is_dragging;              // Currently in drag gesture
    lv_point_t last_drag_pos;      // Last touch position for delta calculation
} bed_mesh_widget_data_t;

/**
 * Draw event handler - renders bed mesh using DRAW_POST pattern
 */
static void bed_mesh_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data || !layer) {
        return;
    }

    if (!data->renderer) {
        spdlog::warn("[bed_mesh] draw_cb: renderer not initialized");
        return;
    }

    // Get widget dimensions
    int width = lv_obj_get_width(obj);
    int height = lv_obj_get_height(obj);

    if (width <= 0 || height <= 0) {
        spdlog::debug("[bed_mesh] draw_cb: invalid dimensions {}x{}", width, height);
        return;
    }

    // Render mesh directly to layer (matches G-code viewer pattern)
    if (!bed_mesh_renderer_render(data->renderer, layer, width, height)) {
        // Render failed - likely no mesh data loaded
        // Draw "No mesh loaded" message in center of canvas
        lv_draw_label_dsc_t label_dsc;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = lv_color_hex(0x808080); // Gray text
        label_dsc.text = "No mesh loaded";
        label_dsc.font = &lv_font_montserrat_16;
        label_dsc.align = LV_TEXT_ALIGN_CENTER;

        // Calculate centered position
        lv_point_t txt_size;
        lv_text_get_size(&txt_size, label_dsc.text, label_dsc.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        lv_area_t label_area;
        label_area.x1 = (width - txt_size.x) / 2;
        label_area.y1 = (height - txt_size.y) / 2;
        label_area.x2 = label_area.x1 + txt_size.x;
        label_area.y2 = label_area.y1 + txt_size.y;

        lv_draw_label(layer, &label_dsc, &label_area);
        return;
    }

    spdlog::trace("[bed_mesh] Render complete");
}

/**
 * Touch press event handler - start drag gesture
 */
static void bed_mesh_press_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    data->is_dragging = true;
    data->last_drag_pos = point;

    // Update renderer dragging state for fast solid-color rendering
    if (data->renderer) {
        bed_mesh_renderer_set_dragging(data->renderer, true);
    }

    spdlog::trace("[bed_mesh] Press at ({}, {}), switching to solid", point.x, point.y);
}

/**
 * Touch pressing event handler - handle drag for rotation
 */
static void bed_mesh_pressing_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data || !data->is_dragging)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    // Safety check: verify input device is still pressed
    lv_indev_state_t state = lv_indev_get_state(indev);
    if (state != LV_INDEV_STATE_PRESSED) {
        // Input was released but we missed the event - force cleanup
        spdlog::warn("[bed_mesh] Detected missed release event (state={}), forcing gradient mode", (int)state);
        data->is_dragging = false;
        if (data->renderer) {
            bed_mesh_renderer_set_dragging(data->renderer, false);
        }
        lv_obj_invalidate(obj);  // Trigger redraw with gradient
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Calculate delta from last position
    int dx = point.x - data->last_drag_pos.x;
    int dy = point.y - data->last_drag_pos.y;

    if (dx != 0 || dy != 0) {
        // Convert pixel movement to rotation angles
        // Scale factor: ~0.5 degrees per pixel (matching G-code viewer)
        // Horizontal drag (dx) = spin rotation (rotation_z)
        // Vertical drag (dy) = tilt rotation (rotation_x), inverted for intuitive control
        data->rotation_z += (int)(dx * 0.5f);
        data->rotation_x -= (int)(dy * 0.5f); // Flip Y for intuitive tilt

        // Clamp tilt to reasonable range (-90 to 0 degrees)
        if (data->rotation_x < -90)
            data->rotation_x = -90;
        if (data->rotation_x > 0)
            data->rotation_x = 0;

        // Wrap spin around 360 degrees
        data->rotation_z = data->rotation_z % 360;
        if (data->rotation_z < 0)
            data->rotation_z += 360;

        // Update renderer rotation
        if (data->renderer) {
            bed_mesh_renderer_set_rotation(data->renderer, data->rotation_x, data->rotation_z);
        }

        // Trigger redraw
        lv_obj_invalidate(obj);

        data->last_drag_pos = point;

        spdlog::trace("[bed_mesh] Drag ({}, {}) -> rotation({}, {})", dx, dy, data->rotation_x,
                      data->rotation_z);
    }
}

/**
 * Touch release event handler - end drag gesture
 */
static void bed_mesh_release_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data)
        return;

    data->is_dragging = false;

    // Update renderer dragging state for high-quality gradient rendering
    if (data->renderer) {
        bed_mesh_renderer_set_dragging(data->renderer, false);
    }

    // Force immediate redraw to switch back to gradient rendering
    lv_obj_invalidate(obj);

    spdlog::trace("[bed_mesh] Release - final rotation({}, {}), switching to gradient", data->rotation_x, data->rotation_z);
}

/**
 * Delete event handler - cleanup resources
 */
static void bed_mesh_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (data) {
        // Destroy renderer
        if (data->renderer) {
            bed_mesh_renderer_destroy(data->renderer);
            data->renderer = nullptr;
            spdlog::debug("[bed_mesh] Destroyed renderer");
        }

        // Free widget data struct
        free(data);
        lv_obj_set_user_data(obj, NULL);
    }
}

/**
 * XML create handler for <bed_mesh>
 * Creates base object and uses DRAW_POST callback for rendering
 * (Architecture matches G-code viewer for touch event handling)
 */
static void* bed_mesh_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    LV_UNUSED(attrs);

    void* parent = lv_xml_state_get_parent(state);

    // Create base object (NOT canvas!)
    lv_obj_t* obj = lv_obj_create((lv_obj_t*)parent);
    if (!obj) {
        spdlog::error("[bed_mesh] Failed to create object");
        return NULL;
    }

    // Configure appearance (transparent background, no border, no padding)
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE); // Touch events work automatically!

    // Allocate widget data struct
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)malloc(sizeof(bed_mesh_widget_data_t));
    if (!data) {
        spdlog::error("[bed_mesh] Failed to allocate widget data");
        lv_obj_delete(obj);
        return NULL;
    }

    // Create renderer
    data->renderer = bed_mesh_renderer_create();
    if (!data->renderer) {
        spdlog::error("[bed_mesh] Failed to create renderer");
        free(data);
        lv_obj_delete(obj);
        return NULL;
    }

    // Set default rotation angles
    data->rotation_x = BED_MESH_ROTATION_X_DEFAULT;
    data->rotation_z = BED_MESH_ROTATION_Z_DEFAULT;
    bed_mesh_renderer_set_rotation(data->renderer, data->rotation_x, data->rotation_z);

    // Initialize touch drag state
    data->is_dragging = false;
    data->last_drag_pos = {0, 0};

    // Store widget data in user_data for cleanup and API access
    lv_obj_set_user_data(obj, data);

    // Register event handlers
    lv_obj_add_event_cb(obj, bed_mesh_draw_cb, LV_EVENT_DRAW_POST, NULL);     // Custom drawing
    lv_obj_add_event_cb(obj, bed_mesh_delete_cb, LV_EVENT_DELETE, NULL);      // Cleanup

    // Register touch event handlers for drag rotation
    lv_obj_add_event_cb(obj, bed_mesh_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, bed_mesh_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, bed_mesh_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, bed_mesh_release_cb, LV_EVENT_PRESS_LOST, NULL);  // Handle drag outside widget

    // Set default size (will be overridden by XML width/height attributes)
    lv_obj_set_size(obj, BED_MESH_CANVAS_WIDTH, BED_MESH_CANVAS_HEIGHT);

    spdlog::debug("[bed_mesh] Created widget with DRAW_POST pattern, renderer initialized");

    return (void*)obj;
}

/**
 * XML apply handler for <bed_mesh>
 * Applies standard lv_obj attributes from XML
 */
static void bed_mesh_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = (lv_obj_t*)item;

    if (!obj) {
        spdlog::error("[bed_mesh] NULL object in xml_apply");
        return;
    }

    // Apply standard lv_obj properties from XML (size, style, align, etc.)
    lv_xml_obj_apply(state, attrs);

    spdlog::trace("[bed_mesh] Applied XML attributes");
}

/**
 * Register <bed_mesh> widget with LVGL XML system
 */
void ui_bed_mesh_register(void) {
    lv_xml_register_widget("bed_mesh", bed_mesh_xml_create, bed_mesh_xml_apply);
    spdlog::info("[bed_mesh] Registered <bed_mesh> widget with XML system");
}

/**
 * Set mesh data for rendering
 */
bool ui_bed_mesh_set_data(lv_obj_t* widget, const float* const* mesh, int rows, int cols) {
    if (!widget) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_data: NULL widget");
        return false;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_data: widget data or renderer not initialized");
        return false;
    }

    if (!mesh || rows <= 0 || cols <= 0) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_data: invalid mesh data (rows={}, cols={})", rows,
                      cols);
        return false;
    }

    // Set mesh data in renderer
    if (!bed_mesh_renderer_set_mesh_data(data->renderer, mesh, rows, cols)) {
        spdlog::error("[bed_mesh] Failed to set mesh data in renderer");
        return false;
    }

    spdlog::info("[bed_mesh] Mesh data loaded: {}x{}", rows, cols);

    // Automatically redraw after setting new data
    ui_bed_mesh_redraw(widget);

    return true;
}

/**
 * Set camera rotation angles
 */
void ui_bed_mesh_set_rotation(lv_obj_t* widget, int angle_x, int angle_z) {
    if (!widget) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_rotation: NULL widget");
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_rotation: widget data or renderer not "
                      "initialized");
        return;
    }

    // Update stored rotation angles
    data->rotation_x = angle_x;
    data->rotation_z = angle_z;

    // Update renderer
    bed_mesh_renderer_set_rotation(data->renderer, angle_x, angle_z);

    spdlog::debug("[bed_mesh] Rotation updated: tilt={}°, spin={}°", angle_x, angle_z);

    // Automatically redraw after rotation change
    ui_bed_mesh_redraw(widget);
}

/**
 * Force redraw of mesh visualization
 */
void ui_bed_mesh_redraw(lv_obj_t* widget) {
    if (!widget) {
        spdlog::warn("[bed_mesh] ui_bed_mesh_redraw: NULL widget");
        return;
    }

    // Trigger DRAW_POST event by invalidating widget
    lv_obj_invalidate(widget);

    spdlog::debug("[bed_mesh] Redraw requested");
}
