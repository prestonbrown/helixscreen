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
#include "env_knobs.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix-xml/src/xml/parsers/lv_xml_obj_parser.h"
#include "lvgl/lvgl.h"
#include "theme_manager.h"
#include "view_gestures.h"

#include <spdlog/spdlog.h>

using namespace helix;
using helix::mesh::BedMeshRenderThread;

#include <chrono>
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

    // Two-finger pan/zoom (3D only). two_finger_occurred holds one-finger
    // rotate off until every finger is up.
    helix::ui::TwoFingerState two_finger;
    bool two_finger_occurred = false;

    // Deferred redraw state (for panels created while hidden)
    bool had_valid_size;    // Has widget ever had non-zero dimensions
    bool mesh_data_pending; // Mesh data was set before widget had valid size

    // Background render thread; null while rendering is off (no data, or panel hidden)
    std::unique_ptr<BedMeshRenderThread> render_thread;

    // Draw descriptor for the async frame. lv_draw_image() defers the draw, so the
    // lv_draw_buf_t must outlive the draw callback; it points straight into the
    // render thread's consumer-owned frame (BedMeshRenderThread::acquire_frame),
    // which stays unchanged until the next acquire_frame() on this thread.
    lv_draw_buf_t blit_draw_buf{};
    int blit_width = 0;
    int blit_height = 0;
    helix::mesh::FrameInfo shown_info; // What the blitted frame shows; valid while blit_width > 0

    // Static mode: no render thread and no interaction. The frame is rendered on the main
    // thread once per change (mesh, bounds, render mode or size) and blitted until the next.
    bool static_mode = false;
    bool static_dirty = false;
    float static_render_ms = 0.0f; // cost of the last static render
    int static_render_count = 0;   // static renders so far
    // A 3D render overran the budget; this widget draws 2D from then on.
    bool static_3d_banned = false;
    // theme_manager_get_changed_subject() when the frame was rendered; the frame bakes the
    // theme colours in.
    int static_theme_gen = -1;
    std::unique_ptr<helix::mesh::PixelBuffer> static_frame;
} bed_mesh_widget_data_t;

/**
 * Run fn against the renderer, holding the render mutex while the render thread
 * exists so the mutation cannot interleave with a frame in progress.
 */
template <typename F> static void with_renderer(bed_mesh_widget_data_t* data, F&& fn) {
    if (data->render_thread) {
        std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
        fn();
    } else {
        fn();
    }
}

/**
 * Whether touches and overlays treat the canvas as the 2D heatmap. Follows the frame
 * on screen, which lags a mode switch until the next render lands.
 */
static bool shows_heatmap(const bed_mesh_widget_data_t* data) {
    if (data->blit_width > 0) {
        return data->shown_info.heatmap;
    }
    return bed_mesh_renderer_is_using_2d(data->renderer);
}

/** Heatmap layout of the frame on screen; invalid when no frame is shown. */
static helix::mesh::HeatmapLayout shown_heatmap_layout(const bed_mesh_widget_data_t* data) {
    if (data->blit_width <= 0) {
        return {};
    }
    return helix::mesh::compute_heatmap_layout(data->shown_info.rows, data->shown_info.cols,
                                               data->blit_width, data->blit_height);
}

/** 2D touch: hit-test against the shown frame and redraw the tooltip if it hit. */
static void heatmap_touch(lv_obj_t* obj, bed_mesh_widget_data_t* data, lv_indev_t* indev) {
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);
    int local_x = point.x - obj_coords.x1;
    int local_y = point.y - obj_coords.y1;
    if (bed_mesh_renderer_handle_touch(data->renderer, local_x, local_y,
                                       shown_heatmap_layout(data))) {
        lv_obj_invalidate(obj);
        spdlog::trace("[bed_mesh] 2D touch at ({}, {})", local_x, local_y);
    }
}

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
 * Shows the screen background with centered "Rendering..." text.
 */
static void draw_async_placeholder(lv_layer_t* layer, const lv_area_t* coords, int width,
                                   int height) {
    // Screen background rectangle (same color as normal mesh background)
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = theme_manager_get_color("screen_bg");
    rect_dsc.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &rect_dsc, coords);

    // Centered "Rendering..." label
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = theme_manager_get_color("text");
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
 * Static mode: bring the cached frame up to date, rendering synchronously when anything it
 * shows changed. A 3D render that overruns kStaticRenderBudgetMs drops this widget to the
 * 2D heatmap for good, so a slow board pays that stall once rather than on every change.
 * Returns false when there is nothing to show (no mesh data yet).
 */
static constexpr float kDefaultStaticRenderBudgetMs = 150.0f;
static float s_static_render_budget_ms = kDefaultStaticRenderBudgetMs;

static int current_theme_generation() {
    lv_subject_t* changed = theme_manager_get_changed_subject();
    return changed ? lv_subject_get_int(changed) : 0;
}

static bool refresh_static_frame(bed_mesh_widget_data_t* data, int width, int height) {
    if (!bed_mesh_renderer_has_data(data->renderer)) {
        return false;
    }
    if (!data->static_frame || data->static_frame->width() != width ||
        data->static_frame->height() != height) {
        data->static_frame = std::make_unique<helix::mesh::PixelBuffer>(width, height);
        data->static_dirty = true;
    }
    if (data->static_theme_gen != current_theme_generation()) {
        data->static_dirty = true;
    }
    if (data->static_dirty) {
        data->static_dirty = false;
        data->static_theme_gen = current_theme_generation();
        ++data->static_render_count;
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = bed_mesh_renderer_render_to_buffer(data->renderer, *data->static_frame,
                                                     fetch_theme_colors());
        data->static_render_ms =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        spdlog::debug("[bed_mesh] Static render {}x{}: {:.1f}ms ({})", width, height,
                      data->static_render_ms, data->static_frame->info.heatmap ? "2D" : "3D");
        if (ok && !data->static_frame->info.heatmap &&
            data->static_render_ms > s_static_render_budget_ms) {
            spdlog::warn("[bed_mesh] Static 3D render took {:.0f}ms (budget {:.0f}ms); "
                         "falling back to the 2D heatmap",
                         data->static_render_ms, s_static_render_budget_ms);
            data->static_3d_banned = true;
            bed_mesh_renderer_set_render_mode(data->renderer, BedMeshRenderMode::Force2D);
            ok = bed_mesh_renderer_render_to_buffer(data->renderer, *data->static_frame,
                                                    fetch_theme_colors());
        }
        if (!ok) {
            data->blit_width = data->blit_height = 0;
            return false;
        }
    }
    const helix::mesh::PixelBuffer* buf = data->static_frame.get();
    lv_draw_buf_init(&data->blit_draw_buf, (uint32_t)buf->width(), (uint32_t)buf->height(),
                     LV_COLOR_FORMAT_ARGB8888, (uint32_t)buf->stride(),
                     const_cast<uint8_t*>(buf->data()), (uint32_t)(buf->stride() * buf->height()));
    data->blit_width = buf->width();
    data->blit_height = buf->height();
    data->shown_info = buf->info;
    return true;
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

    if (!data->render_thread) {
        // Without a render thread nothing is drawn unless the widget is static.
        if (!data->static_mode || !refresh_static_frame(data, width, height)) {
            return;
        }
        lv_draw_image_dsc_t img_dsc;
        lv_draw_image_dsc_init(&img_dsc);
        img_dsc.src = &data->blit_draw_buf;
        lv_area_t area{widget_coords.x1, widget_coords.y1, widget_coords.x1 + data->blit_width - 1,
                       widget_coords.y1 + data->blit_height - 1};
        lv_draw_image(layer, &img_dsc, &area);
        if (data->shown_info.heatmap) {
            helix::mesh::render_heatmap_overlay(layer, data->renderer, shown_heatmap_layout(data),
                                                widget_coords.x1, widget_coords.y1);
        }
        return;
    }

    // A new frame is swapped in without a copy; otherwise the previous one is reused.
    if (const helix::mesh::PixelBuffer* buf = data->render_thread->acquire_frame()) {
        lv_draw_buf_init(&data->blit_draw_buf, (uint32_t)buf->width(), (uint32_t)buf->height(),
                         LV_COLOR_FORMAT_ARGB8888, (uint32_t)buf->stride(),
                         const_cast<uint8_t*>(buf->data()),
                         (uint32_t)(buf->stride() * buf->height()));
        data->blit_width = buf->width();
        data->blit_height = buf->height();
        data->shown_info = buf->info;
        spdlog::trace("[bed_mesh] Blit {}x{} ({:.1f}ms render)", buf->width(), buf->height(),
                      data->render_thread->last_render_time_ms());
    } else {
        data->blit_width = data->blit_height = 0;
    }

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

    // Text and the heatmap's touch overlay need the LVGL font engine, so they are
    // drawn here on the main thread over the blitted frame.
    std::lock_guard<std::mutex> lock(data->render_thread->render_mutex());
    if (data->blit_width <= 0) {
        return; // Labels only make sense over a frame
    }
    if (data->shown_info.heatmap) {
        helix::mesh::render_heatmap_overlay(layer, data->renderer, shown_heatmap_layout(data),
                                            widget_coords.x1, widget_coords.y1);
        return;
    }

    // The frame was projected at (0,0); LVGL draws in absolute screen coordinates,
    // so project the labels at the widget's screen position.
    int saved_offset_x, saved_offset_y;
    bed_mesh_renderer_get_layer_offset(data->renderer, &saved_offset_x, &saved_offset_y);
    bed_mesh_renderer_set_layer_offset(data->renderer, widget_coords.x1, widget_coords.y1);

    helix::mesh::render_axis_labels(layer, data->renderer, width, height);
    helix::mesh::render_numeric_axis_ticks(layer, data->renderer, width, height);

    bed_mesh_renderer_set_layer_offset(data->renderer, saved_offset_x, saved_offset_y);
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

    // In 2D mode: show cell tooltip on touch; no dragging
    if (shows_heatmap(data)) {
        heatmap_touch(obj, data, indev);
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // 3D mode: start drag gesture
    data->is_dragging = true;
    data->last_drag_pos = point;
    data->two_finger = {};
    data->two_finger_occurred = false;

    // Update renderer dragging state for fast solid-color rendering.
    with_renderer(data, [&]() { bed_mesh_renderer_set_dragging(data->renderer, true); });

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
    if (shows_heatmap(data)) {
        heatmap_touch(obj, data, indev);
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
        with_renderer(data, [&]() { bed_mesh_renderer_set_dragging(data->renderer, false); });
        if (data->render_thread) {
            data->render_thread->request_render();
        }
        lv_obj_invalidate(obj); // Trigger redraw with gradient
        return;
    }

    if (data->two_finger_occurred) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Calculate delta from last position
    int dx = point.x - data->last_drag_pos.x;
    int dy = point.y - data->last_drag_pos.y;

    if (dx != 0 || dy != 0) {
        // Convert pixel movement to rotation angles
        // Shared rotation sensitivity with the G-code preview
        // Horizontal drag (dx) = spin rotation (angle_z)
        // Vertical drag (dy) = tilt rotation (angle_x), inverted for intuitive control
        const double delta_z = dx * helix::ui::kRotateDegreesPerPixel;
        const double delta_x = -dy * helix::ui::kRotateDegreesPerPixel; // Flip Y for intuitive tilt

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

        with_renderer(data, apply_rotation);

        if (data->render_thread) {
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
    if (shows_heatmap(data)) {
        bed_mesh_renderer_clear_touch(data->renderer);
        lv_obj_invalidate(obj); // Redraw to hide tooltip
        spdlog::trace("[bed_mesh] 2D touch released - hiding tooltip");
        return;
    }

    // 3D mode: end drag gesture
    data->is_dragging = false;
    data->two_finger = {};
    data->two_finger_occurred = false;

    // Update renderer dragging state for high-quality gradient rendering.
    with_renderer(data, [&]() { bed_mesh_renderer_set_dragging(data->renderer, false); });
    if (data->render_thread) {
        data->render_thread->request_render();
    }

    // Force immediate redraw to switch back to gradient rendering
    lv_obj_invalidate(obj);

    const bed_mesh_view_state_t* view = bed_mesh_renderer_get_view_state(data->renderer);
    spdlog::trace("[bed_mesh] Release - final rotation({:.1f}, {:.1f}), switching to gradient",
                  view ? view->angle_x : 0.0, view ? view->angle_z : 0.0);
}

#if LV_USE_GESTURE_RECOGNITION
// Two-finger pan and pinch zoom, 3D mode only.
static void bed_mesh_gesture_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(obj);

    if (!data || !data->renderer || shows_heatmap(data))
        return;

    const auto sample = helix::ui::read_two_finger_sample(e);
    if (!sample)
        return;

    const helix::ui::TwoFingerStep step = helix::ui::two_finger_step(*sample, data->two_finger);
    if (!step.active)
        return;
    data->two_finger_occurred = true;

    if (step.pan_dx == 0.0f && step.pan_dy == 0.0f && step.zoom == 1.0f)
        return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const int width = lv_area_get_width(&coords);
    const int height = lv_area_get_height(&coords);
    const double anchor_x = step.anchor_x - coords.x1;
    const double anchor_y = step.anchor_y - coords.y1;
    auto apply = [&]() {
        bed_mesh_renderer_apply_two_finger(data->renderer, step.pan_dx, step.pan_dy, step.zoom,
                                           anchor_x, anchor_y, width, height);
    };

    with_renderer(data, apply);
    if (data->render_thread) {
        data->render_thread->request_render();
    }
    lv_obj_invalidate(obj);
}
#endif

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

    // Restart the render thread at the new dimensions
    if (data && data->render_thread && width > 0 && height > 0) {
        data->render_thread->stop();
        data->blit_draw_buf = {};
        data->blit_width = data->blit_height = 0;
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
    if (helix::env_flag("HELIX_BED_MESH_2D")) {
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
#if LV_USE_GESTURE_RECOGNITION
    lv_obj_add_event_cb(obj, bed_mesh_gesture_cb, LV_EVENT_GESTURE, nullptr);
#endif

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
    // render thread iterates renderer->mesh[row][col] under render_mutex_.
    bool set_ok = false;
    with_renderer(data, [&]() {
        set_ok = bed_mesh_renderer_set_mesh_data(data->renderer, mesh, rows, cols);
    });
    if (!set_ok) {
        spdlog::error("[bed_mesh] Failed to set mesh data in renderer");
        return false;
    }

    // Reset adaptive quality state since new mesh may render differently
    if (data->render_thread) {
        data->render_thread->reset_quality();
    }
    data->static_dirty = true;

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
                            double mesh_y_min, double mesh_y_max, bool circular_bed) {
    if (!widget) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_bounds: NULL widget");
        return;
    }

    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer) {
        spdlog::error("[bed_mesh] ui_bed_mesh_set_bounds: widget data or renderer not initialized");
        return;
    }

    with_renderer(data, [&]() {
        bed_mesh_renderer_set_bounds(data->renderer, bed_x_min, bed_x_max, bed_y_min, bed_y_max,
                                     mesh_x_min, mesh_x_max, mesh_y_min, mesh_y_max, circular_bed);
    });
    data->static_dirty = true;

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

    // The thread's frame-ready callback invalidates the widget when the frame is done.
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (data && data->render_thread) {
        data->render_thread->request_render();
        spdlog::debug("[bed_mesh] Redraw requested");
    } else if (data && data->static_mode) {
        data->static_dirty = true;
        lv_obj_invalidate(widget);
    }
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

    with_renderer(data, [&]() { bed_mesh_renderer_evaluate_render_mode(data->renderer); });
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

    if (data->static_mode && data->static_3d_banned && mode != BedMeshRenderMode::Force2D) {
        mode = BedMeshRenderMode::Force2D;
    }
    with_renderer(data, [&]() { bed_mesh_renderer_set_render_mode(data->renderer, mode); });
    data->static_dirty = true;
    ui_bed_mesh_redraw(widget);
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
helix::BedMeshRenderMode ui_bed_mesh_get_render_mode(lv_obj_t* widget) {
    bed_mesh_widget_data_t* data =
        widget ? (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget) : nullptr;
    if (!data || !data->renderer) {
        return BedMeshRenderMode::Auto;
    }
    return bed_mesh_renderer_get_render_mode(data->renderer);
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

    with_renderer(data,
                  [&]() { bed_mesh_renderer_set_zero_plane_visible(data->renderer, visible); });
    data->static_dirty = true;
    ui_bed_mesh_redraw(widget);
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

    with_renderer(data,
                  [&]() { bed_mesh_renderer_set_z_display_offset(data->renderer, offset_mm); });
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

    if (enabled == (data->render_thread != nullptr)) {
        return; // Already in requested mode
    }

    if (enabled) {
        int width = lv_obj_get_width(widget);
        int height = lv_obj_get_height(widget);

        data->render_thread = std::make_unique<BedMeshRenderThread>();
        data->render_thread->set_renderer(data->renderer);
        data->render_thread->set_colors(fetch_theme_colors());

        // Set callback to invalidate widget when frame ready (from render thread).
        // The handle is made here on the LVGL thread; if the widget is deleted
        // before the queued callback fires, it is silently skipped.
        data->render_thread->set_frame_ready_callback([widget = helix::ui::WeakWidget(widget)]() {
            helix::ui::queue_widget_update(widget, [](lv_obj_t* w) { lv_obj_invalidate(w); });
        });

        if (width > 0 && height > 0) {
            data->render_thread->start(width, height);
            data->render_thread->request_render();
        }

        spdlog::info("[bed_mesh] Async rendering enabled ({}x{})", width, height);
    } else {
        // Frees both frame buffers; the blit descriptor pointed into one of them
        data->render_thread.reset();
        data->blit_draw_buf = {};
        data->blit_width = data->blit_height = 0;
        lv_obj_invalidate(widget);
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

    return data->render_thread != nullptr;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
void ui_bed_mesh_set_static_mode(lv_obj_t* widget, bool enabled) {
    if (!widget) {
        return;
    }
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    if (!data || !data->renderer || data->static_mode == enabled) {
        return;
    }
    data->static_mode = enabled;
    data->static_dirty = true;
    if (!enabled) {
        data->static_frame.reset();
        data->blit_draw_buf = {};
        data->blit_width = data->blit_height = 0;
    }
    lv_obj_invalidate(widget);
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
float ui_bed_mesh_static_render_ms(lv_obj_t* widget) {
    if (!widget) {
        return 0.0f;
    }
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    return data ? data->static_render_ms : 0.0f;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
int ui_bed_mesh_static_render_count(lv_obj_t* widget) {
    if (!widget) {
        return 0;
    }
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    return data ? data->static_render_count : 0;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
bool ui_bed_mesh_static_3d_banned(lv_obj_t* widget) {
    if (!widget) {
        return false;
    }
    bed_mesh_widget_data_t* data = (bed_mesh_widget_data_t*)lv_obj_get_user_data(widget);
    return data && data->static_3d_banned;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
void ui_bed_mesh_set_static_render_budget_ms_for_test(float budget_ms) {
    s_static_render_budget_ms = budget_ms > 0.0f ? budget_ms : kDefaultStaticRenderBudgetMs;
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
                            double, bool) {}

void ui_bed_mesh_set_rotation(lv_obj_t*, int, int) {}

void ui_bed_mesh_redraw(lv_obj_t*) {}

void ui_bed_mesh_evaluate_render_mode(lv_obj_t*) {}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
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

bool ui_bed_mesh_has_data(lv_obj_t*) {
    return false;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
void ui_bed_mesh_set_static_mode(lv_obj_t*, bool) {}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
float ui_bed_mesh_static_render_ms(lv_obj_t*) {
    return 0.0f;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
int ui_bed_mesh_static_render_count(lv_obj_t*) {
    return 0;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
bool ui_bed_mesh_static_3d_banned(lv_obj_t*) {
    return false;
}

// NAMESPACE_OK: joins the <bed_mesh> widget's global ui_bed_mesh_* C API
void ui_bed_mesh_set_static_render_budget_ms_for_test(float) {}

#endif // HELIX_HAS_BED_MESH_3D
