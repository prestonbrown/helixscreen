// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_BED_MESH_3D

#include "ui_bed_mesh.h"

#include "ui_fonts.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "bed_mesh_overlays.h"
#include "bed_mesh_render_thread.h"
#include "bed_mesh_renderer.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::mesh::BedMeshRenderThread;

#include <cstdlib>
#include <cstring>
#include <memory>

// Canvas dimensions are in ui_bed_mesh.h; camera angles belong to the renderer.

/**
 * Widget instance data stored in user_data
 */
typedef struct {
    // 3D renderer instance.  It owns the camera orientation (angle_x/angle_z in
    // its bed_mesh_view_state_t) — the widget reads the current angles back via
    // bed_mesh_renderer_get_view_state() rather than mirroring them here.
    bed_mesh_renderer_t* renderer;

    // Touch drag state
    bool is_dragging;         // Currently in drag gesture
    lv_point_t last_drag_pos; // Last touch position for delta calculation

    // Deferred redraw state (for panels created while hidden)
    bool had_valid_size;    // Has widget ever had non-zero dimensions
    bool mesh_data_pending; // Mesh data was set before widget had valid size

    // Async rendering (background thread produces pre-rendered frames)
    std::unique_ptr<BedMeshRenderThread> render_thread;
    bool async_mode = false;

    // Persistent blit buffer for async mode.
    // lv_draw_image() defers the actual draw — the src pointer must remain valid
    // until LVGL's SW draw unit processes the task.  A stack-local lv_draw_buf_t
    // would be destroyed before the task runs, causing "Failed to open image".
    // We copy the render thread's front buffer here (under the swap lock) and
    // keep the lv_draw_buf_t pointing to it across frames.
    lv_draw_buf_t blit_draw_buf{};
    std::vector<uint8_t> blit_pixel_data;
    int blit_width = 0;
    int blit_height = 0;
} bed_mesh_widget_data_t;

/**
 * Fetch theme colors needed for off-screen buffer rendering.
 * Must be called on the main thread where theme_manager_get_color is safe.
 */
static bed_mesh_render_colors_t fetch_theme_colors() {
    bed_mesh_render_colors_t colors{};

    lv_color_t bg = theme_manager_get_color("screen_bg");
    colors.bg_r = bg.red;
    colors.bg_g = bg.green;
    colors.bg_b = bg.blue;

    lv_color_t grid = theme_manager_get_color("elevated_bg");
    colors.grid_r = grid.red;
    colors.grid_g = grid.green;
    colors.grid_b = grid.blue;

    return colors;
}

/**
 * Draw a placeholder when the async render thread has not yet produced a frame.
 * Shows a dark background with centered "Rendering..." text.
 */
static void draw_async_placeholder(lv_layer_t* layer, const lv_area_t* coords, int width,
                                   int height) {
    // Dark background rectangle (same color as normal mesh background)
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = theme_manager_get_color("screen_bg");
    rect_dsc.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &rect_dsc, coords);

    // Centered "Rendering..." label
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_white();
    label_dsc.font = &noto_sans_14;
    label_dsc.opa = LV_OPA_60;
    label_dsc.align = LV_TEXT_ALIGN_CENTER;
    label_dsc.text = "Rendering...";

    // Center the label area within the widget
    const int label_w = 120;
    const int label_h = 20;
    lv_area_t label_area;
    label_area.x1 = coords->x1 + (width - label_w) / 2;
    label_area.y1 = coords->y1 + (height - label_h) / 2;
    label_area.x2 = label_area.x1 + label_w;
    label_area.y2 = label_area.y1 + label_h;
    lv_draw_label(layer, &label_dsc, &label_area);
}

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

    // Get widget's absolute screen coordinates (stable across partial redraws)
    lv_area_t widget_coords;
    lv_obj_get_coords(obj, &widget_coords);
    int width = lv_area_get_width(&widget_coords);
    int height = lv_area_get_height(&widget_coords);

    spdlog::trace("[bed_mesh] draw_cb: rendering at {}x{}", width, height);

    if (width <= 0 || height <= 0) {
        spdlog::debug("[bed_mesh] draw_cb: invalid dimensions {}x{}", width, height);
        return;
    }

    // Async mode: blit pre-rendered buffer from render thread
    if (data->async_mode && data->render_thread) {
        // Lock the front buffer and copy into persistent blit buffer.
        // lv_draw_image() defers the actual draw — by the time LVGL's SW draw
        // unit processes the task, a stack-local lv_draw_buf_t would be destroyed.
        // We copy the pixel data into widget-owned storage so the lv_draw_buf_t
        // and its backing memory remain valid until the next draw callback.
        {
            auto locked = data->render_thread->lock_ready_buffer();
            if (locked) {
                const auto* buf = locked.buffer;
                uint32_t data_size = (uint32_t)(buf->stride() * buf->height());

                // Reallocate persistent buffer if dimensions changed
                if (data->blit_width != buf->width() || data->blit_height != buf->height()) {
                    data->blit_pixel_data.resize(data_size);
                    data->blit_width = buf->width();
                    data->blit_height = buf->height();
                }

                // Copy pixels under the swap lock
                std::memcpy(data->blit_pixel_data.data(), buf->data(), data_size);

                // (Re)initialize the persistent draw buf pointing to our copy
                lv_draw_buf_init(&data->blit_draw_buf, (uint32_t)buf->width(),
                                 (uint32_t)buf->height(), LV_COLOR_FORMAT_ARGB8888,
                                 (uint32_t)buf->stride(), data->blit_pixel_data.data(), data_size);

                spdlog::trace("[bed_mesh] Async blit {}x{} ({:.1f}ms render)", buf->width(),
                              buf->height(), data->render_thread->last_render_time_ms());
            }
        } // swap lock released — blit_pixel_data is our own copy, safe to use

        if (data->blit_width > 0 && data->blit_height > 0) {
            lv_draw_image_dsc_t img_dsc;
            lv_draw_image_dsc_init(&img_dsc);
            img_dsc.src = &data->blit_draw_buf;

            lv_area_t area;
            area.x1 = widget_coords.x1;
            area.y1 = widget_coords.y1;
            area.x2 = widget_coords.x1 + data->blit_width - 1;
            area.y2 = widget_coords.y1 + data->blit_height - 1;

            lv_draw_image(layer, &img_dsc, &area);
        } else {
            // No frame ready yet — draw placeholder
            draw_async_placeholder(layer, &widget_coords, width, height);

            // Schedule a re-invalidation so we pick up the ready buffer next frame
            helix::ui::queue_widget_update(obj, [](lv_obj_t* w) { lv_obj_invalidate(w); });
        }

        // Render axis labels and tick marks on the main thread.
        // These require the LVGL font engine and cannot run in the background.
        // render_to_buffer() sets layer_offset=(0,0) for buffer-local rendering,
        // but axis labels are drawn via LVGL's draw API which uses absolute screen
        // coordinates.  Temporarily set the layer offset to the widget's screen
        // position so projected 3D→2D coordinates land at the correct screen location.
        {
            std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
            int saved_offset_x, saved_offset_y;
            bed_mesh_renderer_get_layer_offset(data->renderer, &saved_offset_x, &saved_offset_y);
            bed_mesh_renderer_set_layer_offset(data->renderer, widget_coords.x1, widget_coords.y1);

            helix::mesh::render_axis_labels(layer, data->renderer, width, height);
            helix::mesh::render_numeric_axis_ticks(layer, data->renderer, width, height);

            bed_mesh_renderer_set_layer_offset(data->renderer, saved_offset_x, saved_offset_y);
        }

        return; // Skip synchronous render path
    }

    // Synchronous render path (original behavior)
    if (!bed_mesh_renderer_render(data->renderer, layer, width, height, widget_coords.x1,
                                  widget_coords.y1)) {
        return;
    }

    spdlog::trace("[bed_mesh] Render complete");
}

/**
 * Touch press event handler - start drag gesture or show 2D tooltip
 */
static void bed_mesh_press_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data || !data->renderer)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Get widget's absolute screen coordinates (not relative to parent)
    // lv_indev_get_point returns screen coordinates, so we need absolute position
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int local_x = point.x - obj_coords.x1;
    int local_y = point.y - obj_coords.y1;
    int width = lv_area_get_width(&obj_coords);
    int height = lv_area_get_height(&obj_coords);

    // In 2D mode: show cell tooltip on touch
    if (bed_mesh_renderer_is_using_2d(data->renderer)) {
        if (bed_mesh_renderer_handle_touch(data->renderer, local_x, local_y, width, height)) {
            lv_obj_invalidate(obj); // Redraw to show tooltip
            spdlog::trace("[bed_mesh] 2D touch at ({}, {}) - showing tooltip", local_x, local_y);
        }
        return; // Don't start dragging in 2D mode
    }

    // 3D mode: start drag gesture
    data->is_dragging = true;
    data->last_drag_pos = point;

    // Update renderer dragging state for fast solid-color rendering.
    // In async mode, lock the render mutex to prevent concurrent access
    // with the background render thread.
    if (data->async_mode && data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        bed_mesh_renderer_set_dragging(data->renderer, true);
    } else {
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

    if (!data || !data->renderer)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    // In 2D mode: update tooltip as finger drags across cells
    if (bed_mesh_renderer_is_using_2d(data->renderer)) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);

        // Get widget's absolute screen coordinates
        lv_area_t obj_coords;
        lv_obj_get_coords(obj, &obj_coords);
        int local_x = point.x - obj_coords.x1;
        int local_y = point.y - obj_coords.y1;
        int width = lv_area_get_width(&obj_coords);
        int height = lv_area_get_height(&obj_coords);

        // Update touch position - if cell changed, redraw
        if (bed_mesh_renderer_handle_touch(data->renderer, local_x, local_y, width, height)) {
            lv_obj_invalidate(obj);
        }
        return;
    }

    // 3D mode: handle rotation drag
    if (!data->is_dragging)
        return;

    // Safety check: verify input device is still pressed
    lv_indev_state_t state = lv_indev_get_state(indev);
    if (state != LV_INDEV_STATE_PRESSED) {
        // Input was released but we missed the event - force cleanup
        spdlog::warn("[bed_mesh] Detected missed release event (state={}), forcing gradient mode",
                     (int)state);
        data->is_dragging = false;
        if (data->renderer) {
            if (data->async_mode && data->render_thread) {
                std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
                bed_mesh_renderer_set_dragging(data->renderer, false);
            } else {
                bed_mesh_renderer_set_dragging(data->renderer, false);
            }
        }
        lv_obj_invalidate(obj); // Trigger redraw with gradient
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
        // Horizontal drag (dx) = spin rotation (angle_z)
        // Vertical drag (dy) = tilt rotation (angle_x), inverted for intuitive control
        const double delta_z = dx * 0.5;
        const double delta_x = -dy * 0.5; // Flip Y for intuitive tilt

        // Accumulate onto the renderer's own camera angles.  Clamping of the
        // tilt and 360° wrapping of the spin happen inside set_rotation().
        double angle_x = 0.0;
        double angle_z = 0.0;
        auto apply_rotation = [&]() {
            const bed_mesh_view_state_t* view = bed_mesh_renderer_get_view_state(data->renderer);
            if (!view) {
                return;
            }
            bed_mesh_renderer_set_rotation(data->renderer, view->angle_x + delta_x,
                                           view->angle_z + delta_z);
            // view aliases the renderer's live state, so it now holds the
            // clamped/wrapped result.
            angle_x = view->angle_x;
            angle_z = view->angle_z;
        };

        // In async mode, lock the render mutex to prevent concurrent access
        // with the background render thread, then request a new frame.
        if (data->async_mode && data->render_thread) {
            std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
            apply_rotation();
        } else {
            apply_rotation();
        }

        // Trigger redraw (async mode: request new frame from render thread)
        if (data->async_mode && data->render_thread) {
            data->render_thread->request_render();
        }
        lv_obj_invalidate(obj);

        data->last_drag_pos = point;

        spdlog::trace("[bed_mesh] Drag ({}, {}) -> rotation({:.1f}, {:.1f})", dx, dy, angle_x,
                      angle_z);
    }
}

/**
 * Touch release event handler - end drag gesture or hide 2D tooltip
 */
static void bed_mesh_release_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data || !data->renderer)
        return;

    // In 2D mode: clear tooltip on release
    if (bed_mesh_renderer_is_using_2d(data->renderer)) {
        bed_mesh_renderer_clear_touch(data->renderer);
        lv_obj_invalidate(obj); // Redraw to hide tooltip
        spdlog::trace("[bed_mesh] 2D touch released - hiding tooltip");
        return;
    }

    // 3D mode: end drag gesture
    data->is_dragging = false;

    // Update renderer dragging state for high-quality gradient rendering.
    // In async mode, lock the render mutex to prevent concurrent access.
    if (data->async_mode && data->render_thread) {
        {
            std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
            bed_mesh_renderer_set_dragging(data->renderer, false);
        }
        data->render_thread->request_render();
    } else {
        bed_mesh_renderer_set_dragging(data->renderer, false);
    }

    // Force immediate redraw to switch back to gradient rendering
    lv_obj_invalidate(obj);

    const bed_mesh_view_state_t* view = bed_mesh_renderer_get_view_state(data->renderer);
    spdlog::trace("[bed_mesh] Release - final rotation({:.1f}, {:.1f}), switching to gradient",
                  view ? view->angle_x : 0.0, view ? view->angle_z : 0.0);
}

/**
 * Size changed event handler - update widget on resize
 *
 * Critical for panels created while hidden: When mesh data is set before the
 * widget has valid dimensions, we defer the redraw until SIZE_CHANGED fires
 * with non-zero dimensions.
 */
static void bed_mesh_size_changed_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    // Get new widget dimensions
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int width = lv_area_get_width(&coords);
    int height = lv_area_get_height(&coords);

    spdlog::trace("[bed_mesh] SIZE_CHANGED: {}x{}", width, height);

    // Check if this is the first time we have valid dimensions
    if (data && width > 0 && height > 0 && !data->had_valid_size) {
        data->had_valid_size = true;
        spdlog::trace("[bed_mesh] First valid size received");

        // If mesh data was set while widget was 0x0, force a proper redraw now
        if (data->mesh_data_pending) {
            data->mesh_data_pending = false;
            spdlog::info("[bed_mesh] Triggering deferred render after gaining valid size");
        }
    }

    // Restart render thread with new dimensions if in async mode
    if (data && data->async_mode && data->render_thread && width > 0 && height > 0) {
        data->render_thread->stop();
        data->render_thread->set_colors(fetch_theme_colors());
        data->render_thread->start(width, height);
        data->render_thread->request_render();
        spdlog::debug("[bed_mesh] Restarted async render thread for new size {}x{}", width, height);
    }

    // Trigger redraw with new dimensions
    lv_obj_invalidate(obj);
}

/**
 * Delete event handler - cleanup resources
 */
static void bed_mesh_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);

    // Wrap raw pointer in unique_ptr for RAII cleanup
    std::unique_ptr<bed_mesh_widget_data_t> data(
        static_cast<bed_mesh_widget_data_t*>(lv_obj_get_user_data(obj)));
    lv_obj_set_user_data(obj, nullptr);

    if (data) {
        // Stop render thread before destroying renderer (thread holds renderer pointer)
        if (data->render_thread) {
            data->render_thread.reset();
            spdlog::trace("[bed_mesh] Stopped render thread");
        }

        // Destroy renderer
        if (data->renderer) {
            bed_mesh_renderer_destroy(data->renderer);
            data->renderer = nullptr;
            spdlog::trace("[bed_mesh] Destroyed renderer");
        }
        // data automatically freed via ~unique_ptr()
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
        return nullptr;
    }

    // Configure appearance (transparent background, no border, no padding)
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE); // Touch events work automatically!

    // Allocate widget data struct with RAII (exception-safe)
    auto data_ptr = std::make_unique<bed_mesh_widget_data_t>();

    // Create renderer
    data_ptr->renderer = bed_mesh_renderer_create();
    if (!data_ptr->renderer) {
        spdlog::error("[bed_mesh] Failed to create renderer");
        helix::ui::safe_delete(obj);
        return nullptr; // unique_ptr automatically cleans up
    }

    // Camera starts at the renderer's own defaults (BED_MESH_DEFAULT_ANGLE_X/Z),
    // set by bed_mesh_renderer_create() — no need to push them back in here.

    // Check for forced 2D mode via environment variable (for testing)
    const char* force_2d = std::getenv("HELIX_BED_MESH_2D");
    if (force_2d && std::strcmp(force_2d, "1") == 0) {
        bed_mesh_renderer_set_render_mode(data_ptr->renderer, BedMeshRenderMode::Force2D);
        spdlog::info("[bed_mesh] 2D heatmap mode forced via HELIX_BED_MESH_2D=1");
    }

    // Initialize touch drag state
    data_ptr->is_dragging = false;
    data_ptr->last_drag_pos = {0, 0};

    // Initialize deferred redraw state (for panels created while hidden)
    data_ptr->had_valid_size = false;
    data_ptr->mesh_data_pending = false;

    // Transfer ownership to LVGL user_data (will be cleaned up in delete callback)
    lv_obj_set_user_data(obj, data_ptr.release());

    // Register event handlers
    lv_obj_add_event_cb(obj, bed_mesh_draw_cb, LV_EVENT_DRAW_POST, nullptr); // Custom drawing
    lv_obj_add_event_cb(obj, bed_mesh_size_changed_cb, LV_EVENT_SIZE_CHANGED,
                        nullptr);                                           // Handle resize
    lv_obj_add_event_cb(obj, bed_mesh_delete_cb, LV_EVENT_DELETE, nullptr); // Cleanup

    // Register touch event handlers for drag rotation
    lv_obj_add_event_cb(obj, bed_mesh_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, bed_mesh_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(obj, bed_mesh_release_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(obj, bed_mesh_release_cb, LV_EVENT_PRESS_LOST,
                        nullptr); // Handle drag outside widget

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
    spdlog::trace("[bed_mesh] Registered <bed_mesh> widget with XML system");
}

/**
 * Set mesh data for rendering
 *
 * If the widget hasn't been laid out yet (0x0 dimensions), the mesh data is
 * stored in the renderer but actual rendering is deferred until SIZE_CHANGED
 * fires with valid dimensions.
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

    // bed_mesh_renderer_set_mesh_data does mesh.clear()/resize(); the background
    // render thread iterates renderer->mesh[row][col] under render_mutex_. Lock
    // before mutating to avoid reading freed/resizing storage.
    bool set_ok;
    if (data->async_mode && data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        set_ok = bed_mesh_renderer_set_mesh_data(data->renderer, mesh, rows, cols);
    } else {
        set_ok = bed_mesh_renderer_set_mesh_data(data->renderer, mesh, rows, cols);
    }
    if (!set_ok) {
        spdlog::error("[bed_mesh] Failed to set mesh data in renderer");
        return false;
    }

    // Reset adaptive quality state since new mesh may render differently
    if (data->render_thread) {
        data->render_thread->reset_quality();
    }

    // Check if widget has valid dimensions yet
    int width = lv_obj_get_width(widget);
    int height = lv_obj_get_height(widget);

    if (width <= 0 || height <= 0) {
        // Widget hasn't been laid out yet - defer rendering to SIZE_CHANGED
        data->mesh_data_pending = true;
        spdlog::info("[bed_mesh] Mesh data loaded: {}x{} (deferred - widget {}x{})", rows, cols,
                     width, height);
    } else {
        data->mesh_data_pending = false;
        spdlog::info("[bed_mesh] Mesh data loaded: {}x{}", rows, cols);
    }

    // Request redraw (will succeed if widget has valid size, otherwise deferred)
    ui_bed_mesh_redraw(widget);

    return true;
}

/**
 * Set coordinate bounds for bed and mesh
 */
void ui_bed_mesh_set_bounds(lv_obj_t* widget, double bed_x_min, double bed_x_max, double bed_y_min,
                            double bed_y_max, double mesh_x_min, double mesh_x_max,
                            double mesh_y_min, double mesh_y_max) {
    if (!widget) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_bounds: NULL widget");
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_bounds: widget data or renderer not initialized");
        return;
    }

    if (data->async_mode && data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        bed_mesh_renderer_set_bounds(data->renderer, bed_x_min, bed_x_max, bed_y_min, bed_y_max,
                                     mesh_x_min, mesh_x_max, mesh_y_min, mesh_y_max);
    } else {
        bed_mesh_renderer_set_bounds(data->renderer, bed_x_min, bed_x_max, bed_y_min, bed_y_max,
                                     mesh_x_min, mesh_x_max, mesh_y_min, mesh_y_max);
    }

    // Request redraw to show updated bounds
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

    // In async mode, request a new frame from the render thread.
    // The thread's frame-ready callback will invalidate the widget when done.
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (data && data->async_mode && data->render_thread) {
        data->render_thread->request_render();
        spdlog::debug("[bed_mesh] Async redraw requested");
        return;
    }

    // Synchronous path: trigger DRAW_POST event by invalidating widget
    lv_obj_invalidate(widget);

    spdlog::debug("[bed_mesh] Redraw requested");
}

/**
 * Evaluate and possibly switch render mode based on FPS history
 *
 * Should be called when the bed mesh panel becomes visible.
 * Mode evaluation only happens on panel entry, never during viewing.
 */
void ui_bed_mesh_evaluate_render_mode(lv_obj_t* widget) {
    if (!widget) {
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        return;
    }

    bed_mesh_renderer_evaluate_render_mode(data->renderer);
}

/**
 * Set render mode (for settings UI)
 */
void ui_bed_mesh_set_render_mode(lv_obj_t* widget, BedMeshRenderMode mode) {
    if (!widget) {
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        return;
    }

    if (data->async_mode && data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        bed_mesh_renderer_set_render_mode(data->renderer, mode);
    } else {
        bed_mesh_renderer_set_render_mode(data->renderer, mode);
    }
    ui_bed_mesh_redraw(widget); // Redraw with new mode (handles async)
}

/**
 * Show or hide the zero reference plane
 */
void ui_bed_mesh_set_zero_plane_visible(lv_obj_t* widget, bool visible) {
    if (!widget) {
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        return;
    }

    if (data->async_mode && data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        bed_mesh_renderer_set_zero_plane_visible(data->renderer, visible);
    } else {
        bed_mesh_renderer_set_zero_plane_visible(data->renderer, visible);
    }
    ui_bed_mesh_redraw(widget); // Redraw with updated plane visibility (handles async)
}

/**
 * Set Z display offset for axis labels
 *
 * When mesh data is normalized (mean-subtracted), this offset is added back
 * so axis labels and tooltips show original probe heights.
 */
void ui_bed_mesh_set_z_display_offset(lv_obj_t* widget, double offset_mm) {
    if (!widget) {
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        return;
    }

    if (data->async_mode && data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        bed_mesh_renderer_set_z_display_offset(data->renderer, offset_mm);
    } else {
        bed_mesh_renderer_set_z_display_offset(data->renderer, offset_mm);
    }
}

/**
 * Enable or disable async rendering mode
 */
void ui_bed_mesh_set_async_mode(lv_obj_t* widget, bool enabled) {
    if (!widget) {
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        return;
    }

    if (enabled == data->async_mode) {
        return; // Already in requested mode
    }

    if (enabled) {
        int width = lv_obj_get_width(widget);
        int height = lv_obj_get_height(widget);

        data->async_mode = true;
        data->render_thread = std::make_unique<BedMeshRenderThread>();
        data->render_thread->set_renderer(data->renderer);
        data->render_thread->set_colors(fetch_theme_colors());

        // Set callback to invalidate widget when frame ready (from render thread).
        // queue_widget_update() guards with lv_obj_is_valid(), so if the widget
        // is deleted before the queued callback fires, it is silently skipped.
        data->render_thread->set_frame_ready_callback([widget]() {
            helix::ui::queue_widget_update(widget, [](lv_obj_t* w) { lv_obj_invalidate(w); });
        });

        if (width > 0 && height > 0) {
            data->render_thread->start(width, height);
            data->render_thread->request_render();
        }

        spdlog::info("[bed_mesh] Async rendering enabled ({}x{})", width, height);
    } else {
        data->render_thread.reset(); // Stops and destroys
        data->async_mode = false;
        lv_obj_invalidate(widget); // Redraw synchronously
        spdlog::info("[bed_mesh] Async rendering disabled");
    }
}

/**
 * Check if async rendering mode is enabled
 */
bool ui_bed_mesh_is_async_mode(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data) {
        return false;
    }

    return data->async_mode;
}

/**
 * Request the render thread to produce a new frame
 */
void ui_bed_mesh_request_async_render(lv_obj_t* widget) {
    if (!widget) {
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data) {
        return;
    }

    if (data->async_mode && data->render_thread) {
        data->render_thread->request_render();
    }
}

bool ui_bed_mesh_has_data(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        return false;
    }

    return bed_mesh_renderer_has_data(data->renderer);
}

#else // !HELIX_HAS_BED_MESH_3D

// Compiled-out build (HELIX_HAS_BED_MESH_3D=0): stub widget keeps XML layouts
// parsing (unregistered tags corrupt sibling parenting); API is no-op.

#include "ui_bed_mesh.h"

#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"

#include <spdlog/spdlog.h>

static void* bed_mesh_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs;
    return (void*)lv_obj_create((lv_obj_t*)lv_xml_state_get_parent(state));
}

static void bed_mesh_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    lv_xml_obj_apply(state, attrs);
}

void ui_bed_mesh_register(void) {
    lv_xml_register_widget("bed_mesh", bed_mesh_xml_create, bed_mesh_xml_apply);
    spdlog::debug("[bed_mesh] Compiled out (HELIX_HAS_BED_MESH_3D=0); registered stub "
                  "<bed_mesh> widget");
}

bool ui_bed_mesh_set_data(lv_obj_t*, const float* const*, int, int) {
    return false;
}

void ui_bed_mesh_set_bounds(lv_obj_t*, double, double, double, double, double, double, double,
                            double) {}

void ui_bed_mesh_set_rotation(lv_obj_t*, int, int) {}

void ui_bed_mesh_redraw(lv_obj_t*) {}

void ui_bed_mesh_evaluate_render_mode(lv_obj_t*) {}

helix::BedMeshRenderMode ui_bed_mesh_get_render_mode(lv_obj_t*) {
    return helix::BedMeshRenderMode::Auto;
}

void ui_bed_mesh_set_render_mode(lv_obj_t*, helix::BedMeshRenderMode) {}

void ui_bed_mesh_set_zero_plane_visible(lv_obj_t*, bool) {}

void ui_bed_mesh_set_z_display_offset(lv_obj_t*, double) {}

void ui_bed_mesh_set_async_mode(lv_obj_t*, bool) {}

bool ui_bed_mesh_is_async_mode(lv_obj_t*) {
    return false;
}

void ui_bed_mesh_request_async_render(lv_obj_t*) {}

bool ui_bed_mesh_has_data(lv_obj_t*) {
    return false;
}

#endif // HELIX_HAS_BED_MESH_3D
