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

#include "ui_gcode_viewer.h"

#include "ui_async_callback.h"

#include "gcode_camera.h"
#include "gcode_parser.h"

#ifdef ENABLE_TINYGL_3D
#include "gcode_tinygl_renderer.h"
#else
#include "gcode_renderer.h"
#endif

#include <lvgl/src/xml/lv_xml_parser.h>
#include <lvgl/src/xml/parsers/lv_xml_obj_parser.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_set>

// Widget state (stored in LVGL object user_data)
struct gcode_viewer_state_t {
    // G-code data
    std::unique_ptr<gcode::ParsedGCodeFile> gcode_file;
    gcode_viewer_state_enum_t viewer_state{GCODE_VIEWER_STATE_EMPTY};

    // Rendering components
    std::unique_ptr<gcode::GCodeCamera> camera;
#ifdef ENABLE_TINYGL_3D
    std::unique_ptr<gcode::GCodeTinyGLRenderer> renderer;
#else
    std::unique_ptr<gcode::GCodeRenderer> renderer;
#endif

    // Gesture state (for touch interaction)
    bool is_dragging{false};
    lv_point_t drag_start{0, 0};
    lv_point_t last_drag_pos{0, 0};

    // Selection state (future-proofed for multi-select)
    std::unordered_set<std::string> selected_objects;

    // Excluded objects state (synced with Klipper)
    std::unordered_set<std::string> excluded_objects;

    // Object tap callback for exclude object UI
    gcode_viewer_object_tap_callback_t object_tap_callback{nullptr};
    void* object_tap_user_data{nullptr};

    // Long-press callback for exclude object flow (more intentional than tap)
    gcode_viewer_object_long_press_callback_t object_long_press_callback{nullptr};
    void* object_long_press_user_data{nullptr};

    // Long-press detection state
    lv_timer_t* long_press_timer{nullptr};       ///< Timer for 500ms long-press detection
    bool long_press_fired{false};                 ///< True if long-press callback was invoked
    std::string long_press_object_name;           ///< Object under finger when long-press started

    // Rendering settings
    bool use_filament_color{true}; // Auto-apply filament color from metadata

    // Async loading state
    std::atomic<bool> geometry_building_{false};
    std::thread geometry_thread_;
    lv_obj_t* loading_container_{nullptr};
    lv_obj_t* loading_spinner_{nullptr};
    lv_obj_t* loading_label_{nullptr};
    bool first_render_{true}; // Show "Rendering..." message on first draw

    // Load completion callback
    gcode_viewer_load_callback_t load_callback_{nullptr};
    void* load_callback_user_data_{nullptr};

    // Constructor
    gcode_viewer_state_t() {
        camera = std::make_unique<gcode::GCodeCamera>();
#ifdef ENABLE_TINYGL_3D
        renderer = std::make_unique<gcode::GCodeTinyGLRenderer>();
        spdlog::info("GCode viewer using TinyGL 3D renderer");
#else
        renderer = std::make_unique<gcode::GCodeRenderer>();
        spdlog::info("GCode viewer using LVGL 2D renderer");
#endif
    }

    // Destructor - ensure thread is cleaned up
    ~gcode_viewer_state_t() {
        if (geometry_thread_.joinable()) {
            geometry_thread_.detach(); // Let it finish in background
        }
    }
};

// Helper: Get widget state from object
static gcode_viewer_state_t* get_state(lv_obj_t* obj) {
    return static_cast<gcode_viewer_state_t*>(lv_obj_get_user_data(obj));
}

// ==============================================
// Event Callbacks
// ==============================================

/**
 * @brief Main draw callback - renders G-code using custom renderer
 */
static void gcode_viewer_draw_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !layer) {
        return;
    }

    spdlog::debug("GCodeViewer: draw_cb called, state={}, gcode_file={}, first_render={}",
                  (int)st->viewer_state, (void*)st->gcode_file.get(), st->first_render_);

    // If no G-code loaded, draw placeholder message
    if (st->viewer_state != GCODE_VIEWER_STATE_LOADED || !st->gcode_file) {
        // TODO: Draw "No G-code loaded" message
        return;
    }

    // On first render after async load, skip rendering to avoid blocking
    // The "Rendering..." label was already created by the async callback
    if (st->first_render_) {
        spdlog::debug(
            "GCodeViewer: First draw after async load - skipping render, will render on timer");
        return; // Timer will trigger actual render
    }

    // FPS tracking for performance monitoring (debug mode only via spdlog level)
    static auto last_frame_time = std::chrono::high_resolution_clock::now();
    static float fps_average = 0.0f;
    static int frame_count = 0;
    static constexpr float FPS_ALPHA = 0.1f; // Exponential moving average smoothing factor

    auto frame_start = std::chrono::high_resolution_clock::now();
    auto frame_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(frame_start - last_frame_time);

    if (frame_duration.count() > 0) {
        float current_fps = 1000000.0f / frame_duration.count();
        fps_average = (fps_average == 0.0f)
                          ? current_fps
                          : (FPS_ALPHA * current_fps + (1.0f - FPS_ALPHA) * fps_average);
    }

    last_frame_time = frame_start;

    // Render G-code (viewport size is already set by SIZE_CHANGED event)
    st->renderer->render(layer, *st->gcode_file, *st->camera);

    // Log FPS every 60 frames (controlled by spdlog level)
    if (++frame_count >= 60) {
        spdlog::debug("[GCode::Viewer] FPS: current={:.1f}, average={:.1f}",
                      (frame_duration.count() > 0 ? 1000000.0f / frame_duration.count() : 0.0f),
                      fps_average);
        frame_count = 0;
    }
}

// Long-press threshold in milliseconds
constexpr uint32_t LONG_PRESS_THRESHOLD_MS = 500;

/**
 * @brief Timer callback for long-press detection
 *
 * Fires after LONG_PRESS_THRESHOLD_MS if user hasn't moved the finger.
 * Picks the object under the initial press position and invokes the long-press callback.
 */
static void long_press_timer_cb(lv_timer_t* timer) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !st->gcode_file)
        return;

    // Timer fired - this is a long-press
    st->long_press_fired = true;

    // Delete the timer (one-shot)
    lv_timer_delete(timer);
    st->long_press_timer = nullptr;

    // Pick object at the original press position
    const char* picked = ui_gcode_viewer_pick_object(obj, st->drag_start.x, st->drag_start.y);

    if (picked && picked[0] != '\0') {
        st->long_press_object_name = picked;

        // Highlight the object to provide visual feedback
        st->selected_objects.clear();
        st->selected_objects.insert(picked);
        ui_gcode_viewer_set_highlighted_objects(obj, st->selected_objects);

        spdlog::info("GCodeViewer: Long-press on object '{}'", picked);

        // Invoke long-press callback
        if (st->object_long_press_callback) {
            st->object_long_press_callback(obj, picked, st->object_long_press_user_data);
        }
    } else {
        st->long_press_object_name.clear();
        spdlog::debug("GCodeViewer: Long-press at ({}, {}) - no object found", st->drag_start.x,
                      st->drag_start.y);

        // Invoke callback with empty string to indicate long-press on empty space
        if (st->object_long_press_callback) {
            st->object_long_press_callback(obj, "", st->object_long_press_user_data);
        }
    }
}

/**
 * @brief Touch press callback - start drag gesture and long-press timer
 */
static void gcode_viewer_press_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    st->is_dragging = true;
    st->drag_start = point;
    st->last_drag_pos = point;
    st->long_press_fired = false;
    st->long_press_object_name.clear();

    // Start long-press timer if callback is registered
    if (st->object_long_press_callback && st->gcode_file) {
        // Cancel any existing timer
        if (st->long_press_timer) {
            lv_timer_delete(st->long_press_timer);
        }
        // Start new timer for long-press detection
        st->long_press_timer = lv_timer_create(long_press_timer_cb, LONG_PRESS_THRESHOLD_MS, obj);
        lv_timer_set_repeat_count(st->long_press_timer, 1); // One-shot timer
    }

    spdlog::trace("GCodeViewer: Press at ({}, {})", point.x, point.y);
}

// Movement threshold to cancel long-press (same as click threshold)
constexpr int LONG_PRESS_MOVE_THRESHOLD = 10;

/**
 * @brief Touch pressing callback - handle drag for camera rotation
 *
 * Also cancels long-press timer if user moves beyond threshold.
 */
static void gcode_viewer_pressing_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st || !st->is_dragging)
        return;

    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Check if movement exceeds threshold - cancel long-press timer
    int total_dx = abs(point.x - st->drag_start.x);
    int total_dy = abs(point.y - st->drag_start.y);

    if ((total_dx >= LONG_PRESS_MOVE_THRESHOLD || total_dy >= LONG_PRESS_MOVE_THRESHOLD) &&
        st->long_press_timer) {
        // User started dragging - cancel long-press
        lv_timer_delete(st->long_press_timer);
        st->long_press_timer = nullptr;
        spdlog::trace("GCodeViewer: Long-press cancelled due to movement");
    }

    // Calculate delta from last position
    int dx = point.x - st->last_drag_pos.x;
    int dy = point.y - st->last_drag_pos.y;

    if (dx != 0 || dy != 0) {
        // Convert pixel movement to rotation angles
        // Scale factor: ~0.5 degrees per pixel
        float delta_azimuth = dx * 0.5f;
        float delta_elevation = -dy * 0.5f; // Flip Y for intuitive control

        st->camera->rotate(delta_azimuth, delta_elevation);

        // Trigger redraw
        lv_obj_invalidate(obj);

        st->last_drag_pos = point;

        spdlog::trace("GCodeViewer: Drag ({}, {}) -> rotate({:.1f}, {:.1f})", dx, dy, delta_azimuth,
                      delta_elevation);
    }
}

/**
 * @brief Touch release callback - handle click vs drag gesture
 *
 * Skips tap handling if long-press already fired (user held for 500ms+).
 */
static void gcode_viewer_release_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    // Cancel long-press timer if still pending
    if (st->long_press_timer) {
        lv_timer_delete(st->long_press_timer);
        st->long_press_timer = nullptr;
    }

    // Get release position
    lv_indev_t* indev = lv_indev_active();
    if (!indev) {
        st->is_dragging = false;
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    // Calculate total drag distance from initial press
    int dx = abs(point.x - st->drag_start.x);
    int dy = abs(point.y - st->drag_start.y);

    const int CLICK_THRESHOLD = 10; // pixels - distinguish click from drag

    // Skip tap handling if long-press already fired
    if (st->long_press_fired) {
        spdlog::trace("GCodeViewer: Release after long-press - skipping tap handling");
        st->is_dragging = false;
        st->long_press_fired = false;
        return;
    }

    // If movement was minimal, treat as click and try to pick object
    if (dx < CLICK_THRESHOLD && dy < CLICK_THRESHOLD && st->gcode_file) {
        spdlog::debug("GCodeViewer: Click detected at ({}, {})", point.x, point.y);
        const char* picked = ui_gcode_viewer_pick_object(obj, point.x, point.y);

        if (picked && picked[0] != '\0') {
            // Object clicked - toggle selection
            std::string picked_name(picked);

            if (st->selected_objects.count(picked_name) > 0) {
                // Already selected - deselect
                st->selected_objects.erase(picked_name);
                spdlog::info("GCodeViewer: Deselected object '{}'", picked_name);
            } else {
                // Not selected - add to selection (multi-select mode)
                st->selected_objects.insert(picked_name);
                spdlog::info("GCodeViewer: Selected object '{}' ({} total selected)", picked_name,
                             st->selected_objects.size());
            }

            // Update highlighting to show all selected objects
            ui_gcode_viewer_set_highlighted_objects(obj, st->selected_objects);

            // Invoke tap callback if registered (for exclude object UI)
            if (st->object_tap_callback) {
                st->object_tap_callback(obj, picked, st->object_tap_user_data);
            }
        } else {
            spdlog::debug("GCodeViewer: Click at ({}, {}) - no object found (G-code may lack "
                          "EXCLUDE_OBJECT metadata)",
                          point.x, point.y);
            // Still invoke callback with empty string to indicate click on empty space
            if (st->object_tap_callback) {
                st->object_tap_callback(obj, "", st->object_tap_user_data);
            }
        }
        // Note: If no object picked, keep current selection (per user requirements)
    }

    st->is_dragging = false;
    spdlog::trace("GCodeViewer: Release at ({}, {}), drag=({}, {})", point.x, point.y, dx, dy);
}

/**
 * @brief Size changed callback - update camera aspect ratio on resize
 */
static void gcode_viewer_size_changed_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (!st)
        return;

    // Get new widget dimensions
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int width = lv_area_get_width(&coords);
    int height = lv_area_get_height(&coords);

    // Update camera and renderer viewport to match new size
    st->camera->set_viewport_size(width, height);
    st->renderer->set_viewport_size(width, height);

    // Trigger redraw with new aspect ratio
    lv_obj_invalidate(obj);

    spdlog::debug("GCodeViewer SIZE_CHANGED: {}x{}, aspect={:.3f}", width, height,
                  (float)width / (float)height);
}

/**
 * @brief Cleanup callback - free resources on widget deletion
 */
static void gcode_viewer_delete_cb(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_target_obj(e);
    gcode_viewer_state_t* st = get_state(obj);

    if (st) {
        // Clean up long-press timer if pending
        if (st->long_press_timer) {
            lv_timer_delete(st->long_press_timer);
            st->long_press_timer = nullptr;
        }

        spdlog::debug("GCodeViewer: Widget destroyed");
        delete st; // C++ destructor will clean up unique_ptrs
        lv_obj_set_user_data(obj, nullptr);
    }
}

// ==============================================
// Public API Implementation
// ==============================================

lv_obj_t* ui_gcode_viewer_create(lv_obj_t* parent) {
    // Create base object
    lv_obj_t* obj = lv_obj_create(parent);
    if (!obj) {
        return nullptr;
    }

    // Allocate state (C++ object)
    gcode_viewer_state_t* st = new gcode_viewer_state_t();
    if (!st) {
        lv_obj_delete(obj);
        return nullptr;
    }

    lv_obj_set_user_data(obj, st);

    // Configure object appearance
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    // Register event handlers
    lv_obj_add_event_cb(obj, gcode_viewer_draw_cb, LV_EVENT_DRAW_POST, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_size_changed_cb, LV_EVENT_SIZE_CHANGED, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_pressing_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_release_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(obj, gcode_viewer_delete_cb, LV_EVENT_DELETE, nullptr);

    // Initialize viewport size based on current widget dimensions
    // This ensures correct aspect ratio from the start
    lv_obj_update_layout(obj); // Force layout calculation
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int width = lv_area_get_width(&coords);
    int height = lv_area_get_height(&coords);

    if (width > 0 && height > 0) {
        st->camera->set_viewport_size(width, height);
        st->renderer->set_viewport_size(width, height);
        spdlog::debug("GCodeViewer INIT: viewport={}x{}, aspect={:.3f}", width, height,
                      (float)width / (float)height);
    } else {
        spdlog::error("GCodeViewer INIT: Invalid size {}x{}, using defaults", width, height);
    }

    spdlog::debug("GCodeViewer: Widget created");
    return obj;
}

// Result structure for async geometry building
struct AsyncBuildResult {
    std::unique_ptr<gcode::ParsedGCodeFile> gcode_file;
    std::unique_ptr<gcode::RibbonGeometry> geometry;
    std::string error_msg;
    bool success{true};
};

/**
 * @brief Asynchronously load and build G-code geometry in background thread
 *
 * Shows loading spinner while parsing and building geometry. Uses background
 * thread to avoid blocking the UI thread. Geometry building is thread-safe
 * (no OpenGL calls, pure CPU work).
 */
static void ui_gcode_viewer_load_file_async(lv_obj_t* obj, const char* file_path) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !file_path) {
        return;
    }

    // Don't start new load if already loading
    if (st->geometry_building_) {
        spdlog::warn("GCodeViewer: Load already in progress, ignoring request");
        return;
    }

    spdlog::info("GCodeViewer: Loading file async: {}", file_path);
    st->viewer_state = GCODE_VIEWER_STATE_LOADING;
    st->first_render_ = true; // Reset for new file

    // Clean up previous loading UI if it exists
    if (st->loading_container_) {
        lv_obj_delete(st->loading_container_);
        st->loading_container_ = nullptr;
    }

    // Create loading UI
    st->loading_container_ = lv_obj_create(obj);
    lv_obj_set_size(st->loading_container_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(st->loading_container_);
    lv_obj_set_flex_flow(st->loading_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st->loading_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    st->loading_spinner_ = lv_spinner_create(st->loading_container_);
    lv_obj_set_size(st->loading_spinner_, 50, 50);

    st->loading_label_ = lv_label_create(st->loading_container_);
    lv_label_set_text(st->loading_label_, "Loading G-code...");

    // Launch worker thread
    st->geometry_building_ = true;

    // Join previous thread if it exists
    if (st->geometry_thread_.joinable()) {
        st->geometry_thread_.join();
    }

    st->geometry_thread_ = std::thread([obj, path = std::string(file_path)]() {
        auto result = std::make_unique<AsyncBuildResult>();

        try {
            // PHASE 1: Parse G-code file (fast, ~100ms)
            std::ifstream file(path);
            if (!file.is_open()) {
                result->success = false;
                result->error_msg = "Failed to open file: " + path;
            } else {
                gcode::GCodeParser parser;
                std::string line;

                while (std::getline(file, line)) {
                    parser.parse_line(line);
                }

                file.close();

                result->gcode_file = std::make_unique<gcode::ParsedGCodeFile>(parser.finalize());
                result->gcode_file->filename = path;

                spdlog::info("GCodeViewer: Parsed {} layers, {} segments",
                             result->gcode_file->layers.size(), result->gcode_file->total_segments);

                // PHASE 2: Build geometry (slow, 1-5s for large files)
                // This is thread-safe - no OpenGL calls, just CPU work
                gcode::GeometryBuilder builder;
                gcode::SimplificationOptions opts{.tolerance_mm = 0.15f};

                // Configure builder with metadata
                // Set tool color palette for multicolor prints
                if (!result->gcode_file->tool_color_palette.empty()) {
                    builder.set_tool_color_palette(result->gcode_file->tool_color_palette);
                    spdlog::debug("GCodeViewer: Set tool color palette with {} colors",
                                  result->gcode_file->tool_color_palette.size());
                }

                if (result->gcode_file->perimeter_extrusion_width_mm > 0.0f) {
                    builder.set_extrusion_width(result->gcode_file->perimeter_extrusion_width_mm);
                } else if (result->gcode_file->extrusion_width_mm > 0.0f) {
                    builder.set_extrusion_width(result->gcode_file->extrusion_width_mm);
                }

                builder.set_layer_height(result->gcode_file->layer_height_mm);

                result->geometry = std::make_unique<gcode::RibbonGeometry>(
                    builder.build(*result->gcode_file, opts));

                spdlog::info("GCodeViewer: Built geometry with {} vertices, {} triangles",
                             result->geometry->vertices.size(),
                             result->geometry->extrusion_triangle_count +
                                 result->geometry->travel_triangle_count);
            }
        } catch (const std::exception& ex) {
            result->success = false;
            result->error_msg = std::string("Exception: ") + ex.what();
        }

        // PHASE 3: Marshal result back to UI thread (SAFE)
        ui_async_call_safe<AsyncBuildResult>(std::move(result), [obj](AsyncBuildResult* r) {
            gcode_viewer_state_t* st = get_state(obj);
            if (!st) {
                return; // Widget was destroyed
            }

            st->geometry_building_ = false;

            // Clean up loading UI
            if (st->loading_container_) {
                lv_obj_delete(st->loading_container_);
                st->loading_container_ = nullptr;
                st->loading_spinner_ = nullptr;
                st->loading_label_ = nullptr;
            }

            if (r->success) {
                spdlog::debug("GCodeViewer: Async callback - setting up geometry");

                // Store G-code data
                st->gcode_file = std::move(r->gcode_file);

                // Set pre-built geometry on renderer
#ifdef ENABLE_TINYGL_3D
                spdlog::debug("GCodeViewer: Calling set_prebuilt_geometry");
                st->renderer->set_prebuilt_geometry(std::move(r->geometry),
                                                    st->gcode_file->filename);
#endif

                // Fit camera to model bounds
                st->camera->fit_to_bounds(st->gcode_file->global_bounding_box);

                st->viewer_state = GCODE_VIEWER_STATE_LOADED;
                spdlog::debug("GCodeViewer: State set to LOADED");

                // Auto-apply filament color if enabled, but ONLY for single-color prints
                // Multicolor prints have multiple colors in the geometry's color palette
#ifdef ENABLE_TINYGL_3D
                size_t color_count = st->renderer->get_geometry_color_count();
                bool is_multicolor = (color_count > 1); // >1 means multiple tool colors
#else
                    bool is_multicolor = false; // 2D renderer doesn't have color palette
#endif

                if (st->use_filament_color && !is_multicolor &&
                    st->gcode_file->filament_color_hex.length() >= 2) {
                    lv_color_t color = lv_color_hex(
                        std::strtol(st->gcode_file->filament_color_hex.c_str() + 1, nullptr, 16));
                    st->renderer->set_extrusion_color(color);
                    spdlog::debug("GCodeViewer: Auto-applied single-color filament: {}",
                                  st->gcode_file->filament_color_hex);
                } else if (is_multicolor) {
                    spdlog::info("GCodeViewer: Multicolor print detected ({} colors) - preserving "
                                 "per-segment colors",
                                 color_count);
                }

                // Clear first_render flag to allow actual rendering on next draw
                st->first_render_ = false;

                // Trigger redraw (will render geometry now that first_render is false)
                lv_obj_invalidate(obj);

                spdlog::info("GCodeViewer: Async load completed successfully");

                // Invoke load callback if registered
                if (st->load_callback_) {
                    spdlog::debug("GCodeViewer: Invoking load callback");
                    st->load_callback_(obj, st->load_callback_user_data_, true);
                }
            } else {
                spdlog::error("GCodeViewer: Async load failed: {}", r->error_msg);
                st->viewer_state = GCODE_VIEWER_STATE_ERROR;
                st->gcode_file.reset();

                // Invoke load callback with error status if registered
                if (st->load_callback_) {
                    spdlog::debug("GCodeViewer: Invoking load callback (error)");
                    st->load_callback_(obj, st->load_callback_user_data_, false);
                }
            }
        });
    });
}

void ui_gcode_viewer_load_file(lv_obj_t* obj, const char* file_path) {
    // Use async version by default
    ui_gcode_viewer_load_file_async(obj, file_path);
}

void ui_gcode_viewer_set_load_callback(lv_obj_t* obj, gcode_viewer_load_callback_t callback,
                                       void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st) {
        return;
    }

    st->load_callback_ = callback;
    st->load_callback_user_data_ = user_data;
    spdlog::debug("GCodeViewer: Load callback registered");
}

void ui_gcode_viewer_set_gcode_data(lv_obj_t* obj, void* gcode_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !gcode_data)
        return;

    // Take ownership of the data (caller must use new to allocate)
    st->gcode_file.reset(static_cast<gcode::ParsedGCodeFile*>(gcode_data));

    // Fit camera to model (uses current camera orientation from reset())
    st->camera->fit_to_bounds(st->gcode_file->global_bounding_box);

    st->viewer_state = GCODE_VIEWER_STATE_LOADED;

    spdlog::info("GCodeViewer: Set G-code data: {} layers, {} segments",
                 st->gcode_file->layers.size(), st->gcode_file->total_segments);

    // Auto-apply filament color if enabled and available
    if (st->use_filament_color && st->gcode_file->filament_color_hex.length() >= 2) {
        lv_color_t color =
            lv_color_hex(std::strtol(st->gcode_file->filament_color_hex.c_str() + 1, nullptr, 16));
        st->renderer->set_extrusion_color(color);
        spdlog::info("GCodeViewer: Auto-applied filament color: {}",
                     st->gcode_file->filament_color_hex);
    }

    // Trigger redraw
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_clear(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->gcode_file.reset();
    st->viewer_state = GCODE_VIEWER_STATE_EMPTY;

    lv_obj_invalidate(obj);
    spdlog::debug("GCodeViewer: Cleared");
}

gcode_viewer_state_enum_t ui_gcode_viewer_get_state(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    return st ? st->viewer_state : GCODE_VIEWER_STATE_EMPTY;
}

// ==============================================
// Camera Controls
// ==============================================

void ui_gcode_viewer_rotate(lv_obj_t* obj, float delta_azimuth, float delta_elevation) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->rotate(delta_azimuth, delta_elevation);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_pan(lv_obj_t* obj, float delta_x, float delta_y) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->pan(delta_x, delta_y);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_zoom(lv_obj_t* obj, float factor) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->zoom(factor);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_reset_camera(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->reset();

    // Re-fit to model if loaded
    if (st->gcode_file) {
        st->camera->fit_to_bounds(st->gcode_file->global_bounding_box);
    }

    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_view(lv_obj_t* obj, gcode_viewer_preset_view_t preset) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    switch (preset) {
    case GCODE_VIEWER_VIEW_ISOMETRIC:
        st->camera->set_isometric_view();
        break;
    case GCODE_VIEWER_VIEW_TOP:
        st->camera->set_top_view();
        break;
    case GCODE_VIEWER_VIEW_FRONT:
        st->camera->set_front_view();
        break;
    case GCODE_VIEWER_VIEW_SIDE:
        st->camera->set_side_view();
        break;
    }

    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_camera_azimuth(lv_obj_t* obj, float azimuth) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->set_azimuth(azimuth);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_camera_elevation(lv_obj_t* obj, float elevation) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->set_elevation(elevation);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_camera_zoom(lv_obj_t* obj, float zoom) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->camera->set_zoom_level(zoom);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_debug_colors(lv_obj_t* obj, bool enable) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

#ifdef ENABLE_TINYGL_3D
    st->renderer->set_debug_face_colors(enable);
    lv_obj_invalidate(obj);
#else
    (void)enable;
#endif
}

// ==============================================
// Rendering Options
// ==============================================

void ui_gcode_viewer_set_show_travels(lv_obj_t* obj, bool show) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_show_travels(show);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_show_extrusions(lv_obj_t* obj, bool show) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_show_extrusions(show);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_layer_range(lv_obj_t* obj, int start_layer, int end_layer) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_layer_range(start_layer, end_layer);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_highlighted_object(lv_obj_t* obj, const char* object_name) {
    // Legacy single-object API - convert to set and call multi-object version
    std::unordered_set<std::string> objects;
    if (object_name && object_name[0] != '\0') {
        objects.insert(object_name);
    }
    ui_gcode_viewer_set_highlighted_objects(obj, objects);
}

void ui_gcode_viewer_set_highlighted_objects(lv_obj_t* obj,
                                             const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_highlighted_objects(object_names);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_excluded_objects(lv_obj_t* obj,
                                          const std::unordered_set<std::string>& object_names) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->excluded_objects = object_names;
    st->renderer->set_excluded_objects(object_names);
    lv_obj_invalidate(obj);

    spdlog::debug("GCodeViewer: Excluded objects updated ({} objects)", object_names.size());
}

void ui_gcode_viewer_set_object_tap_callback(lv_obj_t* obj,
                                             gcode_viewer_object_tap_callback_t callback,
                                             void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->object_tap_callback = callback;
    st->object_tap_user_data = user_data;
}

void ui_gcode_viewer_set_object_long_press_callback(lv_obj_t* obj,
                                                     gcode_viewer_object_long_press_callback_t callback,
                                                     void* user_data) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->object_long_press_callback = callback;
    st->object_long_press_user_data = user_data;

    spdlog::debug("GCodeViewer: Long-press callback {}",
                  callback ? "registered" : "cleared");
}

// ==============================================
// Color & Rendering Control
// ==============================================

void ui_gcode_viewer_set_extrusion_color(lv_obj_t* obj, lv_color_t color) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_extrusion_color(color);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_travel_color(lv_obj_t* obj, lv_color_t color) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_travel_color(color);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_use_filament_color(lv_obj_t* obj, bool enable) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->use_filament_color = enable;

    // If enabling and we have a loaded file with filament color, apply it now
    if (enable && st->gcode_file && st->gcode_file->filament_color_hex.length() >= 2) {
        lv_color_t color =
            lv_color_hex(std::strtol(st->gcode_file->filament_color_hex.c_str() + 1, nullptr, 16));
        st->renderer->set_extrusion_color(color);
        lv_obj_invalidate(obj);
        spdlog::debug("GCodeViewer: Applied filament color: {}",
                      st->gcode_file->filament_color_hex);
    } else if (!enable) {
        // Reset to theme default
        st->renderer->reset_colors();
        lv_obj_invalidate(obj);
    }
}

void ui_gcode_viewer_set_opacity(lv_obj_t* obj, lv_opa_t opacity) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_global_opacity(opacity);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_brightness(lv_obj_t* obj, float factor) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_brightness_factor(factor);
    lv_obj_invalidate(obj);
}

// ==============================================
// Layer Control Extensions
// ==============================================

void ui_gcode_viewer_set_single_layer(lv_obj_t* obj, int layer) {
    ui_gcode_viewer_set_layer_range(obj, layer, layer);
}

int ui_gcode_viewer_get_current_layer_start(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return 0;

    return st->renderer->get_options().layer_start;
}

int ui_gcode_viewer_get_current_layer_end(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return -1;

    return st->renderer->get_options().layer_end;
}

// ==============================================
// Print Progress / Ghost Layer Visualization
// ==============================================

void ui_gcode_viewer_set_print_progress(lv_obj_t* obj, int current_layer) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_print_progress_layer(current_layer);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_ghost_opacity(lv_obj_t* obj, lv_opa_t opacity) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    st->renderer->set_ghost_opacity(opacity);
    lv_obj_invalidate(obj);
}

void ui_gcode_viewer_set_ghost_mode(lv_obj_t* obj, int mode) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

    // Map int to enum (0=Dimmed, 1=Stipple)
    gcode::GhostRenderMode render_mode = (mode == 1)
        ? gcode::GhostRenderMode::Stipple
        : gcode::GhostRenderMode::Dimmed;

    st->renderer->set_ghost_render_mode(render_mode);
    lv_obj_invalidate(obj);
}

int ui_gcode_viewer_get_max_layer(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return -1;

    return st->renderer->get_max_layer_index();
}

// ==============================================
// Metadata Access
// ==============================================

const char* ui_gcode_viewer_get_filament_color(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file || st->gcode_file->filament_color_hex.empty())
        return nullptr;

    return st->gcode_file->filament_color_hex.c_str();
}

const char* ui_gcode_viewer_get_filament_type(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file || st->gcode_file->filament_type.empty())
        return nullptr;

    return st->gcode_file->filament_type.c_str();
}

const char* ui_gcode_viewer_get_printer_model(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file || st->gcode_file->printer_model.empty())
        return nullptr;

    return st->gcode_file->printer_model.c_str();
}

float ui_gcode_viewer_get_estimated_time_minutes(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return 0.0f;

    return st->gcode_file->estimated_print_time_minutes;
}

float ui_gcode_viewer_get_filament_weight_g(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return 0.0f;

    return st->gcode_file->filament_weight_g;
}

float ui_gcode_viewer_get_filament_length_mm(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return 0.0f;

    return st->gcode_file->total_filament_mm;
}

float ui_gcode_viewer_get_filament_cost(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return 0.0f;

    return st->gcode_file->filament_cost;
}

float ui_gcode_viewer_get_nozzle_diameter_mm(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return 0.0f;

    return st->gcode_file->nozzle_diameter_mm;
}

// ==============================================
// Object Picking
// ==============================================

const char* ui_gcode_viewer_pick_object(lv_obj_t* obj, int x, int y) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return nullptr;

    auto result = st->renderer->pick_object(glm::vec2(x, y), *st->gcode_file, *st->camera);

    if (result) {
        // Store in static buffer (safe for single-threaded LVGL)
        static std::string picked_name;
        picked_name = *result;
        return picked_name.c_str();
    }

    return nullptr;
}

// ==============================================
// Statistics
// ==============================================

const char* ui_gcode_viewer_get_filename(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file || st->gcode_file->filename.empty())
        return nullptr;

    return st->gcode_file->filename.c_str();
}

int ui_gcode_viewer_get_layer_count(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st || !st->gcode_file)
        return 0;

    return static_cast<int>(st->gcode_file->layers.size());
}

int ui_gcode_viewer_get_segments_rendered(lv_obj_t* obj) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return 0;

    return static_cast<int>(st->renderer->get_segments_rendered());
}

// ==============================================
// Material & Lighting Control
// ==============================================

void ui_gcode_viewer_set_specular(lv_obj_t* obj, float intensity, float shininess) {
    gcode_viewer_state_t* st = get_state(obj);
    if (!st)
        return;

#ifdef ENABLE_TINYGL_3D
    st->renderer->set_specular(intensity, shininess);
    lv_obj_invalidate(obj); // Request redraw
#else
    spdlog::warn("[GCodeViewer] set_specular() ignored - not using TinyGL 3D renderer");
#endif
}

// ==============================================
// LVGL XML Component Registration
// ==============================================

/**
 * @brief XML create handler for gcode_viewer widget
 */
static void* gcode_viewer_xml_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs; // Required by callback signature, but widget has no XML attributes
    void* parent = lv_xml_state_get_parent(state);
    if (!parent) {
        spdlog::error("[GCodeViewer] XML create: no parent object");
        return nullptr;
    }

    lv_obj_t* obj = ui_gcode_viewer_create((lv_obj_t*)parent);
    if (!obj) {
        spdlog::error("[GCodeViewer] XML create: failed to create widget");
        return nullptr;
    }

    spdlog::trace("[GCodeViewer] XML created widget");
    return (void*)obj;
}

/**
 * @brief XML apply handler for gcode_viewer widget
 * Applies XML attributes to the widget
 */
static void gcode_viewer_xml_apply(lv_xml_parser_state_t* state, const char** attrs) {
    void* item = lv_xml_state_get_item(state);
    lv_obj_t* obj = (lv_obj_t*)item;

    if (!obj) {
        spdlog::error("[GCodeViewer] NULL object in xml_apply");
        return;
    }

    // Apply standard lv_obj properties from XML (size, style, align, name, etc.)
    lv_xml_obj_apply(state, attrs);

    spdlog::trace("[GCodeViewer] Applied XML attributes");
}

/**
 * @brief Register gcode_viewer widget with LVGL XML system
 *
 * Call this during application initialization before loading any XML.
 * Typically called from main() or ui_init().
 */
extern "C" void ui_gcode_viewer_register(void) {
    lv_xml_register_widget("gcode_viewer", gcode_viewer_xml_create, gcode_viewer_xml_apply);
    spdlog::debug("[GCodeViewer] Registered <gcode_viewer> widget with LVGL XML system");
}
